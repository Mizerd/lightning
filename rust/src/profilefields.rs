//! Small extended profile fields (MSC4133): time zone, pronouns, and the
//! colours and "animal identity" Sable writes.
//!
//!   * Time zone (MSC4175): read `m.tz`, then `us.cloke.msc4175.tz`; write both.
//!   * Pronouns (MSC4247): read `m.pronouns`, then `io.fsky.nyx.pronouns`;
//!     write the unstable key, the one deployed clients read.
//!   * Read only: `eu.she-a.color` (MSC4522 name colours),
//!     `chat.commet.profile_color_scheme`, `pet.plz.{me,my,gib}`.
//!
//! Every value is remote text: it is bounded, stripped of control and bidi
//! characters, and shape-checked here. Whether a zone name is a real IANA
//! zone is decided in C++, which owns the zone database.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::ruma::{OwnedUserId, UserId};
use serde_json::{json, Value};

use crate::banner::{is_unsupported, profile_field};
use crate::namecolor::normalized as hex_color;
use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

const TZ_FIELD: &str = "m.tz";
const TZ_FIELD_UNSTABLE: &str = "us.cloke.msc4175.tz";
const PRONOUNS_FIELD: &str = "m.pronouns";
const PRONOUNS_FIELD_UNSTABLE: &str = "io.fsky.nyx.pronouns";
const NAME_COLORS_FIELD: &str = "eu.she-a.color";
const NAME_COLOR_DARK_LEGACY: &str = "moe.sable.app.name_color_dark_theme";
const NAME_COLOR_LIGHT_LEGACY: &str = "moe.sable.app.name_color_light_theme";
const CARD_SCHEME_FIELD: &str = "chat.commet.profile_color_scheme";
const ANIMAL_IS_FIELD: &str = "pet.plz.me";
const ANIMAL_HAS_FIELD: &str = "pet.plz.my";
const ANIMAL_NEED_FIELD: &str = "pet.plz.gib";
const IS_CAT_LEGACY: &str = "kitty.meow.is_cat";
const HAS_CATS_LEGACY: &str = "kitty.meow.has_cats";

const TIMEOUT: Duration = Duration::from_secs(15);

const MAX_TZ_CHARS: usize = 64;
// Sable truncates a summary to 16 characters; longer ones would not round-trip.
const MAX_PRONOUN_CHARS: usize = 16;
const MAX_PRONOUN_SETS: usize = 4;
const MAX_ANIMAL_CHARS: usize = 24;

fn clean_line(text: &str, max_chars: usize) -> String {
    let visible: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .filter(|c| {
            !matches!(
                c,
                '\u{061C}'
                    | '\u{200B}'..='\u{200F}'
                    | '\u{202A}'..='\u{202E}'
                    | '\u{2060}'
                    | '\u{2066}'..='\u{2069}'
                    | '\u{FEFF}'
            )
        })
        .collect();
    visible
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(max_chars)
        .collect::<String>()
        .trim_end()
        .to_owned()
}

/// The shape of an IANA zone name, nothing more. No dots: the name is later
/// looked up in a zone database that may be a directory of files.
pub(crate) fn tz_shape(value: &str) -> Option<String> {
    let name = value.trim();
    let ok = !name.is_empty()
        && name.len() <= MAX_TZ_CHARS
        && !name.starts_with('/')
        && !name.ends_with('/')
        && !name.contains("//")
        && name.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'+' | b'-' | b'/'));
    ok.then(|| name.to_owned())
}

fn language_tag(value: Option<&str>) -> String {
    let tag = value.unwrap_or_default().trim().to_ascii_lowercase();
    let ok = !tag.is_empty()
        && tag.len() <= 12
        && tag.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-');
    if ok { tag } else { "en".to_owned() }
}

/// `(language, summary)` in the owner's order of preference.
fn pronoun_sets(value: &Value) -> Vec<(String, String)> {
    let Some(entries) = value.as_array() else { return Vec::new() };
    entries
        .iter()
        .filter_map(|entry| {
            let summary = clean_line(entry.get("summary")?.as_str()?, MAX_PRONOUN_CHARS);
            (!summary.is_empty())
                .then(|| (language_tag(entry.get("language").and_then(Value::as_str)), summary))
        })
        .take(MAX_PRONOUN_SETS)
        .collect()
}

