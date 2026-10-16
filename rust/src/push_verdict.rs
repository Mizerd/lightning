//! The account's push rules for one event, as matrix-sdk evaluates them.
//!
//! Desktop notifications follow this verdict (NotificationManager::decide), so
//! a room's mode set on any client, the account default, keywords and the
//! suppress rules for notices and edits apply here as they do in Element.
//! Lightning never evaluates rules itself.

use std::{
    collections::HashSet,
    future::Future,
    sync::{Arc, Mutex},
};

use matrix_sdk::{
    room::PushContext,
    ruma::{
        events::AnySyncTimelineEvent,
        push::{Action, PushConditionPowerLevelsCtx, PushConditionRoomCtx, Tweak},
        serde::Raw,
        OwnedRoomId, RoomId, UInt, UserId,
    },
    Room,
};
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    timeline::{EventTimelineItem, TimelineItem},
};
use serde_json::value::RawValue as RawJsonValue;

/// What the rules say about one event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PushVerdict {
    pub(crate) notify: bool,
    pub(crate) highlight: bool,
    /// The matched rule carries a `sound` tweak. This is the whole difference
    /// between "Notify" and "Loud" on the rules page (pushrules.rs
    /// `actions_for`), and matrix-sdk writes a keyword as notify + sound
    /// without highlight, so neither `notify` nor `highlight` can stand in
    /// for it.
    pub(crate) sound: bool,
}

impl PushVerdict {
    pub(crate) const QUIET: Self = Self { notify: false, highlight: false, sound: false };

    pub(crate) fn from_actions(actions: &[Action]) -> Self {
        Self {
            notify: actions.iter().any(Action::should_notify),
            highlight: actions.iter().any(Action::is_highlight),
            sound: actions.iter().any(|action| matches!(action, Action::SetTweak(Tweak::Sound(_)))),
        }
    }

    /// `push_notify` / `push_highlight` / `push_sound` on an event payload.
    /// Without a verdict nothing is written, which C++ reads as unknown.
    pub(crate) fn write(verdict: Option<Self>, payload: &mut serde_json::Value) {
        if let (Some(verdict), Some(object)) = (verdict, payload.as_object_mut()) {
            object.insert("push_notify".to_owned(), verdict.notify.into());
            object.insert("push_highlight".to_owned(), verdict.highlight.into());
            object.insert("push_sound".to_owned(), verdict.sound.into());
        }
    }
}

/// Verdicts for events given to the sync event handlers, which receive the
/// actions the SDK computed while it processed that sync.
///
/// The SDK hands over an empty list both when no rule notifies and when it
/// had no push context, and it builds a context exactly when our own member
/// event is known (matrix-sdk-base `get_push_room_context`). So an empty list
/// is a quiet verdict in a room whose own member the store holds. A room found
/// once is remembered for the client's life, so the initial-sync backlog costs
/// at most one store read per room.
///
/// Without our own member the SDK evaluated nothing, and that is not rare:
/// simplified sliding sync takes member state from `required_state` alone, and
/// a server may answer `$ME` with no member at all (measured against
/// Continuwuity 26.9.1, see rooms.rs "Member roster freshness"). Every room the
/// user has not opened then had no verdict, so a keyword or a plain
/// display-name mention, which only the rules can see, reached neither the
/// bell nor a notification while the server's highlight_count counted it. The
/// rules are then asked here, through the SDK's own evaluator, with the
/// context the SDK would have built: the room's member count and power levels
/// from the store and, for our display name, the account's global one (the
/// last answer kept in the store, then the localpart until one is known, as
/// the SDK does for a member without one). Known limits: a per-room nickname
/// is not seen, and a rename reaches this context up to `OWN_NAME_REFRESH`
/// late.
/// When the store cannot supply the count or the power levels, there is still
/// no verdict: a rule like `.m.rule.room_one_to_one` or `@room` would be
/// misjudged, and C++ keeps its local fallback for an unknown verdict.
#[derive(Clone, Default)]
pub(crate) struct SyncVerdicts {
    rooms_with_own_member: Arc<Mutex<HashSet<OwnedRoomId>>>,
    own_name: Arc<Mutex<OwnDisplayName>>,
}

/// The account's global display name, for the fallback context above. Asked
/// of the server at most once per `OWN_NAME_REFRESH` (sooner after a failure),
/// on a task of its own: an event handler runs inside sync processing and must
/// not wait on a request. Every answer is also kept in the account's state
/// store (`OWN_NAME_STORE_KEY`) and read back once per client before the first
/// fallback, because the request cannot be answered before a restart's
/// initial-sync backlog is judged, and on a server without `/notifications`
/// that backlog is the only place a mention received while offline appears.
/// Judged on the localpart instead, a mention by a display name that differs
/// from it would be lost for good.
#[derive(Default)]
struct OwnDisplayName {
    /// `Some(name)` once answered (or read back from the store); the
    /// localpart when the profile has none.
    name: Option<String>,
    next_ask: Option<std::time::Instant>,
    /// The store has been asked for the last answer (once per client).
    store_checked: bool,
}

/// State-store custom key for the last global display name the server gave.
/// A public profile field in the account's own store, deleted with it.
pub(crate) const OWN_NAME_STORE_KEY: &[u8] = b"lightning.push_verdict.own_display_name";

const OWN_NAME_REFRESH: std::time::Duration = std::time::Duration::from_secs(30 * 60);
const OWN_NAME_RETRY: std::time::Duration = std::time::Duration::from_secs(2 * 60);

