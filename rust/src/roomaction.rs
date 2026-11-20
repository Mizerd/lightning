
use std::collections::HashMap;
use std::future::Future;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use matrix_sdk::ruma::{OwnedRoomId, OwnedUserId, RoomId, UserId};

use serde_json::{json, Value};

pub(crate) const ROOM_ACTION_RETRY_DELAYS: [Duration; 2] =
    [Duration::from_millis(1000), Duration::from_millis(3000)];

/// True when the request never got an answer from the server: the connection
/// failed, was reset, or timed out. Nothing the server said is in this class,
/// so retrying cannot repeat a refusal.
pub(crate) fn is_transport_failure(err: &matrix_sdk::Error) -> bool {
    matches!(err, matrix_sdk::Error::Http(http)
        if matches!(http.as_ref(), matrix_sdk::HttpError::Reqwest(_)))
}

pub(crate) fn failure_reason(err: &matrix_sdk::Error) -> String {
    if let matrix_sdk::Error::Http(http) = err {
        if matches!(http.as_ref(), matrix_sdk::HttpError::RefreshToken(_)) {
            return "refresh_failed".to_owned();
        }
    }
    match err {
        matrix_sdk::Error::AuthenticationRequired => "not_logged_in".to_owned(),
        _ => crate::oauth::sdk_failure_reason(err),
    }
}

pub(crate) fn failure_event(action: &str, err: &matrix_sdk::Error, attempts: u32) -> Value {
    json!({
        "type": "room_action_error",
        "action": action,
        "reason": failure_reason(err),
        "attempts": attempts,
    })
}

/// A 403 on a room we hold as joined is "room_gone" when the server's own joined list no longer has it.
pub(crate) async fn room_failure_event(
    room: &matrix_sdk::Room,
    action: &str,
    err: &matrix_sdk::Error,
    attempts: u32,
) -> Value {
    let mut event = failure_event(action, err, attempts);
    if event["reason"].as_str().is_some_and(|r| r.starts_with("http_403_"))
        && crate::invites::server_dropped_room(room).await
    {
        event["reason"] = json!("room_gone");
    }
    event
}

pub(crate) async fn with_transport_retry<T, F, Fut>(
    delays: &[Duration],
    mut op: F,
) -> (matrix_sdk::Result<T>, u32)
where
    F: FnMut() -> Fut,
    Fut: Future<Output = matrix_sdk::Result<T>>,
{
    let mut attempts: u32 = 0;
    loop {
        attempts += 1;
        match op().await {
            Err(err) if is_transport_failure(&err) => {
                match delays.get(attempts as usize - 1) {
                    Some(delay) => tokio::time::sleep(*delay).await,
                    None => return (Err(err), attempts),
                }
            }
            result => return (result, attempts),
        }
    }
}

pub(crate) async fn set_favourite(
    room: &matrix_sdk::Room,
    favourite: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_is_favourite(favourite, None).await }
    })
    .await
}

pub(crate) async fn set_low_priority(
    room: &matrix_sdk::Room,
    low_priority: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_is_low_priority(low_priority, None).await }
    })
    .await
}

// Reverse-DNS: `m.*` is reserved, and other clients ignore a tag they do not know.
pub(crate) const ARCHIVED_TAG: &str = "org.lightning_matrix.archived";

pub(crate) fn tags_mark_archived(tags: Option<&matrix_sdk::ruma::events::tag::Tags>) -> bool {
    tags.is_some_and(|tags| tags.contains_key(&ARCHIVED_TAG.into()))
}

type ArchivedCache = HashMap<OwnedUserId, HashMap<OwnedRoomId, bool>>;

// Every room payload asks, so the answer is kept in memory instead of re-reading
// the tag from the store each time.
fn archived_cache() -> &'static Mutex<ArchivedCache> {
    static CACHE: OnceLock<Mutex<ArchivedCache>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

fn cached_archived(user: &UserId, room: &RoomId) -> Option<bool> {
    let cache = archived_cache().lock().unwrap_or_else(|e| e.into_inner());
    cache.get(user).and_then(|rooms| rooms.get(room)).copied()
}

