//! Joined rooms the server no longer has the user in.
//!
//! A room left from another session, or one the user was kicked or banned
//! from, reaches this client only if sync mentions it again. matrix-sdk-base
//! moves a room out of `Joined` only when a sliding sync response carries the
//! room with our own `m.room.member` in `required_state`
//! (`response_processors/room/msc4186/mod.rs`, `membership()`); a room that
//! merely stops being mentioned stays `Joined` in the store for ever, and the
//! room list's non-left filter keeps showing it.
//!
//! Measured 2026-10-16 against Continuwuity 26.9.1 (simplified MSC3575, the
//! same list request `RoomListService` sends): after a `/leave` from another
//! device, a kick and a ban alike, the very next response drops the list's
//! `count` by one and names no room at all, no `required_state`, no timeline.
//! A room subscription for that room returns nothing either, a fresh
//! connection never mentions it, and `GET .../state/m.room.member/{me}` is a
//! 404. So the room stayed in Conversations with an enabled composer, through
//! restarts (break-it rerun section 4 F3, and B034 "kicked while viewing").
//!
//! The server does answer the question when asked directly, so this asks:
//!
//! 1. `GET /joined_rooms` names the rooms the server has us joined to. A room
//!    the store holds as joined and that is missing there is a CANDIDATE only:
//!    absence is not evidence (a race with a join, a server quirk).
//! 2. A one-shot `/sync` with `since=0`, filtered to just the candidates and
//!    `include_leave`, returns each room we really left under `rooms.leave`
//!    with our own membership event (`leave` or `ban`, and who sent it). Only
//!    that positive answer retires a room; a room the answer lists as joined,
//!    or does not mention, is left alone. `set_presence=offline`, so asking
//!    never marks the user online (see presence.rs).
//!
//!    NEVER WITHOUT `since`. On Continuwuity 26.9.1 a v3 `/sync` with no
//!    `since` hands back the device's pending to-device messages AND DELETES
//!    THEM: measured, the next v3 call returned none and the sliding sync
//!    `to_device` extension never delivered them. That is where room keys,
//!    MatrixRTC media keys, verification and key requests arrive, so one pass
//!    would have silently dropped whatever was in flight. With `since=0` the
//!    same request still lists the left rooms under `rooms.leave` (left,
//!    kicked and banned alike) and deletes nothing: measured, two calls in a
//!    row each returned the three pending messages and sliding sync then
//!    delivered all three. On Synapse `0` is not a stream token, so the
//!    request fails and the pass fails closed (Synapse delivers the leave
//!    over sliding sync itself); NOT measured there.
//!
//! The retirement is local: the room is marked left (or banned) in the store,
//! exactly as `serveradmin::mark_deleted_room_left` does, and the room list's
//! non-left filter drops it. Nothing is sent to the room.
//!
//! When to ask: the sliding sync list's server-side `count`. It is read on the
//! first load (a restart after a leave), whenever it FALLS (the measured shape
//! of a leave), and whenever the store holds more rooms than the server counts
//! (a leave and a join in one window). On a server that delivers leaves
//! (Synapse) the store and the server agree and a pass is one `/joined_rooms`.

use std::collections::{BTreeMap, HashSet};
use std::future::Future;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::{
            filter::{FilterDefinition, LazyLoadOptions, RoomEventFilter, RoomFilter},
            membership::joined_rooms,
            sync::sync_events::v3,
        },
        presence::PresenceState,
        serde::Raw,
        OwnedRoomId, UInt, UserId,
    },
    Client, Room, RoomState,
};
use matrix_sdk_base::RoomInfoNotableUpdateReasons;
use matrix_sdk_ui::room_list_service::{RoomListLoadingState, RoomListService};
use serde::Deserialize;

use crate::EventQueueRef;

/// After a count change, so the response that moved it has been processed.
const SETTLE: Duration = Duration::from_secs(2);
/// Passes are at least this far apart; a trigger inside the gap waits for it.
const MIN_PASS_GAP: Duration = Duration::from_secs(30);
/// One question each; never retried (the next trigger is the retry).
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
/// Rooms one confirmation asks about. More wait for the next pass.
const CONFIRM_CAP: usize = 50;
/// How often to look for a (re)built room list service.
const SERVICE_POLL: Duration = Duration::from_secs(2);
/// The confirmation's `since`. Present so the server treats the request as
/// incremental and acknowledges (deletes) no to-device message; see the
/// module doc for the measurement.
const CONFIRM_SINCE: &str = "0";

