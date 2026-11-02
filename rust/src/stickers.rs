//! Stickers and image packs: MSC2545 (`im.ponies.*`) and `m.sticker` send.
//!
//! The three MSC2545 events, with shapes transcribed from ruma-events
//! 0.34's `image_pack` module:
//!
//!   * `im.ponies.user_emotes`: global account data, the account's own pack.
//!   * `im.ponies.room_emotes`: room state. The state key is the pack id, so
//!     a room may have several packs; the empty key is the default pack.
//!   * `im.ponies.emote_rooms`: global account data, `{ "rooms": { room_id:
//!     { state_key: {} } } }`, selecting room packs usable everywhere (a
//!     room's own packs are always usable inside it).
//!
//! Hand-written serde_json rather than ruma's typed structs: those need the
//! `unstable-msc2545` feature and ruma-events as a direct dependency. Switch
//! to them if that feature is ever enabled for another reason.
//!
//! Packs are remote, user-chosen content, so nothing here is trusted:
//!
//!   * `url` must be a valid `mxc://server/id`; anything else (an https
//!     tracking pixel, `file://`, `data:`) drops the image.
//!   * A declared `info.mimetype` outside the five raster types is refused,
//!     notably `image/svg+xml` (untrusted SVG is forbidden, and Qt here does
//!     decode svg/svgz). An absent mimetype is unknown, allowed here, and
//!     caught by the byte sniff in `rooms::media_fetch_mxc`.
//!   * Shortcodes, bodies, names and attribution are bounded and stripped of
//!     control characters; C++ treats them as plain labels.
//!   * Packs and images per pack are capped.
//!
//! Nothing here logs a shortcode, body, pack name or mxc.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    deserialized_responses::RawAnySyncOrStrippedState,
    ruma::{
        events::{
            macros::EventContent, room::ImageInfo, sticker::StickerEventContent,
            AnyMessageLikeEventContent, GlobalAccountDataEvent, GlobalAccountDataEventType,
            StateEventType,
        },
        MxcUri, OwnedMxcUri, RoomId, UInt,
    },
};
use serde_json::{json, Value};

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

pub(crate) const USER_EMOTES: &str = "im.ponies.user_emotes";
pub(crate) const ROOM_EMOTES: &str = "im.ponies.room_emotes";
pub(crate) const EMOTE_ROOMS: &str = "im.ponies.emote_rooms";

pub(crate) const STABLE_ROOM_PACK: &str = "m.room.image_pack";
pub(crate) const STABLE_PACK_ROOMS: &str = "m.image_pack.rooms";

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub(crate) enum Namespace {
    Stable,
    Legacy,
}

impl Namespace {
    pub(crate) fn room_type(self) -> &'static str {
        match self {
            Namespace::Stable => STABLE_ROOM_PACK,
            Namespace::Legacy => ROOM_EMOTES,
        }
    }
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Namespace::Stable => "stable",
            Namespace::Legacy => "legacy",
        }
    }
    pub(crate) fn parse(text: &str) -> Option<Self> {
        match text {
            "stable" => Some(Namespace::Stable),
            "legacy" => Some(Namespace::Legacy),
            _ => None,
        }
    }
}

/// An opaque, unambiguous id for a room pack. Room ids and state keys both
/// contain colons, so `room:<room>:<key>` could not be split back; the tuple
/// is JSON-encoded instead. C++ treats the id as opaque and carries the
/// typed fields beside it.
pub(crate) fn room_pack_id(namespace: Namespace, room_id: &str, state_key: &str) -> String {
    format!(
        "room:{}:{}",
        namespace.as_str(),
        serde_json::to_string(&(room_id, state_key)).unwrap_or_default()
    )
}

const MAX_PACKS: usize = 64;

const MAX_IMAGES_PER_PACK: usize = 512;

/// Shortcode length bound: Matrix v1.19's grammar is `[A-Za-z0-9_-]{1,100}`,
/// ASCII, so bytes equal chars. Applies to NEW and renamed keys only: a
/// received key outside the grammar is kept as identity (see `PackImage`).
const MAX_SHORTCODE_BYTES: usize = 100;

const MAX_RAW_KEY_BYTES: usize = 1024;

const MAX_STICKER_UPLOAD_BYTES: u64 = 4 * 1024 * 1024;

const MAX_BODY_CHARS: usize = 160;

const MAX_PACK_NAME_CHARS: usize = 80;
const MAX_ATTRIBUTION_CHARS: usize = 160;

/// One bounded request per state read: the room-action pool is joined at
/// sign-out, so retry loops here would stall shutdown (as in `pinned.rs`).
const PACK_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

const ALLOWED_MIMETYPES: [&str; 5] = [
    "image/png",
    "image/jpeg",
    "image/gif",
    "image/webp",
    "image/bmp",
];

fn one_line(text: &str, max_chars: usize) -> String {
    let collapsed: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let trimmed = collapsed.split_whitespace().collect::<Vec<_>>().join(" ");
    trimmed.chars().take(max_chars).collect()
}

pub(crate) fn is_valid_shortcode(key: &str) -> bool {
    !key.is_empty()
        && key.len() <= MAX_SHORTCODE_BYTES
        && key
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

pub(crate) fn propose_shortcode(requested: &str) -> String {
    let trimmed = requested.trim();
    if is_valid_shortcode(trimmed) {
        return trimmed.to_owned();
    }
    sanitize_shortcode(trimmed)
}

pub(crate) fn sanitize_shortcode(raw: &str) -> String {
    let mut out = String::new();
    for c in raw.trim().chars() {
        if c.is_ascii_alphanumeric() || c == '-' {
            out.push(c);
        } else if !out.ends_with('_') {
            out.push('_');
        }
        if out.len() >= MAX_SHORTCODE_BYTES {
            break;
        }
    }
    out.trim_matches(['-', '_']).to_owned()
}

pub(crate) fn is_valid_mxc(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("mxc://") else {
        return false;
    };
    let Some((_, media_id)) = rest.split_once('/') else {
        return false;
    };
    if media_id.is_empty()
        || !media_id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    {
        return false;
    }
    <&MxcUri>::from(url).parts().is_ok()
}

fn mimetype_allowed(declared: Option<&str>) -> bool {
    match declared {
        None => true,
        Some(m) => ALLOWED_MIMETYPES.contains(&m.trim().to_ascii_lowercase().as_str()),
    }
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub(crate) enum Usage {
    Emoticon,
    Sticker,
}

fn parse_usage(value: Option<&Value>) -> BTreeSet<Usage> {
    parse_usage_detail(value).0
}

fn parse_usage_detail(value: Option<&Value>) -> (BTreeSet<Usage>, bool) {
    let mut out = BTreeSet::new();
    let Some(Value::Array(items)) = value else {
        return (out, false);
    };
    for item in items {
        match item.as_str() {
            Some("emoticon") => {
                out.insert(Usage::Emoticon);
            }
            Some("sticker") => {
                out.insert(Usage::Sticker);
            }
            _ => {}
        }
    }
    (out, !items.is_empty())
}

fn resolve_stable_usage(pack_usage: Option<&Value>) -> (bool, bool, bool) {
    let (known, named) = parse_usage_detail(pack_usage);
    if !named {
        return (true, true, false);
    }
    if known.is_empty() {
        return (false, false, true);
    }
    (known.contains(&Usage::Emoticon), known.contains(&Usage::Sticker), false)
}

fn resolve_usage(
    image: &BTreeSet<Usage>,
    pack: &BTreeSet<Usage>,
) -> (bool, bool) {
    let effective = if !image.is_empty() { image } else { pack };
    if effective.is_empty() {
        return (true, true);
    }
    (
        effective.contains(&Usage::Emoticon),
        effective.contains(&Usage::Sticker),
    )
}

#[derive(Clone, Debug)]
pub(crate) struct PackImage {
    /// The exact `images` map key: the image's identity. Every mutation names
    /// this, never the display text, so a received `hello__world` or a 65-100
    /// byte key is removed or renamed as itself.
    pub key: String,
    pub key_valid: bool,
    pub shortcode: String,
    pub url: String,
    pub body: String,
    pub mimetype: Option<String>,
    pub width: Option<u64>,
    pub height: Option<u64>,
    pub size: Option<u64>,
    pub is_emoticon: bool,
    pub is_sticker: bool,
}

#[derive(Clone, Debug)]
pub(crate) struct Pack {
    pub id: String,
    pub namespace: &'static str,
    pub legacy_twin: bool,
    pub legacy_subscribed: bool,
    pub usage_unsupported: bool,
    pub available: bool,
    pub unavailable: String,
    pub from_space: bool,
    pub pack_emoticon: bool,
    pub pack_sticker: bool,
    pub display_name: String,
    pub avatar_url: String,
    pub attribution: String,
    pub source: &'static str,
    pub room_id: String,
    pub state_key: String,
    pub enabled_globally: bool,
    pub can_manage: bool,
    pub images: Vec<PackImage>,
}

impl Pack {
    fn to_json(&self) -> Value {
        json!({
            "id": self.id,
            "namespace": self.namespace,
            "legacy_twin": self.legacy_twin,
            "legacy_subscribed": self.legacy_subscribed,
            "usage_unsupported": self.usage_unsupported,
            "available": self.available,
            "unavailable": self.unavailable,
            "from_space": self.from_space,
            "pack_emoticon": self.pack_emoticon,
            "pack_sticker": self.pack_sticker,
            "display_name": self.display_name,
            "avatar_url": self.avatar_url,
            "attribution": self.attribution,
            "source": self.source,
            "room_id": self.room_id,
            "state_key": self.state_key,
            "enabled_globally": self.enabled_globally,
            "can_manage": self.can_manage,
            "images": self.images.iter().map(|i| json!({
                "key": i.key,
                "key_valid": i.key_valid,
                "shortcode": i.shortcode,
                "url": i.url,
                "body": i.body,
                "mimetype": i.mimetype.clone().unwrap_or_default(),
                "width": i.width.unwrap_or(0),
                "height": i.height.unwrap_or(0),
                "size": i.size.unwrap_or(0),
                "is_emoticon": i.is_emoticon,
                "is_sticker": i.is_sticker,
            })).collect::<Vec<_>>(),
        })
    }
}

pub(crate) fn parse_pack_content(
    content: &Value,
    id: String,
    source: &'static str,
    room_id: String,
    state_key: String,
    fallback_name: &str,
) -> Option<Pack> {
    parse_pack_content_ns(content, id, source, room_id, state_key, fallback_name,
                          Namespace::Legacy)
}

pub(crate) fn parse_pack_content_ns(
    content: &Value,
    id: String,
    source: &'static str,
    room_id: String,
    state_key: String,
    fallback_name: &str,
    namespace: Namespace,
) -> Option<Pack> {
    let object = content.as_object()?;

    let pack_info = object.get("pack").and_then(|p| p.as_object());
    let pack_usage = parse_usage(pack_info.and_then(|p| p.get("usage")));
    let stable_usage = resolve_stable_usage(pack_info.and_then(|p| p.get("usage")));

    let display_name = pack_info
        .and_then(|p| p.get("display_name"))
        .and_then(|v| v.as_str())
        .map(|s| one_line(s, MAX_PACK_NAME_CHARS))
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| one_line(fallback_name, MAX_PACK_NAME_CHARS));

    let avatar_url = pack_info
        .and_then(|p| p.get("avatar_url"))
        .and_then(|v| v.as_str())
        .filter(|s| is_valid_mxc(s))
        .unwrap_or_default()
        .to_owned();

    let attribution = pack_info
        .and_then(|p| p.get("attribution"))
        .and_then(|v| v.as_str())
        .map(|s| one_line(s, MAX_ATTRIBUTION_CHARS))
        .unwrap_or_default();

    let mut images = Vec::new();
    if let Some(Value::Object(map)) = object.get("images") {
        let sorted: BTreeMap<&String, &Value> = map.iter().collect();
        for (raw_code, entry) in sorted {
            if images.len() >= MAX_IMAGES_PER_PACK {
                break;
            }
            let Some(entry) = entry.as_object() else { continue };

            if raw_code.len() > MAX_RAW_KEY_BYTES {
                continue;
            }
            let key_valid = is_valid_shortcode(raw_code);
            let shortcode = if key_valid {
                raw_code.clone()
            } else {
                one_line(raw_code, MAX_SHORTCODE_BYTES)
            };

            let Some(url) = entry.get("url").and_then(|v| v.as_str()) else {
                continue;
            };
            if !is_valid_mxc(url) {
                continue;
            }

            let info = entry.get("info").and_then(|v| v.as_object());
            let mimetype = info
                .and_then(|i| i.get("mimetype"))
                .and_then(|v| v.as_str())
                .map(|s| s.trim().to_ascii_lowercase());
            if !mimetype_allowed(mimetype.as_deref()) {
                continue;
            }

            let (is_emoticon, is_sticker) = match namespace {
                Namespace::Stable => (stable_usage.0, stable_usage.1),
                Namespace::Legacy => {
                    let image_usage = parse_usage(entry.get("usage"));
                    resolve_usage(&image_usage, &pack_usage)
                }
            };

            let body = entry
                .get("body")
                .and_then(|v| v.as_str())
                .map(|s| one_line(s, MAX_BODY_CHARS))
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| shortcode.clone());

            let dimension = |key: &str| -> Option<u64> {
                info.and_then(|i| i.get(key))
                    .and_then(|v| v.as_u64())
                    .filter(|n| *n > 0)
            };

            images.push(PackImage {
                key: raw_code.clone(),
                key_valid,
                shortcode,
                url: url.to_owned(),
                body,
                mimetype,
                width: dimension("w"),
                height: dimension("h"),
                size: dimension("size"),
                is_emoticon,
                is_sticker,
            });
        }
    }

    Some(Pack {
        id,
        namespace: namespace.as_str(),
        legacy_twin: false,
        legacy_subscribed: false,
        usage_unsupported: namespace == Namespace::Stable && stable_usage.2,
        available: true,
        unavailable: String::new(),
        from_space: false,
        pack_emoticon: match namespace {
            Namespace::Stable => stable_usage.0,
            Namespace::Legacy => resolve_usage(&BTreeSet::new(), &pack_usage).0,
        },
        pack_sticker: match namespace {
            Namespace::Stable => stable_usage.1,
            Namespace::Legacy => resolve_usage(&BTreeSet::new(), &pack_usage).1,
        },
        display_name,
        avatar_url,
        attribution,
        source,
        room_id,
        state_key,
        enabled_globally: false,
        can_manage: false,
        images,
    })
}

pub(crate) fn parse_emote_rooms(content: &Value) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let Some(rooms) = content.get("rooms").and_then(|v| v.as_object()) else {
        return out;
    };
    for (room_id, packs) in rooms {
        if RoomId::parse(room_id).is_err() {
            continue;
        }
        let Some(packs) = packs.as_object() else { continue };
        for state_key in packs.keys() {
            if out.len() >= MAX_PACKS {
                return out;
            }
            out.push((room_id.clone(), state_key.clone()));
        }
    }
    out
}

#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct Subscriptions {
    pub active: Vec<(String, String)>,
    pub source: &'static str,
    pub legacy_only: Vec<(String, String)>,
    pub stable_unreadable: bool,
}

pub(crate) fn effective_subscriptions(stable: &PackRead, legacy: Option<&Value>) -> Subscriptions {
    let legacy_refs = legacy.map(parse_emote_rooms).unwrap_or_default();
    match stable {
        PackRead::Found(content) => {
            let active = parse_emote_rooms(content);
            let legacy_only = legacy_refs
                .into_iter()
                .filter(|r| !active.contains(r))
                .collect();
            Subscriptions { active, source: "stable", legacy_only, stable_unreadable: false }
        }
        PackRead::Absent | PackRead::Failed(_) => Subscriptions {
            source: if legacy.is_some() { "legacy" } else { "none" },
            active: legacy_refs,
            legacy_only: Vec::new(),
            stable_unreadable: matches!(stable, PackRead::Failed(_)),
        },
    }
}

