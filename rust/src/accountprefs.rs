//! Two standard, cross-client account-data preferences: MSC4278
//! `m.media_preview_config` (invite avatars, media previews) and Element's
//! `m.recent_emoji`.
//!
//! This is transport only. The SDK keeps global account data in its state
//! store as sync delivers it, so a read answers from the store (no request),
//! a write is one bounded PUT, and a change made by another device (or our own
//! write coming back) arrives through an event handler that tells C++ only
//! WHICH type changed; C++ then re-reads it. What the values MEAN (unknown
//! value = off, emoji merge) is decided in C++, where it is tested without a
//! server.
//!
//! Only the two types below can cross: the FFI never takes a free-form event
//! type, so QML or a settings import cannot be turned into a way to write
//! arbitrary account data.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    event_handler::EventHandlerHandle,
    ruma::{
        api::client::config::set_global_account_data,
        events::{
            macros::EventContent, GlobalAccountDataEvent, GlobalAccountDataEventType,
        },
        serde::Raw,
    },
    Client,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::backdrop::{classify_write_error, is_canonical_json_safe};
use crate::rooms::require_client;
use crate::{enqueue, EventQueueRef, RustClient};

pub(crate) const MEDIA_PREVIEW_TYPE: &str = "m.media_preview_config";
pub(crate) const RECENT_EMOJI_TYPE: &str = "m.recent_emoji";

/// A write is one small object; anything larger is not one of these two.
const MAX_CONTENT_BYTES: usize = 16 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

/// The allow-list. `None` for anything else.
pub(crate) fn allowed_type(event_type: &str) -> Option<&'static str> {
    match event_type {
        MEDIA_PREVIEW_TYPE => Some(MEDIA_PREVIEW_TYPE),
        RECENT_EMOJI_TYPE => Some(RECENT_EMOJI_TYPE),
        _ => None,
    }
}

/// A content string a write may send: a JSON OBJECT, bounded, with no
/// duplicate-key or non-finite-number trouble.
pub(crate) fn parse_content(json: &str) -> Result<Value, &'static str> {
    if json.len() > MAX_CONTENT_BYTES {
        return Err("too_large");
    }
    let value: Value = serde_json::from_str(json).map_err(|_| "invalid_content")?;
    if !value.is_object() || !is_canonical_json_safe(&value) {
        return Err("invalid_content");
    }
    Ok(value)
}

/// The event C++ receives for a read: `present` is false when the account
/// holds no such data (which is different from an empty object).
pub(crate) fn read_payload(event_type: &str, content: Option<Value>) -> Value {
    match content {
        Some(content) => json!({
            "type": "account_pref", "pref_type": event_type,
            "present": true, "content": content,
        }),
        None => json!({
            "type": "account_pref", "pref_type": event_type,
            "present": false, "content": {},
        }),
    }
}

pub(crate) fn change_payload(event_type: &str) -> Value {
    json!({ "type": "account_pref_changed", "pref_type": event_type })
}

/// Read one type from the state store.
pub(crate) fn read(bridge: &RustClient, event_type: String) -> Result<(), String> {
    let event_type = allowed_type(&event_type).ok_or_else(|| "unsupported_type".to_owned())?;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let content = client
            .account()
            .account_data_raw(GlobalAccountDataEventType::from(event_type))
            .await
            .ok()
            .flatten()
            .and_then(|raw| serde_json::from_str::<Value>(raw.json().get()).ok())
            // `account_data_raw` already answers with the event's CONTENT.
            .filter(Value::is_object);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut payload = read_payload(event_type, content);
        payload["lifecycle"] = json!(lifecycle);
        enqueue(&events, payload);
    });
    Ok(())
}

/// PUT one type. Answers `account_pref_written { pref_type, ok, category }`.
pub(crate) fn write(
    bridge: &RustClient,
    event_type: String,
    content_json: String,
) -> Result<(), String> {
    let event_type = allowed_type(&event_type).ok_or_else(|| "unsupported_type".to_owned())?;
    let content = parse_content(&content_json).map_err(str::to_owned)?;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let result = put(&client, event_type, &content).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "account_pref_written", "pref_type": event_type,
                "lifecycle": lifecycle, "ok": result.is_ok(),
                "category": result.err().unwrap_or_default(),
            }),
        );
    });
    Ok(())
}

async fn put(client: &Client, event_type: &str, content: &Value) -> Result<(), String> {
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let raw = Raw::new(content).map_err(|_| "invalid_content".to_owned())?.cast_unchecked();
    let request = set_global_account_data::v3::Request::new_raw(
        own,
        GlobalAccountDataEventType::from(event_type),
        raw,
    );
    client
        .send(request)
        .with_request_config(RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT))
        .await
        .map(|_| ())
        .map_err(|err| classify_write_error(&err.to_string()).0.to_owned())
}

/// Typed only so the SDK dispatches the type to the handler; never read.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.media_preview_config", kind = GlobalAccountData)]
pub(crate) struct MediaPreviewConfigEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.recent_emoji", kind = GlobalAccountData)]
pub(crate) struct RecentEmojiEventContent {}

/// Another device changed one of the two types, or our own write came back.
pub(crate) fn install_change_handlers(
    client: &Client,
    events: EventQueueRef,
) -> Vec<EventHandlerHandle> {
    let media_events = Arc::clone(&events);
    let media = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<MediaPreviewConfigEventContent>| {
            let events = Arc::clone(&media_events);
            async move {
                enqueue(&events, change_payload(MEDIA_PREVIEW_TYPE));
            }
        },
    );
    let emoji = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<RecentEmojiEventContent>| {
            let events = Arc::clone(&events);
            async move {
                enqueue(&events, change_payload(RECENT_EMOJI_TYPE));
            }
        },
    );
    vec![media, emoji]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_two_standard_types_can_cross() {
        assert_eq!(allowed_type("m.media_preview_config"), Some("m.media_preview_config"));
        assert_eq!(allowed_type("m.recent_emoji"), Some("m.recent_emoji"));
        for other in [
            "m.direct", "m.push_rules", "m.secret_storage.default_key",
            "m.cross_signing.master", "org.lightning_matrix.backgrounds", "", "M.RECENT_EMOJI",
        ] {
            assert_eq!(allowed_type(other), None, "{other} must be refused");
        }
    }

    #[test]
    fn a_write_must_be_a_bounded_json_object() {
        assert!(parse_content(r#"{"invite_avatars":"off"}"#).is_ok());
        assert_eq!(parse_content("[1]"), Err("invalid_content"));
        assert_eq!(parse_content("\"x\""), Err("invalid_content"));
        assert_eq!(parse_content("not json"), Err("invalid_content"));
        let big = format!(r#"{{"a":"{}"}}"#, "x".repeat(MAX_CONTENT_BYTES));
        assert_eq!(parse_content(&big), Err("too_large"));
    }

    #[test]
    fn an_absent_preference_is_not_an_empty_one() {
        let absent = read_payload(MEDIA_PREVIEW_TYPE, None);
        assert_eq!(absent["present"], json!(false));
        let empty = read_payload(MEDIA_PREVIEW_TYPE, Some(json!({})));
        assert_eq!(empty["present"], json!(true));
        assert_eq!(empty["pref_type"], json!("m.media_preview_config"));
    }

    #[test]
    fn a_change_names_only_the_type() {
        assert_eq!(
            change_payload(RECENT_EMOJI_TYPE),
            json!({"type": "account_pref_changed", "pref_type": "m.recent_emoji"})
        );
    }
}