/// How the server says we lost a room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Loss {
    /// Our own leave, from another session.
    Left,
    /// A leave someone else sent: kicked.
    Kicked,
    /// Banned.
    Banned,
    /// Listed under `rooms.leave` with no readable membership event.
    Gone,
}

impl Loss {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Loss::Left => "left",
            Loss::Kicked => "kicked",
            Loss::Banned => "banned",
            Loss::Gone => "gone",
        }
    }
}

/// Decides, from the server's list count, whether a pass is worth asking.
#[derive(Debug, Default)]
pub(crate) struct Trigger {
    last_count: Option<u32>,
}

impl Trigger {
    /// Pure. `local` is how many rooms the store lists (joined, invited or
    /// knocked). True on the first count, on any fall, and while the store
    /// lists more than the server counts.
    pub(crate) fn observe(&mut self, server_count: u32, local: usize) -> bool {
        let first = self.last_count.is_none();
        let fell = self.last_count.is_some_and(|last| server_count < last);
        self.last_count = Some(server_count);
        first || fell || local as u64 > u64::from(server_count)
    }
}

/// Pure: the rooms the store holds as joined that the server does not list.
pub(crate) fn candidates<'a>(
    held_joined: impl IntoIterator<Item = &'a OwnedRoomId>,
    server_joined: &HashSet<OwnedRoomId>,
) -> Vec<OwnedRoomId> {
    let mut out: Vec<OwnedRoomId> = held_joined
        .into_iter()
        .filter(|room| !server_joined.contains(*room))
        .cloned()
        .collect();
    out.sort();
    out.dedup();
    out.truncate(CONFIRM_CAP);
    out
}

#[derive(Deserialize)]
struct MemberContent {
    membership: String,
}

#[derive(Deserialize)]
struct MemberEvent {
    #[serde(rename = "type")]
    kind: String,
    #[serde(default)]
    state_key: Option<String>,
    sender: String,
    content: MemberContent,
}

/// Our own membership in one event, if it is one: (membership, sender).
fn own_membership<T>(raw: &Raw<T>, me: &UserId) -> Option<(String, String)> {
    let event: MemberEvent = raw.deserialize_as_unchecked().ok()?;
    (event.kind == "m.room.member" && event.state_key.as_deref() == Some(me.as_str()))
        .then_some((event.content.membership, event.sender))
}

/// Pure: what one `rooms.leave` entry says about how we lost the room. The
/// newest own membership wins: the timeline's last, else the state's.
pub(crate) fn loss_from_left_room(room: &v3::LeftRoom, me: &UserId) -> Loss {
    let from_timeline = room
        .timeline
        .events
        .iter()
        .rev()
        .find_map(|raw| own_membership(raw, me));
    let from_state = || {
        let events = match &room.state {
            v3::State::Before(state) => &state.events,
            v3::State::After(state) => &state.events,
            _ => return None,
        };
        events.iter().rev().find_map(|raw| own_membership(raw, me))
    };
    match from_timeline.or_else(from_state) {
        Some((membership, _)) if membership == "ban" => Loss::Banned,
        Some((membership, sender)) if membership == "leave" => {
            if sender == me.as_str() {
                Loss::Left
            } else {
                Loss::Kicked
            }
        }
        // `rooms.leave` is the server's word that we are out; an event we
        // cannot read does not change that, it only hides how.
        _ => Loss::Gone,
    }
}

/// Pure: the candidates the answer confirms lost. A room the answer lists as
/// joined or invited, or does not mention, is not.
pub(crate) fn confirmed_losses(
    leave: &BTreeMap<OwnedRoomId, v3::LeftRoom>,
    candidates: &[OwnedRoomId],
    me: &UserId,
) -> Vec<(OwnedRoomId, Loss)> {
    candidates
        .iter()
        .filter_map(|room| leave.get(room).map(|left| (room.clone(), loss_from_left_room(left, me))))
        .collect()
}

fn request_config() -> RequestConfig {
    RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT)
}

async fn fetch_server_joined(client: &Client) -> Option<HashSet<OwnedRoomId>> {
    let response = client
        .send(joined_rooms::v3::Request::new())
        .with_request_config(request_config())
        .await
        .ok()?;
    Some(response.joined_rooms.into_iter().collect())
}