/// `overwrite` is false for a store read, which must not undo a newer tag event.
fn remember_archived(user: &UserId, room: &RoomId, archived: bool, overwrite: bool) {
    let mut cache = archived_cache().lock().unwrap_or_else(|e| e.into_inner());
    let rooms = cache.entry(user.to_owned()).or_default();
    if overwrite {
        rooms.insert(room.to_owned(), archived);
    } else {
        rooms.entry(room.to_owned()).or_insert(archived);
    }
}

pub(crate) fn forget_archived_account(user: &UserId) {
    archived_cache().lock().unwrap_or_else(|e| e.into_inner()).remove(user);
}

pub(crate) async fn is_archived(room: &matrix_sdk::Room) -> bool {
    if let Some(archived) = cached_archived(room.own_user_id(), room.room_id()) {
        return archived;
    }
    let archived = tags_mark_archived(room.tags().await.ok().flatten().as_ref());
    remember_archived(room.own_user_id(), room.room_id(), archived, false);
    cached_archived(room.own_user_id(), room.room_id()).unwrap_or(archived)
}

/// The cached answer only, for callers that cannot wait on the store.
pub(crate) fn archived_known(room: &matrix_sdk::Room) -> bool {
    cached_archived(room.own_user_id(), room.room_id()).unwrap_or(false)
}

pub(crate) async fn load_archived(client: &matrix_sdk::Client) {
    for room in client.joined_rooms() {
        is_archived(&room).await;
    }
}

pub(crate) fn note_archived(room: &matrix_sdk::Room, archived: bool) {
    remember_archived(room.own_user_id(), room.room_id(), archived, true);
}

/// Keeps the cache in step with tags written by any device, and tells the UI.
pub(crate) fn install_archived_handler(
    client: &matrix_sdk::Client,
    events: Option<crate::EventQueueRef>,
) {
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::tag::TagEvent, room: matrix_sdk::Room| {
            let events = events.clone();
            async move {
                let Some(archived) =
                    on_tag_event(&room, tags_mark_archived(Some(&ev.content.tags)))
                else {
                    return;
                };
                if let Some(events) = events {
                    crate::enqueue(&events, json!({
                        "type": "room_tag_confirmed",
                        "room_id": room.room_id().to_string(),
                        "archived": archived,
                    }));
                }
            }
        },
    );
}

/// The archived state to show for a synced tag event, or None when nothing changed.
fn on_tag_event(room: &matrix_sdk::Room, archived: bool) -> Option<bool> {
    tag_event_changes(room.own_user_id(), room.room_id(), archived).then_some(archived)
}

/// Whether a synced tag event changes what the UI shows. An untagged room is the
/// default, so a cold cache meeting `false` (every favourite at sign-in) is no change.
fn tag_event_changes(user: &UserId, room: &RoomId, archived: bool) -> bool {
    let cached = cached_archived(user, room);
    let changed = cached.unwrap_or(false) != archived;
    if changed || cached.is_none() {
        remember_archived(user, room, archived, true);
    }
    changed
}

fn remove_tag_in_background(room: &matrix_sdk::Room) {
    note_archived(room, false);
    let room = room.clone();
    tokio::spawn(async move {
        let _ = room.remove_tag(ARCHIVED_TAG.into()).await;
    });
}

/// A room the user just joined should be visible, even if an old tag says archived.
/// Only a tag this device already knows is cleared; the request never holds up the join.
pub(crate) async fn unarchive_after_join(room: &matrix_sdk::Room) {
    if is_archived(room).await {
        remove_tag_in_background(room);
    }
}

pub(crate) async fn set_archived(
    room: &matrix_sdk::Room,
    archived: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move {
            if archived {
                room.set_tag(ARCHIVED_TAG.into(), matrix_sdk::ruma::events::tag::TagInfo::new()).await?;
            } else {
                room.remove_tag(ARCHIVED_TAG.into()).await?;
            }
            Ok(())
        }
    })
    .await
}