pub(crate) fn plan_stable_subscription_write(
    stable: PackRead,
    legacy: Option<&Value>,
    room_id: &str,
    state_key: &str,
    enabled: bool,
) -> Result<Value, String> {
    let base = match stable {
        PackRead::Found(content) => content,
        PackRead::Absent => {
            let rooms = legacy
                .and_then(|l| l.get("rooms"))
                .filter(|r| r.is_object())
                .cloned()
                .unwrap_or_else(|| json!({}));
            json!({ "rooms": rooms })
        }
        PackRead::Failed(category) => return Err(category),
    };
    apply_emote_rooms_change(base, room_id, state_key, enabled)
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PackRecord {
    pub namespace: Namespace,
    pub state_key: String,
    pub content: Value,
    pub ts: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct ResolvedRoomPack {
    pub namespace: Namespace,
    pub state_key: String,
    pub content: Value,
    pub legacy_twin: bool,
}

fn has_images(content: &Value) -> bool {
    content.get("images").is_some_and(|v| v.is_object())
}

pub(crate) fn resolve_room_packs(records: Vec<PackRecord>) -> Vec<ResolvedRoomPack> {
    let mut newest: BTreeMap<(String, Namespace), PackRecord> = BTreeMap::new();
    for record in records {
        let slot = (record.state_key.clone(), record.namespace);
        match newest.get(&slot) {
            Some(existing) if existing.ts > record.ts => {}
            _ => {
                newest.insert(slot, record);
            }
        }
    }
    let keys: BTreeSet<String> = newest.keys().map(|(k, _)| k.clone()).collect();
    let mut out = Vec::new();
    for key in keys {
        let stable = newest.get(&(key.clone(), Namespace::Stable));
        let legacy = newest.get(&(key.clone(), Namespace::Legacy));
        if let Some(stable) = stable {
            if has_images(&stable.content) {
                out.push(ResolvedRoomPack {
                    namespace: Namespace::Stable,
                    state_key: key.clone(),
                    content: stable.content.clone(),
                    legacy_twin: legacy.is_some_and(|l| has_images(&l.content)),
                });
            }
            continue;
        }
        if let Some(legacy) = legacy {
            if has_images(&legacy.content) {
                out.push(ResolvedRoomPack {
                    namespace: Namespace::Legacy,
                    state_key: key,
                    content: legacy.content.clone(),
                    legacy_twin: false,
                });
            }
        }
    }
    out
}

struct RoomScans {
    entries: std::sync::Mutex<
        std::collections::HashMap<(String, String), (std::time::Instant, Vec<PackRecord>)>,
    >,
}

const ROOM_SCAN_TTL: Duration = Duration::from_secs(10 * 60);

fn room_scans() -> &'static RoomScans {
    static SCANS: once_cell::sync::Lazy<RoomScans> = once_cell::sync::Lazy::new(|| RoomScans {
        entries: std::sync::Mutex::new(std::collections::HashMap::new()),
    });
    &SCANS
}

impl RoomScans {
    fn get(&self, account: &str, room: &str) -> Option<Vec<PackRecord>> {
        let mut map = self.entries.lock().unwrap_or_else(|p| p.into_inner());
        let key = (account.to_owned(), room.to_owned());
        match map.get(&key) {
            Some((at, records)) if at.elapsed() < ROOM_SCAN_TTL => Some(records.clone()),
            Some(_) => {
                map.remove(&key);
                None
            }
            None => None,
        }
    }
    fn put(&self, account: &str, room: &str, records: Vec<PackRecord>) {
        let mut map = self.entries.lock().unwrap_or_else(|p| p.into_inner());
        if map.len() > 256 {
            map.clear();
        }
        map.insert((account.to_owned(), room.to_owned()), (std::time::Instant::now(), records));
    }
    fn forget(&self, account: &str, room: &str) {
        let mut map = self.entries.lock().unwrap_or_else(|p| p.into_inner());
        map.remove(&(account.to_owned(), room.to_owned()));
    }
}

pub(crate) fn forget_room(client: &matrix_sdk::Client, room_id: &str) {
    let account = account_key(client);
    room_scans().forget(&account, room_id);
    absent_answers().forget_prefix(&account, &format!("state|{room_id}|"));
}

fn record_from_event(namespace: Namespace, value: &Value) -> Option<PackRecord> {
    let state_key = value.get("state_key")?.as_str()?.to_owned();
    let content = value.get("content")?.clone();
    let ts = value.get("origin_server_ts").and_then(|v| v.as_u64()).unwrap_or(0);
    Some(PackRecord { namespace, state_key, content, ts })
}

async fn scan_room_packs(
    client: &matrix_sdk::Client,
    room_id: &str,
    deep: bool,
) -> (Vec<PackRecord>, String, bool) {
    use matrix_sdk::ruma::api::client::state::get_state_events;

    let Ok(room) = joined_room(client, room_id) else {
        return (Vec::new(), String::new(), false);
    };
    let name = room.name().unwrap_or_default();
    let mut records = Vec::new();
    for namespace in [Namespace::Stable, Namespace::Legacy] {
        let Ok(events) = room.get_state_events(StateEventType::from(namespace.room_type())).await
        else {
            continue;
        };
        for raw in events {
            let json = match &raw {
                RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
            };
            if let Ok(value) = serde_json::from_str::<Value>(&json) {
                records.extend(record_from_event(namespace, &value));
            }
        }
    }
    let mut scan_failed = false;
    if deep {
        let account = account_key(client);
        let scanned = match room_scans().get(&account, room_id) {
            Some(cached) => Some(cached),
            None => {
                let config = RequestConfig::new()
                    .disable_retry()
                    .timeout(Duration::from_secs(20));
                let request = get_state_events::v3::Request::new(room.room_id().to_owned());
                match client.send(request).with_request_config(config).await {
                    Ok(response) => {
                        let mut found = Vec::new();
                        for raw in response.room_state {
                            let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) else {
                                continue;
                            };
                            let namespace = match value.get("type").and_then(|v| v.as_str()) {
                                Some(STABLE_ROOM_PACK) => Namespace::Stable,
                                Some(ROOM_EMOTES) => Namespace::Legacy,
                                _ => continue,
                            };
                            found.extend(record_from_event(namespace, &value));
                            if found.len() >= MAX_PACKS * 2 {
                                break;
                            }
                        }
                        room_scans().put(&account, room_id, found.clone());
                        Some(found)
                    }
                    Err(_) => {
                        scan_failed = true;
                        None
                    }
                }
            }
        };
        records.extend(scanned.unwrap_or_default());
    }
    (records, name, scan_failed)
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct ParentClaim {
    pub space_id: String,
    pub canonical: bool,
    pub has_via: bool,
    pub reciprocal: bool,
    pub sender_authorised: bool,
}

pub(crate) fn pick_canonical_parent(claims: &[ParentClaim]) -> Option<String> {
    claims
        .iter()
        .filter(|c| c.canonical && c.has_via && (c.reciprocal || c.sender_authorised))
        .map(|c| c.space_id.clone())
        .min()
}

const MAX_ANCESTRY_DEPTH: usize = 8;

async fn canonical_ancestry(client: &matrix_sdk::Client, room_id: &str) -> Vec<String> {
    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
    use matrix_sdk::ruma::events::space::{
        child::SpaceChildEventContent, parent::SpaceParentEventContent,
    };
    use matrix_sdk::ruma::events::SyncStateEvent;

    let mut out = Vec::new();
    let mut visited: BTreeSet<String> = BTreeSet::new();
    visited.insert(room_id.to_owned());
    let mut current = room_id.to_owned();
    for _ in 0..MAX_ANCESTRY_DEPTH {
        let Ok(room) = joined_room(client, &current) else { break };
        let Ok(events) = room.get_state_events_static::<SpaceParentEventContent>().await else {
            break;
        };
        let mut claims = Vec::new();
        for raw in events {
            let Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(event))) = raw.deserialize()
            else {
                continue;
            };
            let space_id = event.state_key.to_string();
            let mut claim = ParentClaim {
                space_id: space_id.clone(),
                canonical: event.content.canonical,
                has_via: !event.content.via.is_empty(),
                reciprocal: false,
                sender_authorised: false,
            };
            if claim.canonical && claim.has_via {
                if let Ok(space) = joined_room(client, &space_id) {
                    if let Ok(Some(child)) = space
                        .get_state_event_static_for_key::<SpaceChildEventContent, _>(room.room_id())
                        .await
                    {
                        if let Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(child))) =
                            child.deserialize()
                        {
                            claim.reciprocal = !child.content.via.is_empty();
                        }
                    }
                    claim.sender_authorised = space
                        .get_member_no_sync(&event.sender)
                        .await
                        .ok()
                        .flatten()
                        .is_some_and(|m| m.can_send_state(StateEventType::SpaceChild));
                }
            }
            claims.push(claim);
        }
        let Some(parent) = pick_canonical_parent(&claims) else { break };
        if !visited.insert(parent.clone()) {
            break;
        }
        if joined_room(client, &parent).is_err() {
            break;
        }
        out.push(parent.clone());
        current = parent;
    }
    out
}

pub(crate) fn fetch_packs(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    deep: bool,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let mut packs: Vec<Pack> = Vec::new();
        let mut seen: BTreeSet<(String, String)> = BTreeSet::new();

        let stable_doc = read_global_account_data_typed(&client, STABLE_PACK_ROOMS).await;
        let legacy_doc = read_global_account_data(&client, EMOTE_ROOMS).await;
        let subs = effective_subscriptions(&stable_doc, legacy_doc.as_ref());
        let legacy_only: BTreeSet<(String, String)> = subs.legacy_only.iter().cloned().collect();

        // ---- 1a. the account's own (legacy) pack ------------------------
        //
        // `{}` (no `images`) is the legacy "pack removed" convention (our own
        // retire writes it): not an empty pack to list and select (live GUI
        // run: an emptied personal pack stayed selected in the picker as
        // "Your pack" with nothing in it).
        if let Some(content) = read_global_account_data(&client, USER_EMOTES)
            .await
            .filter(has_images)
        {
            if let Some(pack) = parse_pack_content(
                &content,
                "user".to_owned(),
                "user",
                String::new(),
                String::new(),
                "Your pack",
            ) {
                packs.push(pack);
            }
        }

        for (sub_room, state_key) in &subs.active {
            if packs.len() >= MAX_PACKS {
                break;
            }
            if !seen.insert((sub_room.clone(), state_key.clone())) {
                continue;
            }
            let (namespace, read, name) = read_subscribed_pack(&client, sub_room, state_key).await;
            let mut pack = match read {
                PackRead::Found(content) => parse_pack_content_ns(
                    &content,
                    room_pack_id(namespace, sub_room, state_key),
                    "room",
                    sub_room.clone(),
                    state_key.clone(),
                    &name,
                    namespace,
                ),
                PackRead::Absent => Some(unavailable_pack(namespace, sub_room, state_key, &name, "absent")),
                PackRead::Failed(category) => {
                    Some(unavailable_pack(namespace, sub_room, state_key, &name, &category))
                }
            };
            if let Some(pack) = pack.as_mut() {
                pack.enabled_globally = true;
                pack.can_manage = pack.available
                    && can_manage_room_packs(&client, sub_room, namespace).await;
            }
            packs.extend(pack);
        }

        let mut room_can_manage = false;
        let mut room_can_manage_legacy = false;
        let mut room_scan_failed = false;
        if !room_id.is_empty() {
            room_can_manage = can_manage_room_packs(&client, &room_id, Namespace::Stable).await;
            room_can_manage_legacy =
                can_manage_room_packs(&client, &room_id, Namespace::Legacy).await;
            let (records, name, scan_failed) = scan_room_packs(&client, &room_id, deep).await;
            room_scan_failed = scan_failed;
            for resolved in resolve_room_packs(records) {
                if packs.len() >= MAX_PACKS {
                    break;
                }
                let coordinate = (room_id.clone(), resolved.state_key.clone());
                if seen.contains(&coordinate) {
                    continue;
                }
                seen.insert(coordinate.clone());
                if let Some(mut pack) = parse_pack_content_ns(
                    &resolved.content,
                    room_pack_id(resolved.namespace, &room_id, &resolved.state_key),
                    "room",
                    room_id.clone(),
                    resolved.state_key.clone(),
                    &name,
                    resolved.namespace,
                ) {
                    pack.legacy_twin = resolved.legacy_twin;
                    pack.legacy_subscribed = legacy_only.contains(&coordinate);
                    pack.can_manage = match resolved.namespace {
                        Namespace::Stable => room_can_manage,
                        Namespace::Legacy => room_can_manage_legacy,
                    };
                    packs.push(pack);
                }
            }
        }

        let mut ancestry_len = 0usize;
        let mut ancestry_ids: Vec<String> = Vec::new();
        if !room_id.is_empty() {
            let ancestors = canonical_ancestry(&client, &room_id).await;
            ancestry_len = ancestors.len();
            ancestry_ids = ancestors.clone();
            for space_id in ancestors {
                if packs.len() >= MAX_PACKS {
                    break;
                }
                let can_stable = can_manage_room_packs(&client, &space_id, Namespace::Stable).await;
                let can_legacy = can_manage_room_packs(&client, &space_id, Namespace::Legacy).await;
                let (records, name, _) = scan_room_packs(&client, &space_id, deep).await;
                for resolved in resolve_room_packs(records) {
                    if packs.len() >= MAX_PACKS {
                        break;
                    }
                    let coordinate = (space_id.clone(), resolved.state_key.clone());
                    if !seen.insert(coordinate.clone()) {
                        continue;
                    }
                    if let Some(mut pack) = parse_pack_content_ns(
                        &resolved.content,
                        room_pack_id(resolved.namespace, &space_id, &resolved.state_key),
                        "room",
                        space_id.clone(),
                        resolved.state_key.clone(),
                        &name,
                        resolved.namespace,
                    ) {
                        pack.legacy_twin = resolved.legacy_twin;
                        pack.legacy_subscribed = legacy_only.contains(&coordinate);
                        pack.from_space = true;
                        pack.can_manage = match resolved.namespace {
                            Namespace::Stable => can_stable,
                            Namespace::Legacy => can_legacy,
                        };
                        packs.push(pack);
                    }
                }
            }
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "sticker_packs",
                "canonical_ancestors": ancestry_len,
                "canonical_ancestor_ids": ancestry_ids,
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "room_can_manage": room_can_manage,
                "room_can_manage_legacy": room_can_manage_legacy,
                "room_scan_failed": room_scan_failed,
                "subscriptions_source": subs.source,
                "stable_subscriptions_unreadable": subs.stable_unreadable,
                "legacy_only_subscriptions": subs.legacy_only.len(),
                "packs": packs.iter().map(Pack::to_json).collect::<Vec<_>>(),
            }),
        );
    });
    Ok(())
}

fn unavailable_pack(
    namespace: Namespace,
    room_id: &str,
    state_key: &str,
    name: &str,
    reason: &str,
) -> Pack {
    Pack {
        id: room_pack_id(namespace, room_id, state_key),
        namespace: namespace.as_str(),
        legacy_twin: false,
        legacy_subscribed: false,
        usage_unsupported: false,
        available: false,
        unavailable: if reason == "unknown_room" { "not_joined".to_owned() } else { reason.to_owned() },
        from_space: false,
        pack_emoticon: false,
        pack_sticker: false,
        display_name: one_line(name, MAX_PACK_NAME_CHARS),
        avatar_url: String::new(),
        attribution: String::new(),
        source: "room",
        room_id: room_id.to_owned(),
        state_key: state_key.to_owned(),
        enabled_globally: true,
        can_manage: false,
        images: Vec::new(),
    }
}

async fn read_subscribed_pack(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
) -> (Namespace, PackRead, String) {
    let (stable, name) = read_one_room_pack(client, room_id, state_key, Namespace::Stable).await;
    match stable {
        PackRead::Found(content) if has_images(&content) => {
            (Namespace::Stable, PackRead::Found(content), name)
        }
        PackRead::Found(_) => (Namespace::Stable, PackRead::Absent, name),
        PackRead::Failed(category) => (Namespace::Stable, PackRead::Failed(category), name),
        PackRead::Absent => {
            let (legacy, name) =
                read_one_room_pack(client, room_id, state_key, Namespace::Legacy).await;
            match legacy {
                PackRead::Found(content) if has_images(&content) => {
                    (Namespace::Legacy, PackRead::Found(content), name)
                }
                PackRead::Found(_) | PackRead::Absent => (Namespace::Stable, PackRead::Absent, name),
                other => (Namespace::Legacy, other, name),
            }
        }
    }
}

async fn can_manage_room_packs(
    client: &matrix_sdk::Client,
    room_id: &str,
    namespace: Namespace,
) -> bool {
    let Ok(room) = joined_room(client, room_id) else {
        return false;
    };
    let Some(own) = client.user_id().map(|id| id.to_owned()) else {
        return false;
    };
    room.get_member_no_sync(&own)
        .await
        .ok()
        .flatten()
        .is_some_and(|m| m.can_send_state(StateEventType::from(namespace.room_type())))
}

pub(crate) fn set_room_pack_enabled(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    enabled: bool,
    namespace: Namespace,
) -> Result<(), String> {
    if RoomId::parse(&room_id).is_err() {
        return Err("room id is not valid".to_owned());
    }
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome =
            set_room_pack_enabled_inner(&client, &room_id, &state_key, enabled, namespace)
                .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, mirror) = match outcome {
            Ok(mirror) => (true, String::new(), mirror),
            Err(category) => (false, category, "none"),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_rooms_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "mirror": mirror,
                "room_id": room_id,
                "state_key": state_key,
                "enabled": enabled,
            }),
        );
    });
    Ok(())
}

pub(crate) fn apply_emote_rooms_change(
    existing: Value,
    room_id: &str,
    state_key: &str,
    enabled: bool,
) -> Result<Value, String> {
    let mut content = existing;
    if !content.is_object() {
        content = json!({});
    }
    if content.get("rooms").and_then(|v| v.as_object()).is_none() {
        content["rooms"] = json!({});
    }

    let rooms = content["rooms"]
        .as_object_mut()
        .ok_or_else(|| "rejected".to_owned())?;
    if enabled {
        if rooms.len() >= MAX_PACKS && !rooms.contains_key(room_id) {
            return Err("too_many_rooms".to_owned());
        }
        let entry = rooms.entry(room_id.to_owned()).or_insert_with(|| json!({}));
        if !entry.is_object() {
            *entry = json!({});
        }
        entry
            .as_object_mut()
            .ok_or_else(|| "rejected".to_owned())?
            .entry(state_key.to_owned())
            .or_insert_with(|| json!({}));
    } else {
        let mut room_now_empty = false;
        if let Some(entry) = rooms.get_mut(room_id).and_then(|v| v.as_object_mut())
        {
            entry.remove(state_key);
            room_now_empty = entry.is_empty();
        }
        if room_now_empty {
            rooms.remove(room_id);
        }
    }
    Ok(content)
}

async fn write_account_data(
    client: &matrix_sdk::Client,
    event_type: &str,
    content: &Value,
) -> Result<(), String> {
    let raw = matrix_sdk::ruma::serde::Raw::new(content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    let written = client
        .account()
        .set_account_data_raw(GlobalAccountDataEventType::from(event_type), raw)
        .await;
    forget_own_write(client, event_type);
    written
        .map(|_| ())
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())
}