/// The filter that asks only about `rooms`, including the ones we left, with
/// the one newest event each (our own leave, for a left room).
pub(crate) fn confirm_filter(rooms: &[OwnedRoomId]) -> FilterDefinition {
    let mut timeline = RoomEventFilter::default();
    timeline.limit = Some(UInt::from(1u32));
    let mut state = RoomEventFilter::default();
    state.types = Some(vec!["m.room.member".to_owned()]);
    state.lazy_load_options = LazyLoadOptions::Enabled { include_redundant_members: false };
    let mut room = RoomFilter::default();
    room.include_leave = true;
    room.rooms = Some(rooms.to_vec());
    room.timeline = timeline;
    room.state = state;
    room.ephemeral = RoomEventFilter::ignore_all();
    room.account_data = RoomEventFilter::ignore_all();
    let mut filter = FilterDefinition::default();
    filter.room = room;
    filter.presence = matrix_sdk::ruma::api::client::filter::Filter::ignore_all();
    filter.account_data = matrix_sdk::ruma::api::client::filter::Filter::ignore_all();
    filter
}

/// The one-shot request: `since=0`, no wait, and never marks the user online.
/// It must never go out without a `since` (the module doc: on Continuwuity
/// that deletes the device's pending to-device messages).
pub(crate) fn confirm_request(rooms: &[OwnedRoomId]) -> v3::Request {
    let mut request = v3::Request::new();
    request.since = Some(CONFIRM_SINCE.to_owned());
    request.filter = Some(v3::Filter::FilterDefinition(confirm_filter(rooms)));
    request.set_presence = PresenceState::Offline;
    request.timeout = Some(Duration::ZERO);
    request
}

async fn fetch_left(
    client: &Client,
    rooms: &[OwnedRoomId],
) -> Option<BTreeMap<OwnedRoomId, v3::LeftRoom>> {
    let response = client
        .send(confirm_request(rooms))
        .with_request_config(request_config())
        .await
        .ok()?;
    Some(response.rooms.leave)
}

/// Marks a still-joined room left (or banned) in the local store. Returns
/// whether it changed anything: a sync that moved the room meanwhile wins.
pub(crate) async fn retire_locally(room: &Room, loss: Loss) -> bool {
    if room.state() != RoomState::Joined {
        return false;
    }
    let mut retired = false;
    let saved = room
        .update_and_save_room_info(|mut info| {
            if info.state() == RoomState::Joined {
                if loss == Loss::Banned {
                    info.set_state(RoomState::Banned);
                } else {
                    info.mark_as_left();
                }
                info.mark_state_partially_synced();
                info.mark_members_missing();
                retired = true;
                (info, RoomInfoNotableUpdateReasons::MEMBERSHIP)
            } else {
                (info, RoomInfoNotableUpdateReasons::NONE)
            }
        })
        .await;
    saved.is_ok() && retired
}

/// Retires every confirmed loss and tells C++ about each. Returns how many.
pub(crate) async fn apply_losses(
    client: &Client,
    losses: &[(OwnedRoomId, Loss)],
    events: &EventQueueRef,
) -> usize {
    let mut retired = 0;
    for (room_id, loss) in losses {
        let Some(room) = client.get_room(room_id) else { continue };
        // Read before the state moves: the name a left room computes may not
        // be the one the user knew it by.
        let name = room
            .cached_display_name()
            .map(|name| name.to_string())
            .unwrap_or_default();
        if retire_locally(&room, *loss).await {
            retired += 1;
            crate::enqueue(
                events,
                serde_json::json!({
                    "type": "room_membership_lost",
                    "room_id": room_id.to_string(),
                    "loss": loss.as_str(),
                    "name": name,
                }),
            );
        }
    }
    retired
}

/// One pass against the server's two answers: `joined` (`/joined_rooms`) and
/// `left` (the confirmation, asked about the candidates only). Fails closed:
/// an unanswered question retires nothing. Returns how many rooms it retired.
pub(crate) async fn reconcile_with<J, JF, L, LF>(
    client: &Client,
    events: &EventQueueRef,
    joined: J,
    left: L,
) -> usize
where
    J: FnOnce() -> JF,
    JF: Future<Output = Option<HashSet<OwnedRoomId>>>,
    L: FnOnce(Vec<OwnedRoomId>) -> LF,
    LF: Future<Output = Option<BTreeMap<OwnedRoomId, v3::LeftRoom>>>,
{
    let held: Vec<OwnedRoomId> =
        client.joined_rooms().iter().map(|room| room.room_id().to_owned()).collect();
    if held.is_empty() {
        return 0;
    }
    let Some(server_joined) = joined().await else { return 0 };
    let candidates = candidates(held.iter(), &server_joined);
    if candidates.is_empty() {
        return 0;
    }
    let Some(me) = client.user_id().map(ToOwned::to_owned) else { return 0 };
    let Some(leave) = left(candidates.clone()).await else { return 0 };
    let losses = confirmed_losses(&leave, &candidates, &me);
    let retired = apply_losses(client, &losses, events).await;
    if retired > 0 {
        crate::enqueue_rooms(events, client).await;
    }
    retired
}

