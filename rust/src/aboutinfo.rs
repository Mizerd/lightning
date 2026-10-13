//! The About page's three backend questions: what does this homeserver say
//! about itself, what account data does the account hold (read only), and
//! clear the SDK's event cache.
//!
//! * Homeserver information is two unauthenticated GETs: `/_matrix/client/
//!   versions` (spec versions and unstable features) and, best effort,
//!   `/_matrix/federation/v1/version` (software name and version, which a
//!   deployment may keep off the client port). The access token is NEVER sent
//!   to either.
//! * The account-data browser enumerates with one filtered initial `/sync`
//!   (`timeout=0`, no `since`), which is the only way to list what exists:
//!   neither the SDK nor its store can list account data by type. The request
//!   is made over the SDK's HTTP client and does not touch the SDK's own sync
//!   position. Secret-bearing types (secret storage, cross-signing keys, the
//!   backup key) are REDACTED here, in Rust, to their type name: their content
//!   never reaches C++ or QML (CLAUDE.md §6).
//! * Clearing the event cache calls the SDK's `clear_all_rooms`. It never
//!   touches the crypto store, the state store or the credentials.

use std::ffi::c_void;
use std::os::raw::c_char;
use std::sync::Arc;
use std::time::Duration;

use serde_json::{json, Value};

use crate::rooms::require_client;
use crate::{bridge, cstr_arg, enqueue, ffi_string, RustClient};

const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
/// `/versions` and the federation version are tiny.
const SMALL_BODY_CAP: usize = 65_536;
/// An account with a lot of account data answers a few hundred KiB.
const SYNC_BODY_CAP: usize = 4 * 1024 * 1024;
/// One pretty-printed entry is cut here; the browser says so.
const ENTRY_CAP: usize = 64 * 1024;
/// More entries than this are not listed; the browser says so.
const ENTRY_COUNT_CAP: usize = 500;

/// Account-data types whose CONTENT is secret or key material. Matched on the
/// type's prefix, so a future `m.secret_storage.key.<id>` is covered.
const SECRET_PREFIXES: &[&str] = &[
    "m.secret_storage.",
    "m.cross_signing.",
    "m.megolm_backup.",
    "m.dehydrated_device",
    "org.matrix.msc3814",
];

/// True when an account-data type's content must never leave this module.
/// Anything with "secret" in its name is treated the same way, so a type this
/// list has not heard of fails closed. Pure and unit-tested.
pub(crate) fn is_secret_type(event_type: &str) -> bool {
    let lower = event_type.to_ascii_lowercase();
    SECRET_PREFIXES.iter().any(|p| lower.starts_with(p)) || lower.contains("secret")
}

/// One list entry from `{"type": .., "content": ..}` events: the type, whether
/// it was redacted, and the pretty JSON (empty when redacted). Pure and
/// unit-tested.
pub(crate) fn entries_from_events(events: &[Value]) -> (Vec<Value>, bool) {
    let mut out: Vec<Value> = Vec::new();
    let mut truncated = false;
    for event in events {
        let Some(ty) = event.get("type").and_then(Value::as_str) else {
            continue;
        };
        if out.len() >= ENTRY_COUNT_CAP {
            truncated = true;
            break;
        }
        if is_secret_type(ty) {
            out.push(json!({ "type": ty, "redacted": true, "json": "", "cut": false }));
            continue;
        }
        let content = event.get("content").cloned().unwrap_or(Value::Null);
        let mut pretty = serde_json::to_string_pretty(&content).unwrap_or_default();
        let cut = pretty.len() > ENTRY_CAP;
        if cut {
            let mut end = ENTRY_CAP;
            while !pretty.is_char_boundary(end) {
                end -= 1;
            }
            pretty.truncate(end);
        }
        out.push(json!({ "type": ty, "redacted": false, "json": pretty, "cut": cut }));
    }
    out.sort_by(|a, b| {
        a["type"].as_str().unwrap_or("").cmp(b["type"].as_str().unwrap_or(""))
    });
    (out, truncated)
}

