//! The user's preferences kept in account data, so they follow the account to
//! every device. The transport only: WHAT is synced, how conflicts resolve and
//! when to write is decided in C++ (`src/settings/SettingsSyncMerge.*`), which
//! is unit-tested without a homeserver.
//!
//! Two global account-data types, both this account's own and readable by the
//! homeserver (so never a secret, key, token, path or device id; the C++ schema
//! is an allow-list):
//!
//! * `org.lightning_matrix.settings`: the blob,
//!   `{ "version": 1, "settings": { "<key>": { "v": <value>, "t": <ms> } } }`.
//! * `org.lightning_matrix.settings.sync`: the ACCOUNT-WIDE switch,
//!   `{ "version": 1, "enabled": <bool> }`. Its own type for the reason
//!   `bgsync` gives: a blob write that began before another device turned sync
//!   off must not be able to turn it back on, so every blob write re-reads the
//!   switch from the server immediately before its PUT. ABSENT means OFF: this
//!   is opt-in.
//!
//! Reads always ask the server (never the store, which lags this device's own
//! writes until their echo returns), the switch AFTER the blob.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    event_handler::EventHandlerHandle,
    ruma::{
        api::client::config::{get_global_account_data, set_global_account_data},
        events::{macros::EventContent, GlobalAccountDataEvent, GlobalAccountDataEventType},
        serde::Raw,
    },
    Client,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::backdrop::{classify_write_error, is_canonical_json_safe};
use crate::rooms::require_client;
use crate::{enqueue, enqueue_terminal_routed, EventQueueRef, RustClient};

pub(crate) const BLOB_TYPE: &str = "org.lightning_matrix.settings";
pub(crate) const SWITCH_TYPE: &str = "org.lightning_matrix.settings.sync";
/// Homeservers cap an event at 64 KiB; stay well inside it.
pub(crate) const MAX_BLOB_BYTES: usize = 48 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(15);

fn bounded() -> RequestConfig {
    RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT)
}

/// The server's copy of one of our two types: `Ok(None)` for 404, `Err` when
/// it could not be asked. Unparseable JSON is `Some(Null)`.
async fn read_server(client: &Client, event_type: &str) -> Result<Option<Value>, String> {
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let request =
        get_global_account_data::v3::Request::new(own, GlobalAccountDataEventType::from(event_type));
    match client.send(request).with_request_config(bounded()).await {
        Ok(r) => Ok(Some(serde_json::from_str(r.account_data.json().get()).unwrap_or(Value::Null))),
        Err(err) => {
            if err.as_client_api_error().is_some_and(|e| e.status_code.as_u16() == 404) {
                Ok(None)
            } else {
                Err(classify_write_error(&err.to_string()).0.to_owned())
            }
        }
    }
}

/// The switch as stored: only an explicit `enabled: true` is on.
pub(crate) fn switch_on(raw: &Value) -> bool {
    raw.get("enabled").and_then(Value::as_bool).unwrap_or(false)
}

pub(crate) fn compose_switch(on: bool) -> Value {
    json!({ "version": 1, "enabled": on })
}

async fn read_switch(client: &Client) -> Result<bool, String> {
    Ok(read_server(client, SWITCH_TYPE).await?.map(|raw| switch_on(&raw)).unwrap_or(false))
}

async fn put(client: &Client, event_type: &str, content: &Value) -> Result<(), String> {
    if !is_canonical_json_safe(content) {
        return Err("invalid_content".to_owned());
    }
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let raw = Raw::new(content).map_err(|_| "invalid_content".to_owned())?.cast_unchecked();
    let request = set_global_account_data::v3::Request::new_raw(
        own,
        GlobalAccountDataEventType::from(event_type),
        raw,
    );
    client
        .send(request)
        .with_request_config(bounded())
        .await
        .map(|_| ())
        .map_err(|err| classify_write_error(&err.to_string()).0.to_owned())
}

/// Validate a blob before it leaves: an object, with an integer `version` and
/// a `settings` object, inside the size bound. `{}` is the cleared blob.
pub(crate) fn check_blob(content: &Value) -> Result<(), &'static str> {
    let Some(object) = content.as_object() else {
        return Err("invalid_content");
    };
    if !object.is_empty() {
        if !object.get("version").is_some_and(Value::is_i64) {
            return Err("invalid_content");
        }
        if !object.get("settings").is_some_and(Value::is_object) {
            return Err("invalid_content");
        }
    }
    let size = serde_json::to_string(content).map(|s| s.len()).unwrap_or(usize::MAX);
    if size > MAX_BLOB_BYTES {
        return Err("upload_too_large");
    }
    Ok(())
}