/// One pass against the live server.
pub(crate) async fn reconcile_once(client: &Client, events: &EventQueueRef) -> usize {
    let asker = client.clone();
    reconcile_with(
        client,
        events,
        || fetch_server_joined(client),
        move |rooms| async move { fetch_left(&asker, &rooms).await },
    )
    .await
}

fn listed_locally(client: &Client) -> usize {
    client
        .rooms()
        .iter()
        .filter(|room| {
            matches!(room.state(), RoomState::Joined | RoomState::Invited | RoomState::Knocked)
        })
        .count()
}

fn current_service(slot: &Mutex<Option<Arc<RoomListService>>>) -> Option<Arc<RoomListService>> {
    slot.lock().ok().and_then(|guard| guard.clone())
}

/// Runs for the life of a sync session (the caller drops it with the sync).
/// Never returns. Idle while no sliding sync room list exists (classic sync
/// delivers `rooms.leave` itself).
pub(crate) async fn run_membership_reconciler(
    client: Client,
    events: EventQueueRef,
    room_list_slot: Arc<Mutex<Option<Arc<RoomListService>>>>,
) {
    let mut trigger = Trigger::default();
    let mut last_pass: Option<tokio::time::Instant> = None;
    loop {
        let Some(service) = current_service(&room_list_slot) else {
            tokio::time::sleep(SERVICE_POLL).await;
            continue;
        };
        let Ok(list) = service.all_rooms().await else {
            tokio::time::sleep(SERVICE_POLL).await;
            continue;
        };
        let mut loading = list.loading_state();
        loop {
            // A rebuilt service ends this list's stream only when the old one
            // is dropped, so look for a replacement while waiting.
            let next = tokio::select! {
                next = loading.next() => next,
                _ = tokio::time::sleep(SERVICE_POLL) => {
                    match current_service(&room_list_slot) {
                        Some(now) if Arc::ptr_eq(&now, &service) => continue,
                        _ => break,
                    }
                }
            };
            let Some(state) = next else { break };
            let RoomListLoadingState::Loaded { maximum_number_of_rooms: Some(_) } = state else {
                continue;
            };
            tokio::time::sleep(SETTLE).await;
            // The newest count after settling, not the one that woke us.
            let RoomListLoadingState::Loaded { maximum_number_of_rooms: Some(count) } =
                loading.get()
            else {
                continue;
            };
            if !trigger.observe(count, listed_locally(&client)) {
                continue;
            }
            if let Some(at) = last_pass {
                let since = at.elapsed();
                if since < MIN_PASS_GAP {
                    tokio::time::sleep(MIN_PASS_GAP - since).await;
                }
            }
            last_pass = Some(tokio::time::Instant::now());
            reconcile_once(&client, &events).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::api::client::sync::sync_events::v5;
    use matrix_sdk::ruma::{room_id, user_id, RoomId};
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk_base::RequestedRequiredStates;
    use serde_json::json;
    use std::collections::VecDeque;

    /// Puts `rooms` into the store as joined, the way a sliding sync response
    /// naming a room without `invite_state` does.
    async fn joined(client: &Client, rooms: &[&RoomId]) {
        let mut response = v5::Response::new("pos-1".to_owned());
        for room in rooms {
            response.rooms.insert((*room).to_owned(), v5::response::Room::default());
        }
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        for room in rooms {
            assert_eq!(client.get_room(room).unwrap().state(), RoomState::Joined);
        }
    }

    fn member(state_key: &str, sender: &str, membership: &str) -> serde_json::Value {
        json!({
            "type": "m.room.member", "state_key": state_key, "sender": sender,
            "event_id": format!("$m-{membership}"),
            "origin_server_ts": 1, "content": { "membership": membership }
        })
    }

    fn left_room(timeline: Vec<serde_json::Value>, state: Vec<serde_json::Value>) -> v3::LeftRoom {
        serde_json::from_value(json!({
            "timeline": { "events": timeline },
            "state": { "events": state },
        }))
        .expect("left room")
    }

    fn ids(rooms: &[&RoomId]) -> HashSet<OwnedRoomId> {
        rooms.iter().map(|room| (*room).to_owned()).collect()
    }

    fn queue() -> EventQueueRef {
        Arc::new(Mutex::new(VecDeque::new()))
    }

    /// The `room_membership_lost` events, by room.
    fn lost(events: &EventQueueRef) -> BTreeMap<String, String> {
        events
            .lock()
            .unwrap()
            .drain(..)
            .filter_map(|line| serde_json::from_str::<serde_json::Value>(&line).ok())
            .filter(|value| value["type"] == "room_membership_lost")
            .map(|value| {
                (value["room_id"].as_str().unwrap().to_owned(),
                 value["loss"].as_str().unwrap().to_owned())
            })
            .collect()
    }

    #[test]
    fn the_count_triggers_on_first_load_on_a_fall_and_on_a_surplus() {
        let mut trigger = Trigger::default();
        assert!(trigger.observe(5, 5), "first load: a restart after a leave");
        assert!(!trigger.observe(5, 5), "nothing moved");
        assert!(!trigger.observe(6, 6), "a join is not a loss");
        assert!(trigger.observe(5, 6), "the measured shape of a leave: count falls");
        assert!(!trigger.observe(5, 5));
        assert!(trigger.observe(6, 7), "a leave and a join in one window: surplus");
        assert!(!trigger.observe(6, 5), "the server knowing more is not our loss");
    }

    #[test]
    fn only_rooms_missing_from_joined_rooms_are_candidates() {
        let a: OwnedRoomId = room_id!("!a:example.org").to_owned();
        let b: OwnedRoomId = room_id!("!b:example.org").to_owned();
        let server: HashSet<OwnedRoomId> = [a.clone()].into_iter().collect();
        assert_eq!(candidates([&a, &b], &server), vec![b.clone()]);
        assert!(candidates([&a], &server).is_empty());
    }

    #[test]
    fn the_newest_own_membership_says_how_the_room_was_lost() {
        let me = user_id!("@me:example.org");
        // Our own leave from another session.
        let left = left_room(vec![member("@me:example.org", "@me:example.org", "leave")], vec![]);
        assert_eq!(loss_from_left_room(&left, me), Loss::Left);
        // A leave someone else sent is a kick.
        let kicked = left_room(vec![member("@me:example.org", "@mod:example.org", "leave")], vec![]);
        assert_eq!(loss_from_left_room(&kicked, me), Loss::Kicked);
        // A ban, read from state when the timeline has none of ours.
        let banned = left_room(
            vec![member("@other:example.org", "@other:example.org", "join")],
            vec![member("@me:example.org", "@mod:example.org", "ban")],
        );
        assert_eq!(loss_from_left_room(&banned, me), Loss::Banned);
        // The timeline is newer than the state.
        let rejoined_then_kicked = left_room(
            vec![member("@me:example.org", "@mod:example.org", "leave")],
            vec![member("@me:example.org", "@me:example.org", "join")],
        );
        assert_eq!(loss_from_left_room(&rejoined_then_kicked, me), Loss::Kicked);
        // Someone else's membership is not ours.
        let other = left_room(vec![member("@other:example.org", "@mod:example.org", "ban")], vec![]);
        assert_eq!(loss_from_left_room(&other, me), Loss::Gone);
        // Listed under rooms.leave with nothing readable: still out.
        assert_eq!(loss_from_left_room(&left_room(vec![], vec![]), me), Loss::Gone);
    }

    #[test]
    fn the_confirmation_asks_only_about_the_candidates_and_never_sets_presence() {
        let rooms = vec![room_id!("!a:example.org").to_owned()];
        let filter = serde_json::to_value(confirm_filter(&rooms)).unwrap();
        assert_eq!(filter["room"]["include_leave"], json!(true));
        assert_eq!(filter["room"]["rooms"], json!(["!a:example.org"]));
        assert_eq!(filter["room"]["timeline"]["limit"], json!(1));
        assert_eq!(filter["room"]["state"]["lazy_load_members"], json!(true));
        let request = confirm_request(&rooms);
        assert_eq!(request.set_presence, PresenceState::Offline);
        assert_eq!(request.timeout, Some(Duration::ZERO));
    }

    // A v3 /sync with no `since` deletes the device's pending to-device
    // messages on Continuwuity (room keys, MatrixRTC keys, verification). The
    // confirmation must always carry one, and it must be the token measured
    // to acknowledge nothing.
    #[test]
    fn the_confirmation_never_goes_out_without_a_since() {
        let rooms = vec![room_id!("!a:example.org").to_owned()];
        let request = confirm_request(&rooms);
        assert!(request.since.is_some(), "a since-less v3 /sync consumes to-device messages");
        assert_eq!(request.since.as_deref(), Some("0"));
        // And on the wire, not just in the struct.
        use matrix_sdk::ruma::api::{auth_scheme::SendAccessToken, OutgoingRequest, SupportedVersions};
        let versions = SupportedVersions::from_parts(&["v1.11".to_owned()], &BTreeMap::new());
        let http = request
            .try_into_http_request::<Vec<u8>>(
                "https://hs.example",
                SendAccessToken::IfRequired("token"),
                std::borrow::Cow::Owned(versions),
            )
            .expect("request serializes");
        let query = http.uri().query().unwrap_or_default().to_owned();
        assert!(
            query.split('&').any(|pair| pair == "since=0"),
            "the confirmation's query must carry since=0: {query}"
        );
    }

    // Section 4 F3 / B034. The server drops a room it no longer has us in
    // from the list and never names it again; only its own answer retires a
    // room, and only the rooms it confirms.
    #[tokio::test]
    async fn a_room_left_elsewhere_is_retired_only_on_the_servers_word() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let me = client.user_id().unwrap().to_string();
        let still = room_id!("!still-joined:example.org");
        let left = room_id!("!left-elsewhere:example.org");
        let kicked = room_id!("!kicked:example.org");
        let banned = room_id!("!banned:example.org");
        let unmentioned = room_id!("!not-in-the-answer:example.org");
        joined(&client, &[still, left, kicked, banned, unmentioned]).await;

        let asked = Arc::new(Mutex::new(Vec::<OwnedRoomId>::new()));
        let record = Arc::clone(&asked);
        let answer: BTreeMap<OwnedRoomId, v3::LeftRoom> = [
            (left.to_owned(), left_room(vec![member(&me, &me, "leave")], vec![])),
            (kicked.to_owned(), left_room(vec![member(&me, "@mod:example.org", "leave")], vec![])),
            (banned.to_owned(), left_room(vec![member(&me, "@mod:example.org", "ban")], vec![])),
            // Not asked about: must not be touched even if listed.
            (still.to_owned(), left_room(vec![member(&me, &me, "leave")], vec![])),
        ]
        .into_iter()
        .collect();

        let events = queue();
        let retired = reconcile_with(
            &client,
            &events,
            || async { Some(ids(&[still])) },
            move |rooms| async move {
                *record.lock().unwrap() = rooms;
                Some(answer)
            },
        )
        .await;

        assert_eq!(retired, 3);
        let mut expected_asked = vec![
            banned.to_owned(), kicked.to_owned(), left.to_owned(), unmentioned.to_owned(),
        ];
        expected_asked.sort();
        assert_eq!(*asked.lock().unwrap(), expected_asked,
                   "only the rooms missing from /joined_rooms are asked about");
        assert_eq!(client.get_room(left).unwrap().state(), RoomState::Left);
        assert_eq!(client.get_room(kicked).unwrap().state(), RoomState::Left);
        assert_eq!(client.get_room(banned).unwrap().state(), RoomState::Banned);
        assert_eq!(client.get_room(still).unwrap().state(), RoomState::Joined);
        assert_eq!(client.get_room(unmentioned).unwrap().state(), RoomState::Joined,
                   "absent from /joined_rooms alone is not a leave");

        let lost = lost(&events);
        assert_eq!(lost.len(), 3);
        assert_eq!(lost[left.as_str()], "left");
        assert_eq!(lost[kicked.as_str()], "kicked");
        assert_eq!(lost[banned.as_str()], "banned");

        // The room list drops them: the store no longer lists them.
        let listed: HashSet<OwnedRoomId> = client
            .rooms()
            .iter()
            .filter(|room| room.state() == RoomState::Joined)
            .map(|room| room.room_id().to_owned())
            .collect();
        assert_eq!(listed, ids(&[still, unmentioned]));
    }

    // Nothing missing from /joined_rooms: the confirmation is never asked.
    #[tokio::test]
    async fn a_server_that_agrees_costs_one_question() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = room_id!("!joined:example.org");
        joined(&client, &[room]).await;
        let events = queue();
        let retired = reconcile_with(
            &client,
            &events,
            || async { Some(ids(&[room])) },
            |_| async { panic!("the confirmation must not be asked") },
        )
        .await;
        assert_eq!(retired, 0);
    }

    // With no answer nothing is touched: a failed /joined_rooms or /sync is
    // never read as "gone". The live fetches fail closed against the mock
    // (unmocked endpoints answer 404).
    #[tokio::test]
    async fn no_answer_retires_nothing() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = room_id!("!joined:example.org");
        joined(&client, &[room]).await;
        let events = queue();
        assert_eq!(reconcile_once(&client, &events).await, 0);
        assert_eq!(client.get_room(room).unwrap().state(), RoomState::Joined);

        let retired = reconcile_with(
            &client,
            &events,
            || async { Some(HashSet::new()) },
            |_| async { None },
        )
        .await;
        assert_eq!(retired, 0);
        assert_eq!(client.get_room(room).unwrap().state(), RoomState::Joined);
        assert!(lost(&events).is_empty());
    }

    // A sync that moved the room meanwhile wins over a stale answer.
    #[tokio::test]
    async fn only_a_joined_room_is_retired() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room_id = room_id!("!r:example.org");
        joined(&client, &[room_id]).await;
        let room = client.get_room(room_id).unwrap();
        assert!(retire_locally(&room, Loss::Kicked).await);
        assert_eq!(room.state(), RoomState::Left);
        assert!(!retire_locally(&room, Loss::Banned).await, "already left: unchanged");
        assert_eq!(room.state(), RoomState::Left);
    }

    /// End to end against a real homeserver: a sliding sync room list, the
    /// reconciler as the sync thread runs it, and three rooms lost three ways
    /// (our own leave from a second device, a kick, a ban) plus one kept.
    /// Two to-device messages are pending for the member's device while the
    /// passes ask (nothing consumes to-device until the end), and the SDK must
    /// still receive both afterwards: a since-less confirmation deletes them
    /// on Continuwuity.
    /// Runs only with `--ignored` and LIGHTNING_LIVE_MEMBERSHIP=1, with
    /// LIGHTNING_TEST_HOMESERVER, LIGHTNING_TEST_USER / _PASSWORD (the member)
    /// and LIGHTNING_TEST_PEER_USER / _PASSWORD (the room creator). Use test
    /// accounts on a test server only (scratch/localhs).
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    #[ignore = "live homeserver; set LIGHTNING_LIVE_MEMBERSHIP=1 and credentials env"]
    async fn live_rooms_lost_elsewhere_leave_the_store() {
        use matrix_sdk::ruma::api::client::{
            membership::{ban_user, kick_user, leave_room},
            room::create_room,
            to_device::send_event_to_device,
        };
        use matrix_sdk::ruma::events::AnyToDeviceEvent;
        use matrix_sdk::ruma::to_device::DeviceIdOrAllDevices;
        use matrix_sdk::ruma::TransactionId;
        use matrix_sdk_ui::sync_service::SyncService;
        const PROBE: &str = "org.lightning.test.membership_probe";
        fn env(name: &str) -> String {
            std::env::var(name).ok().filter(|v| !v.trim().is_empty()).expect(name)
        }
        if std::env::var("LIGHTNING_LIVE_MEMBERSHIP").as_deref() != Ok("1") {
            return;
        }
        let homeserver = env("LIGHTNING_TEST_HOMESERVER");
        let login = |user: String, password: String| {
            let homeserver = homeserver.clone();
            async move {
                let client = Client::builder().homeserver_url(&homeserver).build().await.unwrap();
                client.matrix_auth().login_username(&user, &password).send().await.unwrap();
                client
            }
        };
        let member = login(env("LIGHTNING_TEST_USER"), env("LIGHTNING_TEST_PASSWORD")).await;
        let other_device =
            login(env("LIGHTNING_TEST_USER"), env("LIGHTNING_TEST_PASSWORD")).await;
        let peer = login(env("LIGHTNING_TEST_PEER_USER"), env("LIGHTNING_TEST_PEER_PASSWORD")).await;
        let me = member.user_id().unwrap().to_owned();

        let mut rooms = Vec::new();
        for _ in 0..4 {
            let mut request = create_room::v3::Request::new();
            request.invite = vec![me.clone()];
            let room_id = peer.send(request).await.unwrap().room_id;
            member.join_room_by_id(&room_id).await.unwrap();
            rooms.push(room_id);
        }
        let (kept, left, kicked, banned) = (&rooms[0], &rooms[1], &rooms[2], &rooms[3]);
        // A to-device message for the member's own device, left pending on
        // the server: nothing below consumes to-device until the very end.
        let member_device = member.device_id().unwrap().to_owned();
        let send_probe = |n: u64| {
            let peer = peer.clone();
            let me = me.clone();
            let device = member_device.clone();
            async move {
                let content = Raw::new(&json!({ "n": n })).unwrap().cast_unchecked();
                let messages = [(me, [(DeviceIdOrAllDevices::DeviceId(device), content)]
                    .into_iter()
                    .collect())]
                .into_iter()
                .collect();
                peer.send(send_event_to_device::v3::Request::new_raw(
                    PROBE.into(), TransactionId::new(), messages,
                ))
                .await
                .unwrap();
            }
        };
        send_probe(1).await;
        // Left from the other device BEFORE the list starts: the restart case,
        // which only the first-load pass can catch.
        other_device.send(leave_room::v3::Request::new(left.clone())).await.unwrap();

        let service = Arc::new(RoomListService::new(member.clone()).await.unwrap());
        let sync_service = Arc::clone(&service);
        let sync = tokio::spawn(async move {
            let stream = sync_service.sync();
            futures_util::pin_mut!(stream);
            while futures_util::StreamExt::next(&mut stream).await.is_some() {}
        });
        let slot = Arc::new(Mutex::new(Some(Arc::clone(&service))));
        let events = queue();
        let reconciler = tokio::spawn(run_membership_reconciler(
            member.clone(), Arc::clone(&events), slot,
        ));
        // The first-load pass retires the room left while nothing was running,
        // and only that one.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(20);
        while member.get_room(left).unwrap().state() != RoomState::Left {
            assert!(tokio::time::Instant::now() < deadline, "first-load pass did not run");
            tokio::time::sleep(Duration::from_millis(250)).await;
        }
        for room in [kept, kicked, banned] {
            assert_eq!(member.get_room(room).unwrap().state(), RoomState::Joined);
        }

        // Pending again for the pass the kick and ban set off.
        send_probe(2).await;
        peer.send(kick_user::v3::Request::new(kicked.clone(), me.clone())).await.unwrap();
        peer.send(ban_user::v3::Request::new(banned.clone(), me.clone())).await.unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(90);
        loop {
            let states: Vec<RoomState> =
                rooms.iter().map(|room| member.get_room(room).unwrap().state()).collect();
            if states == [RoomState::Joined, RoomState::Left, RoomState::Left, RoomState::Banned] {
                break;
            }
            assert!(tokio::time::Instant::now() < deadline, "not reconciled: {states:?}");
            tokio::time::sleep(Duration::from_millis(500)).await;
        }
        let lost = lost(&events);
        assert_eq!(lost.get(left.as_str()).map(String::as_str), Some("left"));
        assert_eq!(lost.get(kicked.as_str()).map(String::as_str), Some("kicked"));
        assert_eq!(lost.get(banned.as_str()).map(String::as_str), Some("banned"));
        assert!(!lost.contains_key(kept.as_str()));
        reconciler.abort();
        sync.abort();

        // Both passes asked while the probes were pending; the SDK's own
        // to-device sync must still receive both.
        let seen = Arc::new(Mutex::new(HashSet::<u64>::new()));
        let record = Arc::clone(&seen);
        member.add_event_handler(move |event: Raw<AnyToDeviceEvent>| {
            let record = Arc::clone(&record);
            async move {
                let value: serde_json::Value = event.deserialize_as_unchecked().unwrap();
                if value["type"] == PROBE {
                    if let Some(n) = value["content"]["n"].as_u64() {
                        record.lock().unwrap().insert(n);
                    }
                }
            }
        });
        let sync_service = SyncService::builder(member.clone()).build().await.unwrap();
        sync_service.start().await;
        let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
        loop {
            let got = seen.lock().unwrap().clone();
            if got == HashSet::from([1, 2]) {
                break;
            }
            assert!(
                tokio::time::Instant::now() < deadline,
                "to-device messages pending during a pass were lost: received {got:?} of [1, 2]"
            );
            tokio::time::sleep(Duration::from_millis(250)).await;
        }
        sync_service.stop().await;
    }
}
