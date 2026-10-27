//! Allow-listed account-data transport: the standard media/invite preferences,
//! recent emoji, and Lightning's saved-message references. The latter is
//! metadata only; `check_saved_content` refuses previews and bodies.
//!
//! This is transport only. Simple reads use the SDK state store; saved
//! references and fail-closed policy types read the server. A write is one
//! bounded PUT, and a change made by another device (or our own
//! write coming back) arrives through an event handler that tells C++ only
//! WHICH type changed; C++ then re-reads it. What the values MEAN (unknown
//! value = off, emoji merge) is decided in C++, where it is tested without a
//! server.
//!
//! Only the types below can cross: the FFI never takes a free-form event
//! type, so QML or a settings import cannot be turned into a way to write
//! arbitrary account data.
//!
//! Since 2026-10-25 also the incoming-invite policy (master prompt Part 6, 6):
//! the stable `m.invite_permission_config` (spec v1.18, `default_action:
//! "block"`) and its unstable MSC4380 form
//! `org.matrix.msc4380.invite_permission_config` (`block_all`). Writing them
//! means something ONLY on a homeserver that implements them, so
//! `invite_policy_support` reports what the server advertises in `/versions`
//! and C++ offers the setting only then. Which type to write and how to
//! reconcile both is decided in C++ (AccountPrefs), tested without a server.

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
/// Personal message references only. The homeserver can read room/event ids.
pub(crate) const SAVED_MESSAGES_TYPE: &str = "org.lightning_matrix.saved_messages";
pub(crate) const INVITE_PERMISSION_TYPE: &str = "m.invite_permission_config";
pub(crate) const INVITE_PERMISSION_UNSTABLE_TYPE: &str =
    "org.matrix.msc4380.invite_permission_config";
/// The `/versions` unstable feature a server advertises for MSC4380.
pub(crate) const INVITE_PERMISSION_FEATURE: &str = "org.matrix.msc4380";

/// A write is one small object; never accept an unbounded account-data blob.
const MAX_CONTENT_BYTES: usize = 16 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

