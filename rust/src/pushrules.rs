//! The account's push-rule settings page: master mute, the default alert
//! level for direct messages and for rooms, mention levels, keywords, and a
//! few event rules.
//!
//! Matrix behaviour stays in matrix-sdk. Where `NotificationSettings` has an
//! API (default room modes, enabling a rule, adding and removing a keyword)
//! it is used. It has none for the actions of a content or override rule, so
//! a level (Off / Notify / Loud) is applied with the typed
//! `set_pushrule_actions` request; no push-rule JSON is written by hand.
//!
//! Reading and writing are separate: `summarize` turns a `Ruleset` into the
//! snapshot C++ renders, and `plan` turns one UI command into steps. Both are
//! pure so tests need no server. A snapshot is read after every write with
//! `Account::push_rules`, but that reads the LOCAL account-data store, not the
//! server: it only catches up with a write when the next sync delivers it
//! (which also raises `push_rules_changed`). The C++ side therefore keeps the
//! user's choice on screen until a snapshot reflects it.
//!
//! Only rule ids, keywords the user typed and booleans cross the FFI. Keywords
//! are never logged.

use std::sync::Arc;

use matrix_sdk::{
    notification_settings::{IsEncrypted, IsOneToOne, RoomNotificationMode},
    ruma::{
        api::client::push::{delete_pushrule, set_pushrule_actions},
        push::{
            Action, ConditionalPushRule, HighlightTweakValue, PatternedPushRule,
            PredefinedContentRuleId, PredefinedOverrideRuleId, PredefinedUnderrideRuleId,
            RuleKind, Ruleset, SoundTweakValue, Tweak,
        },
    },
};
use serde_json::{json, Value};

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, notification_settings_handle, RustClient};

/// A keyword longer than this is refused; a rule id travels in a URL path.
const MAX_KEYWORD_CHARS: usize = 120;
/// Keywords listed in a snapshot.
const MAX_KEYWORDS: usize = 200;

/// What a mention, keyword or event rule does, as the page shows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Level {
    Off,
    Notify,
    Loud,
}

impl Level {
    fn as_str(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Notify => "notify",
            Self::Loud => "loud",
        }
    }

    fn parse(value: &str) -> Option<Self> {
        match value {
            "off" => Some(Self::Off),
            "notify" => Some(Self::Notify),
            "loud" => Some(Self::Loud),
            _ => None,
        }
    }
}

fn has_notify(actions: &[Action]) -> bool {
    actions.iter().any(Action::should_notify)
}

fn has_sound(actions: &[Action]) -> bool {
    actions
        .iter()
        .any(|action| matches!(action, Action::SetTweak(Tweak::Sound(_))))
}

/// A rule's level: disabled, or without a notify action, is Off; a sound
/// tweak makes it Loud.
pub(crate) fn level_of(enabled: bool, actions: &[Action]) -> Level {
    if !enabled || !has_notify(actions) {
        Level::Off
    } else if has_sound(actions) {
        Level::Loud
    } else {
        Level::Notify
    }
}

/// The actions that make a rule highlight at `level`. Off has none: a
/// disabled rule keeps its actions, so Off is applied as "disable".
pub(crate) fn actions_for(level: Level) -> Vec<Action> {
    let highlight = Action::SetTweak(Tweak::Highlight(HighlightTweakValue::Yes));
    match level {
        Level::Off => Vec::new(),
        Level::Notify => vec![Action::Notify, highlight],
        Level::Loud => vec![
            Action::Notify,
            Action::SetTweak(Tweak::Sound(SoundTweakValue::Default)),
            highlight,
        ],
    }
}

fn override_rule<'a>(rules: &'a Ruleset, id: &str) -> Option<&'a ConditionalPushRule> {
    rules.override_.iter().find(|rule| rule.rule_id == id)
}

fn underride_rule<'a>(rules: &'a Ruleset, id: &str) -> Option<&'a ConditionalPushRule> {
    rules.underride.iter().find(|rule| rule.rule_id == id)
}

fn content_rule<'a>(rules: &'a Ruleset, id: &str) -> Option<&'a PatternedPushRule> {
    rules.content.iter().find(|rule| rule.rule_id == id)
}

