
use std::collections::BTreeSet;
use std::sync::Arc;

use serde_json::{json, Value};

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

pub(crate) const RULE_USER: &str = "m.policy.rule.user";
pub(crate) const RULE_SERVER: &str = "m.policy.rule.server";
pub(crate) const RULE_ROOM: &str = "m.policy.rule.room";

const RULE_USER_LEGACY: &str = "org.matrix.mjolnir.rule.user";
const RULE_SERVER_LEGACY: &str = "org.matrix.mjolnir.rule.server";
const RULE_ROOM_LEGACY: &str = "org.matrix.mjolnir.rule.room";

const MAX_RULES: usize = 2000;
const MAX_ENTITY_CHARS: usize = 512;
const MAX_SUBSCRIPTIONS: usize = 32;
const SUBSCRIPTIONS_TYPE: &str = "org.lightning_matrix.policy_lists";

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Rule {
    pub kind: String,
    pub entity: String,
    /// The recommendation as written, not an enum: ruma's `Recommendation` has
    /// one known variant (`m.ban`) plus `_Custom`, where Mjolnir's legacy
    /// `org.matrix.mjolnir.ban` lands. Comparing strings keeps legacy lists
    /// readable.
    pub recommendation: String,
    pub reason: String,
    pub state_key: String,
}

impl Rule {
    pub fn is_ban(&self) -> bool {
        self.recommendation == "m.ban"
            || self.recommendation == "org.matrix.mjolnir.ban"
    }
}

fn kind_of(event_type: &str) -> Option<&'static str> {
    match event_type {
        RULE_USER | RULE_USER_LEGACY => Some("user"),
        RULE_SERVER | RULE_SERVER_LEGACY => Some("server"),
        RULE_ROOM | RULE_ROOM_LEGACY => Some("room"),
        _ => None,
    }
}

pub(crate) fn rule_from_state(value: &Value) -> Option<Rule> {
    let event_type = value.get("type")?.as_str()?;
    let kind = kind_of(event_type)?;
    let content = value.get("content")?;
    let entity = content.get("entity")?.as_str()?.trim();
    // Bounded: `glob_matches` is O(pattern x value) and `check_entity` runs it
    // over up to 32 rooms x 2000 rules, and the entity is attacker-written. A
    // Matrix id is at most 255 bytes, so 512 chars is generous.
    if entity.is_empty() || entity.chars().count() > MAX_ENTITY_CHARS {
        return None;
    }
    Some(Rule {
        kind: kind.to_owned(),
        entity: entity.to_owned(),
        recommendation: content
            .get("recommendation")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        reason: content
            .get("reason")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .chars()
            .take(500)
            .collect(),
        state_key: value
            .get("state_key")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
    })
}

pub(crate) fn glob_matches(pattern: &str, value: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let v: Vec<char> = value.chars().collect();
    let (mut pi, mut vi) = (0usize, 0usize);
    let mut star: Option<usize> = None;
    let mut star_vi = 0usize;

    while vi < v.len() {
        // Check `*` first: otherwise a pattern `*` meeting a literal `*` in the
        // (attacker-supplied) value matched literally without recording a star,
        // and the rule under-matched.
        if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            star_vi = vi;
            pi += 1;
        } else if pi < p.len() && (p[pi] == '?' || p[pi] == v[vi]) {
            pi += 1;
            vi += 1;
        } else if let Some(s) = star {
            pi = s + 1;
            star_vi += 1;
            vi = star_vi;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

fn server_of(entity: &str) -> &str {
    match entity.split_once(':') {
        Some((_, server)) => server,
        None => entity,
    }
}

pub(crate) fn find_match<'a>(rules: &'a [Rule], kind: &str, entity: &str)
    -> Option<&'a Rule>
{
    rules.iter().find(|rule| {
        if !rule.is_ban() {
            return false;
        }
        match rule.kind.as_str() {
            "server" => glob_matches(&rule.entity, server_of(entity)),
            k if k == kind => glob_matches(&rule.entity, entity),
            _ => false,
        }
    })
}

pub(crate) async fn read_rules(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::room::Room,
) -> (Vec<Rule>, bool) {
    use matrix_sdk::config::RequestConfig;
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    use matrix_sdk::ruma::api::client::state::get_state_events;
    use matrix_sdk::ruma::events::StateEventType;

    let mut out: Vec<Rule> = Vec::new();
    let mut seen: BTreeSet<(String, String)> = BTreeSet::new();
    let mut absorb = |value: &Value, out: &mut Vec<Rule>| -> bool {
        if let Some(rule) = rule_from_state(value) {
            let key = (rule.kind.clone(), rule.state_key.clone());
            if seen.insert(key) {
                out.push(rule);
            }
        }
        out.len() >= MAX_RULES
    };

    for type_name in [
        RULE_USER, RULE_SERVER, RULE_ROOM,
        RULE_USER_LEGACY, RULE_SERVER_LEGACY, RULE_ROOM_LEGACY,
    ] {
        let Ok(events) = room.get_state_events(StateEventType::from(type_name)).await
        else {
            continue;
        };
        for raw in events {
            let json = match &raw {
                RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
            };
            if let Ok(value) = serde_json::from_str::<Value>(&json) {
                if absorb(&value, &mut out) {
                    return (out, true);
                }
            }
        }
    }
    // No early return on a non-empty store (unlike widgets.rs): after
    // publishing a rule the store holds just that one, and returning it with
    // `truncated: false` would present a one-rule list as the complete ban list.
    // The network read always runs and the results are merged.
    let store_rules = out.len();

    let config = RequestConfig::new()
        .disable_retry()
        .timeout(std::time::Duration::from_secs(20));
    let request = get_state_events::v3::Request::new(room.room_id().to_owned());
    let Ok(response) = client.send(request).with_request_config(config).await else {
        return (out, store_rules > 0);
    };
    for raw in response.room_state {
        let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) else {
            continue;
        };
        if absorb(&value, &mut out) {
            return (out, true);
        }
    }
    (out, false)
}

