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
            room::ImageInfo, sticker::StickerEventContent, AnyMessageLikeEventContent,
            GlobalAccountDataEventType, StateEventType,
        },
        MxcUri, OwnedMxcUri, RoomId, UInt,
    },
};
use serde_json::{json, Value};

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

// ---------------------------------------------------------------------------
// Event types and bounds
// ---------------------------------------------------------------------------

pub(crate) const USER_EMOTES: &str = "im.ponies.user_emotes";
pub(crate) const ROOM_EMOTES: &str = "im.ponies.room_emotes";
pub(crate) const EMOTE_ROOMS: &str = "im.ponies.emote_rooms";

/// Max packs in one snapshot; more means a hostile or broken blob.
const MAX_PACKS: usize = 64;

/// Max images per pack (large public packs have a few hundred).
const MAX_IMAGES_PER_PACK: usize = 512;

/// Shortcode length cap. MSC2545 allows 100 bytes; 64 is stricter, and the
/// sanitized alphabet is ASCII, so chars equal bytes.
const MAX_SHORTCODE_CHARS: usize = 64;

/// Refused before reading the file, so a large screenshot never reaches
/// memory or the homeserver.
const MAX_STICKER_UPLOAD_BYTES: u64 = 4 * 1024 * 1024;

/// A body is the sticker's alt text.
const MAX_BODY_CHARS: usize = 160;

/// Pack display name / attribution.
const MAX_PACK_NAME_CHARS: usize = 80;
const MAX_ATTRIBUTION_CHARS: usize = 160;

/// One bounded request per state read: the room-action pool is joined at
/// sign-out, so retry loops here would stall shutdown (as in `pinned.rs`).
const PACK_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// The five raster types Lightning accepts, matching
/// `rooms::sniff_image_mime`'s outputs: declared types are checked before
/// the request, bytes are sniffed on arrival.
const ALLOWED_MIMETYPES: [&str; 5] = [
    "image/png",
    "image/jpeg",
    "image/gif",
    "image/webp",
    "image/bmp",
];

// ---------------------------------------------------------------------------
// Sanitizers
// ---------------------------------------------------------------------------

/// Collapse to one line, drop control characters, bound the length.
fn one_line(text: &str, max_chars: usize) -> String {
    let collapsed: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let trimmed = collapsed.split_whitespace().collect::<Vec<_>>().join(" ");
    trimmed.chars().take(max_chars).collect()
}

/// Sanitize a shortcode (the key of the `images` map) to MSC2545's alphabet:
/// `[a-zA-Z0-9-_]+`, at most 100 bytes, no colons. Illegal characters become
/// `_`, runs collapse, and leading/trailing separators are trimmed.
///
/// Repaired rather than rejected, because real packs carry illegal
/// shortcodes (Sable writes `sticker-$eventId`) and dropping them would
/// empty other clients' packs. An empty result is dropped.
pub(crate) fn sanitize_shortcode(raw: &str) -> String {
    let mut out = String::new();
    for c in raw.trim().chars() {
        if c.is_ascii_alphanumeric() || c == '-' {
            out.push(c);
        } else if !out.ends_with('_') {
            // Collapse runs of illegal characters into one separator. A literal `_`
            // takes this branch too: once substituted, a typed and an inserted `_`
            // are indistinguishable, so they must collapse together. `-` is never
            // produced by substitution, so its runs are kept.
            out.push('_');
        }
        if out.len() >= MAX_SHORTCODE_CHARS {
            break;
        }
    }
    out.trim_matches(['-', '_']).to_owned()
}

/// True when `url` is a valid `mxc://server/mediaid`, as judged by
/// `MxcUri::parts()`.
pub(crate) fn is_valid_mxc(url: &str) -> bool {
    if !url.starts_with("mxc://") {
        return false;
    }
    <&MxcUri>::from(url).parts().is_ok()
}

/// A declared mimetype must be one Lightning can decode. Absent is unknown
/// and allowed.
fn mimetype_allowed(declared: Option<&str>) -> bool {
    match declared {
        None => true,
        Some(m) => ALLOWED_MIMETYPES.contains(&m.trim().to_ascii_lowercase().as_str()),
    }
}

// ---------------------------------------------------------------------------
// Pack model
// ---------------------------------------------------------------------------

/// Usage as MSC2545 defines it. Unknown values are ignored rather than a
/// third state, so a future client's pack stays visible.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub(crate) enum Usage {
    Emoticon,
    Sticker,
}

