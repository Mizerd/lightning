
use std::collections::{HashMap, HashSet};
use std::sync::{Mutex, OnceLock};

use matrix_sdk::deserialized_responses::TimelineEvent;
use matrix_sdk::ruma::{
    api::client::filter::RoomEventFilter, EventId, OwnedEventId, OwnedRoomId, OwnedUserId, RoomId,
    UserId,
};

pub(crate) const BACKFILL_LIMIT: u32 = 5;
pub(crate) const BACKFILL_IN_FLIGHT: usize = 4;
pub(crate) const BACKFILL_ATTEMPTS: u8 = 3;
/// Spacing between looks at a room whose newest message could not be decrypted:
/// a key may have arrived, and `Room::messages` does not put events into the
/// linked chunk, so `latest_event` will never learn it on its own.
pub(crate) const UNDECRYPTABLE_RELOOK_MS: u64 = 60_000;
pub(crate) const UNDECRYPTABLE_RELOOKS: u8 = 5;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Outcome {
    Found(String, u64),
    Undecryptable(u64, Option<OwnedEventId>),
    Nothing,
    Failed,
}

pub(crate) struct Backfill {
    /// The origin of the millisecond clock every due time is measured on. Owned
    /// here, with the due times, so a rebuilt sync session (the loop around the
    /// looks) can never restart the clock under times that outlive it.
    started: std::time::Instant,
    done: HashSet<OwnedRoomId>,
    in_flight: HashSet<OwnedRoomId>,
    failures: HashMap<OwnedRoomId, u8>,
    relook: HashMap<OwnedRoomId, (u64, u8)>,
}

impl Default for Backfill {
    fn default() -> Self {
        Self {
            started: std::time::Instant::now(),
            done: HashSet::new(),
            in_flight: HashSet::new(),
            failures: HashMap::new(),
            relook: HashMap::new(),
        }
    }
}

impl Backfill {
    pub(crate) fn now_ms(&self) -> u64 {
        self.started.elapsed().as_millis() as u64
    }

    pub(crate) fn pick<I: IntoIterator<Item = OwnedRoomId>>(
        &mut self,
        candidates: I,
        running: usize,
        now_ms: u64,
    ) -> Vec<OwnedRoomId> {
        let mut chosen = Vec::new();
        for room in candidates {
            if running + chosen.len() >= BACKFILL_IN_FLIGHT {
                break;
            }
            if !self.is_eligible(&room, now_ms) {
                continue;
            }
            self.in_flight.insert(room.clone());
            chosen.push(room);
        }
        chosen
    }

    pub(crate) fn finish(&mut self, room: &OwnedRoomId, outcome: &Outcome, now_ms: u64) {
        self.in_flight.remove(room);
        match outcome {
            Outcome::Failed => {
                *self.failures.entry(room.clone()).or_insert(0) += 1;
            }
            Outcome::Undecryptable(..) => {
                let looks = self.relook.get(room).map_or(0, |(_, n)| *n) + 1;
                if looks >= UNDECRYPTABLE_RELOOKS {
                    self.relook.remove(room);
                    self.done.insert(room.clone());
                } else {
                    self.relook.insert(room.clone(), (now_ms + UNDECRYPTABLE_RELOOK_MS, looks));
                }
            }
            Outcome::Found(..) | Outcome::Nothing => {
                self.relook.remove(room);
                self.done.insert(room.clone());
            }
        }
    }

    pub(crate) fn clear_in_flight(&mut self) {
        self.in_flight.clear();
    }

    pub(crate) fn is_done(&self, room: &OwnedRoomId) -> bool {
        self.done.contains(room)
    }

    /// Makes a room lookable again, as if it had never been looked at.
    pub(crate) fn forget(&mut self, room: &RoomId) {
        self.done.remove(room);
        self.failures.remove(room);
        self.relook.remove(room);
    }

    pub(crate) fn is_eligible(&self, room: &OwnedRoomId, now_ms: u64) -> bool {
        !self.done.contains(room)
            && !self.in_flight.contains(room)
            && self.failures.get(room).copied().unwrap_or(0) < BACKFILL_ATTEMPTS
            && self.relook.get(room).map_or(true, |(due, _)| now_ms >= *due)
    }
}