async fn set_room_pack_enabled_inner(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    enabled: bool,
    namespace: Namespace,
) -> Result<&'static str, String> {
    let stable = account_data_read(
        client
            .account()
            .fetch_account_data(GlobalAccountDataEventType::from(STABLE_PACK_ROOMS))
            .await,
    );
    let legacy = account_data_read(
        client
            .account()
            .fetch_account_data(GlobalAccountDataEventType::from(EMOTE_ROOMS))
            .await,
    );
    if stable == PackRead::Absent {
        if let PackRead::Failed(category) = &legacy {
            return Err(category.clone());
        }
    }
    let legacy_value = match &legacy {
        PackRead::Found(value) => Some(value.clone()),
        _ => None,
    };
    let content = plan_stable_subscription_write(
        stable,
        legacy_value.as_ref(),
        room_id,
        state_key,
        enabled,
    )?;
    write_account_data(client, STABLE_PACK_ROOMS, &content).await?;

    if namespace != Namespace::Legacy {
        return Ok("none");
    }
    let mirrored = match legacy {
        PackRead::Failed(_) => Err("unreadable".to_owned()),
        other => {
            let base = match other {
                PackRead::Found(value) => value,
                _ => json!({}),
            };
            match apply_emote_rooms_change(base, room_id, state_key, enabled) {
                Ok(content) => write_account_data(client, EMOTE_ROOMS, &content).await,
                Err(category) => Err(category),
            }
        }
    };
    Ok(if mirrored.is_ok() { "ok" } else { "failed" })
}

pub(crate) struct AbsentAnswers {
    marks: std::sync::Mutex<std::collections::HashMap<(String, String), std::time::Instant>>,
}

const ABSENT_ANSWER_TTL: Duration = Duration::from_secs(15 * 60);

impl AbsentAnswers {
    pub(crate) fn new() -> Self {
        Self { marks: std::sync::Mutex::new(std::collections::HashMap::new()) }
    }

    fn lock(
        &self,
    ) -> std::sync::MutexGuard<'_, std::collections::HashMap<(String, String), std::time::Instant>>
    {
        match self.marks.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        }
    }

    pub(crate) fn is_absent(&self, account: &str, event_type: &str, now: std::time::Instant) -> bool {
        let mut marks = self.lock();
        let key = (account.to_owned(), event_type.to_owned());
        match marks.get(&key) {
            Some(at) if now.saturating_duration_since(*at) < ABSENT_ANSWER_TTL => true,
            Some(_) => {
                marks.remove(&key);
                false
            }
            None => false,
        }
    }

    pub(crate) fn mark_absent(&self, account: &str, event_type: &str, now: std::time::Instant) {
        self.lock().insert((account.to_owned(), event_type.to_owned()), now);
    }

    pub(crate) fn forget(&self, account: &str, event_type: &str) {
        self.lock().remove(&(account.to_owned(), event_type.to_owned()));
    }

    pub(crate) fn forget_prefix(&self, account: &str, prefix: &str) {
        self.lock()
            .retain(|(a, key), _| !(a == account && key.starts_with(prefix)));
    }
}

fn absent_answers() -> &'static AbsentAnswers {
    static ABSENT: once_cell::sync::Lazy<AbsentAnswers> = once_cell::sync::Lazy::new(AbsentAnswers::new);
    &ABSENT
}

fn account_key(client: &matrix_sdk::Client) -> String {
    client.user_id().map(|u| u.to_string()).unwrap_or_default()
}

pub(crate) enum ServerAnswer {
    Found(Value),
    Absent,
    Failed,
}

pub(crate) async fn read_remembering_absence<S, Q, QF>(
    memory: &AbsentAnswers,
    account: &str,
    event_type: &str,
    store: S,
    ask: Q,
) -> Option<Value>
where
    S: std::future::Future<Output = Option<Value>>,
    Q: FnOnce() -> QF,
    QF: std::future::Future<Output = ServerAnswer>,
{
    if let Some(value) = store.await {
        memory.forget(account, event_type);
        return Some(value);
    }
    if memory.is_absent(account, event_type, std::time::Instant::now()) {
        return None;
    }
    match ask().await {
        ServerAnswer::Found(value) => Some(value),
        ServerAnswer::Absent => {
            memory.mark_absent(account, event_type, std::time::Instant::now());
            None
        }
        ServerAnswer::Failed => None,
    }
}

fn forget_own_write(client: &matrix_sdk::Client, event_type: &str) {
    absent_answers().forget(&account_key(client), event_type);
}

fn server_answer<E>(
    fetched: Result<Option<matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnyGlobalAccountDataEventContent>>, E>,
) -> ServerAnswer {
    match fetched {
        Ok(Some(raw)) => match serde_json::from_str::<Value>(raw.json().get()) {
            Ok(value) => ServerAnswer::Found(value),
            Err(_) => ServerAnswer::Failed,
        },
        Ok(None) => ServerAnswer::Absent,
        Err(_) => ServerAnswer::Failed,
    }
}

fn account_data_read<E: std::fmt::Display>(
    fetched: Result<Option<matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnyGlobalAccountDataEventContent>>, E>,
) -> PackRead {
    match fetched {
        Ok(Some(raw)) => classify_pack_read(Ok(raw.json().get())),
        Ok(None) => PackRead::Absent,
        Err(err) => match classify_room_error(&err.to_string()) {
            // Not M_NOT_FOUND (that is Ok(None)): an endpoint-level 404 is a
            // failure, not an answer about this account.
            "not_found" => PackRead::Failed("network".to_owned()),
            other => PackRead::Failed(other.to_owned()),
        },
    }
}

pub(crate) async fn read_remembering_absence_typed<S, Q, QF>(
    memory: &AbsentAnswers,
    account: &str,
    event_type: &str,
    store: S,
    ask: Q,
) -> PackRead
where
    S: std::future::Future<Output = Option<Value>>,
    Q: FnOnce() -> QF,
    QF: std::future::Future<Output = ServerAnswer>,
{
    if let Some(value) = store.await {
        memory.forget(account, event_type);
        return PackRead::Found(value);
    }
    if memory.is_absent(account, event_type, std::time::Instant::now()) {
        return PackRead::Absent;
    }
    match ask().await {
        ServerAnswer::Found(value) => PackRead::Found(value),
        ServerAnswer::Absent => {
            memory.mark_absent(account, event_type, std::time::Instant::now());
            PackRead::Absent
        }
        ServerAnswer::Failed => PackRead::Failed("network".to_owned()),
    }
}

async fn read_global_account_data_typed(
    client: &matrix_sdk::Client,
    event_type: &str,
) -> PackRead {
    let ty = GlobalAccountDataEventType::from(event_type);
    read_remembering_absence_typed(
        absent_answers(),
        &account_key(client),
        event_type,
        async {
            match client.account().account_data_raw(ty.clone()).await {
                Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get())
                    .ok()
                    .filter(|v| v.is_object()),
                _ => None,
            }
        },
        || async { server_answer(client.account().fetch_account_data(ty.clone()).await) },
    )
    .await
}

async fn read_global_account_data(
    client: &matrix_sdk::Client,
    event_type: &str,
) -> Option<Value> {
    let ty = GlobalAccountDataEventType::from(event_type);
    read_remembering_absence(
        absent_answers(),
        &account_key(client),
        event_type,
        async {
            match client.account().account_data_raw(ty.clone()).await {
                Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get()).ok(),
                _ => None,
            }
        },
        || async { server_answer(client.account().fetch_account_data(ty.clone()).await) },
    )
    .await
}

/// What reading one room pack came back as. `Absent` is the server's own
/// M_NOT_FOUND and nothing else: a timeout, a refusal or an unparseable body
/// says nothing about whether the pack exists, and must never be read as
/// "empty" by a writer (a failed read followed by a full-state PUT would
/// replace the real pack with a one-image object).
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum PackRead {
    Found(Value),
    Absent,
    Failed(String),
}

pub(crate) fn classify_pack_read(result: Result<&str, String>) -> PackRead {
    match result {
        Ok(text) => match serde_json::from_str::<Value>(text) {
            Ok(value) if value.is_object() => PackRead::Found(value),
            _ => PackRead::Failed("malformed".to_owned()),
        },
        Err(message) => match classify_room_error(&message) {
            "not_found" => PackRead::Absent,
            other => PackRead::Failed(other.to_owned()),
        },
    }
}

pub(crate) fn base_for_add(read: PackRead) -> Result<Value, String> {
    match read {
        PackRead::Found(content) => Ok(content),
        // A schema-valid empty pack: `images` is required (v1.19).
        PackRead::Absent => Ok(json!({ "images": {} })),
        PackRead::Failed(category) => Err(category),
    }
}

pub(crate) fn base_for_edit(read: PackRead) -> Result<Value, String> {
    match read {
        PackRead::Found(content) => Ok(content),
        PackRead::Absent => Err("not_found".to_owned()),
        PackRead::Failed(category) => Err(category),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ReadPolicy {
    Snapshot,
    Fresh,
}

pub(crate) async fn read_pack_with_policy<S, Q, QF>(
    policy: ReadPolicy,
    store: S,
    memo_absent: bool,
    ask: Q,
) -> (PackRead, bool)
where
    S: std::future::Future<Output = Option<Value>>,
    Q: FnOnce() -> QF,
    QF: std::future::Future<Output = PackRead>,
{
    if policy == ReadPolicy::Snapshot {
        if let Some(content) = store.await {
            return (PackRead::Found(content), false);
        }
        if memo_absent {
            return (PackRead::Absent, false);
        }
    }
    let read = ask().await;
    let remember = policy == ReadPolicy::Snapshot && read == PackRead::Absent;
    (read, remember)
}

async fn read_one_room_pack(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    namespace: Namespace,
) -> (PackRead, String) {
    read_room_pack_with(client, room_id, state_key, namespace, ReadPolicy::Snapshot).await
}

async fn read_room_pack_fresh(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    namespace: Namespace,
) -> PackRead {
    read_room_pack_with(client, room_id, state_key, namespace, ReadPolicy::Fresh).await.0
}

async fn read_room_pack_with(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    namespace: Namespace,
    policy: ReadPolicy,
) -> (PackRead, String) {
    use matrix_sdk::ruma::api::client::state::get_state_event_for_key;

    let Ok(room) = joined_room(client, room_id) else {
        return (PackRead::Failed("unknown_room".to_owned()), String::new());
    };
    let name = room.name().unwrap_or_default();
    let event_type = namespace.room_type();

    let account = account_key(client);
    let memo = format!("state|{room_id}|{event_type}|{state_key}");
    let memo_absent =
        absent_answers().is_absent(&account, &memo, std::time::Instant::now());
    let store = async {
        let Ok(Some(raw)) = room
            .get_state_event(StateEventType::from(event_type), state_key)
            .await
        else {
            return None;
        };
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        let value = serde_json::from_str::<Value>(&json).ok()?;
        let content = value.get("content")?;
        content.get("images").is_some().then(|| content.clone())
    };
    let room_ref = &room;
    let ask = move || async move {
        let room = room_ref;
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(PACK_REQUEST_TIMEOUT);
        let request = get_state_event_for_key::v3::Request::new(
            room.room_id().to_owned(),
            StateEventType::from(event_type),
            state_key.to_owned(),
        );
        match client.send(request).with_request_config(config).await {
            Ok(response) => classify_pack_read(Ok(response.event_or_content.get())),
            Err(err) => classify_pack_read(Err(err.to_string())),
        }
    };
    let (read, remember) = read_pack_with_policy(policy, store, memo_absent, ask).await;
    if remember {
        absent_answers().mark_absent(&account, &memo, std::time::Instant::now());
    } else if matches!(read, PackRead::Found(_)) {
        absent_answers().forget(&account, &memo);
    }
    (read, name)
}

fn sticker_image_info(mimetype: String, width: u64, height: u64, size: u64) -> ImageInfo {
    let mut info = ImageInfo::new();
    if !mimetype.is_empty() {
        info.mimetype = Some(mimetype);
    }
    if width > 0 {
        info.width = UInt::new(width);
    }
    if height > 0 {
        info.height = UInt::new(height);
    }
    if size > 0 {
        info.size = UInt::new(size);
    }
    info
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_sticker(
    bridge: &RustClient,
    room_id: String,
    thread_root_id: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
) -> Result<(), String> {
    // Re-check at the edge: a non-mxc source must never be sent.
    if !is_valid_mxc(&url) {
        return Err("sticker url is not an mxc URI".to_owned());
    }
    if !mimetype.is_empty() && !mimetype_allowed(Some(&mimetype)) {
        return Err("sticker mimetype is not a supported raster image".to_owned());
    }
    let body = one_line(&body, MAX_BODY_CHARS);
    if body.is_empty() {
        return Err("sticker body is empty".to_owned());
    }

    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };

    let content = StickerEventContent::new(
        body,
        sticker_image_info(mimetype, width, height, size),
        OwnedMxcUri::from(url),
    );

    bridge.timelines.send_content(
        &bridge.runtime,
        client,
        room_id,
        thread_root_id,
        AnyMessageLikeEventContent::Sticker(content),
        "sticker_send_failed",
    )
}

pub(crate) fn header_dimensions(data: &[u8]) -> (u64, u64) {
    let be16 = |i: usize| -> Option<u64> {
        Some(u64::from(u16::from_be_bytes([*data.get(i)?, *data.get(i + 1)?])))
    };
    let le16 = |i: usize| -> Option<u64> {
        Some(u64::from(u16::from_le_bytes([*data.get(i)?, *data.get(i + 1)?])))
    };
    let le24 = |i: usize| -> Option<u64> {
        Some(u64::from(*data.get(i)?)
            | u64::from(*data.get(i + 1)?) << 8
            | u64::from(*data.get(i + 2)?) << 16)
    };
    let found: Option<(u64, u64)> = (|| {
        if data.starts_with(b"\x89PNG\r\n\x1a\n") && data.get(12..16)? == b"IHDR" {
            let w = u32::from_be_bytes(data.get(16..20)?.try_into().ok()?);
            let h = u32::from_be_bytes(data.get(20..24)?.try_into().ok()?);
            return Some((u64::from(w), u64::from(h)));
        }
        if data.starts_with(b"GIF87a") || data.starts_with(b"GIF89a") {
            return Some((le16(6)?, le16(8)?));
        }
        if data.starts_with(b"RIFF") && data.get(8..12)? == b"WEBP" {
            return match data.get(12..16)? {
                b"VP8 " => Some((le16(26)? & 0x3fff, le16(28)? & 0x3fff)),
                b"VP8L" => {
                    let b = data.get(21..25)?;
                    let w = 1 + (u64::from(b[0]) | (u64::from(b[1]) & 0x3f) << 8);
                    let h = 1 + (u64::from(b[1]) >> 6
                        | u64::from(b[2]) << 2
                        | (u64::from(b[3]) & 0x0f) << 10);
                    Some((w, h))
                }
                b"VP8X" => Some((le24(24)? + 1, le24(27)? + 1)),
                _ => None,
            };
        }
        if data.starts_with(&[0xff, 0xd8]) {
            let mut i = 2usize;
            for _ in 0..512 {
                while *data.get(i)? != 0xff {
                    i += 1;
                }
                while *data.get(i)? == 0xff {
                    i += 1;
                }
                let marker = *data.get(i)?;
                i += 1;
                match marker {
                    0xd8 | 0x01 | 0xd0..=0xd7 => continue,
                    0xd9 | 0xda => return None,
                    0xc0..=0xcf if marker != 0xc4 && marker != 0xc8 && marker != 0xcc => {
                        return Some((be16(i + 5)?, be16(i + 3)?));
                    }
                    _ => {
                        let len = usize::try_from(be16(i)?).ok()?;
                        if len < 2 {
                            return None;
                        }
                        i += len;
                    }
                }
            }
        }
        None
    })();
    match found {
        Some((w, h)) if (1..=65_535).contains(&w) && (1..=65_535).contains(&h) => (w, h),
        _ => (0, 0),
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn upload_to_user_pack(
    bridge: &RustClient,
    op_id: u64,
    shortcode: String,
    body: String,
    local_path: String,
    usage: String,
) -> Result<(), String> {
    let metadata = std::fs::metadata(&local_path)
        .map_err(|_| "sticker file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("sticker path is not a regular file".to_owned());
    }
    if metadata.len() == 0 || metadata.len() > MAX_STICKER_UPLOAD_BYTES {
        return Err("sticker file size is out of range".to_owned());
    }
    let requested = propose_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = async {
            let data = tokio::fs::read(&local_path)
                .await
                .map_err(|_| "read_failed".to_owned())?;
            let mime_str = crate::rooms::sniff_image_mime(&data)
                .ok_or_else(|| "unsupported_image".to_owned())?;
            let mime: mime::Mime = mime_str
                .parse()
                .map_err(|_| "unsupported_image".to_owned())?;
            let size = data.len() as u64;
            let (width, height) = header_dimensions(&data);
            let upload = client
                .media()
                .upload(&mime, data, None)
                .await
                .map_err(|_| "upload_failed".to_owned())?;
            let url = upload.content_uri.to_string();
            add_to_user_pack_inner(
                &client, requested, url, body, mime_str.to_owned(), width, height,
                size, &usage,
            )
            .await
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_add_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "error": category,
                "shortcode": shortcode,
            }),
        );
    });
    Ok(())
}

pub(crate) fn add_to_user_pack(
    bridge: &RustClient,
    op_id: u64,
    shortcode: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
    usage: String,
) -> Result<(), String> {
    if !is_valid_mxc(&url) {
        return Err("sticker url is not an mxc URI".to_owned());
    }
    if !mimetype.is_empty() && !mimetype_allowed(Some(&mimetype)) {
        return Err("sticker mimetype is not a supported raster image".to_owned());
    }
    let requested = propose_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = add_to_user_pack_inner(
            &client, requested, url, body, mimetype, width, height, size,
            &usage,
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_add_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "shortcode": shortcode,
            }),
        );
    });
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn add_to_room_pack(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    shortcode: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
    namespace: Namespace,
) -> Result<(), String> {
    if !is_valid_mxc(&url) {
        return Err("sticker url is not an mxc URI".to_owned());
    }
    if !mimetype.is_empty() && !mimetype_allowed(Some(&mimetype)) {
        return Err("sticker mimetype is not a supported raster image".to_owned());
    }
    if RoomId::parse(&room_id).is_err() {
        return Err("room id is not valid".to_owned());
    }
    let requested = propose_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = add_to_room_pack_inner(
            &client, &room_id, &state_key, requested, url, body, mimetype,
            width, height, size, namespace,
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_add_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "shortcode": shortcode,
            }),
        );
    });
    Ok(())
}