fn parse_usage(value: Option<&Value>) -> BTreeSet<Usage> {
    let mut out = BTreeSet::new();
    let Some(Value::Array(items)) = value else {
        return out;
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
    out
}

/// MSC2545's inheritance rule, in one place: the image's usage wins if it
/// declares any, else the pack's; empty at both levels means both. Returned
/// as a concrete pair so "empty means both" is not re-derived elsewhere.
fn resolve_usage(
    image: &BTreeSet<Usage>,
    pack: &BTreeSet<Usage>,
) -> (bool /*emoticon*/, bool /*sticker*/) {
    let effective = if !image.is_empty() { image } else { pack };
    if effective.is_empty() {
        return (true, true);
    }
    (
        effective.contains(&Usage::Emoticon),
        effective.contains(&Usage::Sticker),
    )
}

/// One validated pack image, ready to cross the FFI.
#[derive(Clone, Debug)]
pub(crate) struct PackImage {
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

/// One validated pack.
#[derive(Clone, Debug)]
pub(crate) struct Pack {
    /// Stable id for C++: `user`, or `room:<room_id>:<state_key>`.
    pub id: String,
    pub display_name: String,
    pub avatar_url: String,
    pub attribution: String,
    /// `user` | `room`.
    pub source: &'static str,
    /// Empty for the user pack.
    pub room_id: String,
    /// Empty for the user pack; otherwise the state key (which may itself be
    /// empty: the room's default pack).
    pub state_key: String,
    /// Room packs only: whether `im.ponies.emote_rooms` lists this pack, i.e.
    /// it is usable outside its room. Always false for the user pack.
    pub enabled_globally: bool,
    /// Room packs only: whether this account may write `im.ponies.room_emotes`
    /// there, per the room's required power level (asked of the SDK). False
    /// until known.
    pub can_manage: bool,
    pub images: Vec<PackImage>,
}

impl Pack {
    fn to_json(&self) -> Value {
        json!({
            "id": self.id,
            "display_name": self.display_name,
            "avatar_url": self.avatar_url,
            "attribution": self.attribution,
            "source": self.source,
            "room_id": self.room_id,
            "state_key": self.state_key,
            "enabled_globally": self.enabled_globally,
            "can_manage": self.can_manage,
            "images": self.images.iter().map(|i| json!({
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

/// Parse one pack's `{ images, pack }` content. `None` only when it is not
/// an object; a pack with no usable images is an empty pack, not a failure.
pub(crate) fn parse_pack_content(
    content: &Value,
    id: String,
    source: &'static str,
    room_id: String,
    state_key: String,
    fallback_name: &str,
) -> Option<Pack> {
    let object = content.as_object()?;

    let pack_info = object.get("pack").and_then(|p| p.as_object());
    let pack_usage = parse_usage(pack_info.and_then(|p| p.get("usage")));

    let display_name = pack_info
        .and_then(|p| p.get("display_name"))
        .and_then(|v| v.as_str())
        .map(|s| one_line(s, MAX_PACK_NAME_CHARS))
        .filter(|s| !s.is_empty())
        // MSC2545: a room pack without display_name defaults to the room name,
        // which the caller supplies.
        .unwrap_or_else(|| one_line(fallback_name, MAX_PACK_NAME_CHARS));

    // A pack avatar is an image too: non-mxc avatars are dropped.
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
        // Sorted, so a pack's order is stable across refreshes (serde_json has no
        // `preserve_order` here).
        let sorted: BTreeMap<&String, &Value> = map.iter().collect();
        for (raw_code, entry) in sorted {
            if images.len() >= MAX_IMAGES_PER_PACK {
                break;
            }
            let Some(entry) = entry.as_object() else { continue };

            let shortcode = sanitize_shortcode(raw_code);
            if shortcode.is_empty() {
                continue;
            }

            // The one unconditional requirement of MSC2545's PackImage.
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
                // SVG and anything else Lightning cannot decode.
                continue;
            }

            let image_usage = parse_usage(entry.get("usage"));
            let (is_emoticon, is_sticker) = resolve_usage(&image_usage, &pack_usage);

            // MSC2545: `body` defaults to the shortcode.
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
        display_name,
        avatar_url,
        attribution,
        source,
        room_id,
        state_key,
        // Account-relationship properties, set by the caller after parsing.
        enabled_globally: false,
        can_manage: false,
        images,
    })
}

/// Parse `im.ponies.emote_rooms` into the (room id, state key) pairs it
/// enables. Invalid room ids are dropped.
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

// ---------------------------------------------------------------------------
// Reading packs
// ---------------------------------------------------------------------------

/// Read every pack available to this account and emit one `sticker_packs`
/// snapshot, in this order:
///   1. `im.ponies.user_emotes`, the account's own pack;
///   2. the active room's own `im.ponies.room_emotes` packs (usable inside
///      that room without opt-in);
///   3. every (room, state key) in `im.ponies.emote_rooms` not already
///      collected.
///
/// Empty `room_id` skips step 2. Account data is read from the store, then
/// the server, so a cold start is correct rather than empty.
pub(crate) fn fetch_packs(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let mut packs: Vec<Pack> = Vec::new();
        let mut seen: BTreeSet<(String, String)> = BTreeSet::new();

        // ---- 0. which room packs this account has turned on ----------
        //
        // Read first: it also answers `enabled_globally` for the active room's
        // packs.
        let enabled = match read_global_account_data(&client, EMOTE_ROOMS).await {
            Some(content) => parse_emote_rooms(&content),
            None => Vec::new(),
        };
        let enabled_set: BTreeSet<(String, String)> =
            enabled.iter().cloned().collect();

        // ---- 1. the account's own pack -------------------------------
        if let Some(content) = read_global_account_data(&client, USER_EMOTES).await {
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

        // ---- 2. the active room's own packs ---------------------------
        //
        // `can_manage` is reported for the room on the snapshot itself: a room with
        // no pack yet has no row to carry it, and its first pack must be creatable.
        let mut room_can_manage = false;
        if !room_id.is_empty() {
            let can_manage = can_manage_room_packs(&client, &room_id).await;
            room_can_manage = can_manage;
            for (state_key, content, name) in
                read_room_packs(&client, &room_id).await
            {
                if packs.len() >= MAX_PACKS {
                    break;
                }
                seen.insert((room_id.clone(), state_key.clone()));
                if let Some(mut pack) = parse_pack_content(
                    &content,
                    format!("room:{room_id}:{state_key}"),
                    "room",
                    room_id.clone(),
                    state_key.clone(),
                    &name,
                ) {
                    pack.enabled_globally =
                        enabled_set.contains(&(room_id.clone(), state_key));
                    pack.can_manage = can_manage;
                    packs.push(pack);
                }
            }
        }

        // ---- 3. globally enabled room packs ---------------------------
        for (enabled_room, state_key) in enabled {
            if packs.len() >= MAX_PACKS {
                break;
            }
            if seen.contains(&(enabled_room.clone(), state_key.clone())) {
                continue;
            }
            let Some((content, name)) =
                read_one_room_pack(&client, &enabled_room, &state_key).await
            else {
                continue;
            };
            if let Some(mut pack) = parse_pack_content(
                &content,
                format!("room:{enabled_room}:{state_key}"),
                "room",
                enabled_room.clone(),
                state_key,
                &name,
            ) {
                // In `emote_rooms` by construction.
                pack.enabled_globally = true;
                pack.can_manage =
                    can_manage_room_packs(&client, &enabled_room).await;
                packs.push(pack);
            }
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "sticker_packs",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "room_can_manage": room_can_manage,
                "packs": packs.iter().map(Pack::to_json).collect::<Vec<_>>(),
            }),
        );
    });
    Ok(())
}

/// Whether this account may write `im.ponies.room_emotes` in `room_id`, per
/// the room's own required power level, asked of the SDK (never a role
/// label). An unresolvable room or unreadable membership answers false.
async fn can_manage_room_packs(client: &matrix_sdk::Client, room_id: &str) -> bool {
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
        .is_some_and(|m| m.can_send_state(StateEventType::from(ROOM_EMOTES)))
}

/// Turn one room pack on or off in `im.ponies.emote_rooms` ("use this room's
/// stickers everywhere"). Account data: the reader's own choice, no power
/// level. A room's packs remain usable inside that room regardless.
///
/// Read-modify-write against the server copy, never the store: account data
/// has no server-side merge, so a stale blob would drop other devices'
/// selections.
pub(crate) fn set_room_pack_enabled(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    enabled: bool,
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
            set_room_pack_enabled_inner(&client, &room_id, &state_key, enabled)
                .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match outcome {
            Ok(()) => (true, String::new()),
            Err(category) => (false, category),
        };
        enqueue(
            &events,
            json!({
                "type": "sticker_pack_rooms_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "category": category,
                "room_id": room_id,
                "state_key": state_key,
                "enabled": enabled,
            }),
        );
    });
    Ok(())
}

/// The pure `im.ponies.emote_rooms` read-modify-write, testable offline.
/// Takes whatever the server returned (another client's blob) and returns
/// the content to write.
pub(crate) fn apply_emote_rooms_change(
    existing: Value,
    room_id: &str,
    state_key: &str,
    enabled: bool,
) -> Result<Value, String> {
    let mut content = existing;
    // A malformed blob (non-object, or a non-object `rooms`) is replaced, not
    // refused: refusing would lock the user out, and it holds no valid
    // selection to preserve.
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
        // ruma 0.34's ImagePackRoomContent has no overrides: the key's presence is
        // the enablement, so an empty object is the whole payload.
        entry
            .as_object_mut()
            .ok_or_else(|| "rejected".to_owned())?
            .insert(state_key.to_owned(), json!({}));
    } else {
        let mut room_now_empty = false;
        if let Some(entry) = rooms.get_mut(room_id).and_then(|v| v.as_object_mut())
        {
            entry.remove(state_key);
            room_now_empty = entry.is_empty();
        }
        // Drop a room whose last pack was turned off; an absent key means the same.
        if room_now_empty {
            rooms.remove(room_id);
        }
    }
    Ok(content)
}

async fn set_room_pack_enabled_inner(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    enabled: bool,
) -> Result<(), String> {
    let ty = GlobalAccountDataEventType::from(EMOTE_ROOMS);

    let existing = match client.account().fetch_account_data(ty.clone()).await {
        Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get())
            .unwrap_or_else(|_| json!({})),
        Ok(None) => json!({}),
        Err(err) => return Err(classify_room_error(&err.to_string()).to_owned()),
    };

    let content =
        apply_emote_rooms_change(existing, room_id, state_key, enabled)?;

    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    client
        .account()
        .set_account_data_raw(ty, raw)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(())
}