fn rule_type_for(kind: &str) -> Option<&'static str> {
    match kind {
        "user" => Some(RULE_USER),
        "server" => Some(RULE_SERVER),
        "room" => Some(RULE_ROOM),
        _ => None,
    }
}

pub(crate) fn state_key_for(entity: &str) -> String {
    format!("rule:{entity}")
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn write_rule(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    kind: String,
    entity: String,
    state_key: String,
    recommendation: String,
    reason: String,
) -> Result<(), String> {
    let Some(event_type) = rule_type_for(&kind) else {
        return Err("unknown rule kind".to_owned());
    };
    let entity = entity.trim().to_owned();
    if entity.is_empty() {
        return Err("a rule needs an entity".to_owned());
    }
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = async {
            let room = joined_room(&client, &room_id)
                .map_err(|_| "unknown_room".to_owned())?;
            let own = client.user_id().ok_or_else(|| "forbidden".to_owned())?;
            let allowed = room
                .get_member_no_sync(own)
                .await
                .ok()
                .flatten()
                .is_some_and(|m| {
                    m.can_send_state(
                        matrix_sdk::ruma::events::StateEventType::from(event_type),
                    )
                });
            if !allowed {
                return Err("forbidden".to_owned());
            }
            let content = if recommendation.trim().is_empty() {
                json!({})
            } else {
                json!({
                    "entity": entity,
                    "recommendation": recommendation,
                    "reason": reason,
                })
            };
            let key = if state_key.is_empty() {
                state_key_for(&entity)
            } else {
                state_key
            };
            room.send_state_event_raw(event_type, &key, content)
                .await
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
            Ok::<(), String>(())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match outcome {
            Ok(()) => (true, String::new()),
            Err(category) => (false, category),
        };
        enqueue(
            &events,
            json!({
                "type": "policy_rule_written",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
            }),
        );
    });
    Ok(())
}

pub(crate) async fn read_subscriptions(client: &matrix_sdk::Client) -> Vec<String> {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    let ty = GlobalAccountDataEventType::from(SUBSCRIPTIONS_TYPE);
    let Ok(Some(raw)) = client.account().fetch_account_data(ty).await else {
        return Vec::new();
    };
    let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) else {
        return Vec::new();
    };
    value
        .get("rooms")
        .and_then(|v| v.as_array())
        .map(|rooms| {
            rooms
                .iter()
                .filter_map(|v| v.as_str())
                .filter(|id| id.starts_with('!'))
                .take(MAX_SUBSCRIPTIONS)
                .map(|s| s.to_owned())
                .collect()
        })
        .unwrap_or_default()
}

