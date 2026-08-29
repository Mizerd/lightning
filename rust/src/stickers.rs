//! Stickers and image packs — MSC2545 (`im.ponies.*`), plus `m.sticker` send.
//!
//! Lightning invents NO storage format here. The three events are exactly the
//! ones MSC2545 defines, and the shapes below are transcribed from ruma's own
//! `ruma_events::image_pack` module (ruma-events 0.34.0, `src/image_pack.rs`),
//! which models the same MSC:
//!
//!   * `im.ponies.user_emotes`  — GLOBAL ACCOUNT DATA. The account's own pack.
//!   * `im.ponies.room_emotes`  — ROOM STATE. **The state key IS the pack id**,
//!     so one room may publish several packs; the empty state key is the
//!     room's default pack.
//!   * `im.ponies.emote_rooms`  — GLOBAL ACCOUNT DATA. `{ "rooms": { room_id:
//!     { state_key: {} } } }`, selecting which room packs are active GLOBALLY
//!     (a room's own packs are always available inside that room).
//!
//! We deliberately do NOT enable ruma's `unstable-msc2545` feature and use its
//! typed structs. That feature is off in this build, and turning it on would
//! mean taking ruma-events as a DIRECT dependency — a dependency change, which
//! CLAUDE.md forbids doing incidentally. Hand-written serde_json against the
//! identical JSON shape costs nothing and keeps `Cargo.lock` untouched. If the
//! feature is ever enabled for another reason, this module should switch to
//! the typed structs rather than keep its own.
//!
//! # A pack is remote, user-chosen content
//!
//! Every field below arrives from another user's account data or from room
//! state that anyone with the power level can write. Nothing here is trusted:
//!
//!   * `url` MUST be a syntactically valid `mxc://server/id`. Anything else —
//!     an `https://` tracking pixel, a `file://`, a `data:` blob — makes the
//!     image DROPPED, not merely unrendered. A pack that could put an http URL
//!     on a picker tile is a beacon that fires once per pack listing.
//!   * a DECLARED `info.mimetype` outside the five raster types is a refusal.
//!     `image/svg+xml` is the case that matters: CLAUDE.md §6 forbids
//!     untrusted SVG in a media path, and this build's QImageReader really
//!     does decode `svg`/`svgz` (measured, not assumed). An ABSENT mimetype is
//!     UNKNOWN rather than a lie, so it passes here and is caught by the byte
//!     sniff in `rooms::media_fetch_mxc`.
//!   * shortcodes, bodies, pack names and attribution are bounded and stripped
//!     of control characters. They are LABELS on the C++ side and are never
//!     rendered as rich text.
//!   * the number of packs and the number of images per pack are capped, so a
//!     hostile account-data blob cannot turn one picker open into an
//!     unbounded model.
//!
//! Nothing in this module logs a shortcode, a body, a pack name or an mxc.

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

/// At most this many packs are reported in one snapshot. A user pack, a
/// handful of room packs and the active room's own packs is the real shape;
/// anything past this is a hostile or broken blob.
const MAX_PACKS: usize = 64;

/// Images per pack. Real packs run to a few dozen; the largest public ones are
/// a few hundred.
const MAX_IMAGES_PER_PACK: usize = 512;

/// A shortcode is a token, not prose. MSC2545 caps it at 100 BYTES; 64 ASCII
/// characters is stricter and, because the sanitized alphabet is ASCII-only,
/// characters and bytes are the same thing here.
const MAX_SHORTCODE_CHARS: usize = 64;

/// A sticker is a small picture. Refused before the file is read, so an
/// accidental 4K screenshot never reaches memory or the homeserver.
const MAX_STICKER_UPLOAD_BYTES: u64 = 4 * 1024 * 1024;

/// A body is the sticker's alt text — a label.
const MAX_BODY_CHARS: usize = 160;

/// Pack display name / attribution.
const MAX_PACK_NAME_CHARS: usize = 80;
const MAX_ATTRIBUTION_CHARS: usize = 160;

