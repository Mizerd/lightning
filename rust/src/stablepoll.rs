//! Stable `m.poll.start` polls. matrix-sdk-ui only understands the MSC3381
//! unstable namespace, so a stable poll reaches the timeline as an "other"
//! row and its responses and end are never aggregated onto it. This module
//! reads the start from the row's JSON and tallies votes from the room's
//! event cache.

use std::collections::{BTreeSet, HashMap};

use matrix_sdk::{
    ruma::{
        events::{
            poll::{
                compile_poll_results, start::PollStartEventContent, PollResponseData,
            },
            room::message::Relation,
            AnySyncMessageLikeEvent, AnySyncTimelineEvent, SyncMessageLikeEvent,
        },
        serde::Raw,
        EventId, MilliSecondsSinceUnixEpoch, OwnedUserId, UserId,
    },
    Room,
};
use matrix_sdk::deserialized_responses::TimelineEvent;

pub(crate) const START_TYPE: &str = "m.poll.start";
const RESPONSE_TYPES: [&str; 2] = ["m.poll.response", "org.matrix.msc3381.poll.response"];
const END_TYPES: [&str; 2] = ["m.poll.end", "org.matrix.msc3381.poll.end"];

/// A new stable poll, not an edit of one.
pub(crate) fn is_start(event: &AnySyncTimelineEvent) -> bool {
    matches!(
        event,
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::PollStart(
            SyncMessageLikeEvent::Original(start),
        )) if !matches!(start.content.relates_to, Some(Relation::Replacement(_)))
    )
}