pub(crate) fn pick_preview(newest_first: &[TimelineEvent]) -> Option<Outcome> {
    let (event, ts) = newest_first
        .iter()
        .find_map(|event| Some((event, crate::event_ordering_stamp_ms(event)?)))?;
    let raw = event.raw();
    if raw.get_field::<String>("type").ok().flatten().as_deref() == Some("m.room.encrypted") {
        return Some(Outcome::Undecryptable(ts, event.event_id()));
    }
    let text = crate::raw_event_preview_line(raw);
    if text.is_empty() { None } else { Some(Outcome::Found(text, ts)) }
}

pub(crate) fn conversation_filter() -> RoomEventFilter {
    let mut filter = RoomEventFilter::default();
    filter.types = Some(
        crate::CONVERSATION_EVENT_TYPES.iter().map(|kind| (*kind).to_owned()).collect(),
    );
    filter
}

pub(crate) async fn look(room: &matrix_sdk::Room) -> Outcome {
    use matrix_sdk::room::MessagesOptions;
    let mut options = MessagesOptions::backward();
    options.limit = BACKFILL_LIMIT.into();
    options.filter = conversation_filter();
    let response = tokio::time::timeout(
        std::time::Duration::from_secs(20),
        room.messages(options),
    )
    .await;
    let Ok(Ok(messages)) = response else {
        return Outcome::Failed;
    };
    pick_preview(&messages.chunk).unwrap_or(Outcome::Nothing)
}

/// Whether the SDK's latest event is our own join. matrix-sdk picks it on
/// purpose so a room just joined has a first value (its latest-event builder
/// accepts only the OWN Joined / InvitationAccepted / KnockAccepted change of
/// an `m.room.member`, never a profile change or someone else's join). It has
/// no message to preview, so `room_payload` sends a flag and C++ words it in
/// the UI language; before, a DM just accepted showed a blank line.
pub(crate) fn is_own_join(
    raw: &matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnySyncTimelineEvent>,
    own_user_id: &UserId,
) -> bool {
    raw.get_field::<String>("type").ok().flatten().as_deref() == Some("m.room.member")
        && raw.get_field::<String>("state_key").ok().flatten().as_deref()
            == Some(own_user_id.as_str())
        && raw
            .get_field::<serde_json::Value>("content")
            .ok()
            .flatten()
            .and_then(|content| content.get("membership").and_then(|m| m.as_str()).map(str::to_owned))
            .as_deref()
            == Some("join")
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Remembered {
    Message(String, u64),
    Undecryptable(u64, Option<OwnedEventId>),
}

type Key = (OwnedUserId, OwnedRoomId);

struct State {
    generation: u64,
    current: HashMap<OwnedUserId, u64>,
    entries: HashMap<Key, Remembered>,
}

fn state() -> &'static Mutex<State> {
    static STATE: OnceLock<Mutex<State>> = OnceLock::new();
    STATE.get_or_init(|| {
        Mutex::new(State { generation: 0, current: HashMap::new(), entries: HashMap::new() })
    })
}

pub(crate) struct RunGuard {
    user: OwnedUserId,
    generation: u64,
}

impl RunGuard {
    pub(crate) fn new(user: OwnedUserId) -> Self {
        let mut st = state().lock().unwrap_or_else(|e| e.into_inner());
        st.generation += 1;
        let generation = st.generation;
        st.current.insert(user.clone(), generation);
        st.entries.retain(|(owner, _), _| owner != &user);
        Self { user, generation }
    }

    pub(crate) fn remember(&self, room: &RoomId, what: Remembered) {
        let mut st = state().lock().unwrap_or_else(|e| e.into_inner());
        if st.current.get(&self.user) == Some(&self.generation) {
            st.entries.insert((self.user.clone(), room.to_owned()), what);
        }
    }
}

impl Drop for RunGuard {
    fn drop(&mut self) {
        let mut st = state().lock().unwrap_or_else(|e| e.into_inner());
        if st.current.get(&self.user) == Some(&self.generation) {
            st.current.remove(&self.user);
            st.entries.retain(|(owner, _), _| owner != &self.user);
        }
    }
}

pub(crate) fn forget_user(user: &UserId) {
    let mut st = state().lock().unwrap_or_else(|e| e.into_inner());
    st.current.remove(user);
    st.entries.retain(|(owner, _), _| owner != user);
}