/// One bounded request per state read. A pack refresh is disposable and the
/// room-action pool is JOINED during sign-out, so a retrying request loop here
/// would stall shutdown (the same reasoning as `pinned.rs`).
const PACK_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// The five raster types Lightning accepts anywhere. Mirrors
/// `rooms::sniff_image_mime`'s outputs exactly and deliberately: a declared
/// type outside this set is refused before the media is ever requested, and
/// the bytes are sniffed against the same set when they arrive.
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

/// A shortcode is the KEY of the `images` map.
///
/// MSC2545 is explicit about the alphabet: a shortcode MUST match
/// `[a-zA-Z0-9-_]+`, MUST NOT exceed 100 bytes, and MUST NOT contain colons
/// (the MSC says so specifically, to avoid clashing with the `:code:` UI
/// convention). So this is not an invented rule — it is the MSC's own, applied
/// as a REPAIR rather than a rejection: an out-of-alphabet character becomes
/// `_`, runs of `_` collapse, and leading/trailing separators are trimmed.
///
/// Repairing rather than rejecting matters on the READ side: packs in the wild
/// do carry shortcodes the MSC would refuse (Sable writes `sticker-$eventId`,
/// which is illegal twice over — see the round notes), and dropping those
/// images would make another client's pack look empty rather than merely
/// tidied. An empty result means the entry has no usable name at all and IS
/// dropped.
pub(crate) fn sanitize_shortcode(raw: &str) -> String {
    let mut out = String::new();
    for c in raw.trim().chars() {
        if c.is_ascii_alphanumeric() || c == '-' {
            out.push(c);
        } else if !out.ends_with('_') {
            // Collapse any run of illegal characters into one separator, so
            // "a.b.c" is `a_b_c` and not `a___b___c`.
            //
            // A LITERAL `_` takes this branch too, deliberately. Once an
            // illegal character has become `_`, a replacement separator and a
            // typed one are indistinguishable in the output, so the two must
            // collapse together or "emoji_<U+1F600>_here" yields `emoji__here`
            // — a double separator that came from neither the author nor a
            // single substitution. `-` is NOT collapsed: it is never produced
            // by substitution, so a run of them is the author's own.
            out.push('_');
        }
        if out.len() >= MAX_SHORTCODE_CHARS {
            break;
        }
    }
    out.trim_matches(['-', '_']).to_owned()
}

/// True when `url` is a syntactically valid `mxc://server/mediaid`.
///
/// `MxcUri::parts()` is the authority — it is what rejects `mxc://` with no
/// media id, an empty server part, and anything that merely starts with the
/// scheme. A non-mxc string never reaches it.
pub(crate) fn is_valid_mxc(url: &str) -> bool {
    if !url.starts_with("mxc://") {
        return false;
    }
    <&MxcUri>::from(url).parts().is_ok()
}

/// A DECLARED mimetype must be one Lightning can actually decode. Absent is
/// unknown, which is a different fact and is allowed through.
fn mimetype_allowed(declared: Option<&str>) -> bool {
    match declared {
        None => true,
        Some(m) => ALLOWED_MIMETYPES.contains(&m.trim().to_ascii_lowercase().as_str()),
    }
}

// ---------------------------------------------------------------------------
// Pack model
// ---------------------------------------------------------------------------

/// Usage as MSC2545 defines it. Unknown values are IGNORED rather than
/// treated as a third state — a pack from a future client must not become
/// invisible here.
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

/// MSC2545's inheritance rule, written out once so both call sites agree:
/// the IMAGE's own usage wins when it declares any; otherwise the PACK's;
/// and an empty set at both levels means the image is usable as BOTH.
///
/// Returned as a concrete pair rather than a set, because "empty means both"
/// is exactly the kind of implicit rule that gets re-derived differently in a
/// second place.
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
    /// Stable identity for the C++ side: `user` for the account pack, or
    /// `room:<room_id>:<state_key>` for a room pack.
    pub id: String,
    pub display_name: String,
    pub avatar_url: String,
    pub attribution: String,
    /// `user` | `room`.
    pub source: &'static str,
    /// Empty for the user pack.
    pub room_id: String,
    /// Empty for the user pack; the state key otherwise (may legitimately be
    /// the empty string, which is the room's default pack).
    pub state_key: String,
    /// ROOM packs only: whether `im.ponies.emote_rooms` lists this pack, i.e.
    /// whether it is available OUTSIDE its own room. A room's packs are
    /// always usable inside that room, so this says nothing about there.
    /// Always false for the user pack, which is global by definition.
    pub enabled_globally: bool,
    /// ROOM packs only: whether THIS account may write
    /// `im.ponies.room_emotes` in that room — the room's own required power
    /// level for that state event, asked of the SDK. Never a role label, and
    /// false until a snapshot has actually said otherwise.
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