impl SyncVerdicts {
    /// `event` is the event as the handler received it (decrypted when the
    /// SDK decrypted it); it is read only when the SDK had no push context.
    pub(crate) async fn verdict(
        &self,
        room: &Room,
        actions: &[Action],
        event: &RawJsonValue,
    ) -> Option<PushVerdict> {
        self.verdict_with(
            room.room_id(),
            actions,
            || async { matches!(room.get_member_no_sync(room.own_user_id()).await, Ok(Some(_))) },
            || self.fallback_actions(room, event),
        )
        .await
    }

    async fn verdict_with<F, Fut, G, Gut>(
        &self,
        room_id: &RoomId,
        actions: &[Action],
        own_member_known: F,
        fallback: G,
    ) -> Option<PushVerdict>
    where
        F: FnOnce() -> Fut,
        Fut: Future<Output = bool>,
        G: FnOnce() -> Gut,
        Gut: Future<Output = Option<Vec<Action>>>,
    {
        if !actions.is_empty() {
            return Some(PushVerdict::from_actions(actions));
        }
        if self.remembers(room_id) {
            return Some(PushVerdict::QUIET);
        }
        if !own_member_known().await {
            // Not remembered: once the member is stored (a roster fetch on
            // open), the SDK's own context takes over.
            return fallback().await.map(|actions| PushVerdict::from_actions(&actions));
        }
        if let Ok(mut rooms) = self.rooms_with_own_member.lock() {
            rooms.insert(room_id.to_owned());
        }
        Some(PushVerdict::QUIET)
    }

    fn remembers(&self, room_id: &RoomId) -> bool {
        self.rooms_with_own_member
            .lock()
            .map(|rooms| rooms.contains(room_id))
            .unwrap_or(false)
    }

    /// A verdict for an event the SDK evaluated with NO push context (its
    /// `Room::push_context` was `None`), so there are no actions to read and
    /// an empty list says nothing: unlike `verdict`, a room remembered as
    /// having our own member is not taken as quiet. `None` when the fallback
    /// cannot build a context either.
    pub(crate) async fn without_sdk_context(
        &self,
        room: &Room,
        event: &RawJsonValue,
    ) -> Option<PushVerdict> {
        self.fallback_actions(room, event).await.map(|actions| PushVerdict::from_actions(&actions))
    }

    /// The rules' actions for `event` with the context the SDK could not
    /// build; `None` when the store lacks what that context needs.
    async fn fallback_actions(&self, room: &Room, event: &RawJsonValue) -> Option<Vec<Action>> {
        let power_levels = room.power_levels().await.ok()?;
        let context = fallback_room_ctx(
            room.room_id(),
            room.own_user_id(),
            room.active_members_count(),
            self.own_display_name(room).await,
            Some(power_levels.into()),
        )?;
        let rules = room.client().account().push_rules().await.ok()?;
        let raw = Raw::<AnySyncTimelineEvent>::from_json(event.to_owned());
        Some(PushContext::new(context, rules).for_event(&raw).await)
    }

    /// The cached global display name, or the localpart; asks the server in
    /// the background when the cache is due. The first call per client reads
    /// the last answer back from the store (one read, then never again).
    async fn own_display_name(&self, room: &Room) -> String {
        let localpart = room.own_user_id().localpart().to_owned();
        let client = room.client();
        let check_store = self
            .own_name
            .lock()
            .map(|mut slot| !std::mem::replace(&mut slot.store_checked, true))
            .unwrap_or(false);
        if check_store {
            let stored = client
                .state_store()
                .get_custom_value(OWN_NAME_STORE_KEY)
                .await
                .ok()
                .flatten()
                .and_then(|bytes| String::from_utf8(bytes).ok())
                .filter(|name| !name.is_empty());
            if let (Some(stored), Ok(mut slot)) = (stored, self.own_name.lock()) {
                // A server answer that landed meanwhile is newer.
                if slot.name.is_none() {
                    slot.name = Some(stored);
                }
            }
        }
        let Ok(mut slot) = self.own_name.lock() else { return localpart };
        let now = std::time::Instant::now();
        if slot.next_ask.is_none_or(|at| now >= at) {
            slot.next_ask = Some(now + OWN_NAME_REFRESH);
            let cache = Arc::clone(&self.own_name);
            let fallback = localpart.clone();
            tokio::spawn(async move {
                let answer = client.account().get_display_name().await;
                let resolved = match answer {
                    Ok(name) => name.filter(|name| !name.is_empty()).unwrap_or(fallback),
                    Err(_) => {
                        if let Ok(mut slot) = cache.lock() {
                            slot.next_ask = Some(std::time::Instant::now() + OWN_NAME_RETRY);
                        }
                        return;
                    }
                };
                if let Ok(mut slot) = cache.lock() {
                    slot.name = Some(resolved.clone());
                }
                // For the next restart's backlog; a failed write only costs
                // that backlog the localpart until the next answer.
                let _ = client
                    .state_store()
                    .set_custom_value(OWN_NAME_STORE_KEY, resolved.into_bytes())
                    .await;
            });
        }
        slot.name.clone().unwrap_or(localpart)
    }
}

/// The push-condition context the SDK builds (`Room::push_condition_room_ctx`)
/// with our display name supplied rather than read from our member event.
/// `None` without a member count or power levels: the one-to-one rules and
/// `@room`'s sender permission would be judged on nothing. Pure.
pub(crate) fn fallback_room_ctx(
    room_id: &RoomId,
    user_id: &UserId,
    member_count: u64,
    display_name: String,
    power_levels: Option<PushConditionPowerLevelsCtx>,
) -> Option<PushConditionRoomCtx> {
    if member_count == 0 {
        return None;
    }
    let power_levels = power_levels?;
    let mut context = PushConditionRoomCtx::new(
        room_id.to_owned(),
        UInt::new(member_count).unwrap_or(UInt::MAX),
        user_id.to_owned(),
        display_name,
    );
    context.power_levels = Some(power_levels);
    Some(context)
}

