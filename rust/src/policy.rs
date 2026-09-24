//! Mjolnir-style policy lists: `m.policy.rule.{user,server,room}` state
//! events in a room, recommending entities to ban.
//!
//! Reads a policy room's rules, writes them where the account has the power
//! level, and matches an entity against the lists this account subscribes
//! to. It never acts on a match by itself: a list is someone else's
//! judgement, so matches feed the existing ignore and kick/ban actions and
//! the user decides.
//!
//! `Room::get_state_events` is store-only and these events are not in
//! sliding sync's required state, so the store usually answers "no rules";
//! the read falls back to a bounded raw `/state`, as for widgets and pinned
//! events.

use std::collections::BTreeSet;
use std::sync::Arc;

use serde_json::{json, Value};

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

pub(crate) const RULE_USER: &str = "m.policy.rule.user";
pub(crate) const RULE_SERVER: &str = "m.policy.rule.server";
pub(crate) const RULE_ROOM: &str = "m.policy.rule.room";

/// Mjolnir's pre-spec type names, still carried by real lists.
const RULE_USER_LEGACY: &str = "org.matrix.mjolnir.rule.user";
const RULE_SERVER_LEGACY: &str = "org.matrix.mjolnir.rule.server";
const RULE_ROOM_LEGACY: &str = "org.matrix.mjolnir.rule.room";

/// Bound on one room's rules. Public ban lists run to thousands; this only
/// answers questions about a handful of people.
const MAX_RULES: usize = 2000;
/// Longest entity glob a rule may carry (see `rule_from_state`).
const MAX_ENTITY_CHARS: usize = 512;
/// Bound on subscribed lists; each is a whole-room state fetch.
const MAX_SUBSCRIPTIONS: usize = 32;
/// Where the subscription list lives: Lightning's own account data type,
/// since there is no spec for it.
const SUBSCRIPTIONS_TYPE: &str = "org.lightning_matrix.policy_lists";

/// One rule, flattened for the FFI.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Rule {
    /// "user", "server" or "room".
    pub kind: String,
    /// The glob. `*` and `?` are wildcards; everything else is literal.
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
    /// Whether this rule recommends a ban, under either spelling.
    pub fn is_ban(&self) -> bool {
        self.recommendation == "m.ban"
            || self.recommendation == "org.matrix.mjolnir.ban"
    }
}

/// Which of the three kinds a type name is, legacy spellings included.
fn kind_of(event_type: &str) -> Option<&'static str> {
    match event_type {
        RULE_USER | RULE_USER_LEGACY => Some("user"),
        RULE_SERVER | RULE_SERVER_LEGACY => Some("server"),
        RULE_ROOM | RULE_ROOM_LEGACY => Some("room"),
        _ => None,
    }
}

/// Parse one state event into a rule, or reject it. No entity (including a
/// removed rule's empty content) is not a rule; an empty entity is refused
/// here rather than trusting the matcher to handle it.
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

/// Glob match per the policy-rule spec: `*` matches any run (including
/// empty), `?` exactly one character, nothing else is special (not a regex,
/// so `.` and `+` in ids stay literal). Iterative with backtracking, since
/// the pattern is attacker-chosen.
pub(crate) fn glob_matches(pattern: &str, value: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let v: Vec<char> = value.chars().collect();
    let (mut pi, mut vi) = (0usize, 0usize);
    // Where to resume if the current `*` ate too little.
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
            // Backtrack: let the last `*` swallow one more character.
            pi = s + 1;
            star_vi += 1;
            vi = star_vi;
        } else {
            return false;
        }
    }
    // Trailing `*`s may match nothing.
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

/// The server part of a Matrix id, or the whole string without a colon.
fn server_of(entity: &str) -> &str {
    match entity.split_once(':') {
        Some((_, server)) => server,
        None => entity,
    }
}

/// Does any rule in `rules` cover `entity`? User rules match the id; server
/// rules match its server, so "ban everyone from example.org" covers every
/// user on it.
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

