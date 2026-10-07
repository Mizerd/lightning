//! Personal chat backgrounds kept on the homeserver, so they follow the user
//! to every device and survive a sign-out.
//!
//! A personal background is a picture only this account sees (the shared kind
//! is room state, `backdrop.rs`). It lives in ACCOUNT DATA, which only this
//! account can read or write:
//!
//! * global `org.lightning_matrix.backgrounds`: the background for every
//!   room, `{ "version": 1, "default": <record> }`. No `default` means none.
//! * room `org.lightning_matrix.room.background`: this account's own picture
//!   for one room, `{ "version": 1, <record fields> }`; `{}` means none.
//! * global `org.lightning_matrix.backgrounds.settings`: the ACCOUNT-WIDE
//!   switch, `{ "version": 1, "enabled": <bool> }` (absent = on). Its own type
//!   on purpose: a picture write never carries it, so a picture write that
//!   started before another device turned sync off cannot turn it back on
//!   (review R2, 2026-10-07). Every upload and presentation write re-reads it
//!   from the server immediately before its PUT and stops with
//!   `sync_disabled` when it is off.
//!
//! A record is `{ "file": <EncryptedFile>, "info": {...}, "color": "#RRGGBB",
//! "presentation": { "dim", "blur", "tint" (integer percent), "fit", "align" } }`.
//! Every number is an integer: account data is JSON the homeserver stores and
//! Lightning keeps it canonical, as the shared event must be (`backdrop.rs`,
//! the 2026-10-06 M_BAD_JSON). `compose_*` refuses to produce a float.
//!
//! Reading distinguishes CLEARED (an explicit `{}`, or a version-1 object
//! without `default` / `file`) from INVALID (anything malformed: an unreadable
//! file object, no hash, an http url). Only cleared ever removes a local copy;
//! invalid changes nothing.
//!
//! The picture is uploaded ENCRYPTED with the SDK's attachment encryption
//! (`Client::upload_encrypted_file`: AES-256-CTR with a fresh key and IV and a
//! SHA-256 of the ciphertext, the same machinery as encrypted room media), so
//! the media repository holds an opaque blob. Nothing here is custom crypto
//! (CLAUDE.md §6). The key travels in the `file` object, i.e. in account data,
//! which the homeserver (and its administrator) can read: this protects the
//! picture from the media repository and anyone who learns its mxc URI, not
//! from the homeserver itself. Settings says so.
//!
//! The `EncryptedFile` (and with it the key) never crosses to C++: C++ sees a
//! scope, an `id` (the ciphertext's SHA-256, not secret) and the presentation.
//! The decrypted bytes are parked in this module's OWN map
//! (`personal_background_results`, taken with
//! `mx_rust_personal_background_take`), never in the timeline media map: the
//! op ids here come from another counter, and sharing one map by op id could
//! hand a timeline picture from an encrypted room to the background code,
//! which keeps it as a file (review H1, 2026-10-07). The command lane's
//! overflow cleanup is routed by event type for the same reason
//! (`enqueue_terminal_routed`).
//!
//! Every result rides the command lane (op-id terminal events), so a flood of
//! timeline diffs cannot delay or drop it.
//!
//! Scopes: `""` is the every-room default, a room id is that room.

use std::collections::HashMap;
use std::future::Future;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use matrix_sdk::{
    config::RequestConfig,
    event_handler::EventHandlerHandle,
    media::{MediaFormat, MediaRequestParameters},
    ruma::{
        api::client::config::{
            get_global_account_data, get_room_account_data, set_global_account_data,
            set_room_account_data,
        },
        events::{
            macros::EventContent,
            room::{EncryptedFile, MediaSource},
            GlobalAccountDataEvent, GlobalAccountDataEventType, RoomAccountDataEvent,
            RoomAccountDataEventType,
        },
        serde::Raw,
    },
    Client, Room,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use crate::backdrop::{
    classify_write_error, is_canonical_json_safe, is_usable_url, normalise_picture_fields,
    MAX_BACKGROUND_BYTES,
};
use crate::rooms::{joined_room, require_client, sniff_image_mime};
use crate::{enqueue, enqueue_terminal_routed, EventQueueRef, RustClient};

/// Global account data: the every-room personal background.
pub(crate) const GLOBAL_TYPE: &str = "org.lightning_matrix.backgrounds";
/// Room account data: this account's own picture for one room.
pub(crate) const ROOM_TYPE: &str = "org.lightning_matrix.room.background";
/// Global account data: the account-wide switch, and nothing else.
pub(crate) const SETTINGS_TYPE: &str = "org.lightning_matrix.backgrounds.settings";
/// The schema this build reads and writes.
pub(crate) const SCHEMA_VERSION: i64 = 1;

/// One account-data round trip. Runs on the room-action pool, which sign-out
/// joins, so no retry and a hard bound.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(15);
/// One encrypted download, bounded below MediaBridge's own watchdog class for
/// a full payload.
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(60);
/// The upload itself; the SDK sizes its own per-request timeout.
const UPLOAD_TIMEOUT: Duration = Duration::from_secs(120);
/// A whole write (its reads, the upload, the last switch look and the PUT),
/// well inside C++'s 200 s watchdog: a write the watchdog has given up on
/// must not still land afterwards.
const WRITE_DEADLINE: Duration = Duration::from_secs(150);
/// Most rooms one "remove from the server" pass writes.
const MAX_CLEAR_ROOMS: usize = 256;
/// Most decrypted pictures parked at once. C++ takes each one as its event is
/// handled; an event that never arrives (a stale session) must not keep
/// pictures in memory, so the oldest go first.
const MAX_PARKED: usize = 4;

/// The last switch GET a read made, shared by concurrent reads (startup pokes
/// arrive in a burst). Only an answer whose GET STARTED after a read began
/// its entries is reused for that read (`switch_after`).
pub(crate) type SwitchCache = Arc<tokio::sync::Mutex<Option<SwitchSeen>>>;

#[derive(Clone, Copy, Debug)]
pub(crate) struct SwitchSeen {
    lifecycle: u64,
    asked_at: Instant,
    on: bool,
}

/// Decrypted pictures waiting for `mx_rust_personal_background_take`, keyed by
/// this module's op id. Separate from `media_results` on purpose (see the
/// module docs).
pub(crate) type ParkedPictures = Arc<Mutex<HashMap<u64, Vec<u8>>>>;

/// Park one picture, dropping the oldest beyond `MAX_PARKED`.
pub(crate) fn park(parked: &ParkedPictures, op_id: u64, bytes: Vec<u8>) {
    if let Ok(mut map) = parked.lock() {
        while map.len() >= MAX_PARKED {
            let Some(oldest) = map.keys().min().copied() else { break };
            map.remove(&oldest);
        }
        map.insert(op_id, bytes);
    }
}

/// The two parked maps a terminal event of this module may own, for the
/// command lane's routed overflow cleanup.
#[derive(Clone)]
struct Lanes {
    terminal: EventQueueRef,
    media: Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    parked: ParkedPictures,
}

impl Lanes {
    fn of(bridge: &RustClient) -> Self {
        Self {
            terminal: Arc::clone(&bridge.command_events),
            media: Arc::clone(&bridge.media_results),
            parked: Arc::clone(&bridge.personal_background_results),
        }
    }

    fn send(&self, value: Value) {
        enqueue_terminal_routed(&self.terminal, Some(&self.media), Some(&self.parked), value);
    }
}

/// One scope's server copy, as read.
#[derive(Debug, PartialEq)]
pub(crate) enum Entry {
    /// No background: `{}`, or a version-1 object without `default` / `file`.
    Cleared,
    /// Malformed: the server holds something this reader cannot use. Changes
    /// nothing locally (never read as "removed").
    Invalid,
    /// A newer schema this build must not guess at.
    Unsupported(i64),
    Present(Present),
}

#[derive(Debug, PartialEq)]
pub(crate) struct Present {
    /// The canonical `EncryptedFile` JSON: the key material. Stays in Rust.
    pub(crate) file: Value,
    /// The ciphertext's SHA-256 (unpadded base64): names this upload.
    pub(crate) id: String,
    /// The canonical record (file, info, color, presentation).
    pub(crate) record: Value,
}

/// Bare content from a GET, or a full event from the store (`content` used).
fn content_of(raw: &Value) -> &Value {
    match raw.get("content") {
        Some(inner) if inner.is_object() && raw.get("type").is_some() => inner,
        _ => raw,
    }
}

/// `Some(version)` for a usable version (missing = 1), `None` when it is not
/// a positive integer, which is not a schema at all.
fn version_of(object: &Map<String, Value>) -> Option<i64> {
    match object.get("version") {
        None => Some(SCHEMA_VERSION),
        Some(v) => v.as_i64().filter(|n| *n >= 1),
    }
}

/// One record: a usable encrypted file and the clamped picture fields.
pub(crate) fn parse_record(object: &Map<String, Value>) -> Option<Present> {
    let raw_file = object.get("file")?;
    // The SDK's own type decides what an encrypted file is; anything it
    // cannot read (a plain url, a missing key) is not a background.
    let file: EncryptedFile = serde_json::from_value(raw_file.clone()).ok()?;
    if !is_usable_url(file.url.as_str()) {
        return None;
    }
    // Re-serialised from the typed value, so unknown keys never survive.
    let file_json = serde_json::to_value(&file).ok()?;
    let id = file_json
        .get("hashes")
        .and_then(|h| h.get("sha256"))
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty() && s.len() <= 128)?
        .to_owned();
    // Decryption verifies this hash; without key and iv nothing decrypts.
    if file_json.get("key").is_none() || file_json.get("iv").is_none() {
        return None;
    }
    let mut record = Map::new();
    record.insert("file".into(), file_json.clone());
    normalise_picture_fields(object, &mut record);
    Some(Present { file: file_json, id, record: Value::Object(record) })
}