/// Store first, then the server. `None` means absent or unreadable; either
/// way the source contributes nothing.
async fn read_global_account_data(
    client: &matrix_sdk::Client,
    event_type: &str,
) -> Option<Value> {
    let ty = GlobalAccountDataEventType::from(event_type);
    if let Ok(Some(raw)) = client.account().account_data_raw(ty.clone()).await {
        if let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) {
            return Some(value);
        }
    }
    match client.account().fetch_account_data(ty).await {
        Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get()).ok(),
        _ => None,
    }
}

/// Every `im.ponies.room_emotes` state event in one room, as (state key,
/// content, room name). The room name is the MSC's default display name.
async fn read_room_packs(
    client: &matrix_sdk::Client,
    room_id: &str,
) -> Vec<(String, Value, String)> {
    let Ok(room) = joined_room(client, room_id) else {
        return Vec::new();
    };
    let name = room.name().unwrap_or_default();
    let Ok(events) = room
        .get_state_events(StateEventType::from(ROOM_EMOTES))
        .await
    else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for raw in events {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        let Ok(value) = serde_json::from_str::<Value>(&json) else {
            continue;
        };
        let state_key = value
            .get("state_key")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_owned();
        let Some(content) = value.get("content") else { continue };
        // An empty content object retires a state event: a removed pack.
        if content.get("images").is_none() {
            continue;
        }
        out.push((state_key, content.clone(), name.clone()));
    }
    out
}

