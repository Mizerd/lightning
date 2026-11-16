
use std::collections::{BTreeMap, HashMap, HashSet};
use std::sync::{Mutex, OnceLock};
use std::future::Future;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::{membership::joined_rooms, sync::sync_events::v5},
        events::room::member::StrippedRoomMemberEvent,
        serde::Raw,
        OwnedRoomId, OwnedUserId, RoomId, UInt, UserId,
    },
    Client, Room, RoomState,
};
use matrix_sdk_base::RoomInfoNotableUpdateReasons;

use crate::EventQueueRef;

const FIRST_PASS_DELAY: Duration = Duration::from_secs(20);
const PASS_PERIOD: Duration = Duration::from_secs(300);
/// Invitations one answer may list. More than this and the answer is
/// incomplete, so nothing is retired.
const INVITE_PAGE: u32 = 200;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
const CONN_ID: &str = "lightning-invite-check";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ServerMembership {
    Pending,
    Joined,
    Gone,
}

#[derive(Debug, Default, Clone)]
pub(crate) struct ServerInvites {
    pub(crate) invited: HashSet<OwnedRoomId>,
    pub(crate) joined: HashSet<OwnedRoomId>,
}

impl ServerInvites {
    pub(crate) fn verdict(&self, room: &RoomId) -> ServerMembership {
        if self.invited.contains(room) {
            ServerMembership::Pending
        } else if self.joined.contains(room) {
            ServerMembership::Joined
        } else {
            ServerMembership::Gone
        }
    }
}

pub(crate) fn invite_list_complete(count: u64, returned: usize) -> bool {
    count <= returned as u64
}

fn is_pending(state: RoomState) -> bool {
    matches!(state, RoomState::Invited | RoomState::Knocked)
}

pub(crate) async fn fetch_server_invites(client: &Client) -> Option<ServerInvites> {
    let config = RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT);

    let mut filters = v5::request::ListFilters::default();
    filters.is_invite = Some(true);
    let mut list = v5::request::List::default();
    list.ranges = vec![(UInt::MIN, UInt::from(INVITE_PAGE - 1))];
    list.room_details.timeline_limit = UInt::MIN;
    list.filters = Some(filters);
    let mut request = v5::Request::new();
    request.conn_id = Some(CONN_ID.to_owned());
    request.timeout = Some(Duration::ZERO);
    request.lists = BTreeMap::from([("invites".to_owned(), list)]);
    let response = client.send(request).with_request_config(config).await.ok()?;
    let count = response.lists.get("invites").map(|l| u64::from(l.count))?;
    let invited: HashSet<OwnedRoomId> = response.rooms.into_keys().collect();
    if !invite_list_complete(count, invited.len()) {
        return None;
    }

    let joined = client
        .send(joined_rooms::v3::Request::new())
        .with_request_config(config)
        .await
        .ok()?
        .joined_rooms
        .into_iter()
        .collect();
    Some(ServerInvites { invited, joined })
}

async fn fetch_joined(client: &Client) -> Option<HashSet<OwnedRoomId>> {
    let config = RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT);
    let answer = client.send(joined_rooms::v3::Request::new()).with_request_config(config).await.ok()?;
    Some(answer.joined_rooms.into_iter().collect())
}

const JOINED_PROBE_TTL: Duration = Duration::from_secs(15);

type JoinedProbe = std::sync::Arc<tokio::sync::Mutex<Option<(std::time::Instant, Option<HashSet<OwnedRoomId>>)>>>;

static JOINED_PROBES: OnceLock<Mutex<HashMap<String, JoinedProbe>>> = OnceLock::new();

/// One joined-rooms answer per account is shared by every refusal within the TTL; a failed probe is cached too and reads as "not gone".
async fn joined_cached(client: &Client) -> Option<HashSet<OwnedRoomId>> {
    let key = format!("{:?}|{}", client.user_id(), client.homeserver());
    let slot = {
        let mut map = JOINED_PROBES.get_or_init(Default::default).lock().unwrap_or_else(|e| e.into_inner());
        map.retain(|_, s| {
            s.try_lock().map_or(true, |g| g.as_ref().map_or(true, |(at, _)| at.elapsed() < JOINED_PROBE_TTL))
        });
        map.entry(key).or_default().clone()
    };
    let mut guard = slot.lock().await;
    if let Some((at, answer)) = guard.as_ref() {
        if at.elapsed() < JOINED_PROBE_TTL {
            return answer.clone();
        }
    }
    let answer = fetch_joined(client).await;
    *guard = Some((std::time::Instant::now(), answer.clone()));
    answer
}