/// The account-wide switch: false only when the settings object says
/// `"enabled": false` explicitly (whatever its version), true otherwise.
pub(crate) fn switch_on(settings: &Value) -> bool {
    content_of(settings).get("enabled").and_then(Value::as_bool) != Some(false)
}

/// The settings object for the switch.
pub(crate) fn compose_settings(on: bool) -> Value {
    json!({ "version": SCHEMA_VERSION, "enabled": on })
}

/// The global object (`org.lightning_matrix.backgrounds`).
pub(crate) fn parse_global(raw: &Value) -> Entry {
    let Some(object) = content_of(raw).as_object() else {
        return Entry::Invalid;
    };
    let Some(version) = version_of(object) else {
        return Entry::Invalid;
    };
    if version > SCHEMA_VERSION {
        return Entry::Unsupported(version);
    }
    match object.get("default") {
        None | Some(Value::Null) => Entry::Cleared,
        Some(Value::Object(record)) => match parse_record(record) {
            Some(present) => Entry::Present(present),
            None => Entry::Invalid,
        },
        Some(_) => Entry::Invalid,
    }
}

/// One room's object (`org.lightning_matrix.room.background`).
pub(crate) fn parse_room(raw: &Value) -> Entry {
    let Some(object) = content_of(raw).as_object() else {
        return Entry::Invalid;
    };
    let Some(version) = version_of(object) else {
        return Entry::Invalid;
    };
    // Before anything else: a newer schema may name its fields differently,
    // and reading it as "cleared" would delete the local copy.
    if version > SCHEMA_VERSION {
        return Entry::Unsupported(version);
    }
    match object.get("file") {
        None | Some(Value::Null) => Entry::Cleared,
        Some(_) => match parse_record(object) {
            Some(present) => Entry::Present(present),
            None => Entry::Invalid,
        },
    }
}

pub(crate) fn parse_scope(scope: &str, raw: &Value) -> Entry {
    if scope.is_empty() {
        parse_global(raw)
    } else {
        parse_room(raw)
    }
}

/// The record to WRITE: `file` from the upload (or the current one), every
/// other field from what C++ asked for, through the read normaliser, so
/// Lightning never writes anything it would not read back identically.
pub(crate) fn compose_record(file: &Value, requested: &Value) -> Option<Value> {
    let mut candidate = match requested.as_object() {
        Some(object) => object.clone(),
        None => Map::new(),
    };
    candidate.insert("file".into(), file.clone());
    let present = parse_record(&candidate)?;
    is_canonical_json_safe(&present.record).then_some(present.record)
}

/// The account-data content for one picture scope: a record, or `None` to
/// clear. Never carries the switch (that is `SETTINGS_TYPE`).
pub(crate) fn compose_scope(scope: &str, record: Option<Value>) -> Value {
    let content = if scope.is_empty() {
        match record {
            Some(record) => json!({ "version": SCHEMA_VERSION, "default": record }),
            None => json!({ "version": SCHEMA_VERSION }),
        }
    } else {
        match record {
            Some(Value::Object(mut fields)) => {
                fields.insert("version".into(), json!(SCHEMA_VERSION));
                Value::Object(fields)
            }
            _ => json!({}),
        }
    };
    debug_assert!(is_canonical_json_safe(&content));
    content
}

/// What C++ receives for one scope: never the file (the key).
pub(crate) fn entry_json(scope: &str, entry: &Entry) -> Value {
    let mut out = Map::new();
    out.insert("scope".into(), json!(scope));
    match entry {
        Entry::Cleared => {
            out.insert("state".into(), json!("cleared"));
        }
        Entry::Invalid => {
            out.insert("state".into(), json!("invalid"));
        }
        Entry::Unsupported(version) => {
            out.insert("state".into(), json!("unsupported"));
            out.insert("schema_version".into(), json!(version));
        }
        Entry::Present(present) => {
            out.insert("state".into(), json!("present"));
            out.insert("id".into(), json!(present.id));
            for key in ["info", "color", "presentation"] {
                if let Some(value) = present.record.get(key) {
                    out.insert(key.into(), value.clone());
                }
            }
        }
    }
    Value::Object(out)
}

fn bounded() -> RequestConfig {
    RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT)
}

fn global_type(scope: &str) -> GlobalAccountDataEventType {
    GlobalAccountDataEventType::from(if scope == SETTINGS_TYPE { SETTINGS_TYPE } else { GLOBAL_TYPE })
}

/// The raw stored copy of one scope (or of the settings, `SETTINGS_TYPE`),
/// from the state store (what sync wrote).
async fn read_store(client: &Client, scope: &str) -> Option<Value> {
    if scope.is_empty() || scope == SETTINGS_TYPE {
        let raw = client.account().account_data_raw(global_type(scope)).await.ok().flatten()?;
        serde_json::from_str(raw.json().get()).ok()
    } else {
        let room = client.get_room(&matrix_sdk::ruma::RoomId::parse(scope).ok()?)?;
        let raw = room
            .account_data(RoomAccountDataEventType::from(ROOM_TYPE))
            .await
            .ok()
            .flatten()?;
        serde_json::from_str(raw.json().get()).ok()
    }
}

/// The server's copy of one scope (or the settings): `Ok(None)` when it has
/// none (404), `Err` when it could not be asked. Unparseable JSON is
/// `Some(Null)`, which every parser reads as invalid.
async fn read_server(client: &Client, scope: &str) -> Result<Option<Value>, String> {
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let answer = if scope.is_empty() || scope == SETTINGS_TYPE {
        let request = get_global_account_data::v3::Request::new(own, global_type(scope));
        client
            .send(request)
            .with_request_config(bounded())
            .await
            .map(|r| r.account_data.json().get().to_owned())
    } else {
        let room_id = matrix_sdk::ruma::RoomId::parse(scope).map_err(|_| "invalid".to_owned())?;
        let request = get_room_account_data::v3::Request::new(
            own,
            room_id,
            RoomAccountDataEventType::from(ROOM_TYPE),
        );
        client
            .send(request)
            .with_request_config(bounded())
            .await
            .map(|r| r.account_data.json().get().to_owned())
    };
    match answer {
        Ok(json) => Ok(Some(serde_json::from_str(&json).unwrap_or(Value::Null))),
        Err(err) => {
            let not_found = err
                .as_client_api_error()
                .is_some_and(|e| e.status_code.as_u16() == 404);
            if not_found {
                Ok(None)
            } else {
                Err(classify_write_error(&err.to_string()).0.to_owned())
            }
        }
    }
}

/// The server's copy of one scope, never the store's. The store holds what
/// sync last delivered and lags this device's OWN writes until their echo
/// comes back, so a read made just after a write would report the copy that
/// write replaced: measured live, the switch read back "off" 100 ms after
/// this device had turned it on, and a stale "cleared" read the same way
/// would delete the picture the user had just chosen.
async fn read_scope(client: &Client, scope: &str) -> Result<Option<Entry>, String> {
    Ok(read_server(client, scope).await?.map(|raw| parse_scope(scope, &raw)))
}

/// The picture a download fetches: the stored copy when it is the one asked
/// for (the usual case, a change poke has just put it there), else the
/// server's, which must be it too. Never a different picture than the one
/// C++ decided to fetch: a lagging store would otherwise hand back the copy
/// that was replaced.
fn wanted_picture(entry: Option<Entry>, expected: &str) -> Option<Present> {
    match entry {
        Some(Entry::Present(present)) if !expected.is_empty() && present.id == expected => {
            Some(present)
        }
        _ => None,
    }
}

/// The switch as the SERVER holds it now (no settings object = on).
async fn read_switch_server(client: &Client) -> Result<bool, String> {
    Ok(read_server(client, SETTINGS_TYPE).await?.map(|raw| switch_on(&raw)).unwrap_or(true))
}

/// A read's entries FIRST, its switch AFTER (review N1). "Remove server
/// copies" turns the switch off BEFORE it clears anything, so a "cleared" a
/// read sees is always followed by an "off" the same read sees too, and C++
/// (which applies the switch before the entries) makes the marks dormant
/// before it looks at the "cleared". The other order let a read take the
/// switch while it was still on, then a scope cleared just after: "on" plus
/// "cleared" plus a live mark deleted the picture.
pub(crate) async fn entries_then_switch<E, FE, FS>(
    entries: FE,
    switch: impl FnOnce() -> FS,
) -> (E, Option<bool>)
where
    FE: Future<Output = E>,
    FS: Future<Output = Option<bool>>,
{
    let entries = entries.await;
    let switch = switch().await;
    (entries, switch)
}