/// Account-data events out of an initial `/sync` body: the global ones, or the
/// named room's. Pure and unit-tested.
pub(crate) fn events_from_sync(body: &Value, room_id: &str) -> Vec<Value> {
    let events = if room_id.is_empty() {
        body.pointer("/account_data/events")
    } else {
        ["join", "invite", "leave"].iter().find_map(|state| {
            body.get("rooms")
                .and_then(|r| r.get(*state))
                .and_then(|r| r.get(room_id))
                .and_then(|r| r.pointer("/account_data/events"))
        })
    };
    events.and_then(Value::as_array).cloned().unwrap_or_default()
}

/// The filter the enumerating sync carries: nothing but account data.
pub(crate) fn sync_filter(room_id: &str) -> Value {
    let rooms: Vec<&str> = if room_id.is_empty() { vec![] } else { vec![room_id] };
    json!({
        "presence": { "types": [] },
        "account_data": { "limit": if room_id.is_empty() { 1000 } else { 0 } },
        "room": {
            "rooms": rooms,
            "ephemeral": { "types": [] },
            "state": { "types": [] },
            "timeline": { "types": [], "limit": 0 },
            "account_data": { "limit": 1000 },
        },
    })
}

/// `/versions` into `(versions, features)`. Pure and unit-tested.
pub(crate) fn parse_versions(body: &Value) -> (Vec<String>, Vec<(String, bool)>) {
    let versions = body
        .get("versions")
        .and_then(Value::as_array)
        .map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_owned)).collect())
        .unwrap_or_default();
    let mut features: Vec<(String, bool)> = body
        .get("unstable_features")
        .and_then(Value::as_object)
        .map(|o| {
            o.iter()
                .map(|(k, v)| (k.clone(), v.as_bool().unwrap_or(false)))
                .collect()
        })
        .unwrap_or_default();
    features.sort();
    (versions, features)
}

/// `/federation/v1/version` into `(name, version)`. Pure and unit-tested.
pub(crate) fn parse_software(body: &Value) -> (String, String) {
    let server = body.get("server");
    let text = |key: &str| {
        server
            .and_then(|s| s.get(key))
            .and_then(Value::as_str)
            .unwrap_or("")
            .chars()
            .filter(|c| !c.is_control())
            .take(120)
            .collect::<String>()
    };
    (text("name"), text("version"))
}

/// `base` plus path segments, each encoded as one segment.
fn endpoint(base: &url::Url, segments: &[&str]) -> Result<url::Url, String> {
    let mut url = base.clone();
    url.set_query(None);
    url.set_fragment(None);
    url.path_segments_mut()
        .map_err(|_| "homeserver url cannot carry a path".to_owned())?
        .pop_if_empty()
        .extend(segments);
    Ok(url)
}

/// Status and body (None past `cap`). `token` is attached only when given.
async fn get(
    client: &matrix_sdk::Client,
    url: url::Url,
    token: Option<String>,
    cap: usize,
) -> Result<(u16, Option<Value>), &'static str> {
    let mut request = client.http_client().get(url).timeout(REQUEST_TIMEOUT);
    if let Some(token) = token {
        request = request.header(reqwest::header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let mut response = request
        .send()
        .await
        .map_err(|e| if e.is_connect() { "network" } else { "unknown" })?;
    let status = response.status().as_u16();
    let mut bytes: Vec<u8> = Vec::new();
    loop {
        match response.chunk().await {
            Ok(Some(chunk)) => {
                if bytes.len() + chunk.len() > cap {
                    return Ok((status, None));
                }
                bytes.extend_from_slice(&chunk);
            }
            Ok(None) => break,
            Err(_) => return Ok((status, None)),
        }
    }
    Ok((status, serde_json::from_slice(&bytes).ok()))
}

/// Result event `homeserver_info { op_id, ok, address, versions, features,
/// software_name, software_version, category }`.
pub(crate) fn homeserver_info(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let base = client.homeserver();
        let address = base.as_str().trim_end_matches('/').to_owned();
        let client_versions = match endpoint(&base, &["_matrix", "client", "versions"]) {
            Ok(url) => get(&client, url, None, SMALL_BODY_CAP).await,
            Err(_) => Err("network"),
        };
        let mut event = json!({
            "type": "homeserver_info", "op_id": op_id, "lifecycle": lifecycle,
            "address": address, "ok": false, "category": "",
            "versions": [], "features": [], "software_name": "", "software_version": "",
        });
        match client_versions {
            Ok((200, Some(body))) => {
                let (versions, features) = parse_versions(&body);
                event["ok"] = json!(true);
                event["versions"] = json!(versions);
                event["features"] = json!(features
                    .into_iter()
                    .map(|(name, on)| json!({ "name": name, "enabled": on }))
                    .collect::<Vec<_>>());
                // Best effort: most deployments answer it on the federation
                // port only, and a miss is not a failure.
                if let Ok(url) = endpoint(&base, &["_matrix", "federation", "v1", "version"]) {
                    if let Ok((200, Some(body))) = get(&client, url, None, SMALL_BODY_CAP).await {
                        let (name, version) = parse_software(&body);
                        event["software_name"] = json!(name);
                        event["software_version"] = json!(version);
                    }
                }
            }
            Ok(_) => event["category"] = json!("unavailable"),
            Err(category) => event["category"] = json!(category),
        }
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, event);
        }
    });
    Ok(())
}