/// True only on a complete answer: a failed probe is never "gone".
pub(crate) async fn server_dropped_room(room: &Room) -> bool {
    room.state() == RoomState::Joined
        && joined_cached(&room.client()).await.is_some_and(|joined| !joined.contains(room.room_id()))
}

/// Marks a joined room left once the server no longer lists it; a sync that moved the room meanwhile wins.
async fn retire_joined_locally(room: &Room) -> bool {
    let mut retired = false;
    let saved = room
        .update_and_save_room_info(|mut info| {
            if info.state() == RoomState::Joined {
                info.mark_as_left();
                retired = true;
                (info, RoomInfoNotableUpdateReasons::MEMBERSHIP)
            } else {
                (info, RoomInfoNotableUpdateReasons::NONE)
            }
        })
        .await;
    saved.is_ok() && retired
}

/// Marks a still-pending room left in the local store. Returns whether it
/// changed anything: a sync that moved the room meanwhile wins.
pub(crate) async fn retire_locally(room: &Room) -> bool {
    if !is_pending(room.state()) {
        return false;
    }
    let mut retired = false;
    let saved = room
        .update_and_save_room_info(|mut info| {
            if is_pending(info.state()) {
                info.mark_as_left();
                retired = true;
                (info, RoomInfoNotableUpdateReasons::MEMBERSHIP)
            } else {
                (info, RoomInfoNotableUpdateReasons::NONE)
            }
        })
        .await;
    saved.is_ok() && retired
}

pub(crate) async fn leave_or_retire<F, Fut>(room: &Room, ask: F) -> Result<(), String>
where
    F: FnOnce() -> Fut,
    Fut: Future<Output = Option<ServerInvites>>,
{
    let was_pending = is_pending(room.state());
    match room.leave().await {
        Ok(()) => Ok(()),
        Err(error) => {
            let message = error.to_string();
            if was_pending {
                if let Some(answer) = ask().await {
                    if answer.verdict(room.room_id()) == ServerMembership::Gone
                        && retire_locally(room).await
                    {
                        return Ok(());
                    }
                }
            }
            else if server_dropped_room(room).await && retire_joined_locally(room).await {
                return Ok(());
            }
            Err(message)
        }
    }
}

pub(crate) async fn reconcile_with(client: &Client, answer: &ServerInvites) -> usize {
    let mut retired = 0;
    for room in client.rooms() {
        if is_pending(room.state())
            && answer.verdict(room.room_id()) == ServerMembership::Gone
            && retire_locally(&room).await
        {
            retired += 1;
        }
    }
    retired
}

fn any_pending(client: &Client) -> bool {
    client.rooms().iter().any(|room| is_pending(room.state()))
}

pub(crate) async fn run_stale_invite_reconciler(client: Client, events: EventQueueRef) {
    tokio::time::sleep(FIRST_PASS_DELAY).await;
    loop {
        if any_pending(&client) {
            if let Some(answer) = fetch_server_invites(&client).await {
                let retired = reconcile_with(&client, &answer).await;
                let background = crate::bgsession::for_queue(&events);
                if retired > 0 && background.is_none() {
                    crate::enqueue(
                        &events,
                        serde_json::json!({ "type": "stale_invites_retired", "count": retired }),
                    );
                    crate::enqueue_rooms(&events, &client).await;
                } else if let Some(bg) = background.filter(|_| retired > 0) {
                    bg.note_skipped();
                }
            }
        }
        tokio::time::sleep(PASS_PERIOD).await;
    }
}

type SentKey = (OwnedUserId, OwnedRoomId);

fn sent_times() -> &'static Mutex<HashMap<SentKey, u64>> {
    static TIMES: OnceLock<Mutex<HashMap<SentKey, u64>>> = OnceLock::new();
    TIMES.get_or_init(Default::default)
}