/// The items of a live append paired with their JSON slots in the diff
/// payload `fill_diff_json` built: `append` and `push_back`, the two ops C++
/// notifies for. Every other op pairs nothing.
pub(crate) fn live_append_slots<'d, 'v, T: Clone>(
    diff: &'d VectorDiff<T>,
    payload: &'v mut serde_json::Value,
) -> Vec<(&'d T, &'v mut serde_json::Value)> {
    match diff {
        VectorDiff::Append { values } => {
            match payload.get_mut("items").and_then(|items| items.as_array_mut()) {
                Some(slots) => values.iter().zip(slots.iter_mut()).collect(),
                None => Vec::new(),
            }
        }
        VectorDiff::PushBack { value } => match payload.get_mut("item") {
            Some(slot) => vec![(value, slot)],
            None => Vec::new(),
        },
        _ => Vec::new(),
    }
}

/// What the backlog mark needs to know about one timeline item.
pub(crate) struct ItemFacts {
    pub(crate) event_id: String,
    /// `origin_server_ts`, milliseconds.
    pub(crate) timestamp_ms: u64,
    /// `unsigned.age` as delivered, milliseconds, when the server sent one.
    pub(crate) age_ms: Option<u64>,
}

/// Facts for a remote event item; `None` for a local echo or a virtual item.
pub(crate) fn item_facts(item: &TimelineItem) -> Option<ItemFacts> {
    #[derive(serde::Deserialize)]
    struct Unsigned {
        age: Option<i64>,
    }
    let event = item.as_event()?;
    let event_id = event.event_id()?.to_string();
    let age_ms = event
        .original_json()
        .and_then(|raw| raw.get_field::<Unsigned>("unsigned").ok().flatten())
        .and_then(|unsigned| unsigned.age)
        .map(|age| age.max(0) as u64);
    Some(ItemFacts { event_id, timestamp_ms: u64::from(event.timestamp().get()), age_ms })
}

pub(crate) fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| elapsed.as_millis() as u64)
        .unwrap_or(0)
}

/// Marks `backlog` on live appends that are history rather than arrivals. One
/// per open timeline (room or thread).
///
/// A limited sync (a room's first subscription, which re-sends its last 20
/// events; a gap; an expired sync session) makes the event cache drop to its
/// last chunk, and the timeline clears and appends that sync's events in one
/// batch. Among them only the events that reached the server after the
/// timeline opened are new, so after a `Clear` or `Reset` each append is
/// judged on its own: by `unsigned.age` against this device's clock, or by
/// its timestamp without one. An event this timeline already forwarded is
/// never new, reset or not.
pub(crate) struct BacklogMarker {
    opened_ms: u64,
    forwarded: HashSet<String>,
    reset_in_batch: bool,
}

/// Bound on the ids remembered per open timeline.
const MAX_FORWARDED: usize = 20_000;

impl BacklogMarker {
    /// `snapshot`: the items the timeline opened with.
    pub(crate) fn new(opened_ms: u64, snapshot: impl IntoIterator<Item = ItemFacts>) -> Self {
        let mut marker =
            Self { opened_ms, forwarded: HashSet::new(), reset_in_batch: false };
        for facts in snapshot {
            marker.remember(facts.event_id);
        }
        marker
    }

    /// Called once per batch of diffs from the timeline stream.
    pub(crate) fn begin_batch(&mut self) {
        self.reset_in_batch = false;
    }

    pub(crate) fn mark<T: Clone>(
        &mut self,
        diff: &VectorDiff<T>,
        payload: &mut serde_json::Value,
        now_ms: u64,
        facts: impl Fn(&T) -> Option<ItemFacts>,
    ) {
        if matches!(diff, VectorDiff::Clear | VectorDiff::Reset { .. }) {
            self.reset_in_batch = true;
        }
        for (item, slot) in live_append_slots(diff, payload) {
            let Some(facts) = facts(item) else { continue };
            let backlog = self.forwarded.contains(&facts.event_id)
                || (self.reset_in_batch && self.arrived_before_open(&facts, now_ms));
            if let (true, Some(object)) = (backlog, slot.as_object_mut()) {
                object.insert("backlog".to_owned(), true.into());
            }
        }
        let values: Vec<&T> = match diff {
            VectorDiff::Append { values } | VectorDiff::Reset { values } => values.iter().collect(),
            VectorDiff::PushFront { value }
            | VectorDiff::PushBack { value }
            | VectorDiff::Insert { value, .. }
            | VectorDiff::Set { value, .. } => vec![value],
            _ => Vec::new(),
        };
        for value in values {
            if let Some(facts) = facts(value) {
                self.remember(facts.event_id);
            }
        }
    }

    fn arrived_before_open(&self, facts: &ItemFacts, now_ms: u64) -> bool {
        match facts.age_ms {
            // Measured by the server when it answered, so no clock skew.
            Some(age) => now_ms.saturating_sub(age) < self.opened_ms,
            None => facts.timestamp_ms < self.opened_ms,
        }
    }

    fn remember(&mut self, event_id: String) {
        if self.forwarded.len() >= MAX_FORWARDED {
            // Past the bound only the time rule is left.
            self.forwarded.clear();
        }
        self.forwarded.insert(event_id);
    }
}