/// The switch from a GET that started at or after `not_before`: one shared
/// with concurrent reads when there is one, else a new GET. Never an older
/// answer. `None` when it could not be learned.
pub(crate) async fn switch_after<F>(
    cache: &SwitchCache,
    lifecycle: u64,
    not_before: Instant,
    ask: F,
) -> Option<bool>
where
    F: Future<Output = Result<bool, String>>,
{
    let mut seen = cache.lock().await;
    if let Some(last) = *seen {
        if last.lifecycle == lifecycle && last.asked_at >= not_before {
            return Some(last.on);
        }
    }
    let asked_at = Instant::now();
    let on = ask.await.ok()?;
    *seen = Some(SwitchSeen { lifecycle, asked_at, on });
    Some(on)
}

/// A read's entries, then its switch from a GET that started only once the
/// entries were COMPLETE (review F1). A GET that started while they were
/// still being read (another read's, shared) can answer "on" before a
/// removal that a later scope of these entries already shows as "cleared";
/// "cleared" + "on" deletes. Only after the last entry is every "cleared"
/// they hold certain to be followed by the "off" written before it.
pub(crate) async fn entries_then_shared_switch<E, FE, FA>(
    cache: &SwitchCache,
    lifecycle: u64,
    entries: FE,
    ask: impl FnOnce() -> FA,
) -> (E, Option<bool>)
where
    FE: Future<Output = E>,
    FA: Future<Output = Result<bool, String>>,
{
    entries_then_switch(entries, || {
        let entries_done = Instant::now();
        switch_after(cache, lifecycle, entries_done, ask())
    })
    .await
}

/// Validate a scope at the FFI: the default, or a room id.
fn check_scope(scope: &str) -> Result<(), String> {
    if scope.is_empty() || (scope.starts_with('!') && scope.len() <= 255) {
        Ok(())
    } else {
        Err("invalid_scope".to_owned())
    }
}

/// Read one scope (`""`, a room id) or every scope (`"*"`: the default from
/// the server, every joined room from the store, since one request per room
/// at every start is too many; C++ asks the server again for any room it
/// wrote this session, the one case where the store can lag). Emits `personal_backgrounds { op_id, lifecycle,
/// scope, entries, failed, enabled }` on the command lane; a scope with no
/// account data at all is omitted (unknown, not cleared), one that could not
/// be read is listed in `failed`. EVERY read carries the switch (`enabled`,
/// null when unknown), read AFTER the entries (`entries_then_switch`), so C++
/// applies it before any entry: a room read that answers "cleared" because
/// another device removed the server copies must arrive together with the
/// switch that says sync is off.
pub(crate) fn read(bridge: &RustClient, op_id: u64, scope: String) -> Result<(), String> {
    if scope != "*" {
        check_scope(&scope)?;
    }
    let client = require_client(bridge)?;
    let lanes = Lanes::of(bridge);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let switch_cache = Arc::clone(&bridge.personal_switch_cache);
    bridge.spawn_room_action(async move {
        // Requests this read made (for the start-up cost line C++ logs).
        let gets = std::sync::atomic::AtomicUsize::new(0);
        let count = || {
            gets.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        };
        let read_entries = async {
            let mut entries = Vec::new();
            let mut failed = Vec::new();
            let scopes: Vec<String> = if scope == "*" {
                std::iter::once(String::new())
                    .chain(client.joined_rooms().iter().map(|r| r.room_id().to_string()))
                    .collect()
            } else {
                vec![scope.clone()]
            };
            for one in &scopes {
                // Every room from the store only: asking the server once per
                // room would be a request per joined room at every start. A
                // room's own read (`scope` = that room) asks the server.
                let result = if scope == "*" && !one.is_empty() {
                    Ok(read_store(&client, one).await.map(|raw| parse_scope(one, &raw)))
                } else {
                    count();
                    read_scope(&client, one).await
                };
                match result {
                    Ok(Some(entry)) => entries.push(entry_json(one, &entry)),
                    Ok(None) => {}
                    Err(_) => failed.push(json!(one)),
                }
            }
            (entries, failed)
        };
        let ((entries, failed), enabled) =
            entries_then_shared_switch(&switch_cache, lifecycle, read_entries, || async {
                count();
                read_switch_server(&client).await
            })
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        lanes.send(json!({
            "type": "personal_backgrounds",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "scope": scope,
            "entries": entries,
            "failed": failed,
            "enabled": enabled,
            "gets": gets.load(std::sync::atomic::Ordering::Relaxed),
        }));
    });
    Ok(())
}

/// Download and decrypt one scope's picture. The decrypted bytes are parked
/// under `op_id` in `personal_background_results` for
/// `mx_rust_personal_background_take`; the event is
/// `personal_background_bytes { op_id, lifecycle, scope, ok, entry, size,
/// category }`. The SDK decryptor verifies the ciphertext hash, so a
/// corrupted or substituted blob fails rather than decodes. `expected_id` is
/// the picture C++ decided to fetch; another one is refused as "changed".
pub(crate) fn download(
    bridge: &RustClient,
    op_id: u64,
    scope: String,
    expected_id: String,
) -> Result<(), String> {
    check_scope(&scope)?;
    let client = require_client(bridge)?;
    let lanes = Lanes::of(bridge);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let outcome: Result<(Value, Vec<u8>), String> = async {
            let stored = read_store(&client, &scope).await.map(|raw| parse_scope(&scope, &raw));
            let present = match wanted_picture(stored, &expected_id) {
                Some(present) => present,
                None => match read_scope(&client, &scope).await? {
                    Some(Entry::Present(present))
                        if expected_id.is_empty() || present.id == expected_id =>
                    {
                        present
                    }
                    Some(Entry::Present(_)) => return Err("changed".to_owned()),
                    _ => return Err("no_picture".to_owned()),
                },
            };
            let file: EncryptedFile =
                serde_json::from_value(present.file.clone()).map_err(|_| "invalid".to_owned())?;
            let request = MediaRequestParameters {
                source: MediaSource::Encrypted(Box::new(file)),
                format: MediaFormat::File,
            };
            // Never cached by the SDK: the one local copy is the file C++
            // writes into the account's backgrounds directory.
            let bytes = tokio::time::timeout(
                DOWNLOAD_TIMEOUT,
                crate::mediafetch::get_media_content_bounded(
                    &client,
                    &request,
                    false,
                    DOWNLOAD_TIMEOUT,
                ),
            )
            .await
            .map_err(|_| "timeout".to_owned())?
            .map_err(|err| crate::mediafetch::classify_media_error(&err).to_owned())?;
            if bytes.is_empty() || bytes.len() as u64 > MAX_BACKGROUND_BYTES {
                return Err("too_large".to_owned());
            }
            if sniff_image_mime(&bytes).is_none() {
                return Err("unsupported_image".to_owned());
            }
            Ok((entry_json(&scope, &Entry::Present(present)), bytes))
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok((entry, bytes)) => {
                let size = bytes.len();
                park(&lanes.parked, op_id, bytes);
                lanes.send(json!({
                    "type": "personal_background_bytes",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "scope": scope,
                    "ok": true,
                    "entry": entry,
                    "size": size,
                    "category": "",
                }));
            }
            Err(category) => lanes.send(json!({
                "type": "personal_background_bytes",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "scope": scope,
                "ok": false,
                "entry": Value::Null,
                "size": 0,
                "category": category,
            })),
        }
    });
    Ok(())
}

/// What a write does.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum WriteMode {
    /// Encrypt and upload `local_path`, then point the scope at it.
    Upload,
    /// Keep the scope's current picture, with new presentation. Refused with
    /// "changed" unless the server still holds `expected_id`: another device
    /// may have replaced the picture, and a presentation write must never
    /// adopt (and so mark as mirrored) a picture this device never saw.
    Presentation,
    /// Remove the scope's background (the media itself cannot be deleted).
    Clear,
    /// Set the account-wide switch (`enabled` in the request); scope "" only.
    /// Writes `SETTINGS_TYPE` and nothing else.
    SetEnabled,
}

impl WriteMode {
    pub(crate) fn from_ffi(mode: u32) -> Option<Self> {
        match mode {
            0 => Some(Self::Upload),
            1 => Some(Self::Presentation),
            2 => Some(Self::Clear),
            3 => Some(Self::SetEnabled),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Upload => "upload",
            Self::Presentation => "presentation",
            Self::Clear => "clear",
            Self::SetEnabled => "set_enabled",
        }
    }

    /// Writes that put a picture (or its presentation) on the server: they
    /// need the switch on, checked again right before the PUT.
    pub(crate) fn needs_switch(self) -> bool {
        matches!(self, Self::Upload | Self::Presentation)
    }
}