/// One mention row: where it lives and which rule id carries it.
#[derive(Clone, Debug, PartialEq, Eq)]
struct MentionRule {
    kind: RuleKind,
    id: String,
}

fn mention_rule(rules: &Ruleset, key: &str) -> Option<MentionRule> {
    #[allow(deprecated)]
    let candidates: Vec<(RuleKind, String)> = match key {
        "user_id" => vec![(
            RuleKind::Override,
            PredefinedOverrideRuleId::IsUserMention.as_str().to_owned(),
        )],
        "display_name" => vec![(
            RuleKind::Override,
            PredefinedOverrideRuleId::ContainsDisplayName.as_str().to_owned(),
        )],
        "username" => vec![(
            RuleKind::Content,
            PredefinedContentRuleId::ContainsUserName.as_str().to_owned(),
        )],
        // Servers without the modern rule still carry the legacy `@room` one.
        "room" => vec![
            (
                RuleKind::Override,
                PredefinedOverrideRuleId::IsRoomMention.as_str().to_owned(),
            ),
            (
                RuleKind::Override,
                PredefinedOverrideRuleId::RoomNotif.as_str().to_owned(),
            ),
        ],
        _ => return None,
    };
    candidates.into_iter().find_map(|(kind, id)| {
        let present = match kind {
            RuleKind::Content => content_rule(rules, &id).is_some(),
            _ => override_rule(rules, &id).is_some(),
        };
        present.then_some(MentionRule { kind, id })
    })
}

fn rule_state(rules: &Ruleset, kind: &RuleKind, id: &str) -> Option<(bool, Vec<Action>)> {
    match kind {
        RuleKind::Content => content_rule(rules, id).map(|r| (r.enabled, r.actions.clone())),
        RuleKind::Override => override_rule(rules, id).map(|r| (r.enabled, r.actions.clone())),
        RuleKind::Underride => underride_rule(rules, id).map(|r| (r.enabled, r.actions.clone())),
        _ => None,
    }
}

/// `{ present, level }` for one mention or event row. `present` false means
/// the server's ruleset has no such rule and the page hides the row.
fn row(rules: &Ruleset, rule: Option<MentionRule>) -> Value {
    match rule.and_then(|r| rule_state(rules, &r.kind, &r.id)) {
        Some((enabled, actions)) => json!({
            "present": true,
            "level": level_of(enabled, &actions).as_str(),
        }),
        None => json!({ "present": false, "level": "off" }),
    }
}

fn is_all(rules: &Ruleset, id: PredefinedUnderrideRuleId) -> Option<bool> {
    underride_rule(rules, id.as_str()).map(|r| r.enabled && has_notify(&r.actions))
}

/// `{ level: "all"|"mentions", drift }` for one default (DM or room). The two
/// rules, plain and encrypted, are meant to agree; `drift` says they do not,
/// so the page can say so and a write repairs both.
fn default_level(
    rules: &Ruleset,
    plain: PredefinedUnderrideRuleId,
    encrypted: PredefinedUnderrideRuleId,
) -> Value {
    let plain = is_all(rules, plain);
    let encrypted = is_all(rules, encrypted);
    // A rule the server lacks counts as the other one's value.
    let (a, b) = match (plain, encrypted) {
        (Some(a), Some(b)) => (a, b),
        (Some(a), None) => (a, a),
        (None, Some(b)) => (b, b),
        (None, None) => (false, false),
    };
    json!({
        "level": if a { "all" } else { "mentions" },
        "encrypted_level": if b { "all" } else { "mentions" },
        "drift": a != b,
        "present": plain.is_some() || encrypted.is_some(),
    })
}