/// matrix-sdk drops `origin_server_ts` when it types a stripped event, so the
/// time only survives in the raw JSON when the server sends it; Synapse omits
/// it from sliding-sync invite_state, so first-seen is the usual fallback.
fn invite_sent_ms(raw: &str, own: &UserId, now_ms: u64) -> Option<u64> {
    let v: serde_json::Value = serde_json::from_str(raw).ok()?;
    if v.get("state_key")?.as_str()? != own.as_str()
        || v.get("content")?.get("membership")?.as_str()? != "invite"
    {
        return None;
    }
    let ts = v.get("origin_server_ts")?.as_u64()?;
    // An absent, zero or future stamp is a server's guess, not a send time.
    (ts > 0 && ts <= now_ms + 60_000).then_some(ts)
}

pub(crate) fn install_sent_time_handler(client: &Client) {
    client.add_event_handler(|raw: Raw<StrippedRoomMemberEvent>, room: Room| async move {
        if room.state() != RoomState::Invited {
            return;
        }
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_millis() as u64);
        if let Some(ts) = invite_sent_ms(raw.json().get(), room.own_user_id(), now) {
            sent_times()
                .lock()
                .unwrap()
                .insert((room.own_user_id().to_owned(), room.room_id().to_owned()), ts);
        }
    });
}

/// When the invitation was sent, if the server told us; 0 when unknown.
/// Forgets the entry once the room is no longer an invitation.
pub(crate) fn sent_ms(room: &Room) -> u64 {
    let key = (room.own_user_id().to_owned(), room.room_id().to_owned());
    lookup_or_prune(&mut sent_times().lock().unwrap(), &key, room.state() == RoomState::Invited)
}