pub(crate) fn parse_start(raw: &Raw<AnySyncTimelineEvent>) -> Option<PollStartEventContent> {
    if raw.get_field::<String>("type").ok().flatten().as_deref() != Some(START_TYPE) {
        return None;
    }
    let content = raw.get_field::<PollStartEventContent>("content").ok().flatten()?;
    (!content.poll.answers.is_empty()).then_some(content)
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Tally {
    pub votes: HashMap<String, Vec<String>>,
    pub ended: bool,
}

pub(crate) struct Relating {
    pub sender: OwnedUserId,
    pub ts: MilliSecondsSinceUnixEpoch,
    pub selections: Option<Vec<String>>,
}

/// The poll a response or end refers to, for either namespace.
pub(crate) fn relation_target(raw: &Raw<AnySyncTimelineEvent>) -> Option<String> {
    let kind = raw.get_field::<String>("type").ok().flatten()?;
    if !RESPONSE_TYPES.contains(&kind.as_str()) && !END_TYPES.contains(&kind.as_str()) {
        return None;
    }
    let content = raw.get_field::<serde_json::Value>("content").ok().flatten()?;
    let relates = content.get("m.relates_to")?;
    (relates.get("rel_type")?.as_str()? == "m.reference")
        .then(|| relates.get("event_id")?.as_str().map(ToOwned::to_owned))
        .flatten()
}

pub(crate) fn relating(raw: &Raw<AnySyncTimelineEvent>) -> Option<Relating> {
    let kind = raw.get_field::<String>("type").ok().flatten()?;
    let sender = raw.get_field::<OwnedUserId>("sender").ok().flatten()?;
    let ts = raw.get_field::<MilliSecondsSinceUnixEpoch>("origin_server_ts").ok().flatten()?;
    let content = raw.get_field::<serde_json::Value>("content").ok().flatten()?;
    let selections = if RESPONSE_TYPES.contains(&kind.as_str()) {
        let list = content
            .get("m.selections")
            .or_else(|| content.get("org.matrix.msc3381.poll.response")?.get("answers"))?
            .as_array()?;
        Some(list.iter().filter_map(|v| v.as_str().map(ToOwned::to_owned)).collect())
    } else if END_TYPES.contains(&kind.as_str()) {
        None
    } else {
        return None;
    };
    Some(Relating { sender, ts, selections })
}

/// Responses after the first valid end are dropped. Only the poll's sender, or
/// someone allowed to redact other people's events, may end it.
pub(crate) fn tally(
    start: &PollStartEventContent,
    relations: &[Relating],
    may_end: impl Fn(&UserId) -> bool,
) -> Tally {
    let end = relations
        .iter()
        .filter(|r| r.selections.is_none() && may_end(&r.sender))
        .map(|r| r.ts)
        .min();
    let responses = relations.iter().filter_map(|r| {
        Some(PollResponseData {
            sender: &r.sender,
            origin_server_ts: r.ts,
            selections: r.selections.as_deref()?,
        })
    });
    let results = compile_poll_results(&start.poll, responses, end);
    let votes = results
        .into_iter()
        .filter(|(_, users): &(&str, BTreeSet<&UserId>)| !users.is_empty())
        .map(|(id, users)| {
            (id.to_owned(), users.into_iter().map(|u| u.to_string()).collect())
        })
        .collect();
    Tally { votes, ended: end.is_some() }
}

pub(crate) async fn load_tally(
    room: &Room,
    poll_id: &EventId,
    sender: &UserId,
    start: &PollStartEventContent,
) -> Tally {
    let Ok((cache, _drop_handles)) = room.event_cache().await else {
        return Tally::default();
    };
    let related: Vec<TimelineEvent> = cache
        .find_event_relations(
            poll_id,
            Some(vec![matrix_sdk::ruma::events::relation::RelationType::Reference]),
        )
        .await
        .unwrap_or_default();
    let relations: Vec<Relating> = related
        .iter()
        .filter(|event| {
            relation_target(event.raw()).as_deref() == Some(poll_id.as_str())
        })
        .filter_map(|event| relating(event.raw()))
        .collect();
    let any_foreign_end = relations
        .iter()
        .any(|r| r.selections.is_none() && r.sender != sender);
    let power = if any_foreign_end { room.power_levels().await.ok() } else { None };
    tally(start, &relations, |user| {
        user == sender
            || power.as_ref().is_some_and(|p| p.user_can_redact_event_of_other(user))
    })
}

/// The stable polls one open timeline shows, and a room event cache
/// subscription (taken only once one is seen) that says when to re-tally.
#[derive(Default)]
pub(crate) struct Watch {
    polls: HashMap<String, (OwnedUserId, PollStartEventContent)>,
    updates: Option<matrix_sdk::event_cache::RoomEventCacheSubscriber>,
}

impl Watch {
    /// Records the stable polls among `items` and returns the new ones.
    pub(crate) fn observe<'a>(
        &mut self,
        items: impl Iterator<Item = &'a std::sync::Arc<matrix_sdk_ui::timeline::TimelineItem>>,
    ) -> Vec<String> {
        let mut fresh = Vec::new();
        for item in items {
            let Some(event) = item.as_event() else { continue };
            let Some(id) = event.event_id() else { continue };
            if self.polls.contains_key(id.as_str()) {
                continue;
            }
            let Some(start) = event.original_json().and_then(parse_start) else { continue };
            self.polls.insert(id.to_string(), (event.sender().to_owned(), start));
            fresh.push(id.to_string());
        }
        fresh
    }

    pub(crate) fn watching(&self) -> bool {
        self.updates.is_some()
    }

    pub(crate) async fn subscribe(&mut self, room: &Room) {
        if self.updates.is_some() || self.polls.is_empty() {
            return;
        }
        if let Ok((cache, _drop_handles)) = room.event_cache().await {
            self.updates = cache.subscribe().await.ok().map(|(_events, sub)| sub);
        }
    }

    /// Waits for a cache update and returns the polls it may have changed.
    pub(crate) async fn next_update(&mut self) -> Vec<String> {
        use matrix_sdk::event_cache::RoomEventCacheUpdate;
        use tokio::sync::broadcast::error::RecvError;
        let Some(updates) = self.updates.as_mut() else { return Vec::new() };
        match updates.recv().await {
            Ok(RoomEventCacheUpdate::UpdateTimelineEvents(update)) => {
                let mut hit: Vec<String> = Vec::new();
                for diff in &update.diffs {
                    let values: Vec<&TimelineEvent> = match diff {
                        matrix_sdk_ui::eyeball_im::VectorDiff::Append { values }
                        | matrix_sdk_ui::eyeball_im::VectorDiff::Reset { values } => values.iter().collect(),
                        matrix_sdk_ui::eyeball_im::VectorDiff::PushFront { value }
                        | matrix_sdk_ui::eyeball_im::VectorDiff::PushBack { value }
                        | matrix_sdk_ui::eyeball_im::VectorDiff::Insert { value, .. }
                        | matrix_sdk_ui::eyeball_im::VectorDiff::Set { value, .. } => vec![value],
                        _ => Vec::new(),
                    };
                    for event in values {
                        if let Some(target) = relation_target(event.raw()) {
                            if self.polls.contains_key(&target) && !hit.contains(&target) {
                                hit.push(target);
                            }
                        }
                    }
                }
                hit
            }
            Ok(_) => Vec::new(),
            Err(RecvError::Lagged(_)) => self.polls.keys().cloned().collect(),
            Err(RecvError::Closed) => {
                self.updates = None;
                Vec::new()
            }
        }
    }

    /// Re-tallies `ids`; returns those whose tally changed.
    pub(crate) async fn refresh(
        &self,
        room: &Room,
        store: &std::sync::Mutex<HashMap<String, Tally>>,
        ids: &[String],
    ) -> Vec<String> {
        let mut changed = Vec::new();
        for id in ids {
            let Some((sender, start)) = self.polls.get(id) else { continue };
            let Ok(event_id) = EventId::parse(id) else { continue };
            let tally = load_tally(room, &event_id, sender, start).await;
            let Ok(mut map) = store.lock() else { continue };
            if map.get(id) != Some(&tally) {
                if map.len() >= MAX_TALLIES {
                    map.clear();
                }
                map.insert(id.clone(), tally);
                changed.push(id.clone());
            }
        }
        changed
    }
}