/// The allow-list. `None` for anything else.
pub(crate) fn allowed_type(event_type: &str) -> Option<&'static str> {
    match event_type {
        MEDIA_PREVIEW_TYPE => Some(MEDIA_PREVIEW_TYPE),
        RECENT_EMOJI_TYPE => Some(RECENT_EMOJI_TYPE),
        SAVED_MESSAGES_TYPE => Some(SAVED_MESSAGES_TYPE),
        INVITE_PERMISSION_TYPE => Some(INVITE_PERMISSION_TYPE),
        INVITE_PERMISSION_UNSTABLE_TYPE => Some(INVITE_PERMISSION_UNSTABLE_TYPE),
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

/// Keep the account-data event metadata-only even if a future UI mistakenly
/// passes a preview/body. The transport is not an encrypted store.
pub(crate) fn check_saved_content(value: &Value) -> Result<(), &'static str> {
    let entries = value.get("entries").and_then(Value::as_array).ok_or("invalid_content")?;
    if value.as_object().map_or(true, |o| {
        o.len() != 2 || o.get("version") != Some(&json!(1))
    })
        || entries.len() > 100
    {
        return Err("invalid_content");
    }
    let valid_event = |item: Option<&Value>, empty_allowed: bool| {
        item.and_then(Value::as_str).is_some_and(|id| {
            (empty_allowed && id.is_empty())
                || (id.len() <= 255
                    && !id.contains('\u{1f}')
                    && matrix_sdk::ruma::EventId::parse(id).is_ok())
        })
    };
    for item in entries {
        let o = item.as_object().ok_or("invalid_content")?;
        if o.len() != 4
            || !o.get("room_id").and_then(Value::as_str).is_some_and(|id| {
                id.len() <= 255
                    && !id.contains('\u{1f}')
                    && matrix_sdk::ruma::RoomId::parse(id).is_ok()
            })
            || !valid_event(o.get("event_id"), false)
            || !valid_event(o.get("thread_root_id"), true)
            || !o.get("saved_at_ms").is_some_and(Value::is_u64)
        {
            return Err("invalid_content");
        }
    }
    Ok(())
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

/// Whether a type is read from the SERVER rather than the state store. The
/// incoming-invite policy decides a server-side rejection: an empty store
/// before the first sync would read "absent", and a switch built on that would
/// show "allowed" while the account blocks, and compose a write that drops
/// what the account holds. So these two are fetched, and a failed fetch is
/// reported as failed, never as absent.
pub(crate) fn reads_from_server(event_type: &str) -> bool {
    // The media-preview choice too (wave-2 review S4): before the first sync
    // a new device's empty store answers "absent", which would open the
    // fail-closed media and invite-avatar gates on the defaults while the
    // account says "off", and compose a write from nothing.
    event_type == INVITE_PERMISSION_TYPE
        || event_type == INVITE_PERMISSION_UNSTABLE_TYPE
        || event_type == MEDIA_PREVIEW_TYPE
        || event_type == SAVED_MESSAGES_TYPE
}

/// `account_pref_failed { pref_type, category }`: a server read that did not
/// answer. C++ treats the type as unknown.
pub(crate) fn failed_payload(event_type: &str, category: &str) -> Value {
    json!({ "type": "account_pref_failed", "pref_type": event_type, "category": category })
}

/// Read one type: from the state store, or from the server for the types in
/// `reads_from_server`.
pub(crate) fn read(bridge: &RustClient, event_type: String) -> Result<(), String> {
    let event_type = allowed_type(&event_type).ok_or_else(|| "unsupported_type".to_owned())?;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let account = client.account();
        let fetched = if reads_from_server(event_type) {
            match tokio::time::timeout(
                REQUEST_TIMEOUT,
                account.fetch_account_data(GlobalAccountDataEventType::from(event_type)),
            )
            .await
            {
                Ok(Ok(raw)) => Ok(raw),
                Ok(Err(err)) => Err(classify_write_error(&err.to_string()).0),
                Err(_) => Err("timeout"),
            }
        } else {
            Ok(account
                .account_data_raw(GlobalAccountDataEventType::from(event_type))
                .await
                .ok()
                .flatten())
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut payload = match fetched {
            Ok(raw) => read_payload(
                event_type,
                raw.and_then(|raw| serde_json::from_str::<Value>(raw.json().get()).ok())
                    // Both calls answer with the event's CONTENT.
                    .filter(Value::is_object),
            ),
            Err(category) => failed_payload(event_type, category),
        };
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
    if event_type == SAVED_MESSAGES_TYPE {
        check_saved_content(&content).map_err(str::to_owned)?;
    }
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

/// What the homeserver advertises for the incoming-invite policy, from
/// `/versions` (cached by the SDK after the first call): `stable` for spec
/// v1.18 or later, `unstable` for the MSC4380 feature flag. Pure, for the
/// tests.
pub(crate) fn invite_policy_support_from(
    versions: &std::collections::BTreeSet<matrix_sdk::ruma::api::MatrixVersion>,
    features: &std::collections::BTreeSet<matrix_sdk::ruma::api::FeatureFlag>,
) -> (bool, bool) {
    let stable = versions
        .iter()
        .any(|v| *v >= matrix_sdk::ruma::api::MatrixVersion::V1_18);
    let unstable = features
        .contains(&matrix_sdk::ruma::api::FeatureFlag::from(INVITE_PERMISSION_FEATURE));
    (stable, unstable)
}

/// Answers `invite_policy_support { stable, unstable, ok, lifecycle }`; `ok`
/// false when `/versions` could not be read (offline), which C++ shows as
/// "unknown", never as "unsupported".
pub(crate) fn invite_policy_support(bridge: &RustClient) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let answer = tokio::time::timeout(REQUEST_TIMEOUT, async {
            let versions = client.server_versions().await.ok()?;
            let features = client.unstable_features().await.ok()?;
            Some(invite_policy_support_from(&versions, &features))
        })
        .await
        .ok()
        .flatten();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, stable, unstable) = match answer {
            Some((stable, unstable)) => (true, stable, unstable),
            None => (false, false, false),
        };
        enqueue(
            &events,
            json!({
                "type": "invite_policy_support", "lifecycle": lifecycle,
                "ok": ok, "stable": stable, "unstable": unstable,
            }),
        );
    });
    Ok(())
}

