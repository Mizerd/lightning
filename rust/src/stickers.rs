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

// ---------------------------------------------------------------------------
// Event types and bounds
// ---------------------------------------------------------------------------

pub(crate) const USER_EMOTES: &str = "im.ponies.user_emotes";
pub(crate) const ROOM_EMOTES: &str = "im.ponies.room_emotes";
pub(crate) const EMOTE_ROOMS: &str = "im.ponies.emote_rooms";

/// Matrix v1.19's stable identifiers (MSC2545 merged): room-pack state and
/// the global subscription account data. There is NO stable counterpart of
/// `im.ponies.user_emotes`; the account's own legacy pack stays a legacy
/// record.
pub(crate) const STABLE_ROOM_PACK: &str = "m.room.image_pack";
pub(crate) const STABLE_PACK_ROOMS: &str = "m.image_pack.rooms";

/// Which wire dialect a ROOM pack record lives in. A stable and a legacy
/// record under the same (room, state key) are two remote records; which one
/// the standard view shows is `resolve_room_packs`'s rule.
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
    /// Refuses anything else, so a typo cannot pick a dialect.
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

/// Max packs in one snapshot; more means a hostile or broken blob.
const MAX_PACKS: usize = 64;

/// Max images per pack (large public packs have a few hundred).
const MAX_IMAGES_PER_PACK: usize = 512;

/// Shortcode length bound: Matrix v1.19's grammar is `[A-Za-z0-9_-]{1,100}`,
/// ASCII, so bytes equal chars. Applies to NEW and renamed keys only: a
/// received key outside the grammar is kept as identity (see `PackImage`).
const MAX_SHORTCODE_BYTES: usize = 100;

/// A received `images` key longer than this is dropped as hostile. Not a
/// protocol number: it only keeps one absurd key from crossing the FFI.
const MAX_RAW_KEY_BYTES: usize = 1024;

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

/// True when `key` is a shortcode under Matrix v1.19's grammar:
/// `^[A-Za-z0-9_-]{1,100}$`, case-sensitive. The ONE validator for new and
/// renamed keys; never used to rewrite a received key.
pub(crate) fn is_valid_shortcode(key: &str) -> bool {
    !key.is_empty()
        && key.len() <= MAX_SHORTCODE_BYTES
        && key
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// The key a NEW image gets for a requested name: the request itself when it
/// is already a valid shortcode (exactly, case and `__` included), otherwise
/// a sanitized suggestion. Never applied to a key that already exists.
pub(crate) fn propose_shortcode(requested: &str) -> String {
    let trimmed = requested.trim();
    if is_valid_shortcode(trimmed) {
        return trimmed.to_owned();
    }
    sanitize_shortcode(trimmed)
}

/// Turn arbitrary text (a file name, a sticker body) into a SUGGESTED
/// shortcode: illegal characters become `_`, runs collapse, and
/// leading/trailing separators are trimmed. A suggestion only: received keys
/// are identity and are never passed through this (an image keyed
/// `hello__world` must stay `hello__world`).
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
        if out.len() >= MAX_SHORTCODE_BYTES {
            break;
        }
    }
    out.trim_matches(['-', '_']).to_owned()
}

/// True when `url` is a valid `mxc://server/mediaid`, as judged by
/// `MxcUri::parts()`.
pub(crate) fn is_valid_mxc(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("mxc://") else {
        return false;
    };
    // The media id grammar is ASCII alphanumerics, `_` and `-` (no `/`, `?`,
    // `#`, `..`): checked here explicitly rather than trusting a parser's
    // leniency; the server name is ruma's to judge.
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
    parse_usage_detail(value).0
}

/// The known usages, and whether the array named anything at all. An array
/// of ONLY unknown values is not an absent array: under the stable rules it
/// must not become "everything" (PACK-PRO-012).
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

/// Matrix v1.19's rule: usage is PACK-wide; missing or empty means every
/// usage; known values restrict; only-unknown values allow none of the two
/// this client knows (the pack is shown, flagged, not silently broadened).
/// Returns (emoticon, sticker, unsupported).
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
    /// The exact `images` map key: the image's identity. Every mutation names
    /// this, never the display text, so a received `hello__world` or a 65-100
    /// byte key is removed or renamed as itself.
    pub key: String,
    /// Whether `key` satisfies the v1.19 grammar. An invalid received key is
    /// still shown (and removable/renamable) but cannot be typed as `:key:`.
    pub key_valid: bool,
    /// Display text: `key` itself when valid, otherwise a one-line, bounded
    /// rendering of it (possibly empty). A label, never an identity.
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
    /// Opaque id for C++: `user`, or `room_pack_id(namespace, room, key)`.
    pub id: String,
    /// `stable` (`m.room.image_pack`) or `legacy` (`im.ponies.*`; the
    /// account's own pack is always legacy). Every write goes back to this
    /// dialect unless the user deliberately upgrades.
    pub namespace: &'static str,
    /// A legacy record exists under the same (room, key) and is hidden by
    /// this present stable one (stable is authoritative for the standard
    /// view; the twin is reported, never merged).
    pub legacy_twin: bool,
    /// Listed only in the legacy `im.ponies.emote_rooms` while a stable
    /// `m.image_pack.rooms` document exists and is authoritative, so it is
    /// NOT active everywhere; the manager can offer to carry it over.
    pub legacy_subscribed: bool,
    /// The pack's usage names only values this client does not know.
    pub usage_unsupported: bool,
    /// False for a subscribed reference whose pack could not be read; then
    /// `unavailable` names why and `images` is empty.
    pub available: bool,
    pub unavailable: String,
    /// Reached through the active room's canonical space ancestry (usable in
    /// that room because it sits in the space), not subscribed.
    pub from_space: bool,
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
            "namespace": self.namespace,
            "legacy_twin": self.legacy_twin,
            "legacy_subscribed": self.legacy_subscribed,
            "usage_unsupported": self.usage_unsupported,
            "available": self.available,
            "unavailable": self.unavailable,
            "from_space": self.from_space,
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

