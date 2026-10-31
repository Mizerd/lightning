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
//! * Clearing the event cache calls the SDK's `clear_all_rooms`, then compacts
//!   the event cache (and, for the media reset, the media store) through the
//!   SDK's typed store locks and `optimize()` (a SQLite VACUUM) so the files
//!   shrink. The compaction is reported as its own later `cache_compacted`
//!   event; it never delays the clear's result. It never touches the crypto
//!   store, the state store or the credentials.

use std::ffi::c_void;
use std::os::raw::c_char;
use std::path::Path;
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
    // Global: a room filter naming a room id that cannot exist (Synapse treats
    // an empty `rooms` list as "no filter" on some versions, which would mean
    // every room) plus `not_rooms: ["*"]`, so the server has no room to
    // gather. The response is still capped.
    let rooms: Vec<&str> = if room_id.is_empty() { vec![] } else { vec![room_id] };
    let not_rooms: Vec<&str> = if room_id.is_empty() { vec!["*"] } else { vec![] };
    json!({
        "presence": { "types": [] },
        "account_data": { "limit": if room_id.is_empty() { 1000 } else { 0 } },
        "room": {
            "rooms": rooms,
            "not_rooms": not_rooms,
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

/// How long compacting a cache database may take before the reset reports it
/// as not done. VACUUM rewrites the file, so it scales with what is left.
const COMPACT_BUDGET: Duration = Duration::from_secs(90);

/// Bytes a path occupies on disk: the file itself, or every regular file under
/// a directory. Symlinks are neither followed nor counted. A missing path is 0.
pub(crate) fn tree_bytes(path: &Path) -> u64 {
    let Ok(meta) = std::fs::symlink_metadata(path) else {
        return 0;
    };
    if meta.is_file() {
        return meta.len();
    }
    if !meta.is_dir() {
        return 0;
    }
    let Ok(entries) = std::fs::read_dir(path) else {
        return 0;
    };
    entries.flatten().map(|entry| tree_bytes(&entry.path())).sum()
}

/// A SQLite database's footprint: the file and its `-wal`, `-shm` and
/// `-journal` siblings. After a clear most of that can still be free pages
/// (and a long WAL), which is what a VACUUM gives back.
pub(crate) fn sqlite_family_bytes(db: &Path) -> u64 {
    let mut total = tree_bytes(db);
    for suffix in ["-wal", "-shm", "-journal"] {
        let mut name = db.as_os_str().to_owned();
        name.push(suffix);
        total += tree_bytes(Path::new(&name));
    }
    total
}

/// The event cache database's on-disk size, where this session opened it.
pub(crate) fn event_cache_disk_bytes(store_path: &Path) -> u64 {
    if store_path.as_os_str().is_empty() {
        return 0;
    }
    let placement = crate::cachelayout::placement_of(store_path);
    sqlite_family_bytes(&placement.event_cache.join(crate::cachelayout::EVENT_CACHE_DB))
}

/// The in-use SQLite media store's on-disk size, 0 while media is held in
/// memory. The store in use is the one mediastore opened: the old plaintext
/// `<store>/matrix-sdk-media.sqlite3` while it is still active (Legacy), else
/// the encrypted store's directory in the cache directory, so the figures
/// match the store that was vacuumed.
pub(crate) fn media_store_disk_bytes(store_path: &Path) -> u64 {
    if store_path.as_os_str().is_empty() {
        return 0;
    }
    if matches!(crate::mediastore::active(store_path), crate::mediastore::Active::Legacy) {
        return sqlite_family_bytes(&store_path.join(crate::mediastore::MEDIA_DB));
    }
    let placement = crate::cachelayout::placement_of(store_path);
    tree_bytes(&placement.media_store.join(crate::mediastore::DIR_NAME))
}

/// How one cache database's compaction ended. Absent is not failed
/// (CLAUDE.md section 6): `NotApplicable` means there was no SQLite store to
/// compact, and only `Failed` and `TimedOut` may be worded as a problem.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// The VACUUM ran and the file is smaller.
    Compacted,
    /// The VACUUM ran and the file did not get smaller (nothing to give back,
    /// or a release build of the SDK swallowed a failed VACUUM; the SDK logs
    /// it and returns Ok). Reported as figures only, never as success.
    Unchanged,
    /// No SQLite store exists for this cache (media held in memory, no media
    /// key, no store directory). Not a failure.
    NotApplicable,
    /// The store could not be reached or the VACUUM returned an error.
    Failed,
    /// The budget ran out. The VACUUM is not cancelled by this (the query
    /// keeps running on its pool connection), so the file may still shrink and
    /// no "after" figure is reported.
    TimedOut,
}

impl Outcome {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Outcome::Compacted => "compacted",
            Outcome::Unchanged => "unchanged",
            Outcome::NotApplicable => "not_applicable",
            Outcome::Failed => "failed",
            Outcome::TimedOut => "timed_out",
        }
    }
}