/// Verdict for a live timeline item. `None` for a local echo, which has no
/// server JSON yet.
pub(crate) async fn for_timeline_item(
    context: &PushContext,
    item: &EventTimelineItem,
) -> Option<PushVerdict> {
    let raw = item.original_json()?;
    Some(PushVerdict::from_actions(&context.for_event(raw).await))
}

#[cfg(test)]
mod tests {
    use matrix_sdk::ruma::{
        push::{
            Action, EventMatchConditionData, HighlightTweakValue, NewConditionalPushRule,
            NewPatternedPushRule, NewPushRule, NewSimplePushRule, PushCondition,
            PushConditionRoomCtx, RuleKind, Ruleset, SoundTweakValue, Tweak,
        },
        serde::Raw,
        uint, OwnedRoomId, OwnedUserId, RoomId, UserId,
    };
    use std::sync::atomic::{AtomicUsize, Ordering};

    use matrix_sdk_ui::eyeball_im::{Vector, VectorDiff};
    use serde_json::json;

    use super::{
        fallback_room_ctx, live_append_slots, BacklogMarker, ItemFacts, PushConditionPowerLevelsCtx,
        PushVerdict, SyncVerdicts,
    };

    fn me() -> OwnedUserId {
        UserId::parse("@me:example.org").unwrap()
    }

    fn room() -> OwnedRoomId {
        RoomId::parse("!room:example.org").unwrap()
    }

    // A group chat (three members), so `.m.rule.room_one_to_one` stays out of
    // it, as in the reporter's rooms.
    fn context() -> PushConditionRoomCtx {
        PushConditionRoomCtx::new(room(), uint!(3), me(), "Me".to_owned())
    }

    fn message(content: serde_json::Value) -> Raw<serde_json::Value> {
        Raw::new(&json!({
            "type": "m.room.message",
            "event_id": "$event:example.org",
            "room_id": room(),
            "sender": "@them:example.org",
            "origin_server_ts": 1,
            "content": content,
        }))
        .unwrap()
    }

    fn plain() -> Raw<serde_json::Value> {
        message(json!({ "msgtype": "m.text", "body": "hello", "m.mentions": {} }))
    }

    async fn verdict(rules: &Ruleset, event: &Raw<serde_json::Value>) -> PushVerdict {
        PushVerdict::from_actions(rules.get_actions(event, &context()).await)
    }

    // The rule matrix-sdk's NotificationSettings writes for "Mentions &
    // keywords", which is what Element and Element X write too: a `room` rule
    // with no actions.
    fn mentions_and_keywords_room(rules: &mut Ruleset) {
        rules
            .insert(NewPushRule::Room(NewSimplePushRule::new(room(), vec![])), None, None)
            .unwrap();
    }