/// One room pack by state key. Sliding sync does not deliver custom state
/// types, so a store miss is normal and the bounded `/state` read answers
/// (as in `banner.rs`).
async fn read_one_room_pack(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
) -> Option<(Value, String)> {
    use matrix_sdk::ruma::api::client::state::get_state_event_for_key;

    let room = joined_room(client, room_id).ok()?;
    let name = room.name().unwrap_or_default();

    if let Ok(Some(raw)) = room
        .get_state_event(StateEventType::from(ROOM_EMOTES), state_key)
        .await
    {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        if let Ok(value) = serde_json::from_str::<Value>(&json) {
            if let Some(content) = value.get("content") {
                if content.get("images").is_some() {
                    return Some((content.clone(), name));
                }
            }
        }
    }

    let config = RequestConfig::new()
        .disable_retry()
        .timeout(PACK_REQUEST_TIMEOUT);
    let request = get_state_event_for_key::v3::Request::new(
        room.room_id().to_owned(),
        StateEventType::from(ROOM_EMOTES),
        state_key.to_owned(),
    );
    let response = client.send(request).with_request_config(config).await.ok()?;
    let value = serde_json::from_str::<Value>(response.event_or_content.get()).ok()?;
    value.get("images")?;
    Some((value, name))
}

// ---------------------------------------------------------------------------
// The sticker event's `info`
// ---------------------------------------------------------------------------

/// The `info` block of a sticker event, with unknown fields omitted.
///
/// A width of 0 would claim the image is zero pixels wide; unknown is
/// encoded by omitting the field. Stickers from packs Lightning uploaded
/// have no dimensions (`upload_to_user_pack` does not decode images), and
/// other clients use these fields to reserve space.
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

// ---------------------------------------------------------------------------
// Sending a sticker
// ---------------------------------------------------------------------------

/// Send one `m.sticker` to a room or thread.
///
/// The media is the pack's own `mxc://`, as every client sends it. In an
/// encrypted room the event is encrypted but the bitmap is ordinary
/// unencrypted media (inherent to MSC2545), so the picker must never
/// present pack stickers as private.
///
/// Sent through the SDK timeline (`Timeline::send`), so local echo, send
/// queue and Retry work as for messages, and the SDK attaches the `m.thread`
/// relation for a thread root. No relation is built by hand (CLAUDE.md §8).
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