/// Sable's input form: comma separated, each optionally `language:summary`.
pub(crate) fn parse_pronouns_input(text: &str) -> Vec<(String, String)> {
    text.split(',')
        .filter_map(|part| {
            let (language, summary) = match part.split_once(':') {
                Some((language, summary)) => (language_tag(Some(language)), summary),
                None => ("en".to_owned(), part),
            };
            let summary = clean_line(summary, MAX_PRONOUN_CHARS);
            (!summary.is_empty()).then_some((language, summary))
        })
        .take(MAX_PRONOUN_SETS)
        .collect()
}

fn pronouns_input(sets: &[(String, String)]) -> String {
    sets.iter()
        .map(|(language, summary)| {
            if language == "en" { summary.clone() } else { format!("{language}:{summary}") }
        })
        .collect::<Vec<_>>()
        .join(", ")
}

fn pronouns_value(sets: &[(String, String)]) -> Value {
    Value::Array(
        sets.iter()
            .map(|(language, summary)| json!({ "summary": summary, "language": language }))
            .collect(),
    )
}

fn text_field(profile: &Value, field: &str, max_chars: usize) -> String {
    profile
        .get(field)
        .and_then(Value::as_str)
        .map(|text| clean_line(text, max_chars))
        .unwrap_or_default()
}

fn color_at(value: Option<&Value>) -> Option<String> {
    hex_color(value?.as_str()?)
}

/// What the card needs out of a whole `GET /profile/{userId}` answer. Absent
/// and unusable values are both the empty string.
pub(crate) fn fields_from_profile(profile: &Value) -> Value {
    let timezone = [TZ_FIELD, TZ_FIELD_UNSTABLE]
        .iter()
        .find_map(|field| tz_shape(profile.get(*field)?.as_str()?))
        .unwrap_or_default();

    let sets = [PRONOUNS_FIELD, PRONOUNS_FIELD_UNSTABLE]
        .iter()
        .map(|field| profile.get(*field).map(pronoun_sets).unwrap_or_default())
        .find(|sets| !sets.is_empty())
        .unwrap_or_default();
    let pronouns = sets.iter().map(|(_, summary)| summary.as_str()).collect::<Vec<_>>().join(", ");

    let colors = profile.get(NAME_COLORS_FIELD);
    let name_color_dark = color_at(colors.and_then(|c| c.get("on_dark")))
        .or_else(|| color_at(profile.get(NAME_COLOR_DARK_LEGACY)))
        .unwrap_or_default();
    let name_color_light = color_at(colors.and_then(|c| c.get("on_light")))
        .or_else(|| color_at(profile.get(NAME_COLOR_LIGHT_LEGACY)))
        .unwrap_or_default();
    let card_color = color_at(profile.get(CARD_SCHEME_FIELD).and_then(|s| s.get("color")))
        .unwrap_or_default();

    let legacy = |field: &str, word: &str| {
        (profile.get(field).and_then(Value::as_bool) == Some(true))
            .then(|| word.to_owned())
            .unwrap_or_default()
    };
    let mut animal_is = text_field(profile, ANIMAL_IS_FIELD, MAX_ANIMAL_CHARS);
    if animal_is.is_empty() {
        animal_is = legacy(IS_CAT_LEGACY, "cat");
    }
    let mut animal_has = text_field(profile, ANIMAL_HAS_FIELD, MAX_ANIMAL_CHARS);
    if animal_has.is_empty() {
        animal_has = legacy(HAS_CATS_LEGACY, "cats");
    }

    json!({
        "timezone": timezone,
        "pronouns": pronouns,
        "pronouns_input": pronouns_input(&sets),
        "name_color_dark": name_color_dark,
        "name_color_light": name_color_light,
        "card_color": card_color,
        "animal_is": animal_is,
        "animal_has": animal_has,
        "animal_need": text_field(profile, ANIMAL_NEED_FIELD, MAX_ANIMAL_CHARS),
    })
}