/// Parse one LEGACY pack (`im.ponies.*`, per-image usage overrides).
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

/// Parse one pack's `{ images, pack }` content in its dialect. `None` only
/// when it is not an object; a pack with no usable images is an empty pack,
/// not a failure. A stable pack's usage is pack-wide: a per-image `usage`
/// some older client left behind is preserved on write but has no meaning
/// here.
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

            // The raw key is the identity and is kept exactly; it is only
            // dropped when absurdly long. A key outside the grammar is shown with
            // `key_valid: false` rather than silently repaired into a different
            // (possibly colliding) name.
            if raw_code.len() > MAX_RAW_KEY_BYTES {
                continue;
            }
            let key_valid = is_valid_shortcode(raw_code);
            let shortcode = if key_valid {
                raw_code.clone()
            } else {
                one_line(raw_code, MAX_SHORTCODE_BYTES)
            };

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

            let (is_emoticon, is_sticker) = match namespace {
                Namespace::Stable => (stable_usage.0, stable_usage.1),
                Namespace::Legacy => {
                    let image_usage = parse_usage(entry.get("usage"));
                    resolve_usage(&image_usage, &pack_usage)
                }
            };

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
// Subscriptions (stable `m.image_pack.rooms`, legacy `im.ponies.emote_rooms`)
// ---------------------------------------------------------------------------

/// The references that make packs usable everywhere, and where they came
/// from. Policy (documented in docs/feature-contracts.md, "Image packs"):
///
///   * a PRESENT stable document is authoritative, including an empty one (an
///     intentional "unsubscribe from everything" must not be refilled from
///     legacy data on every refresh). References only the legacy document
///     holds are reported as `legacy_only`, not activated.
///   * a CONFIRMED-ABSENT stable document leaves the legacy references in
///     force, exactly as before stable support existed.
///   * an UNREADABLE stable document is not an absence: the legacy references
///     are shown so the picker is not emptied by a timeout, and the snapshot
///     says the stable side could not be read.
#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct Subscriptions {
    pub active: Vec<(String, String)>,
    /// "stable" | "legacy" | "none".
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

/// The stable subscription document to WRITE for one toggle. Starts from the
/// server's current stable document; when the server confirmed there is none
/// yet, the first stable document carries over every legacy reference
/// verbatim (leaf objects included) so creating it does not silently switch
/// off choices the user made in older clients. An unreadable stable document
/// writes nothing.
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

// ---------------------------------------------------------------------------
// Room pack records
// ---------------------------------------------------------------------------

/// One room-pack state record as read (store or `/state`).
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PackRecord {
    pub namespace: Namespace,
    pub state_key: String,
    pub content: Value,
    /// `origin_server_ts`, 0 when unknown (stripped state).
    pub ts: u64,
}

/// A room pack the standard view shows.
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

/// Record-level precedence for one room, pure. Per state key:
///   * a present stable record is authoritative; a legacy twin is hidden and
///     reported (`legacy_twin`), never merged into it;
///   * a stable record that is cleared or redacted (`{}`, no `images`) hides
///     the key entirely: stale legacy content must not come back through it
///     (PACK-MIG-005);
///   * otherwise a legacy record with images is shown as legacy.
///
/// Records of one (namespace, key) seen twice keep the newer one.
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

/// A full-room `/state` scan's pack records, per (account, room), so opening
/// the picker in a room twice does not fetch the room's whole state twice.
/// Dropped by our own writes and by sync delivering a pack event there.
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

/// Forget everything cached about one room's packs (scan and remembered
/// absences): a write of ours or a sync change landed there.
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

/// Every pack record of both dialects in one room: the store, and with `deep`
/// a (cached) full `/state` read. Sliding sync never puts these custom types
/// in `required_state`, so the store holds only what passed through a synced
/// timeline; `deep` is for the moments a user asks (picker, manager), never
/// for plain room navigation. Returns (records, room name, scan_failed).
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

// ---------------------------------------------------------------------------
// Canonical space ancestry (Matrix v1.19: packs of a room's canonical spaces)
// ---------------------------------------------------------------------------

/// One `m.space.parent` claim in a room, with the facts its validity needs.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct ParentClaim {
    pub space_id: String,
    pub canonical: bool,
    pub has_via: bool,
    /// The parent space lists this room as a child (non-empty `via`).
    pub reciprocal: bool,
    /// The claim's sender may add children to the parent space (power to send
    /// `m.space.child` there), so the claim is valid without reciprocity.
    pub sender_authorised: bool,
}

/// The canonical parent under the spaces rules, pure: a claim counts only
/// when it is `canonical` with a `via`, AND either the parent lists the room
/// as a child or the claim's sender has the authority to make it one. Among
/// several, the lowest space id in code-point order wins (Rust `String` order
/// on UTF-8 is code-point order). A claim from an unrelated room never counts
/// (PACK-PRO-020/021).
pub(crate) fn pick_canonical_parent(claims: &[ParentClaim]) -> Option<String> {
    claims
        .iter()
        .filter(|c| c.canonical && c.has_via && (c.reciprocal || c.sender_authorised))
        .map(|c| c.space_id.clone())
        .min()
}

/// How far up the canonical chain Lightning looks. A documented resource
/// bound, not a protocol number.
const MAX_ANCESTRY_DEPTH: usize = 8;

/// The canonical ancestors of `room_id` the account is JOINED to, nearest
/// first. Bounded (`MAX_ANCESTRY_DEPTH`), cycle-safe (visited set), and it
/// stops at the first parent the account is not a member of: that space's
/// packs are not the account's to see (the module requires membership) and
/// its own parents cannot be validated. Never joins anything, never asks the
/// network: only this account's own store (PACK-PRO-022/024).
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
            break; // a cycle
        }
        if joined_room(client, &parent).is_err() {
            break; // not a member: not ours to read, and no further validation
        }
        out.push(parent.clone());
        current = parent;
    }
    out
}