/// One store's compaction: the outcome and its measured on-disk sizes. `after`
/// is 0 (unknown) for `NotApplicable` and `TimedOut`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Compaction {
    pub outcome: Outcome,
    pub before: u64,
    pub after: u64,
}

/// Run one store's `vacuum` future under `budget` and classify the result by
/// the file's measured size, not by the return value alone. `vacuum` is not
/// polled at all when the store is not `applicable`.
pub(crate) async fn compact<E, F, M>(
    applicable: bool,
    budget: Duration,
    before: u64,
    vacuum: F,
    measure_after: M,
) -> Compaction
where
    F: std::future::Future<Output = Result<(), E>>,
    M: FnOnce() -> u64 + Send + 'static,
{
    if !applicable {
        return Compaction { outcome: Outcome::NotApplicable, before: 0, after: 0 };
    }
    match tokio::time::timeout(budget, vacuum).await {
        Err(_) => Compaction { outcome: Outcome::TimedOut, before, after: 0 },
        Ok(result) => {
            let after = tokio::task::spawn_blocking(measure_after).await.unwrap_or(before);
            let outcome = if result.is_err() {
                Outcome::Failed
            } else if after < before {
                Outcome::Compacted
            } else {
                Outcome::Unchanged
            };
            Compaction { outcome, before, after }
        }
    }
}

/// Give a cleared event cache's free pages back to the filesystem, through the
/// SDK's own typed path: the event cache store's lock, then its `optimize()`
/// (which is `vacuum()` for SQLite). That is the path
/// `Client::optimize_stores` takes minus the state store, so the crypto and
/// state stores are not reachable from here, and the store's cross-process
/// lock is respected (a store another process holds is skipped, reported as
/// `Failed`).
pub(crate) async fn compact_event_cache(
    client: &matrix_sdk::Client,
    store_path: &Path,
    before: u64,
) -> Compaction {
    let path = store_path.to_path_buf();
    let vacuum = async {
        let state = client.event_cache_store().lock().await.map_err(|_| ())?;
        match state.as_clean() {
            Some(clean) => clean.optimize().await.map_err(|_| ()),
            None => Err(()),
        }
    };
    compact(
        !store_path.as_os_str().is_empty(),
        COMPACT_BUDGET,
        before,
        vacuum,
        move || event_cache_disk_bytes(&path),
    )
    .await
}

/// The same for the media store, after its entries were cleaned. Only an
/// on-disk SQLite media store is applicable.
pub(crate) async fn compact_media_store(
    client: &matrix_sdk::Client,
    store_path: &Path,
    before: u64,
) -> Compaction {
    let path = store_path.to_path_buf();
    let on_disk = !store_path.as_os_str().is_empty()
        && crate::mediastore::active(store_path).persists();
    let vacuum = async {
        let guard = client.media_store().lock().await.map_err(|_| ())?;
        guard.optimize().await.map_err(|_| ())
    };
    compact(on_disk, COMPACT_BUDGET, before, vacuum, move || media_store_disk_bytes(&path)).await
}

/// Enqueue the later `cache_compacted` event for one store.
pub(crate) fn report_compaction(
    events: &crate::EventQueueRef,
    op_id: u64,
    store: &str,
    result: Compaction,
) {
    enqueue(
        events,
        json!({
            "type": "cache_compacted", "op_id": op_id, "store": store,
            "outcome": result.outcome.as_str(),
            "bytes_before": result.before, "bytes_after": result.after,
        }),
    );
}