/// Read the blob and then the switch from the server. Result event (command
/// lane): `settings_sync_read { op_id, lifecycle, ok, category, blob_state:
/// "absent"|"present"|"unreadable", blob, switch: bool|null }`.
pub(crate) fn read(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let queue = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let blob = read_server(&client, BLOB_TYPE).await;
        let switch = read_switch(&client).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, state, blob_value) = match &blob {
            Ok(None) => (true, "", "absent", Value::Null),
            Ok(Some(Value::Null)) => (true, "", "unreadable", Value::Null),
            Ok(Some(v)) => (true, "", "present", v.clone()),
            Err(c) => (false, c.as_str(), "unreadable", Value::Null),
        };
        let switch_value = switch.as_ref().map(|b| json!(*b)).unwrap_or(Value::Null);
        enqueue_terminal_routed(
            &queue,
            None,
            None,
            json!({
                "type": "settings_sync_read",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok && switch.is_ok(),
                "category": if !ok { category } else { switch.as_ref().err().map(String::as_str).unwrap_or("") },
                "blob_state": state,
                "blob": blob_value,
                "switch": switch_value,
            }),
        );
    });
    Ok(())
}

/// Write. `kind`: 0 = the blob (needs the switch on, checked at the server
/// immediately before), 1 = the switch (`content` is `{"enabled": bool}`),
/// 2 = clear the blob to `{}` (account data cannot be deleted). Result event:
/// `settings_sync_written { op_id, lifecycle, kind, ok, category }`.
pub(crate) fn write(bridge: &RustClient, op_id: u64, kind: u32, content_json: String) -> Result<(), String> {
    let content: Value = serde_json::from_str(&content_json).map_err(|_| "invalid_content".to_owned())?;
    match kind {
        0 => check_blob(&content).map_err(str::to_owned)?,
        1 => {
            if !content.get("enabled").is_some_and(Value::is_boolean) {
                return Err("invalid_content".to_owned());
            }
        }
        2 => {}
        _ => return Err("invalid_content".to_owned()),
    }
    let client = require_client(bridge)?;
    let queue = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result: Result<(), String> = async {
            match kind {
                0 => {
                    if !read_switch(&client).await? {
                        return Err("sync_disabled".to_owned());
                    }
                    put(&client, BLOB_TYPE, &content).await
                }
                1 => put(&client, SWITCH_TYPE, &compose_switch(content["enabled"].as_bool().unwrap_or(false))).await,
                _ => put(&client, BLOB_TYPE, &json!({})).await,
            }
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue_terminal_routed(
            &queue,
            None,
            None,
            json!({
                "type": "settings_sync_written",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "kind": kind,
                "ok": result.is_ok(),
                "category": result.err().unwrap_or_default(),
            }),
        );
    });
    Ok(())
}

/// Typed only so the SDK dispatches the two types to the change handlers;
/// never read. A change is re-read through [`read`], the one parser.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.settings", kind = GlobalAccountData)]
pub(crate) struct SettingsBlobEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.settings.sync", kind = GlobalAccountData)]
pub(crate) struct SettingsSwitchEventContent {}

pub(crate) fn change_payload() -> Value {
    json!({ "type": "settings_sync_changed" })
}

pub(crate) fn install_change_handlers(client: &Client, events: EventQueueRef) -> Vec<EventHandlerHandle> {
    let blob_events = Arc::clone(&events);
    let blob = client.add_event_handler(move |_ev: GlobalAccountDataEvent<SettingsBlobEventContent>| {
        let events = Arc::clone(&blob_events);
        async move {
            enqueue(&events, change_payload());
        }
    });
    let switch = client.add_event_handler(move |_ev: GlobalAccountDataEvent<SettingsSwitchEventContent>| {
        let events = Arc::clone(&events);
        async move {
            enqueue(&events, change_payload());
        }
    });
    vec![blob, switch]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_switch_is_off_unless_explicitly_on() {
        assert!(!switch_on(&json!({})));
        assert!(!switch_on(&json!({ "enabled": "yes" })));
        assert!(!switch_on(&Value::Null));
        assert!(switch_on(&json!({ "version": 1, "enabled": true })));
        assert!(!switch_on(&compose_switch(false)));
        assert!(switch_on(&compose_switch(true)));
    }

    #[test]
    fn a_blob_needs_a_version_and_a_settings_object() {
        assert!(check_blob(&json!({})).is_ok());
        assert!(check_blob(&json!({ "version": 1, "settings": {} })).is_ok());
        assert!(check_blob(&json!({ "settings": {} })).is_err());
        assert!(check_blob(&json!({ "version": 1 })).is_err());
        assert!(check_blob(&json!({ "version": 1, "settings": [] })).is_err());
        assert!(check_blob(&json!([1])).is_err());
    }

    #[test]
    fn an_oversized_blob_is_refused_before_it_is_sent() {
        let big = "x".repeat(MAX_BLOB_BYTES);
        let blob = json!({ "version": 1, "settings": { "k": { "v": big, "t": 1 } } });
        assert_eq!(check_blob(&blob), Err("upload_too_large"));
    }
}
