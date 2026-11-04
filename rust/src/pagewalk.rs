//! Walking a run of history that the timeline filter empties, inside ONE
//! backward page.
//!
//! A room that hosts calls carries thousands of MatrixRTC membership events,
//! which `lightning_event_filter` drops before they become timeline items
//! (docs/timeline-scrolling.md). matrix-sdk's `paginate_backwards` serves ONE
//! STORED CHUNK per call (about twenty events) and ignores the batch size for
//! it, so walking such a run took one C++ round trip per chunk: dispatch, the
//! bridge poll, the controller's continuation delay, and one unit of the fill
//! or near-top budget each. Measured 2026-10-04: one room open made ~20 pages
//! with `added= 0` while `droppedRtc` climbed to ~987, and the reader had to
//! keep scrolling to get past it.
//!
//! Here the bridge keeps going while a page brought no event the timeline
//! would show and the start is not reached, under its own small bound, so C++
//! sees one page per walk. This is NOT a bigger page: each call asks for the
//! same count, a walk ends on the first page that brings anything shown (so it
//! inserts at most what one ordinary page inserts and the pane's row bounds are
//! untouched), and the time bound stops it after about one network round
//! trip's worth of slow pages.
//!
//! What a page brought is read from the room's EVENT CACHE update stream, not
//! from the timeline's items: matrix-sdk sends a pagination's update before
//! `paginate_backwards` returns, while the timeline applies it on its own task
//! some time later, so an item count taken right after a page can still be the
//! old one and a page of real messages would look empty.