/// Parse one pack's CONTENT object (the `{ images, pack }` body of any of the
/// three event types). Returns `None` only when the content is not an object
/// at all; a pack with zero usable images parses to an EMPTY pack, which is a
/// different and reportable fact.
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
        // MSC2545: a room pack with no display_name defaults to the room's
        // name. The caller supplies that; the user pack passes its own label.
        .unwrap_or_else(|| one_line(fallback_name, MAX_PACK_NAME_CHARS));

    // A pack avatar is displayed on a tab — the same untrusted-URL rule as
    // any image. A non-mxc avatar is dropped, not passed through.
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
        // BTreeMap so the order a picker shows is stable across refreshes.
        // A JSON object has no defined order and serde_json preserves input
        // order only with the `preserve_order` feature, which is not enabled;
        // sorting explicitly means the same pack never reshuffles under the
        // user's pointer between one open and the next.
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

            // The one unconditional requirement in MSC2545's PackImage.
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
                // The SVG case, and anything else this client cannot decode.
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
        // Both are properties of the ACCOUNT's relationship to the pack, not
        // of the pack's content, so the caller sets them after parsing.
        enabled_globally: false,
        can_manage: false,
        images,
    })
}

/// Parse `im.ponies.emote_rooms` into the (room id, state key) pairs it
/// enables. Invalid room ids are dropped rather than carried as strings.
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

/// Read every pack available to this account, and emit one `sticker_packs`
/// snapshot.
///
/// Sources, in the order they are presented:
///   1. `im.ponies.user_emotes` — the account's own pack. Always available.
///   2. the ACTIVE room's own `im.ponies.room_emotes` packs (all state keys).
///      MSC2545 makes a room's packs available inside that room without any
///      opt-in; `emote_rooms` is what makes them available *elsewhere*.
///   3. every (room, state key) named in `im.ponies.emote_rooms`, skipping
///      any already collected in step 2.
///
/// `room_id` may be empty — then step 2 is skipped and the snapshot carries
/// the globally available packs only.
///
/// Account data is read from the STORE first and the server second. Sliding
/// sync does deliver global account data, so the store is usually populated;
/// the server read is what makes a cold start correct rather than empty.
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
        // Read FIRST, because it answers `enabled_globally` for the ACTIVE
        // room's packs too — those are available inside their room whatever
        // this says, and the flag is what a "use these everywhere" control
        // reflects.
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
        // `can_manage` is reported for the ROOM, on the snapshot itself, not
        // only per pack — a room with NO pack yet has no pack row to hang it
        // on, and without it the room's FIRST pack could never be created.
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
                // It is in `emote_rooms` by construction — that is how it got
                // into this loop at all.
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

/// Whether THIS account may write `im.ponies.room_emotes` in `room_id`.
///
/// Offer policy is the room's OWN required power level for that state event,
/// asked of the SDK — never a role label and never "is an admin" (the
/// `banner.rs` precedent). A room we cannot resolve, or a membership we
/// cannot read, answers FALSE: an unknown permission must never be presented
/// as permission.
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

/// Turn one ROOM pack on or off in `im.ponies.emote_rooms` — "use this room's
/// stickers everywhere".
///
/// This is ACCOUNT DATA, not room state: it records the reader's own choice
/// and needs no power level. It is also the only one of MSC2545's three
/// events that says anything about availability OUTSIDE a room — a room's
/// packs are always usable inside that room, whatever this holds, which is
/// why turning a pack off does not make it vanish from its own room.
///
/// Read-modify-write against the SERVER copy, never the store: account data
/// has no server-side merge, so writing back a stale blob would silently drop
/// every pack another device enabled since. Same reasoning as
/// `add_to_user_pack`.
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