/// What a picture write sends, decided from the server's current copy. Pure,
/// so the rules (switch, expected id, newer schema) are testable without a
/// server. `uploaded` is the encrypted file of an Upload. The result never
/// carries the switch.
pub(crate) fn plan_write(
    scope: &str,
    mode: WriteMode,
    requested: &Value,
    current_raw: Option<&Value>,
    switch_is_on: bool,
    uploaded: Option<&Value>,
) -> Result<Value, &'static str> {
    if mode == WriteMode::SetEnabled {
        return Err("invalid_mode");
    }
    let current = current_raw.map(|raw| parse_scope(scope, raw));
    if let Some(Entry::Unsupported(_)) = current {
        return Err("newer_schema");
    }
    if mode.needs_switch() && !switch_is_on {
        return Err("sync_disabled");
    }
    let record = match mode {
        WriteMode::Clear | WriteMode::SetEnabled => None,
        WriteMode::Presentation => {
            let expected = requested.get("expected_id").and_then(Value::as_str).unwrap_or("");
            match current {
                Some(Entry::Present(present)) if !expected.is_empty() && present.id == expected => {
                    Some(compose_record(&present.file, requested).ok_or("invalid_content")?)
                }
                _ => return Err("changed"),
            }
        }
        WriteMode::Upload => {
            // A migration (a picture chosen before sync, or while it was
            // off) goes only where the server holds nothing: every device
            // turned back on at once would otherwise overwrite the others'
            // pictures in turn, and each loser would download the winner's
            // over its own without being asked. A different picture there is
            // a conflict for the user, so "changed" sends C++ to read it.
            if only_if_empty(requested) && !matches!(current, None | Some(Entry::Cleared)) {
                return Err("changed");
            }
            let file = uploaded.ok_or("invalid_content")?;
            Some(compose_record(file, requested).ok_or("invalid_content")?)
        }
    };
    Ok(compose_scope(scope, record))
}

/// An upload that may only fill an empty scope (a migration).
fn only_if_empty(requested: &Value) -> bool {
    requested.get("only_if_empty").and_then(Value::as_bool).unwrap_or(false)
}

/// The PUT of a picture write, behind a last look at the switch: when the
/// write needs it on, `switch_now` (the server's switch, read immediately
/// before) must say so, or nothing is sent and the write stops with
/// `sync_disabled`. Another device may have turned sync off during an upload
/// of up to two minutes (review R2).
pub(crate) async fn put_if_switch_on<S, P>(
    needs_switch: bool,
    switch_now: S,
    put: P,
) -> Result<(), (&'static str, String)>
where
    S: Future<Output = Result<bool, String>>,
    P: Future<Output = Result<(), String>>,
{
    if needs_switch {
        match switch_now.await {
            Ok(true) => {}
            Ok(false) => return Err(("send", "sync_disabled".to_owned())),
            Err(category) => return Err(("read", category)),
        }
    }
    put.await.map_err(|category| ("send", category))
}

async fn put_global(client: &Client, event_type: &str, content: &Value) -> Result<(), String> {
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

async fn put_scope(client: &Client, scope: &str, content: Value) -> Result<(), String> {
    if scope.is_empty() {
        return put_global(client, GLOBAL_TYPE, &content).await;
    }
    if !is_canonical_json_safe(&content) {
        return Err("invalid_content".to_owned());
    }
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let room_id = matrix_sdk::ruma::RoomId::parse(scope).map_err(|_| "invalid".to_owned())?;
    let raw = Raw::new(&content).map_err(|_| "invalid_content".to_owned())?.cast_unchecked();
    let request = set_room_account_data::v3::Request::new_raw(
        own,
        room_id,
        RoomAccountDataEventType::from(ROOM_TYPE),
        raw,
    );
    client
        .send(request)
        .with_request_config(bounded())
        .await
        .map(|_| ())
        .map_err(|err| classify_write_error(&err.to_string()).0.to_owned())
}

/// Set, re-present or clear one scope, or set the switch. Emits
/// `personal_background_written { op_id, lifecycle, scope, mode, ok, entry,
/// enabled, category, stage }` on the command lane; `entry` is what the scope
/// holds now (present or cleared), `enabled` the switch a SetEnabled wrote.
pub(crate) fn write(
    bridge: &RustClient,
    op_id: u64,
    scope: String,
    mode: u32,
    local_path: String,
    requested_json: String,
) -> Result<(), String> {
    check_scope(&scope)?;
    let mode = WriteMode::from_ffi(mode).ok_or_else(|| "invalid_mode".to_owned())?;
    let client = require_client(bridge)?;
    if mode == WriteMode::SetEnabled && !scope.is_empty() {
        return Err("invalid_scope".to_owned());
    }
    if !scope.is_empty() {
        // Room account data for a room this account is not in is refused.
        joined_room(&client, &scope)?;
    }
    let requested: Value = if requested_json.trim().is_empty() {
        json!({})
    } else {
        serde_json::from_str(&requested_json).map_err(|_| "invalid_content".to_owned())?
    };
    if mode == WriteMode::Upload {
        let metadata = std::fs::metadata(&local_path).map_err(|_| "read_failed".to_owned())?;
        if !metadata.is_file() {
            return Err("not_a_file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_BACKGROUND_BYTES {
            return Err("file_too_large".to_owned());
        }
    }
    let lanes = Lanes::of(bridge);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let work = async {
            if mode == WriteMode::SetEnabled {
                let on = requested
                    .get("enabled")
                    .and_then(Value::as_bool)
                    .ok_or(("compose", "invalid_content".to_owned()))?;
                put_global(&client, SETTINGS_TYPE, &compose_settings(on))
                    .await
                    .map_err(|category| ("send", category))?;
                return Ok((None, Some(on)));
            }
            // The server's copy decides a presentation change and a newer
            // schema; the switch is read now (before an upload) and again
            // immediately before the PUT.
            let current = read_server(&client, &scope)
                .await
                .map_err(|category| ("read", category))?;
            let switch_before = if mode.needs_switch() {
                read_switch_server(&client).await.map_err(|category| ("read", category))?
            } else {
                true
            };
            let uploaded = if mode == WriteMode::Upload {
                if !switch_before {
                    return Err(("read", "sync_disabled".to_owned()));
                }
                let data = tokio::fs::read(&local_path)
                    .await
                    .map_err(|_| ("read", "read_failed".to_owned()))?;
                // Bounded again on what was actually read: the file may have
                // changed since the synchronous check.
                if data.is_empty() || data.len() as u64 > MAX_BACKGROUND_BYTES {
                    return Err(("read", "file_too_large".to_owned()));
                }
                // The CONTENT decides the type; SVG and anything else
                // non-raster is refused here as in every upload path.
                let mime = sniff_image_mime(&data)
                    .ok_or(("sniff", "unsupported_image".to_owned()))?;
                let size = data.len();
                let mut cursor = std::io::Cursor::new(data);
                let file = tokio::time::timeout(
                    UPLOAD_TIMEOUT,
                    client
                        .upload_encrypted_file(&mut cursor)
                        .with_request_config(RequestConfig::new().retry_limit(2)),
                )
                .await
                .map_err(|_| ("upload", "timeout".to_owned()))?
                .map_err(|err| ("upload", classify_write_error(&err.to_string()).0.to_owned()))?;
                let file =
                    serde_json::to_value(&file).map_err(|_| ("upload", "invalid".to_owned()))?;
                Some((file, mime, size))
            } else {
                None
            };
            // A migration decides on the server's copy as it is NOW, after
            // the upload (seconds in which another device may have filled
            // the scope), not as it was before it.
            let current = if uploaded.is_some() && only_if_empty(&requested) {
                read_server(&client, &scope).await.map_err(|category| ("read", category))?
            } else {
                current
            };
            // The sniffed type and the real size, whatever C++ said.
            let mut asked = requested.clone();
            if let (Some((_, mime, size)), Some(object)) = (&uploaded, asked.as_object_mut()) {
                let mut info = object
                    .get("info")
                    .and_then(Value::as_object)
                    .cloned()
                    .unwrap_or_default();
                info.insert("mimetype".into(), json!(mime));
                info.insert("size".into(), json!(size));
                object.insert("info".into(), Value::Object(info));
            }
            let content = plan_write(
                &scope,
                mode,
                &asked,
                current.as_ref(),
                switch_before,
                uploaded.as_ref().map(|(file, _, _)| file),
            )
            .map_err(|category| ("compose", category.to_owned()))?;
            put_if_switch_on(
                mode.needs_switch(),
                read_switch_server(&client),
                put_scope(&client, &scope, content.clone()),
            )
            .await?;
            Ok((Some(parse_scope(&scope, &content)), None))
        };
        let result: Result<(Option<Entry>, Option<bool>), (&'static str, String)> =
            match tokio::time::timeout(WRITE_DEADLINE, work).await {
                Ok(done) => done,
                Err(_) => Err(("deadline", "timeout".to_owned())),
            };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok((entry, enabled)) => lanes.send(json!({
                "type": "personal_background_written",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "scope": scope,
                "mode": mode.name(),
                "ok": true,
                "entry": entry.map(|e| entry_json(&scope, &e)).unwrap_or(Value::Null),
                "enabled": enabled,
                "category": "",
                "stage": "done",
            })),
            Err((stage, category)) => lanes.send(json!({
                "type": "personal_background_written",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "scope": scope,
                "mode": mode.name(),
                "ok": false,
                "entry": Value::Null,
                "enabled": Value::Null,
                "category": category,
                "stage": stage,
            })),
        }
    });
    Ok(())
}

/// The outcome of a removal pass, counted honestly: every scope it could not
/// clear is `failed` (the write was refused) or `skipped` (not attempted: a
/// room this account is no longer in, a newer schema, past the bound).
#[derive(Debug, Default, PartialEq)]
pub(crate) struct ClearCount {
    pub(crate) cleared: u32,
    pub(crate) failed: u32,
    pub(crate) skipped: u32,
    /// The switch went off on the server (the first write of the pass).
    pub(crate) switched_off: bool,
}

impl ClearCount {
    pub(crate) fn ok(&self) -> bool {
        self.failed == 0 && self.skipped == 0 && self.switched_off
    }
}

/// Which scopes a removal pass writes, and how many it must skip. Pure.
/// `stored` is every joined room with its stored entry; `known` are rooms C++
/// holds copies for; `joined` says whether a room is joined.
pub(crate) fn plan_clear(
    stored: &[(String, Entry)],
    known: &[String],
    joined: &dyn Fn(&str) -> bool,
) -> (Vec<String>, u32) {
    let mut targets: Vec<String> = Vec::new();
    let mut skipped = 0u32;
    for (room, entry) in stored {
        match entry {
            Entry::Present(_) | Entry::Invalid => {
                if targets.len() >= MAX_CLEAR_ROOMS {
                    skipped += 1;
                } else if !targets.contains(room) {
                    targets.push(room.clone());
                }
            }
            Entry::Unsupported(_) => skipped += 1,
            Entry::Cleared => {}
        }
    }
    for room in known {
        if targets.contains(room) || stored.iter().any(|(r, e)| r == room && *e == Entry::Cleared) {
            continue;
        }
        if stored.iter().any(|(r, e)| r == room && matches!(e, Entry::Unsupported(_))) {
            continue; // already counted
        }
        if !joined(room) || targets.len() >= MAX_CLEAR_ROOMS {
            skipped += 1;
        } else {
            targets.push(room.clone());
        }
    }
    (targets, skipped)
}

/// Turn the switch off, then remove every server copy: the settings object
/// becomes `{"version": 1, "enabled": false}` FIRST (so another device sees
/// sync off before it sees any scope cleared, and keeps its own pictures),
/// then the global object `{"version": 1}`, then every joined room whose
/// stored account data holds one, plus the rooms C++ knows of
/// (`known_rooms`, JSON array), gets `{}`. The uploaded media stays on the
/// media repository, which offers users no delete. Emits
/// `personal_backgrounds_cleared { op_id, lifecycle, ok, cleared, failed,
/// skipped, switched_off }` on the command lane.
pub(crate) fn clear_all(bridge: &RustClient, op_id: u64, known_rooms_json: String) -> Result<(), String> {
    let client = require_client(bridge)?;
    let known: Vec<String> = serde_json::from_str::<Vec<String>>(&known_rooms_json)
        .unwrap_or_default()
        .into_iter()
        .filter(|r| check_scope(r).is_ok() && !r.is_empty())
        .collect();
    let lanes = Lanes::of(bridge);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut count = ClearCount::default();
        count.switched_off = put_global(&client, SETTINGS_TYPE, &compose_settings(false))
            .await
            .is_ok();
        if count.switched_off {
            match read_server(&client, "").await {
                Ok(raw) => {
                    if let Some(Entry::Unsupported(_)) = raw.as_ref().map(parse_global) {
                        count.skipped += 1;
                    } else if raw.is_some() {
                        match put_scope(&client, "", compose_scope("", None)).await {
                            Ok(()) => count.cleared += 1,
                            Err(_) => count.failed += 1,
                        }
                    }
                }
                Err(_) => count.failed += 1,
            }
            let mut stored = Vec::new();
            for room in client.joined_rooms() {
                let id = room.room_id().to_string();
                if let Some(raw) = read_store(&client, &id).await {
                    stored.push((id, parse_room(&raw)));
                }
            }
            let joined = |id: &str| joined_room(&client, id).is_ok();
            let (targets, skipped) = plan_clear(&stored, &known, &joined);
            count.skipped += skipped;
            for scope in &targets {
                match put_scope(&client, scope, compose_scope(scope, None)).await {
                    Ok(()) => count.cleared += 1,
                    Err(_) => count.failed += 1,
                }
            }
        } else {
            // Without the switch off first, removing copies could make another
            // device delete its pictures: nothing else is attempted.
            count.failed += 1;
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        lanes.send(json!({
            "type": "personal_backgrounds_cleared",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": count.ok(),
            "cleared": count.cleared,
            "failed": count.failed,
            "skipped": count.skipped,
            "switched_off": count.switched_off,
        }));
    });
    Ok(())
}