/// Result events: `event_cache_cleared { op_id, ok }` as soon as the SDK has
/// cleared the storage (the crypto store is not involved), then, only after a
/// successful clear, a separate `cache_compacted` event for this op carrying
/// the compaction's outcome and measured sizes. Compaction never delays or
/// colours the clear's own result.
pub(crate) fn clear_event_cache(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let store_path = bridge.store_path.clone();
    bridge.spawn_room_action(async move {
        let before = {
            let path = store_path.clone();
            tokio::task::spawn_blocking(move || event_cache_disk_bytes(&path))
                .await
                .unwrap_or(0)
        };
        let ok = client.event_cache().clear_all_rooms().await.is_ok();
        enqueue(&events, json!({ "type": "event_cache_cleared", "op_id": op_id, "ok": ok }));
        // Only a cleared cache is compacted: after a failed clear the file
        // holds live data and a rewrite buys nothing.
        if ok {
            let result = compact_event_cache(&client, &store_path, before).await;
            report_compaction(&events, op_id, "event_cache", result);
        }
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

/// Most sessions listed for one user.
const CLIENT_COUNT_CAP: usize = 64;
/// A session name is whatever its owner typed; it is cut here.
const CLIENT_NAME_CAP: usize = 80;

/// One session name made safe to show: control characters dropped, bounded.
/// Pure and unit-tested. The text is the owner's own and is never trusted as
/// a statement about the software.
pub(crate) fn clean_client_name(name: &str) -> String {
    name.chars().filter(|c| !c.is_control()).take(CLIENT_NAME_CAP).collect::<String>().trim().to_owned()
}

/// The sessions this device's crypto store knows for `user_id`: device id and
/// the display name the owner gave it. That is all the protocol carries about
/// a "client"; nothing is inferred. A user the store does not track yields an
/// empty list, which the UI reports as unknown. Result: `user_clients`.
pub(crate) fn user_clients(
    bridge: &RustClient,
    user_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid = matrix_sdk::ruma::UserId::parse(&user_id).map_err(|_| "invalid user id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut event = json!({
            "type": "user_clients", "op_id": op_id, "lifecycle": lifecycle,
            "user_id": user_id, "ok": false, "devices": [],
        });
        if let Ok(devices) = client.encryption().get_user_devices(&uid).await {
            let mut rows: Vec<Value> = devices
                .devices()
                .map(|d| {
                    json!({
                        "device_id": d.device_id().as_str(),
                        "name": d.display_name().map(clean_client_name).unwrap_or_default(),
                    })
                })
                .collect();
            rows.sort_by(|a, b| a["device_id"].as_str().cmp(&b["device_id"].as_str()));
            rows.truncate(CLIENT_COUNT_CAP);
            event["ok"] = json!(true);
            event["devices"] = json!(rows);
        }
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, event);
        }
    });
    Ok(())
}