/// Result event `account_data_listing { op_id, room_id, ok, truncated,
/// entries: [{type, redacted, json, cut}], category }`.
pub(crate) fn account_data_listing(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    if !room_id.is_empty() {
        matrix_sdk::ruma::RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut event = json!({
            "type": "account_data_listing", "op_id": op_id, "lifecycle": lifecycle,
            "room_id": room_id, "ok": false, "truncated": false, "entries": [], "category": "",
        });
        let answer = async {
            let token = client.access_token().ok_or("network")?;
            let mut url =
                endpoint(&client.homeserver(), &["_matrix", "client", "v3", "sync"]).map_err(|_| "network")?;
            url.query_pairs_mut()
                .append_pair("filter", &sync_filter(&room_id).to_string())
                .append_pair("timeout", "0");
            get(&client, url, Some(token), SYNC_BODY_CAP).await
        }
        .await;
        match answer {
            Ok((200, Some(body))) => {
                let (entries, truncated) = entries_from_events(&events_from_sync(&body, &room_id));
                event["ok"] = json!(true);
                event["truncated"] = json!(truncated);
                event["entries"] = json!(entries);
            }
            Ok((200, None)) => event["category"] = json!("too_large"),
            Ok((401, _)) | Ok((403, _)) => event["category"] = json!("forbidden"),
            Ok(_) => event["category"] = json!("unavailable"),
            Err(category) => event["category"] = json!(category),
        }
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, event);
        }
    });
    Ok(())
}

/// Result event `event_cache_cleared { op_id, ok }`. The SDK clears the
/// storage and tells live observers; the crypto store is not involved.
pub(crate) fn clear_event_cache(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    bridge.spawn_room_action(async move {
        let ok = client.event_cache().clear_all_rooms().await.is_ok();
        enqueue(&events, json!({ "type": "event_cache_cleared", "op_id": op_id, "ok": ok }));
    });
    Ok(())
}

/// Ask the homeserver what it is. Result: `homeserver_info`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_homeserver_info(ptr: *mut c_void, op_id: u64) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        homeserver_info(handle, op_id).map(|_| String::new())
    })
}

/// List the account data (global when `room_id` is empty). Secret types are
/// redacted before the result leaves Rust. Result: `account_data_listing`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_account_data_listing(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        account_data_listing(handle, room_id, op_id).map(|_| String::new())
    })
}