const MAX_TALLIES: usize = 2048;

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::{
        events::AnySyncTimelineEvent,
        room_version_rules::RoomVersionRules,
    };
    use serde_json::json;

    fn raw(value: serde_json::Value) -> Raw<AnySyncTimelineEvent> {
        Raw::from_json_string(value.to_string()).unwrap()
    }

    fn start_json() -> serde_json::Value {
        json!({"type": "m.poll.start", "event_id": "$poll", "sender": "@alice:x",
            "origin_server_ts": 1000, "content": {
                "m.text": [{"body": "Lunch?"}],
                "m.poll": {"kind": "m.disclosed", "max_selections": 1,
                    "question": {"m.text": [{"body": "Lunch?"}]},
                    "answers": [{"m.id": "a", "m.text": [{"body": "Pizza"}]},
                                {"m.id": "b", "m.text": [{"body": "Soup"}]}]}}})
    }

    fn response(id: &str, sender: &str, ts: u64, answers: &[&str], stable: bool)
        -> serde_json::Value
    {
        let mut content = json!({"m.relates_to": {"rel_type": "m.reference", "event_id": "$poll"}});
        if stable {
            content["m.selections"] = json!(answers);
        } else {
            content["org.matrix.msc3381.poll.response"] = json!({"answers": answers});
        }
        json!({"type": if stable { "m.poll.response" } else { "org.matrix.msc3381.poll.response" },
            "event_id": id, "sender": sender, "origin_server_ts": ts, "content": content})
    }

    fn end(id: &str, sender: &str, ts: u64) -> serde_json::Value {
        json!({"type": "m.poll.end", "event_id": id, "sender": sender, "origin_server_ts": ts,
            "content": {"m.text": [{"body": "Ended"}],
                "m.relates_to": {"rel_type": "m.reference", "event_id": "$poll"}}})
    }

    fn relations(values: Vec<serde_json::Value>) -> Vec<Relating> {
        values.into_iter().filter_map(|v| relating(&raw(v))).collect()
    }

    #[test]
    fn a_stable_poll_start_passes_the_timeline_filter_and_its_edit_does_not() {
        let rules = RoomVersionRules::V11;
        let event = raw(start_json()).deserialize().unwrap();
        assert!(is_start(&event));
        assert!(crate::timeline::lightning_event_filter(&event, &rules));
        let mut edit = start_json();
        edit["content"]["m.relates_to"] = json!({"rel_type": "m.replace", "event_id": "$old"});
        edit["content"]["m.new_content"] = edit["content"].clone();
        let event: AnySyncTimelineEvent = raw(edit).deserialize().unwrap();
        assert!(!is_start(&event));
        let response: AnySyncTimelineEvent =
            raw(response("$v", "@bob:x", 2000, &["a"], true)).deserialize().unwrap();
        assert!(!crate::timeline::lightning_event_filter(&response, &rules));
    }

    #[test]
    fn both_namespaces_count_and_only_the_latest_vote_per_user() {
        let start = parse_start(&raw(start_json())).unwrap();
        let tally = tally(&start, &relations(vec![
            response("$v1", "@bob:x", 2000, &["a"], true),
            response("$v2", "@carol:x", 2100, &["a"], false),
            response("$v3", "@bob:x", 2200, &["b"], false),
            response("$v4", "@dave:x", 2300, &["zzz"], true),
        ]), |_| false);
        assert_eq!(tally.votes.get("a"), Some(&vec!["@carol:x".to_owned()]));
        assert_eq!(tally.votes.get("b"), Some(&vec!["@bob:x".to_owned()]));
        assert_eq!(tally.votes.len(), 2, "a spoiled vote counts for nothing");
        assert!(!tally.ended);
    }

    #[test]
    fn only_an_allowed_end_closes_the_poll_and_later_votes_are_dropped() {
        let start = parse_start(&raw(start_json())).unwrap();
        let rels = relations(vec![
            response("$v1", "@bob:x", 2000, &["a"], true),
            end("$e1", "@mallory:x", 2500),
            response("$v2", "@carol:x", 3000, &["b"], true),
        ]);
        let open = tally(&start, &rels, |u| u.as_str() == "@alice:x");
        assert!(!open.ended, "a stranger cannot end someone else's poll");
        assert_eq!(open.votes.len(), 2);
        let closed = tally(&start, &rels, |u| u.as_str() == "@mallory:x");
        assert!(closed.ended);
        assert_eq!(closed.votes.len(), 1);
        assert!(closed.votes.contains_key("a"));
    }

    #[test]
    fn a_relation_targets_its_poll_only_through_a_reference() {
        assert_eq!(relation_target(&raw(response("$v", "@b:x", 1, &["a"], true))).as_deref(),
                   Some("$poll"));
        let mut thread = response("$v", "@b:x", 1, &["a"], true);
        thread["content"]["m.relates_to"]["rel_type"] = json!("m.thread");
        assert_eq!(relation_target(&raw(thread)), None);
        assert!(parse_start(&raw(response("$v", "@b:x", 1, &["a"], true))).is_none());
    }
}