fn event_row(rules: &Ruleset, key: &str) -> Value {
    match key {
        // Reactions and invites notify when their rule carries a notify action.
        "reactions" => row(
            rules,
            override_rule(rules, PredefinedOverrideRuleId::Reaction.as_str()).map(|_| {
                MentionRule {
                    kind: RuleKind::Override,
                    id: PredefinedOverrideRuleId::Reaction.as_str().to_owned(),
                }
            }),
        ),
        "invites" => row(
            rules,
            override_rule(rules, PredefinedOverrideRuleId::InviteForMe.as_str()).map(|_| {
                MentionRule {
                    kind: RuleKind::Override,
                    id: PredefinedOverrideRuleId::InviteForMe.as_str().to_owned(),
                }
            }),
        ),
        // `.m.rule.suppress_notices` is a SUPPRESSING rule: enabled with no
        // notify action silences bot notices. The page shows what the user
        // gets, so "notify" means the suppressor is not in force.
        "notices" => match override_rule(rules, PredefinedOverrideRuleId::SuppressNotices.as_str())
        {
            Some(rule) => {
                let silenced = rule.enabled && !has_notify(&rule.actions);
                json!({ "present": true, "level": if silenced { "off" } else { "notify" } })
            }
            None => json!({ "present": false, "level": "off" }),
        },
        _ => json!({ "present": false, "level": "off" }),
    }
}

/// Keywords the user owns: non-default content rules, grouped by pattern.
/// Several clients write one rule per keyword; if two rules share a pattern
/// the highest level wins, as in Sable.
fn keywords(rules: &Ruleset) -> Vec<Value> {
    let mut out: Vec<(String, Level)> = Vec::new();
    for rule in rules.content.iter().filter(|rule| !rule.default) {
        let level = level_of(rule.enabled, &rule.actions);
        match out.iter_mut().find(|(pattern, _)| *pattern == rule.pattern) {
            Some((_, existing)) => {
                let rank = |l: Level| match l {
                    Level::Off => 0,
                    Level::Notify => 1,
                    Level::Loud => 2,
                };
                if rank(level) > rank(*existing) {
                    *existing = level;
                }
            }
            None => out.push((rule.pattern.clone(), level)),
        }
    }
    out.truncate(MAX_KEYWORDS);
    out.into_iter()
        .map(|(keyword, level)| json!({ "keyword": keyword, "level": level.as_str() }))
        .collect()
}

/// The whole page's state.
pub(crate) fn summarize(rules: &Ruleset) -> Value {
    json!({
        "master_mute": override_rule(rules, PredefinedOverrideRuleId::Master.as_str())
            .is_some_and(|rule| rule.enabled),
        "dm": default_level(
            rules,
            PredefinedUnderrideRuleId::RoomOneToOne,
            PredefinedUnderrideRuleId::EncryptedRoomOneToOne,
        ),
        "rooms": default_level(
            rules,
            PredefinedUnderrideRuleId::Message,
            PredefinedUnderrideRuleId::Encrypted,
        ),
        "mentions": {
            "user_id": row(rules, mention_rule(rules, "user_id")),
            "display_name": row(rules, mention_rule(rules, "display_name")),
            "username": row(rules, mention_rule(rules, "username")),
            "room": row(rules, mention_rule(rules, "room")),
        },
        "events": {
            "reactions": event_row(rules, "reactions"),
            "invites": event_row(rules, "invites"),
            "notices": event_row(rules, "notices"),
        },
        "keywords": keywords(rules),
    })
}

/// One server write.
#[derive(Clone, Debug)]
pub(crate) enum Step {
    Enabled { kind: RuleKind, id: String, enabled: bool },
    Actions { kind: RuleKind, id: String, actions: Vec<Action> },
    /// `SetDefaultRoomNotificationMode` for BOTH the plain and the encrypted
    /// variant, which is what keeps them in agreement.
    DefaultMode { one_to_one: bool, all_messages: bool },
    AddKeyword(String),
    RemoveKeyword(String),
    /// Delete one content rule by its exact rule id; used for keywords another
    /// client created that `clean_keyword` would refuse.
    DeleteContentRule(String),
}

/// Why a command cannot be applied; sent to C++ as a category.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum PlanError {
    /// The command is malformed.
    Invalid,
    /// The server's ruleset lacks the rule this command writes.
    Unsupported,
}

fn set_level(kind: RuleKind, id: String, level: Level) -> Vec<Step> {
    match level {
        Level::Off => vec![Step::Enabled { kind, id, enabled: false }],
        _ => vec![
            // Actions first, so the rule never fires with stale actions.
            Step::Actions { kind: kind.clone(), id: id.clone(), actions: actions_for(level) },
            Step::Enabled { kind, id, enabled: true },
        ],
    }
}