/// The pure `im.ponies.emote_rooms` read-modify-write, split out from the
/// network so it can be tested. Takes whatever the server returned — which
/// may be anything at all, since this is another client's blob — and returns
/// the content to write back.
pub(crate) fn apply_emote_rooms_change(
    existing: Value,
    room_id: &str,
    state_key: &str,
    enabled: bool,
) -> Result<Value, String> {
    let mut content = existing;
    // A non-object (or a `rooms` that is a string, a number, an array) is a
    // blob this client cannot merge into. It is REPLACED rather than refused:
    // refusing would leave the user permanently unable to turn a pack on,
    // and there was no valid selection in it to preserve.
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
        // MSC2545's value is an ImagePackRoomContent, which models NO
        // overrides at all in ruma 0.34 — presence of the key IS the
        // enablement. An empty object is therefore the whole payload.
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
        // A room whose last pack was turned off leaves no empty husk behind:
        // another client reading this sees "no packs from that room", which
        // is what the user asked for and is also what the absent key already
        // means.
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

/// Store first, then the server. `None` means "no such event" OR "could not
/// be read" — the caller treats both as "this source contributed nothing",
/// which is correct: a pack that cannot be read is not a pack with no images.
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

/// Every `im.ponies.room_emotes` state event in one room, as
/// (state key, content, room name). The room name is the MSC's documented
/// default display name for a room pack that does not name itself.
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
        // An empty content object is how Matrix RETIRES a state event —
        // it is a removed pack, not an empty one.
        if content.get("images").is_none() {
            continue;
        }
        out.push((state_key, content.clone(), name.clone()));
    }
    out
}

/// One room pack by state key, for a room the account may not have open.
/// The state store is consulted first; sliding sync does not deliver custom
/// state types, so a store miss is the ORDINARY case and the bounded `/state`
/// read is what actually answers (the same reasoning as `banner.rs`).
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
// Sending a sticker
// ---------------------------------------------------------------------------

/// Send one `m.sticker` to a room or a thread.
///
/// The media is the pack's OWN `mxc://`. That is what MSC2545 packs are and
/// what every other client sends — the pack image is already Matrix media, so
/// there is nothing to upload and nothing to re-encode. **Consequence, stated
/// plainly rather than glossed:** in an encrypted room the sticker EVENT is
/// encrypted by the SDK exactly like every other event, but the BITMAP it
/// points at is ordinary unencrypted media, because that is what a shared pack
/// is. This is inherent to MSC2545, not a Lightning choice, and it is why the
/// picker must never present a pack sticker as private content.
///
/// The event itself goes through the SDK timeline (`Timeline::send`), so the
/// local echo, the send queue and Retry all work exactly as they do for a
/// message — and the SDK, not Lightning, attaches the `m.thread` relation when
/// a thread root is given (matrix-sdk-ui 0.18 handles
/// `AnyMessageLikeEventContent::Sticker` in that path; see `timeline/mod.rs`).
/// Nothing here builds a relation by hand, so a thread sticker can never leak
/// into the main timeline (CLAUDE.md §8).
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
    // The same refusal as the parser, re-applied at the edge: a caller that
    // somehow assembled a non-mxc source must not be able to send it.
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

    let mut info = ImageInfo::new();
    if !mimetype.is_empty() {
        info.mimetype = Some(mimetype);
    }
    info.width = UInt::new(width);
    info.height = UInt::new(height);
    info.size = UInt::new(size);

    let content = StickerEventContent::new(body, info, OwnedMxcUri::from(url));

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