#[allow(clippy::too_many_arguments)]
async fn add_to_room_pack_inner(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    requested: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
    namespace: Namespace,
) -> Result<String, String> {
    let room = joined_room(client, room_id).map_err(|_| "unknown_room".to_owned())?;
    if !can_manage_room_packs(client, room_id, namespace).await {
        return Err("forbidden".to_owned());
    }

    // Only the server's own "not found" is a first use; a failed read writes
    // nothing (PACK-LTN-017).
    let existing =
        base_for_add(read_room_pack_fresh(client, room_id, state_key, namespace).await)?;

    let usage = match namespace {
        Namespace::Stable => "",
        Namespace::Legacy => "sticker",
    };
    let (content, code) =
        add_image_to_pack_content(existing, requested, url, body, mimetype,
                                  width, height, size, usage)?;

    let written = room
        .send_state_event_raw(namespace.room_type(), state_key, content)
        .await;
    forget_room(client, room_id);
    written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn upload_to_room_pack(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    namespace: Namespace,
    shortcode: String,
    body: String,
    local_path: String,
    reuse_url: String,
) -> Result<(), String> {
    if RoomId::parse(&room_id).is_err() {
        return Err("room id is not valid".to_owned());
    }
    let reuse = is_valid_mxc(&reuse_url).then_some(reuse_url);
    if reuse.is_none() {
        let metadata = std::fs::metadata(&local_path)
            .map_err(|_| "sticker file is not readable".to_owned())?;
        if !metadata.is_file() {
            return Err("sticker path is not a regular file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_STICKER_UPLOAD_BYTES {
            return Err("sticker file size is out of range".to_owned());
        }
    }
    let requested = propose_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let mut uploaded = String::new();
        let outcome = async {
            let (url, mime, size, width, height) = match reuse {
                Some(url) => (url, String::new(), 0u64, 0u64, 0u64),
                None => {
                    let data = tokio::fs::read(&local_path)
                        .await
                        .map_err(|_| "read_failed".to_owned())?;
                    let mime_str = crate::rooms::sniff_image_mime(&data)
                        .ok_or_else(|| "unsupported_image".to_owned())?;
                    let mime: mime::Mime =
                        mime_str.parse().map_err(|_| "unsupported_image".to_owned())?;
                    let size = data.len() as u64;
                    let (width, height) = header_dimensions(&data);
                    let upload = client
                        .media()
                        .upload(&mime, data, None)
                        .await
                        .map_err(|_| "upload_failed".to_owned())?;
                    (upload.content_uri.to_string(), mime_str.to_owned(), size, width, height)
                }
            };
            uploaded = url.clone();
            add_to_room_pack_inner(
                &client, &room_id, &state_key, requested, url, body, mime, width, height,
                size, namespace,
            )
            .await
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_add_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "shortcode": shortcode,
                "uploaded_url": uploaded,
            }),
        );
    });
    Ok(())
}

pub(crate) fn usage_value(usage: &str) -> Value {
    match usage {
        "emoticon" => json!(["emoticon"]),
        "both" => json!(["sticker", "emoticon"]),
        _ => json!(["sticker"]),
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn add_image_to_pack_content(
    existing: Value,
    requested: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
    usage: &str,
) -> Result<(Value, String), String> {
    let mut content = existing;
    if !content.is_object() {
        content = json!({});
    }
    if content.get("images").and_then(|v| v.as_object()).is_none() {
        content["images"] = json!({});
    }
    let images = content["images"]
        .as_object_mut()
        .ok_or_else(|| "rejected".to_owned())?;

    if images.len() >= MAX_IMAGES_PER_PACK {
        return Err("pack_full".to_owned());
    }
    if images
        .values()
        .any(|v| v.get("url").and_then(|u| u.as_str()) == Some(url.as_str()))
    {
        return Err("duplicate".to_owned());
    }

    let base = if requested.is_empty() {
        "sticker".to_owned()
    } else {
        requested
    };
    let mut code = base.clone();
    let mut n = 2u32;
    while images.contains_key(&code) {
        let suffix = format!("-{n}");
        let keep = base.len().min(MAX_SHORTCODE_BYTES - suffix.len());
        code = format!("{}{suffix}", &base[..keep]);
        n += 1;
        if n > 999 {
            return Err("rejected".to_owned());
        }
    }

    let mut info = serde_json::Map::new();
    if !mimetype.is_empty() {
        info.insert("mimetype".to_owned(), json!(mimetype));
    }
    if width > 0 {
        info.insert("w".to_owned(), json!(width));
    }
    if height > 0 {
        info.insert("h".to_owned(), json!(height));
    }
    if size > 0 {
        info.insert("size".to_owned(), json!(size));
    }

    let mut entry = serde_json::Map::new();
    entry.insert("url".to_owned(), json!(url));
    if !body.is_empty() && body != code {
        entry.insert("body".to_owned(), json!(body));
    }
    if !info.is_empty() {
        entry.insert("info".to_owned(), Value::Object(info));
    }
    if !usage.is_empty() {
        entry.insert("usage".to_owned(), usage_value(usage));
    }

    images.insert(code.clone(), Value::Object(entry));
    Ok((content, code))
}

pub(crate) enum PackEdit {
    RemoveImage { key: String },
    RenameImage { from: String, to: String },
    SetName { name: String },
    SetUsage { usage: String },
    SetBody { key: String, body: String },
    DeletePack,
}

impl PackEdit {
    fn apply(&self, existing: Value) -> Result<(Value, String), String> {
        self.apply_in(existing, Namespace::Legacy)
    }

    fn apply_in(&self, existing: Value, namespace: Namespace) -> Result<(Value, String), String> {
        match self {
            PackEdit::RemoveImage { key } => {
                remove_image_from_pack_content(existing, key)
                    .map(|c| (c, String::new()))
            }
            PackEdit::RenameImage { from, to } => {
                rename_image_in_pack_content(existing, from, to)
            }
            PackEdit::SetName { name } => {
                set_pack_display_name_content(existing, name)
                    .map(|c| (c, String::new()))
            }
            PackEdit::SetBody { key, body } => {
                set_image_body_content(existing, key, body).map(|c| (c, String::new()))
            }
            PackEdit::SetUsage { usage } => {
                if namespace != Namespace::Stable {
                    return Err("unsupported".to_owned());
                }
                set_pack_usage_content(existing, usage).map(|c| (c, String::new()))
            }
            // Legacy drops the name too: a named empty pack would not read as
            // deleted there.
            PackEdit::DeletePack => match namespace {
                Namespace::Legacy => Ok((json!({}), String::new())),
                Namespace::Stable => {
                    let mut out = json!({ "images": {} });
                    if let Some(pack) = existing.get("pack").filter(|p| p.is_object()) {
                        out["pack"] = pack.clone();
                    }
                    Ok((out, String::new()))
                }
            },
        }
    }
}

pub(crate) fn edit_pack(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    edit: PackEdit,
    namespace: Namespace,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = if room_id.is_empty() {
            edit_user_pack_inner(&client, &edit).await
        } else {
            edit_room_pack_inner(&client, &room_id, &state_key, &edit, namespace).await
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_edit_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "shortcode": shortcode,
            }),
        );
    });
    Ok(())
}

async fn edit_user_pack_inner(
    client: &matrix_sdk::Client,
    edit: &PackEdit,
) -> Result<String, String> {
    let ty = GlobalAccountDataEventType::from(USER_EMOTES);
    // Unlike the add path, a 404 is a real answer here: these operations edit
    // something that must already exist. An unparseable body is a failed
    // read, never an empty pack to overwrite.
    let existing = base_for_edit(account_data_read(
        client.account().fetch_account_data(ty.clone()).await,
    ))?;
    let (content, code) = edit.apply(existing)?;
    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    let written = client.account().set_account_data_raw(ty, raw).await;
    forget_own_write(&client, USER_EMOTES);
    written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

async fn edit_room_pack_inner(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    edit: &PackEdit,
    namespace: Namespace,
) -> Result<String, String> {
    let room = joined_room(client, room_id).map_err(|_| "unknown_room".to_owned())?;
    if !can_manage_room_packs(client, room_id, namespace).await {
        return Err("forbidden".to_owned());
    }
    let existing =
        base_for_edit(read_room_pack_fresh(client, room_id, state_key, namespace).await)?;
    let (content, code) = edit.apply_in(existing, namespace)?;
    let written = room
        .send_state_event_raw(namespace.room_type(), state_key, content)
        .await;
    forget_room(client, room_id);
    written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

/// The content of a NEW stable pack: schema-valid (`images` present even when
/// empty, which some reference clients omit), with the name and pack-wide
/// usage the user chose (`both` writes no usage: absent means every usage).
pub(crate) fn new_stable_pack_content(name: &str, usage: &str) -> Value {
    let mut pack = serde_json::Map::new();
    let name = one_line(name, MAX_PACK_NAME_CHARS);
    if !name.is_empty() {
        pack.insert("display_name".to_owned(), json!(name));
    }
    match usage {
        "emoticon" => {
            pack.insert("usage".to_owned(), json!(["emoticon"]));
        }
        "sticker" => {
            pack.insert("usage".to_owned(), json!(["sticker"]));
        }
        _ => {}
    }
    let mut content = json!({ "images": {} });
    if !pack.is_empty() {
        content["pack"] = Value::Object(pack);
    }
    content
}

pub(crate) fn plan_create(read: PackRead, name: &str, usage: &str) -> Result<Value, String> {
    match read {
        PackRead::Absent => Ok(new_stable_pack_content(name, usage)),
        PackRead::Found(_) => Err("pack_exists".to_owned()),
        PackRead::Failed(category) => Err(category),
    }
}

pub(crate) fn create_room_pack(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    name: String,
    usage: String,
) -> Result<(), String> {
    if RoomId::parse(&room_id).is_err() {
        return Err("room id is not valid".to_owned());
    }
    if state_key.len() > 255 {
        return Err("state key is too long".to_owned());
    }
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = async {
            let room = joined_room(&client, &room_id).map_err(|_| "unknown_room".to_owned())?;
            if !can_manage_room_packs(&client, &room_id, Namespace::Stable).await {
                return Err("forbidden".to_owned());
            }
            let read = read_room_pack_fresh(&client, &room_id, &state_key, Namespace::Stable)
                .await;
            let content = plan_create(read, &name, &usage)?;
            let written = room
                .send_state_event_raw(STABLE_ROOM_PACK, &state_key, content)
                .await;
            forget_room(&client, &room_id);
            written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
            Ok::<String, String>(state_key.clone())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, shortcode) = match outcome {
            Ok(code) => (true, String::new(), code),
            Err(category) => (false, category, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_edit_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "shortcode": shortcode,
            }),
        );
    });
    Ok(())
}

pub(crate) fn set_image_body_content(existing: Value, key: &str, body: &str) -> Result<Value, String> {
    let mut content = existing;
    let entry = content
        .get_mut("images")
        .and_then(|v| v.as_object_mut())
        .and_then(|images| images.get_mut(key))
        .and_then(|v| v.as_object_mut())
        .ok_or_else(|| "not_found".to_owned())?;
    let body = one_line(body, MAX_BODY_CHARS);
    if body.is_empty() {
        entry.remove("body");
    } else {
        entry.insert("body".to_owned(), json!(body));
    }
    Ok(content)
}

pub(crate) fn set_pack_usage_content(existing: Value, usage: &str) -> Result<Value, String> {
    let mut content = existing;
    if !content.is_object() {
        return Err("rejected".to_owned());
    }
    if content.get("pack").and_then(|p| p.as_object()).is_none() {
        content["pack"] = json!({});
    }
    let pack = content["pack"].as_object_mut().ok_or_else(|| "rejected".to_owned())?;
    match usage {
        "emoticon" => {
            pack.insert("usage".to_owned(), json!(["emoticon"]));
        }
        "sticker" => {
            pack.insert("usage".to_owned(), json!(["sticker"]));
        }
        "both" => {
            pack.remove("usage");
        }
        _ => return Err("rejected".to_owned()),
    }
    Ok(content)
}

pub(crate) fn remove_image_from_pack_content(
    existing: Value,
    shortcode: &str,
) -> Result<Value, String> {
    let mut content = existing;
    let images = content
        .get_mut("images")
        .and_then(|v| v.as_object_mut())
        .ok_or_else(|| "not_found".to_owned())?;
    if images.remove(shortcode).is_none() {
        return Err("not_found".to_owned());
    }
    Ok(content)
}

pub(crate) fn rename_image_in_pack_content(
    existing: Value,
    from: &str,
    to: &str,
) -> Result<(Value, String), String> {
    if !is_valid_shortcode(to) {
        return Err("invalid_shortcode".to_owned());
    }
    let code = to.to_owned();
    if code == from {
        return Ok((existing, code));
    }
    let mut content = existing;
    let images = content
        .get_mut("images")
        .and_then(|v| v.as_object_mut())
        .ok_or_else(|| "not_found".to_owned())?;
    if images.contains_key(&code) {
        return Err("shortcode_taken".to_owned());
    }
    let entry = images.remove(from).ok_or_else(|| "not_found".to_owned())?;
    images.insert(code.clone(), entry);
    Ok((content, code))
}

pub(crate) fn set_pack_display_name_content(
    existing: Value,
    name: &str,
) -> Result<Value, String> {
    let trimmed = one_line(name, MAX_BODY_CHARS);
    let mut content = existing;
    if !content.is_object() {
        content = json!({});
    }
    if content.get("pack").and_then(|p| p.as_object()).is_none() {
        content["pack"] = json!({});
    }
    let pack = content["pack"]
        .as_object_mut()
        .ok_or_else(|| "rejected".to_owned())?;
    if trimmed.is_empty() {
        pack.remove("display_name");
    } else {
        pack.insert("display_name".to_owned(), json!(trimmed));
    }
    Ok(content)
}

#[allow(clippy::too_many_arguments)]
async fn add_to_user_pack_inner(
    client: &matrix_sdk::Client,
    requested: String,
    url: String,
    body: String,
    mimetype: String,
    width: u64,
    height: u64,
    size: u64,
    usage: &str,
) -> Result<String, String> {
    let ty = GlobalAccountDataEventType::from(USER_EMOTES);

    let existing = base_for_add(account_data_read(
        client.account().fetch_account_data(ty.clone()).await,
    ))?;

    let (mut content, code) = add_image_to_pack_content(
        existing, requested, url, body, mimetype, width, height, size, usage,
    )?;

    if content.get("pack").and_then(|p| p.as_object()).is_none() {
        content["pack"] = json!({ "display_name": "Stickers" });
    }

    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    let written = client.account().set_account_data_raw(ty, raw).await;
    forget_own_write(&client, USER_EMOTES);
    written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

pub(crate) const PACK_HOME_TYPE: &str = "org.lightning_matrix.image_pack_home";
pub(crate) const PACK_HOME_MARKER: &str = "org.lightning_matrix.image_pack_home";

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Partition {
    Emoji,
    Stickers,
    Both,
}

impl Partition {
    pub(crate) fn state_key(self) -> &'static str {
        match self {
            Partition::Emoji => "emoji",
            Partition::Stickers => "stickers",
            Partition::Both => "emoji-and-stickers",
        }
    }
    fn usage(self) -> &'static str {
        match self {
            Partition::Emoji => "emoticon",
            Partition::Stickers => "sticker",
            Partition::Both => "both",
        }
    }
}

pub(crate) struct MigrationPlan {
    pub packs: Vec<(Partition, Value)>,
    pub invalid_keys: Vec<String>,
}

pub(crate) fn plan_user_pack_migration(legacy: &Value, base_name: &str) -> MigrationPlan {
    let pack_info = legacy.get("pack").and_then(|p| p.as_object());
    let pack_usage = parse_usage(pack_info.and_then(|p| p.get("usage")));
    let mut parts: BTreeMap<Partition, serde_json::Map<String, Value>> = BTreeMap::new();
    let mut invalid_keys = Vec::new();
    if let Some(images) = legacy.get("images").and_then(|v| v.as_object()) {
        for (key, entry) in images {
            let Some(object) = entry.as_object() else { continue };
            if !object.get("url").and_then(|v| v.as_str()).is_some_and(is_valid_mxc) {
                continue;
            }
            if !is_valid_shortcode(key) {
                invalid_keys.push(key.clone());
                continue;
            }
            let (emoticon, sticker) =
                resolve_usage(&parse_usage(object.get("usage")), &pack_usage);
            let partition = match (emoticon, sticker) {
                (true, false) => Partition::Emoji,
                (false, true) => Partition::Stickers,
                _ => Partition::Both,
            };
            let mut copy = object.clone();
            copy.remove("usage");
            parts.entry(partition).or_default().insert(key.clone(), Value::Object(copy));
        }
    }
    let mut packs = Vec::new();
    for (partition, images) in parts {
        let mut content = new_stable_pack_content("", partition.usage());
        let suffix = match partition {
            Partition::Emoji => "emoji",
            Partition::Stickers => "stickers",
            Partition::Both => "emoji and stickers",
        };
        let name = if base_name.trim().is_empty() {
            format!("My {suffix}")
        } else {
            format!("{} ({suffix})", one_line(base_name, MAX_PACK_NAME_CHARS - 24))
        };
        let pack = content
            .as_object_mut()
            .expect("object")
            .entry("pack")
            .or_insert_with(|| json!({}));
        pack["display_name"] = json!(name);
        if let Some(info) = pack_info {
            for field in ["attribution", "avatar_url"] {
                if let Some(v) = info.get(field) {
                    pack[field] = v.clone();
                }
            }
        }
        content["images"] = Value::Object(images);
        packs.push((partition, content));
    }
    MigrationPlan { packs, invalid_keys }
}