/// The `m.profile_fields` capability for one key. A server that does not
/// advertise the capability is not known to refuse, so the write decides.
pub(crate) fn capability_allows(capabilities: &Value, field: &str) -> bool {
    let Some(cap) = capabilities
        .get("capabilities")
        .and_then(|c| c.get("m.profile_fields"))
    else {
        return true;
    };
    if cap.get("enabled").and_then(Value::as_bool) == Some(false) {
        return false;
    }
    let lists = |key: &str| {
        cap.get(key)
            .and_then(Value::as_array)
            .map(|list| list.iter().any(|entry| entry.as_str() == Some(field)))
    };
    lists("disallowed") != Some(true) && lists("allowed") != Some(false)
}

/// The keys a write touches and the value they get; `None` deletes them.
pub(crate) fn plan_write(
    name: &str,
    value: &str,
) -> Result<(&'static [&'static str], Option<Value>), String> {
    match name {
        "timezone" => {
            let keys: &[&str] = &[TZ_FIELD, TZ_FIELD_UNSTABLE];
            if value.trim().is_empty() {
                return Ok((keys, None));
            }
            let zone = tz_shape(value).ok_or_else(|| "invalid_value".to_owned())?;
            Ok((keys, Some(Value::String(zone))))
        }
        "pronouns" => {
            let keys: &[&str] = &[PRONOUNS_FIELD_UNSTABLE];
            let sets = parse_pronouns_input(value);
            Ok((keys, (!sets.is_empty()).then(|| pronouns_value(&sets))))
        }
        _ => Err("unknown_field".to_owned()),
    }
}

/// A value is published once any key holds it, but it is only gone when no
/// key still does: a delete that leaves one behind keeps showing the old value.
pub(crate) fn write_outcome(deleting: bool, results: &[Result<(), String>]) -> Result<(), String> {
    let succeeded = results.iter().filter(|r| r.is_ok()).count();
    let done = if deleting { succeeded == results.len() } else { succeeded > 0 };
    if done {
        return Ok(());
    }
    Err(results
        .iter()
        .rev()
        .find_map(|r| r.as_ref().err().cloned())
        .unwrap_or_default())
}

pub(crate) fn fetch_profile_fields(
    bridge: &RustClient,
    op_id: u64,
    user_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId = UserId::parse(user_id.as_str())
        .map_err(|_| "invalid user id".to_owned())?
        .into();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let own = client.user_id().is_some_and(|me| me.as_str() == uid.as_str());
        let profile = match profile_field::get_all(&client, uid.as_str(), TIMEOUT).await {
            Ok(answer) if answer.status == 200 => {
                serde_json::from_str::<Value>(&answer.body).ok()
            }
            _ => None,
        };
        let mut event = json!({
            "type": "profile_fields",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "user_id": uid.to_string(),
            "ok": profile.is_some(),
            "fields": fields_from_profile(profile.as_ref().unwrap_or(&Value::Null)),
        });
        if own {
            let capabilities = match profile_field::capabilities(&client, TIMEOUT).await {
                Ok(answer) if answer.status == 200 => {
                    serde_json::from_str::<Value>(&answer.body).unwrap_or(Value::Null)
                }
                _ => Value::Null,
            };
            event["can_edit_timezone"] = json!(
                capability_allows(&capabilities, TZ_FIELD)
                    || capability_allows(&capabilities, TZ_FIELD_UNSTABLE)
            );
            event["can_edit_pronouns"] =
                json!(capability_allows(&capabilities, PRONOUNS_FIELD_UNSTABLE));
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, event);
    });
    Ok(())
}