fn lookup_or_prune(map: &mut HashMap<SentKey, u64>, key: &SentKey, invited: bool) -> u64 {
    if invited {
        map.get(key).copied().unwrap_or(0)
    } else {
        map.remove(key);
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk_base::RequestedRequiredStates;
    use serde_json::json;

    async fn invited(client: &Client, rooms: &[&RoomId]) {
        let me = client.user_id().expect("logged in").to_string();
        let mut response = v5::Response::new("pos-1".to_owned());
        for room in rooms {
            let mut entry = v5::response::Room::default();
            entry.invite_state = Some(vec![
                Raw::from_json_string(
                    json!({
                        "type": "m.room.member", "state_key": me,
                        "sender": "@inviter:example.org",
                        "content": { "membership": "invite" }
                    })
                    .to_string(),
                )
                .unwrap(),
            ]);
            response.rooms.insert((*room).to_owned(), entry);
        }
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        for room in rooms {
            assert_eq!(client.get_room(room).unwrap().state(), RoomState::Invited);
        }
    }

    fn answer(invited: &[&RoomId], joined: &[&RoomId]) -> ServerInvites {
        ServerInvites {
            invited: invited.iter().map(|r| (*r).to_owned()).collect(),
            joined: joined.iter().map(|r| (*r).to_owned()).collect(),
        }
    }

    #[test]
    fn invite_send_time_is_read_from_the_raw_member_event() {
        let me = matrix_sdk::ruma::user_id!("@me:example.org");
        let ev = |ts: &str, key: &str, ms: &str| {
            format!(
                r#"{{"type":"m.room.member","state_key":"{key}",{ts}"sender":"@a:b.c","content":{{"membership":"{ms}"}}}}"#
            )
        };
        let with = r#""origin_server_ts":1700000000000,"#;
        let now = 1_700_000_100_000;
        assert_eq!(invite_sent_ms(&ev(with, "@me:example.org", "invite"), me, now), Some(1_700_000_000_000));
        assert_eq!(invite_sent_ms(&ev("", "@me:example.org", "invite"), me, now), None, "spec-compliant stripped event");
        assert_eq!(invite_sent_ms(&ev(with, "@other:example.org", "invite"), me, now), None);
        assert_eq!(invite_sent_ms(&ev(with, "@me:example.org", "join"), me, now), None);
        assert_eq!(invite_sent_ms(&ev(r#""origin_server_ts":0,"#, "@me:example.org", "invite"), me, now), None);
        assert_eq!(invite_sent_ms(&ev(with, "@me:example.org", "invite"), me, 1_000), None, "future stamp");
    }

    #[test]
    fn a_send_time_is_forgotten_once_the_room_is_no_longer_invited() {
        let key: SentKey = (
            matrix_sdk::ruma::user_id!("@me:example.org").to_owned(),
            room_id!("!r:example.org").to_owned(),
        );
        let mut map = HashMap::from([(key.clone(), 5)]);
        assert_eq!(lookup_or_prune(&mut map, &key, true), 5);
        assert_eq!(lookup_or_prune(&mut map, &key, false), 0);
        assert!(map.is_empty());
    }

    #[test]
    fn only_absence_from_both_sets_means_gone() {
        let a = room_id!("!a:example.org");
        let b = room_id!("!b:example.org");
        let c = room_id!("!c:example.org");
        let server = answer(&[a], &[b]);
        assert_eq!(server.verdict(a), ServerMembership::Pending);
        assert_eq!(server.verdict(b), ServerMembership::Joined);
        assert_eq!(server.verdict(c), ServerMembership::Gone);
        assert!(invite_list_complete(3, 3));
        assert!(invite_list_complete(0, 0));
        assert!(!invite_list_complete(201, 200));
    }

    #[tokio::test]
    async fn an_invite_rejected_elsewhere_is_retired_only_on_the_servers_word() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let gone = room_id!("!rejected-elsewhere:example.org");
        let pending = room_id!("!still-invited:example.org");
        let joined_elsewhere = room_id!("!accepted-elsewhere:example.org");
        invited(&client, &[gone, pending, joined_elsewhere]).await;

        let retired = reconcile_with(&client, &answer(&[pending], &[joined_elsewhere])).await;

        assert_eq!(retired, 1);
        assert_eq!(client.get_room(gone).unwrap().state(), RoomState::Left);
        assert_eq!(client.get_room(pending).unwrap().state(), RoomState::Invited);
        assert_eq!(client.get_room(joined_elsewhere).unwrap().state(), RoomState::Invited);

        let again = reconcile_with(&client, &answer(&[], &[])).await;
        assert_eq!(again, 2, "only the two still-pending rooms remain");
    }

    // With no answer (request failed, list incomplete) nothing is touched,
    // and the fetch itself fails closed against a server that has no
    // sliding sync endpoint (the mock answers 404).
    #[tokio::test]
    async fn no_answer_retires_nothing() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = room_id!("!invited:example.org");
        invited(&client, &[room]).await;
        assert!(fetch_server_invites(&client).await.is_none());
        let r = client.get_room(room).unwrap();
        let result = leave_or_retire(&r, || async { None }).await;
        assert!(result.is_err(), "an unanswered question is not a leave");
        assert_eq!(r.state(), RoomState::Invited);
    }

    #[tokio::test]
    async fn a_refused_reject_falls_back_only_on_a_complete_answer() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let gone = room_id!("!gone:example.org");
        let pending = room_id!("!pending:example.org");
        invited(&client, &[gone, pending]).await;
        // No /leave mock: the mock server answers 404.

        let r = client.get_room(gone).unwrap();
        let result = leave_or_retire(&r, || async { Some(answer(&[pending], &[])) }).await;
        assert_eq!(result, Ok(()));
        assert_eq!(r.state(), RoomState::Left);

        let r = client.get_room(pending).unwrap();
        let result = leave_or_retire(&r, || async { Some(answer(&[pending], &[])) }).await;
        assert!(result.is_err(), "the server still lists it: the refusal stands");
        assert_eq!(r.state(), RoomState::Invited);
    }

    #[tokio::test]
    async fn refusals_share_one_joined_probe_and_a_failed_one_is_not_gone() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let a = server.sync_joined_room(&client, room_id!("!a:example.org")).await;
        let b = server.sync_joined_room(&client, room_id!("!b:example.org")).await;
        // No /joined_rooms mock: the mock server answers 404.
        let (x, y, z) = tokio::join!(
            server_dropped_room(&a),
            server_dropped_room(&b),
            server_dropped_room(&a)
        );
        assert!(!x && !y && !z, "a failed probe never reads as gone");
        let probes = server
            .server()
            .received_requests()
            .await
            .unwrap_or_default()
            .iter()
            .filter(|r| r.url.path().ends_with("/joined_rooms"))
            .count();
        assert_eq!(probes, 1);
    }
}