pub(crate) async fn set_unread_flag(
    room: &matrix_sdk::Room,
    unread: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_unread_flag(unread).await }
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_lightning_tag_marks_a_room_archived() {
        use matrix_sdk::ruma::events::tag::{TagInfo, TagName, Tags};
        assert!(!tags_mark_archived(None));
        let mut tags = Tags::new();
        tags.insert(TagName::Favorite, TagInfo::new());
        tags.insert(TagName::from("u.archived"), TagInfo::new());
        assert!(!tags_mark_archived(Some(&tags)));
        tags.insert(TagName::from(ARCHIVED_TAG), TagInfo::new());
        assert!(tags_mark_archived(Some(&tags)));
        assert_eq!(TagName::from(ARCHIVED_TAG).to_string(), ARCHIVED_TAG);
    }

    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc};
    use std::thread;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{api::client::sync::sync_events::v5, room_id, OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        Client, SessionMeta, SessionTokens,
    };
    use matrix_sdk_base::RequestedRequiredStates;

    const FAST: [Duration; 2] = [Duration::from_millis(10), Duration::from_millis(10)];

    #[derive(Clone, Copy)]
    enum TagScript {
        DropFirst,
        DropAlways,
        Forbidden,
        ForbiddenStillListed,
    }

    fn read_request_line(stream: &mut TcpStream) -> String {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        let head_end = loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                return String::new();
            }
            buf.extend_from_slice(&chunk[..n]);
            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                break pos + 4;
            }
        };
        let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
        let length = head
            .lines()
            .find_map(|line| {
                line.to_ascii_lowercase()
                    .strip_prefix("content-length:")
                    .map(|v| v.trim().parse::<usize>().unwrap_or(0))
            })
            .unwrap_or(0);
        while buf.len() < head_end + length {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        head.lines().next().unwrap_or("").to_owned()
    }

    fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    fn serve(script: TagScript) -> (String, Arc<AtomicUsize>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let tag_requests = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&tag_requests);
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request_line(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else if path.ends_with("/joined_rooms") {
                        let body = match script {
                            TagScript::ForbiddenStillListed => r#"{"joined_rooms":["!fav:example.org"]}"#,
                            _ => r#"{"joined_rooms":[]}"#,
                        };
                        respond(&mut stream, "200 OK", body);
                    } else if path.contains("/tags/") {
                        let n = seen.fetch_add(1, Ordering::SeqCst) + 1;
                        match script {
                            TagScript::DropFirst if n == 1 => drop(stream),
                            TagScript::DropAlways => drop(stream),
                            TagScript::Forbidden | TagScript::ForbiddenStillListed => respond(
                                &mut stream,
                                "403 Forbidden",
                                r#"{"errcode":"M_FORBIDDEN","error":"secret detail !room:example.org"}"#,
                            ),
                            _ => respond(&mut stream, "200 OK", "{}"),
                        }
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        (format!("http://{}:{}", addr.ip(), addr.port()), tag_requests)
    }

    fn serve_recording() -> (String, Arc<std::sync::Mutex<Vec<String>>>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let lines = Arc::new(std::sync::Mutex::new(Vec::new()));
        let seen = Arc::clone(&lines);
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request_line(&mut stream);
                    if line.contains("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                        return;
                    }
                    seen.lock().unwrap().push(line);
                    respond(&mut stream, "200 OK", "{}");
                });
            }
        });
        (format!("http://{}:{}", addr.ip(), addr.port()), lines)
    }

    #[tokio::test]
    async fn archiving_puts_and_unarchiving_deletes_the_lightning_tag() {
        let (homeserver, lines) = serve_recording();
        let (_client, room) = client_with_room(&homeserver).await;

        let (on, _) = tokio::time::timeout(BOUND, set_archived(&room, true, &FAST))
            .await.expect("bounded");
        let (off, _) = tokio::time::timeout(BOUND, set_archived(&room, false, &FAST))
            .await.expect("bounded");
        assert!(on.is_ok() && off.is_ok(), "{on:?} {off:?}");

        let tag_lines: Vec<String> = lines.lock().unwrap().iter()
            .filter(|line| line.contains("/tags/")).cloned().collect();
        assert_eq!(tag_lines.len(), 2, "{tag_lines:?}");
        let suffix = format!("/tags/{ARCHIVED_TAG}");
        assert!(tag_lines[0].starts_with("PUT ") && tag_lines[0].contains(&suffix), "{}", tag_lines[0]);
        assert!(tag_lines[1].starts_with("DELETE ") && tag_lines[1].contains(&suffix), "{}", tag_lines[1]);
    }

    #[test]
    fn untagged_rooms_at_sign_in_change_nothing() {
        let user = OwnedUserId::try_from("@cold-cache:example.org").expect("user");
        forget_archived_account(&user);
        for i in 0..50 {
            let room = OwnedRoomId::try_from(format!("!fav{i}:example.org")).expect("room");
            assert!(!tag_event_changes(&user, &room, false), "room {i} was reported");
            assert!(!tag_event_changes(&user, &room, false), "room {i} repeated");
        }
        let room = OwnedRoomId::try_from("!old:example.org").expect("room");
        assert!(tag_event_changes(&user, &room, true));
        assert!(!tag_event_changes(&user, &room, true));
        assert!(tag_event_changes(&user, &room, false));
        forget_archived_account(&user);
    }

    #[tokio::test]
    async fn archiving_right_after_a_join_stays_archived() {
        let (homeserver, lines) = serve_recording();
        let (_client, room) = client_with_room_for(&homeserver, "@join-archive:example.org").await;
        forget_archived_account(room.own_user_id());
        tokio::time::timeout(Duration::from_millis(500), unarchive_after_join(&room))
            .await
            .expect("a join is not held up");
        let (result, _) = tokio::time::timeout(BOUND, set_archived(&room, true, &FAST))
            .await.expect("bounded");
        assert!(result.is_ok(), "{result:?}");
        note_archived(&room, true);
        assert_eq!(on_tag_event(&room, true), None, "our own echo is no change");
        tokio::time::sleep(Duration::from_millis(300)).await;
        let deletes: Vec<String> = lines.lock().unwrap().iter()
            .filter(|line| line.starts_with("DELETE ")).cloned().collect();
        assert!(deletes.is_empty(), "{deletes:?}");
        assert!(is_archived(&room).await);
        forget_archived_account(room.own_user_id());
    }

    #[tokio::test]
    async fn a_store_read_never_undoes_a_newer_tag_event() {
        let (homeserver, _lines) = serve_recording();
        let (_client, room) = client_with_room_for(&homeserver, "@cache-order:example.org").await;
        forget_archived_account(room.own_user_id());
        note_archived(&room, true);
        remember_archived(room.own_user_id(), room.room_id(), false, false);
        assert!(is_archived(&room).await);
        forget_archived_account(room.own_user_id());
        assert!(!is_archived(&room).await, "the store has no tag for this room");
    }

    async fn client_with_room(homeserver: &str) -> (Client, matrix_sdk::Room) {
        client_with_room_for(homeserver, "@me:example.org").await
    }

    async fn client_with_room_for(homeserver: &str, user: &str) -> (Client, matrix_sdk::Room) {
        let client = Client::builder()
            .homeserver_url(homeserver)
            .handle_refresh_tokens()
            .build()
            .await
            .expect("client");
        let session = MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from(user).expect("user id"),
                device_id: OwnedDeviceId::from("TESTDEVICE"),
            },
            tokens: SessionTokens { access_token: "access-0".to_owned(), refresh_token: None },
        };
        client
            .matrix_auth()
            .restore_session(session, RoomLoadSettings::default())
            .await
            .expect("restore");
        let id = room_id!("!fav:example.org");
        let mut response = v5::Response::new("pos-1".to_owned());
        response.rooms.insert(id.to_owned(), v5::response::Room::default());
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        let room = client.get_room(id).expect("room known after sync");
        (client, room)
    }

    const BOUND: Duration = Duration::from_secs(20);

    /// Timing only: `cargo test --release room_payload_cost -- --ignored --nocapture`.
    #[tokio::test(flavor = "multi_thread")]
    #[ignore]
    async fn room_payload_cost_with_and_without_the_archived_cache() {
        const ROOMS: usize = 2000;
        let (homeserver, _lines) = serve_recording();
        let store = std::env::temp_dir()
            .join(format!("lightning-archived-bench-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&store);
        let client = Client::builder()
            .homeserver_url(&homeserver)
            .sqlite_store(&store, None)
            .build()
            .await
            .expect("client");
        client
            .matrix_auth()
            .restore_session(
                MatrixSession {
                    meta: SessionMeta {
                        user_id: OwnedUserId::try_from("@bench:example.org").expect("user"),
                        device_id: OwnedDeviceId::from("BENCH"),
                    },
                    tokens: SessionTokens { access_token: "a".to_owned(), refresh_token: None },
                },
                RoomLoadSettings::default(),
            )
            .await
            .expect("restore");
        let mut response = v5::Response::new("pos-1".to_owned());
        for i in 0..ROOMS {
            let id = matrix_sdk::ruma::OwnedRoomId::try_from(format!("!r{i}:example.org"))
                .expect("room id");
            response.rooms.insert(id.clone(), v5::response::Room::default());
            let tag = if i % 10 == 0 { ARCHIVED_TAG } else { "u.other" };
            let raw = matrix_sdk::ruma::serde::Raw::from_json_string(format!(
                r#"{{"type":"m.tag","content":{{"tags":{{"{tag}":{{}}}}}}}}"#
            ))
            .expect("raw tag");
            response.extensions.account_data.rooms.insert(id, vec![raw]);
        }
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        let events: crate::EventQueueRef =
            Arc::new(std::sync::Mutex::new(std::collections::VecDeque::new()));
        let user = client.user_id().expect("user").to_owned();
        let pass = |cache: bool| {
            let client = client.clone();
            let events = Arc::clone(&events);
            let user = user.clone();
            async move {
                if !cache {
                    forget_archived_account(&user);
                }
                let started = std::time::Instant::now();
                crate::enqueue_rooms_stamped(&events, &client, None).await;
                events.lock().unwrap().clear();
                started.elapsed()
            }
        };
        pass(false).await;
        let mut uncached = Vec::new();
        let mut cached = Vec::new();
        for _ in 0..10 {
            uncached.push(pass(false).await);
        }
        pass(true).await;
        for _ in 0..10 {
            cached.push(pass(true).await);
        }
        uncached.sort();
        cached.sort();
        let archived = client.joined_rooms().iter()
            .filter(|room| archived_known(room)).count();
        println!("rooms={ROOMS} archived={archived} enqueue_rooms_stamped median: \
                  store read per room {:?}, cached {:?}", uncached[5], cached[5]);
        let _ = std::fs::remove_dir_all(&store);
    }

    #[tokio::test]
    async fn a_favourite_write_survives_one_dropped_connection() {
        let (homeserver, tag_requests) = serve(TagScript::DropFirst);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        assert!(result.is_ok(), "a single dropped connection failed the write: {result:?}");
        assert_eq!(attempts, 2);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 2);
    }

    #[tokio::test]
    async fn the_sdk_alone_does_not_retry_a_dropped_connection() {
        let (homeserver, tag_requests) = serve(TagScript::DropAlways);
        let (_client, room) = client_with_room(&homeserver).await;

        let err = tokio::time::timeout(BOUND, room.set_is_favourite(true, None))
            .await
            .expect("bounded")
            .expect_err("a dropped connection cannot succeed");

        assert!(is_transport_failure(&err), "{err:?}");
        assert_eq!(tag_requests.load(Ordering::SeqCst), 1);
        assert!(
            matches!(failure_reason(&err).as_str(), "network" | "connect" | "timeout"),
            "{}",
            failure_reason(&err)
        );
    }

    #[tokio::test]
    async fn a_dead_server_is_retried_a_bounded_number_of_times() {
        let (homeserver, tag_requests) = serve(TagScript::DropAlways);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        let err = result.expect_err("every attempt was dropped");
        assert_eq!(attempts, 3);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 3);
        let event = failure_event("favourite", &err, attempts);
        assert_eq!(event["attempts"], 3);
        assert!(matches!(event["reason"].as_str(), Some("network" | "connect" | "timeout")),
                "{event}");
    }

    #[tokio::test]
    async fn a_refusal_is_not_retried_and_names_its_errcode() {
        let (homeserver, tag_requests) = serve(TagScript::Forbidden);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        let err = result.expect_err("the server refused");
        assert_eq!(attempts, 1);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 1);
        let event = failure_event("favourite", &err, attempts);
        assert_eq!(event["type"], "room_action_error");
        assert_eq!(event["action"], "favourite");
        assert_eq!(event["reason"], "http_403_M_FORBIDDEN");
        let text = event.to_string();
        assert!(!text.contains("secret") && !text.contains("!room"), "{text}");
    }

    #[tokio::test]
    async fn a_refusal_in_a_room_the_server_dropped_says_the_room_is_gone() {
        let (homeserver, _) = serve(TagScript::Forbidden);
        let (_client, room) = client_with_room(&homeserver).await;
        let (result, attempts) = set_archived(&room, true, &FAST).await;
        let event = room_failure_event(&room, "archive", &result.expect_err("refused"), attempts).await;
        assert_eq!(event["reason"], "room_gone");

        let (homeserver, _) = serve(TagScript::ForbiddenStillListed);
        let (_client, room) = client_with_room(&homeserver).await;
        let (result, attempts) = set_archived(&room, true, &FAST).await;
        let event = room_failure_event(&room, "archive", &result.expect_err("refused"), attempts).await;
        assert_eq!(event["reason"], "http_403_M_FORBIDDEN", "still listed: the refusal stands");
    }

    #[tokio::test]
    async fn a_forbidden_send_in_a_room_the_server_dropped_reads_room_gone() {
        let row = || json!({"op": "set", "item": {"send_error": "rejected", "send_forbidden": true}});

        let (homeserver, _) = serve(TagScript::Forbidden);
        let (_client, room) = client_with_room(&homeserver).await;
        let mut value = row();
        crate::timeline::annotate_room_gone(&room, &mut value).await;
        assert_eq!(value["item"]["send_error"], "room_gone");
        assert!(value["item"].get("send_forbidden").is_none());

        let mut snapshot = vec![json!({"send_error": "sent"}), row()["item"].clone()];
        crate::timeline::annotate_room_gone_rows(&room, &mut snapshot).await;
        assert_eq!(snapshot[1]["send_error"], "room_gone", "a reopened room's snapshot keeps it");
        assert!(snapshot[1].get("send_forbidden").is_none());

        let (homeserver, _) = serve(TagScript::ForbiddenStillListed);
        let (_client, room) = client_with_room(&homeserver).await;
        let mut value = json!({"op": "reset", "items": [row()["item"].clone()]});
        crate::timeline::annotate_room_gone(&room, &mut value).await;
        assert_eq!(value["items"][0]["send_error"], "rejected", "still listed: the refusal stands");
        assert!(value["items"][0].get("send_forbidden").is_none());
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn annotating_a_quote_never_waits_on_the_network() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                thread::spawn(move || {
                    if read_request_line(&mut stream).contains("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else {
                        thread::sleep(Duration::from_secs(60));
                    }
                });
            }
        });
        let (_client, room) = client_with_room(&format!("http://{addr}")).await;
        let mut rows = vec![json!({
            "reply_to_event_id": "$quoted:example.org", "reply_to_media_key": "k",
        })];
        tokio::time::timeout(
            Duration::from_secs(3),
            crate::timeline::annotate_room_gone_rows(&room, &mut rows),
        )
        .await
        .expect("a row emission must not wait on a server lookup");
        assert!(rows[0].get("reply_to_media_key").is_none(), "unknown keeps the thumbnail hidden");
    }

    #[tokio::test]
    async fn the_unread_mark_survives_one_dropped_connection() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let writes = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&writes);
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request_line(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.contains("/rooms/") && path.contains("/account_data/") {
                        if seen.fetch_add(1, Ordering::SeqCst) == 0 {
                            drop(stream);
                        } else {
                            respond(&mut stream, "200 OK", "{}");
                        }
                    } else if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        let homeserver = format!("http://{}:{}", addr.ip(), addr.port());
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_unread_flag(&room, true, &FAST)).await.expect("bounded");

        let writes = writes.load(Ordering::SeqCst);
        assert!(result.is_ok(), "{result:?} writes={writes}");
        assert_eq!(attempts, 2, "writes={writes}");
        assert_eq!(writes, 2);
    }
}
