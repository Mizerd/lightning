
use crate::banner::{is_unsupported, profile_field};
use crate::rooms::require_client;
use crate::{enqueue, RustClient};
use matrix_sdk::ruma::{OwnedUserId, UserId};
use matrix_sdk::Client;
use serde_json::json;
use std::sync::Arc;
use std::time::Duration;

pub(crate) const FIELD: &str = "org.lightning.name_color";

const TIMEOUT: Duration = Duration::from_secs(15);

/// `#rrggbb`, lowercase, nothing else. Validated in both directions: the
/// value is remote text written by someone else's client and ends up on a
/// QML colour property. Anything else is dropped, not repaired.
pub(crate) fn normalized(value: &str) -> Option<String> {
    let trimmed = value.trim();
    let hex = trimmed.strip_prefix('#')?;
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    Some(format!("#{}", hex.to_ascii_lowercase()))
}

fn from_body(body: &str) -> Option<String> {
    let parsed: serde_json::Value = serde_json::from_str(body).ok()?;
    normalized(parsed.get(FIELD)?.as_str()?)
}

pub(crate) async fn fetch(client: &Client, user_id: &str)
    -> Result<Option<String>, String>
{
    let answer = profile_field::get(client, user_id, FIELD, TIMEOUT).await?;
    match answer.status {
        200 => Ok(from_body(&answer.body)),
        // The field is absent: neither an error nor "unsupported".
        404 if !is_unsupported(&answer.body) => Ok(None),
        404 => Err("unsupported".to_owned()),
        s => Err(format!("http_{s}")),
    }
}

pub(crate) async fn set_own(client: &Client, value: &str) -> Result<(), String> {
    let user_id = client
        .user_id()
        .ok_or_else(|| "not_logged_in".to_owned())?
        .to_string();

    let answer = if value.trim().is_empty() {
        profile_field::delete(client, &user_id, FIELD, TIMEOUT).await?
    } else {
        let colour = normalized(value).ok_or_else(|| "invalid_colour".to_owned())?;
        profile_field::set(client, &user_id, FIELD, &colour, TIMEOUT).await?
    };

    match answer.status {
        200 | 201 | 204 => Ok(()),
        404 if !is_unsupported(&answer.body) => Ok(()),
        404 => Err("unsupported".to_owned()),
        403 => Err("forbidden".to_owned()),
        s => Err(format!("http_{s}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn onlySixHexDigitsBehindAHashSurvive() {
        assert_eq!(normalized("#AABBCC").as_deref(), Some("#aabbcc"));
        assert_eq!(normalized("  #a1b2c3  ").as_deref(), Some("#a1b2c3"));
        assert_eq!(normalized("aabbcc"), None);
        assert_eq!(normalized("#abc"), None);
        assert_eq!(normalized("#aabbccdd"), None);
        assert_eq!(normalized("#gggggg"), None);
        assert_eq!(normalized(""), None);
        assert_eq!(normalized("#"), None);
    }

    #[test]
    fn aHostileFieldValueIsDroppedRatherThanPassedOn() {
        for hostile in [
            "red",
            "javascript:alert(1)",
            "#aabbcc; background: url(http://evil.example)",
            "\"#aabbcc\"",
            "#aabbcc\u{0000}",
        ] {
            assert_eq!(normalized(hostile), None, "accepted {hostile:?}");
        }
    }

    #[test]
    fn theFieldIsReadOutOfItsOwnKeyAndNoOther() {
        assert_eq!(
            from_body(r##"{"org.lightning.name_color":"#123456"}"##).as_deref(),
            Some("#123456")
        );
        assert_eq!(from_body(r##"{"displayname":"#123456"}"##), None);
        assert_eq!(from_body(r##"{"org.lightning.name_color":42}"##), None);
        assert_eq!(from_body("not json"), None);
        assert_eq!(from_body("{}"), None);
    }
}

pub(crate) fn fetch_name_color(
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
        let (colour, supported, failed) = match fetch(&client, uid.as_str()).await {
            Ok(value) => (value.unwrap_or_default(), true, false),
            Err(reason) if reason == "unsupported" => (String::new(), false, false),
            Err(_) => (String::new(), true, true),
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "name_color", "lifecycle": lifecycle, "op_id": op_id,
            "user_id": uid.as_str(), "color": colour, "supported": supported,
            "failed": failed,
        }));
    });
    Ok(())
}

pub(crate) fn set_name_color(
    bridge: &RustClient,
    op_id: u64,
    value: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let stored = normalized(&value).unwrap_or_default();
    bridge.spawn_room_action(async move {
        let outcome = set_own(&client, &value).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(()) => enqueue(&events, json!({
                "type": "name_color_set", "lifecycle": lifecycle,
                "op_id": op_id, "ok": true, "color": stored, "category": "",
            })),
            Err(category) => enqueue(&events, json!({
                "type": "name_color_set", "lifecycle": lifecycle,
                "op_id": op_id, "ok": false, "color": "", "category": category,
            })),
        }
    });
    Ok(())
}