/// Clear the SDK's event cache (not the crypto store). Result:
/// `event_cache_cleared`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_event_cache_clear(ptr: *mut c_void, op_id: u64) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        clear_event_cache(handle, op_id).map(|_| String::new())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secret_types_are_recognised_by_prefix_and_by_name() {
        for t in [
            "m.secret_storage.default_key",
            "m.secret_storage.key.abc",
            "m.cross_signing.master",
            "m.cross_signing.user_signing",
            "m.megolm_backup.v1",
            "M.Cross_Signing.Self_Signing",
            "org.example.my_secret_thing",
        ] {
            assert!(is_secret_type(t), "{t}");
        }
        for t in ["m.direct", "m.push_rules", "m.fully_read", "im.vector.setting.breadcrumbs"] {
            assert!(!is_secret_type(t), "{t}");
        }
    }

    #[test]
    fn a_secret_entry_carries_its_type_name_and_nothing_else() {
        let events = vec![
            json!({"type": "m.cross_signing.master", "content": {"encrypted": {"K": {"ciphertext": "SECRETBYTES"}}}}),
            json!({"type": "m.direct", "content": {"@a:b": ["!r:b"]}}),
        ];
        let (entries, truncated) = entries_from_events(&events);
        assert!(!truncated);
        let all = serde_json::to_string(&entries).unwrap();
        assert!(!all.contains("SECRETBYTES") && !all.contains("ciphertext"));
        let secret = entries.iter().find(|e| e["type"] == "m.cross_signing.master").unwrap();
        assert_eq!(secret["redacted"], true);
        assert_eq!(secret["json"], "");
        let direct = entries.iter().find(|e| e["type"] == "m.direct").unwrap();
        assert_eq!(direct["redacted"], false);
        assert!(direct["json"].as_str().unwrap().contains("\"@a:b\""));
    }

    #[test]
    fn entries_are_sorted_capped_and_long_json_is_cut() {
        let big = "x".repeat(ENTRY_CAP * 2);
        let (entries, _) = entries_from_events(&[
            json!({"type": "b.big", "content": {"v": big}}),
            json!({"type": "a.small", "content": {}}),
        ]);
        assert_eq!(entries[0]["type"], "a.small");
        assert_eq!(entries[1]["cut"], true);
        assert!(entries[1]["json"].as_str().unwrap().len() <= ENTRY_CAP);
        let many: Vec<Value> = (0..ENTRY_COUNT_CAP + 5)
            .map(|i| json!({"type": format!("t.{i:04}"), "content": {}}))
            .collect();
        let (entries, truncated) = entries_from_events(&many);
        assert!(truncated);
        assert_eq!(entries.len(), ENTRY_COUNT_CAP);
    }

    #[test]
    fn sync_events_come_from_global_or_the_named_room() {
        let body = json!({
            "account_data": {"events": [{"type": "m.direct", "content": {}}]},
            "rooms": {"join": {"!r:b": {"account_data": {"events": [{"type": "m.tag", "content": {}}]}}}},
        });
        assert_eq!(events_from_sync(&body, "")[0]["type"], "m.direct");
        assert_eq!(events_from_sync(&body, "!r:b")[0]["type"], "m.tag");
        assert!(events_from_sync(&body, "!other:b").is_empty());
    }

    #[test]
    fn versions_and_software_parse_and_tolerate_garbage() {
        let (v, f) = parse_versions(&json!({
            "versions": ["v1.1", "v1.11"],
            "unstable_features": {"org.b": true, "org.a": false, "org.c": "x"},
        }));
        assert_eq!(v, ["v1.1", "v1.11"]);
        assert_eq!(
            f,
            [("org.a".into(), false), ("org.b".into(), true), ("org.c".into(), false)]
        );
        assert_eq!(parse_versions(&json!("nonsense")), (vec![], vec![]));
        let (n, ver) = parse_software(&json!({"server": {"name": "Synapse", "version": "1.2\u{7}3"}}));
        assert_eq!((n.as_str(), ver.as_str()), ("Synapse", "123"));
        assert_eq!(parse_software(&json!({})), (String::new(), String::new()));
    }

    #[test]
    fn the_sync_filter_asks_for_account_data_only() {
        let f = sync_filter("");
        assert_eq!(f["room"]["rooms"], json!([]));
        assert_eq!(f["room"]["timeline"]["limit"], 0);
        assert_eq!(sync_filter("!r:b")["room"]["rooms"], json!(["!r:b"]));
    }

    #[test]
    fn endpoint_never_adds_or_escapes_a_segment() {
        let base = url::Url::parse("https://h.example/prefix/").unwrap();
        assert_eq!(
            endpoint(&base, &["_matrix", "client", "versions"]).unwrap().as_str(),
            "https://h.example/prefix/_matrix/client/versions"
        );
    }
}