/// Sessions of one user as this device knows them. Result: `user_clients`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_user_clients(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        user_clients(handle, user_id, op_id).map(|_| String::new())
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
    fn a_session_name_loses_control_characters_and_is_bounded() {
        assert_eq!(clean_client_name("Element\u{7}\n Web"), "Element Web");
        assert_eq!(clean_client_name(&"x".repeat(500)).chars().count(), CLIENT_NAME_CAP);
        assert_eq!(clean_client_name("  "), "");
    }

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
        assert_eq!((n.as_str(), ver.as_str()), ("Synapse", "1.23"));
        assert_eq!(parse_software(&json!({})), (String::new(), String::new()));
    }

    #[test]
    fn the_sync_filter_asks_for_account_data_only() {
        let f = sync_filter("");
        assert_eq!(f["room"]["rooms"], json!([]));
        assert_eq!(f["room"]["not_rooms"], json!(["*"]));
        assert_eq!(sync_filter("!r:b")["room"]["not_rooms"], json!([]));
        assert_eq!(f["room"]["timeline"]["limit"], 0);
        assert_eq!(sync_filter("!r:b")["room"]["rooms"], json!(["!r:b"]));
    }

    #[test]
    fn disk_sizes_count_a_database_with_its_sidecars_and_ignore_the_rest() {
        let dir = std::env::temp_dir().join(format!("lightning-sizes-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("sub")).unwrap();
        std::fs::write(dir.join("a.sqlite3"), vec![0u8; 1000]).unwrap();
        std::fs::write(dir.join("a.sqlite3-wal"), vec![0u8; 200]).unwrap();
        std::fs::write(dir.join("a.sqlite3-shm"), vec![0u8; 30]).unwrap();
        std::fs::write(dir.join("b.sqlite3"), vec![0u8; 7]).unwrap();
        std::fs::write(dir.join("sub").join("c"), vec![0u8; 5]).unwrap();
        assert_eq!(sqlite_family_bytes(&dir.join("a.sqlite3")), 1230);
        assert_eq!(tree_bytes(&dir), 1242);
        assert_eq!(tree_bytes(&dir.join("missing")), 0);
        assert_eq!(sqlite_family_bytes(&dir.join("missing.sqlite3")), 0);
        let _ = std::fs::remove_dir_all(&dir);
    }

    // The point of the compaction: clearing rows leaves the file as big as it
    // was, and the SDK store's own vacuum() is what gives the space back.
    #[tokio::test]
    async fn vacuuming_the_sdk_store_shrinks_its_file_after_a_clear() {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;
        use matrix_sdk::{SqliteMediaStore, SqliteStoreConfig};
        use matrix_sdk_base::media::store::{IgnoreMediaRetentionPolicy, MediaStore};

        let dir = std::env::temp_dir().join(format!("lightning-vacuum-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let store = SqliteMediaStore::open_with_config(&SqliteStoreConfig::new(&dir))
            .await
            .unwrap();
        let request = MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from("mxc://example.org/Big")),
            format: MediaFormat::File,
        };
        let payload: Vec<u8> = (0..6 * 1024 * 1024).map(|i| (i % 251) as u8).collect();
        store
            .add_media_content(&request, payload, IgnoreMediaRetentionPolicy::Yes)
            .await
            .unwrap();
        let full = tree_bytes(&dir);
        store.remove_media_content(&request).await.unwrap();
        let cleared = tree_bytes(&dir);
        // Through the same classifier the reset uses, not the bare vacuum().
        let measured = dir.clone();
        let result = compact(
            true,
            Duration::from_secs(60),
            cleared,
            store.vacuum(),
            move || tree_bytes(&measured),
        )
        .await;
        assert!(full > 5 * 1024 * 1024, "the payload was not stored: {full}");
        assert_eq!(result.outcome, Outcome::Compacted, "{result:?}");
        assert!(
            result.after < full / 2,
            "vacuum did not return the space: full {full}, cleared {cleared}, after {}",
            result.after
        );
        drop(store);
        let _ = std::fs::remove_dir_all(&dir);
    }

    // The classifier: the verdict follows the measured size, an absent store is
    // not a failure, a timeout reports no "after", and a store that is not
    // applicable is never asked to vacuum.
    #[tokio::test]
    async fn compaction_outcomes_follow_the_measured_size_and_absence_is_not_failure() {
        use std::sync::atomic::{AtomicBool, Ordering};
        let budget = Duration::from_secs(5);

        let shrank = compact(true, budget, 1000, async { Ok::<(), ()>(()) }, || 100).await;
        assert_eq!(shrank, Compaction { outcome: Outcome::Compacted, before: 1000, after: 100 });

        // Ok from the SDK but the file did not shrink (a release build logs a
        // failed VACUUM and returns Ok): never "compacted".
        let same = compact(true, budget, 1000, async { Ok::<(), ()>(()) }, || 1000).await;
        assert_eq!(same.outcome, Outcome::Unchanged);

        let failed = compact(true, budget, 1000, async { Err::<(), ()>(()) }, || 1000).await;
        assert_eq!(failed.outcome, Outcome::Failed);

        // No SQLite store (in-memory media, no key): not applicable, no figures,
        // and the vacuum future is never polled.
        let polled = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&polled);
        let none = compact(
            false,
            budget,
            0,
            async move {
                flag.store(true, Ordering::SeqCst);
                Ok::<(), ()>(())
            },
            || 7,
        )
        .await;
        assert_eq!(none, Compaction { outcome: Outcome::NotApplicable, before: 0, after: 0 });
        assert!(!polled.load(Ordering::SeqCst));
        assert_ne!(none.outcome.as_str(), "failed");

        // A vacuum that outlives the budget: timed out, no "after" figure.
        let slow = compact(
            true,
            Duration::from_millis(20),
            1000,
            std::future::pending::<Result<(), ()>>(),
            || 1,
        )
        .await;
        assert_eq!(slow, Compaction { outcome: Outcome::TimedOut, before: 1000, after: 0 });
    }

    // The compaction budget stays below the C++ side's wait for a compaction
    // report (kCompactionWaitMs in AboutController.h, 100 s).
    #[test]
    fn the_compaction_budget_is_inside_the_uis_wait() {
        assert!(COMPACT_BUDGET < Duration::from_secs(100));
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