// ---------------------------------------------------------------------------
// Writing the user's own pack
// ---------------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
/// Upload a local image file and add it to this account's own pack: the
/// only way to create a pack from nothing (every other route needs an
/// existing mxc).
///
/// Uploads, then reuses `add_to_user_pack_inner`, so dedupe, collision
/// handling and the cap are shared with the save path. The MIME is sniffed
/// from the bytes with the shared sniffer, so SVG is refused.
pub(crate) fn upload_to_user_pack(
    bridge: &RustClient,
    op_id: u64,
    shortcode: String,
    body: String,
    local_path: String,
) -> Result<(), String> {
    // Checked before reading, so an absurd file never reaches memory.
    let metadata = std::fs::metadata(&local_path)
        .map_err(|_| "sticker file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("sticker path is not a regular file".to_owned());
    }
    if metadata.len() == 0 || metadata.len() > MAX_STICKER_UPLOAD_BYTES {
        return Err("sticker file size is out of range".to_owned());
    }
    let requested = sanitize_shortcode(&shortcode);
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
            let upload = client
                .media()
                .upload(&mime, data, None)
                .await
                .map_err(|_| "upload_failed".to_owned())?;
            let url = upload.content_uri.to_string();
            // Dimensions stay 0: `info` is advisory, and decoding here just to fill it
            // would add an image decoder to this path.
            add_to_user_pack_inner(
                &client, requested, url, body, mime_str.to_owned(), 0, 0, size,
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

/// Add one image to `im.ponies.user_emotes` ("save this sticker").
///
/// Read-modify-write against the server copy, never the store: account data
/// has no merge, and a stale write would delete images added since. An
/// image whose `url` is already present is reported as `duplicate` and
/// nothing is written; a shortcode taken by a different url gets a numeric
/// suffix (`cat`, `cat-2`, ...), so nothing is overwritten.
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
) -> Result<(), String> {
    if !is_valid_mxc(&url) {
        return Err("sticker url is not an mxc URI".to_owned());
    }
    if !mimetype.is_empty() && !mimetype_allowed(Some(&mimetype)) {
        return Err("sticker mimetype is not a supported raster image".to_owned());
    }
    let requested = sanitize_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = add_to_user_pack_inner(
            &client, requested, url, body, mimetype, width, height, size,
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

/// Add one image to a room's `im.ponies.room_emotes` pack. Room state, so
/// power-level gated:
///
///   * the gate is the room's own required level for
///     `im.ponies.room_emotes`, via the SDK (`can_send_state`), and false
///     when membership cannot be read. The server would refuse anyway; this
///     stops callers that did not ask.
///   * nothing is optimistic: the caller re-reads the pack afterwards.
///
/// Read-modify-write of the current state event, so a concurrent edit by
/// another moderator is not clobbered.
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
    let requested = sanitize_shortcode(&shortcode);
    let body = one_line(&body, MAX_BODY_CHARS);
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = add_to_room_pack_inner(
            &client, &room_id, &state_key, requested, url, body, mimetype,
            width, height, size,
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
) -> Result<String, String> {
    let room = joined_room(client, room_id).map_err(|_| "unknown_room".to_owned())?;
    // Gate before anything is read or built.
    if !can_manage_room_packs(client, room_id).await {
        return Err("forbidden".to_owned());
    }

    // A miss means no pack under that key yet: a normal first use.
    let existing = match read_one_room_pack(client, room_id, state_key).await {
        Some((content, _name)) => content,
        None => json!({}),
    };

    let (content, code) =
        add_image_to_pack_content(existing, requested, url, body, mimetype,
                                  width, height, size)?;

    room.send_state_event_raw(ROOM_EMOTES, state_key, content)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

/// Pure "add one image to a pack content object", shared by the user and
/// room pack writers so duplicate policy, the size cap and shortcode
/// collisions cannot diverge. Returns the content and the shortcode
/// actually used (possibly suffixed).
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
    // Already present, by identity (the mxc), not by name.
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
        code = format!("{base}-{n}");
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
    // Mark it a sticker so it stays out of emoticon completion.
    entry.insert("usage".to_owned(), json!(["sticker"]));

    images.insert(code.clone(), Value::Object(entry));
    Ok((content, code))
}

/// One pack edit, independent of where the pack lives (account data or room
/// state). The rules live in the pure transforms; this carries the intent
/// to the one shared writer.
pub(crate) enum PackEdit {
    RemoveImage { shortcode: String },
    RenameImage { from: String, to: String },
    SetName { name: String },
    /// Empty the pack by writing an empty object (no delete verb exists for
    /// account data or state). Not a redaction: the event stays in history.
    DeletePack,
}

impl PackEdit {
    /// Apply to a content object. Returns the content to write and the applied
    /// shortcode for a rename (empty otherwise).
    fn apply(&self, existing: Value) -> Result<(Value, String), String> {
        match self {
            PackEdit::RemoveImage { shortcode } => {
                remove_image_from_pack_content(existing, shortcode)
                    .map(|c| (c, String::new()))
            }
            PackEdit::RenameImage { from, to } => {
                rename_image_in_pack_content(existing, from, to)
            }
            PackEdit::SetName { name } => {
                set_pack_display_name_content(existing, name)
                    .map(|c| (c, String::new()))
            }
            // Drops the name too: a named empty pack would not read as deleted.
            PackEdit::DeletePack => Ok((json!({}), String::new())),
        }
    }
}

/// Edit a pack: the user's own (empty `room_id`) or the room pack under
/// `state_key`. One writer for both. Read-modify-write, so concurrent edits
/// from other moderators or devices are not clobbered.
pub(crate) fn edit_pack(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    state_key: String,
    edit: PackEdit,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let outcome = if room_id.is_empty() {
            edit_user_pack_inner(&client, &edit).await
        } else {
            edit_room_pack_inner(&client, &room_id, &state_key, &edit).await
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
    // something that must already exist.
    let existing = match client.account().fetch_account_data(ty.clone()).await {
        Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get())
            .unwrap_or_else(|_| json!({})),
        Ok(None) => return Err("not_found".to_owned()),
        Err(err) => return Err(classify_room_error(&err.to_string()).to_owned()),
    };
    let (content, code) = edit.apply(existing)?;
    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    client
        .account()
        .set_account_data_raw(ty, raw)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

async fn edit_room_pack_inner(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    edit: &PackEdit,
) -> Result<String, String> {
    let room = joined_room(client, room_id).map_err(|_| "unknown_room".to_owned())?;
    // The same gate as the add path; deleting a room's pack is more
    // destructive than adding to it.
    if !can_manage_room_packs(client, room_id).await {
        return Err("forbidden".to_owned());
    }
    let existing = match read_one_room_pack(client, room_id, state_key).await {
        Some((content, _name)) => content,
        None => return Err("not_found".to_owned()),
    };
    let (content, code) = edit.apply(existing)?;
    room.send_state_event_raw(ROOM_EMOTES, state_key, content)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

/// Remove one image from a pack by shortcode. Pure and shared by both
/// writers. Removing the last image leaves an empty `images` map and keeps
/// the `pack` block, so the pack keeps its name.
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
        // Not found: report it rather than "removed", since the caller re-reads the
        // pack.
        return Err("not_found".to_owned());
    }
    Ok(content)
}

/// Change one image's shortcode, keeping its entry. The shortcode is the map
/// key, so this re-keys in one transform (remove-then-add could lose the
/// image if the add failed). A collision is refused rather than suffixed:
/// a deliberate rename means the name typed.
pub(crate) fn rename_image_in_pack_content(
    existing: Value,
    from: &str,
    to: &str,
) -> Result<(Value, String), String> {
    let code = sanitize_shortcode(to);
    if code.is_empty() {
        return Err("invalid_shortcode".to_owned());
    }
    if code == from {
        // Unchanged: report success, the pack is as requested.
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

/// Set or clear a pack's display name. Empty removes the field: MSC2545
/// room packs without one fall back to the room name, whereas "" would show
/// a blank tab.
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
) -> Result<String, String> {
    let ty = GlobalAccountDataEventType::from(USER_EMOTES);

    // Server read. A 404 means no pack yet: a normal first use.
    let existing = match client.account().fetch_account_data(ty.clone()).await {
        Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get())
            .unwrap_or_else(|_| json!({})),
        Ok(None) => json!({}),
        Err(err) => return Err(classify_room_error(&err.to_string()).to_owned()),
    };

    // The shared transform (see add_image_to_pack_content).
    let (mut content, code) = add_image_to_pack_content(
        existing, requested, url, body, mimetype, width, height, size,
    )?;

    // Name a brand-new user pack so it is not an unnamed tab elsewhere. Not for
    // room packs, which default to the room name.
    if content.get("pack").and_then(|p| p.as_object()).is_none() {
        content["pack"] = json!({ "display_name": "Stickers" });
    }

    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    client
        .account()
        .set_account_data_raw(ty, raw)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

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
        // MSC2545: body defaults to the shortcode.
        assert_eq!(image.body, "cat");
        // No usage anywhere means BOTH.
        assert!(image.is_emoticon);
        assert!(image.is_sticker);
    }

    #[test]
    fn a_non_mxc_url_is_dropped_not_carried() {
        // An https URL on a picker tile would be a tracking request per listing.
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
                // Absent mimetype is unknown: passes here, byte-sniffed on arrival.
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
                // An unknown usage value is ignored, so the pack's usage applies.
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

        // No usage at either level: both.
        let open = parse(json!({
            "images": { "x": { "url": "mxc://example.org/1" } }
        }));
        assert_eq!((open.images[0].is_emoticon, open.images[0].is_sticker),
                   (true, true));
    }

    #[test]
    fn shortcodes_are_repaired_to_the_mscs_own_alphabet() {
        // MSC2545: `[a-zA-Z0-9-_]+`, no colons, at most 100 bytes.
        assert_eq!(sanitize_shortcode("cat"), "cat");
        assert_eq!(sanitize_shortcode("  cat  "), "cat");
        assert_eq!(sanitize_shortcode(":wrapped:"), "wrapped");
        assert_eq!(sanitize_shortcode("has space"), "has_space");
        assert_eq!(sanitize_shortcode("ctrl\u{0007}x"), "ctrl_x");
        assert_eq!(sanitize_shortcode("my.cool.cat"), "my_cool_cat");
        // Runs of illegal characters collapse to one separator.
        assert_eq!(sanitize_shortcode("a...b"), "a_b");
        assert_eq!(sanitize_shortcode("emoji_\u{1F600}_here"), "emoji_here");
        // Sable's shape, illegal twice over (`$`, and a colon in room versions
        // 1-2): repaired, not dropped.
        assert_eq!(
            sanitize_shortcode("sticker-$AbCd:example.org"),
            "sticker-_AbCd_example_org"
        );
        // Nothing usable at all.
        assert_eq!(sanitize_shortcode(":::"), "");
        assert_eq!(sanitize_shortcode("---"), "");
        assert_eq!(sanitize_shortcode(""), "");
        // Bounded.
        assert_eq!(
            sanitize_shortcode(&"a".repeat(500)).len(),
            MAX_SHORTCODE_CHARS
        );
    }

    #[test]
    fn an_image_whose_shortcode_sanitizes_to_nothing_is_dropped() {
        let pack = parse(json!({
            "images": {
                ":::": { "url": "mxc://example.org/5" },
                "good": { "url": "mxc://example.org/6" }
            }
        }));
        assert_eq!(pack.images.len(), 1);
        assert_eq!(pack.images[0].shortcode, "good");
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
        // MSC2545: a room pack without display_name uses the room name.
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
        // An empty pack and no pack are different facts.
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
        )
    }

    #[test]
    fn adding_an_image_writes_the_mscs_own_shape() {
        let (content, code) =
            add(json!({}), "cat", "mxc://example.org/a").expect("adds");
        assert_eq!(code, "cat");
        let entry = &content["images"]["cat"];
        assert_eq!(entry["url"], json!("mxc://example.org/a"));
        // A saved image is marked a sticker, never an emoticon.
        assert_eq!(entry["usage"], json!(["sticker"]));
        assert_eq!(entry["info"]["mimetype"], json!("image/png"));
        assert_eq!(entry["info"]["w"], json!(128));
        // `body` equal to the shortcode is MSC2545's default; not written.
        assert!(entry.get("body").is_none());
    }

    #[test]
    fn the_same_mxc_twice_is_a_duplicate_by_identity_not_by_name() {
        let (content, _) =
            add(json!({}), "cat", "mxc://example.org/a").expect("adds");
        // A different name for the same image is still the same image.
        assert_eq!(
            add(content.clone(), "kitty", "mxc://example.org/a"),
            Err("duplicate".to_owned())
        );
        // A different image under a taken name gets a suffix.
        let (after, code) =
            add(content, "cat", "mxc://example.org/b").expect("adds");
        assert_eq!(code, "cat-2");
        assert_eq!(after["images"]["cat"]["url"], json!("mxc://example.org/a"));
        assert_eq!(after["images"]["cat-2"]["url"], json!("mxc://example.org/b"));
    }

    #[test]
    fn an_unnamed_image_still_gets_a_usable_shortcode() {
        // Nothing survives sanitizing; the image still needs a key.
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
        // A full pack must not silently discard the addition.
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
        // Read-modify-write: other devices' or moderators' images survive.
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
        // The pack metadata is untouched.
        assert_eq!(after["pack"]["display_name"], json!("Theirs"));
    }

    #[test]
    fn enabling_a_room_pack_preserves_every_other_selection() {
        // Other devices' selections survive our write.
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
        // The key's presence is the enablement.
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
        // One of two removed: the room stays with the other.
        assert_eq!(after["rooms"]["!a:example.org"], json!({ "": {} }));

        let empty = apply_emote_rooms_change(
            after, "!a:example.org", "", false,
        )
        .expect("applies");
        // Its last pack removed: the room key goes too.
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
        // The cap is on rooms, so an existing room may gain a second pack.
        let after =
            apply_emote_rooms_change(full, "!r0:example.org", "second", true)
                .expect("applies");
        assert_eq!(
            after["rooms"]["!r0:example.org"].as_object().unwrap().len(),
            2
        );
    }

    // No dimensions must serialize as absent, not zero. Checked on the JSON,
    // since that is what other clients parse.
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
        // The two facts it DOES have still travel.
        assert_eq!(wire.get("size").and_then(Value::as_u64), Some(43008));
        assert_eq!(
            wire.get("mimetype").and_then(Value::as_str),
            Some("image/webp")
        );
    }

    // Real dimensions pass through untouched.
    #[test]
    fn a_measured_sticker_still_carries_what_it_measured() {
        let info = sticker_image_info("image/png".to_owned(), 512, 384, 900);
        let wire = serde_json::to_value(&info).expect("info serializes");
        assert_eq!(wire.get("w").and_then(Value::as_u64), Some(512));
        assert_eq!(wire.get("h").and_then(Value::as_u64), Some(384));
    }

    // An empty mimetype stays omitted: MSC2545 allows absence, and "" would be
    // an invented type.
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
    }

    #[test]
    fn allowed_mimetypes_match_the_byte_sniffers_outputs() {
        // The declared-type allowlist and the byte sniffer accept exactly the same
        // set.
        for m in ALLOWED_MIMETYPES {
            assert!(mimetype_allowed(Some(m)), "{m} should be allowed");
        }
        assert!(!mimetype_allowed(Some("image/svg+xml")));
        assert!(!mimetype_allowed(Some("image/avif")));
        assert!(!mimetype_allowed(Some("text/html")));
        assert!(!mimetype_allowed(Some("")));
        // Absent is unknown, which is allowed.
        assert!(mimetype_allowed(None));
    }

    // ── Pack management (MSC2545 CRUD) ──────────────────────────────────

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
        // The pack's name is kept.
        assert_eq!(out["pack"]["display_name"], json!("Blobs"));
    }

    #[test]
    fn removing_something_absent_is_reported_not_swallowed() {
        // Removing something absent is reported, not swallowed.
        let content = json!({ "images": { "blob": { "url": "mxc://e/a" } } });
        assert_eq!(
            remove_image_from_pack_content(content, "nope").unwrap_err(),
            "not_found"
        );
        // A pack with no images gives the same answer, not a panic.
        assert_eq!(
            remove_image_from_pack_content(json!({}), "blob").unwrap_err(),
            "not_found"
        );
    }

    #[test]
    fn removing_the_last_image_leaves_an_empty_pack_that_keeps_its_name() {
        // Not the same as deleting: the pack keeps its name (a room pack would
        // otherwise fall back to the room name).
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
        // The whole entry moves, including usage.
        assert_eq!(images["blobcat"]["url"], json!("mxc://example.org/a"));
        assert_eq!(images["blobcat"]["usage"], json!(["emoticon"]));

        // A collision is refused, not suffixed.
        assert_eq!(
            rename_image_in_pack_content(content.clone(), "blob", "wave")
                .unwrap_err(),
            "shortcode_taken"
        );
        // Renaming something absent is not a silent no-op.
        assert_eq!(
            rename_image_in_pack_content(content.clone(), "ghost", "x")
                .unwrap_err(),
            "not_found"
        );
        // A name that sanitizes to nothing is refused before anything moves.
        assert_eq!(
            rename_image_in_pack_content(content, "blob", "   ").unwrap_err(),
            "invalid_shortcode"
        );
    }

    #[test]
    fn renaming_to_the_same_name_succeeds_without_losing_the_image() {
        // Renaming to the same name succeeds (a naive "target exists" guard would
        // refuse it).
        let content = json!({ "images": { "blob": { "url": "mxc://e/a" } } });
        let (out, code) =
            rename_image_in_pack_content(content, "blob", "blob").unwrap();
        assert_eq!(code, "blob");
        assert_eq!(out["images"]["blob"]["url"], json!("mxc://e/a"));
    }

    #[test]
    fn an_empty_pack_name_removes_the_field_rather_than_storing_blank() {
        // An empty name removes the field: a room pack then shows the room name
        // instead of a blank tab.
        let named =
            set_pack_display_name_content(json!({}), "  Blobs  ").unwrap();
        assert_eq!(named["pack"]["display_name"], json!("Blobs"));
        let cleared = set_pack_display_name_content(named, "").unwrap();
        assert!(cleared["pack"].as_object().unwrap().get("display_name")
                    .is_none());
        // The images survive a rename in either direction.
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
        // Deleting leaves nothing, name included. An empty object is the idiom; it
        // is not a redaction.
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
        // The action arrives as a string; each arm must run its own transform.
        let base = json!({
            "pack": { "display_name": "Blobs" },
            "images": {
                "blob": { "url": "mxc://e/a" },
                "wave": { "url": "mxc://e/b" },
            }
        });
        let (removed, _) = PackEdit::RemoveImage { shortcode: "blob".into() }
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
        // Images are untouched by renaming the pack.
        assert_eq!(named["images"].as_object().unwrap().len(), 2);
    }
}