// ---------------------------------------------------------------------------
// Reading packs
// ---------------------------------------------------------------------------

/// Read every pack available to this account and emit one `sticker_packs`
/// snapshot, in Matrix v1.19's source order:
///   1. the account's own legacy `im.ponies.user_emotes` pack and every pack
///      the authoritative subscription document references (global);
///   2. the active room's own packs, stable and legacy, by
///      `resolve_room_packs` (usable inside that room without opt-in).
///
/// A subscribed reference that cannot be read stays in the snapshot as an
/// unavailable row with its reason, never silently dropped and never joined.
/// Empty `room_id` skips step 2; `deep` allows a full `/state` read of the
/// active room (see `scan_room_packs`).
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

        // ---- 0. subscriptions -------------------------------------------
        let stable_doc = read_global_account_data_typed(&client, STABLE_PACK_ROOMS).await;
        let legacy_doc = read_global_account_data(&client, EMOTE_ROOMS).await;
        let subs = effective_subscriptions(&stable_doc, legacy_doc.as_ref());
        let legacy_only: BTreeSet<(String, String)> = subs.legacy_only.iter().cloned().collect();

        // ---- 1a. the account's own (legacy) pack ------------------------
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

        // ---- 1b. globally subscribed packs --------------------------------
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

        // ---- 2. the active room's own packs -------------------------------
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

        // ---- 3. the active room's canonical space ancestry --------------
        //
        // Usable in this room because it sits in those spaces; not a
        // subscription. Same record precedence as the room itself.
        let mut ancestry_len = 0usize;
        if !room_id.is_empty() {
            let ancestors = canonical_ancestry(&client, &room_id).await;
            ancestry_len = ancestors.len();
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

/// A subscribed reference whose pack could not be read: kept visible with
/// its reason (`absent`, `not_joined`, `forbidden`, `network`, ...) so the
/// user can see and remove it; Lightning never joins a room for it.
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

/// Read a subscribed (room, key): the stable record first, the legacy one
/// only when the server CONFIRMED there is no stable record (a failed stable
/// read is not permission to show stale legacy content). A cleared stable
/// record (`{}`) is reported as absent rather than falling back.
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

/// Whether this account may write the EXACT pack event type in `room_id`,
/// per the room's own power levels, asked of the SDK (never a role label,
/// never "level 100"). `m.room.image_pack` and `im.ponies.room_emotes` can
/// require different levels, so each is asked separately.
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

/// Subscribe or unsubscribe one room pack ("use everywhere"). Account data:
/// the reader's own choice, no power level. A room's packs remain usable
/// inside that room regardless.
///
/// Writes the STABLE `m.image_pack.rooms` (see `plan_stable_subscription_write`
/// for the first-document rule). For a LEGACY pack the same change is also
/// mirrored into `im.ponies.emote_rooms`, so older clients on the user's other
/// devices keep seeing it; that second write is reported separately
/// (`mirror`: `ok` | `failed` | `none`) and never turns a saved stable write
/// into a reported failure. Two writes are not one transaction.
///
/// Read-modify-write against the server copy, never the store. This narrows
/// but cannot close the race with another device writing at the same moment:
/// account data has no compare-and-swap.
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

/// The pure subscription read-modify-write, testable offline. Works on both
/// documents (same shape). Every other room, every other state key and every
/// leaf object survives: enabling an already-present key keeps its leaf (the
/// leaf is reserved for future fields), and only the toggled key is touched.
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
        // The key's presence is the subscription; its (reserved) leaf object is
        // kept as found.
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
        // An empty room object has no stable meaning (it is NOT "every pack"),
        // so a room whose last reference went is dropped rather than left.
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
    // The sync echo of this write has not landed; a remembered "not found"
    // would hide it (AbsentAnswers).
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
    // Seeding a first stable document from an UNREADABLE legacy one could hide
    // every legacy choice; refuse instead.
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

/// Account-data types the server answered "not found" for, per account.
///
/// Every room open asks for the sticker packs, which reads `im.ponies.emote_rooms`
/// and `im.ponies.user_emotes`; an account that never used stickers gets a 404
/// for both, every time (live check 2026-10-01: 9 opens, 9 pairs of 404s). The
/// answer cannot change without account data arriving, so it is remembered. It
/// is NOT consulted before the local store: sync delivering the type puts it in
/// the store, which is read first, so the next read finds it and drops the mark.
/// Our own writes drop it too (the sync echo has not landed yet). A mark also
/// expires, so a sync that never carries the type cannot hide it for a whole
/// session.
pub(crate) struct AbsentAnswers {
    marks: std::sync::Mutex<std::collections::HashMap<(String, String), std::time::Instant>>,
}

/// How long "the server has none" is believed without a sync saying otherwise.
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

    /// Whether `event_type` is believed absent for `account` at `now`.
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

    /// Drop every mark of `account` whose key starts with `prefix`.
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

/// What asking the server for one account-data type came back as.
pub(crate) enum ServerAnswer {
    Found(Value),
    /// M_NOT_FOUND: the account has none.
    Absent,
    /// Anything else: says nothing about whether it exists.
    Failed,
}

/// The decision behind `read_global_account_data`, apart from the client.
/// `store` is the local read (sync's copy), `ask` the server request, run only
/// when the store has nothing and the type is not believed absent.
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
        // Sync (or a write of ours) has it: whatever was believed is stale.
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

/// A write of ours to `event_type`: the sync echo has not landed, so a
/// remembered "not found" would hide it.
fn forget_own_write(client: &matrix_sdk::Client, event_type: &str) {
    absent_answers().forget(&account_key(client), event_type);
}

/// The server's answer to an account-data GET, as the memory sees it. Only a
/// successful "no such event" (`fetch_account_data` maps M_NOT_FOUND to
/// `Ok(None)`) is Absent.
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

/// An account-data GET as a `PackRead`, for writers: `fetch_account_data`
/// maps M_NOT_FOUND to `Ok(None)`.
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

/// As `read_remembering_absence`, keeping "absent" and "failed" apart.
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

/// Store first, then the server, keeping a confirmed absence apart from a
/// failure (the subscription policy depends on the difference).
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

/// Store first, then the server. `None` means absent or unreadable; either
/// way the source contributes nothing. A server "not found" is remembered
/// (`AbsentAnswers`), so the next read asks nothing; an error is not.
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
    /// The coarse class (`forbidden`, `network`, `malformed`, ...).
    Failed(String),
}