/// Reject keywords a rule id cannot carry. A leading `.` is reserved for
/// predefined rules.
pub(crate) fn clean_keyword(raw: &str) -> Result<String, PlanError> {
    let keyword = raw.trim();
    if keyword.is_empty()
        || keyword.chars().count() > MAX_KEYWORD_CHARS
        || keyword.starts_with('.')
        || keyword.contains('/')
        || keyword.chars().any(char::is_control)
    {
        return Err(PlanError::Invalid);
    }
    Ok(keyword.to_owned())
}

/// Turn one command into steps. `kind` is the command family, `key` what it
/// targets and `value` the new setting.
pub(crate) fn plan(
    rules: &Ruleset,
    kind: &str,
    key: &str,
    value: &str,
) -> Result<Vec<Step>, PlanError> {
    match kind {
        "master_mute" => {
            let on = match value {
                "on" => true,
                "off" => false,
                _ => return Err(PlanError::Invalid),
            };
            override_rule(rules, PredefinedOverrideRuleId::Master.as_str())
                .ok_or(PlanError::Unsupported)?;
            Ok(vec![Step::Enabled {
                kind: RuleKind::Override,
                id: PredefinedOverrideRuleId::Master.as_str().to_owned(),
                enabled: on,
            }])
        }
        "default_level" => {
            let one_to_one = match key {
                "dm" => true,
                "rooms" => false,
                _ => return Err(PlanError::Invalid),
            };
            let all_messages = match value {
                "all" => true,
                "mentions" => false,
                _ => return Err(PlanError::Invalid),
            };
            Ok(vec![Step::DefaultMode { one_to_one, all_messages }])
        }
        "mention" => {
            let level = Level::parse(value).ok_or(PlanError::Invalid)?;
            let rule = mention_rule(rules, key).ok_or(
                if matches!(key, "user_id" | "display_name" | "username" | "room") {
                    PlanError::Unsupported
                } else {
                    PlanError::Invalid
                },
            )?;
            Ok(set_level(rule.kind, rule.id, level))
        }
        "event" => match key {
            "reactions" | "invites" => {
                let level = match Level::parse(value).ok_or(PlanError::Invalid)? {
                    Level::Loud => Level::Notify,
                    other => other,
                };
                let id = if key == "reactions" {
                    PredefinedOverrideRuleId::Reaction
                } else {
                    PredefinedOverrideRuleId::InviteForMe
                }
                .as_str()
                .to_owned();
                override_rule(rules, &id).ok_or(PlanError::Unsupported)?;
                let actions = match level {
                    Level::Off => Vec::new(),
                    _ if key == "invites" => vec![
                        Action::Notify,
                        Action::SetTweak(Tweak::Sound(SoundTweakValue::Default)),
                    ],
                    _ => vec![Action::Notify],
                };
                // An empty action list is how a rule says "do not notify" and
                // keeps it enabled, so nothing falls through to a later rule.
                Ok(vec![
                    Step::Actions { kind: RuleKind::Override, id: id.clone(), actions },
                    Step::Enabled { kind: RuleKind::Override, id, enabled: true },
                ])
            }
            "notices" => {
                let id = PredefinedOverrideRuleId::SuppressNotices.as_str().to_owned();
                override_rule(rules, &id).ok_or(PlanError::Unsupported)?;
                match value {
                    // Bot notices then follow the room's own level.
                    "notify" => Ok(vec![Step::Enabled {
                        kind: RuleKind::Override,
                        id,
                        enabled: false,
                    }]),
                    "off" => Ok(vec![
                        Step::Actions {
                            kind: RuleKind::Override,
                            id: id.clone(),
                            actions: Vec::new(),
                        },
                        Step::Enabled { kind: RuleKind::Override, id, enabled: true },
                    ]),
                    _ => Err(PlanError::Invalid),
                }
            }
            _ => Err(PlanError::Invalid),
        },
        "keyword_set" => {
            // Add the keyword if it is new, then set its level.
            let level = Level::parse(value).ok_or(PlanError::Invalid)?;
            // A keyword that already exists is addressed by its exact pattern,
            // so one another client wrote (a slash, a leading dot, padding)
            // can still be re-levelled. Only a NEW keyword must be clean.
            let exact: Vec<String> = rules
                .content
                .iter()
                .filter(|rule| !rule.default && rule.pattern == key)
                .map(|rule| rule.rule_id.clone())
                .collect();
            if !exact.is_empty() {
                let mut steps = Vec::new();
                for id in exact {
                    steps.extend(set_level(RuleKind::Content, id, level));
                }
                return Ok(steps);
            }
            let keyword = clean_keyword(key)?;
            let existing: Vec<String> = rules
                .content
                .iter()
                .filter(|rule| !rule.default && rule.pattern == keyword)
                .map(|rule| rule.rule_id.clone())
                .collect();
            let mut steps = Vec::new();
            let ids = if existing.is_empty() {
                steps.push(Step::AddKeyword(keyword.clone()));
                // matrix-sdk names the new rule after its keyword.
                vec![keyword]
            } else {
                existing
            };
            for id in ids {
                steps.extend(set_level(RuleKind::Content, id, level));
            }
            Ok(steps)
        }
        "keyword_remove" => {
            let exact: Vec<String> = rules
                .content
                .iter()
                .filter(|rule| !rule.default && rule.pattern == key)
                .map(|rule| rule.rule_id.clone())
                .collect();
            if !exact.is_empty() {
                return Ok(exact.into_iter().map(Step::DeleteContentRule).collect());
            }
            let keyword = clean_keyword(key)?;
            Ok(vec![Step::RemoveKeyword(keyword)])
        }
        _ => Err(PlanError::Invalid),
    }
}