/// Read one policy room's rules. Bounded, and `truncated` says so, so a
/// partial answer does not read as complete.
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
            // One entry per (type, state key): legacy and current names, and store and
            // network, can carry the same rule.
            let key = (rule.kind.clone(), rule.state_key.clone());
            if seen.insert(key) {
                out.push(rule);
            }
        }
        out.len() >= MAX_RULES
    };

    // 1. The store, free when the state is there.
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

    // 2. The network, which is what actually answers (see the module docs).
    let config = RequestConfig::new()
        .disable_retry()
        .timeout(std::time::Duration::from_secs(20));
    let request = get_state_events::v3::Request::new(room.room_id().to_owned());
    let Ok(response) = client.send(request).with_request_config(config).await else {
        // The network read failed: whatever the store gave is a fragment, so it is
        // reported as truncated.
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

/// The conventional state key for a rule, `rule:<entity>`. Not required,
/// but matching other tools lets them see and replace the same rule.
pub(crate) fn state_key_for(entity: &str) -> String {
    format!("rule:{entity}")
}

/// Publish or remove one rule. An empty `recommendation` removes it by
/// writing an empty content object (Mjolnir's convention).
///
/// An empty `state_key` derives it from the entity (for new rules). A
/// removal must pass the rule's own key as read from the room: rules from
/// other tools may use a different key, and removing by the derived key
/// would report success while the rule stayed.
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
            // The gate: the room's required level for this event type, from the SDK;
            // false when membership cannot be read.
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

/// Read this account's subscribed policy rooms.
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

/// Subscribe to or unsubscribe from one policy room. Read-modify-write
/// against the server copy, so a list another device added is not dropped.
/// Still last-write-wins: account data has no conditional write.
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
        // Already as requested: success.
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

/// Read one policy room and answer with its rules.
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
                // Whether the account may publish here, so the UI only offers what can
                // work. From the SDK; false when membership cannot be read.
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
                // Bounded reads say so.
                "truncated": truncated,
                "rules": rows,
            }),
        );
    });
    Ok(())
}

/// Read every subscribed list and answer whether any covers `entity`,
/// together with the subscription list, so "on a list you follow" and "you
/// follow no lists" come from one consistent answer.
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

/// Subscribe to or unsubscribe from a policy room, and answer with the list.
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

/// Answer with the current subscription list, without changing it.
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

        // `*` may match nothing, at either end or in the middle.
        assert!(glob_matches("*", ""));
        assert!(glob_matches("a*b", "ab"));
        assert!(glob_matches("**", "anything"));

        // Regex metacharacters are literal: `.` and `+` occur in localparts.
        assert!(glob_matches("@a.b:example.org", "@a.b:example.org"));
        assert!(!glob_matches("@a.b:example.org", "@axb:example.org"));
        assert!(!glob_matches("@a+:example.org", "@aaa:example.org"));

        // Backtracking, which a naive left-to-right loop gets wrong.
        assert!(glob_matches("*abc", "zzabc"));
        assert!(glob_matches("*a*b", "xaybzb"));
        assert!(!glob_matches("*abc", "zzabcd"));
    }

    // A literal `*` in the value must not consume the pattern's wildcard;
    // otherwise a rule under-matches.
    #[test]
    fn a_literal_star_in_the_value_does_not_eat_the_patterns_wildcard() {
        assert!(glob_matches("*x", "*yx"));
        assert!(glob_matches("*", "***"));
        assert!(glob_matches("a*c", "a*b*c"));
        // A `*` in the pattern is still a wildcard.
        assert!(glob_matches("*", "@a:example.org"));
        assert!(!glob_matches("a*c", "abd"));
    }

    // The entity is attacker-written and matched across up to 32 x 2000 rules,
    // so it is bounded.
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

        // At the bound it is still a rule.
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
        // A server rule covers everyone on that server...
        assert!(find_match(&rules, "user", "@anyone:spam.example").is_some());
        assert!(find_match(&rules, "user", "@anyone:other.example").is_none());
        // ...and rooms on it.
        assert!(find_match(&rules, "room", "!abc:spam.example").is_some());
    }

    #[test]
    fn a_rule_that_does_not_recommend_a_ban_matches_nothing() {
        // Other recommendations are not bans and must not be acted on as such.
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
        // ruma parses the legacy value as `_Custom`; comparing the enum would read a
        // real ban list as empty, hence the string.
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
        // Removal is an empty content object; reading it as a rule with an empty
        // entity would be dangerous under a careless matcher.
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
        // Writing Mjolnir's key lets Mjolnir replace the same rule.
        assert_eq!(state_key_for("@a:example.org"), "rule:@a:example.org");
    }
}