/// Classify a state GET: `Ok(body)` is the event content as text, `Err` the
/// error's display text.
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

/// The content an ADD starts from: an existing pack, or a new empty one only
/// when the server said there is none. Any failed read refuses the write.
pub(crate) fn base_for_add(read: PackRead) -> Result<Value, String> {
    match read {
        PackRead::Found(content) => Ok(content),
        // A schema-valid empty pack: `images` is required (v1.19).
        PackRead::Absent => Ok(json!({ "images": {} })),
        PackRead::Failed(category) => Err(category),
    }
}

/// The content an EDIT starts from: it edits something that must exist.
pub(crate) fn base_for_edit(read: PackRead) -> Result<Value, String> {
    match read {
        PackRead::Found(content) => Ok(content),
        PackRead::Absent => Err("not_found".to_owned()),
        PackRead::Failed(category) => Err(category),
    }
}

/// One room pack by state key, and the room's name (the MSC's default display
/// name). Sliding sync does not deliver custom state types, so a store miss
/// is normal and the bounded `/state` read answers (as in `banner.rs`).
async fn read_one_room_pack(
    client: &matrix_sdk::Client,
    room_id: &str,
    state_key: &str,
    namespace: Namespace,
) -> (PackRead, String) {
    use matrix_sdk::ruma::api::client::state::get_state_event_for_key;

    let Ok(room) = joined_room(client, room_id) else {
        return (PackRead::Failed("unknown_room".to_owned()), String::new());
    };
    let name = room.name().unwrap_or_default();
    let event_type = namespace.room_type();

    if let Ok(Some(raw)) = room
        .get_state_event(StateEventType::from(event_type), state_key)
        .await
    {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        if let Ok(value) = serde_json::from_str::<Value>(&json) {
            if let Some(content) = value.get("content") {
                if content.get("images").is_some() {
                    return (PackRead::Found(content.clone()), name);
                }
            }
        }
    }

    // A server "no such event" is remembered per (account, room, type, key),
    // so a legacy-only subscription does not ask for its missing stable twin
    // on every refresh; sync delivering either type there, or a write of ours,
    // drops the mark (`forget_room`).
    let account = account_key(client);
    let memo = format!("state|{room_id}|{event_type}|{state_key}");
    if absent_answers().is_absent(&account, &memo, std::time::Instant::now()) {
        return (PackRead::Absent, name);
    }
    let config = RequestConfig::new()
        .disable_retry()
        .timeout(PACK_REQUEST_TIMEOUT);
    let request = get_state_event_for_key::v3::Request::new(
        room.room_id().to_owned(),
        StateEventType::from(event_type),
        state_key.to_owned(),
    );
    let read = match client.send(request).with_request_config(config).await {
        Ok(response) => classify_pack_read(Ok(response.event_or_content.get())),
        Err(err) => classify_pack_read(Err(err.to_string())),
    };
    if read == PackRead::Absent {
        absent_answers().mark_absent(&account, &memo, std::time::Instant::now());
    }
    (read, name)
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
    usage: String,
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
                &usage,
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

/// Add one image to a room pack, in the pack's own dialect (`namespace`): a
/// legacy pack is edited as legacy, never silently moved. Room state, so
/// power-level gated:
///
///   * the gate is the room's own required level for that EXACT event type,
///     via the SDK (`can_send_state`), and false when membership cannot be
///     read. The server would refuse anyway; this stops callers that did not
///     ask.
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
    // Gate before anything is read or built.
    if !can_manage_room_packs(client, room_id, namespace).await {
        return Err("forbidden".to_owned());
    }

    // Only the server's own "not found" is a first use; a failed read writes
    // nothing (PACK-LTN-017).
    let existing =
        base_for_add(read_one_room_pack(client, room_id, state_key, namespace).await.0)?;

    // A stable image has no per-image usage (it is pack-wide in v1.19); a
    // legacy room image keeps the conservative "sticker".
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

/// The `usage` array written for an added image. `emoticon` -> custom emoji,
/// `both` -> either, anything else -> sticker.
pub(crate) fn usage_value(usage: &str) -> Value {
    match usage {
        "emoticon" => json!(["emoticon"]),
        "both" => json!(["sticker", "emoticon"]),
        _ => json!(["sticker"]),
    }
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
    // Exact, case-sensitive: `Cat` and `cat` are two different keys.
    while images.contains_key(&code) {
        let suffix = format!("-{n}");
        // Keep the suffixed key inside the 100-byte grammar (ASCII, so any byte
        // index is a char boundary).
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
    // A saved sticker stays out of emoticon completion; a saved custom emoji
    // stays out of the sticker picker. Anything unrecognised is a sticker,
    // which is the conservative one (it never appears inline in a message box).
    // An EMPTY usage writes no per-image field: stable packs (usage is
    // pack-wide in Matrix v1.19).
    if !usage.is_empty() {
        entry.insert("usage".to_owned(), usage_value(usage));
    }

    images.insert(code.clone(), Value::Object(entry));
    Ok((content, code))
}

/// One pack edit, independent of where the pack lives (account data or room
/// state). The rules live in the pure transforms; this carries the intent
/// to the one shared writer.
pub(crate) enum PackEdit {
    /// `key` / `from` are exact `images` keys, never display text.
    RemoveImage { key: String },
    RenameImage { from: String, to: String },
    SetName { name: String },
    /// Pack-wide usage (`emoticon` | `sticker` | `both`), stable packs only.
    SetUsage { usage: String },
    /// One image's description (`body`): what screen readers and clients that
    /// cannot show the image read. Empty removes it (the shortcode then
    /// stands in).
    SetBody { key: String, body: String },
    /// Retire the pack (no delete verb exists for account data or state; not a
    /// redaction: the events stay in history and uploaded media is untouched).
    /// Legacy: an empty object, the convention older clients read as removed.
    /// Stable: the schema-valid `{"images":{}}`, keeping the pack metadata so
    /// the retired pack is still recognisable.
    DeletePack,
}

impl PackEdit {
    /// Apply to a content object. Returns the content to write and the applied
    /// shortcode for a rename (empty otherwise).
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

/// Edit a pack: the user's own (empty `room_id`) or the room pack under
/// `state_key`. One writer for both. Read-modify-write, so concurrent edits
/// from other moderators or devices are not clobbered.
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
    // The sync echo of this write has not landed; a remembered "not found"
    // would hide it (AbsentAnswers).
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
    // The same gate as the add path, for the exact event type; deleting a
    // room's pack is more destructive than adding to it.
    if !can_manage_room_packs(client, room_id, namespace).await {
        return Err("forbidden".to_owned());
    }
    let existing =
        base_for_edit(read_one_room_pack(client, room_id, state_key, namespace).await.0)?;
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

/// The pure "create" decision: only a confirmed absence creates; an existing
/// record (even a retired one) is never overwritten by a create.
pub(crate) fn plan_create(read: PackRead, name: &str, usage: &str) -> Result<Value, String> {
    match read {
        PackRead::Absent => Ok(new_stable_pack_content(name, usage)),
        PackRead::Found(_) => Err("pack_exists".to_owned()),
        PackRead::Failed(category) => Err(category),
    }
}

/// Create a new, empty STABLE room pack under `state_key` in `room_id`.
/// Answers on `sticker_pack_edit_result` with `shortcode` set to the state
/// key. The caller chooses the key (unique among known packs); a key that
/// already holds a record is refused with `pack_exists`.
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
    // General event limit for a state key; not a shortcode rule.
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
            let read = read_one_room_pack(&client, &room_id, &state_key, Namespace::Stable)
                .await
                .0;
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

/// Set or clear one image's `body`, keeping every other field of the entry.
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

/// Set a stable pack's pack-wide usage. `both` removes the field (absent
/// means every usage); unknown values another client wrote are dropped only
/// because the user chose a new explicit usage.
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
    // A deliberate rename means exactly the name typed: validated against the
    // grammar, never repaired into something else.
    if !is_valid_shortcode(to) {
        return Err("invalid_shortcode".to_owned());
    }
    let code = to.to_owned();
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
    usage: &str,
) -> Result<String, String> {
    let ty = GlobalAccountDataEventType::from(USER_EMOTES);

    // Server read. A 404 means no pack yet: a normal first use. Anything that
    // is not a readable object refuses the write rather than replacing it.
    let existing = base_for_add(account_data_read(
        client.account().fetch_account_data(ty.clone()).await,
    ))?;

    // The shared transform (see add_image_to_pack_content).
    let (mut content, code) = add_image_to_pack_content(
        existing, requested, url, body, mimetype, width, height, size, usage,
    )?;

    // Name a brand-new user pack so it is not an unnamed tab elsewhere. Not for
    // room packs, which default to the room name.
    if content.get("pack").and_then(|p| p.as_object()).is_none() {
        content["pack"] = json!({ "display_name": "Stickers" });
    }

    let raw = matrix_sdk::ruma::serde::Raw::new(&content)
        .map_err(|_| "rejected".to_owned())?
        .cast_unchecked();
    let written = client.account().set_account_data_raw(ty, raw).await;
    // The sync echo of this write has not landed; a remembered "not found"
    // would hide it (AbsentAnswers).
    forget_own_write(&client, USER_EMOTES);
    written.map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
    Ok(code)
}

// ---------------------------------------------------------------------------
// Standards-native personal packs ("My packs") and migrating the legacy
// personal pack into them
// ---------------------------------------------------------------------------
//
// Matrix v1.19 has NO stable personal account-data pack. Lightning's product
// design: one private, invite-only room owned by the account holds the
// account's packs as ordinary `m.room.image_pack` state, referenced by
// `m.image_pack.rooms`. Not a new room type and not an `m.*` event: the room is
// marked only by an owned-namespace key in its creation content, and found
// again through owned-namespace account data (the journal). Pack state and the
// images are NOT end-to-end encrypted by being in a private room.

/// Owned-namespace account data: which room is this account's pack home.
pub(crate) const PACK_HOME_TYPE: &str = "org.lightning_matrix.image_pack_home";
/// Owned-namespace key in the home room's `m.room.create` content.
pub(crate) const PACK_HOME_MARKER: &str = "org.lightning_matrix.image_pack_home";

/// Which standard pack a legacy image goes to when migrating. Legacy packs
/// can give each image its own usage; a stable pack cannot, so a mixed
/// collection is split into at most three packs (the product default the
/// campaign asks for) and none of them is broadened.
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

/// The migration plan, pure: legacy content in, one stable content per
/// non-empty partition out, plus the keys that cannot go (outside the v1.19
/// grammar: they stay in the untouched legacy pack and are listed for
/// repair). Image entries are copied as they are (url, body, info, unknown
/// fields) minus the legacy per-image `usage`, which the partition now
/// carries pack-wide. The pack's attribution and avatar travel too.
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

/// Resume rule, pure: writing a partition onto a record that already exists
/// (an interrupted earlier run, or another device) adds only the images it
/// lacks, by exact key; nothing there is overwritten or removed.
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

/// Which of the account's joined rooms carry the home marker, lowest id
/// first. More than one means two devices (or a lost create response) made
/// homes; they are reported, never deleted.
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

/// Find this account's pack home, creating it only when nothing says one
/// exists. Order: the journal's room if joined; else a joined room carrying
/// the marker (a create whose response was lost, or another device's); else
/// create one, writing the journal right after. Returns (room id, created,
/// duplicate count).
async fn ensure_pack_home(client: &matrix_sdk::Client) -> Result<(String, bool, usize), String> {
    use matrix_sdk::ruma::api::client::room::create_room;
    use matrix_sdk::ruma::api::client::room::Visibility;

    let journal = read_global_account_data(client, PACK_HOME_TYPE).await;
    let marked = marked_home_rooms(client).await;
    let duplicates = marked.len().saturating_sub(1);
    if let Some(room_id) = journal
        .as_ref()
        .and_then(|j| j.get("room_id"))
        .and_then(|v| v.as_str())
    {
        if joined_room(client, room_id).is_ok() {
            return Ok((room_id.to_owned(), false, duplicates));
        }
    }
    if let Some(room_id) = marked.first() {
        write_account_data(client, PACK_HOME_TYPE, &json!({ "room_id": room_id, "version": 1 }))
            .await?;
        return Ok((room_id.clone(), false, duplicates));
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

/// Copy the legacy personal pack into standard packs in the account's pack
/// home and subscribe to them. Each step's real outcome is reported
/// (`home`, `written`, `subscribed`); a failure part-way leaves everything
/// already done in place, and running it again resumes (the home is found
/// again, existing records only gain missing images, subscribing twice is a
/// no-op). The legacy pack is never modified.
///
/// Answers `sticker_pack_migration { op_id, lifecycle, ok, category,
/// room_id, created, duplicates, packs, invalid, subscribed }`.
pub(crate) fn migrate_user_pack(bridge: &RustClient, op_id: u64) -> Result<(), String> {
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
            let (room_id, created, duplicates) = ensure_pack_home(&client).await?;
            report["room_id"] = json!(room_id);
            report["created"] = json!(created);
            report["duplicates"] = json!(duplicates);
            let room = joined_room(&client, &room_id).map_err(|_| "unknown_room".to_owned())?;
            let mut written = Vec::new();
            for (partition, content) in plan.packs {
                let key = partition.state_key();
                let existing =
                    read_one_room_pack(&client, &room_id, key, Namespace::Stable).await.0;
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

// Typed only so the SDK dispatches these four types to the change handlers;
// never read (a change is re-read through `fetch_packs`, the one parser).
#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.room.image_pack", kind = State, state_key_type = String)]
pub(crate) struct StableImagePackEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.room_emotes", kind = State, state_key_type = String)]
pub(crate) struct LegacyRoomEmotesEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "m.image_pack.rooms", kind = GlobalAccountData)]
pub(crate) struct StableImagePackRoomsEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.emote_rooms", kind = GlobalAccountData)]
pub(crate) struct LegacyEmoteRoomsEventContent {}

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize, EventContent)]
#[ruma_event(type = "im.ponies.user_emotes", kind = GlobalAccountData)]
pub(crate) struct LegacyUserEmotesEventContent {}

/// `{type: sticker_packs_changed, room_id}`; an empty room id means account
/// data (subscriptions or the account's own pack). Nothing else crosses.
pub(crate) fn change_payload(room_id: &str) -> Value {
    json!({ "type": "sticker_packs_changed", "room_id": room_id })
}

/// Report pack and subscription changes from sync so an open picker or
/// manager updates without a restart (PACK-LTN-002). A room change also drops
/// that room's cached scan and remembered absences, so the re-read sees it.
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
            MAX_SHORTCODE_BYTES
        );
    }

    #[test]
    fn a_malformed_received_key_is_kept_as_its_own_identity() {
        // PACK-PRO-008 / PACK-LTN-009: a key outside the grammar stays visible
        // and addressable by its EXACT text; it is not repaired into `good`
        // (which would then collide with the real `good`).
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
        // A hostile, absurdly long key is still dropped.
        let long = parse(json!({ "images": {
            "k".repeat(MAX_RAW_KEY_BYTES + 1): { "url": "mxc://example.org/1" } } }));
        assert!(long.images.is_empty());
    }

    #[test]
    fn stable_valid_keys_survive_exactly_including_case_runs_and_length() {
        // PACK-PRO-005/006, PACK-LTN-006/007/008: the old reader collapsed
        // `hello__world` to `hello_world` and truncated at 64, so an edit
        // addressed a different (or no) image.
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
        // 101 bytes is outside the grammar, but still shown, not truncated.
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
        // A valid request is taken exactly; only invalid text is suggested.
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

        // A malformed key is removable and repairable as itself.
        let removed = remove_image_from_pack_content(content.clone(), "bad key").unwrap();
        assert!(removed["images"].get("bad key").is_none());
        let (repaired, code) =
            rename_image_in_pack_content(content.clone(), "bad key", "good_key").unwrap();
        assert_eq!(code, "good_key");
        assert_eq!(repaired["images"]["good_key"]["url"], json!("mxc://e/f"));
        assert_eq!(repaired["images"].as_object().unwrap().len(), 5);

        // Collision is exact and case-sensitive: `Cat` -> `cat` is taken,
        // `Cat` -> `CAT` is not.
        assert_eq!(
            rename_image_in_pack_content(content.clone(), "Cat", "cat").unwrap_err(),
            "shortcode_taken"
        );
        assert!(rename_image_in_pack_content(content.clone(), "Cat", "CAT").is_ok());
        // A rename target is validated, never repaired into another name.
        for bad in ["has space", "a:b", "", &"q".repeat(101)] {
            assert_eq!(
                rename_image_in_pack_content(content.clone(), "Cat", bad).unwrap_err(),
                "invalid_shortcode",
                "{bad:?}"
            );
        }
        // A 65-100 byte target is accepted as typed.
        let long = "L".repeat(100);
        let (_, code) = rename_image_in_pack_content(content, "Cat", &long).unwrap();
        assert_eq!(code, long);
    }

    #[test]
    fn a_suffixed_key_stays_inside_the_grammar_and_is_case_exact() {
        let base = json!({ "images": { "Cat": { "url": "mxc://e/1" } } });
        // `cat` is a different key from `Cat`: no suffix.
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
        // PACK-LTN-017: the old add path mapped every miss to `{}` and wrote a
        // one-image pack over a real one whose read had merely failed.
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
        // An unparseable or non-object body is a failure, not a blank slate.
        for body in ["not json", "[]", "\"x\""] {
            let read = classify_pack_read(Ok(body));
            assert_eq!(read, PackRead::Failed("malformed".to_owned()), "{body}");
            assert!(base_for_add(read).is_err());
        }
        // Only a confirmed absence lets an add create, and it creates the
        // schema-valid empty pack, never `{}`.
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
        // Unknown spellings are stickers, the one that never lands inline.
        let junk = add_as(json!({}), "cat", "mxc://example.org/a", "emoji!");
        assert_eq!(junk["images"]["cat"]["usage"], json!(["sticker"]));
        // And the parser reads each back the way it was meant.
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
        // PACK-PRO-027 / media grammar: no extra path, query, fragment or
        // traversal in the media id.
        for bad in ["mxc://e.org/a/b", "mxc://e.org/a?x=1", "mxc://e.org/a#f",
                    "mxc://e.org/..", "mxc://e.org/a b", "mxc://e.org/"] {
            assert!(!is_valid_mxc(bad), "{bad}");
        }
        assert!(is_valid_mxc("mxc://e.org:8448/Ab_c-9"));
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
        // An invalid name is refused before anything moves.
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
        // Images are untouched by renaming the pack.
        assert_eq!(named["images"].as_object().unwrap().len(), 2);
    }
    // ---- stable namespaces (Matrix v1.19) --------------------------------

    fn parse_stable(content: Value) -> Pack {
        parse_pack_content_ns(&content, "id".into(), "room", "!r:e".into(), "k".into(),
                              "Room", Namespace::Stable)
            .expect("object")
    }

    #[test]
    fn stable_usage_is_pack_wide_and_unknown_only_is_not_everything() {
        // PACK-PRO-010/011/012, PACK-LTN-011.
        let both = parse_stable(json!({ "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((both.images[0].is_emoticon, both.images[0].is_sticker), (true, true));
        let empty = parse_stable(json!({ "pack": { "usage": [] },
                                         "images": { "a": { "url": "mxc://e/1" } } }));
        assert_eq!((empty.images[0].is_emoticon, empty.images[0].is_sticker), (true, true));
        // A per-image `usage` is a legacy field: in a stable pack it is ignored.
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
        // The same document read as LEGACY keeps the per-image override
        // (PACK-PRO-013).
        let legacy = parse(json!({ "pack": { "usage": ["emoticon"] },
            "images": { "a": { "url": "mxc://e/1", "usage": ["sticker"] } } }));
        assert_eq!((legacy.images[0].is_emoticon, legacy.images[0].is_sticker),
                   (false, true));
    }

    #[test]
    fn room_pack_ids_are_unambiguous_across_colons_and_dialects() {
        // `room:<room>:<key>` could not be split: `!a:b` + `c:d` and `!a:b:c` +
        // `d` collided.
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
        // PACK-LTN-001/004, PACK-MIG-004/005/018.
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
        // The empty state key is an ordinary pack (PACK-PRO-002); `cleared`
        // and `retired` are gone; `twin` is the stable record, flagged.
        assert_eq!(keys, vec![("", Namespace::Stable), ("old", Namespace::Legacy),
                              ("twin", Namespace::Stable)]);
        let twin = out.iter().find(|r| r.state_key == "twin").unwrap();
        assert!(twin.legacy_twin);
        assert_eq!(twin.content["images"]["x"]["url"], json!("mxc://e/stable"));
        // Two copies of one record keep the newer one.
        let out = resolve_room_packs(vec![
            rec(Namespace::Stable, "k", img("mxc://e/new"), 10),
            rec(Namespace::Stable, "k", img("mxc://e/old"), 3),
        ]);
        assert_eq!(out[0].content["images"]["x"]["url"], json!("mxc://e/new"));
    }

    #[test]
    fn a_present_stable_subscription_document_is_authoritative_even_empty() {
        // PACK-MIG-006: an intentional "unsubscribe from everything" stays so.
        let legacy = json!({ "rooms": { "!a:e": { "x": {} } } });
        let empty = effective_subscriptions(&PackRead::Found(json!({ "rooms": {} })),
                                            Some(&legacy));
        assert!(empty.active.is_empty());
        assert_eq!(empty.source, "stable");
        assert_eq!(empty.legacy_only, vec![("!a:e".to_owned(), "x".to_owned())]);
        // Confirmed absent: legacy references stay in force.
        let absent = effective_subscriptions(&PackRead::Absent, Some(&legacy));
        assert_eq!(absent.active, vec![("!a:e".to_owned(), "x".to_owned())]);
        assert_eq!(absent.source, "legacy");
        assert!(!absent.stable_unreadable);
        // Unreadable is not absent (PACK-MIG-007): shown, flagged.
        let failed = effective_subscriptions(&PackRead::Failed("network".into()),
                                             Some(&legacy));
        assert!(failed.stable_unreadable);
        // An empty inner map is NOT "every pack in the room" (PACK-PRO-016).
        let none = effective_subscriptions(
            &PackRead::Found(json!({ "rooms": { "!a:e": {} } })), None);
        assert!(none.active.is_empty());
    }

    #[test]
    fn the_first_stable_document_carries_legacy_choices_and_leaves_leaves_alone() {
        // PACK-PRO-015/017: subscribe to one explicit key, keep every other
        // room, key and leaf object.
        let legacy = json!({ "rooms": { "!old:e": { "a": { "future": 1 } } } });
        let first = plan_stable_subscription_write(PackRead::Absent, Some(&legacy),
                                                   "!new:e", "k", true).unwrap();
        assert_eq!(first["rooms"]["!old:e"]["a"]["future"], json!(1));
        assert_eq!(first["rooms"]["!new:e"]["k"], json!({}));
        assert_eq!(first["rooms"].as_object().unwrap().len(), 2);
        // An existing stable document: other leaves survive a toggle, and
        // re-enabling a present key keeps its leaf.
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
        // An unreadable stable document writes nothing.
        assert_eq!(plan_stable_subscription_write(PackRead::Failed("network".into()),
                                                  None, "!a:e", "k", true).unwrap_err(),
                   "network");
    }

    #[test]
    fn creating_needs_a_confirmed_absence_and_writes_valid_stable_content() {
        // PACK-PRO-001/002, PACK-MIG-018: `images` is present even when empty.
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
        // Usage is a stable, pack-wide edit.
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
        // PACK-UX-003.
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
        // PACK-LTN-003, PACK-MIG-013.
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
        // PACK-PRO-019/020/021.
        let claim = |id: &str, canonical, via, reciprocal, auth| ParentClaim {
            space_id: id.into(), canonical, has_via: via, reciprocal, sender_authorised: auth,
        };
        // A false claim (no reciprocity, sender without authority) is ignored.
        assert_eq!(pick_canonical_parent(&[claim("!evil:e", true, true, false, false)]), None);
        // Not canonical, or no via: ignored.
        assert_eq!(pick_canonical_parent(&[claim("!a:e", false, true, true, true),
                                           claim("!b:e", true, false, true, true)]), None);
        // Reciprocal OR an authorised sender suffices.
        assert_eq!(pick_canonical_parent(&[claim("!a:e", true, true, true, false)]),
                   Some("!a:e".into()));
        assert_eq!(pick_canonical_parent(&[claim("!a:e", true, true, false, true)]),
                   Some("!a:e".into()));
        // Two valid canonical parents: the lowest id by code point.
        assert_eq!(pick_canonical_parent(&[claim("!b:e", true, true, true, false),
                                           claim("!B:e", true, true, true, false),
                                           claim("!a:e", true, true, false, true)]),
                   Some("!B:e".into()));
    }

    #[test]
    fn canonical_ancestry_is_bounded_cycle_safe_and_membership_scoped() {
        // PACK-PRO-019/020/022, PACK-LTN-013/014: child -> A -> B -> A (a
        // cycle); a forged canonical claim to `!evil` (no reciprocity, sender
        // with no authority there) is ignored; ancestry never passes a space
        // the account has not joined.
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
                // Lower id than `!a`, canonical, but nobody in `!evil` agrees.
                ev("m.space.parent", "!0evil:e", "@stranger:e", "$2", via.clone()),
            ]);
            room(a, vec![
                ev("m.space.child", child.as_str(), &me, "$3", child_of.clone()),
                ev("m.space.child", b.as_str(), &me, "$7", child_of.clone()),
                ev("m.space.parent", b.as_str(), &me, "$4", via.clone()),
            ]);
            room(b, vec![
                ev("m.space.child", a.as_str(), &me, "$5", child_of.clone()),
                // A cycle back to A.
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
            // Starting at B: its parent A, then A's parent B is already
            // visited, so the walk stops.
            let from_b = canonical_ancestry(&client, b.as_str()).await;
            assert_eq!(from_b, vec![a.to_string()]);
            drop(server);
        });
    }

    #[test]
    fn a_mixed_legacy_pack_is_split_without_broadening_or_losing_anything() {
        // PACK-PRO-014, PACK-LTN-005, PACK-MIG-003.
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
        // Inherited pack usage `sticker`: `cat` is a sticker, not broadened.
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
        // Every planned pack is a valid stable document; the plan read the
        // legacy value without changing it.
        for (_, content) in &plan.packs {
            assert!(content["images"].is_object());
        }
        assert!(legacy["images"]["wave"].get("usage").is_some());
    }

    #[test]
    fn resuming_a_migration_adds_only_what_is_missing() {
        // PACK-PRO-052, PACK-MIG-011/012.
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

    // ---- remembering a server "not found" -------------------------------

    use std::sync::atomic::{AtomicUsize, Ordering};

    /// One read against `memory`, with a store that holds `stored` and a server
    /// that answers `answer`; returns the value and how many times it was asked.
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

    // The reported chatter: a room open asked for a type the server had already
    // said it lacks. Asked once per account, not once per open.
    #[tokio::test]
    async fn a_server_not_found_is_asked_once() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        for _ in 0..9 {
            assert!(read_once(&memory, None, || ServerAnswer::Absent, &asked).await.is_none());
        }
        assert_eq!(asked.load(Ordering::SeqCst), 1);
    }

    // A failure says nothing about whether the pack exists: asked again.
    #[tokio::test]
    async fn a_failed_request_is_not_remembered() {
        let memory = AbsentAnswers::new();
        let asked = AtomicUsize::new(0);
        for _ in 0..3 {
            assert!(read_once(&memory, None, || ServerAnswer::Failed, &asked).await.is_none());
        }
        assert_eq!(asked.load(Ordering::SeqCst), 3);
    }

    // Sync delivering the type puts it in the store, which is read first: the
    // mark is dropped and the value used, with no request.
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
        // And the type is gone again later: that is a new question, asked.
        assert!(read_once(&memory, None, || ServerAnswer::Absent, &asked).await.is_none());
        assert_eq!(asked.load(Ordering::SeqCst), 2);
    }

    // A found answer is never remembered as absence.
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

    // Every account-data write in this file must drop the mark: a write whose
    // echo has not landed would otherwise be hidden for the TTL. Each write site
    // is a `set_account_data_raw` call; each must be followed by the helper.
    // (A source sweep, since the sites need a live homeserver: mutation-checked
    // by deleting one call.)
    #[test]
    fn every_account_data_write_forgets_the_absent_mark() {
        let source = include_str!("stickers.rs");
        let production = &source[..source.find("#[cfg(test)]").unwrap()];
        let writes = production.matches(".set_account_data_raw(").count();
        // Call sites, not the definition (`fn forget_own_write(`); the shared
        // writer `write_account_data` passes its own `client` parameter.
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