pub(crate) fn merge_for_resume(existing: PackRead, planned: Value) -> Result<Value, String> {
    match existing {
        PackRead::Absent => Ok(planned),
        PackRead::Failed(category) => Err(category),
        PackRead::Found(mut current) => {
            if current.get("images").and_then(|v| v.as_object()).is_none() {
                current["images"] = json!({});
            }
            let images = current["images"].as_object_mut().ok_or("rejected")?;
            if let Some(planned_images) = planned.get("images").and_then(|v| v.as_object()) {
                for (key, entry) in planned_images {
                    images.entry(key.clone()).or_insert_with(|| entry.clone());
                }
            }
            if current.get("pack").is_none() {
                if let Some(pack) = planned.get("pack") {
                    current["pack"] = pack.clone();
                }
            }
            Ok(current)
        }
    }
}

async fn marked_home_rooms(client: &matrix_sdk::Client) -> Vec<String> {
    let mut out = Vec::new();
    for room in client.joined_rooms() {
        let Ok(Some(raw)) = room.get_state_event(StateEventType::RoomCreate, "").await else {
            continue;
        };
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        let Ok(value) = serde_json::from_str::<Value>(&json) else { continue };
        if value["content"].get(PACK_HOME_MARKER).is_some_and(|v| v == &json!(true)) {
            out.push(room.room_id().to_string());
        }
    }
    out.sort();
    out
}

#[derive(Debug, PartialEq)]
pub(crate) enum HomeDecision {
    UseJournal,
    UseMarked(String),
    Create,
    Refuse(String),
}

pub(crate) fn plan_pack_home(
    journal: &PackRead,
    journal_joined: bool,
    marked: &[String],
    start_new: bool,
) -> HomeDecision {
    match journal {
        PackRead::Failed(category) => HomeDecision::Refuse(category.clone()),
        PackRead::Found(_) if journal_joined => HomeDecision::UseJournal,
        _ => match marked.first() {
            Some(room) => HomeDecision::UseMarked(room.clone()),
            None if *journal == PackRead::Absent || start_new => HomeDecision::Create,
            None => HomeDecision::Refuse("home_unavailable".to_owned()),
        },
    }
}

async fn ensure_pack_home(
    client: &matrix_sdk::Client,
    start_new: bool,
) -> Result<(String, bool, usize), String> {
    use matrix_sdk::ruma::api::client::room::create_room;
    use matrix_sdk::ruma::api::client::room::Visibility;

    // The journal as the SERVER holds it: a store miss or a remembered "not
    // found" is not proof that no home exists, and a failed read must never
    // lead to creating a second room.
    let journal = account_data_read(
        client
            .account()
            .fetch_account_data(GlobalAccountDataEventType::from(PACK_HOME_TYPE))
            .await,
    );
    let marked = marked_home_rooms(client).await;
    let duplicates = marked.len().saturating_sub(1);
    let journal_room = match &journal {
        PackRead::Found(j) => j.get("room_id").and_then(|v| v.as_str()).map(str::to_owned),
        _ => None,
    };
    let journal_joined = journal_room
        .as_deref()
        .is_some_and(|id| joined_room(client, id).is_ok());
    match plan_pack_home(&journal, journal_joined, &marked, start_new) {
        HomeDecision::UseJournal => {
            return Ok((journal_room.unwrap_or_default(), false, duplicates));
        }
        HomeDecision::UseMarked(room_id) => {
            write_account_data(client, PACK_HOME_TYPE, &json!({ "room_id": room_id, "version": 1 }))
                .await?;
            return Ok((room_id, false, duplicates));
        }
        HomeDecision::Refuse(category) => return Err(category),
        HomeDecision::Create => {}
    }
    let creation: matrix_sdk::ruma::serde::Raw<create_room::v3::CreationContent> =
        matrix_sdk::ruma::serde::Raw::from_json_string(
            json!({ PACK_HOME_MARKER: true }).to_string(),
        )
        .map_err(|_| "rejected".to_owned())?;
    let mut request = create_room::v3::Request::new();
    request.name = Some("My emoji and stickers".to_owned());
    request.topic = Some(
        "Lightning keeps your own emoji and sticker packs here. Pack images are \
         regular Matrix media, visible to the hosting server."
            .to_owned(),
    );
    request.preset = Some(create_room::v3::RoomPreset::PrivateChat);
    request.visibility = Visibility::Private;
    request.creation_content = Some(creation);
    // Once, never retried: a retried create makes a second room.
    let room_id = client
        .send(request)
        .with_request_config(crate::rooms::create_request_config())
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?
        .room_id;
    write_account_data(
        client,
        PACK_HOME_TYPE,
        &json!({ "room_id": room_id.as_str(), "version": 1 }),
    )
    .await?;
    Ok((room_id.to_string(), true, duplicates))
}

