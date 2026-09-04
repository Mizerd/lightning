//! A display-name colour the user chooses, carried in their Matrix profile so
//! other Lightning clients see it.
//!
//! # Why a profile field
//!
//! Matrix has no standard for this. The options were account data (private to
//! one account — nobody else can read it, which defeats the point), a custom
//! state event per room (a different colour in every room, and a write to
//! every room on every change), or an extended profile field. Only the last
//! one is global, readable by anyone who can see the profile, and changed in
//! one write — which is what "other Lightning users see the colour I set"
//! actually requires.
//!
//! The field is `org.lightning.name_color`, over MSC4133 extended profile
//! fields — the same transport `banner.rs` already uses for MSC4427 profile
//! banners, and for the same reason documented there at length: ruma's typed
//! endpoints cannot select the stable path against a Synapse that advertises
//! the MSC as stable in `unstable_features`, so the path is addressed
//! directly over the SDK's own configured transport.
//!
//! A homeserver without extended profile fields answers M_UNRECOGNIZED. That
//! is reported as unsupported, not as an error: the colour is a nicety, and
//! every name still has its derived theme colour underneath.
//!
//! # What is NOT stored
//!
//! A rendered colour. The value is a hue the sender likes; the VIEWER's
//! client decides what that becomes on the viewer's background, because a
//! colour legible on the sender's theme can be invisible on the viewer's.
//! Storing "#101010" and painting it verbatim would let anyone hand every
//! other user an unreadable name — the clamp lives in `AppTheme.userColor`,
//! and this module's only job is to carry the choice honestly.

use crate::banner::{is_unsupported, profile_field};
use crate::rooms::require_client;
use crate::{enqueue, RustClient};
use matrix_sdk::ruma::{OwnedUserId, UserId};
use matrix_sdk::Client;
use serde_json::json;
use std::sync::Arc;
use std::time::Duration;

/// The profile field. `org.` rather than `m.`: this is not a spec key and
/// must not look like one.
pub(crate) const FIELD: &str = "org.lightning.name_color";

const TIMEOUT: Duration = Duration::from_secs(15);

/// `#rrggbb`, lowercase, and nothing else.
///
/// Validated on the way IN and again on the way OUT, because the value is
/// remote text: a profile field is writable by its owner and read by everyone
/// else, so what arrives has been through somebody else's client. Anything
/// that is not exactly six hex digits behind a `#` is dropped rather than
/// repaired — a "nearly valid" colour is not worth guessing at, and passing
/// unvalidated text to a QML colour property is how a string ends up
/// somewhere it was never meant to be.
pub(crate) fn normalized(value: &str) -> Option<String> {
    let trimmed = value.trim();
    let hex = trimmed.strip_prefix('#')?;
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    Some(format!("#{}", hex.to_ascii_lowercase()))
}

/// Pull the colour out of a `{"org.lightning.name_color": "#aabbcc"}` body.
fn from_body(body: &str) -> Option<String> {
    let parsed: serde_json::Value = serde_json::from_str(body).ok()?;
    normalized(parsed.get(FIELD)?.as_str()?)
}

/// Read one user's chosen colour.
///
/// `Ok(None)` means "they have not set one", which is the ordinary case and
/// is NOT an error — a 404 on the field is the server saying the key is
/// absent. Only a server that does not know the endpoint at all is reported
/// as unsupported.
pub(crate) async fn fetch(client: &Client, user_id: &str)
    -> Result<Option<String>, String>
{
    let answer = profile_field::get(client, user_id, FIELD, TIMEOUT).await?;
    match answer.status {
        200 => Ok(from_body(&answer.body)),
        // The field is absent. Not an error, and not "unsupported" either:
        // the endpoint answered, it simply has nothing to say about this key.
        404 if !is_unsupported(&answer.body) => Ok(None),
        404 => Err("unsupported".to_owned()),
        s => Err(format!("http_{s}")),
    }
}

/// Set (or, with an empty value, clear) the local user's colour.
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
        // Clearing something that was never set is a success, not a failure:
        // the caller asked for "no colour" and there is no colour.
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
        // Everything else is dropped rather than repaired.
        assert_eq!(normalized("aabbcc"), None);      // no hash
        assert_eq!(normalized("#abc"), None);        // short form not accepted
        assert_eq!(normalized("#aabbccdd"), None);   // alpha not accepted
        assert_eq!(normalized("#gggggg"), None);     // not hex
        assert_eq!(normalized(""), None);
        assert_eq!(normalized("#"), None);
    }

    // The value arrives from somebody else's client and reaches a QML colour
    // property. A string that is not a colour must never get that far.
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
        // A body carrying somebody else's key says nothing about this one.
        assert_eq!(from_body(r##"{"displayname":"#123456"}"##), None);
        assert_eq!(from_body(r##"{"org.lightning.name_color":42}"##), None);
        assert_eq!(from_body("not json"), None);
        assert_eq!(from_body("{}"), None);
    }
}

// ── Dispatch, shaped like every other profile read in this crate ──────────

/// Read one user's colour and answer on `name_color`.
///
/// `supported: false` means the homeserver has no extended profile fields at
/// all; the UI renders that as nothing, never as "this user chose no colour".
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
        let (colour, supported) = match fetch(&client, uid.as_str()).await {
            Ok(value) => (value.unwrap_or_default(), true),
            Err(reason) if reason == "unsupported" => (String::new(), false),
            // Any other failure is a transient read problem, not a statement
            // about the user or the server. Reported as "no colour, server
            // supported" so the derived colour is used and the next sync can
            // try again.
            Err(_) => (String::new(), true),
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "name_color", "lifecycle": lifecycle, "op_id": op_id,
            "user_id": uid.as_str(), "color": colour, "supported": supported,
        }));
    });
    Ok(())
}

/// Set or clear this account's colour; answers on `name_color_set`.
pub(crate) fn set_name_color(
    bridge: &RustClient,
    op_id: u64,
    value: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    // Normalised HERE as well as inside set_own, so the event this reports
    // back carries what was actually stored rather than what was typed.
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