pub(crate) fn set_own_profile_field(
    bridge: &RustClient,
    op_id: u64,
    name: String,
    value: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid = client
        .user_id()
        .map(ToOwned::to_owned)
        .ok_or_else(|| "no session".to_owned())?;
    let (keys, new_value) = plan_write(&name, &value)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut results: Vec<Result<(), String>> = Vec::new();
        for field in keys {
            let answer = match &new_value {
                Some(v) => profile_field::set_json(&client, uid.as_str(), field, v, TIMEOUT).await,
                None => profile_field::delete(&client, uid.as_str(), field, TIMEOUT).await,
            };
            results.push(match answer {
                Ok(a) if (200..300).contains(&a.status) => Ok(()),
                Ok(a) if new_value.is_none() && a.status == 404 && !is_unsupported(&a.body) => {
                    Ok(())
                }
                Ok(a) => Err(a.body),
                Err(text) => Err(text),
            });
        }
        let outcome = write_outcome(new_value.is_none(), &results);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let category = match &outcome {
            Ok(()) => "",
            Err(text) if is_unsupported(text) => "unsupported",
            Err(text) => classify_room_error(text),
        };
        enqueue(
            &events,
            json!({
                "type": "profile_field_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": outcome.is_ok(),
                "name": name,
                "category": category,
            }),
        );
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fields(body: &str) -> Value {
        fields_from_profile(&serde_json::from_str::<Value>(body).expect("json"))
    }

    #[test]
    fn the_field_names_are_the_ones_other_clients_already_use() {
        assert_eq!(TZ_FIELD, "m.tz");
        assert_eq!(TZ_FIELD_UNSTABLE, "us.cloke.msc4175.tz");
        assert_eq!(PRONOUNS_FIELD, "m.pronouns");
        assert_eq!(PRONOUNS_FIELD_UNSTABLE, "io.fsky.nyx.pronouns");
        assert_eq!(NAME_COLORS_FIELD, "eu.she-a.color");
        assert_eq!(CARD_SCHEME_FIELD, "chat.commet.profile_color_scheme");
        assert_eq!(
            [ANIMAL_IS_FIELD, ANIMAL_HAS_FIELD, ANIMAL_NEED_FIELD],
            ["pet.plz.me", "pet.plz.my", "pet.plz.gib"]
        );
    }

    #[test]
    fn the_time_zone_is_read_from_the_stable_key_and_then_the_unstable_one() {
        assert_eq!(fields(r#"{"m.tz":"Europe/Paris"}"#)["timezone"], "Europe/Paris");
        assert_eq!(
            fields(r#"{"us.cloke.msc4175.tz":"America/New_York"}"#)["timezone"],
            "America/New_York"
        );
        assert_eq!(
            fields(r#"{"m.tz":"Europe/Paris","us.cloke.msc4175.tz":"Asia/Tokyo"}"#)["timezone"],
            "Europe/Paris"
        );
        // An unusable stable value does not hide a usable unstable one.
        assert_eq!(
            fields(r#"{"m.tz":"<b>x</b>","us.cloke.msc4175.tz":"Asia/Tokyo"}"#)["timezone"],
            "Asia/Tokyo"
        );
        assert_eq!(fields(r#"{"displayname":"A"}"#)["timezone"], "");
        assert_eq!(fields(r#"{"m.tz":42}"#)["timezone"], "");
    }

    #[test]
    fn a_zone_name_that_is_not_shaped_like_one_is_dropped() {
        assert_eq!(tz_shape(" Europe/Vilnius ").as_deref(), Some("Europe/Vilnius"));
        assert_eq!(tz_shape("Etc/GMT+5").as_deref(), Some("Etc/GMT+5"));
        assert_eq!(tz_shape("America/Port-au-Prince").as_deref(), Some("America/Port-au-Prince"));
        for bad in [
            "", "../../etc/passwd", "/etc/localtime", "Europe/", "Europe//Paris",
            "Europe/Par is", "<img src=x>", "Europe/Pa\nris", "Zone.Name",
        ] {
            assert_eq!(tz_shape(bad), None, "{bad:?}");
        }
        assert_eq!(tz_shape(&"a".repeat(MAX_TZ_CHARS + 1)), None);
    }

    #[test]
    fn pronouns_are_read_in_order_and_bounded() {
        let f = fields(
            r#"{"io.fsky.nyx.pronouns":[
                {"language":"en","summary":"it/its"},
                {"language":"DE","summary":"sie/ihr"},
                {"summary":"   "},
                {"language":"en"},
                "she/her"]}"#,
        );
        assert_eq!(f["pronouns"], "it/its, sie/ihr");
        assert_eq!(f["pronouns_input"], "it/its, de:sie/ihr");

        let many = (0..10)
            .map(|i| format!(r#"{{"summary":"p{i}/{}"}}"#, "x".repeat(40)))
            .collect::<Vec<_>>()
            .join(",");
        let f = fields(&format!(r#"{{"io.fsky.nyx.pronouns":[{many}]}}"#));
        let shown: Vec<&str> = f["pronouns"].as_str().unwrap().split(", ").collect();
        assert_eq!(shown.len(), MAX_PRONOUN_SETS);
        assert!(shown.iter().all(|s| s.chars().count() == MAX_PRONOUN_CHARS));
    }

    #[test]
    fn the_stable_pronouns_key_wins_and_the_unstable_one_is_the_fallback() {
        let both = r#"{"m.pronouns":[{"summary":"she/her"}],
                       "io.fsky.nyx.pronouns":[{"summary":"they/them"}]}"#;
        assert_eq!(fields(both)["pronouns"], "she/her");
        let empty_stable = r#"{"m.pronouns":[],
                               "io.fsky.nyx.pronouns":[{"summary":"they/them"}]}"#;
        assert_eq!(fields(empty_stable)["pronouns"], "they/them");
        assert_eq!(fields(r#"{"io.fsky.nyx.pronouns":"she/her"}"#)["pronouns"], "");
    }

    #[test]
    fn markup_and_control_characters_never_survive_as_structure() {
        let f = fields(
            "{\"io.fsky.nyx.pronouns\":[{\"summary\":\"a\\n\\u202Eb\\u0007c\"}],\
              \"pet.plz.me\":\"fox\\r\\n<b>\"}",
        );
        assert_eq!(f["pronouns"], "a b c");
        assert_eq!(f["animal_is"], "fox <b>");
    }

    #[test]
    fn invisible_and_direction_characters_are_removed() {
        let dirty = "a\u{061C}b\u{200B}c\u{200C}d\u{200D}e\u{2060}f\u{FEFF}g\u{200E}h\u{2067}i";
        assert_eq!(clean_line(dirty, 64), "abcdefghi");
    }

    #[test]
    fn a_delete_that_leaves_a_key_behind_is_a_failure() {
        let failed = || Err::<(), String>("M_FORBIDDEN".to_owned());
        assert_eq!(write_outcome(true, &[Ok(()), Ok(())]), Ok(()));
        assert_eq!(write_outcome(true, &[Ok(()), failed()]), Err("M_FORBIDDEN".to_owned()));
        assert_eq!(write_outcome(true, &[failed(), Ok(())]), Err("M_FORBIDDEN".to_owned()));
        // A set is published as soon as one key carries it.
        assert_eq!(write_outcome(false, &[Ok(()), failed()]), Ok(()));
        assert_eq!(write_outcome(false, &[failed(), failed()]), Err("M_FORBIDDEN".to_owned()));
    }

    #[test]
    fn colours_are_hex_or_nothing() {
        let f = fields(
            r##"{"eu.she-a.color":{"on_dark":"#AABBCC","on_light":"red"},
                 "moe.sable.app.name_color_light_theme":"#102030",
                 "moe.sable.app.name_color_dark_theme":"#000000",
                 "chat.commet.profile_color_scheme":{"color":"#FF8800","brightness":"dark"}}"##,
        );
        assert_eq!(f["name_color_dark"], "#aabbcc");
        assert_eq!(f["name_color_light"], "#102030");
        assert_eq!(f["card_color"], "#ff8800");

        let hostile = fields(
            r##"{"eu.she-a.color":"#aabbcc",
                 "chat.commet.profile_color_scheme":{"color":"url(http://x)"}}"##,
        );
        assert_eq!(hostile["name_color_dark"], "");
        assert_eq!(hostile["name_color_light"], "");
        assert_eq!(hostile["card_color"], "");
    }

    #[test]
    fn animal_identity_reads_the_current_keys_and_the_legacy_booleans() {
        let f = fields(r#"{"pet.plz.me":"fox","pet.plz.my":"two cats","pet.plz.gib":"treats"}"#);
        assert_eq!(f["animal_is"], "fox");
        assert_eq!(f["animal_has"], "two cats");
        assert_eq!(f["animal_need"], "treats");

        let legacy = fields(r#"{"kitty.meow.is_cat":true,"kitty.meow.has_cats":false}"#);
        assert_eq!(legacy["animal_is"], "cat");
        assert_eq!(legacy["animal_has"], "");

        let long = fields(&format!(r#"{{"pet.plz.me":"{}"}}"#, "x".repeat(200)));
        assert_eq!(long["animal_is"].as_str().unwrap().chars().count(), MAX_ANIMAL_CHARS);
    }

    #[test]
    fn a_failed_fetch_parses_to_all_empty_fields() {
        let f = fields_from_profile(&Value::Null);
        assert!(f.as_object().unwrap().values().all(|v| v == ""));
    }

    #[test]
    fn a_time_zone_is_written_under_both_keys_and_cleared_from_both() {
        let (keys, value) = plan_write("timezone", " Europe/Vilnius ").unwrap();
        assert_eq!(keys, ["m.tz", "us.cloke.msc4175.tz"]);
        assert_eq!(value, Some(json!("Europe/Vilnius")));

        let (keys, value) = plan_write("timezone", "  ").unwrap();
        assert_eq!(keys, ["m.tz", "us.cloke.msc4175.tz"]);
        assert_eq!(value, None);

        assert_eq!(plan_write("timezone", "../x").unwrap_err(), "invalid_value");
    }

    #[test]
    fn pronouns_are_written_under_the_key_sable_reads_in_its_shape() {
        let (keys, value) = plan_write("pronouns", "she/her, de:sie/ihr, , x:").unwrap();
        assert_eq!(keys, ["io.fsky.nyx.pronouns"]);
        assert_eq!(
            value,
            Some(json!([
                { "summary": "she/her", "language": "en" },
                { "summary": "sie/ihr", "language": "de" },
            ]))
        );
        let (_, cleared) = plan_write("pronouns", " , ").unwrap();
        assert_eq!(cleared, None);

        let (_, long) = plan_write("pronouns", &"z".repeat(60)).unwrap();
        assert_eq!(long.unwrap()[0]["summary"].as_str().unwrap().len(), MAX_PRONOUN_CHARS);
    }

    #[test]
    fn what_is_written_reads_back_as_typed() {
        let (_, value) = plan_write("pronouns", "they/them, fr:iel").unwrap();
        let f = fields_from_profile(&json!({ PRONOUNS_FIELD_UNSTABLE: value.unwrap() }));
        assert_eq!(f["pronouns_input"], "they/them, fr:iel");
    }

    #[test]
    fn only_the_two_editable_fields_can_be_written() {
        assert_eq!(plan_write("m.tz", "Europe/Paris").unwrap_err(), "unknown_field");
        assert_eq!(plan_write("displayname", "x").unwrap_err(), "unknown_field");
    }

    #[test]
    fn the_capability_refuses_only_what_it_says_it_refuses() {
        let caps = |body: &str| serde_json::from_str::<Value>(body).unwrap();
        assert!(capability_allows(&Value::Null, "m.tz"));
        assert!(capability_allows(&caps(r#"{"capabilities":{}}"#), "m.tz"));
        let on = caps(r#"{"capabilities":{"m.profile_fields":{"enabled":true,"disallowed":[]}}}"#);
        assert!(capability_allows(&on, "m.tz"));
        let off = caps(r#"{"capabilities":{"m.profile_fields":{"enabled":false}}}"#);
        assert!(!capability_allows(&off, "m.tz"));
        let denied = caps(
            r#"{"capabilities":{"m.profile_fields":{"enabled":true,"disallowed":["m.tz"]}}}"#,
        );
        assert!(!capability_allows(&denied, "m.tz"));
        assert!(capability_allows(&denied, "us.cloke.msc4175.tz"));
        let listed = caps(
            r#"{"capabilities":{"m.profile_fields":{"enabled":true,"allowed":["m.tz"]}}}"#,
        );
        assert!(capability_allows(&listed, "m.tz"));
        assert!(!capability_allows(&listed, "io.fsky.nyx.pronouns"));
    }
}