pub(crate) async fn set_subscribed(
    client: &matrix_sdk::Client,
    room_id: &str,
    subscribed: bool,
) -> Result<Vec<String>, String> {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    if !room_id.starts_with('!') {
        return Err("rejected".to_owned());
    }
    let mut rooms = read_subscriptions(client).await;
    let present = rooms.iter().any(|r| r == room_id);
    if subscribed == present {
        return Ok(rooms);
    }
    if subscribed {
        if rooms.len() >= MAX_SUBSCRIPTIONS {
            return Err("too_many".to_owned());
        }
        rooms.push(room_id.to_owned());
    } else {
        rooms.retain(|r| r != room_id);
    }
    let ty = GlobalAccountDataEventType::from(SUBSCRIPTIONS_TYPE);
    let content = json!({ "rooms": rooms });
    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    client
        .account()
        .set_account_data_raw(ty, raw)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(rooms)
}

pub(crate) fn fetch_rules(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let (rules, truncated, ok, can_write) = match joined_room(&client, &room_id) {
            Ok(room) => {
                let (rules, truncated) = read_rules(&client, &room).await;
                let can_write = match client.user_id() {
                    Some(own) => room
                        .get_member_no_sync(own)
                        .await
                        .ok()
                        .flatten()
                        .is_some_and(|m| {
                            m.can_send_state(
                                matrix_sdk::ruma::events::StateEventType::from(
                                    RULE_USER,
                                ),
                            )
                        }),
                    None => false,
                };
                (rules, truncated, true, can_write)
            }
            Err(_) => (Vec::new(), false, false, false),
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let rows: Vec<Value> = rules
            .iter()
            .map(|r| {
                json!({
                    "kind": r.kind,
                    "entity": r.entity,
                    "recommendation": r.recommendation,
                    "isBan": r.is_ban(),
                    "reason": r.reason,
                    "stateKey": r.state_key,
                })
            })
            .collect();
        enqueue(
            &events,
            json!({
                "type": "policy_rules",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "room_id": room_id,
                "can_write": can_write,
                "truncated": truncated,
                "rules": rows,
            }),
        );
    });
    Ok(())
}

pub(crate) fn check_entity(
    bridge: &RustClient,
    op_id: u64,
    kind: String,
    entity: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let subscriptions = read_subscriptions(&client).await;
        let mut hit: Option<(String, Rule)> = None;
        for room_id in &subscriptions {
            let Ok(room) = joined_room(&client, room_id) else {
                continue;
            };
            let (rules, _) = read_rules(&client, &room).await;
            if let Some(rule) = find_match(&rules, &kind, &entity) {
                hit = Some((room_id.clone(), rule.clone()));
                break;
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut payload = json!({
            "type": "policy_check",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": true,
            "entity": entity,
            "kind": kind,
            "subscriptions": subscriptions,
            "matched": hit.is_some(),
        });
        if let Some((room_id, rule)) = hit {
            payload["room_id"] = json!(room_id);
            payload["rule_entity"] = json!(rule.entity);
            payload["rule_kind"] = json!(rule.kind);
            payload["reason"] = json!(rule.reason);
        }
        enqueue(&events, payload);
    });
    Ok(())
}

pub(crate) fn subscribe(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    subscribed: bool,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = set_subscribed(&client, &room_id, subscribed).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, rooms) = match outcome {
            Ok(rooms) => (true, String::new(), rooms),
            Err(category) => (false, category, Vec::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "policy_subscriptions",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "rooms": rooms,
            }),
        );
    });
    Ok(())
}