/// The global object as the SDK's handlers see it; typed only so the SDK
/// dispatches this one type to [`install_change_handlers`]. Never read: a
/// change is re-read through [`read`], the one parser.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.backgrounds", kind = GlobalAccountData)]
pub(crate) struct PersonalBackgroundsEventContent {}

/// The switch object, likewise.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.backgrounds.settings", kind = GlobalAccountData)]
pub(crate) struct PersonalBackgroundSettingsEventContent {}

/// One room's object, likewise.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.room.background", kind = RoomAccountData)]
pub(crate) struct PersonalRoomBackgroundEventContent {}

/// What crosses to C++ when a scope's account data changed in sync: the scope
/// only ("" also for the switch, which every read carries). Another device
/// set or removed a picture, or our own write came back.
pub(crate) fn change_payload(scope: &str) -> Value {
    json!({ "type": "personal_background_changed", "scope": scope })
}

/// Observe live changes of all three types. Account data reaches the client
/// through sliding sync's account-data extension (enabled by the room list
/// service; room account data for every room in the lists), and the SDK
/// hands it to these handlers after the store has it (global handlers before
/// any room's), so a `read` of the scope answers with the new value.
pub(crate) fn install_change_handlers(client: &Client, events: EventQueueRef) -> Vec<EventHandlerHandle> {
    let global_events = Arc::clone(&events);
    let global = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<PersonalBackgroundsEventContent>| {
            let events = Arc::clone(&global_events);
            async move {
                enqueue(&events, change_payload(""));
            }
        },
    );
    let settings_events = Arc::clone(&events);
    let settings = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<PersonalBackgroundSettingsEventContent>| {
            let events = Arc::clone(&settings_events);
            async move {
                enqueue(&events, change_payload(""));
            }
        },
    );
    let room = client.add_event_handler(
        move |_ev: RoomAccountDataEvent<PersonalRoomBackgroundEventContent>, room: Room| {
            let events = Arc::clone(&events);
            async move {
                enqueue(&events, change_payload(room.room_id().as_str()));
            }
        },
    );
    vec![global, settings, room]
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    /// A real encrypted file for `plain`, made by the SDK's own encryptor
    /// (what `upload_encrypted_file` does before the upload), with the url
    /// the upload would have returned.
    fn encrypt(plain: &[u8], url: &str) -> (Value, Vec<u8>) {
        let mut reader = std::io::Cursor::new(plain.to_vec());
        let mut encryptor = matrix_sdk_base::crypto::AttachmentEncryptor::new(&mut reader);
        let mut ciphertext = Vec::new();
        encryptor.read_to_end(&mut ciphertext).expect("encrypt");
        let keys = encryptor.finish();
        let file = EncryptedFile::new(url.into(), keys.encryption_info, keys.hashes);
        (serde_json::to_value(&file).expect("file json"), ciphertext)
    }

    fn decrypt(file: &Value, ciphertext: &[u8]) -> Vec<u8> {
        let file: EncryptedFile = serde_json::from_value(file.clone()).expect("file");
        let mut cursor = std::io::Cursor::new(ciphertext.to_vec());
        let mut reader =
            matrix_sdk_base::crypto::AttachmentDecryptor::new(&mut cursor, file.into())
                .expect("decryptor");
        let mut plain = Vec::new();
        reader.read_to_end(&mut plain).expect("decrypt");
        plain
    }

    fn png() -> Vec<u8> {
        let mut bytes = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        bytes.extend_from_slice(&[0u8; 64]);
        bytes
    }

    fn present_id(entry: &Entry) -> &str {
        match entry {
            Entry::Present(p) => &p.id,
            other => panic!("expected present, got {other:?}"),
        }
    }

    /// Runs a future to completion on a small runtime (the guard is async).
    fn block_on<F: Future>(future: F) -> F::Output {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("runtime")
            .block_on(future)
    }

    #[test]
    fn the_types_are_namespaced_and_stable() {
        // Other Lightning builds read exactly these names; renaming one makes
        // every synced background vanish.
        assert_eq!(GLOBAL_TYPE, "org.lightning_matrix.backgrounds");
        assert_eq!(ROOM_TYPE, "org.lightning_matrix.room.background");
        assert_eq!(SETTINGS_TYPE, "org.lightning_matrix.backgrounds.settings");
        use matrix_sdk::ruma::events::StaticEventContent;
        assert_eq!(PersonalBackgroundsEventContent::TYPE, GLOBAL_TYPE);
        assert_eq!(PersonalRoomBackgroundEventContent::TYPE, ROOM_TYPE);
        assert_eq!(PersonalBackgroundSettingsEventContent::TYPE, SETTINGS_TYPE);
    }

    // The round trip the feature depends on: encrypt with the SDK, write the
    // record as account data, read it back as another device would, and
    // decrypt to the same bytes. The key never leaves the record; what C++
    // receives (`entry_json`) carries no key at all.
    #[test]
    fn an_encrypted_file_survives_account_data_and_decrypts_to_the_same_picture() {
        let plain = png();
        let (file, ciphertext) = encrypt(&plain, "mxc://example.org/blob");
        assert_ne!(ciphertext, plain, "the media repository must hold ciphertext");
        let requested = json!({
            "info": { "mimetype": "image/png", "size": plain.len(), "w": 2560, "h": 1440 },
            "color": "#102030",
            "presentation": { "dim": 30, "blur": 10, "tint": 25, "fit": "contain", "align": "top" },
        });
        for scope in ["", "!room:example.org"] {
            let record = compose_record(&file, &requested).expect("record");
            let content = compose_scope(scope, Some(record));
            let stored: Value =
                serde_json::from_str(&serde_json::to_string(&content).unwrap()).unwrap();
            let Entry::Present(present) = parse_scope(scope, &stored) else {
                panic!("not read back: {stored}");
            };
            assert_eq!(decrypt(&present.file, &ciphertext), plain);
            assert_eq!(present.id, file["hashes"]["sha256"].as_str().unwrap());
            let shown = entry_json(scope, &Entry::Present(present));
            assert_eq!(shown["presentation"]["fit"], json!("contain"));
            let text = shown.to_string();
            assert!(!text.contains("\"k\""), "the key must not reach C++: {text}");
            assert!(!text.contains("mxc://"), "nor the url: {text}");
        }
    }

    // Account data is written as canonical JSON, like the shared event (the
    // 2026-10-06 M_BAD_JSON was a float in event content). Whatever C++ asks
    // for, presentation leaves as integers and nothing anywhere is a float.
    #[test]
    fn what_we_write_is_canonical_json_with_no_float_anywhere() {
        let (file, _) = encrypt(&png(), "mxc://example.org/blob");
        let requested = json!({
            "info": { "mimetype": "image/jpeg", "size": 784600.0, "w": 2560.5, "h": 1440 },
            "presentation": { "dim": 0.2, "blur": 0.0, "tint": 25.7, "fit": "tile" },
            "color": "#ABCDEF",
            "extra": 1.5,
            "expected_id": "never written",
        });
        for scope in ["", "!room:example.org"] {
            let content =
                compose_scope(scope, Some(compose_record(&file, &requested).expect("record")));
            assert!(is_canonical_json_safe(&content), "{content}");
            let record = if scope.is_empty() { &content["default"] } else { &content };
            for key in ["dim", "blur", "tint"] {
                assert!(record["presentation"][key].is_i64(), "{key}: {content}");
            }
            assert!(record.get("extra").is_none(), "unknown keys are not written");
            assert!(record.get("expected_id").is_none(), "request-only keys are not written");
            assert_eq!(content["version"], json!(1));
            let text = serde_json::to_string(&content).unwrap();
            let again = serde_json::to_string(
                &serde_json::from_str::<Value>(&text).unwrap(),
            )
            .unwrap();
            assert_eq!(text, again);
        }
        assert_eq!(compose_scope("", None), json!({ "version": 1 }));
        assert_eq!(compose_scope("!r:e.org", None), json!({}));
        assert_eq!(compose_settings(false), json!({ "version": 1, "enabled": false }));
        assert!(is_canonical_json_safe(&compose_settings(true)));
    }

    // Review M2: only an explicit "none" is CLEARED. Malformed content (an
    // unreadable file, no hash, an http url, a non-object) is INVALID, which
    // C++ never reads as "removed on another device".
    #[test]
    fn only_an_explicit_none_is_cleared_and_malformed_content_is_invalid() {
        assert_eq!(parse_global(&json!({})), Entry::Cleared);
        assert_eq!(parse_global(&json!({ "version": 1 })), Entry::Cleared);
        assert_eq!(parse_room(&json!({})), Entry::Cleared);
        assert_eq!(parse_room(&json!({ "version": 1 })), Entry::Cleared);

        let (file, _) = encrypt(&png(), "mxc://example.org/blob");
        let mut http = file.clone();
        http["url"] = json!("https://evil.example/x.png");
        let mut no_hash = file.clone();
        no_hash["hashes"] = json!({});
        for bad in [
            json!({ "url": "mxc://example.org/x" }), // a plain url, no key
            http,
            no_hash,
            json!("mxc://example.org/x"),
        ] {
            assert_eq!(parse_room(&json!({ "file": bad.clone() })), Entry::Invalid, "{bad}");
            assert_eq!(parse_global(&json!({ "default": { "file": bad.clone() } })), Entry::Invalid);
        }
        assert_eq!(parse_global(&json!({ "default": "x" })), Entry::Invalid);
        assert_eq!(parse_global(&json!("x")), Entry::Invalid);
        assert_eq!(parse_room(&json!(42)), Entry::Invalid);
        assert_eq!(parse_room(&json!({ "version": 0, "file": file.clone() })), Entry::Invalid);
        assert_eq!(parse_global(&json!({ "version": 2, "default": {} })), Entry::Unsupported(2));
        assert_eq!(parse_room(&json!({ "version": 3 })), Entry::Unsupported(3));
        let event = json!({ "type": ROOM_TYPE, "content": { "file": file.clone() } });
        assert!(matches!(parse_room(&event), Entry::Present(_)));
        let mut extra = file.clone();
        extra["tracking"] = json!("x");
        let Entry::Present(p) = parse_room(&json!({ "file": extra })) else { panic!() };
        assert!(p.file.get("tracking").is_none());
        assert_eq!(entry_json("!r:e.org", &Entry::Invalid)["state"], json!("invalid"));
        // The switch: only an explicit false is off.
        assert!(switch_on(&json!({})));
        assert!(switch_on(&json!({ "version": 1, "enabled": true })));
        assert!(!switch_on(&json!({ "version": 1, "enabled": false })));
        assert!(!switch_on(&json!({ "version": 7, "enabled": false })));
    }

    // Review M1: a presentation write keeps the server's picture ONLY when it
    // is the one this device expects; another picture is "changed". The
    // switch, off, refuses uploads and presentation writes; removal is
    // always allowed; a newer schema is never overwritten.
    #[test]
    fn writes_follow_the_servers_copy_and_the_switch() {
        let (a, _) = encrypt(&png(), "mxc://example.org/a");
        let (b, _) = encrypt(&png(), "mxc://example.org/b");
        let room = "!room:example.org";
        let id_a = a["hashes"]["sha256"].as_str().unwrap().to_owned();
        let server_b = compose_scope(room, compose_record(&b, &json!({})));
        let ask = |expected: &str| json!({ "presentation": { "dim": 50 }, "expected_id": expected });

        assert_eq!(
            plan_write(room, WriteMode::Presentation, &ask(&id_a), Some(&server_b), true, None),
            Err("changed")
        );
        assert_eq!(
            plan_write(room, WriteMode::Presentation, &json!({}), Some(&server_b), true, None),
            Err("changed")
        );
        assert_eq!(
            plan_write(room, WriteMode::Presentation, &ask(&id_a), None, true, None),
            Err("changed")
        );
        let server_a = compose_scope(room, compose_record(&a, &json!({})));
        let sent = plan_write(room, WriteMode::Presentation, &ask(&id_a), Some(&server_a), true, None)
            .expect("same picture");
        assert_eq!(present_id(&parse_room(&sent)), id_a);
        assert_eq!(sent["presentation"]["dim"], json!(50));

        assert_eq!(
            plan_write(room, WriteMode::Upload, &json!({}), None, false, Some(&a)),
            Err("sync_disabled")
        );
        // A migration only fills an empty scope; a user's own pick replaces.
        let migrate = json!({ "only_if_empty": true });
        assert_eq!(
            plan_write(room, WriteMode::Upload, &migrate, Some(&server_b), true, Some(&a)),
            Err("changed")
        );
        assert_eq!(
            plan_write(room, WriteMode::Upload, &migrate, Some(&json!({ "file": 1 })), true, Some(&a)),
            Err("changed"),
            "an invalid copy is not empty either"
        );
        let filled = plan_write(room, WriteMode::Upload, &migrate, None, true, Some(&a))
            .expect("empty scope");
        assert_eq!(present_id(&parse_room(&filled)), id_a);
        assert!(filled.get("only_if_empty").is_none(), "the control key is not stored");
        let cleared_room = compose_scope(room, None);
        assert!(plan_write(room, WriteMode::Upload, &migrate, Some(&cleared_room), true, Some(&a)).is_ok());
        assert!(plan_write(room, WriteMode::Upload, &json!({}), Some(&server_b), true, Some(&a)).is_ok());
        assert_eq!(
            plan_write(room, WriteMode::Presentation, &ask(&id_a), Some(&server_a), false, None),
            Err("sync_disabled")
        );
        assert_eq!(
            plan_write(room, WriteMode::Clear, &json!({}), Some(&server_a), false, None),
            Ok(json!({}))
        );
        assert_eq!(
            plan_write(room, WriteMode::Clear, &json!({}), Some(&json!({ "version": 2 })), true, None),
            Err("newer_schema")
        );
        assert_eq!(
            plan_write("", WriteMode::SetEnabled, &json!({ "enabled": true }), None, true, None),
            Err("invalid_mode")
        );
    }

    // Review R2: the switch is not part of any picture write. bgsync-2 kept
    // `enabled` inside the global picture object, so a default-picture write
    // that read it before an upload of up to two minutes wrote that stale
    // value back, undoing another device's opt-out. Now no picture write can
    // carry it, whatever the server held or C++ asked for.
    #[test]
    fn a_picture_write_never_carries_the_switch() {
        let (a, _) = encrypt(&png(), "mxc://example.org/a");
        let id_a = a["hashes"]["sha256"].as_str().unwrap().to_owned();
        // A server copy written by bgsync-2, with the switch inside.
        let mut old_global = compose_scope("", compose_record(&a, &json!({})));
        old_global["enabled"] = json!(true);
        let asked = json!({ "enabled": true, "presentation": { "dim": 40 }, "expected_id": id_a });
        for (mode, uploaded) in [
            (WriteMode::Upload, Some(&a)),
            (WriteMode::Presentation, None),
            (WriteMode::Clear, None),
        ] {
            let sent = plan_write("", mode, &asked, Some(&old_global), true, uploaded)
                .unwrap_or_else(|e| panic!("{mode:?}: {e}"));
            assert!(sent.get("enabled").is_none(), "{mode:?} wrote the switch: {sent}");
        }
    }

    // Review R2: the switch is read again immediately before the PUT. Here it
    // was on when the write was planned (after the upload) and went off on
    // another device before the PUT: nothing is sent, and the write ends
    // with sync_disabled.
    #[test]
    fn the_switch_turning_off_between_read_and_put_stops_the_put() {
        let (a, _) = encrypt(&png(), "mxc://example.org/a");
        let planned = plan_write("!r:e.org", WriteMode::Upload, &json!({}), None, true, Some(&a))
            .expect("planned while the switch was on");
        assert!(parse_room(&planned) != Entry::Cleared);

        let sent = std::sync::atomic::AtomicBool::new(false);
        let outcome = block_on(put_if_switch_on(
            true,
            async { Ok(false) },
            async {
                sent.store(true, std::sync::atomic::Ordering::SeqCst);
                Ok(())
            },
        ));
        assert_eq!(outcome, Err(("send", "sync_disabled".to_owned())));
        assert!(!sent.load(std::sync::atomic::Ordering::SeqCst), "the PUT ran anyway");

        // Still on: the PUT runs. A removal does not need the switch at all.
        assert_eq!(block_on(put_if_switch_on(true, async { Ok(true) }, async { Ok(()) })), Ok(()));
        assert_eq!(
            block_on(put_if_switch_on(false, async { Ok(false) }, async { Ok(()) })),
            Ok(())
        );
        // An unreadable switch is not "on".
        assert_eq!(
            block_on(put_if_switch_on(true, async { Err("network".to_owned()) }, async { Ok(()) })),
            Err(("read", "network".to_owned()))
        );
    }

    // Review M6: a removal pass reports every scope it did not clear; one
    // that could not turn the switch off first is never ok.
    #[test]
    fn a_removal_pass_counts_what_it_skipped() {
        let (a, _) = encrypt(&png(), "mxc://example.org/a");
        let present = |room: &str| parse_room(&compose_scope(room, compose_record(&a, &json!({}))));
        let stored = vec![
            ("!one:e.org".to_owned(), present("!one:e.org")),
            ("!bad:e.org".to_owned(), Entry::Invalid),
            ("!new:e.org".to_owned(), Entry::Unsupported(2)),
            ("!none:e.org".to_owned(), Entry::Cleared),
        ];
        let known = vec![
            "!one:e.org".to_owned(),
            "!left:e.org".to_owned(),
            "!quiet:e.org".to_owned(),
        ];
        let joined = |id: &str| id != "!left:e.org";
        let (targets, skipped) = plan_clear(&stored, &known, &joined);
        assert_eq!(targets, vec!["!one:e.org", "!bad:e.org", "!quiet:e.org"]);
        assert_eq!(skipped, 2, "the left room and the newer schema");
        let count = ClearCount { cleared: 4, failed: 0, skipped, switched_off: true };
        assert!(!count.ok(), "a pass that skipped anything is partial, never ok");
        assert!(ClearCount { cleared: 1, failed: 0, skipped: 0, switched_off: true }.ok());
        assert!(!ClearCount { cleared: 1, failed: 0, skipped: 0, switched_off: false }.ok());
    }

    #[test]
    fn write_modes_map_from_the_ffi_and_nothing_else() {
        assert_eq!(WriteMode::from_ffi(0), Some(WriteMode::Upload));
        assert_eq!(WriteMode::from_ffi(1), Some(WriteMode::Presentation));
        assert_eq!(WriteMode::from_ffi(2), Some(WriteMode::Clear));
        assert_eq!(WriteMode::from_ffi(3), Some(WriteMode::SetEnabled));
        assert_eq!(WriteMode::from_ffi(4), None);
        assert!(WriteMode::Upload.needs_switch() && WriteMode::Presentation.needs_switch());
        assert!(!WriteMode::Clear.needs_switch() && !WriteMode::SetEnabled.needs_switch());
        assert!(check_scope("").is_ok());
        assert!(check_scope("!room:example.org").is_ok());
        assert!(check_scope("@user:example.org").is_err());
        assert!(check_scope("*").is_err());
    }

    #[test]
    fn parked_pictures_are_bounded() {
        let parked: ParkedPictures = Arc::new(Mutex::new(HashMap::new()));
        for op in 1..=10u64 {
            park(&parked, op, vec![op as u8]);
        }
        let map = parked.lock().unwrap();
        assert_eq!(map.len(), MAX_PARKED);
        assert!(map.contains_key(&10) && !map.contains_key(&1), "the oldest go first");
    }

    // Review H1: the picture a download decrypts must never land in the
    // timeline media map. Runs the real download against a mock homeserver;
    // fails on a version that parked in media_results.
    #[test]
    fn a_downloaded_picture_never_touches_the_timeline_media_map() {
        use matrix_sdk::ruma::api::client::sync::sync_events::v5;
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;

        let plain = png();
        let (file, ciphertext) = encrypt(&plain, "mxc://example.org/blob");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            server.mock_versions().ok().mount().await;
            server.mock_authed_media_download().ok_bytes(ciphertext.clone()).mount().await;
            let mut response = v5::Response::new("pos-1".to_owned());
            response.extensions.account_data.global = vec![Raw::from_json_string(
                json!({ "type": GLOBAL_TYPE,
                        "content": compose_scope("", compose_record(&file, &json!({}))) })
                .to_string(),
            )
            .unwrap()];
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });

        let dir = std::env::temp_dir().join(format!("lightning-bgsync-h1-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client);
        bridge.media_results.lock().unwrap().insert(5, b"timeline picture".to_vec());

        let Entry::Present(stored) = parse_scope("", &compose_scope("", compose_record(&file, &json!({}))))
        else {
            panic!("the record parses");
        };
        download(&bridge, 5, String::new(), stored.id).expect("download started");
        let started = std::time::Instant::now();
        let event: Value = loop {
            if let Some(raw) = bridge.command_events.lock().unwrap().pop_front() {
                break serde_json::from_str(&raw).unwrap();
            }
            assert!(started.elapsed() < Duration::from_secs(20), "no answer");
            std::thread::sleep(Duration::from_millis(20));
        };
        assert_eq!(event["type"], json!("personal_background_bytes"), "{event}");
        assert_eq!(event["ok"], json!(true), "{event}");
        assert_eq!(
            bridge.media_results.lock().unwrap().get(&5).map(Vec::as_slice),
            Some(&b"timeline picture"[..]),
            "the timeline's parked bytes were overwritten"
        );
        assert_eq!(
            bridge.personal_background_results.lock().unwrap().get(&5),
            Some(&plain),
            "the decrypted picture is parked in its own map"
        );
        drop(bridge);
        drop(server);
        drop(rt);
    }

    // Review N1: B reads the switch (on), A's "Remove server copies" turns it
    // off and clears, B reads the cleared scope: "on" + "cleared" + a live
    // mark deleted B's picture. The fixture serves the state before the
    // removal to the FIRST request and the state after it to every later
    // one; whichever order a read uses, it must never come back with
    // "cleared" and "on" together. Fails with the switch read first.
    #[test]
    fn a_read_takes_its_switch_after_its_entries() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let requests = AtomicUsize::new(0);
        // Request 0 sees (on, present); A's removal lands; 1+ see (off, cleared).
        let removed = || requests.fetch_add(1, Ordering::SeqCst) > 0;
        let (entry, switch) = rt.block_on(entries_then_switch(
            async { if removed() { "cleared" } else { "present" } },
            || async { Some(!removed()) },
        ));
        assert!(
            !(entry == "cleared" && switch == Some(true)),
            "a cleared scope came with the switch still on"
        );
        assert_eq!((entry, switch), ("present", Some(false)));
    }

    // Review F1: the switch GET a read shares must have started after its
    // entries were COMPLETE. Here another read's GET starts (and caches "on")
    // while this read's entries are still being read; A's removal lands; the
    // entries end on "cleared". Reusing that GET answered "cleared" + "on".
    // Fails with the instant taken when the entries BEGIN.
    #[test]
    fn a_shared_switch_answer_never_predates_the_end_of_the_entries() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let cache: SwitchCache = Arc::new(tokio::sync::Mutex::new(None));
        let asked = AtomicUsize::new(0);
        let ask = |on: bool| {
            let asked = &asked;
            async move {
                asked.fetch_add(1, Ordering::SeqCst);
                Ok::<bool, String>(on)
            }
        };
        rt.block_on(async {
            let entries = async {
                tokio::time::sleep(Duration::from_millis(3)).await;
                // t1: a concurrent read's GET, before the removal: "on".
                assert_eq!(switch_after(&cache, 1, Instant::now(), ask(true)).await, Some(true));
                tokio::time::sleep(Duration::from_millis(3)).await;
                // The removal lands; the last scope read shows it.
                "cleared"
            };
            // After the removal the server says "off".
            let (entry, switch) =
                entries_then_shared_switch(&cache, 1, entries, || ask(false)).await;
            assert_eq!(entry, "cleared");
            assert_eq!(switch, Some(false), "a GET from before the entries ended was reused");
            assert_eq!(asked.load(Ordering::SeqCst), 2);
        });
    }

    // Sharing itself: a GET that started after a read's entries ended is
    // reused by it; another session's never is.
    #[test]
    fn a_shared_switch_answer_never_predates_the_entries() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let cache: SwitchCache = Arc::new(tokio::sync::Mutex::new(None));
        let asked = AtomicUsize::new(0);
        let ask = |on: bool| {
            let asked = &asked;
            async move {
                asked.fetch_add(1, Ordering::SeqCst);
                Ok::<bool, String>(on)
            }
        };
        rt.block_on(async {
            let before = Instant::now();
            assert_eq!(switch_after(&cache, 1, before, ask(true)).await, Some(true));
            // A read whose entries began before that GET shares it...
            assert_eq!(switch_after(&cache, 1, before, ask(false)).await, Some(true));
            assert_eq!(asked.load(Ordering::SeqCst), 1, "the GET was shared");
            // ...one that began after it asks again, and sees the new value.
            tokio::time::sleep(Duration::from_millis(5)).await;
            let later = Instant::now();
            assert_eq!(switch_after(&cache, 1, later, ask(false)).await, Some(false));
            assert_eq!(asked.load(Ordering::SeqCst), 2);
            // Another session never reuses it.
            assert_eq!(switch_after(&cache, 2, before, ask(true)).await, Some(true));
            assert_eq!(asked.load(Ordering::SeqCst), 3);
        });
    }

    // The command lane's overflow cleanup frees a dropped event's parked bytes
    // in the map that event's type owns: a dropped background event never
    // removes a timeline payload with the same op number, nor the reverse.
    #[test]
    fn overflow_cleanup_is_routed_by_event_type() {
        use std::collections::VecDeque;
        let queue: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let media: Arc<Mutex<HashMap<u64, Vec<u8>>>> = Arc::new(Mutex::new(HashMap::new()));
        let parked: ParkedPictures = Arc::new(Mutex::new(HashMap::new()));
        media.lock().unwrap().insert(7, b"timeline".to_vec());
        parked.lock().unwrap().insert(7, b"background".to_vec());
        parked.lock().unwrap().insert(8, b"background 8".to_vec());
        // The oldest event is a background one for op 7, then a timeline one
        // for op 8; fill to the cap so both are dropped in order.
        enqueue_terminal_routed(
            &queue,
            Some(&media),
            Some(&parked),
            json!({ "type": "personal_background_bytes", "op_id": 7 }),
        );
        enqueue_terminal_routed(
            &queue,
            Some(&media),
            Some(&parked),
            json!({ "type": "media_ready", "op_id": 8 }),
        );
        for i in 0..crate::COMMAND_QUEUE_CAP {
            enqueue_terminal_routed(
                &queue,
                Some(&media),
                Some(&parked),
                json!({ "type": "noise", "op_id": 1000 + i }),
            );
        }
        assert!(media.lock().unwrap().contains_key(&7), "a background event freed a timeline payload");
        assert!(!parked.lock().unwrap().contains_key(&7), "its own payload was kept");
        assert!(parked.lock().unwrap().contains_key(&8), "a timeline event freed a background payload");
    }

    // Another device changes a background (or the switch): the change
    // arrives in sync's account-data extension and must reach C++ with its
    // scope. Fails without `install_change_handlers`.
    #[tokio::test]
    async fn an_account_data_change_in_sync_reaches_cpp_with_its_scope() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;
        use std::collections::VecDeque;

        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let events: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let _handles = install_change_handlers(&client, Arc::clone(&events));

        let room = room_id!("!mine:example.org");
        let (file, _) = encrypt(&png(), "mxc://example.org/blob");
        let mut response = v5::Response::new("pos-1".to_owned());
        response.rooms.insert(room.to_owned(), v5::response::Room::default());
        response.extensions.account_data.global = vec![
            Raw::from_json_string(json!({ "type": "m.unrelated", "content": {} }).to_string())
                .unwrap(),
            Raw::from_json_string(
                json!({ "type": SETTINGS_TYPE, "content": compose_settings(false) }).to_string(),
            )
            .unwrap(),
            Raw::from_json_string(
                json!({ "type": GLOBAL_TYPE,
                        "content": compose_scope("", compose_record(&file, &json!({}))) })
                .to_string(),
            )
            .unwrap(),
        ];
        response.extensions.account_data.rooms.insert(
            room.to_owned(),
            vec![Raw::from_json_string(json!({ "type": ROOM_TYPE, "content": {} }).to_string())
                .unwrap()],
        );
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");

        let queued: Vec<Value> = events
            .lock()
            .unwrap()
            .iter()
            .map(|s| serde_json::from_str(s).unwrap())
            .collect();
        assert!(queued.contains(&change_payload("")), "{queued:?}");
        assert!(queued.contains(&change_payload(room.as_str())), "{queued:?}");
        let distinct: std::collections::BTreeSet<String> =
            queued.iter().map(|v| v.to_string()).collect();
        assert_eq!(distinct.len(), 2, "{queued:?}");
        assert!(queued.iter().all(|v| v.as_object().map(|o| o.len()) == Some(2)));
        let stored = read_store(&client, "").await.expect("stored");
        assert!(matches!(parse_global(&stored), Entry::Present(_)));
    }

    // The store lags this device's own writes until sync echoes them, so a
    // read made right after a write must ask the SERVER. Here the store
    // still holds an older state (switch off, a picture) and the server has
    // moved on (no settings = on, no picture). Store-first answered
    // "enabled: false" with the old picture: the switch this device had just
    // turned on read back off (live, 2026-10-07), and a stale "cleared" the
    // same way would delete a picture the user had just chosen. A download
    // never takes a picture other than the one asked for.
    #[test]
    fn a_read_asks_the_server_not_the_lagging_store() {
        use matrix_sdk::ruma::api::client::sync::sync_events::v5;
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;

        let (file, _) = encrypt(&png(), "mxc://example.org/old");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            server.mock_versions().ok().mount().await;
            let mut response = v5::Response::new("pos-1".to_owned());
            response.extensions.account_data.global = vec![
                Raw::from_json_string(
                    json!({ "type": SETTINGS_TYPE, "content": compose_settings(false) })
                        .to_string(),
                )
                .unwrap(),
                Raw::from_json_string(
                    json!({ "type": GLOBAL_TYPE,
                            "content": compose_scope("", compose_record(&file, &json!({}))) })
                    .to_string(),
                )
                .unwrap(),
            ];
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });
        let dir = std::env::temp_dir().join(format!("lightning-bgsync-lag-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client);
        let next = |bridge: &crate::RustClient| -> Value {
            let started = std::time::Instant::now();
            loop {
                if let Some(raw) = bridge.command_events.lock().unwrap().pop_front() {
                    break serde_json::from_str(&raw).unwrap();
                }
                assert!(started.elapsed() < Duration::from_secs(20), "no answer");
                std::thread::sleep(Duration::from_millis(20));
            }
        };

        read(&bridge, 1, String::new()).expect("read started");
        let event = next(&bridge);
        assert_eq!(event["type"], json!("personal_backgrounds"), "{event}");
        assert_eq!(event["enabled"], json!(true), "the switch came from the store: {event}");
        assert_eq!(event["entries"], json!([]), "the picture came from the store: {event}");
        assert_eq!(event["failed"], json!([]), "{event}");

        // A download of another picture than the stored one goes to the
        // server, which has none: refused, the stored (replaced) copy is not
        // handed back in its place.
        download(&bridge, 2, String::new(), "N".repeat(43)).expect("download started");
        let event = next(&bridge);
        assert_eq!(event["type"], json!("personal_background_bytes"), "{event}");
        assert_eq!(event["ok"], json!(false), "{event}");
        assert!(bridge.personal_background_results.lock().unwrap().is_empty());
        drop(bridge);
        drop(server);
        drop(rt);
    }
}
