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
    ruma::{push::Action, OwnedRoomId, RoomId},
    Room,
};
use matrix_sdk_ui::{eyeball_im::VectorDiff, timeline::EventTimelineItem};

/// What the rules say about one event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PushVerdict {
    pub(crate) notify: bool,
    pub(crate) highlight: bool,
}

impl PushVerdict {
    pub(crate) const QUIET: Self = Self { notify: false, highlight: false };

    pub(crate) fn from_actions(actions: &[Action]) -> Self {
        Self {
            notify: actions.iter().any(Action::should_notify),
            highlight: actions.iter().any(Action::is_highlight),
        }
    }

    /// `push_notify` / `push_highlight` on an event payload. Without a verdict
    /// nothing is written, which C++ reads as unknown.
    pub(crate) fn write(verdict: Option<Self>, payload: &mut serde_json::Value) {
        if let (Some(verdict), Some(object)) = (verdict, payload.as_object_mut()) {
            object.insert("push_notify".to_owned(), verdict.notify.into());
            object.insert("push_highlight".to_owned(), verdict.highlight.into());
        }
    }
}

/// Verdicts for events given to the sync event handlers, which receive the
/// actions the SDK computed while it processed that sync.
///
/// The SDK hands over an empty list both when no rule notifies and when it
/// had no push context, and it builds a context exactly when our own member
/// event is known (matrix-sdk-base `get_push_room_context`). So an empty list
/// is a quiet verdict in a room whose own member the store holds, and no
/// verdict otherwise. Evaluating the rules again could only repeat one of
/// those two answers. A room found once is remembered for the client's life,
/// so the initial-sync backlog costs at most one store read per room.
#[derive(Clone, Default)]
pub(crate) struct SyncVerdicts {
    rooms_with_own_member: Arc<Mutex<HashSet<OwnedRoomId>>>,
}

impl SyncVerdicts {
    pub(crate) async fn verdict(&self, room: &Room, actions: &[Action]) -> Option<PushVerdict> {
        self.verdict_with(room.room_id(), actions, || async {
            matches!(room.get_member_no_sync(room.own_user_id()).await, Ok(Some(_)))
        })
        .await
    }

    async fn verdict_with<F, Fut>(
        &self,
        room_id: &RoomId,
        actions: &[Action],
        own_member_known: F,
    ) -> Option<PushVerdict>
    where
        F: FnOnce() -> Fut,
        Fut: Future<Output = bool>,
    {
        if !actions.is_empty() {
            return Some(PushVerdict::from_actions(actions));
        }
        if self.remembers(room_id) {
            return Some(PushVerdict::QUIET);
        }
        if !own_member_known().await {
            return None;
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

    use super::{live_append_slots, PushVerdict, SyncVerdicts};

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
            PushVerdict { notify: false, highlight: false }
        );
        assert_eq!(
            PushVerdict::from_actions(&[Action::Notify]),
            PushVerdict { notify: true, highlight: false }
        );
        assert_eq!(
            PushVerdict::from_actions(&[Action::Notify, highlight.clone()]),
            PushVerdict { notify: true, highlight: true }
        );
        assert_eq!(
            PushVerdict::from_actions(&[
                Action::Notify,
                Action::SetTweak(Tweak::Highlight(HighlightTweakValue::No)),
            ]),
            PushVerdict { notify: true, highlight: false }
        );
    }

    #[test]
    fn write_adds_both_fields_only_with_a_verdict() {
        let mut payload = json!({ "event_id": "$e" });
        PushVerdict::write(None, &mut payload);
        assert!(payload.get("push_notify").is_none());
        assert!(payload.get("push_highlight").is_none());
        PushVerdict::write(Some(PushVerdict { notify: false, highlight: true }), &mut payload);
        assert_eq!(payload["push_notify"], json!(false));
        assert_eq!(payload["push_highlight"], json!(true));
    }

    #[tokio::test]
    async fn a_plain_message_notifies_under_the_default_rules() {
        let rules = Ruleset::server_default(&me());
        assert_eq!(
            verdict(&rules, &plain()).await,
            PushVerdict { notify: true, highlight: false }
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
            PushVerdict { notify: true, highlight: true }
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
            PushVerdict { notify: true, highlight: false }
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
            PushVerdict { notify: true, highlight: true }
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
            .verdict_with(&room(), &[Action::Notify], || async {
                lookups.fetch_add(1, Ordering::SeqCst);
                true
            })
            .await;
        assert_eq!(verdict, Some(PushVerdict { notify: true, highlight: false }));
        assert_eq!(lookups.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn an_empty_list_is_quiet_once_our_member_is_known_and_looked_up_once() {
        let verdicts = SyncVerdicts::default();
        let lookups = AtomicUsize::new(0);
        for _ in 0..5 {
            let verdict = verdicts
                .verdict_with(&room(), &[], || async {
                    lookups.fetch_add(1, Ordering::SeqCst);
                    true
                })
                .await;
            assert_eq!(verdict, Some(PushVerdict::QUIET));
        }
        assert_eq!(lookups.load(Ordering::SeqCst), 1, "one lookup per room, not per event");
    }

    #[tokio::test]
    async fn an_empty_list_without_our_member_is_no_verdict_until_it_is_known() {
        let verdicts = SyncVerdicts::default();
        // No own member: the SDK had no push context, so there is no verdict.
        assert_eq!(verdicts.verdict_with(&room(), &[], || async { false }).await, None);
        // Not remembered: the next event looks again, and is quiet once known.
        assert_eq!(
            verdicts.verdict_with(&room(), &[], || async { true }).await,
            Some(PushVerdict::QUIET)
        );
        // Another room is looked up on its own.
        let other = RoomId::parse("!other:example.org").unwrap();
        assert_eq!(verdicts.verdict_with(&other, &[], || async { false }).await, None);
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
}