pub(crate) fn remembered(user: &UserId, room: &RoomId) -> Option<Remembered> {
    let st = state().lock().unwrap_or_else(|e| e.into_inner());
    st.entries.get(&(user.to_owned(), room.to_owned())).cloned()
}

pub(crate) fn replace_remembered(
    user: &UserId,
    room: &RoomId,
    was: &Remembered,
    now: Option<Remembered>,
) {
    let mut st = state().lock().unwrap_or_else(|e| e.into_inner());
    let key = (user.to_owned(), room.to_owned());
    if st.entries.get(&key) != Some(was) {
        return;
    }
    match now {
        Some(what) => {
            st.entries.insert(key, what);
        }
        None => {
            st.entries.remove(&key);
        }
    }
}

/// Users added to or removed from the ignore list between two of its values.
pub(crate) fn ignore_list_delta(before: &[String], after: &[String]) -> Vec<String> {
    let mut changed: Vec<String> = before
        .iter()
        .filter(|user| !after.contains(user))
        .chain(after.iter().filter(|user| !before.contains(user)))
        .cloned()
        .collect();
    changed.sort();
    changed.dedup();
    changed
}

/// matrix-sdk empties every room's event cache on an ignore-list change, so
/// `latest_event` goes blank everywhere: a room no changed user is a member of,
/// or wrote the preview of, keeps the preview it had instead of fetching it.
pub(crate) fn across_ignore_change(
    preview: &str,
    ts: u64,
    changed_user_involved: bool,
) -> Option<Remembered> {
    (!changed_user_involved && !preview.is_empty())
        .then(|| Remembered::Message(preview.to_owned(), ts))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum UndecryptableNow {
    Unknown,
    StillUndecryptable,
    Decrypted(String),
}

pub(crate) fn settle(
    remembered: Option<Remembered>,
    latest_event_id: Option<&EventId>,
    now: UndecryptableNow,
) -> Option<Remembered> {
    match remembered {
        Some(Remembered::Undecryptable(ts, Some(id))) => {
            if latest_event_id == Some(&*id) {
                return None;
            }
            match now {
                UndecryptableNow::Decrypted(text) if text.is_empty() => None,
                UndecryptableNow::Decrypted(text) => Some(Remembered::Message(text, ts)),
                UndecryptableNow::Unknown | UndecryptableNow::StillUndecryptable => {
                    Some(Remembered::Undecryptable(ts, Some(id)))
                }
            }
        }
        other => other,
    }
}

pub(crate) fn merge_remembered(
    own_preview: String,
    own_activity: u64,
    remembered: Option<Remembered>,
) -> (String, u64, bool) {
    match remembered {
        Some(Remembered::Message(preview, ts)) if own_preview.is_empty() => {
            (preview, own_activity.max(ts), false)
        }
        Some(Remembered::Undecryptable(ts, _)) if own_preview.is_empty() => {
            (own_preview, own_activity.max(ts), true)
        }
        Some(Remembered::Undecryptable(ts, _)) if ts > own_activity => (String::new(), ts, true),
        _ => (own_preview, own_activity, false),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::room_id;
    use matrix_sdk::ruma::serde::Raw;
    use matrix_sdk::test_utils::mocks::{MatrixMockServer, RoomMessagesResponseTemplate};
    use serde_json::json;

    fn raw(value: serde_json::Value) -> TimelineEvent {
        TimelineEvent::from_plaintext(serde_json::from_value(value).unwrap())
    }

    fn message(ts: u64, body: &str) -> serde_json::Value {
        json!({
            "type": "m.room.message", "event_id": format!("$m{ts}:x"),
            "room_id": "!a:example.org",
            "sender": "@a:x", "origin_server_ts": ts,
            "content": { "msgtype": "m.text", "body": body }
        })
    }

    fn churn(ts: u64) -> serde_json::Value {
        json!({
            "type": "org.matrix.msc3401.call.member", "state_key": format!("_@a:x_d{ts}"),
            "event_id": format!("$c{ts}:x"), "room_id": "!a:example.org",
            "sender": "@a:x", "origin_server_ts": ts, "content": {}
        })
    }

    fn rooms(n: usize) -> Vec<OwnedRoomId> {
        (0..n).map(|i| OwnedRoomId::try_from(format!("!r{i}:x")).unwrap()).collect()
    }

    #[test]
    fn only_our_own_join_is_an_own_join() {
        use matrix_sdk::ruma::user_id;
        let me = user_id!("@me:example.org");
        let member = |state_key: &str, membership: &str| {
            raw(json!({
                "type": "m.room.member", "state_key": state_key,
                "event_id": "$j:x", "room_id": "!a:example.org",
                "sender": state_key, "origin_server_ts": 5,
                "content": { "membership": membership }
            }))
        };
        assert!(is_own_join(member("@me:example.org", "join").raw(), me));
        assert!(!is_own_join(member("@other:example.org", "join").raw(), me));
        assert!(!is_own_join(member("@me:example.org", "leave").raw(), me));
        assert!(!is_own_join(member("@me:example.org", "invite").raw(), me));
        assert!(!is_own_join(raw(message(5, "hi")).raw(), me));
    }

    #[test]
    fn at_most_four_looks_are_in_flight_not_four_per_pass() {
        let mut backfill = Backfill::default();
        let all = rooms(10);
        let first = backfill.pick(all.clone(), 0, 0);
        assert_eq!(first, all[..4].to_vec());
        assert!(backfill.pick(all.clone(), 4, 0).is_empty());
        backfill.finish(&all[0], &Outcome::Nothing, 0);
        backfill.finish(&all[1], &Outcome::Found("x".into(), 1), 0);
        assert_eq!(backfill.pick(all.clone(), 2, 0), all[4..6].to_vec());
    }

    #[test]
    fn only_a_completed_look_retires_a_room_and_a_failed_one_retries_a_bounded_number_of_times() {
        let mut backfill = Backfill::default();
        let room = rooms(1).remove(0);
        for attempt in 0..BACKFILL_ATTEMPTS {
            assert_eq!(backfill.pick([room.clone()], 0, 0), vec![room.clone()], "attempt {attempt}");
            assert!(backfill.pick([room.clone()], 0, 0).is_empty());
            backfill.finish(&room, &Outcome::Failed, 0);
            assert!(!backfill.is_done(&room));
        }
        assert!(backfill.pick([room.clone()], 0, 0).is_empty(), "failures are bounded");

        let other = rooms(2).remove(1);
        backfill.pick([other.clone()], 0, 0);
        backfill.finish(&other, &Outcome::Nothing, 0);
        assert!(backfill.is_done(&other));
        assert!(!backfill.is_eligible(&other, 1_000_000));
    }

    #[test]
    fn an_undecryptable_room_is_looked_at_again_after_the_interval_and_not_for_ever() {
        let mut backfill = Backfill::default();
        let room = rooms(1).remove(0);
        let mut now = 1_000;
        for look in 1..=UNDECRYPTABLE_RELOOKS {
            assert_eq!(backfill.pick([room.clone()], 0, now), vec![room.clone()], "look {look}");
            backfill.finish(&room, &Outcome::Undecryptable(5, None), now);
            if look < UNDECRYPTABLE_RELOOKS {
                assert!(!backfill.is_done(&room));
                assert!(backfill.pick([room.clone()], 0, now + UNDECRYPTABLE_RELOOK_MS - 1).is_empty());
                now += UNDECRYPTABLE_RELOOK_MS;
            }
        }
        assert!(backfill.is_done(&room), "the placeholder is left after the bound");
        assert!(backfill.pick([room.clone()], 0, now + 10 * UNDECRYPTABLE_RELOOK_MS).is_empty());
    }

    #[test]
    fn a_due_time_survives_a_sync_session_rebuild() {
        // The loop around the looks rebuilds on every sync-session restart; the
        // Backfill (and its clock) lives outside it, so a due time set before
        // the rebuild is still in the future after it.
        let mut backfill = Backfill::default();
        let room = rooms(1).remove(0);
        backfill.pick([room.clone()], 0, backfill.now_ms());
        let before = backfill.now_ms();
        backfill.finish(&room, &Outcome::Undecryptable(5, None), before);
        let after = backfill.now_ms();
        assert!(after >= before, "the clock never restarts");
        assert!(!backfill.is_eligible(&room, after), "still waiting out its interval");
        assert!(backfill.is_eligible(&room, before + UNDECRYPTABLE_RELOOK_MS));
    }

    #[test]
    fn a_later_look_that_finds_the_message_retires_the_room() {
        let mut backfill = Backfill::default();
        let room = rooms(1).remove(0);
        backfill.pick([room.clone()], 0, 0);
        backfill.finish(&room, &Outcome::Undecryptable(5, None), 0);
        backfill.pick([room.clone()], 0, UNDECRYPTABLE_RELOOK_MS);
        backfill.finish(&room, &Outcome::Found("now readable".into(), 5), UNDECRYPTABLE_RELOOK_MS);
        assert!(backfill.is_done(&room));
    }

    #[test]
    fn the_preview_is_the_first_conversation_event_not_the_churn_above_it() {
        let page = vec![raw(churn(40)), raw(churn(30)), raw(message(20, "last words")),
                        raw(message(10, "older"))];
        assert_eq!(pick_preview(&page), Some(Outcome::Found("last words".to_owned(), 20)));
    }

    #[test]
    fn an_undecryptable_newest_message_is_its_own_outcome_with_no_text_and_hides_older_ones() {
        let utd = raw(json!({
            "type": "m.room.encrypted", "event_id": "$u:x", "sender": "@a:x",
            "origin_server_ts": 30,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x",
                         "session_id": "s", "sender_key": "k", "device_id": "d" }
        }));
        let page = vec![raw(churn(40)), utd, raw(message(20, "older decrypted"))];
        assert_eq!(
            pick_preview(&page),
            Some(Outcome::Undecryptable(30, Some(OwnedEventId::try_from("$u:x").unwrap())))
        );
        let lookalike = vec![raw(message(31, "Unable to decrypt"))];
        assert_eq!(
            pick_preview(&lookalike),
            Some(Outcome::Found("Unable to decrypt".to_owned(), 31))
        );
    }

    #[test]
    fn an_edit_or_state_or_a_page_of_churn_selects_nothing() {
        let edit = raw(json!({
            "type": "m.room.message", "event_id": "$e:x", "sender": "@a:x",
            "origin_server_ts": 50,
            "content": {
                "msgtype": "m.text", "body": "* fixed",
                "m.new_content": { "msgtype": "m.text", "body": "fixed" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$m1:x" }
            }
        }));
        let topic = raw(json!({
            "type": "m.room.topic", "state_key": "", "event_id": "$t:x",
            "sender": "@a:x", "origin_server_ts": 60, "content": { "topic": "t" }
        }));
        assert_eq!(pick_preview(&[edit, topic, raw(churn(1))]), None);
        assert_eq!(pick_preview(&[]), None);
    }

    #[test]
    fn an_ignore_list_change_names_both_who_was_ignored_and_who_was_unignored() {
        let v = |ids: &[&str]| ids.iter().map(|id| (*id).to_owned()).collect::<Vec<_>>();
        assert_eq!(ignore_list_delta(&v(&["@a:hs"]), &v(&["@a:hs", "@b:hs"])), v(&["@b:hs"]));
        assert_eq!(ignore_list_delta(&v(&["@a:hs", "@b:hs"]), &v(&["@b:hs"])), v(&["@a:hs"]));
        assert_eq!(ignore_list_delta(&v(&["@a:hs"]), &v(&["@c:hs"])), v(&["@a:hs", "@c:hs"]));
        assert!(ignore_list_delta(&v(&["@a:hs"]), &v(&["@a:hs"])).is_empty());
    }

    #[test]
    fn across_an_ignore_change_only_a_room_the_user_is_not_in_keeps_its_preview_unfetched() {
        let kept = across_ignore_change("hello", 40, false);
        assert_eq!(kept, Some(Remembered::Message("hello".into(), 40)));
        assert_eq!(merge_remembered(String::new(), 0, kept), ("hello".into(), 40, false),
                   "the blank the cache clear leaves is filled with the old preview and stamp");
        assert_eq!(across_ignore_change("hello", 40, true), None);
        assert_eq!(across_ignore_change("", 40, false), None);

        let mut backfill = Backfill::default();
        let room = rooms(1).remove(0);
        backfill.pick([room.clone()], 0, 0);
        backfill.finish(&room, &Outcome::Found("old".into(), 1), 0);
        assert!(!backfill.is_eligible(&room, 0));
        backfill.forget(&room);
        assert_eq!(backfill.pick([room.clone()], 0, 0), vec![room],
                   "a room the change touches is looked at again");
    }

    #[test]
    fn a_remembered_look_fills_only_a_blank_payload_and_takes_the_later_stamp() {
        let found = Some(Remembered::Message("last words".into(), 20));
        assert_eq!(merge_remembered(String::new(), 0, found.clone()), ("last words".into(), 20, false));
        assert_eq!(merge_remembered(String::new(), 50, found.clone()), ("last words".into(), 50, false));
        assert_eq!(merge_remembered("own".into(), 5, found), ("own".into(), 5, false));
        assert_eq!(merge_remembered(String::new(), 0, None), (String::new(), 0, false));
        let utd = Some(Remembered::Undecryptable(9, None));
        assert_eq!(merge_remembered(String::new(), 3, utd.clone()), (String::new(), 9, true));
        assert_eq!(merge_remembered("own".into(), 3, utd.clone()), (String::new(), 9, true));
        assert_eq!(merge_remembered("own".into(), 9, utd.clone()), ("own".into(), 9, false));
        assert_eq!(merge_remembered("own".into(), 12, utd), ("own".into(), 12, false));
    }

    #[test]
    fn a_remembered_undecryptable_event_is_settled_by_its_id_not_its_stamp() {
        let id = OwnedEventId::try_from("$late:x").unwrap();
        let other = OwnedEventId::try_from("$other:x").unwrap();
        let utd = Some(Remembered::Undecryptable(1_000, Some(id.clone())));

        let settled = settle(utd.clone(), Some(&id), UndecryptableNow::Unknown);
        assert_eq!(settled, None);
        assert_eq!(
            merge_remembered("now readable".into(), 997, settled),
            ("now readable".into(), 997, false)
        );

        for now in [UndecryptableNow::Unknown, UndecryptableNow::StillUndecryptable] {
            let settled = settle(utd.clone(), Some(&other), now);
            assert_eq!(settled, utd);
            assert_eq!(merge_remembered("older".into(), 500, settled), (String::new(), 1_000, true));
        }

        let settled = settle(utd.clone(), Some(&other), UndecryptableNow::Decrypted(String::new()));
        assert_eq!(settled, None);
        assert_eq!(merge_remembered("older".into(), 500, settled), ("older".into(), 500, false));

        let settled = settle(utd.clone(), None, UndecryptableNow::Decrypted("hello".into()));
        assert_eq!(settled, Some(Remembered::Message("hello".into(), 1_000)));
        assert_eq!(merge_remembered(String::new(), 0, settled), ("hello".into(), 1_000, false));

        let anonymous = Some(Remembered::Undecryptable(1_000, None));
        assert_eq!(
            settle(anonymous.clone(), Some(&id), UndecryptableNow::Decrypted("x".into())),
            anonymous
        );
        let message = Some(Remembered::Message("m".into(), 1));
        assert_eq!(settle(message.clone(), Some(&id), UndecryptableNow::Unknown), message);
    }

    #[test]
    fn a_settled_entry_replaces_only_what_it_was_checked_against() {
        let user: OwnedUserId = "@settle:x".try_into().unwrap();
        let room = room_id!("!settle:x");
        let run = RunGuard::new(user.clone());
        let id = OwnedEventId::try_from("$s:x").unwrap();
        let utd = Remembered::Undecryptable(7, Some(id));
        run.remember(room, utd.clone());
        run.remember(room, Remembered::Message("newer".into(), 9));
        replace_remembered(&user, room, &utd, None);
        assert_eq!(remembered(&user, room), Some(Remembered::Message("newer".into(), 9)));
        run.remember(room, utd.clone());
        replace_remembered(&user, room, &utd, Some(Remembered::Message("hello".into(), 7)));
        assert_eq!(remembered(&user, room), Some(Remembered::Message("hello".into(), 7)));
        replace_remembered(&user, room, &Remembered::Message("hello".into(), 7), None);
        assert_eq!(remembered(&user, room), None);
    }

    #[test]
    fn a_detached_older_run_cannot_clear_or_write_into_the_newer_run() {
        let alice: OwnedUserId = "@alice:x".try_into().unwrap();
        let bob: OwnedUserId = "@bob:x".try_into().unwrap();
        let room = room_id!("!shared:x");

        let old = RunGuard::new(alice.clone());
        old.remember(room, Remembered::Message("old secret".into(), 1));
        assert!(remembered(&alice, room).is_some());

        let new = RunGuard::new(alice.clone());
        assert_eq!(remembered(&alice, room), None, "a new run starts empty");
        new.remember(room, Remembered::Message("fresh".into(), 2));

        old.remember(room, Remembered::Message("late old".into(), 3));
        assert_eq!(remembered(&alice, room), Some(Remembered::Message("fresh".into(), 2)));
        drop(old);
        assert_eq!(remembered(&alice, room), Some(Remembered::Message("fresh".into(), 2)));

        assert_eq!(remembered(&bob, room), None);
        let other = RunGuard::new(bob.clone());
        other.remember(room, Remembered::Undecryptable(4, None));
        assert_eq!(remembered(&alice, room), Some(Remembered::Message("fresh".into(), 2)));
        assert_eq!(remembered(&bob, room), Some(Remembered::Undecryptable(4, None)));

        drop(new);
        assert_eq!(remembered(&alice, room), None);
        assert_eq!(remembered(&bob, room), Some(Remembered::Undecryptable(4, None)));
    }

    #[test]
    fn the_filter_asks_only_for_conversation_types() {
        let types = conversation_filter().types.unwrap();
        assert!(types.contains(&"m.room.message".to_owned()));
        assert!(types.contains(&"m.room.encrypted".to_owned()));
        assert!(!types.iter().any(|kind| kind.contains("call")));
    }

    // look() against a mock homeserver, with MORE than ten churn events above the
    // message (the documented repro). The mock returns the whole page the way an
    // unfiltered server would, so this proves the selection skips the churn; the
    // request assertion proves the filter and limit are what a filtering server
    // is asked for.
    #[tokio::test]
    async fn look_finds_the_message_under_more_than_ten_churn_events() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = server.sync_joined_room(&client, room_id!("!a:example.org")).await;

        let mut chunk: Vec<serde_json::Value> = (0..12).map(|i| churn(100 - i)).collect();
        chunk.push(message(20, "three under thirty"));
        chunk.push(message(19, "older"));
        let events: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> =
            chunk.into_iter().map(|v| serde_json::from_value(v).unwrap()).collect();
        server
            .mock_room_messages()
            .ok(RoomMessagesResponseTemplate::default().events(events))
            .mount()
            .await;

        assert_eq!(
            look(&room).await,
            Outcome::Found("three under thirty".to_owned(), 20)
        );

        let requests = server.server().received_requests().await.unwrap();
        let sent = requests
            .iter()
            .find(|request| request.url.path().ends_with("/messages"))
            .expect("a /messages request");
        let query = sent.url.query().unwrap_or_default().to_owned();
        assert!(query.contains("limit=5"), "{query}");
        assert!(query.contains("dir=b"), "{query}");
        assert!(!query.contains("from="), "{query}");
        assert!(query.contains("m.room.message"), "filter types missing: {query}");
    }

    #[tokio::test]
    async fn look_reports_a_failed_request_as_failed_not_as_nothing() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = server.sync_joined_room(&client, room_id!("!a:example.org")).await;
        // No /messages mock mounted: the request fails.
        assert_eq!(look(&room).await, Outcome::Failed);
    }

    #[test]
    fn forgetting_a_user_clears_only_that_user_and_refuses_its_late_results() {
        let carol: OwnedUserId = "@carol:x".try_into().unwrap();
        let dave: OwnedUserId = "@dave:x".try_into().unwrap();
        let room = room_id!("!forget:x");
        let run = RunGuard::new(carol.clone());
        run.remember(room, Remembered::Message("carol's plaintext".into(), 1));
        let other = RunGuard::new(dave.clone());
        other.remember(room, Remembered::Message("dave's".into(), 2));

        forget_user(&carol);
        assert_eq!(remembered(&carol, room), None);
        assert_eq!(remembered(&dave, room), Some(Remembered::Message("dave's".into(), 2)));
        run.remember(room, Remembered::Message("late".into(), 3));
        assert_eq!(remembered(&carol, room), None);
        drop(run);
        drop(other);
        assert_eq!(remembered(&dave, room), None);
    }
}