    #[test]
    fn actions_map_to_notify_and_highlight() {
        let highlight = Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes));
        assert_eq!(
            PushVerdict::from_actions(&[]),
            PushVerdict { notify: false, highlight: false, sound: false }
        );
        assert_eq!(
            PushVerdict::from_actions(&[Action::Notify]),
            PushVerdict { notify: true, highlight: false, sound: false }
        );
        assert_eq!(
            PushVerdict::from_actions(&[Action::Notify, highlight.clone()]),
            PushVerdict { notify: true, highlight: true, sound: false }
        );
        assert_eq!(
            PushVerdict::from_actions(&[
                Action::Notify,
                Action::SetTweak(Tweak::Highlight(HighlightTweakValue::No)),
            ]),
            PushVerdict { notify: true, highlight: false, sound: false }
        );
    }

    #[test]
    fn write_adds_both_fields_only_with_a_verdict() {
        let mut payload = json!({ "event_id": "$e" });
        PushVerdict::write(None, &mut payload);
        assert!(payload.get("push_notify").is_none());
        assert!(payload.get("push_highlight").is_none());
        assert!(payload.get("push_sound").is_none());
        PushVerdict::write(Some(PushVerdict { notify: false, highlight: true, sound: true }), &mut payload);
        assert_eq!(payload["push_notify"], json!(false));
        assert_eq!(payload["push_highlight"], json!(true));
        assert_eq!(payload["push_sound"], json!(true));
    }

    // "Notify" and "Loud" on the rules page differ ONLY by the sound tweak
    // (pushrules.rs `actions_for`). Before the sound bit, both mapped to the
    // same verdict, so this device chimed for both or neither.
    #[test]
    fn loud_and_notify_differ_only_in_sound() {
        use crate::pushrules::{actions_for, Level};
        let notify = PushVerdict::from_actions(&actions_for(Level::Notify));
        let loud = PushVerdict::from_actions(&actions_for(Level::Loud));
        assert_eq!(notify, PushVerdict { notify: true, highlight: true, sound: false });
        assert_eq!(loud, PushVerdict { notify: true, highlight: true, sound: true });
        assert_ne!(notify, loud, "the two levels must be distinguishable here");
    }

    #[tokio::test]
    async fn a_plain_message_notifies_under_the_default_rules() {
        let rules = Ruleset::server_default(&me());
        assert_eq!(
            verdict(&rules, &plain()).await,
            PushVerdict { notify: true, highlight: false, sound: false }
        );
    }

    // Issue #15: the room rule set on another client.
    #[tokio::test]
    async fn a_mentions_and_keywords_room_rule_quiets_a_plain_message() {
        let mut rules = Ruleset::server_default(&me());
        mentions_and_keywords_room(&mut rules);
        assert!(!verdict(&rules, &plain()).await.notify);
    }

    #[tokio::test]
    async fn a_mention_still_notifies_in_a_mentions_and_keywords_room() {
        let mut rules = Ruleset::server_default(&me());
        mentions_and_keywords_room(&mut rules);
        let mention = message(json!({
            "msgtype": "m.text",
            "body": "hi",
            "m.mentions": { "user_ids": [me()] },
        }));
        assert_eq!(
            verdict(&rules, &mention).await,
            PushVerdict { notify: true, highlight: true, sound: true }
        );
    }

    fn keyword_rule(rules: &mut Ruleset, actions: Vec<Action>) {
        rules
            .insert(
                NewPushRule::Content(NewPatternedPushRule::new(
                    "banana".to_owned(),
                    "banana".to_owned(),
                    actions,
                )),
                None,
                None,
            )
            .unwrap();
    }

    fn keyword_message() -> Raw<serde_json::Value> {
        message(json!({
            "msgtype": "m.text",
            "body": "I like banana bread",
            "m.mentions": {},
        }))
    }

    // matrix-sdk (so Element X) writes a keyword as notify + sound, with no
    // highlight, so notify alone has to be enough.
    #[tokio::test]
    async fn an_sdk_keyword_notifies_in_a_mentions_and_keywords_room() {
        let mut rules = Ruleset::server_default(&me());
        mentions_and_keywords_room(&mut rules);
        keyword_rule(
            &mut rules,
            vec![Action::Notify, Action::SetTweak(Tweak::Sound(SoundTweakValue::Default))],
        );
        assert_eq!(
            verdict(&rules, &keyword_message()).await,
            PushVerdict { notify: true, highlight: false, sound: true }
        );
    }

    #[tokio::test]
    async fn a_highlighting_keyword_notifies_and_highlights() {
        let mut rules = Ruleset::server_default(&me());
        mentions_and_keywords_room(&mut rules);
        keyword_rule(
            &mut rules,
            vec![Action::Notify, Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes))],
        );
        assert_eq!(
            verdict(&rules, &keyword_message()).await,
            PushVerdict { notify: true, highlight: true, sound: false }
        );
    }

    // Issue #15: the account default for group chats set to mentions and
    // keywords (Element X's "Group chats" setting), with no room rule at all.
    #[tokio::test]
    async fn an_account_default_of_mentions_and_keywords_quiets_a_plain_message() {
        let mut rules = Ruleset::server_default(&me());
        for rule_id in [".m.rule.message", ".m.rule.encrypted"] {
            rules
                .set_actions(RuleKind::Underride, rule_id, vec![])
                .unwrap();
        }
        assert!(!verdict(&rules, &plain()).await.notify);
    }

    #[tokio::test]
    async fn notices_and_edits_are_quiet_under_the_default_rules() {
        let rules = Ruleset::server_default(&me());
        let notice = message(json!({ "msgtype": "m.notice", "body": "bot", "m.mentions": {} }));
        assert!(!verdict(&rules, &notice).await.notify);
        let edit = message(json!({
            "msgtype": "m.text",
            "body": "* fixed",
            "m.mentions": {},
            "m.new_content": { "msgtype": "m.text", "body": "fixed", "m.mentions": {} },
            "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig:example.org" },
        }));
        assert!(!verdict(&rules, &edit).await.notify);
    }

    #[tokio::test]
    async fn a_muted_room_quiets_even_a_mention() {
        let mut rules = Ruleset::server_default(&me());
        // matrix-sdk's Mute: an override rule matching the room, no actions.
        rules
            .insert(
                NewPushRule::Override(NewConditionalPushRule::new(
                    room().to_string(),
                    vec![PushCondition::EventMatch(EventMatchConditionData::new(
                        "room_id".to_owned(),
                        room().to_string(),
                    ))],
                    vec![],
                )),
                None,
                None,
            )
            .unwrap();
        let mention = message(json!({
            "msgtype": "m.text",
            "body": "hi",
            "m.mentions": { "user_ids": [me()] },
        }));
        assert!(!verdict(&rules, &mention).await.notify);
    }

    // S1: the SDK's own actions are the verdict; an empty list costs one own-member
    // lookup per room, never one per event.
    #[tokio::test]
    async fn a_non_empty_action_list_needs_no_lookup() {
        let verdicts = SyncVerdicts::default();
        let lookups = AtomicUsize::new(0);
        let verdict = verdicts
            .verdict_with(
                &room(),
                &[Action::Notify],
                || async {
                    lookups.fetch_add(1, Ordering::SeqCst);
                    true
                },
                || async { panic!("the SDK's own actions need no fallback") },
            )
            .await;
        assert_eq!(verdict, Some(PushVerdict { notify: true, highlight: false, sound: false }));
        assert_eq!(lookups.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn an_empty_list_is_quiet_once_our_member_is_known_and_looked_up_once() {
        let verdicts = SyncVerdicts::default();
        let lookups = AtomicUsize::new(0);
        for _ in 0..5 {
            let verdict = verdicts
                .verdict_with(
                    &room(),
                    &[],
                    || async {
                        lookups.fetch_add(1, Ordering::SeqCst);
                        true
                    },
                    || async { panic!("a known member needs no fallback") },
                )
                .await;
            assert_eq!(verdict, Some(PushVerdict::QUIET));
        }
        assert_eq!(lookups.load(Ordering::SeqCst), 1, "one lookup per room, not per event");
    }

    #[tokio::test]
    async fn an_empty_list_without_our_member_asks_the_fallback_until_it_is_known() {
        let verdicts = SyncVerdicts::default();
        let fallbacks = AtomicUsize::new(0);
        // No own member: the SDK had no push context. A fallback that cannot
        // build one is still no verdict.
        assert_eq!(
            verdicts
                .verdict_with(&room(), &[], || async { false }, || async {
                    fallbacks.fetch_add(1, Ordering::SeqCst);
                    None
                })
                .await,
            None
        );
        // One that can is the verdict.
        let highlight = Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes));
        assert_eq!(
            verdicts
                .verdict_with(&room(), &[], || async { false }, || async {
                    fallbacks.fetch_add(1, Ordering::SeqCst);
                    Some(vec![Action::Notify, highlight.clone()])
                })
                .await,
            Some(PushVerdict { notify: true, highlight: true, sound: false })
        );
        // Not remembered: the next event looks again, and is quiet once known,
        // without asking the fallback.
        assert_eq!(
            verdicts
                .verdict_with(&room(), &[], || async { true }, || async {
                    fallbacks.fetch_add(1, Ordering::SeqCst);
                    None
                })
                .await,
            Some(PushVerdict::QUIET)
        );
        assert_eq!(fallbacks.load(Ordering::SeqCst), 2);
        // Another room is looked up on its own.
        let other = RoomId::parse("!other:example.org").unwrap();
        assert_eq!(
            verdicts.verdict_with(&other, &[], || async { false }, || async { None }).await,
            None
        );
    }

    // Spec 1.17 dropped `.m.rule.contains_display_name` from the defaults
    // (ruma's server_default follows), but servers still send it in the
    // account's rules, and the reporter's server highlighted on it. Its shape,
    // under an id `insert` accepts.
    fn display_name_rule(rules: &mut Ruleset) {
        #[allow(deprecated)]
        rules
            .insert(
                NewPushRule::Override(NewConditionalPushRule::new(
                    "contains_display_name".to_owned(),
                    vec![PushCondition::ContainsDisplayName],
                    vec![
                        Action::Notify,
                        Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes)),
                    ],
                )),
                None,
                None,
            )
            .unwrap();
    }

    fn power_levels() -> PushConditionPowerLevelsCtx {
        use matrix_sdk::ruma::{
            int,
            power_levels::NotificationPowerLevels,
            room_version_rules::{AuthorizationRules, RoomPowerLevelsRules},
        };
        PushConditionPowerLevelsCtx::new(
            std::collections::BTreeMap::new(),
            int!(0),
            NotificationPowerLevels::new(),
            RoomPowerLevelsRules::new(&AuthorizationRules::V1, []),
        )
    }

    async fn fallback_verdict(
        rules: &Ruleset,
        event: &Raw<serde_json::Value>,
        display_name: &str,
    ) -> PushVerdict {
        let context =
            fallback_room_ctx(&room(), &me(), 3, display_name.to_owned(), Some(power_levels()))
                .expect("a count and power levels make a context");
        PushVerdict::from_actions(rules.get_actions(event, &context).await)
    }

    // Break-it rerun, section 10: with no own member in the store, a server
    // keyword and a plain display-name mention in a room nobody had opened
    // reached neither the bell nor a notification. These are what the
    // fallback context must see; neither carries m.mentions.
    #[tokio::test]
    async fn the_fallback_context_sees_a_keyword_and_a_plain_display_name() {
        let mut rules = Ruleset::server_default(&me());
        keyword_rule(
            &mut rules,
            vec![Action::Notify, Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes))],
        );
        let keyword = message(json!({ "msgtype": "m.text", "body": "saw a BANANA today" }));
        assert!(fallback_verdict(&rules, &keyword, "Rokas").await.highlight);
        display_name_rule(&mut rules);
        let by_name = message(json!({ "msgtype": "m.text", "body": "post-restart Rokas name only" }));
        assert!(fallback_verdict(&rules, &by_name, "Rokas").await.highlight);
        // The name it was given, not any word: a plain message stays a plain
        // message.
        assert!(!fallback_verdict(&rules, &plain(), "Rokas").await.highlight);
    }

    // Review of the break-it fix (L3, L1): the fallback's glue through a real
    // client store rather than a hand-built context. The room is synced the
    // way Continuwuity sends it, with no member of ours, so the SDK has no
    // push context; the fallback must still find the power levels (which need
    // the create event's creators), the member count, and the account's own
    // rules from account data, and turn the handler's JSON into an event. The
    // display name is the one a previous run stored, as after a restart: the
    // server's profile answer (none is mounted here) cannot have arrived.
    #[tokio::test]
    async fn the_fallback_reads_its_whole_context_from_a_synced_store() {
        use matrix_sdk::{
            ruma::{
                api::client::sync::sync_events::v5, events::push_rules::PushRulesEventContent,
                room_id,
            },
            test_utils::mocks::MatrixMockServer,
        };
        use matrix_sdk_base::RequestedRequiredStates;
        use serde_json::value::{to_raw_value, RawValue};

        fn raw<T>(value: serde_json::Value) -> Raw<T> {
            Raw::from_json_string(value.to_string()).unwrap()
        }
        fn state(kind: &str, key: &str, content: serde_json::Value, id: &str) -> serde_json::Value {
            json!({
                "type": kind, "state_key": key, "sender": "@alice:example.org",
                "event_id": id, "origin_server_ts": 1_700_000_000_000u64, "content": content,
            })
        }
        fn joined(user: &str, id: &str) -> serde_json::Value {
            state("m.room.member", user, json!({ "membership": "join" }), id)
        }
        fn event(body: &str) -> Box<RawValue> {
            to_raw_value(&json!({
                "type": "m.room.message", "event_id": "$m:example.org",
                "sender": "@alice:example.org", "origin_server_ts": 1_700_000_000_001u64,
                "content": { "msgtype": "m.text", "body": body },
            }))
            .unwrap()
        }

        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let me = client.user_id().unwrap().to_owned();
        assert_ne!(me.localpart(), "Rokas", "the stored name must differ from the localpart");

        let mut rules = Ruleset::server_default(&me);
        keyword_rule(
            &mut rules,
            vec![Action::Notify, Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes))],
        );
        display_name_rule(&mut rules);
        let full = room_id!("!full:example.org");
        let bare = room_id!("!bare:example.org");
        let mut response = v5::Response::new("pos-1".to_owned());
        let mut entry = v5::response::Room::default();
        entry.required_state = vec![
            state("m.room.create", "", json!({ "room_version": "11" }), "$c"),
            state(
                "m.room.power_levels",
                "",
                json!({ "users": { "@alice:example.org": 100 } }),
                "$p",
            ),
            joined("@alice:example.org", "$a"),
            joined("@bob:example.org", "$b"),
        ]
        .into_iter()
        .map(raw)
        .collect();
        entry.joined_count = Some(uint!(3));
        response.rooms.insert(full.to_owned(), entry);
        // A room whose power levels the store does not hold.
        let mut entry = v5::response::Room::default();
        entry.required_state = vec![raw(joined("@alice:example.org", "$a2"))];
        entry.joined_count = Some(uint!(3));
        response.rooms.insert(bare.to_owned(), entry);
        response.extensions.account_data.global = vec![raw(json!({
            "type": "m.push_rules",
            "content": PushRulesEventContent::new(rules),
        }))];
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        client
            .state_store()
            .set_custom_value(super::OWN_NAME_STORE_KEY, b"Rokas".to_vec())
            .await
            .expect("stored name");

        let room = client.get_room(full).expect("synced");
        assert!(room.get_member_no_sync(&me).await.unwrap().is_none(), "no member of ours");
        let verdicts = SyncVerdicts::default();
        let highlight = |verdict: Option<PushVerdict>| verdict.map(|verdict| verdict.highlight);
        assert_eq!(highlight(verdicts.verdict(&room, &[], &event("saw a BANANA")).await), Some(true));
        assert_eq!(
            highlight(verdicts.verdict(&room, &[], &event("ping Rokas please")).await),
            Some(true),
            "the stored name, not the localpart"
        );
        assert_eq!(highlight(verdicts.verdict(&room, &[], &event("hello")).await), Some(false));
        // The late-decryption entry (L4) asks the same fallback, and it does
        // so even for a room remembered as having our member: with no SDK
        // context there is no empty action list to call quiet. Through
        // `verdict` this room would now read as quiet.
        assert_eq!(
            verdicts.verdict_with(full, &[], || async { true }, || async { None }).await,
            Some(PushVerdict::QUIET)
        );
        assert_eq!(
            highlight(verdicts.without_sdk_context(&room, &event("saw a BANANA")).await),
            Some(true)
        );
        // No power levels: still no verdict, never a guessed one.
        let room = client.get_room(bare).expect("synced");
        assert_eq!(verdicts.verdict(&room, &[], &event("saw a BANANA")).await, None);
    }

    #[test]
    fn the_fallback_context_needs_a_member_count_and_power_levels() {
        assert!(fallback_room_ctx(&room(), &me(), 0, "Me".to_owned(), Some(power_levels()))
            .is_none());
        assert!(fallback_room_ctx(&room(), &me(), 2, "Me".to_owned(), None).is_none());
        let context =
            fallback_room_ctx(&room(), &me(), 2, "Me".to_owned(), Some(power_levels())).unwrap();
        assert_eq!(context.member_count, uint!(2));
        assert_eq!(context.user_display_name, "Me");
        assert_eq!(context.user_id, me());
    }

    // The annotation's pairing of timeline items with their JSON slots.
    #[test]
    fn live_append_slots_pair_append_and_push_back_in_order() {
        let append = VectorDiff::Append { values: Vector::from(vec![10u32, 20, 30]) };
        let mut payload = json!({ "op": "append", "items": [{ "n": 0 }, { "n": 1 }, { "n": 2 }] });
        let pairs = live_append_slots(&append, &mut payload);
        assert_eq!(pairs.len(), 3);
        for (value, slot) in pairs {
            slot["value"] = json!(*value);
        }
        assert_eq!(payload["items"][0], json!({ "n": 0, "value": 10 }));
        assert_eq!(payload["items"][2], json!({ "n": 2, "value": 30 }));

        let push_back = VectorDiff::PushBack { value: 7u32 };
        let mut payload = json!({ "op": "push_back", "item": {} });
        let pairs = live_append_slots(&push_back, &mut payload);
        assert_eq!(pairs.len(), 1);
        assert_eq!(*pairs[0].0, 7);
    }

    #[test]
    fn live_append_slots_pair_nothing_for_other_ops() {
        let mut payload = json!({ "item": {}, "items": [{}] });
        for diff in [
            VectorDiff::PushFront { value: 1u32 },
            VectorDiff::Insert { index: 0, value: 1 },
            VectorDiff::Set { index: 0, value: 1 },
            VectorDiff::Reset { values: Vector::from(vec![1]) },
        ] {
            assert!(live_append_slots(&diff, &mut payload).is_empty());
        }
        // An append whose payload lost its items pairs nothing rather than
        // misplacing a verdict.
        let append = VectorDiff::Append { values: Vector::from(vec![1u32]) };
        assert!(live_append_slots(&append, &mut json!({ "op": "append" })).is_empty());
    }

    // A test item: (event id, origin_server_ts, unsigned.age); id "" = no event.
    type Item = (&'static str, u64, Option<u64>);

    fn facts(item: &Item) -> Option<ItemFacts> {
        (!item.0.is_empty()).then(|| ItemFacts {
            event_id: item.0.to_owned(),
            timestamp_ms: item.1,
            age_ms: item.2,
        })
    }

    const OPENED: u64 = 100_000;
    const NOW: u64 = 110_000;

    fn marker(snapshot: &[Item]) -> BacklogMarker {
        BacklogMarker::new(OPENED, snapshot.iter().filter_map(facts))
    }

    fn append(marker: &mut BacklogMarker, items: &[Item]) -> serde_json::Value {
        let mut payload = json!({ "op": "append", "items": vec![json!({}); items.len()] });
        let diff = VectorDiff::Append { values: items.iter().copied().collect::<Vector<_>>() };
        marker.mark(&diff, &mut payload, NOW, facts);
        payload
    }

    fn clear(marker: &mut BacklogMarker) {
        marker.mark(&VectorDiff::<Item>::Clear, &mut json!({ "op": "clear" }), NOW, facts);
    }

    fn is_backlog(slot: &serde_json::Value) -> bool {
        slot.get("backlog") == Some(&json!(true))
    }

    // The shape a room's first subscription produced live: the timeline
    // cleared, then its last 20 events appended in the same batch, all of them
    // older than the open.
    #[test]
    fn history_re_appended_after_a_clear_is_backlog() {
        let mut marker = marker(&[("$latest", 99_000, Some(10))]);
        marker.begin_batch();
        clear(&mut marker);
        let payload = append(
            &mut marker,
            &[
                ("$old", 50_000, Some(60_000)),
                ("$older", 40_000, None),
                ("$latest", 99_000, Some(11_000)),
            ],
        );
        for slot in payload["items"].as_array().unwrap() {
            assert!(is_backlog(slot), "{slot}");
        }
    }

    // Review B1: a gap or an expired sync session produces the same batch, and
    // an event that reached the server after the open is a new message.
    #[test]
    fn an_unseen_event_after_the_open_is_not_backlog_even_after_a_clear() {
        let mut marker = marker(&[("$latest", 99_000, None)]);
        marker.begin_batch();
        clear(&mut marker);
        let payload = append(
            &mut marker,
            &[
                ("$history", 90_000, Some(20_000)),
                ("$new", 105_000, Some(4_000)),
                ("$no_age", 105_000, None),
            ],
        );
        assert!(is_backlog(&payload["items"][0]));
        assert!(!is_backlog(&payload["items"][1]));
        assert!(!is_backlog(&payload["items"][2]));
    }

    // The age is measured by the server when it answered, so a skewed
    // timestamp does not decide when an age came with the event.
    #[test]
    fn the_age_decides_over_a_skewed_timestamp() {
        let mut marker = marker(&[]);
        marker.begin_batch();
        clear(&mut marker);
        let payload = append(
            &mut marker,
            // Server clock ahead: stamped after the open, reached the server
            // 15 s ago (before it). Server clock behind: stamped long before,
            // reached the server 100 ms ago.
            &[("$ahead", 200_000, Some(15_000)), ("$behind", 1_000, Some(100))],
        );
        assert!(is_backlog(&payload["items"][0]));
        assert!(!is_backlog(&payload["items"][1]));
    }

    // Without a reset the time is not consulted (a live message stamped early
    // still notifies), but an event this timeline already forwarded is never
    // new: here a live message notified once, then re-delivered after a
    // clear, and a snapshot event moved to the end.
    #[test]
    fn a_forwarded_event_is_backlog_and_time_counts_only_after_a_reset() {
        let mut marker = marker(&[("$snap", 99_000, None)]);
        marker.begin_batch();
        let first = append(&mut marker, &[("$live", 105_000, Some(10)), ("$early", 1_000, None)]);
        assert!(!is_backlog(&first["items"][0]));
        assert!(!is_backlog(&first["items"][1]));

        marker.begin_batch();
        marker.mark(&VectorDiff::<Item>::Remove { index: 0 }, &mut json!({}), NOW, facts);
        let moved = append(&mut marker, &[("$snap", 99_000, None)]);
        assert!(is_backlog(&moved["items"][0]));

        marker.begin_batch();
        clear(&mut marker);
        let again = append(&mut marker, &[("$live", 105_000, Some(5_000))]);
        assert!(is_backlog(&again["items"][0]));

        // The reset does not carry into the next batch.
        marker.begin_batch();
        let next = append(&mut marker, &[("$later", 50_000, None)]);
        assert!(!is_backlog(&next["items"][0]));
    }

    // Appends before the clear in the same batch, and items with no event
    // (local echoes, virtual rows), are never marked.
    #[test]
    fn appends_before_the_clear_and_items_without_an_event_are_not_marked() {
        let mut marker = marker(&[]);
        marker.begin_batch();
        let early = append(&mut marker, &[("$before", 10_000, None)]);
        clear(&mut marker);
        let virtual_rows = append(&mut marker, &[("", 10_000, None)]);
        assert!(!is_backlog(&early["items"][0]));
        assert!(!is_backlog(&virtual_rows["items"][0]));
        // A reset the subscriber sends after lagging starts a resync too.
        marker.begin_batch();
        marker.mark(
            &VectorDiff::Reset { values: Vector::from(vec![("$kept", 10_000, None)]) },
            &mut json!({}),
            NOW,
            facts,
        );
        let mut push_back = json!({ "op": "push_back", "item": {} });
        let old = VectorDiff::PushBack { value: ("$old", 10_000, None) };
        marker.mark(&old, &mut push_back, NOW, facts);
        assert!(is_backlog(&push_back["item"]));
    }
}