pub(crate) fn fetch_subscriptions(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let rooms = read_subscriptions(&client).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "policy_subscriptions",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": true,
                "category": "",
                "rooms": rooms,
            }),
        );
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_glob_is_a_glob_and_not_a_regular_expression() {
        assert!(glob_matches("@spam:example.org", "@spam:example.org"));
        assert!(glob_matches("@*:example.org", "@anyone:example.org"));
        assert!(glob_matches("*:example.org", "@a:example.org"));
        assert!(glob_matches("@bot?:example.org", "@bot1:example.org"));
        assert!(!glob_matches("@bot?:example.org", "@bot12:example.org"));
        assert!(!glob_matches("@*:example.org", "@anyone:other.org"));

        assert!(glob_matches("*", ""));
        assert!(glob_matches("a*b", "ab"));
        assert!(glob_matches("**", "anything"));

        assert!(glob_matches("@a.b:example.org", "@a.b:example.org"));
        assert!(!glob_matches("@a.b:example.org", "@axb:example.org"));
        assert!(!glob_matches("@a+:example.org", "@aaa:example.org"));

        assert!(glob_matches("*abc", "zzabc"));
        assert!(glob_matches("*a*b", "xaybzb"));
        assert!(!glob_matches("*abc", "zzabcd"));
    }

    #[test]
    fn a_literal_star_in_the_value_does_not_eat_the_patterns_wildcard() {
        assert!(glob_matches("*x", "*yx"));
        assert!(glob_matches("*", "***"));
        assert!(glob_matches("a*c", "a*b*c"));
        assert!(glob_matches("*", "@a:example.org"));
        assert!(!glob_matches("a*c", "abd"));
    }

    #[test]
    fn an_absurdly_long_entity_is_not_a_rule() {
        let long: String = std::iter::repeat('*').take(MAX_ENTITY_CHARS + 1)
            .collect();
        let value = json!({
            "type": "m.policy.rule.user",
            "state_key": "rule:x",
            "content": { "entity": long, "recommendation": "m.ban" },
        });
        assert!(rule_from_state(&value).is_none());

        let ok: String = std::iter::repeat('a').take(MAX_ENTITY_CHARS)
            .collect();
        let value = json!({
            "type": "m.policy.rule.user",
            "state_key": "rule:x",
            "content": { "entity": ok, "recommendation": "m.ban" },
        });
        assert!(rule_from_state(&value).is_some());
    }

    #[test]
    fn a_server_rule_covers_everyone_on_that_server() {
        let rules = vec![Rule {
            kind: "server".into(),
            entity: "spam.example".into(),
            recommendation: "m.ban".into(),
            reason: "spam".into(),
            state_key: "rule:spam.example".into(),
        }];
        assert!(find_match(&rules, "user", "@anyone:spam.example").is_some());
        assert!(find_match(&rules, "user", "@anyone:other.example").is_none());
        assert!(find_match(&rules, "room", "!abc:spam.example").is_some());
    }

    #[test]
    fn a_rule_that_does_not_recommend_a_ban_matches_nothing() {
        let rules = vec![Rule {
            kind: "user".into(),
            entity: "@a:example.org".into(),
            recommendation: "m.mute".into(),
            reason: String::new(),
            state_key: "rule:@a:example.org".into(),
        }];
        assert!(find_match(&rules, "user", "@a:example.org").is_none());
    }

    #[test]
    fn mjolnirs_legacy_spelling_is_read_as_a_ban() {
        let rules = vec![Rule {
            kind: "user".into(),
            entity: "@a:example.org".into(),
            recommendation: "org.matrix.mjolnir.ban".into(),
            reason: String::new(),
            state_key: "rule:@a:example.org".into(),
        }];
        assert!(find_match(&rules, "user", "@a:example.org").is_some());
    }

    #[test]
    fn the_legacy_type_names_are_read_too() {
        for (ty, kind) in [
            ("m.policy.rule.user", "user"),
            ("org.matrix.mjolnir.rule.user", "user"),
            ("m.policy.rule.server", "server"),
            ("org.matrix.mjolnir.rule.server", "server"),
            ("m.policy.rule.room", "room"),
            ("org.matrix.mjolnir.rule.room", "room"),
        ] {
            let value = json!({
                "type": ty,
                "state_key": "rule:@a:example.org",
                "content": {
                    "entity": "@a:example.org",
                    "recommendation": "m.ban",
                    "reason": "why",
                },
            });
            let rule = rule_from_state(&value)
                .unwrap_or_else(|| panic!("{ty} was not read"));
            assert_eq!(rule.kind, kind);
            assert_eq!(rule.entity, "@a:example.org");
        }
    }

    #[test]
    fn a_removed_rule_is_not_a_rule() {
        let removed = json!({
            "type": "m.policy.rule.user",
            "state_key": "rule:@a:example.org",
            "content": {},
        });
        assert!(rule_from_state(&removed).is_none());
        let blank = json!({
            "type": "m.policy.rule.user",
            "state_key": "rule:@a:example.org",
            "content": { "entity": "   ", "recommendation": "m.ban" },
        });
        assert!(rule_from_state(&blank).is_none());
    }

    #[test]
    fn the_state_key_follows_the_convention_other_tools_write() {
        assert_eq!(state_key_for("@a:example.org"), "rule:@a:example.org");
    }
}