async fn run_step(
    client: &matrix_sdk::Client,
    settings: &matrix_sdk::notification_settings::NotificationSettings,
    step: Step,
) -> Result<(), String> {
    match step {
        Step::Enabled { kind, id, enabled } => settings
            .set_push_rule_enabled(kind, id, enabled)
            .await
            .map_err(|e| e.to_string()),
        Step::Actions { kind, id, actions } => client
            .send(set_pushrule_actions::v3::Request::new(kind, id, actions))
            .await
            .map(|_| ())
            .map_err(|e| e.to_string()),
        Step::DefaultMode { one_to_one, all_messages } => {
            let mode = if all_messages {
                RoomNotificationMode::AllMessages
            } else {
                RoomNotificationMode::MentionsAndKeywordsOnly
            };
            let one_to_one = if one_to_one { IsOneToOne::Yes } else { IsOneToOne::No };
            // Both variants, always, so plain and encrypted never disagree.
            for encrypted in [IsEncrypted::No, IsEncrypted::Yes] {
                settings
                    .set_default_room_notification_mode(encrypted, one_to_one, mode)
                    .await
                    .map_err(|e| e.to_string())?;
            }
            Ok(())
        }
        Step::AddKeyword(keyword) => {
            settings.add_keyword(keyword).await.map_err(|e| e.to_string())
        }
        Step::RemoveKeyword(keyword) => {
            settings.remove_keyword(&keyword).await.map_err(|e| e.to_string())
        }
        Step::DeleteContentRule(id) => client
            .send(delete_pushrule::v3::Request::new(RuleKind::Content, id))
            .await
            .map(|_| ())
            .map_err(|e| e.to_string()),
    }
}

fn snapshot_event(op_id: u64, lifecycle: u64, rules: &Ruleset) -> Value {
    let mut event = summarize(rules);
    if let Some(object) = event.as_object_mut() {
        object.insert("type".to_owned(), "push_rules_snapshot".into());
        object.insert("op_id".to_owned(), op_id.into());
        object.insert("lifecycle".to_owned(), lifecycle.into());
        object.insert("ok".to_owned(), true.into());
    }
    event
}

fn failure_event(op_id: u64, lifecycle: u64, category: &str) -> Value {
    json!({
        "type": "push_rules_snapshot",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "ok": false,
        "category": category,
    })
}

/// Fetch the account's push rules from the server. Result event
/// `push_rules_snapshot`.
pub(crate) fn request_snapshot(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.account().push_rules().await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let event = match result {
            Ok(rules) => snapshot_event(op_id, lifecycle, &rules),
            Err(err) => failure_event(op_id, lifecycle, classify_room_error(&err.to_string())),
        };
        enqueue(&events, event);
    });
    Ok(())
}