pub(crate) fn migrate_user_pack(
    bridge: &RustClient,
    op_id: u64,
    start_new: bool,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut report = json!({
            "type": "sticker_pack_migration",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": false,
            "category": "",
            "room_id": "",
            "created": false,
            "duplicates": 0,
            "packs": 0,
            "invalid": 0,
            "subscribed": false,
        });
        let outcome: Result<(), String> = async {
            let legacy = base_for_edit(account_data_read(
                client
                    .account()
                    .fetch_account_data(GlobalAccountDataEventType::from(USER_EMOTES))
                    .await,
            ))?;
            let name = legacy
                .get("pack")
                .and_then(|p| p.get("display_name"))
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_owned();
            let plan = plan_user_pack_migration(&legacy, &name);
            report["invalid"] = json!(plan.invalid_keys.len());
            if plan.packs.is_empty() {
                return Err("no_images".to_owned());
            }
            let (room_id, created, duplicates) = ensure_pack_home(&client, start_new).await?;
            report["room_id"] = json!(room_id);
            report["created"] = json!(created);
            report["duplicates"] = json!(duplicates);
            let room = joined_room(&client, &room_id).map_err(|_| "unknown_room".to_owned())?;
            let mut written = Vec::new();
            for (partition, content) in plan.packs {
                let key = partition.state_key();
                let existing =
                    read_room_pack_fresh(&client, &room_id, key, Namespace::Stable).await;
                let content = merge_for_resume(existing, content)?;
                let sent = room.send_state_event_raw(STABLE_ROOM_PACK, key, content).await;
                forget_room(&client, &room_id);
                sent.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                written.push(key);
                report["packs"] = json!(written.len());
            }
            for key in &written {
                set_room_pack_enabled_inner(&client, &room_id, key, true, Namespace::Stable)
                    .await?;
            }
            report["subscribed"] = json!(true);
            Ok(())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(()) => report["ok"] = json!(true),
            Err(category) => report["category"] = json!(category),
        }
        enqueue(&events, report);
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Live changes
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.room.image_pack", kind = State, state_key_type = String)]
pub(crate) struct StableImagePackEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.room_emotes", kind = State, state_key_type = String)]
pub(crate) struct LegacyRoomEmotesEventContent {}

// A Space link is removed by writing `{}`, which ruma's typed
// `SpaceParentEventContent`/`SpaceChildEventContent` refuse (`via` is
// required), so handlers on those types never saw a removal and a Space's
// packs stayed offered until a manual reload (live run 2026-10-26). These
// accept any content.
#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.space.parent", kind = State, state_key_type = String)]
pub(crate) struct SpaceParentChangeEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.space.child", kind = State, state_key_type = String)]
pub(crate) struct SpaceChildChangeEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.image_pack.rooms", kind = GlobalAccountData)]
pub(crate) struct StableImagePackRoomsEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.emote_rooms", kind = GlobalAccountData)]
pub(crate) struct LegacyEmoteRoomsEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.user_emotes", kind = GlobalAccountData)]
pub(crate) struct LegacyUserEmotesEventContent {}

pub(crate) fn change_payload(room_id: &str) -> Value {
    json!({ "type": "sticker_packs_changed", "room_id": room_id })
}

pub(crate) fn install_change_handlers(
    client: &matrix_sdk::Client,
    events: crate::EventQueueRef,
) -> Vec<matrix_sdk::event_handler::EventHandlerHandle> {
    let mut handles = Vec::new();
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: SyncStableImagePackEvent, room: matrix_sdk::Room, client: matrix_sdk::Client| {
            let q = Arc::clone(&q);
            async move {
                forget_room(&client, room.room_id().as_str());
                enqueue(&q, change_payload(room.room_id().as_str()));
            }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: SyncLegacyRoomEmotesEvent, room: matrix_sdk::Room, client: matrix_sdk::Client| {
            let q = Arc::clone(&q);
            async move {
                forget_room(&client, room.room_id().as_str());
                enqueue(&q, change_payload(room.room_id().as_str()));
            }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: SyncSpaceParentChangeEvent, room: matrix_sdk::Room| {
            let q = Arc::clone(&q);
            async move { enqueue(&q, change_payload(room.room_id().as_str())) }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: SyncSpaceChildChangeEvent, room: matrix_sdk::Room| {
            let q = Arc::clone(&q);
            async move { enqueue(&q, change_payload(room.room_id().as_str())) }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<StableImagePackRoomsEventContent>| {
            let q = Arc::clone(&q);
            async move { enqueue(&q, change_payload("")) }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<LegacyEmoteRoomsEventContent>| {
            let q = Arc::clone(&q);
            async move { enqueue(&q, change_payload("")) }
        },
    ));
    let q = Arc::clone(&events);
    handles.push(client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<LegacyUserEmotesEventContent>| {
            let q = Arc::clone(&q);
            async move { enqueue(&q, change_payload("")) }
        },
    ));
    handles
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(content: Value) -> Pack {
        parse_pack_content(
            &content,
            "user".to_owned(),
            "user",
            String::new(),
            String::new(),
            "Your pack",
        )
        .expect("object content parses")
    }

    #[test]
    fn a_minimal_pack_image_parses_with_its_defaults() {
        let pack = parse(json!({
            "images": { "cat": { "url": "mxc://example.org/abc" } }
        }));
        assert_eq!(pack.images.len(), 1);
        let image = &pack.images[0];
        assert_eq!(image.shortcode, "cat");
        assert_eq!(image.url, "mxc://example.org/abc");
        assert_eq!(image.body, "cat");
        assert!(image.is_emoticon);
        assert!(image.is_sticker);
    }

    #[test]
    fn a_non_mxc_url_is_dropped_not_carried() {
        let pack = parse(json!({
            "images": {
                "beacon": { "url": "https://tracker.example/pixel.gif" },
                "file":   { "url": "file:///etc/passwd" },
                "data":   { "url": "data:image/gif;base64,AAAA" },
                "bare":   { "url": "mxc://" },
                "noserver": { "url": "mxc:///abc" },
                "good":   { "url": "mxc://example.org/abc" }
            }
        }));
        assert_eq!(pack.images.len(), 1);
        assert_eq!(pack.images[0].shortcode, "good");
    }

    #[test]
    fn a_declared_svg_mimetype_is_refused_and_an_absent_one_is_not() {
        let pack = parse(json!({
            "images": {
                "svg":  { "url": "mxc://example.org/1",
                          "info": { "mimetype": "image/svg+xml" } },
                "svg2": { "url": "mxc://example.org/2",
                          "info": { "mimetype": "IMAGE/SVG+XML" } },
                "html": { "url": "mxc://example.org/3",
                          "info": { "mimetype": "text/html" } },
                "png":  { "url": "mxc://example.org/4",
                          "info": { "mimetype": "image/png" } },
                "unknown": { "url": "mxc://example.org/5" }
            }
        }));
        let codes: Vec<&str> =
            pack.images.iter().map(|i| i.shortcode.as_str()).collect();
        assert_eq!(codes, vec!["png", "unknown"]);
    }

    #[test]
    fn image_usage_overrides_pack_usage_and_empty_means_both() {
        let pack = parse(json!({
            "pack": { "usage": ["emoticon"] },
            "images": {
                "inherits":  { "url": "mxc://example.org/1" },
                "overrides": { "url": "mxc://example.org/2",
                               "usage": ["sticker"] },
                "both":      { "url": "mxc://example.org/3",
                               "usage": ["sticker", "emoticon"] },
                "future":    { "url": "mxc://example.org/4",
                               "usage": ["hologram"] }
            }
        }));
        let by = |code: &str| -> (bool, bool) {
            let i = pack
                .images
                .iter()
                .find(|i| i.shortcode == code)
                .expect("present");
            (i.is_emoticon, i.is_sticker)
        };
        assert_eq!(by("inherits"), (true, false));
        assert_eq!(by("overrides"), (false, true));
        assert_eq!(by("both"), (true, true));
        assert_eq!(by("future"), (true, false));

        let open = parse(json!({
            "images": { "x": { "url": "mxc://example.org/1" } }
        }));
        assert_eq!((open.images[0].is_emoticon, open.images[0].is_sticker),
                   (true, true));
    }

    #[test]
    fn shortcodes_are_repaired_to_the_mscs_own_alphabet() {
        assert_eq!(sanitize_shortcode("cat"), "cat");
        assert_eq!(sanitize_shortcode("  cat  "), "cat");
        assert_eq!(sanitize_shortcode(":wrapped:"), "wrapped");
        assert_eq!(sanitize_shortcode("has space"), "has_space");
        assert_eq!(sanitize_shortcode("ctrl\u{0007}x"), "ctrl_x");
        assert_eq!(sanitize_shortcode("my.cool.cat"), "my_cool_cat");
        assert_eq!(sanitize_shortcode("a...b"), "a_b");
        assert_eq!(sanitize_shortcode("emoji_\u{1F600}_here"), "emoji_here");
        assert_eq!(
            sanitize_shortcode("sticker-$AbCd:example.org"),
            "sticker-_AbCd_example_org"
        );
        assert_eq!(sanitize_shortcode(":::"), "");
        assert_eq!(sanitize_shortcode("---"), "");
        assert_eq!(sanitize_shortcode(""), "");
        assert_eq!(
            sanitize_shortcode(&"a".repeat(500)).len(),
            MAX_SHORTCODE_BYTES
        );
    }

    #[test]
    fn a_malformed_received_key_is_kept_as_its_own_identity() {
        let pack = parse(json!({
            "images": {
                ":::": { "url": "mxc://example.org/5" },
                "go od": { "url": "mxc://example.org/7" },
                "": { "url": "mxc://example.org/8" },
                "good": { "url": "mxc://example.org/6" }
            }
        }));
        assert_eq!(pack.images.len(), 4);
        let by_key = |k: &str| pack.images.iter().find(|i| i.key == k).expect(k);
        assert!(by_key("good").key_valid);
        assert_eq!(by_key("good").shortcode, "good");
        assert!(!by_key(":::").key_valid);
        assert!(!by_key("go od").key_valid);
        assert_eq!(by_key("go od").shortcode, "go od");
        assert!(!by_key("").key_valid);
        let long = parse(json!({ "images": {
            "k".repeat(MAX_RAW_KEY_BYTES + 1): { "url": "mxc://example.org/1" } } }));
        assert!(long.images.is_empty());
    }

    #[test]
    fn stable_valid_keys_survive_exactly_including_case_runs_and_length() {
        let k64 = "a".repeat(64);
        let k65 = "b".repeat(65);
        let k99 = "c".repeat(99);
        let k100 = "d".repeat(100);
        let mut images = serde_json::Map::new();
        for (i, k) in ["hello__world", "hello_world", "Cat", "cat", "_cat_", "-cat-",
                       "x", k64.as_str(), k65.as_str(), k99.as_str(), k100.as_str()]
            .iter()
            .enumerate()
        {
            images.insert((*k).to_owned(), json!({ "url": format!("mxc://e/{i}") }));
        }
        let pack = parse(json!({ "images": images }));
        assert_eq!(pack.images.len(), 11);
        for image in &pack.images {
            assert!(image.key_valid, "{} should be valid", image.key);
            assert_eq!(image.shortcode, image.key, "display must equal identity");
        }
        let k101 = "e".repeat(101);
        let over = parse(json!({ "images": { k101.clone(): { "url": "mxc://e/z" } } }));
        assert_eq!(over.images[0].key, k101);
        assert!(!over.images[0].key_valid);
    }

    #[test]
    fn the_shortcode_grammar_is_exactly_the_stable_one() {
        for ok in ["a", "A", "0", "_", "-", "Cat_Wave", "hello__world", "-x-"] {
            assert!(is_valid_shortcode(ok), "{ok}");
        }
        assert!(is_valid_shortcode(&"z".repeat(100)));
        for bad in ["", "a b", "a:b", "a/b", "caf\u{e9}", "\u{1F600}", "a.b", "a+b"] {
            assert!(!is_valid_shortcode(bad), "{bad:?}");
        }
        assert!(!is_valid_shortcode(&"z".repeat(101)));
        assert_eq!(propose_shortcode("hello__world"), "hello__world");
        assert_eq!(propose_shortcode("Cat"), "Cat");
        assert_eq!(propose_shortcode(" my cat.png "), "my_cat_png");
    }

    #[test]
    fn edits_address_the_exact_key_and_never_its_neighbour() {
        let content = json!({ "images": {
            "hello__world": { "url": "mxc://e/a" },
            "hello_world": { "url": "mxc://e/b" },
            "Cat": { "url": "mxc://e/c" },
            "cat": { "url": "mxc://e/d" },
            "bad key": { "url": "mxc://e/f" },
        } });
        let removed = remove_image_from_pack_content(content.clone(), "hello__world").unwrap();
        let images = removed["images"].as_object().unwrap();
        assert!(!images.contains_key("hello__world"));
        assert_eq!(images["hello_world"]["url"], json!("mxc://e/b"));

        let removed = remove_image_from_pack_content(content.clone(), "Cat").unwrap();
        assert_eq!(removed["images"]["cat"]["url"], json!("mxc://e/d"));
        assert!(removed["images"].get("Cat").is_none());

        let removed = remove_image_from_pack_content(content.clone(), "bad key").unwrap();
        assert!(removed["images"].get("bad key").is_none());
        let (repaired, code) =
            rename_image_in_pack_content(content.clone(), "bad key", "good_key").unwrap();
        assert_eq!(code, "good_key");
        assert_eq!(repaired["images"]["good_key"]["url"], json!("mxc://e/f"));
        assert_eq!(repaired["images"].as_object().unwrap().len(), 5);

        assert_eq!(
            rename_image_in_pack_content(content.clone(), "Cat", "cat").unwrap_err(),
            "shortcode_taken"
        );
        assert!(rename_image_in_pack_content(content.clone(), "Cat", "CAT").is_ok());
        for bad in ["has space", "a:b", "", &"q".repeat(101)] {
            assert_eq!(
                rename_image_in_pack_content(content.clone(), "Cat", bad).unwrap_err(),
                "invalid_shortcode",
                "{bad:?}"
            );
        }
        let long = "L".repeat(100);
        let (_, code) = rename_image_in_pack_content(content, "Cat", &long).unwrap();
        assert_eq!(code, long);
    }

    #[test]
    fn a_suffixed_key_stays_inside_the_grammar_and_is_case_exact() {
        let base = json!({ "images": { "Cat": { "url": "mxc://e/1" } } });
        let (_, code) = add(base.clone(), "cat", "mxc://e/2").unwrap();
        assert_eq!(code, "cat");
        let (_, code) = add(base, "Cat", "mxc://e/2").unwrap();
        assert_eq!(code, "Cat-2");
        let long = "m".repeat(100);
        let full = json!({ "images": { long.clone(): { "url": "mxc://e/1" } } });
        let (_, code) = add(full, &long, "mxc://e/2").unwrap();
        assert!(is_valid_shortcode(&code), "{code}");
        assert!(code.ends_with("-2"));
    }

    #[test]
    fn a_failed_read_never_becomes_an_empty_pack_to_write() {
        assert_eq!(
            classify_pack_read(Err("[404 / M_NOT_FOUND] Event not found".to_owned())),
            PackRead::Absent
        );
        for (message, class) in [
            ("[403 / M_FORBIDDEN] You are not in the room", "forbidden"),
            ("operation timed out", "network"),
            ("error sending request: connection refused", "network"),
            ("[429 / M_LIMIT_EXCEEDED] slow down", "rate_limited"),
        ] {
            let read = classify_pack_read(Err(message.to_owned()));
            assert_eq!(read, PackRead::Failed(class.to_owned()), "{message}");
            assert_eq!(base_for_add(read.clone()).unwrap_err(), class);
            assert_eq!(base_for_edit(read).unwrap_err(), class);
        }
        for body in ["not json", "[]", "\"x\""] {
            let read = classify_pack_read(Ok(body));
            assert_eq!(read, PackRead::Failed("malformed".to_owned()), "{body}");
            assert!(base_for_add(read).is_err());
        }
        assert_eq!(base_for_add(PackRead::Absent).unwrap(), json!({ "images": {} }));
        assert_eq!(base_for_edit(PackRead::Absent).unwrap_err(), "not_found");
        let found = json!({ "images": { "a": { "url": "mxc://e/1" } }, "x-other": 1 });
        assert_eq!(base_for_add(classify_pack_read(Ok(&found.to_string()))).unwrap(), found);
    }

    #[test]
    fn a_body_is_collapsed_to_one_line() {
        let pack = parse(json!({
            "images": { "body": { "url": "mxc://example.org/6",
                                  "body": "line one\nline two\u{0007}   spaced" } }
        }));
        assert_eq!(pack.images[0].body, "line one line two spaced");
    }

    #[test]
    fn a_body_longer_than_the_bound_is_truncated() {
        let pack = parse(json!({
            "images": { "x": { "url": "mxc://example.org/1",
                               "body": "b".repeat(1000) } }
        }));
        assert_eq!(pack.images[0].body.chars().count(), MAX_BODY_CHARS);
    }

    #[test]
    fn a_pack_is_capped_and_the_cap_is_not_a_crash() {
        let mut images = serde_json::Map::new();
        for n in 0..(MAX_IMAGES_PER_PACK + 50) {
            images.insert(
                format!("code{n:04}"),
                json!({ "url": format!("mxc://example.org/{n}") }),
            );
        }
        let pack = parse(json!({ "images": Value::Object(images) }));
        assert_eq!(pack.images.len(), MAX_IMAGES_PER_PACK);
    }

    #[test]
    fn pack_info_is_sanitized_and_a_non_mxc_avatar_is_dropped() {
        let pack = parse(json!({
            "pack": {
                "display_name": "My\nPack\u{0007}",
                "avatar_url": "https://tracker.example/a.png",
                "attribution": "by\tsomeone"
            },
            "images": {}
        }));
        assert_eq!(pack.display_name, "My Pack");
        assert_eq!(pack.avatar_url, "");
        assert_eq!(pack.attribution, "by someone");

        let good = parse(json!({
            "pack": { "avatar_url": "mxc://example.org/a" },
            "images": {}
        }));
        assert_eq!(good.avatar_url, "mxc://example.org/a");
    }

    #[test]
    fn a_pack_with_no_name_falls_back_to_the_supplied_default() {
        let pack = parse_pack_content(
            &json!({ "images": {} }),
            "room:!r:example.org:".to_owned(),
            "room",
            "!r:example.org".to_owned(),
            String::new(),
            "Cat Lovers",
        )
        .expect("parses");
        assert_eq!(pack.display_name, "Cat Lovers");
    }

    #[test]
    fn non_object_content_is_not_a_pack() {
        assert!(parse_pack_content(
            &json!("nope"),
            "user".to_owned(),
            "user",
            String::new(),
            String::new(),
            "Your pack"
        )
        .is_none());
    }

    #[test]
    fn an_empty_pack_is_a_pack_not_a_parse_failure() {
        let pack = parse(json!({ "images": {} }));
        assert!(pack.images.is_empty());
    }

    #[test]
    fn emote_rooms_drops_invalid_room_ids() {
        let enabled = parse_emote_rooms(&json!({
            "rooms": {
                "!good:example.org": { "": {}, "packtwo": {} },
                "not a room id": { "": {} },
                "@user:example.org": { "": {} }
            }
        }));
        assert_eq!(enabled.len(), 2);
        assert!(enabled
            .iter()
            .all(|(room, _)| room == "!good:example.org"));
        let keys: BTreeSet<&str> =
            enabled.iter().map(|(_, k)| k.as_str()).collect();
        assert!(keys.contains(""));
        assert!(keys.contains("packtwo"));
    }

    #[test]
    fn emote_rooms_tolerates_a_missing_or_malformed_map() {
        assert!(parse_emote_rooms(&json!({})).is_empty());
        assert!(parse_emote_rooms(&json!({ "rooms": "nope" })).is_empty());
        assert!(parse_emote_rooms(&json!({ "rooms": { "!r:e.org": 3 } })).is_empty());
    }

    fn add(existing: Value, code: &str, url: &str) -> Result<(Value, String), String> {
        add_image_to_pack_content(
            existing,
            sanitize_shortcode(code),
            url.to_owned(),
            code.to_owned(),
            "image/png".to_owned(),
            128,
            96,
            4096,
            "sticker",
        )
    }

    fn add_as(existing: Value, code: &str, url: &str, usage: &str) -> Value {
        add_image_to_pack_content(
            existing,
            sanitize_shortcode(code),
            url.to_owned(),
            code.to_owned(),
            "image/png".to_owned(),
            0,
            0,
            0,
            usage,
        )
        .expect("adds")
        .0
    }

    #[test]
    fn the_requested_usage_is_what_gets_written() {
        let emoji = add_as(json!({}), "cat", "mxc://example.org/a", "emoticon");
        assert_eq!(emoji["images"]["cat"]["usage"], json!(["emoticon"]));
        let both = add_as(json!({}), "cat", "mxc://example.org/a", "both");
        assert_eq!(both["images"]["cat"]["usage"], json!(["sticker", "emoticon"]));
        let sticker = add_as(json!({}), "cat", "mxc://example.org/a", "sticker");
        assert_eq!(sticker["images"]["cat"]["usage"], json!(["sticker"]));
        let junk = add_as(json!({}), "cat", "mxc://example.org/a", "emoji!");
        assert_eq!(junk["images"]["cat"]["usage"], json!(["sticker"]));
        let pack = parse_pack_content(
            &emoji, "user".to_owned(), "user", String::new(), String::new(), "x")
            .expect("pack");
        assert!(pack.images[0].is_emoticon && !pack.images[0].is_sticker);
    }

    #[test]
    fn adding_an_image_writes_the_mscs_own_shape() {
        let (content, code) =
            add(json!({}), "cat", "mxc://example.org/a").expect("adds");
        assert_eq!(code, "cat");
        let entry = &content["images"]["cat"];
        assert_eq!(entry["url"], json!("mxc://example.org/a"));
        assert_eq!(entry["usage"], json!(["sticker"]));
        assert_eq!(entry["info"]["mimetype"], json!("image/png"));
        assert_eq!(entry["info"]["w"], json!(128));
        assert!(entry.get("body").is_none());
    }

    #[test]
    fn the_same_mxc_twice_is_a_duplicate_by_identity_not_by_name() {
        let (content, _) =
            add(json!({}), "cat", "mxc://example.org/a").expect("adds");
        assert_eq!(
            add(content.clone(), "kitty", "mxc://example.org/a"),
            Err("duplicate".to_owned())
        );
        let (after, code) =
            add(content, "cat", "mxc://example.org/b").expect("adds");
        assert_eq!(code, "cat-2");
        assert_eq!(after["images"]["cat"]["url"], json!("mxc://example.org/a"));
        assert_eq!(after["images"]["cat-2"]["url"], json!("mxc://example.org/b"));
    }

    #[test]
    fn an_unnamed_image_still_gets_a_usable_shortcode() {
        let (content, code) =
            add(json!({}), ":::", "mxc://example.org/a").expect("adds");
        assert_eq!(code, "sticker");
        assert!(content["images"].get("sticker").is_some());
    }

    #[test]
    fn a_full_pack_refuses_rather_than_evicting() {
        let mut images = serde_json::Map::new();
        for n in 0..MAX_IMAGES_PER_PACK {
            images.insert(
                format!("c{n}"),
                json!({ "url": format!("mxc://example.org/{n}") }),
            );
        }
        let full = json!({ "images": Value::Object(images) });
        assert_eq!(
            add(full, "new", "mxc://example.org/new"),
            Err("pack_full".to_owned())
        );
    }

    #[test]
    fn a_malformed_pack_content_is_replaced_rather_than_crashing() {
        for junk in [json!("nope"), json!(3), json!({ "images": "nope" })] {
            let (content, code) =
                add(junk, "cat", "mxc://example.org/a").expect("adds");
            assert_eq!(code, "cat");
            assert!(content["images"]["cat"]["url"].is_string());
        }
    }

    #[test]
    fn adding_preserves_every_image_already_in_the_pack() {
        let before = json!({
            "pack": { "display_name": "Theirs" },
            "images": {
                "one": { "url": "mxc://example.org/1" },
                "two": { "url": "mxc://example.org/2", "usage": ["emoticon"] }
            }
        });
        let (after, _) =
            add(before, "three", "mxc://example.org/3").expect("adds");
        assert_eq!(after["images"].as_object().unwrap().len(), 3);
        assert_eq!(after["images"]["two"]["usage"], json!(["emoticon"]));
        assert_eq!(after["pack"]["display_name"], json!("Theirs"));
    }

    #[test]
    fn enabling_a_room_pack_preserves_every_other_selection() {
        let before = json!({
            "rooms": {
                "!a:example.org": { "": {}, "second": {} },
                "!b:example.org": { "": {} }
            }
        });
        let after = apply_emote_rooms_change(
            before, "!c:example.org", "packone", true,
        )
        .expect("applies");
        let rooms = after["rooms"].as_object().expect("object");
        assert_eq!(rooms.len(), 3);
        assert_eq!(rooms["!a:example.org"].as_object().unwrap().len(), 2);
        assert!(rooms["!c:example.org"]
            .as_object()
            .unwrap()
            .contains_key("packone"));
        assert_eq!(rooms["!c:example.org"]["packone"], json!({}));
    }

    #[test]
    fn disabling_the_last_pack_removes_the_room_rather_than_leaving_a_husk() {
        let before = json!({
            "rooms": {
                "!a:example.org": { "": {}, "second": {} },
                "!b:example.org": { "": {} }
            }
        });
        let after =
            apply_emote_rooms_change(before, "!a:example.org", "second", false)
                .expect("applies");
        assert_eq!(after["rooms"]["!a:example.org"], json!({ "": {} }));

        let empty = apply_emote_rooms_change(
            after, "!a:example.org", "", false,
        )
        .expect("applies");
        assert!(!empty["rooms"]
            .as_object()
            .unwrap()
            .contains_key("!a:example.org"));
        assert!(empty["rooms"]
            .as_object()
            .unwrap()
            .contains_key("!b:example.org"));
    }

    #[test]
    fn disabling_something_that_was_never_enabled_is_not_an_error() {
        let after = apply_emote_rooms_change(
            json!({}), "!a:example.org", "nope", false,
        )
        .expect("applies");
        assert_eq!(after["rooms"], json!({}));
    }

    #[test]
    fn a_malformed_blob_is_replaced_rather_than_making_the_control_dead() {
        for junk in [json!("nope"), json!(7), json!([1, 2]),
                     json!({ "rooms": "nope" })] {
            let after =
                apply_emote_rooms_change(junk, "!a:example.org", "", true)
                    .expect("applies");
            assert_eq!(after["rooms"]["!a:example.org"], json!({ "": {} }));
        }
    }

    #[test]
    fn the_room_count_is_bounded_but_an_existing_room_can_still_be_extended() {
        let mut rooms = serde_json::Map::new();
        for n in 0..MAX_PACKS {
            rooms.insert(format!("!r{n}:example.org"), json!({ "": {} }));
        }
        let full = json!({ "rooms": Value::Object(rooms) });
        assert_eq!(
            apply_emote_rooms_change(
                full.clone(), "!new:example.org", "", true
            ),
            Err("too_many_rooms".to_owned())
        );
        let after =
            apply_emote_rooms_change(full, "!r0:example.org", "second", true)
                .expect("applies");
        assert_eq!(
            after["rooms"]["!r0:example.org"].as_object().unwrap().len(),
            2
        );
    }

    #[test]
    fn an_unmeasured_sticker_omits_its_dimensions_rather_than_claiming_zero() {
        let info = sticker_image_info("image/webp".to_owned(), 0, 0, 43008);
        let wire = serde_json::to_value(&info).expect("info serializes");
        assert!(
            wire.get("w").is_none(),
            "an unmeasured sticker asserts a width: {wire}"
        );
        assert!(
            wire.get("h").is_none(),
            "an unmeasured sticker asserts a height: {wire}"
        );
        assert_eq!(wire.get("size").and_then(Value::as_u64), Some(43008));
        assert_eq!(
            wire.get("mimetype").and_then(Value::as_str),
            Some("image/webp")
        );
    }

    #[test]
    fn a_measured_sticker_still_carries_what_it_measured() {
        let info = sticker_image_info("image/png".to_owned(), 512, 384, 900);
        let wire = serde_json::to_value(&info).expect("info serializes");
        assert_eq!(wire.get("w").and_then(Value::as_u64), Some(512));
        assert_eq!(wire.get("h").and_then(Value::as_u64), Some(384));
    }

    #[test]
    fn an_empty_mimetype_is_omitted_not_written_as_an_empty_string() {
        let info = sticker_image_info(String::new(), 0, 0, 0);
        let wire = serde_json::to_value(&info).expect("info serializes");
        assert!(
            wire.get("mimetype").is_none(),
            "an empty mimetype reached the wire: {wire}"
        );
        assert!(wire.get("size").is_none(), "a zero size reached the wire: {wire}");
    }

    #[test]
    fn is_valid_mxc_accepts_only_a_real_media_uri() {
        assert!(is_valid_mxc("mxc://example.org/abc"));
        assert!(!is_valid_mxc("mxc://example.org"));
        assert!(!is_valid_mxc("mxc://"));
        assert!(!is_valid_mxc("mxc:///abc"));
        assert!(!is_valid_mxc("https://example.org/a.png"));
        assert!(!is_valid_mxc(""));
        assert!(!is_valid_mxc("  mxc://example.org/abc"));
        for bad in ["mxc://e.org/a/b", "mxc://e.org/a?x=1", "mxc://e.org/a#f",
                    "mxc://e.org/..", "mxc://e.org/a b", "mxc://e.org/"] {
            assert!(!is_valid_mxc(bad), "{bad}");
        }
        assert!(is_valid_mxc("mxc://e.org:8448/Ab_c-9"));
    }

    #[test]
    fn allowed_mimetypes_match_the_byte_sniffers_outputs() {
        for m in ALLOWED_MIMETYPES {
            assert!(mimetype_allowed(Some(m)), "{m} should be allowed");
        }
        assert!(!mimetype_allowed(Some("image/svg+xml")));
        assert!(!mimetype_allowed(Some("image/avif")));
        assert!(!mimetype_allowed(Some("text/html")));
        assert!(!mimetype_allowed(Some("")));
        assert!(mimetype_allowed(None));
    }

    #[test]
    fn removing_an_image_takes_only_that_one() {
        let content = json!({
            "pack": { "display_name": "Blobs" },
            "images": {
                "blob": { "url": "mxc://example.org/a" },
                "wave": { "url": "mxc://example.org/b" },
            }
        });
        let out = remove_image_from_pack_content(content, "blob").unwrap();
        let images = out["images"].as_object().unwrap();
        assert!(!images.contains_key("blob"));
        assert!(images.contains_key("wave"));
        assert_eq!(out["pack"]["display_name"], json!("Blobs"));
    }

    #[test]
    fn removing_something_absent_is_reported_not_swallowed() {
        let content = json!({ "images": { "blob": { "url": "mxc://e/a" } } });
        assert_eq!(
            remove_image_from_pack_content(content, "nope").unwrap_err(),
            "not_found"
        );
        assert_eq!(
            remove_image_from_pack_content(json!({}), "blob").unwrap_err(),
            "not_found"
        );
    }

    #[test]
    fn removing_the_last_image_leaves_an_empty_pack_that_keeps_its_name() {
        let content = json!({
            "pack": { "display_name": "Blobs" },
            "images": { "blob": { "url": "mxc://example.org/a" } }
        });
        let out = remove_image_from_pack_content(content, "blob").unwrap();
        assert!(out["images"].as_object().unwrap().is_empty());
        assert_eq!(out["pack"]["display_name"], json!("Blobs"));
    }

    #[test]
    fn renaming_keeps_the_entry_and_refuses_a_taken_name() {
        let content = json!({
            "images": {
                "blob": { "url": "mxc://example.org/a", "usage": ["emoticon"] },
                "wave": { "url": "mxc://example.org/b" },
            }
        });
        let (out, code) =
            rename_image_in_pack_content(content.clone(), "blob", "blobcat")
                .unwrap();
        assert_eq!(code, "blobcat");
        let images = out["images"].as_object().unwrap();
        assert!(!images.contains_key("blob"));
        assert_eq!(images["blobcat"]["url"], json!("mxc://example.org/a"));
        assert_eq!(images["blobcat"]["usage"], json!(["emoticon"]));

        assert_eq!(
            rename_image_in_pack_content(content.clone(), "blob", "wave")
                .unwrap_err(),
            "shortcode_taken"
        );
        assert_eq!(
            rename_image_in_pack_content(content.clone(), "ghost", "x")
                .unwrap_err(),
            "not_found"
        );
        assert_eq!(
            rename_image_in_pack_content(content, "blob", "   ").unwrap_err(),
            "invalid_shortcode"
        );
    }

    #[test]
    fn renaming_to_the_same_name_succeeds_without_losing_the_image() {
        let content = json!({ "images": { "blob": { "url": "mxc://e/a" } } });
        let (out, code) =
            rename_image_in_pack_content(content, "blob", "blob").unwrap();
        assert_eq!(code, "blob");
        assert_eq!(out["images"]["blob"]["url"], json!("mxc://e/a"));
    }

    #[test]
    fn an_empty_pack_name_removes_the_field_rather_than_storing_blank() {
        let named =
            set_pack_display_name_content(json!({}), "  Blobs  ").unwrap();
        assert_eq!(named["pack"]["display_name"], json!("Blobs"));
        let cleared = set_pack_display_name_content(named, "").unwrap();
        assert!(cleared["pack"].as_object().unwrap().get("display_name")
                    .is_none());
        let with_images = json!({
            "pack": { "display_name": "Old" },
            "images": { "blob": { "url": "mxc://e/a" } }
        });
        let out = set_pack_display_name_content(with_images, "New").unwrap();
        assert_eq!(out["pack"]["display_name"], json!("New"));
        assert_eq!(out["images"]["blob"]["url"], json!("mxc://e/a"));
    }

    #[test]
    fn deleting_a_pack_leaves_nothing_behind_including_its_name() {
        let content = json!({
            "pack": { "display_name": "Blobs" },
            "images": { "blob": { "url": "mxc://e/a" } }
        });
        let (out, code) = PackEdit::DeletePack.apply(content).unwrap();
        assert_eq!(code, "");
        assert_eq!(out, json!({}));
    }

    #[test]
    fn every_edit_dispatches_to_its_own_transform() {
        let base = json!({
            "pack": { "display_name": "Blobs" },
            "images": {
                "blob": { "url": "mxc://e/a" },
                "wave": { "url": "mxc://e/b" },
            }
        });
        let (removed, _) = PackEdit::RemoveImage { key: "blob".into() }
            .apply(base.clone())
            .unwrap();
        assert_eq!(removed["images"].as_object().unwrap().len(), 1);

        let (renamed, code) = PackEdit::RenameImage {
            from: "blob".into(),
            to: "blobcat".into(),
        }
        .apply(base.clone())
        .unwrap();
        assert_eq!(code, "blobcat");
        assert_eq!(renamed["images"].as_object().unwrap().len(), 2);

        let (named, _) = PackEdit::SetName { name: "Other".into() }
            .apply(base.clone())
            .unwrap();
        assert_eq!(named["pack"]["display_name"], json!("Other"));
        assert_eq!(named["images"].as_object().unwrap().len(), 2);
    }

    fn parse_stable(content: Value) -> Pack {
        parse_pack_content_ns(&content, "id".into(), "room", "!r:e".into(), "k".into(),
                              "Room", Namespace::Stable)
            .expect("object")
    }

    #[test]
    fn stable_usage_is_pack_wide_and_unknown_only_is_not_everything() {
        let both = parse_stable(json!({ "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((both.images[0].is_emoticon, both.images[0].is_sticker), (true, true));
        let empty = parse_stable(json!({ "pack": { "usage": [] },
                                         "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((empty.images[0].is_emoticon, empty.images[0].is_sticker), (true, true));
        let emo = parse_stable(json!({ "pack": { "usage": ["emoticon"] },
            "images": { "a": { "url": "mxc://e/1", "usage": ["sticker"] } } }));
        assert_eq!((emo.images[0].is_emoticon, emo.images[0].is_sticker), (true, false));
        let unknown = parse_stable(json!({ "pack": { "usage": ["hologram"] },
            "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((unknown.images[0].is_emoticon, unknown.images[0].is_sticker),
                   (false, false));
        assert!(unknown.usage_unsupported);
        let mixed = parse_stable(json!({ "pack": { "usage": ["hologram", "sticker"] },
            "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((mixed.images[0].is_emoticon, mixed.images[0].is_sticker),
                   (false, true));
        assert!(!mixed.usage_unsupported);
        let legacy = parse(json!({ "pack": { "usage": ["emoticon"] },
            "images": { "a": { "url": "mxc://e/1", "usage": ["sticker"] } } }));
        assert_eq!((legacy.images[0].is_emoticon, legacy.images[0].is_sticker),
                   (false, true));
    }

    #[test]
    fn the_pack_level_usage_says_where_a_new_image_would_be_usable() {
        let emo = parse_stable(json!({ "pack": { "usage": ["emoticon"] }, "images": {} }));
        assert_eq!((emo.pack_emoticon, emo.pack_sticker), (true, false));
        let open = parse_stable(json!({ "images": {} }));
        assert_eq!((open.pack_emoticon, open.pack_sticker), (true, true));
        let legacy = parse(json!({ "pack": { "usage": ["sticker"] }, "images": {} }));
        assert_eq!((legacy.pack_emoticon, legacy.pack_sticker), (false, true));
    }

    #[test]
    fn room_pack_ids_are_unambiguous_across_colons_and_dialects() {
        let a = room_pack_id(Namespace::Stable, "!a:b", "c:d");
        let b = room_pack_id(Namespace::Stable, "!a:b:c", "d");
        assert_ne!(a, b);
        assert_ne!(room_pack_id(Namespace::Stable, "!a:b", ""),
                   room_pack_id(Namespace::Legacy, "!a:b", ""));
    }

    fn rec(ns: Namespace, key: &str, content: Value, ts: u64) -> PackRecord {
        PackRecord { namespace: ns, state_key: key.into(), content, ts }
    }

    #[test]
    fn a_present_stable_record_wins_and_a_cleared_one_does_not_resurrect_legacy() {
        let img = |u: &str| json!({ "images": { "x": { "url": u } } });
        let out = resolve_room_packs(vec![
            rec(Namespace::Legacy, "twin", img("mxc://e/legacy"), 5),
            rec(Namespace::Stable, "twin", img("mxc://e/stable"), 1),
            rec(Namespace::Stable, "cleared", json!({}), 9),
            rec(Namespace::Legacy, "cleared", img("mxc://e/stale"), 1),
            rec(Namespace::Stable, "", json!({ "images": {} }), 1),
            rec(Namespace::Legacy, "old", img("mxc://e/old"), 1),
            rec(Namespace::Legacy, "retired", json!({}), 1),
        ]);
        let keys: Vec<(&str, Namespace)> =
            out.iter().map(|r| (r.state_key.as_str(), r.namespace)).collect();
        assert_eq!(keys, vec![("", Namespace::Stable), ("old", Namespace::Legacy),
                              ("twin", Namespace::Stable)]);
        let twin = out.iter().find(|r| r.state_key == "twin").unwrap();
        assert!(twin.legacy_twin);
        assert_eq!(twin.content["images"]["x"]["url"], json!("mxc://e/stable"));
        let out = resolve_room_packs(vec![
            rec(Namespace::Stable, "k", img("mxc://e/new"), 10),
            rec(Namespace::Stable, "k", img("mxc://e/old"), 3),
        ]);
        assert_eq!(out[0].content["images"]["x"]["url"], json!("mxc://e/new"));
    }

    #[test]
    fn a_present_stable_subscription_document_is_authoritative_even_empty() {
        let legacy = json!({ "rooms": { "!a:e": { "x": {} } } });
        let empty = effective_subscriptions(&PackRead::Found(json!({ "rooms": {} })),
                                            Some(&legacy));
        assert!(empty.active.is_empty());
        assert_eq!(empty.source, "stable");
        assert_eq!(empty.legacy_only, vec![("!a:e".to_owned(), "x".to_owned())]);
        let absent = effective_subscriptions(&PackRead::Absent, Some(&legacy));
        assert_eq!(absent.active, vec![("!a:e".to_owned(), "x".to_owned())]);
        assert_eq!(absent.source, "legacy");
        assert!(!absent.stable_unreadable);
        let failed = effective_subscriptions(&PackRead::Failed("network".into()),
                                             Some(&legacy));
        assert!(failed.stable_unreadable);
        let none = effective_subscriptions(
            &PackRead::Found(json!({ "rooms": { "!a:e": {} } })), None);
        assert!(none.active.is_empty());
    }

    #[test]
    fn the_first_stable_document_carries_legacy_choices_and_leaves_leaves_alone() {
        let legacy = json!({ "rooms": { "!old:e": { "a": { "future": 1 } } } });
        let first = plan_stable_subscription_write(PackRead::Absent, Some(&legacy),
                                                   "!new:e", "k", true).unwrap();
        assert_eq!(first["rooms"]["!old:e"]["a"]["future"], json!(1));
        assert_eq!(first["rooms"]["!new:e"]["k"], json!({}));
        assert_eq!(first["rooms"].as_object().unwrap().len(), 2);
        let current = json!({ "rooms": { "!a:e": { "k": { "keep": true }, "j": {} } },
                              "x-top": "kept" });
        let out = plan_stable_subscription_write(PackRead::Found(current.clone()), None,
                                                 "!a:e", "k", true).unwrap();
        assert_eq!(out["rooms"]["!a:e"]["k"]["keep"], json!(true));
        assert_eq!(out["x-top"], json!("kept"));
        let off = plan_stable_subscription_write(PackRead::Found(current), None,
                                                 "!a:e", "j", false).unwrap();
        assert_eq!(off["rooms"]["!a:e"]["k"]["keep"], json!(true));
        assert!(off["rooms"]["!a:e"].get("j").is_none());
        assert_eq!(plan_stable_subscription_write(PackRead::Failed("network".into()),
                                                  None, "!a:e", "k", true).unwrap_err(),
                   "network");
    }

    #[test]
    fn creating_needs_a_confirmed_absence_and_writes_valid_stable_content() {
        let made = plan_create(PackRead::Absent, "Lightning animals", "emoticon").unwrap();
        assert_eq!(made, json!({ "images": {}, "pack": {
            "display_name": "Lightning animals", "usage": ["emoticon"] } }));
        assert_eq!(plan_create(PackRead::Absent, "", "both").unwrap(), json!({ "images": {} }));
        assert_eq!(plan_create(PackRead::Found(json!({})), "x", "both").unwrap_err(),
                   "pack_exists");
        assert_eq!(plan_create(PackRead::Failed("forbidden".into()), "x", "both").unwrap_err(),
                   "forbidden");
    }

    #[test]
    fn retiring_a_stable_pack_writes_valid_empty_content_and_legacy_keeps_its_convention() {
        let base = json!({ "pack": { "display_name": "P" },
                           "images": { "a": { "url": "mxc://e/1" } } });
        let (stable, _) = PackEdit::DeletePack.apply_in(base.clone(), Namespace::Stable).unwrap();
        assert_eq!(stable, json!({ "images": {}, "pack": { "display_name": "P" } }));
        let (legacy, _) = PackEdit::DeletePack.apply_in(base.clone(), Namespace::Legacy).unwrap();
        assert_eq!(legacy, json!({}));
        let (used, _) = PackEdit::SetUsage { usage: "sticker".into() }
            .apply_in(base.clone(), Namespace::Stable).unwrap();
        assert_eq!(used["pack"]["usage"], json!(["sticker"]));
        assert_eq!(used["images"], base["images"]);
        let (open, _) = PackEdit::SetUsage { usage: "both".into() }
            .apply_in(used, Namespace::Stable).unwrap();
        assert!(open["pack"].get("usage").is_none());
        assert!(PackEdit::SetUsage { usage: "both".into() }
            .apply_in(base, Namespace::Legacy).is_err());
    }

    #[test]
    fn a_description_edit_touches_only_that_images_body() {
        let base = json!({ "images": {
            "a": { "url": "mxc://e/1", "body": "old", "info": { "w": 2 }, "x": 1 },
            "b": { "url": "mxc://e/2", "body": "keep" } } });
        let (out, _) = PackEdit::SetBody { key: "a".into(), body: "A new\nline".into() }
            .apply_in(base.clone(), Namespace::Stable).unwrap();
        assert_eq!(out["images"]["a"]["body"], json!("A new line"));
        assert_eq!(out["images"]["a"]["info"]["w"], json!(2));
        assert_eq!(out["images"]["a"]["x"], json!(1));
        assert_eq!(out["images"]["b"]["body"], json!("keep"));
        let (cleared, _) = PackEdit::SetBody { key: "a".into(), body: " ".into() }
            .apply_in(base.clone(), Namespace::Legacy).unwrap();
        assert!(cleared["images"]["a"].get("body").is_none());
        assert_eq!(PackEdit::SetBody { key: "zz".into(), body: "x".into() }
            .apply_in(base, Namespace::Stable).unwrap_err(), "not_found");
    }

    #[test]
    fn a_stable_add_writes_no_per_image_usage() {
        let (content, _) = add_image_to_pack_content(json!({ "images": {} }), "cat".into(),
            "mxc://e/1".into(), "A cat".into(), String::new(), 0, 0, 0, "").unwrap();
        assert!(content["images"]["cat"].get("usage").is_none());
        assert_eq!(content["images"]["cat"]["body"], json!("A cat"));
    }

    #[test]
    fn unknown_fields_survive_every_edit() {
        let base = json!({ "x-pack-meta": { "a": 1 },
            "pack": { "display_name": "P", "x-extra": true },
            "images": { "a": { "url": "mxc://e/1", "x-img": "kept" },
                        "b": { "url": "mxc://e/2" } } });
        let (renamed, _) = PackEdit::RenameImage { from: "a".into(), to: "c".into() }
            .apply_in(base.clone(), Namespace::Stable).unwrap();
        assert_eq!(renamed["images"]["c"]["x-img"], json!("kept"));
        assert_eq!(renamed["x-pack-meta"]["a"], json!(1));
        let (named, _) = PackEdit::SetName { name: "Q".into() }
            .apply_in(base, Namespace::Stable).unwrap();
        assert_eq!(named["pack"]["x-extra"], json!(true));
    }

    #[test]
    fn only_a_valid_canonical_claim_counts_and_the_lowest_id_wins() {
        let claim = |id: &str, canonical, via, reciprocal, auth| ParentClaim {
            space_id: id.into(), canonical, has_via: via, reciprocal, sender_authorised: auth,
        };
        assert_eq!(pick_canonical_parent(&[claim("!evil:e", true, true, false, false)]), None);
        assert_eq!(pick_canonical_parent(&[claim("!a:e", false, true, true, true),
                                           claim("!b:e", true, false, true, true)]), None);
        assert_eq!(pick_canonical_parent(&[claim("!a:e", true, true, true, false)]),
                   Some("!a:e".into()));
        assert_eq!(pick_canonical_parent(&[claim("!a:e", true, true, false, true)]),
                   Some("!a:e".into()));
        assert_eq!(pick_canonical_parent(&[claim("!b:e", true, true, true, false),
                                           claim("!B:e", true, true, true, false),
                                           claim("!a:e", true, true, false, true)]),
                   Some("!B:e".into()));
    }

    #[test]
    fn canonical_ancestry_is_bounded_cycle_safe_and_membership_scoped() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;

        let child = room_id!("!child:e");
        let a = room_id!("!a:e");
        let b = room_id!("!b:e");
        let evil = room_id!("!evil:e");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            let me = client.user_id().unwrap().to_string();
            let ev = |ty: &str, key: &str, sender: &str, id: &str, content: Value| {
                Raw::from_json_string(json!({
                    "type": ty, "state_key": key, "sender": sender, "event_id": id,
                    "origin_server_ts": 1_700_000_000_000u64, "content": content,
                }).to_string()).unwrap()
            };
            let via = json!({ "via": ["e"], "canonical": true });
            let child_of = json!({ "via": ["e"] });
            let mut response = v5::Response::new("p1".to_owned());
            let mut room = |id: &matrix_sdk::ruma::RoomId, state: Vec<Raw<_>>| {
                let mut entry = v5::response::Room::default();
                entry.required_state = state;
                response.rooms.insert(id.to_owned(), entry);
            };
            room(child, vec![
                ev("m.space.parent", a.as_str(), &me, "$1", via.clone()),
                ev("m.space.parent", "!0evil:e", "@stranger:e", "$2", via.clone()),
            ]);
            room(a, vec![
                ev("m.space.child", child.as_str(), &me, "$3", child_of.clone()),
                ev("m.space.child", b.as_str(), &me, "$7", child_of.clone()),
                ev("m.space.parent", b.as_str(), &me, "$4", via.clone()),
            ]);
            room(b, vec![
                ev("m.space.child", a.as_str(), &me, "$5", child_of.clone()),
                ev("m.space.parent", a.as_str(), &me, "$6", via.clone()),
            ]);
            let _ = evil;
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            let chain = tokio::time::timeout(
                Duration::from_secs(10),
                canonical_ancestry(&client, child.as_str()),
            )
            .await
            .expect("bounded");
            assert_eq!(chain, vec![a.to_string(), b.to_string()]);
            let from_b = canonical_ancestry(&client, b.as_str()).await;
            assert_eq!(from_b, vec![a.to_string()]);
            drop(server);
        });
    }

    #[test]
    fn a_mixed_legacy_pack_is_split_without_broadening_or_losing_anything() {
        let legacy = json!({
            "pack": { "display_name": "Mine", "attribution": "me", "usage": ["sticker"] },
            "images": {
                "wave": { "url": "mxc://e/1", "usage": ["emoticon"], "body": "Wave" },
                "cat": { "url": "mxc://e/2" },
                "both": { "url": "mxc://e/3", "usage": ["sticker", "emoticon"],
                          "info": { "w": 64 }, "x-extra": 1 },
                "bad key": { "url": "mxc://e/4" },
                "nourl": { "body": "x" },
            }
        });
        let plan = plan_user_pack_migration(&legacy, "Mine");
        assert_eq!(plan.invalid_keys, vec!["bad key".to_owned()]);
        let by: BTreeMap<Partition, &Value> = plan.packs.iter().map(|(p, c)| (*p, c)).collect();
        assert_eq!(by.len(), 3);
        assert!(by[&Partition::Stickers]["images"].get("cat").is_some());
        assert_eq!(by[&Partition::Stickers]["pack"]["usage"], json!(["sticker"]));
        let wave = &by[&Partition::Emoji]["images"]["wave"];
        assert_eq!(wave["body"], json!("Wave"));
        assert!(wave.get("usage").is_none(), "no per-image usage in a stable pack");
        assert_eq!(by[&Partition::Emoji]["pack"]["usage"], json!(["emoticon"]));
        let both = &by[&Partition::Both];
        assert!(both["pack"].get("usage").is_none(), "both = absent usage");
        assert_eq!(both["images"]["both"]["info"]["w"], json!(64));
        assert_eq!(both["images"]["both"]["x-extra"], json!(1));
        assert_eq!(both["pack"]["attribution"], json!("me"));
        assert_eq!(both["pack"]["display_name"], json!("Mine (emoji and stickers)"));
        for (_, content) in &plan.packs {
            assert!(content["images"].is_object());
        }
        assert!(legacy["images"]["wave"].get("usage").is_some());
    }

    #[test]
    fn resuming_a_migration_adds_only_what_is_missing() {
        let planned = json!({ "pack": { "display_name": "P" },
                              "images": { "a": { "url": "mxc://e/1" }, "b": { "url": "mxc://e/2" } } });
        assert_eq!(merge_for_resume(PackRead::Absent, planned.clone()).unwrap(), planned);
        let existing = json!({ "pack": { "display_name": "Renamed" },
                               "images": { "a": { "url": "mxc://e/9", "body": "edited" } } });
        let merged = merge_for_resume(PackRead::Found(existing), planned.clone()).unwrap();
        assert_eq!(merged["images"]["a"]["url"], json!("mxc://e/9"), "not overwritten");
        assert_eq!(merged["images"]["b"]["url"], json!("mxc://e/2"));
        assert_eq!(merged["pack"]["display_name"], json!("Renamed"));
        assert_eq!(merge_for_resume(PackRead::Failed("network".into()), planned).unwrap_err(),
                   "network");
    }

    #[test]
    fn a_writer_reads_the_server_even_when_the_store_or_the_memo_disagree() {
        let rt = tokio::runtime::Runtime::new().unwrap();
        let older = json!({ "images": { "a": { "url": "mxc://e/old" } } });
        let newer = json!({ "images": { "a": { "url": "mxc://e/old" },
                                         "b": { "url": "mxc://e/new" } } });
        let (fresh, _) = rt.block_on(read_pack_with_policy(
            ReadPolicy::Fresh, async { Some(older.clone()) }, false,
            || async { PackRead::Found(newer.clone()) }));
        assert_eq!(fresh, PackRead::Found(newer.clone()));
        let (snap, _) = rt.block_on(read_pack_with_policy(
            ReadPolicy::Snapshot, async { Some(older.clone()) }, false,
            || async { PackRead::Found(newer.clone()) }));
        assert_eq!(snap, PackRead::Found(older.clone()), "snapshots stay cheap");
        let (fresh, remember) = rt.block_on(read_pack_with_policy(
            ReadPolicy::Fresh, async { None }, true,
            || async { PackRead::Found(newer.clone()) }));
        assert_eq!(fresh, PackRead::Found(newer.clone()));
        assert!(!remember);
        assert_eq!(base_for_add(fresh).unwrap(), newer);
        let (snap, _) = rt.block_on(read_pack_with_policy(
            ReadPolicy::Snapshot, async { None }, true,
            || async { PackRead::Found(newer.clone()) }));
        assert_eq!(snap, PackRead::Absent);
        let (failed, _) = rt.block_on(read_pack_with_policy(
            ReadPolicy::Fresh, async { Some(older.clone()) }, false,
            || async { PackRead::Failed("network".into()) }));
        assert!(base_for_add(failed).is_err());
    }

    #[test]
    fn every_room_pack_writer_reads_fresh() {
        let source = include_str!("stickers.rs");
        let production = &source[..source.find("#[cfg(test)]").unwrap()];
        for writer in ["async fn add_to_room_pack_inner(", "async fn edit_room_pack_inner(",
                       "pub(crate) fn create_room_pack(", "pub(crate) fn migrate_user_pack("] {
            let start = production.find(writer).unwrap_or_else(|| panic!("{writer}"));
            let end = production[start + writer.len()..]
                .find("\nasync fn ")
                .into_iter()
                .chain(production[start + writer.len()..].find("\npub(crate) "))
                .min()
                .map(|n| start + writer.len() + n)
                .unwrap_or(production.len());
            let body = &production[start..end];
            assert!(body.contains("read_room_pack_fresh("), "{writer} must read fresh");
            assert!(!body.contains("read_one_room_pack("), "{writer} reads a snapshot");
        }
    }

    #[test]
    fn a_pack_home_is_created_only_on_a_confirmed_absence_with_no_marked_room() {
        let marked = vec!["!home:e".to_owned()];
        assert_eq!(plan_pack_home(&PackRead::Failed("network".into()), false, &[], false),
                   HomeDecision::Refuse("network".into()));
        assert_eq!(plan_pack_home(&PackRead::Failed("network".into()), false, &marked, false),
                   HomeDecision::Refuse("network".into()));
        assert_eq!(plan_pack_home(&PackRead::Found(json!({ "room_id": "!j:e" })), true, &marked, false),
                   HomeDecision::UseJournal);
        assert_eq!(plan_pack_home(&PackRead::Absent, false, &marked, false),
                   HomeDecision::UseMarked("!home:e".into()));
        assert_eq!(plan_pack_home(&PackRead::Absent, false, &[], false), HomeDecision::Create);
        assert_eq!(plan_pack_home(&PackRead::Found(json!({ "room_id": "!j:e" })), false, &[], false),
                   HomeDecision::Refuse("home_unavailable".into()));
        assert_eq!(plan_pack_home(&PackRead::Found(json!({ "room_id": "!j:e" })), false, &[], true),
                   HomeDecision::Create);
        assert_eq!(plan_pack_home(&PackRead::Failed("network".into()), false, &[], true),
                   HomeDecision::Refuse("network".into()));
    }

    #[tokio::test]
    async fn pack_and_space_changes_in_sync_poke_cpp_with_their_room() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;
        use std::collections::VecDeque;
        use std::sync::Mutex;

        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let events: crate::EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let _handles = install_change_handlers(&client, Arc::clone(&events));

        let space = room_id!("!space:example.org");
        let room = room_id!("!room:example.org");
        let state = |ty: &str, key: &str, id: &str, content: Value| {
            Raw::from_json_string(json!({
                "type": ty, "state_key": key, "event_id": id,
                "sender": "@other:example.org", "origin_server_ts": 1_700_000_000_000_u64,
                "content": content,
            }).to_string()).expect("raw")
        };
        let mut space_update = v5::response::Room::default();
        space_update.timeline = vec![
            state("m.space.child", room.as_str(), "$c1", json!({ "via": ["example.org"] })),
            state("m.room.topic", "", "$t1", json!({ "topic": "not a pack change" })),
        ];
        let mut room_update = v5::response::Room::default();
        room_update.timeline = vec![
            state("m.room.image_pack", "k", "$p1", json!({ "images": {} })),
            state("m.space.parent", space.as_str(), "$sp", json!({ "via": ["example.org"], "canonical": true })),
        ];
        let mut response = v5::Response::new("pos-1".to_owned());
        response.rooms.insert(space.to_owned(), space_update);
        response.rooms.insert(room.to_owned(), room_update);
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
        let pokes_for = |id: &str| queued.iter().filter(|v| **v == change_payload(id)).count();
        assert_eq!(pokes_for(space.as_str()), 1, "{queued:?}");
        assert_eq!(pokes_for(room.as_str()), 2, "{queued:?}");
        assert_eq!(queued.len(), 3, "the topic change must not poke: {queued:?}");

        events.lock().unwrap().clear();
        let mut space_removal = v5::response::Room::default();
        space_removal.timeline = vec![state("m.space.child", room.as_str(), "$c2", json!({}))];
        let mut room_removal = v5::response::Room::default();
        room_removal.timeline = vec![state("m.space.parent", space.as_str(), "$sp2", json!({}))];
        let mut response = v5::Response::new("pos-2".to_owned());
        response.rooms.insert(space.to_owned(), space_removal);
        response.rooms.insert(room.to_owned(), room_removal);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        let removals: Vec<Value> = events
            .lock()
            .unwrap()
            .iter()
            .map(|s| serde_json::from_str(s).unwrap())
            .collect();
        assert_eq!(removals.len(), 2, "{removals:?}");
        assert!(removals.contains(&change_payload(space.as_str())), "{removals:?}");
        assert!(removals.contains(&change_payload(room.as_str())), "{removals:?}");
    }

    #[test]
    fn upload_dimensions_come_from_the_header_alone() {
        let mut png = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR".to_vec();
        png.extend_from_slice(&256u32.to_be_bytes());
        png.extend_from_slice(&128u32.to_be_bytes());
        assert_eq!(header_dimensions(&png), (256, 128));
        let gif = [b"GIF89a".as_slice(), &[40, 0, 20, 0]].concat();
        assert_eq!(header_dimensions(&gif), (40, 20));
        let mut webp = b"RIFF\0\0\0\0WEBPVP8X".to_vec();
        webp.extend_from_slice(&[0; 8]);
        webp.extend_from_slice(&[43, 1, 0, 149, 0, 0]);
        assert_eq!(header_dimensions(&webp), (300, 150));
        let mut vp8l = b"RIFF\0\0\0\0WEBPVP8L".to_vec();
        vp8l.extend_from_slice(&[0, 0, 0, 0, 0x2f]);
        vp8l.extend_from_slice(&[2, 0x40, 0, 0]);
        assert_eq!(header_dimensions(&vp8l), (3, 2));
        let jpeg = [
            &[0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0][..],
            &[0xff, 0xc0, 0, 17, 8, 0x01, 0xe0, 0x02, 0x80, 3][..],
        ]
        .concat();
        assert_eq!(header_dimensions(&jpeg), (640, 480));
        assert_eq!(header_dimensions(&png[..20]), (0, 0));
        assert_eq!(header_dimensions(b"<svg"), (0, 0));
        let mut zero = png.clone();
        zero[16..20].copy_from_slice(&0u32.to_be_bytes());
        assert_eq!(header_dimensions(&zero), (0, 0));
        let mut huge = png.clone();
        huge[16..20].copy_from_slice(&100_000u32.to_be_bytes());
        assert_eq!(header_dimensions(&huge), (0, 0));
        assert_eq!(header_dimensions(&[0xff, 0xd8, 0xff]), (0, 0));
    }

    use std::sync::atomic::{AtomicUsize, Ordering};

    async fn read_once(
        memory: &AbsentAnswers,
        stored: Option<Value>,
        answer: fn() -> ServerAnswer,
        asked: &AtomicUsize,
    ) -> Option<Value> {
        read_remembering_absence(
            memory,
            "@a:example.org",
            USER_EMOTES,
            async move { stored },
            || async {
                asked.fetch_add(1, Ordering::SeqCst);
                answer()
            },
        )
        .await
    }

    #[tokio::test]
    async fn a_server_not_found_is_asked_once() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        for _ in 0..9 {
            assert!(read_once(&memory, None, || ServerAnswer::Absent, &asked).await.is_none());
        }
        assert_eq!(asked.load(Ordering::SeqCst), 1);
    }

    #[tokio::test]
    async fn a_failed_request_is_not_remembered() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        for _ in 0..3 {
            assert!(read_once(&memory, None, || ServerAnswer::Failed, &asked).await.is_none());
        }
        assert_eq!(asked.load(Ordering::SeqCst), 3);
    }

    #[tokio::test]
    async fn sync_delivering_the_type_ends_the_memory() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        assert!(read_once(&memory, None, || ServerAnswer::Absent, &asked).await.is_none());
        let pack = json!({ "images": {} });
        assert_eq!(
            read_once(&memory, Some(pack.clone()), || ServerAnswer::Absent, &asked).await,
            Some(pack)
        );
        assert_eq!(asked.load(Ordering::SeqCst), 1, "the store answered the second read");
        assert!(read_once(&memory, None, || ServerAnswer::Absent, &asked).await.is_none());
        assert_eq!(asked.load(Ordering::SeqCst), 2);
    }

    #[tokio::test]
    async fn a_found_answer_is_not_cached_as_absent() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        let got = read_once(&memory, None, || ServerAnswer::Found(json!({"a": 1})), &asked).await;
        assert_eq!(got, Some(json!({"a": 1})));
        read_once(&memory, None, || ServerAnswer::Found(json!({"a": 1})), &asked).await;
        assert_eq!(asked.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn the_memory_is_per_account_per_type_and_expires() {
        let memory = AbsentAnswers::new();
        let t0 = std::time::Instant::now();
        memory.mark_absent("@a:x", USER_EMOTES, t0);
        assert!(memory.is_absent("@a:x", USER_EMOTES, t0));
        assert!(!memory.is_absent("@b:x", USER_EMOTES, t0), "another account");
        assert!(!memory.is_absent("@a:x", EMOTE_ROOMS, t0), "another type");
        assert!(memory.is_absent("@a:x", USER_EMOTES, t0 + ABSENT_ANSWER_TTL - Duration::from_secs(1)));
        assert!(!memory.is_absent("@a:x", USER_EMOTES, t0 + ABSENT_ANSWER_TTL), "expired");
        memory.mark_absent("@a:x", USER_EMOTES, t0);
        memory.forget("@a:x", USER_EMOTES);
        assert!(!memory.is_absent("@a:x", USER_EMOTES, t0), "our own write forgets it");
    }

    #[test]
    fn only_a_clean_not_found_is_absent() {
        use matrix_sdk::ruma::{events::AnyGlobalAccountDataEventContent, serde::Raw};
        type Fetched = Result<Option<Raw<AnyGlobalAccountDataEventContent>>, ()>;
        assert!(matches!(server_answer::<()>(Ok(None)), ServerAnswer::Absent));
        assert!(matches!(server_answer::<()>(Err(())), ServerAnswer::Failed));
        let found: Fetched = Ok(Some(Raw::from_json_string(r#"{"images":{}}"#.to_owned()).unwrap()));
        assert!(matches!(server_answer(found), ServerAnswer::Found(v) if v == json!({"images": {}})));
        let garbled: Fetched = Ok(Some(Raw::from_json_string("{}".to_owned()).unwrap()));
        assert!(matches!(server_answer(garbled), ServerAnswer::Found(_)));
    }

    #[test]
    fn every_account_data_write_forgets_the_absent_mark() {
        let source = include_str!("stickers.rs");
        let production = &source[..source.find("#[cfg(test)]").unwrap()];
        let writes = production.matches(".set_account_data_raw(").count();
        let forgets = production.matches("    forget_own_write(").count();
        assert_eq!(writes, 3, "a write site was added or removed: update this test");
        assert_eq!(forgets, writes, "every write site calls forget_own_write");
    }

    #[test]
    fn forgetting_an_own_write_clears_the_global_mark() {
        let account = "@forget-test:example.org";
        absent_answers().mark_absent(account, USER_EMOTES, std::time::Instant::now());
        assert!(absent_answers().is_absent(account, USER_EMOTES, std::time::Instant::now()));
        absent_answers().forget(account, USER_EMOTES);
        assert!(!absent_answers().is_absent(account, USER_EMOTES, std::time::Instant::now()));
    }
}