/// Typed only so the SDK dispatches the type to the handler; never read.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.media_preview_config", kind = GlobalAccountData)]
pub(crate) struct MediaPreviewConfigEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.recent_emoji", kind = GlobalAccountData)]
pub(crate) struct RecentEmojiEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.saved_messages", kind = GlobalAccountData)]
pub(crate) struct SavedMessagesEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.invite_permission_config", kind = GlobalAccountData)]
pub(crate) struct InvitePermissionEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4380.invite_permission_config", kind = GlobalAccountData)]
pub(crate) struct UnstableInvitePermissionEventContent {}

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
    let emoji_events = Arc::clone(&events);
    let emoji = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<RecentEmojiEventContent>| {
            let events = Arc::clone(&emoji_events);
            async move {
                enqueue(&events, change_payload(RECENT_EMOJI_TYPE));
            }
        },
    );
    let invite_events = Arc::clone(&events);
    let invite = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<InvitePermissionEventContent>| {
            let events = Arc::clone(&invite_events);
            async move {
                enqueue(&events, change_payload(INVITE_PERMISSION_TYPE));
            }
        },
    );
    let unstable = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<UnstableInvitePermissionEventContent>| {
            let events = Arc::clone(&events);
            async move {
                enqueue(&events, change_payload(INVITE_PERMISSION_UNSTABLE_TYPE));
            }
        },
    );
    let saved_events = Arc::clone(&events);
    let saved = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<SavedMessagesEventContent>| {
            let events = Arc::clone(&saved_events);
            async move {
                enqueue(&events, change_payload(SAVED_MESSAGES_TYPE));
            }
        },
    );
    vec![media, emoji, invite, unstable, saved]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn saved_messages_account_data_rejects_plaintext_and_composites() {
        let reference = json!({"room_id":"!room:example.org", "event_id":"$reply",
            "thread_root_id":"$root", "saved_at_ms":42});
        assert_eq!(allowed_type(SAVED_MESSAGES_TYPE), Some(SAVED_MESSAGES_TYPE));
        assert!(reads_from_server(SAVED_MESSAGES_TYPE));
        assert!(check_saved_content(&json!({"version":1,"entries":[reference.clone()]})).is_ok());
        let mut with_body = reference.clone();
        with_body["body"] = json!("secret");
        assert!(check_saved_content(&json!({"version":1,"entries":[with_body]})).is_err());
        let mut composite = reference;
        composite["room_id"] = json!("!room:example.org\u{1f}thread\u{1f}$root");
        assert!(check_saved_content(&json!({"version":1,"entries":[composite]})).is_err());
    }

    #[test]
    fn the_policy_types_are_read_from_the_server() {
        assert!(reads_from_server(INVITE_PERMISSION_TYPE));
        assert!(reads_from_server(INVITE_PERMISSION_UNSTABLE_TYPE));
        assert!(reads_from_server(MEDIA_PREVIEW_TYPE));
        assert!(!reads_from_server(RECENT_EMOJI_TYPE));
        let failed = failed_payload(INVITE_PERMISSION_TYPE, "timeout");
        assert_eq!(failed["type"], json!("account_pref_failed"));
        assert_eq!(failed["pref_type"], json!(INVITE_PERMISSION_TYPE));
    }

    #[test]
    fn the_invite_policy_types_cross_and_support_reads_versions() {
        assert_eq!(allowed_type(INVITE_PERMISSION_TYPE), Some(INVITE_PERMISSION_TYPE));
        assert_eq!(
            allowed_type(INVITE_PERMISSION_UNSTABLE_TYPE),
            Some(INVITE_PERMISSION_UNSTABLE_TYPE)
        );
        use matrix_sdk::ruma::api::{FeatureFlag, MatrixVersion};
        use std::collections::BTreeSet;
        let old: BTreeSet<MatrixVersion> = [MatrixVersion::V1_11].into_iter().collect();
        let new: BTreeSet<MatrixVersion> =
            [MatrixVersion::V1_11, MatrixVersion::V1_18].into_iter().collect();
        let none: BTreeSet<FeatureFlag> = BTreeSet::new();
        let msc: BTreeSet<FeatureFlag> =
            [FeatureFlag::from(INVITE_PERMISSION_FEATURE)].into_iter().collect();
        assert_eq!(invite_policy_support_from(&old, &none), (false, false));
        assert_eq!(invite_policy_support_from(&new, &none), (true, false));
        assert_eq!(invite_policy_support_from(&old, &msc), (false, true));
    }

    #[test]
    fn only_allowlisted_account_data_types_can_cross() {
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