/// Apply one command, then report. Result events: `push_rules_write
/// { op_id, lifecycle, kind, key, ok, category }` followed, on success, by a
/// fresh `push_rules_snapshot` with the same op id.
pub(crate) fn write_rule(
    bridge: &RustClient,
    kind: String,
    key: String,
    value: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let slot = Arc::clone(&bridge.notification_settings);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let report = |ok: bool, category: &str| {
            enqueue(&events, json!({
                "type": "push_rules_write",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "kind": kind,
                "ok": ok,
                "category": category,
            }));
        };
        // Plan against the account's rules as last synced (a local store read,
        // not a server request).
        let rules = match client.account().push_rules().await {
            Ok(rules) => rules,
            Err(err) => {
                if timelines.lifecycle_current(lifecycle) {
                    report(false, classify_room_error(&err.to_string()));
                }
                return;
            }
        };
        let steps = match plan(&rules, &kind, &key, &value) {
            Ok(steps) => steps,
            Err(PlanError::Invalid) => {
                report(false, "invalid");
                return;
            }
            Err(PlanError::Unsupported) => {
                report(false, "unsupported");
                return;
            }
        };
        let settings = notification_settings_handle(&slot, &client).await;
        for step in steps {
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            if let Err(err) = run_step(&client, &settings, step).await {
                if timelines.lifecycle_current(lifecycle) {
                    report(false, classify_room_error(&err));
                }
                return;
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        report(true, "");
        let event = match client.account().push_rules().await {
            Ok(rules) => snapshot_event(op_id, lifecycle, &rules),
            Err(err) => failure_event(op_id, lifecycle, classify_room_error(&err.to_string())),
        };
        enqueue(&events, event);
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::push::{ConditionalPushRuleInit, PatternedPushRuleInit};

    /// ruma's `server_default` omits the deprecated rules (display name,
    /// username, legacy `@room`), but real homeservers such as Synapse still
    /// ship them, and the page keys on them. Add them as a server would.
    fn rules() -> Ruleset {
        let mut rules = Ruleset::server_default(matrix_sdk::ruma::user_id!("@me:example.org"));
        for id in [".m.rule.contains_display_name", ".m.rule.roomnotif"] {
            rules.override_.insert(ConditionalPushRule::from(ConditionalPushRuleInit {
                actions: actions_for(Level::Loud),
                default: true,
                enabled: true,
                rule_id: id.into(),
                conditions: vec![],
            }));
        }
        rules.content.insert(PatternedPushRule::from(PatternedPushRuleInit {
            actions: actions_for(Level::Loud),
            default: true,
            enabled: true,
            rule_id: ".m.rule.contains_user_name".into(),
            pattern: "me".into(),
        }));
        rules
    }

    fn loud() -> Vec<Action> {
        actions_for(Level::Loud)
    }

    #[test]
    fn level_is_derived_from_enabled_and_actions() {
        assert_eq!(level_of(false, &loud()), Level::Off);
        assert_eq!(level_of(true, &[]), Level::Off);
        assert_eq!(level_of(true, &actions_for(Level::Notify)), Level::Notify);
        assert_eq!(level_of(true, &loud()), Level::Loud);
        // A level survives a round trip through its actions.
        for level in [Level::Notify, Level::Loud] {
            assert_eq!(level_of(true, &actions_for(level)), level);
        }
    }

    #[test]
    fn server_default_summary_matches_the_spec_defaults() {
        let summary = summarize(&rules());
        assert_eq!(summary["master_mute"], false);
        // Spec defaults: every message notifies, in DMs and in rooms.
        assert_eq!(summary["dm"]["level"], "all");
        assert_eq!(summary["dm"]["drift"], false);
        assert_eq!(summary["rooms"]["level"], "all");
        assert_eq!(summary["rooms"]["drift"], false);
        assert_eq!(summary["mentions"]["user_id"]["present"], true);
        assert_eq!(summary["mentions"]["user_id"]["level"], "loud");
        assert_eq!(summary["mentions"]["room"]["present"], true);
        // Reactions are silenced out of the box; bot notices are suppressed.
        assert_eq!(summary["events"]["reactions"]["level"], "off");
        assert_eq!(summary["events"]["notices"]["level"], "off");
        assert_eq!(summary["events"]["invites"]["level"], "loud");
        assert_eq!(summary["keywords"].as_array().map(Vec::len), Some(0));
    }

    #[test]
    fn plain_and_encrypted_variants_that_disagree_are_flagged_as_drift() {
        let mut rules = rules();
        // Plain rooms are set to mentions, encrypted rooms still notify.
        let plain = rules
            .underride
            .iter()
            .find(|r| r.rule_id == PredefinedUnderrideRuleId::Message.as_str())
            .cloned()
            .expect("message rule");
        let mut changed = plain.clone();
        changed.actions = vec![];
        rules.underride.replace(changed);
        let summary = summarize(&rules);
        assert_eq!(summary["rooms"]["level"], "mentions");
        assert_eq!(summary["rooms"]["encrypted_level"], "all");
        assert_eq!(summary["rooms"]["drift"], true);
    }

    #[test]
    fn a_default_level_write_covers_both_variants_in_one_step() {
        let steps = plan(&rules(), "default_level", "rooms", "all").unwrap();
        assert_eq!(steps.len(), 1);
        assert!(matches!(
            steps[0],
            Step::DefaultMode { one_to_one: false, all_messages: true }
        ));
        let steps = plan(&rules(), "default_level", "dm", "mentions").unwrap();
        assert!(matches!(
            steps[0],
            Step::DefaultMode { one_to_one: true, all_messages: false }
        ));
        assert_eq!(
            plan(&rules(), "default_level", "dm", "loud").unwrap_err(),
            PlanError::Invalid
        );
    }

    #[test]
    fn master_mute_toggles_the_master_override() {
        let steps = plan(&rules(), "master_mute", "", "on").unwrap();
        match &steps[0] {
            Step::Enabled { kind, id, enabled } => {
                assert_eq!(*kind, RuleKind::Override);
                assert_eq!(id, ".m.rule.master");
                assert!(*enabled);
            }
            other => panic!("unexpected step {other:?}"),
        }
        assert!(plan(&rules(), "master_mute", "", "maybe").is_err());
    }

    #[test]
    fn mention_levels_set_actions_before_enabling_and_off_only_disables() {
        let loud_steps = plan(&rules(), "mention", "display_name", "loud").unwrap();
        assert!(matches!(loud_steps[0], Step::Actions { .. }));
        assert!(matches!(loud_steps[1], Step::Enabled { enabled: true, .. }));
        let off = plan(&rules(), "mention", "display_name", "off").unwrap();
        assert_eq!(off.len(), 1);
        assert!(matches!(off[0], Step::Enabled { enabled: false, .. }));
        // The username rule is a CONTENT rule.
        match &plan(&rules(), "mention", "username", "notify").unwrap()[0] {
            Step::Actions { kind, id, .. } => {
                assert_eq!(*kind, RuleKind::Content);
                assert_eq!(id, ".m.rule.contains_user_name");
            }
            other => panic!("unexpected step {other:?}"),
        }
        assert_eq!(
            plan(&rules(), "mention", "nonsense", "off").unwrap_err(),
            PlanError::Invalid
        );
    }

    #[test]
    fn the_room_mention_falls_back_to_the_legacy_rule() {
        let mut rules = rules();
        let modern = PredefinedOverrideRuleId::IsRoomMention.as_str();
        let found = override_rule(&rules, modern).cloned().expect("modern rule");
        rules.override_.shift_remove(&found);
        match &plan(&rules, "mention", "room", "off").unwrap()[0] {
            Step::Enabled { id, .. } => assert_eq!(id, ".m.rule.roomnotif"),
            other => panic!("unexpected step {other:?}"),
        }
        // And with neither rule, the row is unsupported rather than invented.
        let legacy = override_rule(&rules, ".m.rule.roomnotif").cloned().expect("legacy");
        rules.override_.shift_remove(&legacy);
        assert_eq!(
            plan(&rules, "mention", "room", "off").unwrap_err(),
            PlanError::Unsupported
        );
        assert_eq!(summarize(&rules)["mentions"]["room"]["present"], false);
    }

    #[test]
    fn bot_notices_are_inverted_because_the_rule_is_a_suppressor() {
        // Default: suppressor in force, so notices are silenced.
        assert_eq!(summarize(&rules())["events"]["notices"]["level"], "off");
        // "Notify" removes the suppressor by disabling it.
        let steps = plan(&rules(), "event", "notices", "notify").unwrap();
        assert!(matches!(steps[0], Step::Enabled { enabled: false, .. }));
        let mut disabled = rules();
        let rule = override_rule(&disabled, ".m.rule.suppress_notices").cloned().unwrap();
        let mut changed = rule.clone();
        changed.enabled = false;
        disabled.override_.replace(changed);
        assert_eq!(summarize(&disabled)["events"]["notices"]["level"], "notify");
    }

    #[test]
    fn reactions_off_keeps_the_rule_enabled_with_no_actions() {
        let steps = plan(&rules(), "event", "reactions", "off").unwrap();
        match (&steps[0], &steps[1]) {
            (Step::Actions { actions, .. }, Step::Enabled { enabled, .. }) => {
                assert!(actions.is_empty());
                assert!(*enabled);
            }
            other => panic!("unexpected steps {other:?}"),
        }
    }

    // A keyword another client created that clean_keyword() would refuse must
    // still be removable and re-levelled, by its exact pattern and rule id.
    #[test]
    fn a_foreign_keyword_the_validator_refuses_can_still_be_changed() {
        let mut with = rules();
        with.content.insert(PatternedPushRule::from(PatternedPushRuleInit {
            actions: loud(),
            default: false,
            enabled: true,
            rule_id: "team-alert".into(),
            pattern: "a/b".into(),
        }));
        // Adding the same text fresh is still refused...
        assert_eq!(
            plan(&rules(), "keyword_set", "a/b", "off").unwrap_err(),
            PlanError::Invalid
        );
        // ...but the existing rule is addressed by its id.
        let steps = plan(&with, "keyword_set", "a/b", "off").unwrap();
        assert!(matches!(&steps[0],
            Step::Enabled { kind: RuleKind::Content, id, enabled: false } if id == "team-alert"));
        let steps = plan(&with, "keyword_remove", "a/b", "").unwrap();
        assert!(matches!(&steps[0], Step::DeleteContentRule(id) if id == "team-alert"));
    }

    #[test]
    fn keywords_are_validated_added_and_grouped() {
        assert_eq!(clean_keyword("  lightning  ").unwrap(), "lightning");
        for bad in ["", "   ", ".m.rule.master", "a/b", "a\nb"] {
            assert_eq!(clean_keyword(bad).unwrap_err(), PlanError::Invalid, "{bad:?}");
        }
        assert!(clean_keyword(&"x".repeat(MAX_KEYWORD_CHARS + 1)).is_err());

        // A new keyword is added, then given its level.
        let steps = plan(&rules(), "keyword_set", "lightning", "loud").unwrap();
        assert!(matches!(&steps[0], Step::AddKeyword(k) if k == "lightning"));
        assert!(matches!(&steps[1], Step::Actions { kind: RuleKind::Content, .. }));

        // An existing keyword is not added twice.
        let mut with = rules();
        with.content.insert(PatternedPushRule::from(PatternedPushRuleInit {
            actions: loud(),
            default: false,
            enabled: true,
            rule_id: "lightning".into(),
            pattern: "lightning".into(),
        }));
        let steps = plan(&with, "keyword_set", "lightning", "off").unwrap();
        assert_eq!(steps.len(), 1);
        assert!(matches!(steps[0], Step::Enabled { enabled: false, .. }));
        let list = summarize(&with)["keywords"].clone();
        assert_eq!(list[0]["keyword"], "lightning");
        assert_eq!(list[0]["level"], "loud");
        // The predefined username rule is a mention row, never a keyword.
        assert_eq!(list.as_array().map(Vec::len), Some(1));
    }

    #[test]
    fn a_custom_conditional_rule_does_not_leak_into_the_summary() {
        // A custom conditional rule is how mutes are stored; they are per-room
        // and belong to the room's own mode, not to this page.
        let mut with = rules();
        with.override_.insert(ConditionalPushRule::from(ConditionalPushRuleInit {
            actions: vec![],
            default: false,
            enabled: true,
            rule_id: "!r:example.org".into(),
            conditions: vec![],
        }));
        assert_eq!(summarize(&with)["keywords"].as_array().map(Vec::len), Some(0));
        assert_eq!(summarize(&with)["master_mute"], false);
    }
}