/// Add one image to `im.ponies.user_emotes` ("steal this sticker").
///
/// Read-modify-write of the account data event. The read is from the SERVER,
/// never the store: the store may be stale, and writing a stale pack back
/// would silently delete every image added since. That is the same reasoning
/// as `Room::pin_event`'s read-modify-send, applied to account data, which has
/// no server-side merge of its own.
///
/// Duplicate policy (Lightning's own choice, documented in the round report —
/// see the note about Sable): an image whose `url` is ALREADY in the pack is
/// reported as `duplicate` and nothing is written, so pressing the button
/// twice cannot fill the pack with copies of one sticker. A shortcode that is
/// taken by a DIFFERENT url gets a numeric suffix (`cat`, `cat-2`, `cat-3`),
/// so a name collision never overwrites an existing image.
#[allow(clippy::too_many_arguments)]
/// Upload a LOCAL image file and add it to this account's own pack.
///
/// This is the only way to BOOTSTRAP a pack. Every other route into one —
/// "add to my stickers" on a sticker somebody sent — needs an mxc that
/// already exists, so a user with no packs and nobody sending them stickers
/// had no way in at all. Reported as "i dont have any sticker packs to test
/// it".
///
/// Uploads first, then reuses `add_to_user_pack_inner`, so the pack write,
/// the dedupe, the shortcode collision handling and the cap are ONE
/// implementation shared with the save path rather than a second copy.
///
/// The MIME is sniffed from the bytes, never taken from the file name, and
/// the sniffer is the shared one — so this cannot accept a format the rest of
/// the client refuses, and it refuses SVG.
pub(crate) fn upload_to_user_pack(
    bridge: &RustClient,
    op_id: u64,
    shortcode: String,
    body: String,
    local_path: String,
) -> Result<(), String> {
    // Checked before the file is read, so an absurd file is refused without
    // being pulled into memory.
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
            // Dimensions are left at 0: the pack entry's `info` is advisory,
            // and decoding an image in the bridge purely to fill it would add
            // an image decoder to a path that does not need one. Clients size
            // a sticker from the picture itself.
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

/// Add one image to a ROOM's `im.ponies.room_emotes` pack ("add to this
/// room's stickers").
///
/// This is the one part of MSC2545 that is ROOM STATE rather than account
/// data, so unlike `add_to_user_pack` it is POWER-LEVEL GATED. Two rules
/// follow from that and neither may be softened:
///
///   * the gate is the room's OWN required level for `im.ponies.room_emotes`,
///     asked of the SDK (`can_send_state`) — never a role label, never "is an
///     admin", and FALSE when the membership cannot be read. The server would
///     refuse anyway; asking first is what lets the UI stop offering an action
///     that cannot work, and the check here is what stops a caller that did
///     not ask.
///   * nothing is applied optimistically anywhere. The write completes, the
///     caller re-reads the authoritative pack, so a refusal cannot leave a
///     picker showing an image the room does not have.
///
/// Read-modify-write of the CURRENT state event, exactly as the user-pack
/// path does: a concurrent edit by another moderator must not be clobbered by
/// a stale copy of ours.
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
    // The gate, before anything is read or built.
    if !can_manage_room_packs(client, room_id).await {
        return Err("forbidden".to_owned());
    }

    // The CURRENT pack. A miss is "this room has no pack under that key yet",
    // which is a normal first use — the same shape as the user pack's 404.
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

/// The pure "add one image to a pack content object" transform, shared by the
/// user-pack and room-pack writers so the two can never disagree about
/// duplicate policy, the size cap, or shortcode collisions. Split out from
/// the network so it is testable.
///
/// Returns the content to write plus the shortcode the image actually got —
/// which may carry a numeric suffix the caller did not ask for.
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
    // Already present, by IDENTITY (the mxc), not by name.
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
    // The saved image is a STICKER. Saying so explicitly keeps it out of an
    // emoticon completion list, where a 512px picture inline in a sentence is
    // not what the user asked for.
    entry.insert("usage".to_owned(), json!(["sticker"]));

    images.insert(code.clone(), Value::Object(entry));
    Ok((content, code))
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

    // SERVER read. A 404 is "no pack yet", which is a normal first use.
    let existing = match client.account().fetch_account_data(ty.clone()).await {
        Ok(Some(raw)) => serde_json::from_str::<Value>(raw.json().get())
            .unwrap_or_else(|_| json!({})),
        Ok(None) => json!({}),
        Err(err) => return Err(classify_room_error(&err.to_string()).to_owned()),
    };

    // ONE transform, shared with the room-pack writer, so duplicate policy,
    // the size cap and shortcode collisions cannot drift between the two.
    let (mut content, code) = add_image_to_pack_content(
        existing, requested, url, body, mimetype, width, height, size,
    )?;

    // Give a brand-new pack a name, so it does not show up as an unnamed tab
    // in every other client. Deliberately NOT done for a room pack: MSC2545
    // says a room pack with no display_name defaults to the ROOM's name, and
    // stamping "Stickers" over that would be worse than the default.
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
        // The tracking-pixel case: an https URL on a picker tile would fire
        // one request per pack listing, to a host the pack author chose.
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
                // Absent mimetype is UNKNOWN, not a lie — it passes here and
                // is caught by the byte sniff when the media arrives.
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
                // An unknown usage value is IGNORED, leaving the set empty,
                // so the pack's usage applies — a future client's pack must
                // not become invisible.
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

        // And with no usage at ALL, at either level, everything is both.
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
        // Runs of illegal characters collapse to ONE separator.
        assert_eq!(sanitize_shortcode("a...b"), "a_b");
        assert_eq!(sanitize_shortcode("emoji_\u{1F600}_here"), "emoji_here");
        // Sable writes this, and it is illegal twice over (`$`, and a colon
        // in room versions 1-2). We repair it rather than drop the image.
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
        // MSC2545: a room pack with no display_name defaults to the room's
        // name. The caller passes it; the parser must use it.
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
        // "The user has a pack with nothing in it" and "there is no pack"
        // are different facts and the UI says different things about them.
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
        // A saved image is a STICKER, said explicitly, so it never lands in
        // an emoticon completion list.
        assert_eq!(entry["usage"], json!(["sticker"]));
        assert_eq!(entry["info"]["mimetype"], json!("image/png"));
        assert_eq!(entry["info"]["w"], json!(128));
        // `body` equals the shortcode, which MSC2545 already defaults to —
        // writing it again would be noise.
        assert!(entry.get("body").is_none());
    }

    #[test]
    fn the_same_mxc_twice_is_a_duplicate_by_identity_not_by_name() {
        let (content, _) =
            add(json!({}), "cat", "mxc://example.org/a").expect("adds");
        // A DIFFERENT name for the same image is still the same image.
        assert_eq!(
            add(content.clone(), "kitty", "mxc://example.org/a"),
            Err("duplicate".to_owned())
        );
        // A different image under a taken name gets a suffix rather than
        // overwriting what is there.
        let (after, code) =
            add(content, "cat", "mxc://example.org/b").expect("adds");
        assert_eq!(code, "cat-2");
        assert_eq!(after["images"]["cat"]["url"], json!("mxc://example.org/a"));
        assert_eq!(after["images"]["cat-2"]["url"], json!("mxc://example.org/b"));
    }

    #[test]
    fn an_unnamed_image_still_gets_a_usable_shortcode() {
        // Nothing survives sanitizing: the pack still needs a key.
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
        // A full pack must not silently discard what the user asked to keep.
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
        // The whole point of the read-modify-write: a concurrent edit by
        // another device or another moderator must survive ours.
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
        // And the pack's own metadata is untouched — a room pack must not be
        // renamed by someone adding one image to it.
        assert_eq!(after["pack"]["display_name"], json!("Theirs"));
    }

    #[test]
    fn enabling_a_room_pack_preserves_every_other_selection() {
        // The whole point of reading the SERVER copy: another device's
        // choices must survive our write.
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
        // Presence of the key IS the enablement; the value carries nothing.
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
        // One of two removed: the room stays, with the other pack.
        assert_eq!(after["rooms"]["!a:example.org"], json!({ "": {} }));

        let empty = apply_emote_rooms_change(
            after, "!a:example.org", "", false,
        )
        .expect("applies");
        // Its last pack removed: the room key goes too. An empty husk and an
        // absent key mean the same thing, and only one of them is tidy.
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
        // A room already in the map may gain a second pack: the cap is on
        // ROOMS, and refusing here would be a cap on nothing.
        let after =
            apply_emote_rooms_change(full, "!r0:example.org", "second", true)
                .expect("applies");
        assert_eq!(
            after["rooms"]["!r0:example.org"].as_object().unwrap().len(),
            2
        );
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
        // The declared-type allowlist and the magic-byte sniffer must accept
        // exactly the same set, or a pack image passes one gate and fails the
        // other for reasons nobody can see. This pins them together.
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
}