use std::future::Future;
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::deserialized_responses::TimelineEvent;
use matrix_sdk::event_cache::{RoomEventCacheSubscriber, RoomEventCacheUpdate};
use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility;
use matrix_sdk::ruma::room_version_rules::RoomVersionRules;
use matrix_sdk_ui::eyeball_im::VectorDiff;
use matrix_sdk_ui::timeline::Timeline;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct PageOutcome {
    pub(crate) hit_start: bool,
    pub(crate) grew: bool,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct WalkBudget {
    pub(crate) max_pages: u32,
    pub(crate) max_time: Duration,
}

pub(crate) const FILTERED_RUN_WALK: WalkBudget = WalkBudget {
    max_pages: 8,
    max_time: Duration::from_millis(1000),
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct WalkOutcome {
    pub(crate) hit_start: bool,
    pub(crate) pages: u32,
}

pub(crate) async fn walk_filtered_run<F, Fut, E>(
    budget: WalkBudget,
    still_wanted: impl Fn() -> bool,
    mut page: F,
) -> Result<WalkOutcome, E>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = Result<PageOutcome, E>>,
{
    let started = tokio::time::Instant::now();
    let first = page().await?;
    let mut pages = 1u32;
    let mut last = first;
    while !last.hit_start
        && !last.grew
        && pages < budget.max_pages
        && started.elapsed() < budget.max_time
        && still_wanted()
    {
        match page().await {
            Ok(outcome) => {
                last = outcome;
                pages += 1;
            }
            Err(_) => break,
        }
    }
    Ok(WalkOutcome { hit_start: last.hit_start, pages })
}

pub(crate) fn inserts_any<T: Clone>(diffs: &[VectorDiff<T>], shown: impl Fn(&T) -> bool) -> bool {
    diffs.iter().any(|diff| match diff {
        VectorDiff::Append { values } | VectorDiff::Reset { values } => {
            values.iter().any(|value| shown(value))
        }
        VectorDiff::PushFront { value }
        | VectorDiff::PushBack { value }
        | VectorDiff::Insert { value, .. } => shown(value),
        _ => false,
    })
}

fn event_is_shown(event: &TimelineEvent, rules: &RoomVersionRules) -> bool {
    match event.raw().deserialize() {
        Ok(parsed) => crate::timeline::shown_by_lightning_filter(&parsed, rules),
        Err(_) => true,
    }
}

fn drain_shows_something(
    subscriber: &mut RoomEventCacheSubscriber,
    rules: &RoomVersionRules,
) -> bool {
    use tokio::sync::broadcast::error::TryRecvError;
    let mut shown = false;
    loop {
        match subscriber.try_recv() {
            Ok(RoomEventCacheUpdate::UpdateTimelineEvents(update)) => {
                if inserts_any(&update.diffs, |event| event_is_shown(event, rules)) {
                    shown = true;
                }
            }
            Ok(_) => {}
            // Updates were dropped, so what this page brought is unknown.
            Err(TryRecvError::Lagged(_)) => shown = true,
            Err(TryRecvError::Empty) | Err(TryRecvError::Closed) => break,
        }
    }
    shown
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct BackwardWalk {
    pub(crate) outcome: WalkOutcome,
    pub(crate) history_reopened: bool,
}

pub(crate) async fn walk_backwards(
    timeline: &Arc<Timeline>,
    count: u16,
    budget: WalkBudget,
    still_wanted: impl Fn() -> bool,
    claim_reopen: impl FnOnce() -> bool,
) -> Result<BackwardWalk, matrix_sdk_ui::timeline::Error> {
    let outcome = walk_once(timeline, count, budget, &still_wanted).await?;
    if !outcome.hit_start
        || !still_wanted()
        || !start_is_our_own_membership(timeline).await
        || !claim_reopen()
        || !reopen_history(timeline).await
    {
        return Ok(BackwardWalk { outcome, history_reopened: false });
    }
    let outcome = walk_once(timeline, count, budget, &still_wanted).await?;
    Ok(BackwardWalk { outcome, history_reopened: true })
}

// matrix-sdk keeps "start reached" for good once its linked chunk has no gap,
// e.g. after a `/messages` answered without `end` while the homeserver could
// not yet backfill a federated join; history the joiner may see is then lost.
async fn start_is_our_own_membership(timeline: &Timeline) -> bool {
    let room = timeline.room();
    if matches!(
        room.history_visibility_or_default(),
        HistoryVisibility::Joined | HistoryVisibility::Invited
    ) {
        return false;
    }
    let Ok((cache, _drop_handles)) = room.event_cache().await else { return false };
    let Ok(events) = cache.events().await else { return false };
    let Some(first) = events.first() else { return false };
    let raw = first.raw();
    let field = |name: &str| raw.get_field::<String>(name).ok().flatten();
    field("type").as_deref() == Some("m.room.member")
        && field("state_key").as_deref() == Some(room.own_user_id().as_str())
}

// Clearing the room's cache is the SDK's own way back to an unknown start: the
// next back-pagination asks `/messages` from the live end again.
async fn reopen_history(timeline: &Timeline) -> bool {
    match timeline.room().event_cache().await {
        Ok((cache, _drop_handles)) => cache.clear().await.is_ok(),
        Err(_) => false,
    }
}

async fn walk_once(
    timeline: &Arc<Timeline>,
    count: u16,
    budget: WalkBudget,
    still_wanted: &impl Fn() -> bool,
) -> Result<WalkOutcome, matrix_sdk_ui::timeline::Error> {
    let room = timeline.room();
    let rules = room.clone_info().room_version_rules_or_default();
    // Subscribed BEFORE the first page, and the drop handles kept for the
    // whole walk. Without a subscription nothing is known about a page, so
    // every page counts as productive and the walk is one page, as before it.
    let cache = room.event_cache().await.ok();
    let subscriber = match cache.as_ref() {
        Some((room_cache, _drop_handles)) => {
            room_cache.subscribe().await.ok().map(|(_events, subscriber)| subscriber)
        }
        None => None,
    };
    let subscriber = Arc::new(tokio::sync::Mutex::new(subscriber));
    walk_filtered_run(budget, still_wanted, || {
        let timeline = Arc::clone(timeline);
        let subscriber = Arc::clone(&subscriber);
        let rules = rules.clone();
        async move {
            let hit_start = timeline.paginate_backwards(count).await?;
            let mut guard = subscriber.lock().await;
            let grew = match guard.as_mut() {
                Some(subscriber) => drain_shows_something(subscriber, &rules),
                None => true,
            };
            Ok::<_, matrix_sdk_ui::timeline::Error>(PageOutcome { hit_start, grew })
        }
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::{walk_filtered_run, PageOutcome, WalkBudget, WalkOutcome};
    use std::cell::Cell;
    use std::time::Duration;

    const FILTERED: PageOutcome = PageOutcome { hit_start: false, grew: false };
    const GREW: PageOutcome = PageOutcome { hit_start: false, grew: true };
    const START: PageOutcome = PageOutcome { hit_start: true, grew: false };

    fn budget(max_pages: u32) -> WalkBudget {
        WalkBudget { max_pages, max_time: Duration::from_secs(60) }
    }

    async fn walk(
        script: &[Result<PageOutcome, ()>],
        budget: WalkBudget,
        wanted: bool,
    ) -> (Result<WalkOutcome, ()>, usize) {
        let calls = Cell::new(0usize);
        let result = walk_filtered_run(budget, || wanted, || {
            let index = calls.get();
            calls.set(index + 1);
            let answer = script.get(index).copied().unwrap_or(Ok(FILTERED));
            async move { answer }
        })
        .await;
        (result, calls.get())
    }

    #[tokio::test]
    async fn a_filtered_run_is_walked_in_one_request_until_something_appears() {
        let (result, calls) =
            walk(&[Ok(FILTERED), Ok(FILTERED), Ok(FILTERED), Ok(GREW)], budget(8), true)
                .await;
        assert_eq!(calls, 4);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 4 }));
    }

    #[tokio::test]
    async fn a_page_that_adds_anything_is_never_followed_by_another() {
        let (result, calls) = walk(&[Ok(GREW)], budget(8), true).await;
        assert_eq!(calls, 1);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[tokio::test]
    async fn the_start_of_history_ends_the_walk_and_is_reported() {
        let (result, calls) = walk(&[Ok(FILTERED), Ok(START)], budget(8), true).await;
        assert_eq!(calls, 2);
        assert_eq!(result, Ok(WalkOutcome { hit_start: true, pages: 2 }));
    }

    #[tokio::test]
    async fn an_endless_filtered_run_stops_at_the_page_bound() {
        let (result, calls) = walk(&[], budget(5), true).await;
        assert_eq!(calls, 5, "the walk ignored its page bound");
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 5 }));
    }

    #[tokio::test]
    async fn an_endless_filtered_run_stops_at_the_time_bound() {
        let calls = Cell::new(0usize);
        let result = walk_filtered_run(
            WalkBudget { max_pages: 1000, max_time: Duration::from_millis(300) },
            || true,
            || {
                calls.set(calls.get() + 1);
                async {
                    tokio::time::sleep(Duration::from_millis(30)).await;
                    Ok::<_, ()>(FILTERED)
                }
            },
        )
        .await;
        assert!(result.is_ok());
        assert!(calls.get() >= 2, "the walk never walked");
        assert!(calls.get() <= 12, "the walk ignored its time bound: {} pages", calls.get());
    }

    #[tokio::test]
    async fn a_closed_room_is_not_walked_any_further() {
        let (result, calls) = walk(&[], budget(8), false).await;
        assert_eq!(calls, 1, "a page was walked for a room nobody has open");
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[tokio::test]
    async fn a_first_page_error_is_reported_and_a_later_one_ends_the_walk_quietly() {
        let (result, calls) = walk(&[Err(())], budget(8), true).await;
        assert_eq!(calls, 1);
        assert_eq!(result, Err(()));

        let (result, calls) = walk(&[Ok(FILTERED), Err(())], budget(8), true).await;
        assert_eq!(calls, 2);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[test]
    fn only_insertions_of_a_shown_item_count_as_growth() {
        use super::inserts_any;
        use matrix_sdk_ui::eyeball_im::VectorDiff;
        let shown = |value: &u32| *value >= 100;

        assert!(!inserts_any::<u32>(&[], shown));
        assert!(!inserts_any(&[VectorDiff::Append { values: vec![1, 2, 3].into() }], shown));
        assert!(inserts_any(&[VectorDiff::Append { values: vec![1, 100].into() }], shown));
        assert!(inserts_any(&[VectorDiff::PushFront { value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::PushBack { value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::Insert { index: 0, value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::Reset { values: vec![100].into() }], shown));
        assert!(!inserts_any(&[VectorDiff::Set { index: 0, value: 100 }], shown));
        assert!(!inserts_any(
            &[VectorDiff::Remove { index: 0 }, VectorDiff::Clear, VectorDiff::PopFront],
            shown,
        ));
    }

    #[test]
    fn paginate_back_runs_the_walk() {
        let source = include_str!("timeline.rs");
        let start = source.find("pub fn paginate_back(").expect("paginate_back exists");
        let end = source[start..]
            .find("pub fn send_text(")
            .map(|offset| start + offset)
            .expect("paginate_back is followed by send_text");
        let body = &source[start..end];
        assert!(body.contains("crate::pagewalk::walk_backwards("),
                "paginate_back no longer walks filtered history");
        assert!(body.contains("crate::pagewalk::FILTERED_RUN_WALK"),
                "paginate_back walks without the documented bound");
        assert!(!body.contains(".paginate_backwards("),
                "paginate_back pages the timeline directly again");
    }
}

#[cfg(test)]
mod sdk_tests {
    use super::{walk_backwards, WalkBudget};
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        config::SyncSettings,
        ruma::{OwnedDeviceId, OwnedRoomId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };
    use matrix_sdk_ui::timeline::{Timeline, TimelineBuilder, TimelineFocus};
    use serde_json::{json, Value};

    const ROOM: &str = "!churn:example.org";
    const ME: &str = "@me:example.org";

    fn read_request(stream: &mut TcpStream) -> String {
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

    fn rtc(n: u64, ts: u64) -> Value {
        json!({
            "type": "org.matrix.msc3401.call.member",
            "state_key": format!("_@caller:example.org_DEV{n}"),
            "sender": "@caller:example.org",
            "event_id": format!("$rtc{n}"),
            "origin_server_ts": ts,
            "room_id": ROOM,
            "content": {},
        })
    }

    fn message(id: &str, ts: u64) -> Value {
        json!({
            "type": "m.room.message",
            "sender": "@caller:example.org",
            "event_id": id,
            "origin_server_ts": ts,
            "room_id": ROOM,
            "content": { "msgtype": "m.text", "body": "a real message" },
        })
    }

    fn churn_page(first: u64, newest_ts: u64) -> Vec<Value> {
        (0..20).map(|i| rtc(first + i, newest_ts - i)).collect()
    }

    fn sync_body() -> String {
        let room = json!({
                "state": { "events": [
                    {
                        "type": "m.room.create", "state_key": "", "sender": ME,
                        "event_id": "$create", "origin_server_ts": 1_000,
                        "content": { "creator": ME, "room_version": "10" },
                    },
                    {
                        "type": "m.room.member", "state_key": ME, "sender": ME,
                        "event_id": "$me", "origin_server_ts": 1_001,
                        "content": { "membership": "join" },
                    },
                ]},
                "timeline": {
                    "events": [rtc(0, 10_000_000)],
                    "limited": true,
                    "prev_batch": "t1",
                },
        });
        let mut join = serde_json::Map::new();
        join.insert(ROOM.to_owned(), room);
        json!({ "next_batch": "s1", "rooms": { "join": join } }).to_string()
    }

    fn messages_body(from: &str) -> String {
        let (chunk, end): (Vec<Value>, Option<&str>) = match from {
            "t1" => (churn_page(1, 9_000_000), Some("t2")),
            "t2" => (churn_page(21, 8_000_000), Some("t3")),
            "t3" => (vec![message("$hello", 7_000_000)], Some("t4")),
            "t4" => (vec![message("$older", 6_000_000)], Some("t5")),
            _ => (Vec::new(), None),
        };
        let mut body = json!({ "start": from, "chunk": chunk, "state": [] });
        if let Some(end) = end {
            body["end"] = json!(end);
        }
        body.to_string()
    }

    fn query_value<'a>(target: &'a str, key: &str) -> Option<&'a str> {
        target.split_once('?')?.1.split('&').find_map(|pair| {
            pair.split_once('=').filter(|(k, _)| *k == key).map(|(_, v)| v)
        })
    }

    fn serve(tokens: Arc<Mutex<Vec<String>>>) -> String {
        serve_room(tokens, sync_body, messages_body)
    }

    fn serve_room(
        tokens: Arc<Mutex<Vec<String>>>,
        sync: fn() -> String,
        messages: fn(&str) -> String,
    ) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let tokens = Arc::clone(&tokens);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let mut parts = line.split_whitespace();
                    let method = parts.next().unwrap_or("").to_owned();
                    let target = parts.next().unwrap_or("").to_owned();
                    let path = target.split('?').next().unwrap_or("").to_owned();
                    if path == "/_matrix/client/versions" {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else if path.ends_with("/sync") {
                        respond(&mut stream, "200 OK", &sync());
                    } else if path.ends_with("/messages") {
                        let from = query_value(&target, "from").unwrap_or("").to_owned();
                        tokens.lock().unwrap().push(from.clone());
                        respond(&mut stream, "200 OK", &messages(&from));
                    } else if method == "POST" && path.ends_with("/keys/upload") {
                        respond(&mut stream, "200 OK",
                                r#"{"one_time_key_counts":{"signed_curve25519":50}}"#);
                    } else if method == "POST" && path.ends_with("/keys/query") {
                        respond(&mut stream, "200 OK", r#"{"device_keys":{},"failures":{}}"#);
                    } else {
                        respond(&mut stream, "404 Not Found",
                                r#"{"errcode":"M_UNRECOGNIZED","error":"Unrecognized request"}"#);
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    #[test]
    fn a_churn_run_is_crossed_in_one_request_and_the_walk_stops_at_the_message() {
        let tokens: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let addr = serve(Arc::clone(&tokens));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let outcome = runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: SessionMeta {
                            user_id: OwnedUserId::try_from(ME).expect("user id"),
                            device_id: OwnedDeviceId::from("TESTDEVICE"),
                        },
                        tokens: SessionTokens {
                            access_token: "test-token".to_owned(),
                            refresh_token: None,
                        },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");
            client.event_cache().subscribe().expect("event cache");
            client.sync_once(SyncSettings::default()).await.expect("sync");
            let room_id = OwnedRoomId::try_from(ROOM).expect("room id");
            let room = client.get_room(&room_id).expect("the synced room");
            let timeline = Arc::new(
                TimelineBuilder::new(&room)
                    .event_filter(crate::timeline::lightning_event_filter)
                    .with_focus(TimelineFocus::Live { hide_threaded_events: true })
                    .build()
                    .await
                    .expect("timeline"),
            );
            walk_backwards(
                &timeline,
                20,
                WalkBudget { max_pages: 8, max_time: Duration::from_secs(20) },
                || true,
                || true,
            )
            .await
            .expect("walk")
            .outcome
        });

        let asked = tokens.lock().unwrap().clone();
        assert!(asked.iter().any(|t| t == "t1"), "{asked:?}");
        assert!(asked.iter().any(|t| t == "t2"), "{asked:?}");
        assert!(asked.iter().any(|t| t == "t3"), "the walk stopped inside the churn: {asked:?}");
        assert!(!asked.iter().any(|t| t == "t4"),
                "the walk went past a page that brought a message: {asked:?}");
        assert!(!outcome.hit_start, "{outcome:?}");
        assert!(outcome.pages >= 3, "{outcome:?}");
    }

    const JOINED: &str = "!joined:example.org";

    fn joined_room_event(kind: &str, id: &str, ts: u64, sender: &str, extra: Value) -> Value {
        let mut event = json!({
            "type": kind,
            "sender": sender,
            "event_id": id,
            "origin_server_ts": ts,
            "room_id": JOINED,
        });
        if let (Some(event), Some(extra)) = (event.as_object_mut(), extra.as_object()) {
            for (key, value) in extra {
                event.insert(key.clone(), value.clone());
            }
        }
        event
    }

    fn joined_text(id: &str, ts: u64) -> Value {
        joined_room_event("m.room.message", id, ts, "@host:remote.org",
                          json!({ "content": { "msgtype": "m.text", "body": "hello" } }))
    }

    fn our_join() -> Value {
        joined_room_event("m.room.member", "$ourjoin", 5_000_000, ME,
                          json!({ "state_key": ME, "content": { "membership": "join" } }))
    }

    fn joined_sync(visibility: &str) -> String {
        let room = json!({
            "state": { "events": [
                joined_room_event("m.room.create", "$create", 1_000, "@host:remote.org",
                                  json!({ "state_key": "",
                                          "content": { "creator": "@host:remote.org",
                                                       "room_version": "10" } })),
                joined_room_event("m.room.history_visibility", "$vis", 1_001,
                                  "@host:remote.org",
                                  json!({ "state_key": "",
                                          "content": { "history_visibility": visibility } })),
            ]},
            "timeline": {
                "events": [our_join(), joined_text("$after", 6_000_000)],
                "limited": false,
                "prev_batch": "t1",
            },
        });
        let mut join = serde_json::Map::new();
        join.insert(JOINED.to_owned(), room);
        json!({ "next_batch": "s1", "rooms": { "join": join } }).to_string()
    }

    fn shared_sync() -> String {
        joined_sync("shared")
    }

    fn joined_only_sync() -> String {
        joined_sync("joined")
    }

    // Synapse answers `/messages` with no `end` when it could not backfill
    // over federation; asked again from the live end, it has the history.
    fn backfilled_messages(from: &str) -> String {
        let (chunk, end): (Vec<Value>, Option<&str>) = match from {
            "" => (vec![joined_text("$after", 6_000_000), our_join(),
                        joined_text("$older", 4_000_000)], Some("t2")),
            _ => (Vec::new(), None),
        };
        let mut body = json!({ "start": from, "chunk": chunk, "state": [] });
        if let Some(end) = end {
            body["end"] = json!(end);
        }
        body.to_string()
    }

    async fn walk_for_test(
        timeline: &Arc<Timeline>,
        budget: WalkBudget,
        allow_reopen: bool,
    ) -> bool {
        let walk = walk_backwards(timeline, 20, budget, || true, || allow_reopen)
            .await
            .expect("walk");
        walk.outcome.hit_start
    }

    struct JoinedWalk {
        hit_start: bool,
        asked: Vec<String>,
        cached: Vec<String>,
    }

    fn walk_joined_room(sync: fn() -> String, allow_reopen: bool) -> JoinedWalk {
        let tokens: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let addr = serve_room(Arc::clone(&tokens), sync, backfilled_messages);
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let (hit_start, cached) = runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: SessionMeta {
                            user_id: OwnedUserId::try_from(ME).expect("user id"),
                            device_id: OwnedDeviceId::from("TESTDEVICE"),
                        },
                        tokens: SessionTokens {
                            access_token: "test-token".to_owned(),
                            refresh_token: None,
                        },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");
            client.event_cache().subscribe().expect("event cache");
            client.sync_once(SyncSettings::default()).await.expect("sync");
            let room_id = OwnedRoomId::try_from(JOINED).expect("room id");
            let room = client.get_room(&room_id).expect("the synced room");
            let timeline = Arc::new(
                TimelineBuilder::new(&room)
                    .event_filter(crate::timeline::lightning_event_filter)
                    .with_focus(TimelineFocus::Live { hide_threaded_events: true })
                    .build()
                    .await
                    .expect("timeline"),
            );
            let budget = WalkBudget { max_pages: 8, max_time: Duration::from_secs(20) };
            let hit_start = walk_for_test(&timeline, budget, allow_reopen).await;
            let (cache, _handles) = room.event_cache().await.expect("room cache");
            let cached = cache
                .events()
                .await
                .expect("events")
                .iter()
                .filter_map(|event| event.event_id().map(|id| id.to_string()))
                .collect::<Vec<_>>();
            (hit_start, cached)
        });
        let asked = tokens.lock().unwrap().clone();
        JoinedWalk { hit_start, asked, cached }
    }

    #[test]
    fn a_start_at_our_own_join_is_asked_of_the_server_once_more() {
        let walk = walk_joined_room(shared_sync, true);
        assert!(!walk.hit_start, "the start was declared at our own join: {:?}", walk.asked);
        assert_eq!(walk.asked, vec!["t1".to_owned(), String::new()]);
        assert!(walk.cached.iter().any(|id| id == "$older"), "{:?}", walk.cached);
    }

    #[test]
    fn a_start_at_our_own_join_stands_when_joiners_cannot_see_history() {
        let walk = walk_joined_room(joined_only_sync, true);
        assert!(walk.hit_start);
        assert_eq!(walk.asked, vec!["t1".to_owned()]);
    }

    #[test]
    fn a_start_at_our_own_join_is_asked_again_only_when_the_caller_allows_it() {
        let walk = walk_joined_room(shared_sync, false);
        assert!(walk.hit_start);
        assert_eq!(walk.asked, vec!["t1".to_owned()]);
    }
}
