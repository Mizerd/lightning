//! MatrixRTC (MSC4143): the Matrix side of modern calling.
//!
//! Owns what a MatrixRTC session puts on the Matrix wire: who is in a call,
//! which SFU transport ("focus") they advertise, rings, and declines. No
//! media. The wire is pinned to the reference implementations (matrix-js-sdk
//! `src/matrixrtc`, element-call), which are ahead of the unmerged MSCs:
//!
//! * Membership is the state event `org.matrix.msc3401.call.member`
//!   ([`EV_MEMBER_LEGACY`]). The sticky MSC4354 form ([`EV_MEMBER_STICKY`])
//!   is parsed and tested, but matrix-sdk 0.18 cannot observe sticky events.
//! * Rings are `org.matrix.msc4075.rtc.notification`, with our own event
//!   content: ruma 0.34 types it only as stable `m.rtc.notification`, so a
//!   ruma handler never fires for Element's rings.
//!
//! Transports are discovered, never hardcoded, in element-call's order: the
//! homeserver's advertisement, then the focus existing participants
//! advertise (`foci_preferred`, i.e. `oldest_membership`), then a
//! user-configured URL. Never a vendor default.
//!
//! Safety: almost every string here is remote-chosen. Inbound strings are
//! bounded and control-character free ([`sane`]) and collections capped; an
//! invalid membership is dropped, never partially trusted. `member.user_id`
//! must equal the event sender (as in the reference). Transport URLs must be
//! `https:`. Nothing here logs a sender-chosen string, URL or member id.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;
use std::sync::Mutex;
use std::time::Duration;

use matrix_sdk::deserialized_responses::{EncryptionInfo, RawAnySyncOrStrippedState};
use matrix_sdk::event_handler::EventHandlerDropGuard;
use matrix_sdk::ruma::events::macros::EventContent;
use matrix_sdk::ruma::events::relation::Reference;
use matrix_sdk::ruma::events::{Mentions, StateEventType};
use matrix_sdk::ruma::{EventId, MilliSecondsSinceUnixEpoch, OwnedEventId};
use matrix_sdk_base::crypto::CollectStrategy;
use matrix_sdk::config::RequestConfig;
use matrix_sdk::ruma::api::client::state::get_state_events;
use matrix_sdk::{Client, Room};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};

use crate::rooms::{classify_room_error, joined_room, public_ip, require_client};
use crate::{enqueue, RustClient};

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

/// Legacy (and currently the only *deployed*) membership state event.
pub(crate) const EV_MEMBER_LEGACY: &str = "org.matrix.msc3401.call.member";
/// MSC4143 sticky membership event. Parsed but not observable on
/// matrix-sdk 0.18, so unused outside tests.
#[allow(dead_code)]
pub(crate) const EV_MEMBER_STICKY: &str = "org.matrix.msc4143.rtc.member";
/// MSC4143 slot state event, describing an open/closed session in a room.
pub(crate) const EV_SLOT: &str = "org.matrix.msc4143.rtc.slot";
/// The notification event Element sends. The string lives in the
/// event-content derive; this constant is asserted against it in tests.
#[allow(dead_code)]
pub(crate) const EV_NOTIFICATION_UNSTABLE: &str =
    "org.matrix.msc4075.rtc.notification";

/// The application every call-shaped session uses.
const APPLICATION_CALL: &str = "m.call";

/// `call_id: ""` is the room-wide call; the slot vocabulary spells it
/// `"ROOM"`. Converting between them, as the reference does, keeps one call
/// from looking like two.
const SLOT_ID_ROOM: &str = "ROOM";

/// Membership validity when `expires` is absent (the reference's
/// `DEFAULT_EXPIRE_DURATION`).
const DEFAULT_EXPIRE_MS: u64 = 4 * 60 * 60 * 1000;
/// Ceiling on how far past the envelope's origin_server_ts a membership may
/// claim to live.
///
/// `expires` and `created_ts` are member-written content; unclamped, a
/// membership could never expire and always sort oldest, and "oldest" picks
/// the SFU every later joiner sends its OpenID token to. Measured from the
/// event rather than `created_ts`, because a refresh keeps the original join
/// time and writes `expires = (now - created) + period`; each refresh is a
/// new event, so long calls stay alive and nothing is immortal.
const MAX_EXPIRE_MS: u64 = 24 * 60 * 60 * 1000;
/// Max amount a `created_ts` may be ahead of origin_server_ts. A future join
/// time is never legitimate; it would only make the membership sort last.
const MAX_CREATED_TS_SKEW_MS: u64 = 5 * 60 * 1000;
/// Max amount a `created_ts` may be behind origin_server_ts. Refreshes carry
/// the original join time; clamping to a day bounds how far backdating can
/// reach while long calls still sort first.
const MAX_BACKDATE_MS: u64 = 24 * 60 * 60 * 1000;

/// Element caps notification lifetime at 90 s (`parseCallNotificationContent`).
const MAX_NOTIFICATION_LIFETIME_MS: u64 = 90_000;

// Bounds on values a remote party chooses.
const MAX_WIRE_LEN: usize = 255;
/// Timeout for a raise/lower, short because the user is watching the
/// toggle.
const HAND_TIMEOUT: Duration = Duration::from_secs(10);
/// How many memberships the join-time raised-hand pass probes (each a
/// cache-first relations load). Beyond this, earlier hands are missed; the
/// sync handler still sees every later one.
const MAX_HAND_PROBES: usize = 24;
const MAX_URL_LEN: usize = 1024;
const MAX_MEMBERS: usize = 128;
/// Bound on raw membership state events parsed, applied before the
/// 128-member cap so a room with many stale events does not do all the work.
const MAX_RAW_MEMBER_EVENTS: usize = 512;
/// The membership fallback must not stall key distribution.
const MEMBERSHIP_FETCH_TIMEOUT: Duration = Duration::from_secs(10);
/// How recent the newest event in a room's stored membership view must be
/// for "the store shows nothing live" to still warrant a `/state` request.
///
/// The `/state` fallback in [`read_membership_events`] is this module's
/// most expensive operation. Gating it only on "no live membership in the
/// store" spent one per idle room on startup, since every room that ever
/// hosted a call has expired membership state. A real change is stamped
/// now, a replay of an old call is stamped then, so the store's newest
/// instant discriminates. Well over twice `MEMBERSHIP_EXPIRY_NO_DELAYED_MS`,
/// so a peer whose refresh we have not received still earns one request.
///
/// Compared with server-stamped times, like the liveness filter; a skewed
/// device clock shifts both the same way.
const SESSION_SIGNAL_HORIZON_MS: u64 = 15 * 60 * 1000;
/// Minimum gap between two implicit `/state` escalations for one room; it
/// doubles up to the max below. Without the doubling, one ghost membership
/// (an unclean exit) would buy a request per poke for the whole horizon.
/// Mirrors `RtcController::m_serverReadStreak` in C++, which paces forced
/// reads.
const SERVER_ESCALATION_COOLDOWN_MS: u64 = 15_000;
const SERVER_ESCALATION_COOLDOWN_MAX_MS: u64 = 300_000;
/// How long a ring keeps its room worth one `/state`. The room is not open,
/// so its state may be stale, and the incoming call's Answer button is
/// gated on a session read. Longer than `MAX_NOTIFICATION_LIFETIME_MS`.
const RING_ESCALATION_WINDOW_MS: u64 = 3 * 60 * 1000;
/// Ceiling on the per-room mark tables below (like `MAX_REPORT_MARKS`).
/// Keys are joined rooms, not attacker-chosen, but must stay bounded.
const MAX_ROOM_MARKS: usize = 256;
const MAX_TRANSPORTS: usize = 8;
#[allow(dead_code)]
const MAX_VERSIONS: usize = 8;

const DISCOVERY_TIMEOUT: Duration = Duration::from_secs(15);
/// Bound on the discovery response body; larger answers are refused.
const MAX_DISCOVERY_BODY: usize = 64 * 1024;

// ---------------------------------------------------------------------------
// Sanitizers
// ---------------------------------------------------------------------------

/// Accept a bounded, control-character-free string, or nothing. `None`
/// means "drop the surrounding thing"; there is no lossy repair.
fn sane(value: &str, max: usize) -> Option<&str> {
    if value.is_empty() || value.len() > max {
        return None;
    }
    if value.chars().any(|c| c.is_control()) {
        return None;
    }
    Some(value)
}

fn sane_string(value: Option<&serde_json::Value>, max: usize) -> Option<String> {
    sane(value?.as_str()?, max).map(ToOwned::to_owned)
}

/// A transport URL must be absolute HTTPS with a host: it is where call
/// authorization is exchanged.
fn sane_https_url(value: &str) -> Option<String> {
    let trimmed = sane(value, MAX_URL_LEN)?;
    let parsed = url::Url::parse(trimmed).ok()?;
    if parsed.scheme() != "https" {
        return None;
    }
    // Embedded credentials in an advertised URL are never legitimate.
    if !parsed.username().is_empty() || parsed.password().is_some() {
        return None;
    }
    let host = parsed.host()?;
    // Foci come from remote participants, so refuse the obvious SSRF shapes
    // here; a DNS-resolution check happens at connection time. Uses the same
    // `public_ip` policy as link previews (which unmaps `::ffff:a.b.c.d` and
    // refuses CGNAT, multicast and the rest).
    match host {
        url::Host::Ipv4(addr) => {
            if !public_ip(std::net::IpAddr::V4(addr)) {
                return None;
            }
        }
        url::Host::Ipv6(addr) => {
            if !public_ip(std::net::IpAddr::V6(addr)) {
                return None;
            }
        }
        url::Host::Domain(name) => {
            if !public_hostname(name) {
                return None;
            }
        }
    }
    Some(parsed.to_string())
}

/// Names that can only resolve locally: `localhost`, `.local` (mDNS),
/// `.localhost`, `.internal`.
pub(crate) fn public_hostname(name: &str) -> bool {
    let lower = name.trim_end_matches('.').to_ascii_lowercase();
    !(lower == "localhost"
        || lower.ends_with(".localhost")
        || lower.ends_with(".local")
        || lower.ends_with(".internal"))
}

/// Why a host could not be resolved to an approved address. Distinguished
/// so the UI does not report an unresolvable name as a private-address
/// refusal.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum HostRefusal {
    /// The name can only mean this machine or link (`localhost`, `.local`,
    /// `.internal`); refused before any lookup.
    PrivateName,
    /// The resolver returned nothing or did not answer.
    Unresolved,
    /// At least one resolved address is not public. The whole name is refused
    /// (see docs/matrixrtc.md): a public A record beside a loopback AAAA is the
    /// rebinding shape this guards against.
    NonPublicAddress,
}

/// Every address `host` resolves to, in resolver order, if all are public.
/// Resolution is the only way to see where an attacker-chosen name points.
/// Returns the whole list so a connecting caller can try the next one.
pub(crate) async fn resolve_public_hosts(
    host: &str,
    port: u16,
) -> Result<Vec<std::net::SocketAddr>, HostRefusal> {
    if !public_hostname(host) {
        return Err(HostRefusal::PrivateName);
    }
    let addresses: Vec<std::net::SocketAddr> = tokio::net::lookup_host((host, port))
        .await
        .map_err(|_| HostRefusal::Unresolved)?
        .collect();
    if addresses.is_empty() {
        return Err(HostRefusal::Unresolved);
    }
    if !addresses.iter().all(|address| public_ip(address.ip())) {
        return Err(HostRefusal::NonPublicAddress);
    }
    Ok(addresses)
}

/// Resolve `host`, require every address to be public, and return the
/// first, for callers that pin one address (reqwest's `.resolve()`).
pub(crate) async fn resolve_public_host(
    host: &str,
    port: u16,
) -> Option<std::net::SocketAddr> {
    resolve_public_hosts(host, port)
        .await
        .ok()
        .and_then(|addresses| addresses.into_iter().next())
}

// ---------------------------------------------------------------------------
// Transports (foci)
// ---------------------------------------------------------------------------

/// A LiveKit transport: where to obtain SFU authorization for a session.
/// `service_url` is the JWT service; the SFU URL comes back from
/// `POST {service_url}/sfu/get`. Nothing here is a media endpoint or a
/// credential.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct LivekitTransport {
    pub service_url: String,
    /// SFU room alias, when the advertiser pinned one; otherwise the JWT
    /// service derives it from the room id.
    pub alias: Option<String>,
}

impl LivekitTransport {
    fn to_json(&self) -> serde_json::Value {
        let mut value = json!({
            "type": "livekit",
            "livekit_service_url": self.service_url,
        });
        if let Some(alias) = &self.alias {
            value["livekit_alias"] = json!(alias);
        }
        value
    }
}

/// Parse one transport object. Only `type: "livekit"` is understood; others
/// are skipped rather than guessed at.
pub(crate) fn parse_transport(value: &serde_json::Value) -> Option<LivekitTransport> {
    let object = value.as_object()?;
    if object.get("type")?.as_str()? != "livekit" {
        return None;
    }
    let service_url = sane_https_url(object.get("livekit_service_url")?.as_str()?)?;
    let alias = object
        .get("livekit_alias")
        .and_then(|value| value.as_str())
        .and_then(|value| sane(value, MAX_WIRE_LEN))
        .map(ToOwned::to_owned);
    Some(LivekitTransport { service_url, alias })
}

fn parse_transport_list(value: Option<&serde_json::Value>) -> Vec<LivekitTransport> {
    let Some(array) = value.and_then(|value| value.as_array()) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for entry in array.iter().take(MAX_TRANSPORTS) {
        if let Some(transport) = parse_transport(entry) {
            if !out.contains(&transport) {
                out.push(transport);
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Membership
// ---------------------------------------------------------------------------

/// Which wire format a membership came from. Diagnostics only.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum MembershipKind {
    /// `org.matrix.msc3401.call.member` state event.
    Session,
    /// `org.matrix.msc4143.rtc.member` sticky event. Only produced by the
    /// tested parser on matrix-sdk 0.18.
    #[allow(dead_code)]
    Rtc,
}

impl MembershipKind {
    fn as_str(self) -> &'static str {
        match self {
            MembershipKind::Session => "session",
            MembershipKind::Rtc => "rtc",
        }
    }
}

/// One participant device in a MatrixRTC session.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct RtcMember {
    pub user_id: String,
    pub device_id: String,
    /// The identity this device uses on the SFU, derived, never read:
    /// `"{user_id}:{device_id}"` for session memberships, or the hashed member
    /// id for the sticky format.
    pub rtc_identity: String,
    /// `m.call#ROOM` for the room-wide call.
    pub slot_id: String,
    /// Sender-declared intent, sanitized to a closed set.
    pub intent: &'static str,
    /// When this device first joined, in ms. Falls back to the event ts.
    pub created_ts: u64,
    /// Absolute expiry in ms. Past this the membership is stale and dropped;
    /// the only defence against a client that died without cleaning up.
    pub expires_at_ms: u64,
    /// Foci this device advertises, in its own preference order.
    pub foci: Vec<LivekitTransport>,
    pub kind: MembershipKind,
    /// Room-resolved profile, filled in after parsing (see `read_session`).
    pub display_name: String,
    pub avatar_mxc: String,
    /// The `m.call.member` state event that declared this membership.
    /// element-call raises a hand as an `m.reaction` annotating it. Filled in
    /// by `read_session` from the envelope; empty when there is none, which
    /// matches no hand rather than a wrong one.
    pub event_id: String,
}

impl RtcMember {
    fn to_json(&self) -> serde_json::Value {
        json!({
            "user_id": self.user_id,
            // Resolved from the room state: m.call.member carries no profile. Empty
            // when the member is not in the store; the UI shows initials.
            "display_name": self.display_name,
            "avatar_mxc": self.avatar_mxc,
            "device_id": self.device_id,
            "rtc_identity": self.rtc_identity,
            "slot_id": self.slot_id,
            "intent": self.intent,
            "created_ts": self.created_ts,
            "expires_at_ms": self.expires_at_ms,
            // A raised hand annotates this state event.
            "event_id": self.event_id,
            "kind": self.kind.as_str(),
            "foci": self.foci.iter().map(LivekitTransport::to_json)
                .collect::<Vec<_>>(),
        })
    }
}

/// Sender-declared intent, collapsed to a closed set. The field is free
/// text and decides whether a video answer is offered; unknown values
/// degrade to audio.
fn intent_str(value: Option<&serde_json::Value>) -> &'static str {
    match value.and_then(|value| value.as_str()) {
        Some("video") => "video",
        Some("audio") => "audio",
        _ => "audio",
    }
}

/// Convert a legacy `call_id` to a slot id with the reference's `""` ->
/// `"ROOM"` rule, so both formats compare equal.
fn slot_id_for_call_id(application: &str, call_id: &str) -> String {
    let id = if call_id.is_empty() { SLOT_ID_ROOM } else { call_id };
    format!("{application}#{id}")
}

/// The SFU identity for the sticky format:
/// unpadded-base64(sha256(canonical JSON `[user_id, device_id, member_id]`)).
/// Must match `computeRtcIdentityRaw` byte for byte.
#[allow(dead_code)] // sticky lane only; see EV_MEMBER_STICKY.
pub(crate) fn rtc_identity(user_id: &str, device_id: &str, member_id: &str) -> String {
    let canonical = serde_json::to_string(&[user_id, device_id, member_id])
        .unwrap_or_default();
    let digest = Sha256::digest(canonical.as_bytes());
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD_NO_PAD.encode(digest)
}

/// Parse a legacy `org.matrix.msc3401.call.member` content. `sender` and
/// `event_ts` come from the envelope, never the content.
pub(crate) fn parse_session_membership(
    content: &serde_json::Value,
    sender: &str,
    event_ts: u64,
) -> Option<RtcMember> {
    let object = content.as_object()?;

    // `{}` content is a leave (the retraction), not a participant.
    if object.is_empty() {
        return None;
    }
    let application = sane_string(object.get("application"), MAX_WIRE_LEN)?;
    if application != APPLICATION_CALL {
        return None;
    }
    let device_id = sane_string(object.get("device_id"), MAX_WIRE_LEN)?;
    // `call_id` is required but empty for the room call, so not `sane`.
    let call_id = object.get("call_id")?.as_str()?;
    if call_id.len() > MAX_WIRE_LEN || call_id.chars().any(|c| c.is_control()) {
        return None;
    }
    // `focus_active.type` is required by the reference validator.
    object.get("focus_active")?.as_object()?.get("type")?.as_str()?;

    let user_id = sane(sender, MAX_WIRE_LEN)?.to_owned();

    // Both timestamps are member claims feeding focus selection and liveness,
    // so both are clamped against origin_server_ts: created_ts to
    // [event - MAX_BACKDATE_MS, event + MAX_CREATED_TS_SKEW_MS], the deadline
    // to event + MAX_EXPIRE_MS (see MAX_EXPIRE_MS for why not created_ts).
    let created_ts = object
        .get("created_ts")
        .and_then(|value| value.as_u64())
        .unwrap_or(event_ts)
        .min(event_ts.saturating_add(MAX_CREATED_TS_SKEW_MS))
        .max(event_ts.saturating_sub(MAX_BACKDATE_MS));
    let expires = object
        .get("expires")
        .and_then(|value| value.as_u64())
        .unwrap_or(DEFAULT_EXPIRE_MS);
    // Saturating, so a hostile `expires` cannot wrap into the past.
    let expires_at_ms = created_ts
        .saturating_add(expires)
        .min(event_ts.saturating_add(MAX_EXPIRE_MS));

    // Derived, never read: the JWT service assigns `{user}:{device}` from the
    // OpenID-verified user and the requested device, and the reference treats
    // an absent `membershipID` the same way. Reading the content field would
    // let a member claim another's identity, stealing their key ring, media
    // key and tile. The content field is ignored.
    let rtc_identity = format!("{user_id}:{device_id}");

    Some(RtcMember {
        user_id,
        device_id,
        rtc_identity,
        slot_id: slot_id_for_call_id(&application, call_id),
        intent: intent_str(object.get("m.call.intent")),
        created_ts,
        expires_at_ms,
        foci: parse_transport_list(object.get("foci_preferred")),
        kind: MembershipKind::Session,
        display_name: String::new(),
        avatar_mxc: String::new(),
        // Content only; read_session fills in the envelope's id.
        event_id: String::new(),
    })
}

/// Parse an MSC4143 `org.matrix.msc4143.rtc.member` content, in step with
/// the reference validator (including `member.user_id == sender`). Not
/// reachable from sync on matrix-sdk 0.18; tested so the format does not
/// rot.
#[allow(dead_code)] // sticky lane only; see EV_MEMBER_STICKY.
pub(crate) fn parse_rtc_membership(
    content: &serde_json::Value,
    sender: &str,
    event_ts: u64,
) -> Option<RtcMember> {
    let object = content.as_object()?;

    let application = object.get("application")?.as_object()?;
    let app_type = sane_string(application.get("type"), MAX_WIRE_LEN)?;
    // A '#' in the application would make the slot id ambiguous.
    if app_type.contains('#') {
        return None;
    }

    let slot_id = sane_string(object.get("slot_id"), MAX_WIRE_LEN)?;
    // The slot id must name this application and split cleanly in two.
    let (slot_app, slot_rest) = slot_id.split_once('#')?;
    if slot_app != app_type || slot_rest.contains('#') {
        return None;
    }

    let member = object.get("member")?.as_object()?;
    let user_id = sane_string(member.get("user_id"), MAX_WIRE_LEN)?;
    // Forgery guard — see module docs.
    if user_id != sender {
        return None;
    }
    let device_id = sane_string(member.get("device_id"), MAX_WIRE_LEN)?;
    let member_id = sane_string(member.get("id"), MAX_WIRE_LEN)?;

    // `transports` is required and `published` must be an array.
    let transports = object.get("transports")?.as_object()?;
    transports.get("published")?.as_array()?;
    let foci = parse_transport_list(transports.get("published"));

    // `versions` is required and must be an array of strings.
    let versions = object.get("versions")?.as_array()?;
    if versions.len() > MAX_VERSIONS
        || !versions.iter().all(|value| value.is_string())
    {
        return None;
    }

    // Without a sticky key the event has no retraction identity.
    let sticky = object
        .get("sticky_key")
        .or_else(|| object.get("msc4354_sticky_key"))
        .and_then(|value| value.as_str())?;
    if sane(sticky, MAX_WIRE_LEN).is_none() {
        return None;
    }

    Some(RtcMember {
        rtc_identity: rtc_identity(&user_id, &device_id, &member_id),
        user_id,
        device_id,
        slot_id,
        intent: intent_str(application.get("m.call.intent")),
        created_ts: event_ts,
        // Sticky memberships are retracted, never aged out (the reference gives no
        // expiry); a 4 h fallback would drop live participants.
        expires_at_ms: u64::MAX,
        foci,
        kind: MembershipKind::Rtc,
        display_name: String::new(),
        avatar_mxc: String::new(),
        event_id: String::new(),
    })
}

// ---------------------------------------------------------------------------
// Session aggregation
// ---------------------------------------------------------------------------

/// The live state of one room's MatrixRTC session.
#[derive(Clone, Debug, Default)]
pub(crate) struct RtcSession {
    pub members: Vec<RtcMember>,
    /// True when a slot state event exists and says the session is closed.
    pub slot_closed: bool,
    /// True when a slot state event was present at all.
    pub slot_present: bool,
    /// Where the memberships came from: "store", "store-no-session" (nothing
    /// live, no reason to ask), "store-cooling-own"/"-ring"/"-recent" (a reason,
    /// but within the room's escalation backoff), "server", "server-none"
    /// (asked; no membership state) or "store-fallback" (asked; request
    /// failed). Distinguishes a stale store from a room where nobody is
    /// published.
    pub source: &'static str,
    /// Raw membership events considered before parsing, expiry and dedup, so
    /// `participants=0 raw=7` differs from `participants=0 raw=0`.
    pub raw_count: usize,
}

/// Aggregate parsed memberships into a session.
///
/// * Expired memberships are dropped (as Element does), or vanished
///   clients show as participants.
/// * Dedup is per `(user_id, device_id)`, keeping the newest `created_ts`:
///   one device may briefly hold two events during the state-key migration,
///   and one user on two devices is two participants.
pub(crate) fn aggregate_session(
    mut members: Vec<RtcMember>,
    now_ms: u64,
) -> Vec<RtcMember> {
    members.retain(|member| member.expires_at_ms > now_ms);

    let mut best: BTreeMap<(String, String), RtcMember> = BTreeMap::new();
    for member in members {
        let key = (member.user_id.clone(), member.device_id.clone());
        match best.get(&key) {
            Some(existing) if existing.created_ts >= member.created_ts => {}
            _ => {
                best.insert(key, member);
            }
        }
    }

    let mut out: Vec<RtcMember> = best.into_values().collect();
    // Oldest first: this ordering is the `oldest_membership` focus rule, so it
    // must be stable and by join time.
    out.sort_by(|a, b| {
        a.created_ts
            .cmp(&b.created_ts)
            .then_with(|| a.user_id.cmp(&b.user_id))
            .then_with(|| a.device_id.cmp(&b.device_id))
    });
    out.truncate(MAX_MEMBERS);
    out
}

/// Pick the focus per `focus_selection: "oldest_membership"`: the SFU
/// advertised by whoever joined first. Everyone must agree, or participants
/// land on different SFUs.
pub(crate) fn select_focus(members: &[RtcMember]) -> Option<LivekitTransport> {
    // The oldest membership's own first focus, and nothing else: the reference
    // reads `getOldestMembership()`'s `foci_preferred[0]` and yields nothing if
    // it has none. Walking on would diverge from Element.
    members.first().and_then(|member| member.foci.first().cloned())
}

// ---------------------------------------------------------------------------
// Transport discovery endpoint (MSC4143)
// ---------------------------------------------------------------------------

/// Read `org.matrix.msc4143.rtc_foci` (or stable `m.rtc_foci`) from
/// `GET https://<server_name>/.well-known/matrix/client`, which is where
/// MSC4143 advertises SFUs and Element Call reads them. There is no
/// client-API transports endpoint.
///
/// Hand-rolled over the SDK's HTTP client: `Client::well_known()` is private
/// and the typed field needs matrix-sdk's `unstable-msc4143` feature. The
/// file is public, so no access token is sent. Fetched from the MXID's
/// server name, not the delegated homeserver URL, since the delegating
/// domain serves this file.
mod transports_endpoint {
    use matrix_sdk::Client;

    /// Transport objects are open-ended, so the body stays raw JSON and
    /// `parse_transport` alone decides what is understood.
    pub(super) struct Answer {
        pub status: u16,
        pub transports: Vec<serde_json::Value>,
    }

    fn endpoint(client: &Client) -> Result<String, String> {
        // The MXID's server name, checked to be host-shaped before it goes into a
        // URL, so a hostile value cannot redirect the request.
        let server_name = client
            .user_id()
            .ok_or_else(|| "no session".to_owned())?
            .server_name()
            .as_str()
            .to_owned();
        if server_name.is_empty()
            || server_name.len() > 255
            || server_name.contains('/')
            || server_name.contains('\\')
            || server_name.contains('@')
            || server_name.contains('?')
            || server_name.contains('#')
            || server_name.chars().any(|c| c.is_whitespace() || c.is_control())
        {
            return Err("unusable server name".to_owned());
        }
        // Always https: this file chooses the SFU every call is routed through.
        let url = format!("https://{server_name}/.well-known/matrix/client");
        // Parsed, so a value that slipped the checks cannot target another host.
        let parsed = url::Url::parse(&url).map_err(|_| "bad url".to_owned())?;
        if parsed.scheme() != "https" || parsed.host_str() != Some(&server_name)
        {
            return Err("unusable server name".to_owned());
        }
        Ok(parsed.to_string())
    }

    pub(super) async fn get(
        client: &Client,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client)?;
        // No Authorization header: the file is public, and a bearer token would
        // leak to whatever host the MXID's domain resolves to.
        let response = client
            .http_client()
            .get(url)
            .timeout(timeout)
            .send()
            .await
            .map_err(|err| err.to_string())?;
        let status = response.status().as_u16();
        // Refuse an oversized answer before reading: `text()` buffers the whole
        // body first.
        if response
            .content_length()
            .is_some_and(|len| len > super::MAX_DISCOVERY_BODY as u64)
        {
            return Ok(Answer { status, transports: Vec::new() });
        }
        // Content-Length may be absent, so also stop reading at the cap.
        let mut body = Vec::with_capacity(1024);
        let mut stream = response;
        while let Some(chunk) =
            stream.chunk().await.map_err(|err| err.to_string())?
        {
            if body.len() + chunk.len() > super::MAX_DISCOVERY_BODY {
                return Ok(Answer { status, transports: Vec::new() });
            }
            body.extend_from_slice(&chunk);
        }
        let body = String::from_utf8_lossy(&body).into_owned();
        // Unstable key first, then stable, as ruma reads them.
        let transports = serde_json::from_str::<serde_json::Value>(&body)
            .ok()
            .and_then(|value| {
                value
                    .get("org.matrix.msc4143.rtc_foci")
                    .or_else(|| value.get("m.rtc_foci"))
                    .and_then(|list| list.as_array())
                    .cloned()
            })
            .unwrap_or_default();
        Ok(Answer { status, transports })
    }
}

// ---------------------------------------------------------------------------
// Notification event (MSC4075)
// ---------------------------------------------------------------------------

/// `org.matrix.msc4075.rtc.notification`, the ring. Defined here because
/// ruma 0.34 types only the stable `m.rtc.notification`, which is deaf to
/// Element's rings. Both are observed (see [`register_rtc_handlers`]); this
/// one is sent, matching Element.
#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4075.rtc.notification", kind = MessageLike)]
pub(crate) struct Msc4075RtcNotificationEventContent {
    /// `"ring"` for a DM-style ring, `"notification"` for a group call
    /// announcement that must not ring everyone.
    pub notification_type: String,
    pub sender_ts: MilliSecondsSinceUnixEpoch,
    /// Milliseconds, as a plain integer (what the reference reads).
    pub lifetime: u64,
    #[serde(rename = "m.mentions", default, skip_serializing_if = "Option::is_none")]
    pub mentions: Option<Mentions>,
    #[serde(rename = "m.relates_to", default, skip_serializing_if = "Option::is_none")]
    pub relates_to: Option<Reference>,
    #[serde(rename = "m.call.intent", default, skip_serializing_if = "Option::is_none")]
    pub call_intent: Option<String>,
}

/// Legacy membership state event, typed only so the SDK reports changes.
/// The content is a passthrough: the handler just signals "membership
/// changed" and the session is re-read through the one parser above.
#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(
    type = "org.matrix.msc3401.call.member",
    kind = State,
    state_key_type = String
)]
pub(crate) struct LegacyRtcMemberEventContent {}

// ---------------------------------------------------------------------------
// Reading a room's session
// ---------------------------------------------------------------------------

/// Raw JSON of a state event from the store. `RawAnySyncOrStrippedState`
/// wraps two `Raw` types (joined vs invite stripped state), both carrying
/// the envelope fields needed here.
fn raw_state_json(raw: &RawAnySyncOrStrippedState) -> Option<serde_json::Value> {
    let json = match raw {
        RawAnySyncOrStrippedState::Sync(raw) => raw.json(),
        RawAnySyncOrStrippedState::Stripped(raw) => raw.json(),
    };
    serde_json::from_str(json.get()).ok()
}

/// Is this raw membership event a live participant now? The same parse as
/// the session read, so the two cannot disagree.
fn membership_event_is_live(value: &serde_json::Value, now_ms: u64) -> bool {
    let Some(object) = value.as_object() else { return false };
    let Some(sender) = object.get("sender").and_then(|v| v.as_str()) else {
        return false;
    };
    let event_ts = object
        .get("origin_server_ts")
        .and_then(|value| value.as_u64())
        .unwrap_or(now_ms);
    let Some(content) = object.get("content") else { return false };
    parse_session_membership(content, sender, event_ts)
        .is_some_and(|member| member.expires_at_ms > now_ms)
}

/// Does the store's answer stand on its own, or must the server be asked?
/// "Usable" means at least one membership that parses and has not expired.
/// A merely non-empty check would let a dead membership left by an unclean
/// exit suppress the network read, sending the media key to the wrong
/// devices.
pub(crate) fn store_view_is_usable(
    events: &[serde_json::Value],
    now_ms: u64,
) -> bool {
    events
        .iter()
        .any(|value| membership_event_is_live(value, now_ms))
}

/// The newest instant in a stored membership view that could belong to a
/// running session, used once we know nothing is live:
///
///  * a parseable membership contributes its deadline (a peer whose refresh
///    we have not received expired only just now);
///  * anything else (a `{}` retraction, an unparseable event) contributes
///    its `origin_server_ts`, since a retraction is itself recent activity.
///
/// A missing `origin_server_ts` reads as now, the conservative direction,
/// matching `membership_event_is_live`.
fn newest_session_signal_ms(
    events: &[serde_json::Value],
    now_ms: u64,
) -> Option<u64> {
    let mut newest: Option<u64> = None;
    for value in events {
        let Some(object) = value.as_object() else { continue };
        let event_ts = object
            .get("origin_server_ts")
            .and_then(|value| value.as_u64())
            .unwrap_or(now_ms);
        let mut signal = event_ts;
        if let (Some(sender), Some(content)) = (
            object.get("sender").and_then(|value| value.as_str()),
            object.get("content"),
        ) {
            if let Some(member) =
                parse_session_membership(content, sender, event_ts)
            {
                signal = signal.max(member.expires_at_ms);
            }
        }
        newest = Some(newest.map_or(signal, |best: u64| best.max(signal)));
    }
    newest
}

/// Why a store read with nothing live still warrants one `/state`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EscalationReason {
    /// We hold a published membership in this room, so we are in its call. The
    /// store can hold only stale retractions while the server has the live
    /// memberships, and media keys go to the devices these name, so an empty
    /// read would send the key to nobody.
    OwnCall,
    /// A ring arrived for this room moments ago. The room is not open, so its
    /// state may be stale, and Answer is gated on the session read.
    Ring,
    /// The store's newest call-member event is recent enough that a session
    /// could still be running.
    RecentActivity,
}

impl EscalationReason {
    /// The word a read reports when it wanted the homeserver but the room's
    /// backoff held it off, so "did not ask" and "not allowed to ask" differ.
    fn cooling_word(self) -> &'static str {
        match self {
            EscalationReason::OwnCall => "store-cooling-own",
            EscalationReason::Ring => "store-cooling-ring",
            EscalationReason::RecentActivity => "store-cooling-recent",
        }
    }
}

/// What a non-forced session read does with the store's answer. The whole
/// decision, matched exhaustively in [`read_membership_events`], so a new
/// variant cannot be left unhandled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum StoreVerdict {
    /// The store holds a live membership. Answer from it; no request.
    Answer,
    /// Nothing live and no sign of a session the store cannot see: a real "no
    /// call in this room" answer.
    AnswerNoSession,
    /// Nothing live, but a session may exist that the store has not caught up
    /// with. Worth one `/state`, subject to the caller's per-room backoff.
    AskServer(EscalationReason),
}

pub(crate) fn store_read_verdict(
    events: &[serde_json::Value],
    now_ms: u64,
    in_own_call: bool,
    rang_recently: bool,
) -> StoreVerdict {
    if store_view_is_usable(events, now_ms) {
        return StoreVerdict::Answer;
    }
    // Ordered by strength of evidence (all cost one request), so the log names
    // why the request was spent.
    if in_own_call {
        return StoreVerdict::AskServer(EscalationReason::OwnCall);
    }
    if rang_recently {
        return StoreVerdict::AskServer(EscalationReason::Ring);
    }
    match newest_session_signal_ms(events, now_ms) {
        Some(signal)
            if now_ms.saturating_sub(signal) <= SESSION_SIGNAL_HORIZON_MS =>
        {
            StoreVerdict::AskServer(EscalationReason::RecentActivity)
        }
        // Includes the empty store (rooms that never hosted a call).
        _ => StoreVerdict::AnswerNoSession,
    }
}

/// The gap owed before this room may escalate again after
/// `consecutive_asks` escalations that found nothing live. Pure for tests.
fn escalation_cooldown_ms(consecutive_asks: u32) -> u64 {
    let mut ms = SERVER_ESCALATION_COOLDOWN_MS;
    // The first ask owes the base gap.
    for _ in 1..consecutive_asks {
        if ms >= SERVER_ESCALATION_COOLDOWN_MAX_MS {
            break;
        }
        ms = ms.saturating_mul(2);
    }
    ms.min(SERVER_ESCALATION_COOLDOWN_MAX_MS)
}

/// When each room last spent an implicit `/state`, and how many in a row
/// found nothing live.
fn server_escalation_marks() -> &'static std::sync::Mutex<
    std::collections::HashMap<String, (std::time::Instant, u32)>,
> {
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<
            std::collections::HashMap<String, (std::time::Instant, u32)>,
        >,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

/// Rooms a MatrixRTC ring arrived for, and when.
fn rtc_ring_marks()
    -> &'static std::sync::Mutex<
        std::collections::HashMap<String, std::time::Instant>,
    >
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<
            std::collections::HashMap<String, std::time::Instant>,
        >,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

/// Record that this room just rang. Uses `Instant`, so clock steps cannot
/// move the window.
pub(crate) fn note_rtc_ring(room_id: &str) {
    if let Ok(mut marks) = rtc_ring_marks().lock() {
        if marks.len() >= MAX_ROOM_MARKS && !marks.contains_key(room_id) {
            marks.clear();
        }
        marks.insert(room_id.to_owned(), std::time::Instant::now());
    }
    // A ring is fresh evidence of a session, so it also clears this room's
    // backoff: the next read may ask immediately.
    clear_server_escalation_backoff(room_id);
}

fn rtc_ring_is_recent(room_id: &str) -> bool {
    rtc_ring_marks()
        .lock()
        .ok()
        .and_then(|marks| marks.get(room_id).copied())
        .is_some_and(|at| {
            let since_ms = at.elapsed().as_millis() as u64;
            since_ms <= RING_ESCALATION_WINDOW_MS
        })
}

/// May this room spend an implicit `/state` now? Records the attempt when
/// it says yes.
fn claim_server_escalation(room_id: &str) -> bool {
    let Ok(mut marks) = server_escalation_marks().lock() else {
        // Fail open on a poisoned lock: a live call's media keys depend on this
        // fallback.
        return true;
    };
    let now = std::time::Instant::now();
    let previous = marks.get(room_id).copied();
    if let Some((last, asks)) = previous {
        let waited_ms = now.saturating_duration_since(last).as_millis() as u64;
        if waited_ms < escalation_cooldown_ms(asks) {
            return false;
        }
    }
    if marks.len() >= MAX_ROOM_MARKS && previous.is_none() {
        marks.clear();
    }
    let asks = previous.map_or(0, |(_, asks)| asks).saturating_add(1);
    marks.insert(room_id.to_owned(), (now, asks));
    true
}

/// An escalation that found somebody live resets the streak (the next gap
/// is the base one) but keeps the gap, so a store that lags for a whole call
/// does not escalate on every poke.
fn note_server_escalation_found_live(room_id: &str) {
    if let Ok(mut marks) = server_escalation_marks().lock() {
        if let Some(entry) = marks.get_mut(room_id) {
            entry.1 = 0;
        }
    }
}

/// Forget a room's backoff entirely, so its next read may ask at once.
fn clear_server_escalation_backoff(room_id: &str) {
    if let Ok(mut marks) = server_escalation_marks().lock() {
        marks.remove(room_id);
    }
}

/// Bound a membership list to `cap`, keeping live memberships before dead
/// ones. The cap bounds work; cutting arbitrarily (or alphabetically) could
/// keep only ghosts and drop the people actually in the call.
fn bound_membership_events(
    mut events: Vec<serde_json::Value>,
    now_ms: u64,
    cap: usize,
) -> Vec<serde_json::Value> {
    if events.len() <= cap {
        return events;
    }
    events.sort_by_key(|value| !membership_event_is_live(value, now_ms));
    events.truncate(cap);
    events
}

/// The slot one membership event occupies, for merging two views. The state
/// key (`{user}_{device}_{application}`) is the identity; sender plus device
/// id is the fallback when no envelope field is present.
fn membership_slot(value: &serde_json::Value) -> Option<String> {
    let object = value.as_object()?;
    if let Some(key) = object.get("state_key").and_then(|v| v.as_str()) {
        if !key.is_empty() {
            return Some(key.to_owned());
        }
    }
    let sender = object.get("sender").and_then(|v| v.as_str())?;
    let device = object
        .get("content")
        .and_then(|content| content.get("device_id"))
        .and_then(|value| value.as_str())
        .unwrap_or_default();
    Some(format!("{sender}\u{1f}{device}"))
}

/// Merge two views of a room's memberships, keeping the newer event per
/// state key. The server's `/state` can predate our own publish, and the
/// store can hold an event the server already replaced; a retraction wins
/// on its timestamp like anything else.
pub(crate) fn merge_membership_events(
    store: Vec<serde_json::Value>,
    server: Vec<serde_json::Value>,
    now_ms: u64,
) -> Vec<serde_json::Value> {
    fn ts(value: &serde_json::Value) -> u64 {
        value
            .get("origin_server_ts")
            .and_then(|value| value.as_u64())
            .unwrap_or(0)
    }
    let mut best: BTreeMap<String, serde_json::Value> = BTreeMap::new();
    let mut unkeyed: Vec<serde_json::Value> = Vec::new();
    for value in store.into_iter().chain(server.into_iter()) {
        let Some(slot) = membership_slot(&value) else {
            unkeyed.push(value);
            continue;
        };
        match best.get(&slot) {
            Some(existing) if ts(existing) >= ts(&value) => {}
            _ => {
                best.insert(slot, value);
            }
        }
    }
    let mut out: Vec<serde_json::Value> = best.into_values().collect();
    out.append(&mut unkeyed);
    bound_membership_events(out, now_ms, MAX_RAW_MEMBER_EVENTS)
}

/// Every membership state event for a room as raw JSON, and its source.
///
/// The store is asked first, the homeserver second. Sliding sync delivers
/// `CallMember` state, but not promptly for rooms we are not subscribed to:
/// a store can hold only stale retractions while the server has the live
/// memberships. Media keys go to the devices these events name, so an empty
/// read sends the key to nobody and every frame is dropped.
///
/// The network read is a fallback, used only when the store has nothing
/// live and there is a reason to believe a session exists (see
/// [`StoreVerdict`]), with a per-room backoff. The store read happens on
/// every poke, so a call starting is still noticed immediately.
///
/// `prefer_server` asks the server anyway, for a caller with evidence the
/// store is incomplete (an SFU participant no membership accounts for).
async fn read_membership_events(
    room: &Room,
    now_ms: u64,
    prefer_server: bool,
) -> (Vec<serde_json::Value>, &'static str) {
    // Parse first, bound second: the store query is unordered, so bounding
    // first could miss every live membership.
    let from_store = bound_membership_events(
        room.get_state_events(StateEventType::from(EV_MEMBER_LEGACY))
            .await
            .unwrap_or_default()
            .iter()
            .filter_map(raw_state_json)
            .collect(),
        now_ms,
        MAX_RAW_MEMBER_EVENTS,
    );

    if !prefer_server {
        let room_id = room.room_id().as_str();
        match store_read_verdict(
            &from_store,
            now_ms,
            room_has_published_membership(room_id),
            rtc_ring_is_recent(room_id),
        ) {
            StoreVerdict::Answer => return (from_store, "store"),
            StoreVerdict::AnswerNoSession => {
                return (from_store, "store-no-session")
            }
            StoreVerdict::AskServer(reason) => {
                if !claim_server_escalation(room_id) {
                    return (from_store, reason.cooling_word());
                }
            }
        }
    }

    let client = room.client();
    let config = RequestConfig::new()
        .disable_retry()
        .timeout(MEMBERSHIP_FETCH_TIMEOUT);
    let request = get_state_events::v3::Request::new(room.room_id().to_owned());
    let Ok(response) = client.send(request).with_request_config(config).await
    else {
        return (from_store, "store-fallback");
    };
    // No bound before the filter: `/state` is dominated by `m.room.member`, so
    // bounding the raw response could hide every `m.call.member`. The bound
    // that matters is on what is kept.
    let mut from_server = Vec::new();
    for raw in response.room_state.iter() {
        let Ok(value) = serde_json::from_str::<serde_json::Value>(raw.json().get())
        else {
            continue;
        };
        if value.get("type").and_then(|t| t.as_str()) != Some(EV_MEMBER_LEGACY) {
            continue;
        }
        from_server.push(value);
        if from_server.len() >= MAX_RAW_MEMBER_EVENTS {
            break;
        }
    }
    if from_server.is_empty() {
        // The server answered and the room has no membership state: a real answer,
        // distinct from a failed request.
        return (from_store, "server-none");
    }
    let merged = merge_membership_events(from_store, from_server, now_ms);
    if !prefer_server && store_view_is_usable(&merged, now_ms) {
        // Found what it was spent on, so the streak resets. Forced reads are paced
        // by their caller and do not touch this backoff.
        note_server_escalation_found_live(room.room_id().as_str());
    }
    (merged, "server")
}

async fn read_session(
    room: &Room,
    now_ms: u64,
    prefer_server: bool,
) -> RtcSession {
    let mut members = Vec::new();

    let (raw_members, source) =
        read_membership_events(room, now_ms, prefer_server).await;
    let raw_count = raw_members.len();

    for value in raw_members.iter() {
        // Loose JSON: only `sender`, `content` and `origin_server_ts` are needed,
        // and one unreadable membership must not poison the rest.
        let Some(object) = value.as_object() else { continue };
        let Some(sender) = object.get("sender").and_then(|v| v.as_str()) else {
            continue;
        };
        let event_ts = object
            .get("origin_server_ts")
            .and_then(|value| value.as_u64())
            .unwrap_or(now_ms);
        let Some(content) = object.get("content") else { continue };
        if let Some(mut member) =
            parse_session_membership(content, sender, event_ts)
        {
            // The envelope's event id, which a raised hand annotates.
            member.event_id = object
                .get("event_id")
                .and_then(|value| value.as_str())
                .filter(|id| {
                    id.len() <= MAX_WIRE_LEN
                        && !id.chars().any(|c| c.is_control())
                })
                .unwrap_or_default()
                .to_owned();
            members.push(member);
        }
    }

    let (slot_present, slot_closed) = read_slot(room).await;

    let mut members = aggregate_session(members, now_ms);
    // Resolve profiles after aggregation, and with `_no_sync` so a facepile
    // costs no network round trips.
    for member in &mut members {
        let Ok(user_id) = matrix_sdk::ruma::UserId::parse(&member.user_id)
        else {
            continue;
        };
        if let Ok(Some(profile)) = room.get_member_no_sync(&user_id).await {
            member.display_name =
                profile.display_name().unwrap_or_default().to_owned();
            member.avatar_mxc = profile
                .avatar_url()
                .map(|url| url.to_string())
                .unwrap_or_default();
        }
    }

    RtcSession {
        members,
        slot_closed,
        slot_present,
        source,
        raw_count,
    }
}

/// Read the MSC4143 slot state, if any. Absence is not "closed" (almost no
/// deployment publishes slots); only an explicit `status: "closed"` closes.
async fn read_slot(room: &Room) -> (bool, bool) {
    let Ok(events) = room.get_state_events(StateEventType::from(EV_SLOT)).await else {
        return (false, false);
    };
    let mut present = false;
    let mut closed = false;
    let room_slot = format!("{APPLICATION_CALL}#{SLOT_ID_ROOM}");
    for raw in events {
        let Some(value) = raw_state_json(&raw) else { continue };
        let Some(content) = value.get("content").and_then(|v| v.as_object()) else {
            continue;
        };
        // Exact match only: an empty state key is not the room call's slot, and
        // treating it as one would let anyone able to send this type hide the call.
        let Some(state_key) = value.get("state_key").and_then(|v| v.as_str())
        else {
            continue;
        };
        if state_key != room_slot {
            continue;
        }
        present = true;
        // Fail closed, as the reference does: open only if it says so and the slot
        // is for this application.
        let status_open =
            content.get("status").and_then(|v| v.as_str()) == Some("open");
        let app_matches = content
            .get("application")
            .and_then(|v| v.as_object())
            .and_then(|app| app.get("type"))
            .and_then(|v| v.as_str())
            == Some(APPLICATION_CALL);
        if !status_open || !app_matches {
            closed = true;
        }
    }
    (present, closed)
}

/// Closed-set category for a discovery HTTP status. Never the body.
fn status_category(status: u16) -> &'static str {
    match status {
        400 | 404 | 405 => "unsupported",
        401 | 403 => "forbidden",
        429 => "rate_limited",
        500..=599 => "server_error",
        _ => "unknown",
    }
}

/// Whether a discovery outcome settles "does this homeserver have
/// MatrixRTC?". Definitive: an empty category (it answered) and
/// `unsupported` (400/404/405, no MSC4143). Anything else (`forbidden`,
/// `rate_limited`, `server_error`, `network`, …) leaves it open, so the UI
/// says "couldn't check" and the caller may retry.
fn discovery_answer_is_definitive(category: &str) -> bool {
    category.is_empty() || category == "unsupported"
}

/// Local wall clock, compared with server-supplied timestamps for expiry;
/// a skewed device clock can drop live participants or keep dead ones (as
/// in the reference implementation).
fn now_ms() -> u64 {
    MilliSecondsSinceUnixEpoch::now().get().into()
}

fn session_payload(room_id: &str, session: &RtcSession) -> serde_json::Value {
    let focus = select_focus(&session.members);
    json!({
        "type": "rtc_session",
        "room_id": room_id,
        "member_count": session.members.len(),
        "slot_present": session.slot_present,
        "slot_closed": session.slot_closed,
        // Source and raw count: a fixed word and a number only.
        "source": session.source,
        "raw_count": session.raw_count,
        "focus": focus.as_ref().map(LivekitTransport::to_json),
        "members": session.members.iter().map(RtcMember::to_json)
            .collect::<Vec<_>>(),
    })
}

// ---------------------------------------------------------------------------
// FFI-facing operations
// ---------------------------------------------------------------------------

/// Report the current MatrixRTC session for one room.
///
/// `prefer_server` also asks the homeserver and merges the views. It costs
/// one `/state`, so it is for a caller with evidence the store is
/// incomplete, rate limited by that caller.
pub(crate) fn request_session(
    bridge: &RustClient,
    room_id: String,
    prefer_server: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let session = read_session(&room, now_ms(), prefer_server).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut payload = session_payload(&room_id, &session);
        payload["op_id"] = json!(op_id);
        payload["lifecycle"] = json!(lifecycle);
        enqueue(&events, payload);
    });
    Ok(())
}

/// Discover the SFU transports this account can use: the homeserver's
/// answer plus the focus the room's participants advertise, labelled by
/// source so C++ can apply policy and explain unavailability.
pub(crate) fn request_transports(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    // Discovery is account-scoped; the room only adds the participant fallback.
    let room = joined_room(&client, &room_id).ok();

    bridge.spawn_room_action(async move {
        let advertised = match transports_endpoint::get(&client, DISCOVERY_TIMEOUT)
            .await
        {
            Ok(answer) if (200..300).contains(&answer.status) => Ok(answer
                .transports
                .iter()
                .filter_map(parse_transport)
                .take(MAX_TRANSPORTS)
                .collect::<Vec<_>>()),
            // 404/400/M_UNRECOGNIZED means "no MatrixRTC on this homeserver", which
            // must stay distinguishable from a transient failure.
            Ok(answer) => Err(status_category(answer.status).to_owned()),
            Err(err) => Err(classify_room_error(&err).to_owned()),
        };

        let mut participant_focus = None;
        if let Some(room) = room.as_ref() {
            let session = read_session(room, now_ms(), false).await;
            participant_focus = select_focus(&session.members);
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        let (server_transports, category) = match advertised {
            Ok(list) => (list, String::new()),
            Err(category) => (Vec::new(), category),
        };

        enqueue(&events, json!({
            "type": "rtc_transports",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            // "The server settled this", not "the server said yes"; see
            // discovery_answer_is_definitive.
            "server_answered": discovery_answer_is_definitive(&category),
            "category": category,
            "server_transports": server_transports.iter()
                .map(LivekitTransport::to_json).collect::<Vec<_>>(),
            "participant_focus": participant_focus.as_ref()
                .map(LivekitTransport::to_json),
        }));
    });
    Ok(())
}

/// Send an `org.matrix.msc4075.rtc.notification`. `notification_type` is
/// clamped to the reference's two values and `lifetime` to Element's 90 s
/// cap, past which other clients consider the ring invalid.
pub(crate) fn send_notification(
    bridge: &RustClient,
    room_id: String,
    notification_type: String,
    intent: String,
    lifetime_ms: u64,
    membership_event_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;

    let notification_type = match notification_type.as_str() {
        "ring" => "ring",
        "notification" => "notification",
        _ => return Err("notification type must be ring or notification".to_owned()),
    };
    let intent = match intent.as_str() {
        "video" => "video",
        _ => "audio",
    };
    let lifetime = lifetime_ms.clamp(1_000, MAX_NOTIFICATION_LIFETIME_MS);

    // Relating the ring to our membership event lets receivers connect it to a
    // session and target a decline at it.
    let relates_to = match sane(&membership_event_id, MAX_WIRE_LEN) {
        Some(id) => match OwnedEventId::try_from(id.to_owned()) {
            Ok(event_id) => Some(Reference::new(event_id)),
            Err(_) => return Err("invalid membership event id".to_owned()),
        },
        None => None,
    };

    let content = Msc4075RtcNotificationEventContent {
        notification_type: notification_type.to_owned(),
        sender_ts: MilliSecondsSinceUnixEpoch::now(),
        lifetime,
        // `room: true` makes it reach members through push rules, as Element does.
        mentions: Some(Mentions::with_room_mention()),
        relates_to,
        call_intent: Some(intent.to_owned()),
    };

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result =
            tokio::time::timeout(DISCOVERY_TIMEOUT, room.send(content)).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, event_id) = match result {
            Ok(Ok(response)) => (true, String::new(), response.response.event_id.to_string()),
            Ok(Err(err)) => (
                false,
                classify_room_error(&err.to_string()).to_owned(),
                String::new(),
            ),
            Err(_) => (false, "network".to_owned(), String::new()),
        };
        enqueue(&events, json!({
            "type": "rtc_send_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": ok,
            "category": category,
            "event_id": event_id,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Publishing our own membership
// ---------------------------------------------------------------------------

/// Membership validity when the server can retract it for us (an MSC4140
/// delayed retraction is armed). 4 h is the reference's
/// `DEFAULT_EXPIRE_DURATION`; the delayed retraction does the real cleanup.
const MEMBERSHIP_EXPIRY_MS: u64 = 4 * 60 * 60 * 1000;

/// Membership validity when nothing server-side will retract it (no
/// MSC4140; Synapse has it off by default). `expires` is then the only
/// cleanup, so it must run out in minutes, with the client re-publishing
/// often enough that a live participant never ages out. 5 minutes against
/// `SfuCallController`'s 60 s re-publish survives five failed refreshes;
/// shortening it without that cadence would drop people mid-call.
const MEMBERSHIP_EXPIRY_NO_DELAYED_MS: u64 = 5 * 60 * 1000;

/// The `expires` duration to write, given the membership's age.
///
/// Readers compute the deadline as `created_ts + expires`, and a refresh
/// keeps `created_ts` (so focus selection does not reorder). Writing a
/// constant would therefore republish the same absolute deadline, and the
/// membership would expire a fixed period after the join however often it
/// is refreshed: peers then drop the user and rotate keys without them.
///
/// `created_ts` is server time and `now_ms` local, as in the reference; skew
/// matters only at the scale of the refresh slack.
fn expires_for_refresh(period_ms: u64, created_ts: Option<u64>, now_ms: u64)
    -> u64
{
    match created_ts {
        // A first publish has no created_ts, so peers date it from the event's
        // origin_server_ts: the duration is the period.
        None => period_ms,
        // Saturating both ways: a future created_ts (skew, or hostile) must not
        // wrap, and yields the plain period.
        Some(created) => now_ms
            .saturating_sub(created)
            .saturating_add(period_ms),
    }
}

/// The `expires` for the no-MSC4140 fallback write, which replaces our state
/// event while keeping the original `created_ts`. A bare constant there
/// would publish a membership already expired whenever this is not a fresh
/// join, dropping our own device from every reader's list. It can run on
/// MSC4140 servers too, since `schedule_delayed_leave` reports any failure
/// (timeout, 429, 5xx) as an empty delay id. Same rule as
/// `expires_for_refresh`.
fn fallback_expires_ms(created_ts: Option<u64>, now_ms: u64) -> u64 {
    expires_for_refresh(MEMBERSHIP_EXPIRY_NO_DELAYED_MS, created_ts, now_ms)
}

/// MSC4140 delayed-event timeout: the server retracts our membership if we
/// stop restarting it. The only cleanup that survives a crash, kill or lost
/// network.
const DELAYED_LEAVE_TIMEOUT_MS: u64 = 8_000;

/// Homeservers that have refused to arm a delayed retraction.
///
/// Per server, because MSC4140 support is a server property: it decides
/// the `expires` of a publish's first write (saving a second write on every
/// refresh) and feeds the scheduled-send probe. A process-global flag let
/// one old server disable crash cleanup for every other account.
///
///   * An entry is added only for a category meaning the endpoint is absent
///     (`delayed_refusal_is_permanent`); a timeout, 429 or 5xx also yields an
///     empty delay id, and latching on those would disable crash cleanup
///     for a whole server over one blip.
///   * A successful arm removes the entry, so a server that gains support
///     recovers.
static DELAYED_EVENTS_REFUSED: Mutex<BTreeSet<String>> =
    Mutex::new(BTreeSet::new());

/// Record or clear the refusal for one homeserver. Takes a plain string so
/// it is testable; the `_for_client` wrappers are the production callers.
fn mark_delayed_refusal(server: &str, refused: bool) {
    let Ok(mut seen) = DELAYED_EVENTS_REFUSED.lock() else {
        return;
    };
    if refused {
        seen.insert(server.to_owned());
    } else {
        seen.remove(server);
    }
}

/// Has this homeserver refused? Another server's answer is not evidence.
fn delayed_refusal_recorded(server: &str) -> bool {
    DELAYED_EVENTS_REFUSED
        .lock()
        .map(|seen| seen.contains(server))
        .unwrap_or(false)
}

/// The key: the homeserver URL, since the property belongs to the server.
fn delayed_refusal_key(client: &Client) -> String {
    client.homeserver().to_string()
}

fn mark_delayed_refusal_for_client(client: &Client, refused: bool) {
    mark_delayed_refusal(&delayed_refusal_key(client), refused);
}

fn delayed_refusal_recorded_for_client(client: &Client) -> bool {
    delayed_refusal_recorded(&delayed_refusal_key(client))
}

/// Which (room, state key) memberships this process has published.
///
/// Decides whether a publish is a join or a refresh (the FFI carries no
/// flag). A refresh must inherit `created_ts`, which orders
/// `oldest_membership` focus selection and anchors `expires_for_refresh`. A
/// join must not: after an unclean exit on a server without MSC4140, our old
/// membership lingers, and inheriting its `created_ts` makes peers see an
/// unchanged `(userId, deviceId, membershipTs)`. matrix-js-sdk's
/// `rolloutOutboundKey` then sends no media key to the rejoined device, and
/// MatrixRTC has no key request, so it never decrypts. element-call's
/// `makeMyMembership` likewise omits `created_ts` on a join.
///
/// Keyed on (room, state key), so it is account-scoped (the state key
/// contains user and device).
static OWN_MEMBERSHIP_PUBLISHED: Mutex<BTreeSet<String>> =
    Mutex::new(BTreeSet::new());

/// Key for both halves. U+001F cannot occur in a room id or state key.
fn membership_publish_key(room_id: &str, state_key: &str) -> String {
    format!("{room_id}\u{1f}{state_key}")
}

fn mark_membership_published(key: &str) {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.insert(key.to_owned());
    }
    // Joining a call is fresh evidence of a session, so clear the room's
    // escalation backoff; otherwise the media key lane could read a stale store.
    if let Some(room_id) = key.split('\u{1f}').next() {
        clear_server_escalation_backoff(room_id);
    }
}

/// Forget it, so the next publish is a join. Called on the intent to leave,
/// not on retraction success: a failed retraction leaves the same ghost a
/// crash does.
fn forget_membership_published(key: &str) {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.remove(key);
    }
}

/// Forget every membership this process published in one room, without a
/// client or session. The sign-out path has no session left to derive the
/// state key from, and a stale mark would make the next join inherit a
/// ghost's `created_ts`.
fn forget_room_memberships_published(room_id: &str) {
    let prefix = format!("{room_id}\u{1f}");
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.retain(|key| !key.starts_with(&prefix));
    }
}

/// Drop the whole set. Only a membership this session published may be
/// inherited, so the set is cleared when a session ends rather than relying
/// on every leave path.
pub(crate) fn forget_all_memberships_published() {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.clear();
    }
    // The per-room mark tables are session state too; cleared here because
    // every teardown path runs this (see lib.rs), so the next account starts
    // clean.
    if let Ok(mut marks) = server_escalation_marks().lock() {
        marks.clear();
    }
    if let Ok(mut marks) = rtc_ring_marks().lock() {
        marks.clear();
    }
}

/// Has this process already published this membership? Pure over the set,
/// so the join/refresh rule is testable without a homeserver.
fn membership_published_in_this_process(key: &str) -> bool {
    OWN_MEMBERSHIP_PUBLISHED
        .lock()
        .map(|seen| seen.contains(key))
        .unwrap_or(false)
}

/// Is this process in a call in this room, i.e. does it hold a published
/// membership for any of its devices there? Prefix match on the same key as
/// `forget_room_memberships_published`.
fn room_has_published_membership(room_id: &str) -> bool {
    let prefix = format!("{room_id}\u{1f}");
    OWN_MEMBERSHIP_PUBLISHED
        .lock()
        .map(|seen| seen.iter().any(|key| key.starts_with(&prefix)))
        .unwrap_or(false)
}

/// Does this refusal category mean the homeserver does not implement
/// MSC4140, rather than something that may work next time? Pure for tests.
///
///   * `unrecognized`: Synapse's 404 M_UNRECOGNIZED;
///   * `not_found`: a plain 404;
///   * `no_delay_id`: the server ignored the delay parameter, applied the
///     body and returned 200 without a `delay_id` (GitHub #10);
///   * `delayed_unsupported`: Synapse with `msc4140_enabled` off answers
///     `400 M_UNKNOWN` "Delayed events are not supported on this server"
///     with `org.matrix.msc4140.errcode: M_MAX_DELAY_UNSUPPORTED` (the body
///     is not applied).
///
/// Everything else (`network`, including timeouts, `rate_limited`,
/// `forbidden`, `invalid`) is transient or room-specific. `forbidden` is
/// how a room's power levels refuse a state write.
pub(crate) fn delayed_refusal_is_permanent(category: &str) -> bool {
    matches!(
        category,
        "unrecognized" | "not_found" | "no_delay_id" | "delayed_unsupported"
    )
}

/// The `/versions` feature flag for MSC4140.
const MSC4140_FEATURE: &str = "org.matrix.msc4140";

/// Does the homeserver advertise delayed events?
///
/// Consulted only after an arm failed, as corroboration, never as a gate,
/// so a server that supports MSC4140 without advertising it still works.
/// `unstable_features()` holds only features set to true, so absence covers
/// both `false` and a server too old to name it. An unanswerable query
/// reads as "advertised": never latch on not knowing, since the latch
/// disables crash cleanup.
async fn server_advertises_delayed_events(client: &Client) -> bool {
    use matrix_sdk::ruma::api::FeatureFlag;
    match tokio::time::timeout(DISCOVERY_TIMEOUT, client.unstable_features())
        .await
    {
        Ok(Ok(features)) => features.contains(&FeatureFlag::from(MSC4140_FEATURE)),
        _ => true,
    }
}

/// Whether this process has seen the homeserver refuse a delayed event,
/// for the scheduled-send probe.
pub(crate) fn delayed_events_assumed_refused(client: &Client) -> bool {
    delayed_refusal_recorded_for_client(client)
}

/// The state key Element writes:
/// `{user}_{device}_{application}{slotId}`, with a leading underscore except
/// on room versions where a user-scoped key is owned by its user
/// (`org.matrix.msc3757`/`msc3779`). Getting it wrong makes the write fail
/// or leaves us in the call twice.
pub(crate) fn membership_state_key(
    user_id: &str,
    device_id: &str,
    room_version: &str,
) -> String {
    // The room call's slot id is "" in a state key ("ROOM" is not used on
    // this wire).
    let key = format!("{user_id}_{device_id}_{APPLICATION_CALL}");
    if room_version.starts_with("org.matrix.msc3757")
        || room_version.starts_with("org.matrix.msc3779")
    {
        key
    } else {
        format!("_{key}")
    }
}

/// The membership content we publish, in the legacy session format that
/// every deployed server and Element understand (matrix-sdk 0.18 cannot
/// send sticky events).
fn own_membership_content(
    device_id: &str,
    user_id: &str,
    focus: Option<&LivekitTransport>,
    intent: &str,
    created_ts: Option<u64>,
    expires_ms: u64,
) -> serde_json::Value {
    let mut content = json!({
        "application": APPLICATION_CALL,
        // "" — the room-wide call. See slot_id_for_call_id.
        "call_id": "",
        "scope": "m.room",
        "device_id": device_id,
        // The SFU participant identity for this format; the SFU assigns exactly
        // this.
        "membershipID": format!("{user_id}:{device_id}"),
        // Not a constant: depends on whether anything else will retract this
        // membership (see MEMBERSHIP_EXPIRY_NO_DELAYED_MS).
        "expires": expires_ms,
        "m.call.intent": intent,
        "focus_active": {
            "type": "livekit",
            "focus_selection": "oldest_membership",
        },
        "foci_preferred": focus
            .map(|f| vec![f.to_json()])
            .unwrap_or_default(),
    });
    // On a refresh, created_ts keeps the original join so focus selection does
    // not reorder.
    if let Some(created) = created_ts {
        content["created_ts"] = json!(created);
    }
    content
}

/// Publish or refresh our membership in a room's call.
///
///  1. Write the membership state event.
///  2. Arm an MSC4140 delayed retraction a few seconds out, restarted
///     periodically, so a crash does not leave a phantom participant.
///
/// A server without MSC4140 refuses step 2; that is reported as
/// `delayed_unsupported`, not a failure, and cleanup relies on `expires`.
pub(crate) fn publish_membership(
    bridge: &RustClient,
    room_id: String,
    focus_url: String,
    intent: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let user_id = client
        .user_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();
    let device_id = client
        .device_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();
    let intent = match intent.as_str() {
        "video" => "video",
        _ => "audio",
    };
    let focus = if focus_url.trim().is_empty() {
        None
    } else {
        sane_https_url(&focus_url).map(|service_url| LivekitTransport {
            service_url,
            alias: None,
        })
    };

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let room_version = room
            .clone_info()
            .room_version()
            .map(|v| v.to_string())
            .unwrap_or_default();
        let state_key =
            membership_state_key(&user_id, &device_id, &room_version);

        // Preserve created_ts only across a refresh. A join starts its own clock
        // (see OWN_MEMBERSHIP_PUBLISHED); `read_own_created_ts` is not even called
        // on a join, so a crashed session's ghost cannot leak in.
        let publish_key =
            membership_publish_key(room.room_id().as_str(), &state_key);
        let is_refresh = membership_published_in_this_process(&publish_key);
        let created_ts = if is_refresh {
            read_own_created_ts(&room, &state_key).await
        } else {
            None
        };
        // Carry the room as `livekit_alias`, as Element publishes it, so our focus
        // has the same shape as every other client's for the same SFU.
        let focus = focus.map(|transport| LivekitTransport {
            service_url: transport.service_url.clone(),
            alias: transport
                .alias
                .clone()
                .or_else(|| Some(room.room_id().to_string())),
        });

        // The expiry assumed from what we know about this server; checked against
        // what the server does with the delayed retraction below.
        let assumed_no_delayed = delayed_refusal_recorded_for_client(&client);
        let period_ms = if assumed_no_delayed {
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        } else {
            MEMBERSHIP_EXPIRY_MS
        };
        // Measured from created_ts (see expires_for_refresh).
        let expires_ms = expires_for_refresh(period_ms, created_ts, now_ms());
        let content = own_membership_content(
            &device_id, &user_id, focus.as_ref(), intent, created_ts,
            expires_ms);

        let result = tokio::time::timeout(
            DISCOVERY_TIMEOUT,
            room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key,
                                      content.clone()),
        )
        .await;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        let (mut ok, mut category, mut event_id) = match result {
            Ok(Ok(response)) => (true, String::new(), response.event_id.to_string()),
            Ok(Err(err)) => (
                false,
                classify_room_error(&err.to_string()).to_owned(),
                String::new(),
            ),
            Err(_) => (false, "network".to_owned(), String::new()),
        };

        // From now on this process owns the state key, so later publishes are
        // refreshes. Only on an accepted write; a failed publish's retry is still a
        // join.
        if ok {
            mark_membership_published(&publish_key);
        }

        // The delayed retraction is a `{}` PUT to our own state key, marked delayed
        // only by a query parameter (`?org.matrix.msc4140.delay=`) on the v3 state
        // endpoint. A server that ignores the parameter applies the body, which
        // retracts the membership we just published (GitHub #10: calls dropping to
        // one participant right after a refresh).
        //
        // A join is repaired by the reconciliation below; a refresh
        // (`assumed_no_delayed`) is not. So arm only where the repair can follow;
        // see `delayed_retraction_is_repairable`. element-call avoids this by
        // ordering (arm first, then write); reordering is the better but larger
        // fix.
        let mut delay_id = String::new();
        let mut delayed_category = String::new();
        if ok && delayed_retraction_is_repairable(assumed_no_delayed) {
            match schedule_delayed_leave(&client, room.room_id().as_str(),
                                         &state_key).await
            {
                Ok(id) => {
                    // A successful arm clears this server's refusal entry, so a server that
                    // gains support recovers.
                    if !id.is_empty() {
                        mark_delayed_refusal_for_client(&client, false);
                    }
                    delay_id = id;
                }
                Err(category) => delayed_category = category,
            }
        }

        // Reconcile the assumption with what the server did. We assumed delayed
        // events work and they did not: re-publish with the short expiry. There
        // is provably no armed event to disturb, and otherwise the membership
        // would claim 4 h that nothing cuts short. (With the gate above we never
        // arm while assuming refusal, so the opposite case cannot occur.)
        //
        // A server that gains MSC4140 is re-probed on the next publish allowed to
        // try, since a successful arm clears the refusal entry; a transient
        // failure never latches.
        if ok && !assumed_no_delayed && delay_id.is_empty() {
            // Latch only for a permanent category (`delayed_refusal_is_permanent`); the
            // short re-publish happens regardless.
            //
            // The server's `/versions` is a second witness, since the category comes
            // from an error string: a server that does not advertise MSC4140 cannot
            // arm one. Consulted only after a failed arm (see
            // `server_advertises_delayed_events`), and not for `rate_limited`, since
            // the latch is per server for the whole process. `network` is not excluded:
            // it is the catch-all where unrecognised wordings land. The flag is
            // unstable, so conforming servers may stop advertising it once stable.
            let transient_refusal = delayed_category == "rate_limited";
            let unadvertised = !transient_refusal
                && !server_advertises_delayed_events(&client).await;
            if delayed_refusal_is_permanent(&delayed_category) || unadvertised {
                mark_delayed_refusal_for_client(&client, true);
                // Log it: reporting `delayed_reason= "network"` on a server that published
                // `org.matrix.msc4140: false` would be misleading.
                if unadvertised && !delayed_refusal_is_permanent(&delayed_category)
                {
                    delayed_category = "delayed_unsupported".to_owned();
                }
            }
            // A second write with the short expiry, replacing our own state event under
            // the same key (one membership, created_ts unchanged). Measured from
            // created_ts like the first write; see fallback_expires_ms.
            let short = own_membership_content(
                &device_id, &user_id, focus.as_ref(), intent, created_ts,
                fallback_expires_ms(created_ts, now_ms()));
            let retry = tokio::time::timeout(
                DISCOVERY_TIMEOUT,
                room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key, short),
            )
            .await;
            match retry {
                Ok(Ok(response)) => event_id = response.event_id.to_string(),
                // The first write landed but with a 4 h expiry and no server-side cleanup,
                // the ghost this path prevents. Report failure so the caller does not
                // proceed into a call it cannot clean up after.
                Ok(Err(err)) => {
                    ok = false;
                    category = classify_room_error(&err.to_string()).to_owned();
                }
                Err(_) => {
                    ok = false;
                    category = "network".to_owned();
                }
            }
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "rtc_membership_published",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": ok,
            "category": category,
            "event_id": event_id,
            // Empty means no server-side cleanup is armed; the caller relies on
            // `expires`.
            "delay_id": delay_id,
            "delayed_category": delayed_category,
        }));
    });
    Ok(())
}

/// Read the `created_ts` of our existing membership, only if it is still
/// live. Inheriting the timestamp of an expired membership would publish
/// one born expired: others drop it and we appear with no media. A fresh
/// join after an expired ghost starts its own clock.
async fn read_own_created_ts(room: &Room, state_key: &str) -> Option<u64> {
    let raw = room
        .get_state_event(StateEventType::from(EV_MEMBER_LEGACY), state_key)
        .await
        .ok()
        .flatten()?;
    let value = raw_state_json(&raw)?;
    inheritable_created_ts(
        value.get("content")?,
        value.get("origin_server_ts").and_then(|v| v.as_u64()),
        now_ms(),
    )
}

/// The pure half of read_own_created_ts, testable without a live SDK.
fn inheritable_created_ts(
    content: &serde_json::Value,
    origin_server_ts: Option<u64>,
    now_ms: u64,
) -> Option<u64> {
    // Empty content is a retraction: a fresh join.
    if content.as_object().is_some_and(|o| o.is_empty()) {
        return None;
    }
    let created = content
        .get("created_ts")
        .and_then(|v| v.as_u64())
        .or(origin_server_ts)?;
    let expires = content
        .get("expires")
        .and_then(|v| v.as_u64())
        .unwrap_or(DEFAULT_EXPIRE_MS);
    // Saturating, so a hostile `expires` cannot make a live membership look
    // dead. A future timestamp counts as live; skew is not policed here.
    if created.saturating_add(expires) <= now_ms {
        return None;
    }
    Some(created)
}

/// Cancel one delayed event. Best effort: callers are committed, and an
/// uncancelled event would retract a membership we are about to rewrite.
async fn cancel_delayed_leave(client: &Client, delay_id: &str) {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event;
    if delay_id.is_empty() {
        return;
    }
    let request = update_delayed_event::unstable::Request::new(
        delay_id.to_owned(),
        update_delayed_event::unstable::UpdateAction::Cancel,
    );
    let _ = tokio::time::timeout(DISCOVERY_TIMEOUT, client.send(request)).await;
}

/// Whether a delayed retraction may be armed on this publish: only where
/// the reconciliation can put the membership back if the server applied the
/// `{}` outright (the `!assumed_no_delayed && delay_id.is_empty()` branch).
/// On a refresh no repair branch exists. Separate so the rule is testable.
pub(crate) fn delayed_retraction_is_repairable(assumed_no_delayed: bool) -> bool {
    !assumed_no_delayed
}

/// Schedule the server-side (MSC4140) retraction of our membership.
async fn schedule_delayed_leave(
    client: &Client,
    room_id: &str,
    state_key: &str,
) -> Result<String, String> {
    use matrix_sdk::ruma::api::client::delayed_events::{
        delayed_state_event, DelayParameters,
    };
    let room_id = matrix_sdk::ruma::RoomId::parse(room_id)
        .map_err(|_| "invalid".to_owned())?;
    // Empty content retracts the membership.
    let request = delayed_state_event::unstable::Request::new_raw(
        room_id,
        state_key.to_owned(),
        StateEventType::from(EV_MEMBER_LEGACY),
        DelayParameters::Timeout {
            timeout: Duration::from_millis(DELAYED_LEAVE_TIMEOUT_MS),
        },
        matrix_sdk::ruma::serde::Raw::new(&json!({}))
            .map_err(|_| "invalid".to_owned())?
            .cast_unchecked(),
    );
    match tokio::time::timeout(DISCOVERY_TIMEOUT, client.send(request)).await {
        Ok(Ok(response)) => {
            if response.delay_id.is_empty() {
                // No delay id: the server applied the `{}` body as an ordinary state write
                // and retracted us.
                Err("no_delay_id".to_owned())
            } else {
                Ok(response.delay_id)
            }
        }
        // A server without MSC4140 answers 404/400; cleanup falls back to `expires`.
        Ok(Err(err)) => Err(classify_delayed_leave_failure(&err)),
        Err(_) => Err("network".to_owned()),
    }
}

/// Does this error say the server does not do delayed events? Pure so the
/// wording is pinned by a test.
pub(crate) fn delayed_failure_says_unsupported(message: &str) -> bool {
    let lc = message.to_lowercase();
    // The MSC's errcode and Synapse's message. `m_max_delay_exceeded` does not
    // match: it means delayed events work but the delay was too long.
    lc.contains("m_max_delay_unsupported")
        || lc.contains("delayed events are not supported")
}

/// Why a delayed retraction could not be armed, in the vocabulary of
/// `delayed_refusal_is_permanent`.
///
/// Handles two cases `classify_room_error` cannot: a server that ignores the
/// delay parameter answers 200 with no `delay_id`, which ruma reports as a
/// deserialization failure on an accepted status (GitHub #10); and Synapse
/// with `msc4140_enabled` off answers 400 M_UNKNOWN "Delayed events are not
/// supported on this server" (see `delayed_failure_says_unsupported`).
fn classify_delayed_leave_failure(err: &matrix_sdk::HttpError) -> String {
    use matrix_sdk::ruma::api::error::FromHttpResponseError;
    use matrix_sdk::HttpError;

    // The server answered with a Matrix error (404 M_UNRECOGNIZED, 403, 429 …):
    // classify_room_error handles those.
    if err.as_client_api_error().is_some() {
        // Except Synapse's MSC4140 refusal (400 M_UNKNOWN, "Delayed events are not
        // supported on this server", `org.matrix.msc4140.errcode:
        // M_MAX_DELAY_UNSUPPORTED`), which classify_room_error would file as
        // `network`. Matched on both the errcode and the message, since the
        // errcode lives in a non-standard field; the `/versions` check in
        // `publish_membership` backs this up. Not `M_MAX_DELAY_EXCEEDED`, which
        // means delayed events are supported.
        if delayed_failure_says_unsupported(&err.to_string()) {
            return "delayed_unsupported".to_owned();
        }
        return classify_room_error(&err.to_string()).to_owned();
    }
    // A body ruma could not read on an accepted status, matched on the error
    // variant since the serde message names a field, not a status.
    if let HttpError::Api(api) = err {
        if matches!(**api, FromHttpResponseError::Deserialization(_)) {
            return "no_delay_id".to_owned();
        }
    }
    // Anything else is transient by default; a blip must never latch.
    "network".to_owned()
}

/// Restart the delayed retraction, so it keeps not-firing while we are alive.
pub(crate) fn restart_delayed_leave(
    bridge: &RustClient,
    delay_id: String,
    op_id: u64,
) -> Result<(), String> {
    update_delayed(bridge, delay_id, "restart", op_id)
}

/// Retract our membership now: send the empty content and cancel the
/// pending delayed event so nothing fires later.
pub(crate) fn retract_membership(
    bridge: &RustClient,
    room_id: String,
    delay_id: String,
    op_id: u64,
) -> Result<(), String> {
    // First, before anything that can fail: sign-out reaches here without a
    // session, and leaving is an intent the bookkeeping must follow.
    forget_room_memberships_published(&room_id);

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let user_id = client
        .user_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();
    let device_id = client
        .device_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let room_version = room
            .clone_info()
            .room_version()
            .map(|v| v.to_string())
            .unwrap_or_default();
        let state_key =
            membership_state_key(&user_id, &device_id, &room_version);

        // The next publish for this state key is a join. Forgotten before and
        // regardless of the retraction's outcome (see OWN_MEMBERSHIP_PUBLISHED).
        forget_membership_published(&membership_publish_key(
            room.room_id().as_str(), &state_key));

        // Retract first: if the cancel then fails, the worst case is a redundant
        // retraction; the other order leaves a window with neither.
        let result = tokio::time::timeout(
            DISCOVERY_TIMEOUT,
            room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key, json!({})),
        )
        .await;

        cancel_delayed_leave(&client, &delay_id).await;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match result {
            Ok(Ok(_)) => (true, String::new()),
            Ok(Err(err)) => {
                (false, classify_room_error(&err.to_string()).to_owned())
            }
            Err(_) => (false, "network".to_owned()),
        };
        enqueue(&events, json!({
            "type": "rtc_membership_retracted",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": ok,
            "category": category,
        }));
    });
    Ok(())
}

fn update_delayed(
    bridge: &RustClient,
    delay_id: String,
    action: &str,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event;
    use update_delayed_event::unstable::UpdateAction;
    let client = require_client(bridge)?;
    if sane(&delay_id, MAX_WIRE_LEN).is_none() {
        return Err("invalid delay id".to_owned());
    }
    let action = match action {
        "restart" => UpdateAction::Restart,
        "cancel" => UpdateAction::Cancel,
        _ => return Err("unknown delayed action".to_owned()),
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request =
            update_delayed_event::unstable::Request::new(delay_id, action);
        let result =
            tokio::time::timeout(DISCOVERY_TIMEOUT, client.send(request)).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match result {
            Ok(Ok(_)) => (true, String::new()),
            Ok(Err(err)) => {
                (false, classify_room_error(&err.to_string()).to_owned())
            }
            Err(_) => (false, "network".to_owned()),
        };
        enqueue(&events, json!({
            "type": "rtc_delayed_updated",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": ok,
            "category": category,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Raised hands
// ---------------------------------------------------------------------------

/// The reaction key element-call uses for a raised hand: U+1F590 plus
/// U+FE0F. Taken from element-call (`useReactionsSender.tsx`,
/// `ReactionsReader.ts`), which compares exactly this string.
pub(crate) const HAND_RAISED_KEY: &str = "\u{1F590}\u{FE0F}";

/// Raise or lower this device's hand, in element-call's wire format:
///
///   raise  → an `m.reaction` with `m.relates_to`
///            `{ rel_type: "m.annotation", event_id: <our own m.call.member
///            state event>, key: "🖐️" }`
///   lower  → a redaction of that reaction
///
/// Targeting our membership event scopes the hand to this call: a new
/// membership is a new event. `reaction_event_id` is required to lower and
/// ignored to raise.
pub(crate) fn set_hand_raised(
    bridge: &RustClient,
    room_id: String,
    membership_event_id: String,
    reaction_event_id: String,
    raised: bool,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::reaction::ReactionEventContent;
    use matrix_sdk::ruma::events::relation::Annotation;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    if raised {
        let target = match sane(&membership_event_id, MAX_WIRE_LEN) {
            Some(id) => EventId::parse(id)
                .map_err(|_| "invalid membership event id".to_owned())?,
            None => return Err("no membership event to annotate".to_owned()),
        };
        let content = ReactionEventContent::new(Annotation::new(
            target,
            HAND_RAISED_KEY.to_owned(),
        ));
        bridge.spawn_room_action(async move {
            let result =
                tokio::time::timeout(HAND_TIMEOUT, room.send(content)).await;
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let (ok, category, event_id) = match result {
                Ok(Ok(sent)) => {
                    (true, String::new(), sent.response.event_id.to_string())
                }
                Ok(Err(err)) => (
                    false,
                    classify_room_error(&err.to_string()).to_owned(),
                    String::new(),
                ),
                Err(_) => (false, "network".to_owned(), String::new()),
            };
            enqueue(&events, json!({
                "type": "rtc_hand_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "raised": true,
                "category": category,
                // Needed to redact, i.e. lower, the hand later.
                "event_id": event_id,
            }));
        });
        return Ok(());
    }

    let target = match sane(&reaction_event_id, MAX_WIRE_LEN) {
        Some(id) => EventId::parse(id)
            .map_err(|_| "invalid reaction event id".to_owned())?,
        None => return Err("no raised hand to lower".to_owned()),
    };
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(
            HAND_TIMEOUT,
            room.redact(&target, None, None),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match result {
            Ok(Ok(_)) => (true, String::new()),
            Ok(Err(err)) => {
                (false, classify_room_error(&err.to_string()).to_owned())
            }
            Err(_) => (false, "network".to_owned()),
        };
        enqueue(&events, json!({
            "type": "rtc_hand_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": ok,
            "raised": false,
            "category": category,
            "event_id": String::new(),
        }));
    });
    Ok(())
}

/// Read the hands already raised in a room's call: a hand raised before we
/// joined produces no sync event for us. element-call walks each
/// membership's annotations the same way.
///
/// Bounded: at most `MAX_HAND_PROBES` memberships, cache-first, once per
/// join (later hands come through the sync handler). A membership whose
/// annotations cannot be read contributes nothing, never a lowered hand.
pub(crate) fn read_raised_hands(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::relation::RelationType;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let now_ms = now_ms();
    bridge.spawn_room_action(async move {
        let session = read_session(&room, now_ms, false).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut hands = Vec::new();
        for member in session.members.iter().take(MAX_HAND_PROBES) {
            let Some(id) = sane(&member.event_id, MAX_WIRE_LEN) else {
                continue;
            };
            let Ok(event_id) = EventId::parse(id) else { continue };
            let loaded = room
                .load_or_fetch_event_with_relations(
                    &event_id,
                    Some(vec![RelationType::Annotation]),
                    None,
                )
                .await;
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let Ok((_target, relations)) = loaded else { continue };
            for relation in &relations {
                let Ok(parsed) = relation.raw().deserialize() else { continue };
                // The sender must own the membership: anyone may annotate anyone's
                // membership event, and without this one user could raise everybody's hand.
                if parsed.sender().as_str() != member.user_id {
                    continue;
                }
                let Ok(value) =
                    serde_json::from_str::<serde_json::Value>(relation.raw().json().get())
                else {
                    continue;
                };
                if value.get("type").and_then(|t| t.as_str())
                    != Some("m.reaction")
                {
                    continue;
                }
                // A redacted reaction has no key, so it reads as not raised.
                let key = value
                    .get("content")
                    .and_then(|c| c.get("m.relates_to"))
                    .and_then(|r| r.get("key"))
                    .and_then(|k| k.as_str());
                if key != Some(HAND_RAISED_KEY) {
                    continue;
                }
                let Some(reaction_id) =
                    value.get("event_id").and_then(|v| v.as_str())
                else {
                    continue;
                };
                hands.push(json!({
                    "user_id": member.user_id,
                    "device_id": member.device_id,
                    "rtc_identity": member.rtc_identity,
                    "membership_event_id": member.event_id,
                    "reaction_event_id": reaction_id,
                }));
                break;   // one hand per membership
            }
        }
        enqueue(&events, json!({
            "type": "rtc_hands",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "hands": hands,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Transient call reactions (io.element.call.reaction)
// ---------------------------------------------------------------------------

/// element-call's transient reaction event type (`src/reactions/index.ts`
/// `ElementCallReactionEventType`). The string lives in the event-content
/// derive; this constant is asserted against it in tests.
#[allow(dead_code)]
pub(crate) const EV_CALL_REACTION: &str = "io.element.call.reaction";

/// The reaction pairs element-call knows, `(name, emoji)`, transcribed from
/// `element-call/src/reactions/index.ts` (`ReactionSet`).
///
/// Closed for sending: element-call looks the sound up by `name`, so an
/// unknown name reaches Element as a silent generic reaction. Not applied
/// to received reactions, so a new element-call entry is still shown;
/// inbound emoji are bounded by [`reaction_emoji`].
const ELEMENT_CALL_REACTIONS: &[(&str, &str)] = &[
    ("thumbsup", "\u{1F44D}"),
    ("party", "\u{1F389}"),
    ("clapping", "\u{1F44F}"),
    ("dog", "\u{1F436}"),
    ("cat", "\u{1F431}"),
    ("lightbulb", "\u{1F4A1}"),
    ("crickets", "\u{1F997}"),
    ("thumbsdown", "\u{1F44E}"),
    ("dizzy", "\u{1F635}\u{200D}\u{1F4AB}"),
    ("ok", "\u{1F44C}"),
    ("heart", "\u{1F970}"),
    ("laugh", "\u{1F604}"),
    ("deer", "\u{1F98C}"),
    ("rock", "\u{1F918}"),
    ("wave", "\u{1F44B}"),
    ("drum", "\u{1F941}"),
];

/// Send timeout. Reactions are fire-and-forget; this only bounds the task.
const REACTION_TIMEOUT: Duration = Duration::from_secs(10);

/// Byte bound on an inbound `emoji` field. element-call sends 4 to 11
/// bytes; this allows longer legitimate sequences and refuses payloads.
const MAX_REACTION_EMOJI_LEN: usize = 64;
/// Code-point bound on the cluster [`first_emoji_cluster`] builds (a family
/// sequence is seven), which also guarantees termination on hostile input.
const MAX_REACTION_CLUSTER_CHARS: usize = 8;

/// element-call's transient reaction:
///
///   `{ "m.relates_to": { "rel_type": "m.reference",
///                        "event_id": <the sender's own m.call.member
///                                     state event> },
///      "emoji": "\u{1F44D}", "name": "thumbsup" }`
///
/// Referencing the membership scopes the reaction to one call and
/// participant. The sender must own that membership; that check lives in
/// C++ (`RtcController::identityForMembership`, shared with hands).
///
/// `relates_to` and `emoji` are required, so malformed content never
/// reaches the handler. serde does not verify an internally tagged
/// struct's tag inbound, so `rel_type: m.annotation` still deserializes as
/// a `Reference`; the type pins what we send, and element-call's reader
/// ignores `rel_type` too. `name` is optional inbound; we always send it.
#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "io.element.call.reaction", kind = MessageLike)]
pub(crate) struct ElementCallReactionEventContent {
    pub emoji: String,
    #[serde(default)]
    pub name: String,
    #[serde(rename = "m.relates_to")]
    pub relates_to: Reference,
}

/// Reduce an inbound `emoji` to one drawable cluster, or nothing: bounded
/// by [`sane`], then cut to the first cluster, as element-call does with
/// `Intl.Segmenter`, so a packed sentence shows as one glyph.
fn reaction_emoji(raw: &str) -> Option<String> {
    let bounded = sane(raw.trim(), MAX_REACTION_EMOJI_LEN)?;
    let cluster = first_emoji_cluster(bounded);
    if cluster.is_empty() {
        return None;
    }
    Some(cluster)
}

/// True for a regional indicator symbol letter (half of a flag).
fn is_regional_indicator(value: char) -> bool {
    ('\u{1F1E6}'..='\u{1F1FF}').contains(&value)
}

/// The first grapheme-like cluster of `value`: the first character plus
/// following code points that cannot stand alone (variation selectors,
/// ZWJ and what they join, skin tones, keycap, combining marks, tag
/// sequences, the second regional indicator). Covers element-call's set.
///
/// Not a Unicode segmentation implementation (`unicode-segmentation` is not
/// a direct dependency); where it differs it returns at most one code point
/// too few.
fn first_emoji_cluster(value: &str) -> String {
    let mut out = String::new();
    let mut chars = value.chars().peekable();
    let Some(first) = chars.next() else {
        return out;
    };
    out.push(first);
    // Set after a ZWJ, which binds the next code point into the cluster.
    let mut after_join = false;
    while out.chars().count() < MAX_REACTION_CLUSTER_CHARS {
        let Some(&next) = chars.peek() else { break };
        let continues = after_join
            || matches!(next,
                '\u{200D}'                  // zero-width joiner
                | '\u{FE00}'..='\u{FE0F}'   // variation selectors
                | '\u{1F3FB}'..='\u{1F3FF}' // skin tone modifiers
                | '\u{20E3}'                // combining enclosing keycap
                | '\u{0300}'..='\u{036F}'   // combining diacritical marks
                | '\u{E0020}'..='\u{E007F}' // tag characters (flag sequences)
            )
            || (out.chars().count() == 1
                && is_regional_indicator(first)
                && is_regional_indicator(next));
        if !continues {
            break;
        }
        after_join = next == '\u{200D}';
        out.push(next);
        chars.next();
    }
    out
}

/// Send one transient call reaction.
///
/// `membership_event_id` is this device's own `m.call.member` event, as
/// observed by RtcController, so a refresh's replacement is referenced (as
/// in `set_hand_raised`). The `(name, emoji)` pair must be in
/// [`ELEMENT_CALL_REACTIONS`].
///
/// No backlog sweep, unlike hands: reactions are transient, and sweeping at
/// join would resurrect stale ones. element-call does not either.
pub(crate) fn send_call_reaction(
    bridge: &RustClient,
    room_id: String,
    membership_event_id: String,
    emoji: String,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;

    let target = match sane(&membership_event_id, MAX_WIRE_LEN) {
        Some(id) => EventId::parse(id)
            .map_err(|_| "invalid membership event id".to_owned())?,
        None => return Err("no membership event to reference".to_owned()),
    };
    let Some((name, emoji)) = ELEMENT_CALL_REACTIONS
        .iter()
        .find(|(known_name, known_emoji)| {
            *known_name == name && *known_emoji == emoji
        })
    else {
        return Err("unknown reaction".to_owned());
    };

    let content = ElementCallReactionEventContent {
        emoji: (*emoji).to_owned(),
        name: (*name).to_owned(),
        relates_to: Reference::new(target),
    };

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result =
            tokio::time::timeout(REACTION_TIMEOUT, room.send(content)).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, event_id) = match result {
            Ok(Ok(sent)) => {
                (true, String::new(), sent.response.event_id.to_string())
            }
            Ok(Err(err)) => (
                false,
                classify_room_error(&err.to_string()).to_owned(),
                String::new(),
            ),
            Err(_) => (false, "network".to_owned(), String::new()),
        };
        // The generic RTC send lane. Nothing to keep: reactions are never redacted
        // and tiles are drawn only from arrived events.
        enqueue(&events, json!({
            "type": "rtc_send_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": ok,
            "category": category,
            "event_id": event_id,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Media encryption keys (io.element.call.encryption_keys)
// ---------------------------------------------------------------------------

/// The to-device type Element uses for call media keys.
pub(crate) const EV_CALL_KEYS: &str = "io.element.call.encryption_keys";
/// Media keys are 32 raw bytes; LiveKit's HKDF turns them into AES-128-GCM.
const MEDIA_KEY_BYTES: usize = 32;
/// Highest key index our sender stamps (SfuCallController allocates
/// `(index + 1) % 16`).
///
/// Received keys are not held to this: matrix-js-sdk rotates modulo 256 and
/// element-call's ring holds 256, so the received index is a whole `u8`.
const MAX_SEND_KEY_INDEX: u8 = 15;

/// How long a device-list refresh is trusted before the next media-key
/// distribution pays for another.
pub(crate) const DEVICE_REFRESH_TTL_SECS: u64 = 60;

/// Is a `/keys/query` owed for this user before encrypting to their
/// devices? `None` means never refreshed in this process. Pure for tests.
pub(crate) fn device_refresh_due(elapsed_secs: Option<u64>) -> bool {
    match elapsed_secs {
        None => true,
        Some(secs) => secs >= DEVICE_REFRESH_TTL_SECS,
    }
}

/// Users whose device list was refreshed, and when. Bounded by the people
/// this process has been in calls with.
fn device_refresh_marks()
    -> &'static std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

/// When we last reported that an Olm to-device message from a peer would
/// not decrypt. Kept apart from [`device_refresh_marks`] so reporting and
/// refreshing cannot suppress each other. Only written for users already
/// in that map, and capped.
fn undecryptable_report_marks()
    -> &'static std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

/// Ceiling on [`undecryptable_report_marks`], like the C++ cooldown table.
const MAX_REPORT_MARKS: usize = 256;

/// Should an undecryptable Olm to-device message be reported as a lost
/// media key? It carries no type, so this only reports for senders we have
/// distributed a media key to ([`device_refresh_marks`]); for anyone else
/// it is not a call fault. Pure for tests.
pub(crate) fn undecryptable_key_report_due(
    is_media_key_peer: bool,
    since_last_report_secs: Option<u64>,
) -> bool {
    is_media_key_peer && device_refresh_due(since_last_report_secs)
}

/// Send our current media key to the devices in the call, Olm-encrypted per
/// device (`encrypt_and_send_raw_to_device`), so the homeserver never sees
/// it. `targets` are `(user_id, device_id)` pairs from the observed
/// membership: only devices that declared themselves present.
pub(crate) fn send_media_key(
    bridge: &RustClient,
    room_id: String,
    key_base64: String,
    key_index: u8,
    targets_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    if key_index > MAX_SEND_KEY_INDEX {
        return Err("key index out of range".to_owned());
    }
    // Parsed first so a malformed list fails synchronously.
    let targets: Vec<(String, String)> =
        serde_json::from_str::<Vec<serde_json::Value>>(&targets_json)
            .map_err(|_| "invalid targets".to_owned())?
            .into_iter()
            .filter_map(|value| {
                let user = value.get("user_id")?.as_str()?;
                let device = value.get("device_id")?.as_str()?;
                Some((
                    sane(user, MAX_WIRE_LEN)?.to_owned(),
                    sane(device, MAX_WIRE_LEN)?.to_owned(),
                ))
            })
            .take(MAX_MEMBERS)
            .collect();
    if targets.is_empty() {
        return Err("no targets".to_owned());
    }

    let own_user = client
        .user_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();
    let own_device = client
        .device_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let content = json!({
            "keys": { "index": key_index, "key": key_base64 },
            "member": {
                "id": format!("{own_user}:{own_device}"),
                "claimed_device_id": own_device,
            },
            "room_id": room.room_id().to_string(),
            "session": {
                "application": APPLICATION_CALL,
                "call_id": "",
                "scope": "m.room",
            },
            "sent_ts": u64::from(MilliSecondsSinceUnixEpoch::now().get()),
        });

        // Resolve target devices. An unresolvable device is skipped, never
        // substituted: sending a key to the wrong device is worse.
        let mut resolved = Vec::new();
        for (user, device) in &targets {
            // Never send our own device its own key.
            if user == &own_user && device == &own_device {
                continue;
            }
            let Ok(user_id) = matrix_sdk::ruma::UserId::parse(user) else {
                continue;
            };
            let device_id: matrix_sdk::ruma::OwnedDeviceId =
                device.as_str().into();
            // Refresh before the lookup, not only when the device is missing: a
            // device present with rotated keys makes us encrypt to the old identity
            // key, the recipient drops it ("Olm event doesn't contain a ciphertext for
            // our key"), and neither end notices. Rate-limited per user.
            let refresh_due = {
                let marks = device_refresh_marks().lock().ok();
                match marks {
                    Some(marks) => device_refresh_due(
                        marks.get(user).map(|at| at.elapsed().as_secs()),
                    ),
                    // A poisoned lock must not disable the refresh.
                    None => true,
                }
            };
            if refresh_due {
                let _ = client.encryption().request_user_identity(&user_id).await;
                if let Ok(mut marks) = device_refresh_marks().lock() {
                    marks.insert(user.clone(), std::time::Instant::now());
                }
            }
            let mut found =
                client.encryption().get_device(&user_id, &device_id).await;
            // `get_device` only reads the store; a peer whose keys were never
            // downloaded is absent, and the key would go to nobody.
            // `request_user_identity` does a real `/keys/query`, so the second lookup
            // succeeds. Only on a miss.
            if !matches!(found, Ok(Some(_))) {
                let _ = client.encryption().request_user_identity(&user_id).await;
                found =
                    client.encryption().get_device(&user_id, &device_id).await;
            }
            if let Ok(Some(device)) = found {
                resolved.push(device);
            }
        }

        let (ok, category, delivered) = if resolved.is_empty() {
            (false, "no_devices".to_owned(), 0usize)
        } else {
            let total = resolved.len();
            let raw = matrix_sdk::ruma::serde::Raw::new(&content)
                .map(|raw| raw.cast_unchecked())
                .ok();
            match raw {
                Some(raw) => {
                    let result = client
                        .encryption()
                        .encrypt_and_send_raw_to_device(
                            resolved.iter().collect(),
                            EV_CALL_KEYS,
                            raw,
                            CollectStrategy::AllDevices,
                        )
                        .await;
                    match result {
                        // The SDK returns the devices it could not reach, so partial delivery is
                        // visible.
                        Ok(failures) => (
                            failures.len() < total,
                            if failures.is_empty() {
                                String::new()
                            } else {
                                "partial".to_owned()
                            },
                            total - failures.len(),
                        ),
                        Err(err) => (
                            false,
                            classify_room_error(&err.to_string()).to_owned(),
                            0,
                        ),
                    }
                }
                None => (false, "invalid".to_owned(), 0),
            }
        };

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        // Counts only; the key is never enqueued or logged.
        enqueue(&events, json!({
            "type": "rtc_key_sent",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": ok,
            "category": category,
            "delivered": delivered,
            "key_index": key_index,
        }));
    });
    Ok(())
}

/// Inbound media key, decrypted by Olm. The sender and Olm-verified device
/// are trusted; the content's `member` block is a claim, used only to fill
/// in an id.
#[allow(unexpected_cfgs)]
#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "io.element.call.encryption_keys", kind = ToDevice)]
pub(crate) struct CallEncryptionKeysEventContent {
    pub keys: MediaKeyEntry,
    pub member: MediaKeyMember,
    pub room_id: String,
    #[serde(default)]
    pub sent_ts: Option<u64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(crate) struct MediaKeyEntry {
    pub index: u8,
    pub key: String,
}

/// The fields of a received media key after validation.
pub(crate) struct ValidatedMediaKey<'a> {
    pub index: u8,
    pub key: &'a str,
    pub room_id: &'a str,
    /// The Olm-verified device, never the content's claim.
    pub device_id: &'a str,
}

/// Validate a received media key's fields. Pure for tests. The index is
/// bounded by its type (`u8`, the C++ ring size); the claimed device must
/// equal the Olm device.
pub(crate) fn validate_media_key<'a>(
    content: &'a CallEncryptionKeysEventContent,
    olm_device_id: &'a str,
) -> Result<ValidatedMediaKey<'a>, &'static str> {
    let key = sane(&content.keys.key, 512).ok_or("key field is not usable")?;
    let room_id =
        sane(&content.room_id, MAX_WIRE_LEN).ok_or("room id is not usable")?;
    let claimed = sane(&content.member.claimed_device_id, MAX_WIRE_LEN)
        .ok_or("claimed device id is not usable")?;
    let device_id = sane(olm_device_id, MAX_WIRE_LEN)
        .ok_or("olm device id is not usable")?;
    if claimed != device_id {
        return Err("claimed device does not match the olm device");
    }
    Ok(ValidatedMediaKey { index: content.keys.index, key, room_id, device_id })
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(crate) struct MediaKeyMember {
    #[serde(default)]
    pub id: Option<String>,
    pub claimed_device_id: String,
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

/// Handlers registered for the sync loop's lifetime.
pub(crate) struct RtcHandlerGuards {
    _guards: Vec<EventHandlerDropGuard>,
}

/// Register MatrixRTC observation. Both handlers are thin:
///
/// * membership changes enqueue a payload-free poke, and C++ re-reads the
///   session, so there is one parse path;
/// * the MSC4075 notification feeds the `call_rtc_*` lane `calls.rs` owns,
///   reusing its ring policy, ignore check and incoming-call surface.
pub(crate) fn register_rtc_handlers(
    client: &Client,
    events: &Arc<std::sync::Mutex<std::collections::VecDeque<String>>>,
    timelines: &Arc<crate::timeline::TimelineRegistry>,
) -> RtcHandlerGuards {
    let mut guards = Vec::new();

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |_ev: SyncLegacyRtcMemberEvent, room: Room| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    enqueue(&events, json!({
                        "type": "rtc_session_changed",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    // Raised hands: an `m.reaction` annotating the raiser's own
    // `m.call.member` event, lowered by redacting it. Not filtered by room:
    // most reactions fail the key check immediately, and a room filter would
    // need call state and could miss a hand raised right after joining.
    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: matrix_sdk::ruma::events::reaction::OriginalSyncReactionEvent,
                  room: Room| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    if ev.content.relates_to.key != HAND_RAISED_KEY {
                        return;
                    }
                    // C++ matches this to a membership and requires the sender to own it.
                    enqueue(&events, json!({
                        "type": "rtc_hand_changed",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "sender": ev.sender.to_string(),
                        "membership_event_id":
                            ev.content.relates_to.event_id.to_string(),
                        "reaction_event_id": ev.event_id.to_string(),
                        "raised": true,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: matrix_sdk::ruma::events::room::redaction::SyncRoomRedactionEvent,
                  room: Room| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    // A redaction only names what it removed, so every one is forwarded and
                    // C++ checks whether it is a tracked reaction id.
                    let Some(redacts) = ev.as_original()
                        .and_then(|original| original.redacts.as_ref())
                        .or_else(|| ev.as_original().and_then(|o| o.content.redacts.as_ref()))
                    else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "rtc_hand_changed",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "sender": ev.sender().to_string(),
                        "reaction_event_id": redacts.to_string(),
                        "raised": false,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    // element-call transient reactions. Not filtered by room (see hands). The
    // emoji is bounded and cut to one cluster here; attribution is done in C++
    // (`RtcController::identityForMembership`), not duplicated here.
    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncElementCallReactionEvent, room: Room| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    // Nothing drawable: dropped, as element-call does.
                    let Some(emoji) = reaction_emoji(&ev.content.emoji) else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "rtc_call_reaction",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "sender": ev.sender.to_string(),
                        "membership_event_id":
                            ev.content.relates_to.event_id.to_string(),
                        "emoji": emoji,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncMsc4075RtcNotificationEvent,
                  room: Room,
                  client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client.user_id().is_some_and(|user| user == ev.sender);
                    // A ring is the strongest evidence of a session in a room we are not
                    // viewing, so it licenses one `/state`. Recorded for our own ring too.
                    note_rtc_ring(room.room_id().as_str());
                    let notification_type = match ev.content.notification_type.as_str()
                    {
                        "ring" => "ring",
                        _ => "notification",
                    };
                    let intent = match ev.content.call_intent.as_deref() {
                        Some("video") => "video",
                        _ => "audio",
                    };
                    enqueue(&events, json!({
                        "type": "call_rtc_notification",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "origin_server_ts": u64::from(ev.origin_server_ts.get()),
                        "sender_ts": u64::from(ev.content.sender_ts.get()),
                        "lifetime_ms": ev.content.lifetime
                            .min(MAX_NOTIFICATION_LIFETIME_MS),
                        "call_intent": intent,
                        "notification_type": notification_type,
                        // MatrixRTC lane, not legacy m.call.invite, so one call cannot ring twice.
                        "rtc": true,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: CallEncryptionKeysEvent, encryption: Option<EncryptionInfo>| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    // Every discard logs its reason: otherwise "the key never arrived" and "the
                    // key arrived and was discarded" look identical (every frame dropped, no
                    // log). Sender and a fixed reason only, never key material.
                    macro_rules! discard {
                        ($reason:expr) => {{
                            enqueue(&events, json!({
                                "type": "rtc_key_discarded",
                                "lifecycle": timelines.lifecycle(),
                                "reason": $reason,
                                "sender": ev.sender.to_string(),
                            }));
                            return;
                        }};
                    }
                    // Olm or nothing. `None` means the event arrived in the clear, which any
                    // user can send with any `sender`. Only Olm decryption vouches for sender
                    // and device, which the key ring is keyed on. Plaintext keys are refused.
                    let Some(encryption) = encryption else {
                        discard!("not encrypted");
                    };
                    // Olm decrypted it but the SDK could not attribute it to a known device.
                    let Some(sender_device) = encryption.sender_device.as_ref()
                    else {
                        discard!("sending device not resolved");
                    };
                    if encryption.sender != ev.sender {
                        discard!("olm sender does not match the envelope");
                    }
                    let valid = match validate_media_key(
                        &ev.content, sender_device.as_str())
                    {
                        Ok(valid) => valid,
                        Err(reason) => discard!(reason),
                    };
                    let (key, room_id, device_id) =
                        (valid.key, valid.room_id, valid.device_id);
                    enqueue(&events, json!({
                        "type": "rtc_key_received",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room_id,
                        // Both vouched for by Olm. `claimed_device_id` keeps its name for the
                        // consumer but carries the verified device.
                        "sender": ev.sender.to_string(),
                        "claimed_device_id": device_id,
                        "key_index": valid.index,
                        // The key: C++ memory only, never QML, never logged. Base64 as received.
                        "key": key,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    // A media key whose Olm message failed never reaches the handler above:
    // matrix-sdk hands undecryptable to-device events to handlers as the
    // `m.room.encrypted` envelope (event_handler/mod.rs), so the typed handler
    // is never called. This reports that loss for peers we sent a media key
    // to; it does not retry or soften a refusal.
    //
    // It cannot tell the cause apart (`ToDeviceUnableToDecryptInfo` never
    // reaches handlers):
    //
    //   * the sender encrypted to an identity key we no longer hold
    //     (`MissingCiphertext`); the SDK only re-establishes sessions on
    //     `SessionWedged`, so rejoining does not fix it;
    //   * the sender device is unknown and the message lacked MSC4147
    //     `sender_device_keys` (`MissingSigningKey`);
    //   * the sender device fails the trust requirement
    //     (`UnverifiedSenderDevice`), a correct refusal.
    //
    // Sender only (server-asserted here, fine for a diagnostic). lib.rs has a
    // second, general handler on this type (`ToDeviceRoomEncryptedEvent`); both
    // are intentional.
    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: matrix_sdk::ruma::events::room::encrypted::ToDeviceRoomEncryptedEvent| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let Some(sender) = sane(ev.sender.as_str(), MAX_WIRE_LEN)
                    else {
                        return;
                    };
                    // A poisoned lock must not silence the diagnostic; fail towards reporting.
                    // C++ applies a 60 s per-(sender, reason) cooldown.
                    let is_peer = match device_refresh_marks().lock() {
                        Ok(marks) => marks.contains_key(sender),
                        Err(_) => true,
                    };
                    let elapsed = match undecryptable_report_marks().lock() {
                        Ok(marks) => marks
                            .get(sender)
                            .map(|at| at.elapsed().as_secs()),
                        Err(_) => None,
                    };
                    if !undecryptable_key_report_due(is_peer, elapsed) {
                        return;
                    }
                    if let Ok(mut marks) = undecryptable_report_marks().lock()
                    {
                        // Refresh an existing mark; add one only while there is room.
                        if marks.len() < MAX_REPORT_MARKS
                            || marks.contains_key(sender)
                        {
                            marks.insert(
                                sender.to_owned(),
                                std::time::Instant::now(),
                            );
                        }
                    }
                    enqueue(&events, json!({
                        "type": "rtc_key_discarded",
                        "lifecycle": timelines.lifecycle(),
                        "reason": "an olm to-device message from them would \
                                   not decrypt on this device, so if it was \
                                   carrying the key the key never arrived",
                        "sender": sender,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    RtcHandlerGuards { _guards: guards }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::events::StaticEventContent;

    // A received key's index is the whole byte, bounded by type: 200 and 255
    // parse, 256 and -1 do not deserialize. (C++ enforces the same bound.)
    #[test]
    fn received_media_key_index_is_bounded_by_its_type() {
        let content = |index: serde_json::Value| {
            json!({
                "keys": { "index": index, "key": "a2V5" },
                "member": { "claimed_device_id": "DEV" },
                "room_id": "!r:x",
            })
        };
        for ok in [0u64, 15, 16, 200, 255] {
            let parsed: CallEncryptionKeysEventContent =
                serde_json::from_value(content(json!(ok)))
                    .unwrap_or_else(|e| panic!("index {ok} must parse: {e}"));
            assert_eq!(u64::from(parsed.keys.index), ok);
        }
        for bad in [json!(256), json!(-1), json!(1000), json!("7")] {
            assert!(
                serde_json::from_value::<CallEncryptionKeysEventContent>(
                    content(bad.clone())).is_err(),
                "index {bad} must not deserialise");
        }
        // Our sender still stamps 0..=15.
        assert_eq!(MAX_SEND_KEY_INDEX, 15);
    }

    // Indices 16 and 255 pass validation; a device mismatch is refused.
    #[test]
    fn media_key_validation_accepts_the_whole_index_byte() {
        let content = |index: u8, claimed: &str| -> CallEncryptionKeysEventContent {
            serde_json::from_value(json!({
                "keys": { "index": index, "key": "a2V5" },
                "member": { "claimed_device_id": claimed },
                "room_id": "!r:x",
            }))
            .unwrap()
        };
        for index in [0u8, 15, 16, 200, 255] {
            let c = content(index, "DEV");
            let valid = validate_media_key(&c, "DEV")
                .unwrap_or_else(|e| panic!("index {index} refused: {e}"));
            assert_eq!(valid.index, index);
            assert_eq!(valid.device_id, "DEV");
            assert_eq!(valid.room_id, "!r:x");
        }
        let lying = content(16, "OTHER");
        assert_eq!(
            validate_media_key(&lying, "DEV").err(),
            Some("claimed device does not match the olm device"));
        let c = content(1, "DEV");
        assert_eq!(validate_media_key(&c, "").err(),
                   Some("olm device id is not usable"));
    }

    fn session_content() -> serde_json::Value {
        json!({
            "application": "m.call",
            "call_id": "",
            "scope": "m.room",
            "device_id": "DEVICE",
            "membershipID": "@a:x:DEVICE",
            "expires": 14_400_000u64,
            "focus_active": { "type": "livekit", "focus_selection": "oldest_membership" },
            "foci_preferred": [
                { "type": "livekit", "livekit_service_url": "https://sfu.example.org" }
            ]
        })
    }

    #[test]
    fn parses_a_real_element_session_membership() {
        let member = parse_session_membership(&session_content(), "@a:x", 1_000)
            .expect("valid membership");
        assert_eq!(member.user_id, "@a:x");
        assert_eq!(member.device_id, "DEVICE");
        assert_eq!(member.rtc_identity, "@a:x:DEVICE");
        // `call_id: ""` normalises to the slot vocabulary.
        assert_eq!(member.slot_id, "m.call#ROOM");
        assert_eq!(member.expires_at_ms, 1_000 + 14_400_000);
        assert_eq!(member.foci.len(), 1);
        assert_eq!(member.foci[0].service_url, "https://sfu.example.org/");
        assert_eq!(member.kind, MembershipKind::Session);
    }

    #[test]
    fn membership_identity_defaults_to_user_colon_device() {
        let mut content = session_content();
        content.as_object_mut().unwrap().remove("membershipID");
        let member =
            parse_session_membership(&content, "@a:x", 1_000).expect("valid");
        assert_eq!(member.rtc_identity, "@a:x:DEVICE");
    }

    #[test]
    fn empty_content_is_a_leave_not_a_participant() {
        assert!(parse_session_membership(&json!({}), "@a:x", 1).is_none());
    }

    #[test]
    fn membership_without_focus_active_is_refused() {
        let mut content = session_content();
        content.as_object_mut().unwrap().remove("focus_active");
        assert!(parse_session_membership(&content, "@a:x", 1).is_none());
    }

    #[test]
    fn membership_with_control_characters_is_dropped_whole() {
        let mut content = session_content();
        content["device_id"] = json!("DEV\u{0}ICE");
        assert!(parse_session_membership(&content, "@a:x", 1).is_none());
    }

    #[test]
    fn non_call_application_is_ignored() {
        let mut content = session_content();
        content["application"] = json!("m.something.else");
        assert!(parse_session_membership(&content, "@a:x", 1).is_none());
    }

    #[test]
    fn hostile_expires_cannot_wrap_into_the_past() {
        let mut content = session_content();
        content["expires"] = json!(u64::MAX);
        let member =
            parse_session_membership(&content, "@a:x", 5_000).expect("valid");
        // Neither wrapped into the past nor immortal: capped relative to the event.
        assert_eq!(member.expires_at_ms, 5_000 + MAX_EXPIRE_MS);
        assert_eq!(aggregate_session(vec![member.clone()], 10_000).len(), 1);
        assert_eq!(
            aggregate_session(vec![member], 5_000 + MAX_EXPIRE_MS + 1).len(),
            0
        );
    }

    /// The focus-takeover shape: `created_ts: 0` and `expires: u64::MAX`. The
    /// join is clamped to at most a day old and the deadline to a day past the
    /// event, so without a refresh it ages out.
    #[test]
    fn a_backdated_immortal_membership_is_bounded_by_its_own_event() {
        let mut content = session_content();
        content["created_ts"] = json!(0u64);
        content["expires"] = json!(u64::MAX);
        let now = 1_700_000_000_000u64;
        let member =
            parse_session_membership(&content, "@mallory:x", now).expect("valid");
        assert_eq!(member.created_ts, now - MAX_BACKDATE_MS);
        assert_eq!(member.expires_at_ms, now + MAX_EXPIRE_MS);
        assert_eq!(aggregate_session(vec![member.clone()], now).len(), 1);
        assert!(aggregate_session(vec![member], now + MAX_EXPIRE_MS + 1).is_empty());
    }

    /// A refresh of a call older than a day (`created_ts` preserved, `expires`
    /// as expires_for_refresh() writes it) stays live.
    #[test]
    fn a_refresh_of_a_day_old_call_is_still_live() {
        let created = 1_700_000_000_000u64;
        let now = created + 25 * 60 * 60 * 1000; // call age 25 h
        let mut content = session_content();
        content["created_ts"] = json!(created);
        content["expires"] = json!(expires_for_refresh(MEMBERSHIP_EXPIRY_MS, Some(created), now));
        let member = parse_session_membership(&content, "@a:x", now).expect("valid");
        assert!(member.expires_at_ms > now, "refresh must extend past now");
        assert_eq!(aggregate_session(vec![member.clone()], now).len(), 1);
        // Still ordered as the older member against someone joining now.
        let younger = parse_session_membership(&session_content(), "@b:y", now).expect("valid");
        let ordered = aggregate_session(vec![younger, member], now);
        assert_eq!(ordered[0].user_id, "@a:x");
    }

    /// A future join time is pulled back to the envelope's timestamp, so the
    /// expiry cannot be pushed out of reach.
    #[test]
    fn a_future_created_ts_is_clamped_to_the_envelope() {
        let mut content = session_content();
        content["created_ts"] = json!(u64::MAX);
        let member =
            parse_session_membership(&content, "@a:x", 5_000).expect("valid");
        assert_eq!(member.created_ts, 5_000 + MAX_CREATED_TS_SKEW_MS);
        assert_eq!(
            member.expires_at_ms,
            5_000 + MAX_CREATED_TS_SKEW_MS + 14_400_000
        );
    }

    /// `membershipID` is content; the identity is derived from the vouched-for
    /// sender, so an impostor cannot claim another participant's identity.
    #[test]
    fn a_membership_cannot_claim_another_participants_identity() {
        let mut content = session_content();
        content["membershipID"] = json!("@alice:x:ALICEDEV");
        let member = parse_session_membership(&content, "@mallory:x", 1_000)
            .expect("valid");
        assert_eq!(member.rtc_identity, "@mallory:x:DEVICE");
    }

    #[test]
    fn missing_expires_falls_back_to_four_hours() {
        let mut content = session_content();
        content.as_object_mut().unwrap().remove("expires");
        let member =
            parse_session_membership(&content, "@a:x", 1_000).expect("valid");
        assert_eq!(member.expires_at_ms, 1_000 + DEFAULT_EXPIRE_MS);
    }

    #[test]
    fn foci_are_read_from_the_well_known_keys_element_uses() {
        // Foci come from `.well-known/matrix/client` under
        // `org.matrix.msc4143.rtc_foci` (stable alias `m.rtc_foci`), as ruma models
        // and Element Call reads. Pinned as literal key names.
        let unstable = serde_json::json!({
            "m.homeserver": { "base_url": "https://matrix.example.org" },
            "org.matrix.msc4143.rtc_foci": [
                {
                    "type": "livekit",
                    "livekit_service_url": "https://sfu.example.org/",
                },
            ],
        });
        let pick = |value: &serde_json::Value| -> Vec<LivekitTransport> {
            let list = value
                .get("org.matrix.msc4143.rtc_foci")
                .or_else(|| value.get("m.rtc_foci"))
                .and_then(|list| list.as_array())
                .cloned()
                .unwrap_or_default();
            list.iter().filter_map(parse_transport).collect()
        };
        let found = pick(&unstable);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].service_url, "https://sfu.example.org/");

        // The stable alias works too.
        let stable = serde_json::json!({
            "m.rtc_foci": [
                { "type": "livekit",
                  "livekit_service_url": "https://sfu.example.org/" },
            ],
        });
        assert_eq!(pick(&stable).len(), 1);

        // The old invented key is not a source.
        let wrong = serde_json::json!({
            "rtc_transports": [
                { "type": "livekit",
                  "livekit_service_url": "https://sfu.example.org/" },
            ],
        });
        assert!(pick(&wrong).is_empty());
    }

    #[test]
    fn http_transport_is_refused_and_https_survives() {
        assert!(parse_transport(&json!({
            "type": "livekit",
            "livekit_service_url": "http://sfu.example.org"
        }))
        .is_none());
        assert!(parse_transport(&json!({
            "type": "livekit",
            "livekit_service_url": "https://sfu.example.org"
        }))
        .is_some());
    }

    // A 404/400/405 (`unsupported`) is a definitive "no MatrixRTC here", not
    // "could not check", or discovery re-runs on every room change.
    #[test]
    fn a_homeserver_without_matrixrtc_counts_as_having_answered() {
        for status in [400u16, 404, 405] {
            assert_eq!(status_category(status), "unsupported");
            assert!(
                discovery_answer_is_definitive(status_category(status)),
                "HTTP {status} is a homeserver saying it has no MatrixRTC, \
                 not a check that failed"
            );
        }
        // A normal answer is definitive, even an empty list.
        assert!(discovery_answer_is_definitive(""));

        // Genuine failures stay open, or a 500 would be read as "no calling".
        for category in [
            "forbidden",
            "rate_limited",
            "server_error",
            "unknown",
            "network",
            "not_found",
            "unrecognized",
        ] {
            assert!(
                !discovery_answer_is_definitive(category),
                "`{category}` is a failed check and must not read as a \
                 settled answer"
            );
        }
        // Pin every status category so a new mapping cannot silently become
        // definitive.
        for status in [401u16, 403, 429, 500, 503, 418] {
            assert!(!discovery_answer_is_definitive(status_category(status)));
        }
    }

    #[test]
    fn unknown_transport_type_is_skipped_not_guessed() {
        assert!(parse_transport(&json!({
            "type": "jitsi",
            "livekit_service_url": "https://sfu.example.org"
        }))
        .is_none());
    }

    #[test]
    fn transport_alias_is_carried_when_present() {
        let transport = parse_transport(&json!({
            "type": "livekit",
            "livekit_service_url": "https://sfu.example.org",
            "livekit_alias": "!room:x"
        }))
        .expect("valid");
        assert_eq!(transport.alias.as_deref(), Some("!room:x"));
    }

    fn member_at(user: &str, device: &str, created: u64, expires: u64) -> RtcMember {
        RtcMember {
            user_id: user.to_owned(),
            device_id: device.to_owned(),
            rtc_identity: format!("{user}:{device}"),
            slot_id: "m.call#ROOM".to_owned(),
            intent: "audio",
            created_ts: created,
            expires_at_ms: expires,
            foci: Vec::new(),
            kind: MembershipKind::Session,
            display_name: String::new(),
            avatar_mxc: String::new(),
            event_id: String::new(),
        }
    }

    // ---------------------------------------------------------------------
    // Which view of the room's state a session read may trust (GitHub #10:
    // the participant list dropped while the peer was still in the call, so
    // media keys went to an empty target set).
    // ---------------------------------------------------------------------

    fn membership_event(
        sender: &str,
        device: &str,
        origin_server_ts: u64,
        expires: u64,
    ) -> serde_json::Value {
        json!({
            "type": EV_MEMBER_LEGACY,
            "sender": sender,
            "state_key": format!("_{sender}_{device}_m.call"),
            "origin_server_ts": origin_server_ts,
            "content": {
                "application": "m.call",
                "call_id": "",
                "scope": "m.room",
                "device_id": device,
                "expires": expires,
                "focus_active": {"type": "livekit"},
                "foci_preferred": [],
            },
        })
    }

    fn retraction_event(
        sender: &str,
        device: &str,
        origin_server_ts: u64,
    ) -> serde_json::Value {
        json!({
            "type": EV_MEMBER_LEGACY,
            "sender": sender,
            "state_key": format!("_{sender}_{device}_m.call"),
            "origin_server_ts": origin_server_ts,
            "content": {},
        })
    }

    // The no-MSC4140 fallback write must not publish an already-expired
    // membership (see fallback_expires_ms).
    #[test]
    fn the_fallback_write_is_measured_from_created_ts_not_from_zero() {
        let join = 1_000_000u64;
        let now = join + 20 * 60 * 1000; // twenty minutes into the call
        let expires = fallback_expires_ms(Some(join), now);
        // What every reader computes, this file's own parser included.
        let deadline = join + expires;
        assert!(
            deadline > now,
            "the fallback write published a membership that was already dead: \
             deadline {deadline} is not after now {now}"
        );
        // A full period out, not merely non-negative.
        assert_eq!(deadline, now + MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
    }

    #[test]
    fn a_fresh_join_still_gets_the_plain_period() {
        // No created_ts: the duration is the period.
        assert_eq!(
            fallback_expires_ms(None, 5_000_000),
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        );
    }

    #[test]
    fn a_membership_the_fallback_wrote_survives_its_own_parser() {
        // End to end through the real parser.
        let join = 1_000_000u64;
        let now = join + 20 * 60 * 1000;
        let event_ts = now;
        let content = json!({
            "application": "m.call",
            "call_id": "",
            "scope": "m.room",
            "device_id": "DEVICE",
            "created_ts": join,
            "expires": fallback_expires_ms(Some(join), now),
            "focus_active": {"type": "livekit"},
            "foci_preferred": [],
        });
        let member = parse_session_membership(&content, "@a:x", event_ts)
            .expect("the fallback wrote a parseable membership");
        assert!(
            member.expires_at_ms > now,
            "the fallback write is dropped by our own liveness filter"
        );
    }

    #[test]
    fn a_store_holding_one_live_membership_answers_for_itself() {
        let store = vec![membership_event("@a:x", "D1", 1_000, 300_000)];
        assert!(store_view_is_usable(&store, 60_000));
    }

    #[test]
    fn a_store_holding_only_retractions_is_not_usable() {
        let store = vec![retraction_event("@a:x", "D1", 1_000)];
        assert!(!store_view_is_usable(&store, 60_000));
    }

    #[test]
    fn a_ghost_membership_does_not_make_the_store_authoritative() {
        // A dead ghost membership (non-empty content) must not make the store
        // authoritative, or the network read could never run.
        let ghost = membership_event("@ghost:x", "D9", 1_000, 300_000);
        assert!(!ghost["content"].as_object().unwrap().is_empty());
        assert!(!store_view_is_usable(&[ghost], 1_000 + 300_001));
    }

    #[test]
    fn an_unparseable_membership_does_not_make_the_store_authoritative() {
        // Non-empty content that does not parse as a membership.
        let junk = json!({
            "type": EV_MEMBER_LEGACY,
            "sender": "@a:x",
            "state_key": "_@a:x_D1_m.call",
            "origin_server_ts": 1_000,
            "content": {"unrelated": true},
        });
        assert!(!store_view_is_usable(&[junk], 2_000));
    }

    // -----------------------------------------------------------------
    // The `/state` escalation and when it is worth one request.
    //
    // Mutation check for the `AnswerNoSession` cases: replace the tail of
    // `store_read_verdict` after the `Answer` return with
    // `StoreVerdict::AskServer(EscalationReason::RecentActivity)` (the old
    // two-outcome behaviour) and they all fail.
    // -----------------------------------------------------------------

    /// A week of wall clock, so a "long dead" fixture is unambiguous.
    const A_WEEK_MS: u64 = 7 * 24 * 60 * 60 * 1000;

    #[test]
    fn an_idle_room_full_of_dead_memberships_costs_no_request() {
        // Initial sync replays a room's old `m.call.member` state; with nothing
        // live and no call, no request is warranted.
        let now = A_WEEK_MS;
        let store = vec![
            membership_event("@a:x", "D1", 1_000, 300_000),
            membership_event("@b:x", "D2", 2_000, 300_000),
            retraction_event("@c:x", "D3", 3_000),
        ];
        assert_eq!(
            store_read_verdict(&store, now, false, false),
            StoreVerdict::AnswerNoSession
        );
    }

    #[test]
    fn a_room_that_has_never_hosted_a_call_costs_no_request() {
        // Opening a room refreshes its session (`setCurrentRoomId`); an empty store
        // must not cost a `/state`.
        assert_eq!(
            store_read_verdict(&[], A_WEEK_MS, false, false),
            StoreVerdict::AnswerNoSession
        );
        assert_eq!(newest_session_signal_ms(&[], A_WEEK_MS), None);
    }

    #[test]
    fn a_live_membership_is_still_answered_by_the_store_alone() {
        // A live membership is answered by the store for free, which keeps a call
        // starting prompt.
        let now = 60_000u64;
        let store = vec![membership_event("@a:x", "D1", 1_000, 300_000)];
        assert_eq!(
            store_read_verdict(&store, now, false, false),
            StoreVerdict::Answer
        );
    }

    #[test]
    fn a_membership_that_lapsed_moments_ago_still_buys_one_request() {
        // A peer whose refresh we have not received expired seconds ago; inside
        // the horizon, it still earns a request.
        let now = A_WEEK_MS;
        let store = vec![membership_event("@a:x", "D1", now - 330_000, 300_000)];
        assert!(!store_view_is_usable(&store, now));
        assert_eq!(
            store_read_verdict(&store, now, false, false),
            StoreVerdict::AskServer(EscalationReason::RecentActivity)
        );
    }

    #[test]
    fn a_retraction_from_moments_ago_still_buys_one_request() {
        // A retraction parses to nothing but is still the newest activity (the
        // `origin_server_ts` arm of `newest_session_signal_ms`).
        let now = A_WEEK_MS;
        let fresh = vec![retraction_event("@a:x", "D1", now - 30_000)];
        assert_eq!(
            store_read_verdict(&fresh, now, false, false),
            StoreVerdict::AskServer(EscalationReason::RecentActivity)
        );
        // An hour later the room is quiet again.
        let stale = vec![retraction_event("@a:x", "D1", now - 60 * 60 * 1000)];
        assert_eq!(
            store_read_verdict(&stale, now, false, false),
            StoreVerdict::AnswerNoSession
        );
    }

    #[test]
    fn the_deadline_counts_even_when_the_event_itself_is_old() {
        // A long call refreshes rarely, so the newest deadline can be much later
        // than the newest event.
        let now = A_WEEK_MS;
        let event_ts = now - 4 * 60 * 60 * 1000;
        let store = vec![membership_event("@a:x", "D1", event_ts, 4 * 60 * 60 * 1000 - 60_000)];
        assert_eq!(
            newest_session_signal_ms(&store, now),
            Some(now - 60_000),
            "the deadline is the signal, not the event that carried it"
        );
    }

    #[test]
    fn being_in_the_call_always_buys_the_request() {
        // Being in the call always buys the request: the store may hold only stale
        // retractions while the server has the live memberships, and media keys go
        // to the devices these name.
        let now = A_WEEK_MS;
        let store: Vec<serde_json::Value> = (0..13)
            .map(|i| retraction_event(&format!("@peer{i}:x"), "D", 1_000))
            .collect();
        assert_eq!(
            store_read_verdict(&store, now, false, false),
            StoreVerdict::AnswerNoSession,
            "same bytes, no call of our own: nothing to chase"
        );
        assert_eq!(
            store_read_verdict(&store, now, true, false),
            StoreVerdict::AskServer(EscalationReason::OwnCall),
            "a call we are IN must still reach the homeserver, or its media \
             keys go to nobody"
        );
    }

    #[test]
    fn a_ring_buys_the_request_for_a_room_nothing_has_looked_at() {
        // A ring for a room that is not open: Answer is gated on a session read.
        assert_eq!(
            store_read_verdict(&[], A_WEEK_MS, false, true),
            StoreVerdict::AskServer(EscalationReason::Ring)
        );
    }

    #[test]
    fn the_escalation_backoff_doubles_and_stops_at_the_ceiling() {
        // Without doubling, one ghost membership would buy a request per poke for
        // the whole horizon.
        assert_eq!(escalation_cooldown_ms(0), SERVER_ESCALATION_COOLDOWN_MS);
        assert_eq!(escalation_cooldown_ms(1), SERVER_ESCALATION_COOLDOWN_MS);
        assert_eq!(escalation_cooldown_ms(2), 2 * SERVER_ESCALATION_COOLDOWN_MS);
        assert_eq!(escalation_cooldown_ms(3), 4 * SERVER_ESCALATION_COOLDOWN_MS);
        assert_eq!(
            escalation_cooldown_ms(64),
            SERVER_ESCALATION_COOLDOWN_MAX_MS,
            "the gap must stop growing, or a room becomes permanently \
             un-askable"
        );
        // Monotonic and never past the ceiling.
        let mut previous = 0u64;
        for asks in 0..40u32 {
            let gap = escalation_cooldown_ms(asks);
            assert!(gap >= previous);
            assert!(gap <= SERVER_ESCALATION_COOLDOWN_MAX_MS);
            previous = gap;
        }
    }

    #[test]
    fn the_backoff_is_per_room_and_a_ring_cancels_it() {
        // Shares the global mark tables with the publish-set tests, hence
        // PUBLISH_SET_TEST_LOCK (`forget_all_memberships_published` clears all
        // three).
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let room = "!backoff:matrix.example";
        let other = "!neighbour:matrix.example";
        clear_server_escalation_backoff(room);
        clear_server_escalation_backoff(other);

        assert!(claim_server_escalation(room), "the first ask is free");
        assert!(
            !claim_server_escalation(room),
            "a second ask inside the gap is exactly the storm this bounds"
        );
        assert!(
            claim_server_escalation(other),
            "one room's backoff must never silence another's — a per-process \
             gate would make a real call wait on an idle room's ghost"
        );

        // A ring cancels the gap.
        note_rtc_ring(room);
        assert!(rtc_ring_is_recent(room));
        assert!(!rtc_ring_is_recent(other));
        assert!(
            claim_server_escalation(room),
            "a ring must not have to wait out a backoff the room earned \
             while it was idle: the Answer button is gated on this read"
        );

        // A session ending clears them all.
        forget_all_memberships_published();
        assert!(!rtc_ring_is_recent(room));
        assert!(
            claim_server_escalation(room),
            "an account switch must not inherit the previous account's gap"
        );
        clear_server_escalation_backoff(room);
        clear_server_escalation_backoff(other);
    }

    #[test]
    fn a_bound_cuts_the_dead_memberships_before_the_live_ones() {
        // Thousands of stale memberships and two live ones: an arbitrary cut could
        // keep only ghosts.
        let now = 10_000_000u64;
        let mut events: Vec<serde_json::Value> = (0..600)
            .map(|i| membership_event(&format!("@ghost{i}:x"), "D", 1_000, 300_000))
            .collect();
        events.push(membership_event("@live:x", "D1", now - 1_000, 300_000));
        events.push(membership_event("@live2:x", "D2", now - 1_000, 300_000));

        let bounded = bound_membership_events(events, now, MAX_RAW_MEMBER_EVENTS);
        assert_eq!(bounded.len(), MAX_RAW_MEMBER_EVENTS);
        let live: Vec<&str> = bounded
            .iter()
            .filter(|value| membership_event_is_live(value, now))
            .map(|value| value["sender"].as_str().unwrap())
            .collect();
        assert_eq!(live.len(), 2, "a live membership was cut before a ghost");
        assert!(live.contains(&"@live:x") && live.contains(&"@live2:x"));
    }

    #[test]
    fn a_list_within_the_bound_is_left_exactly_as_it_was() {
        let events = vec![
            membership_event("@a:x", "D1", 1_000, 300_000),
            retraction_event("@b:x", "D2", 2_000),
        ];
        let bounded = bound_membership_events(events.clone(), 60_000, 8);
        assert_eq!(bounded, events);
    }

    #[test]
    fn merging_keeps_the_newer_event_for_each_state_key() {
        // /state can predate our own publish; the store has our echo first.
        let store = vec![membership_event("@a:x", "D1", 90_000, 360_000)];
        let server = vec![
            membership_event("@a:x", "D1", 30_000, 300_000),
            membership_event("@b:x", "D2", 30_000, 300_000),
        ];
        let merged = merge_membership_events(store, server, 60_000);
        assert_eq!(merged.len(), 2);
        let ours = merged
            .iter()
            .find(|value| value["sender"] == "@a:x")
            .expect("our own membership survived the merge");
        assert_eq!(ours["origin_server_ts"], 90_000);
    }

    #[test]
    fn a_newer_retraction_wins_over_an_older_join() {
        // A stale stored copy must not resurrect a participant who left.
        let store = vec![membership_event("@b:x", "D2", 30_000, 300_000)];
        let server = vec![retraction_event("@b:x", "D2", 90_000)];
        let merged = merge_membership_events(store, server, 60_000);
        assert_eq!(merged.len(), 1);
        assert!(merged[0]["content"].as_object().unwrap().is_empty());
        assert!(!store_view_is_usable(&merged, 91_000));
    }

    #[test]
    fn merging_does_not_collapse_two_devices_of_one_user() {
        let merged = merge_membership_events(
            vec![membership_event("@a:x", "D1", 10_000, 300_000)],
            vec![membership_event("@a:x", "D2", 10_000, 300_000)],
            60_000,
        );
        assert_eq!(merged.len(), 2);
    }

    #[test]
    fn expired_memberships_are_not_participants() {
        let live = member_at("@a:x", "D1", 10, 5_000);
        let dead = member_at("@b:x", "D2", 20, 100);
        let out = aggregate_session(vec![live, dead], 1_000);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].user_id, "@a:x");
    }

    #[test]
    fn same_user_on_two_devices_is_two_participants() {
        let out = aggregate_session(
            vec![
                member_at("@a:x", "D1", 10, 9_000),
                member_at("@a:x", "D2", 20, 9_000),
            ],
            1_000,
        );
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn duplicate_membership_for_one_device_keeps_the_newest() {
        let out = aggregate_session(
            vec![
                member_at("@a:x", "D1", 10, 9_000),
                member_at("@a:x", "D1", 50, 9_000),
            ],
            1_000,
        );
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].created_ts, 50);
    }

    #[test]
    fn focus_comes_from_the_oldest_membership() {
        // Everyone must reach the same SFU: oldest membership wins, not parse
        // order.
        let mut newer = member_at("@b:x", "D2", 100, 9_000);
        newer.foci = vec![LivekitTransport {
            service_url: "https://newer.example.org/".to_owned(),
            alias: None,
        }];
        let mut older = member_at("@a:x", "D1", 10, 9_000);
        older.foci = vec![LivekitTransport {
            service_url: "https://older.example.org/".to_owned(),
            alias: None,
        }];

        let ordered = aggregate_session(vec![newer, older], 1_000);
        let focus = select_focus(&ordered).expect("a focus");
        assert_eq!(focus.service_url, "https://older.example.org/");
    }

    #[test]
    fn focus_does_not_walk_past_a_silent_oldest_member() {
        // The reference yields nothing when the oldest member advertises no focus;
        // walking on would diverge from Element.
        let silent = member_at("@a:x", "D1", 10, 9_000);
        let mut advertiser = member_at("@b:x", "D2", 20, 9_000);
        advertiser.foci = vec![LivekitTransport {
            service_url: "https://sfu.example.org/".to_owned(),
            alias: None,
        }];
        let ordered = aggregate_session(vec![silent, advertiser], 1_000);
        assert!(
            select_focus(&ordered).is_none(),
            "must not adopt a younger member's focus"
        );
    }

    #[test]
    fn transport_url_refuses_credentials_and_unroutable_hosts() {
        // Foci are advertised by remote participants and will be connected to.
        for bad in [
            "https://user:pw@sfu.example.org",
            "https://127.0.0.1/",
            "https://localhost/",
            "https://10.0.0.5/",
            "https://192.168.1.10/",
            "https://169.254.1.1/",
            "https://[::1]/",
            "https://[fe80::1]/",
            "https://[fc00::1]/",
            // IPv4-mapped IPv6 loopback and metadata addresses.
            "https://[::ffff:127.0.0.1]/",
            "https://[::ffff:169.254.169.254]/",
            // CGNAT, multicast, and the local-only name suffixes.
            "https://100.64.0.1/",
            "https://224.0.0.1/",
            "https://sfu.local/",
            "https://sfu.internal/",
            "https://Sfu.LOCALHOST./",
        ] {
            assert!(
                parse_transport(&json!({
                    "type": "livekit",
                    "livekit_service_url": bad
                }))
                .is_none(),
                "{bad} must be refused"
            );
        }
        assert!(parse_transport(&json!({
            "type": "livekit",
            "livekit_service_url": "https://sfu.example.org/"
        }))
        .is_some());
    }

    #[test]
    fn no_members_means_no_focus() {
        assert!(select_focus(&[]).is_none());
    }

    // --- sticky (MSC4143) format ---------------------------------------

    fn rtc_content() -> serde_json::Value {
        json!({
            "slot_id": "m.call#ROOM",
            "member": { "user_id": "@a:x", "device_id": "DEVICE", "id": "member-1" },
            "application": { "type": "m.call" },
            "transports": {
                "published": [
                    { "type": "livekit", "livekit_service_url": "https://sfu.example.org" }
                ],
                "can_subscribe": ["livekit"]
            },
            "versions": ["1"],
            "sticky_key": "abc"
        })
    }

    #[test]
    fn parses_a_sticky_rtc_membership() {
        let member =
            parse_rtc_membership(&rtc_content(), "@a:x", 500).expect("valid");
        assert_eq!(member.slot_id, "m.call#ROOM");
        assert_eq!(member.kind, MembershipKind::Rtc);
        assert_eq!(member.foci.len(), 1);
    }

    #[test]
    fn sticky_membership_for_another_user_is_forgery() {
        // member.user_id is a claim; a mismatch with the sender is forgery.
        assert!(parse_rtc_membership(&rtc_content(), "@attacker:x", 500).is_none());
    }

    #[test]
    fn sticky_slot_id_must_name_its_application() {
        let mut content = rtc_content();
        content["slot_id"] = json!("m.other#ROOM");
        assert!(parse_rtc_membership(&content, "@a:x", 500).is_none());
    }

    #[test]
    fn sticky_slot_id_with_two_separators_is_refused() {
        let mut content = rtc_content();
        content["slot_id"] = json!("m.call#a#b");
        assert!(parse_rtc_membership(&content, "@a:x", 500).is_none());
    }

    #[test]
    fn sticky_membership_without_a_sticky_key_is_refused() {
        let mut content = rtc_content();
        content.as_object_mut().unwrap().remove("sticky_key");
        assert!(parse_rtc_membership(&content, "@a:x", 500).is_none());
        // The MSC4354-prefixed spelling is equally acceptable.
        let mut prefixed = rtc_content();
        let object = prefixed.as_object_mut().unwrap();
        object.remove("sticky_key");
        object.insert("msc4354_sticky_key".to_owned(), json!("abc"));
        assert!(parse_rtc_membership(&prefixed, "@a:x", 500).is_some());
    }

    #[test]
    fn sticky_membership_needs_transports_and_versions() {
        for missing in ["transports", "versions"] {
            let mut content = rtc_content();
            content.as_object_mut().unwrap().remove(missing);
            assert!(
                parse_rtc_membership(&content, "@a:x", 500).is_none(),
                "membership without {missing} must be refused"
            );
        }
    }

    #[test]
    // A device that is present but stale must still be refreshed: its rotated
    // keys would otherwise make the recipient drop the media key while the
    // sender sees success.
    #[test]
    fn a_device_refresh_is_owed_before_the_first_send_and_then_rate_limited() {
        // Never refreshed in this process: always owed.
        assert!(device_refresh_due(None));
        // Just refreshed: not owed again, or every key rotation would query every
        // participant.
        assert!(!device_refresh_due(Some(0)));
        assert!(!device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS - 1)));
        // Past the window: owed again, so mid-call key rotations are picked up.
        assert!(device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS)));
        assert!(device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS * 10)));
        // A real bound: not zero, not enormous.
        assert!(DEVICE_REFRESH_TTL_SECS >= 10 && DEVICE_REFRESH_TTL_SECS <= 600);
    }

    // A loss not attributable to a call must not be reported as one: an
    // undecryptable to-device message has no type, so only a sender we sent a
    // media key to qualifies. Otherwise any failed message (or injected
    // `sender`) would trigger it and grow the table.
    #[test]
    fn an_undecryptable_message_is_only_a_call_fault_for_a_call_peer() {
        assert!(!undecryptable_key_report_due(false, None));
        assert!(!undecryptable_key_report_due(false, Some(0)));
        assert!(!undecryptable_key_report_due(
            false,
            Some(DEVICE_REFRESH_TTL_SECS * 10),
        ));
    }

    // A wedged session re-sends, so the report is paced: the event queue drops
    // its oldest entries, and repeats would evict real events. One line a
    // minute per peer.
    #[test]
    fn a_lost_key_is_reported_once_and_then_paced() {
        // Never said for this peer: say it.
        assert!(undecryptable_key_report_due(true, None));
        // Just said: do not repeat.
        assert!(!undecryptable_key_report_due(true, Some(0)));
        assert!(!undecryptable_key_report_due(
            true,
            Some(DEVICE_REFRESH_TTL_SECS - 1),
        ));
        // Still broken a minute later: report again.
        assert!(undecryptable_key_report_due(
            true,
            Some(DEVICE_REFRESH_TTL_SECS),
        ));
    }

    #[test]
    fn rtc_identity_is_the_hash_the_reference_implementation_computes() {
        // Unpadded base64 of sha256 over the canonical JSON array; must match
        // Element so both agree which SFU participant is which device.
        let identity = rtc_identity("@a:x", "DEVICE", "member-1");
        assert!(!identity.contains('='), "must be unpadded");
        assert_eq!(identity.len(), 43, "sha256 in unpadded base64");
        // Stable across calls and sensitive to every component.
        assert_eq!(identity, rtc_identity("@a:x", "DEVICE", "member-1"));
        assert_ne!(identity, rtc_identity("@a:x", "DEVICE", "member-2"));
        assert_ne!(identity, rtc_identity("@a:x", "OTHER", "member-1"));
        assert_ne!(identity, rtc_identity("@b:x", "DEVICE", "member-1"));
    }

    #[test]
    fn membership_state_key_matches_what_element_writes() {
        // Element writes `_{user}_{device}_{application}`, dropping the leading
        // underscore only on room versions where a user owns user-scoped keys.
        // Wrong, and the write is refused or we appear twice.
        assert_eq!(
            membership_state_key("@a:x", "DEVICE", "10"),
            "_@a:x_DEVICE_m.call"
        );
        assert_eq!(
            membership_state_key("@a:x", "DEVICE", "org.matrix.msc3757.10"),
            "@a:x_DEVICE_m.call"
        );
        assert_eq!(
            membership_state_key("@a:x", "DEVICE", "org.matrix.msc3779"),
            "@a:x_DEVICE_m.call"
        );
    }

    #[test]
    fn published_membership_round_trips_through_our_own_parser() {
        // What we write must parse under the rules we apply to Element's.
        let focus = LivekitTransport {
            service_url: "https://sfu.example.org/".to_owned(),
            alias: None,
        };
        let content = own_membership_content(
            "DEVICE", "@a:x", Some(&focus), "video", None,
            MEMBERSHIP_EXPIRY_MS);
        let member = parse_session_membership(&content, "@a:x", 5_000)
            .expect("our own membership must parse");
        assert_eq!(member.device_id, "DEVICE");
        assert_eq!(member.rtc_identity, "@a:x:DEVICE");
        assert_eq!(member.slot_id, "m.call#ROOM");
        assert_eq!(member.intent, "video");
        assert_eq!(member.foci.len(), 1);
        // A fresh join carries no created_ts, so the event ts is the join.
        assert_eq!(member.created_ts, 5_000);
        assert_eq!(member.expires_at_ms, 5_000 + MEMBERSHIP_EXPIRY_MS);
    }

    #[test]
    fn a_refresh_preserves_the_original_join_time() {
        // created_ts orders focus selection; resetting it on refresh would
        // reshuffle everyone's SFU.
        let content =
            own_membership_content("DEVICE", "@a:x", None, "audio", Some(111),
                                   MEMBERSHIP_EXPIRY_MS);
        let member = parse_session_membership(&content, "@a:x", 999_000)
            .expect("valid");
        assert_eq!(member.created_ts, 111);
        assert_eq!(member.expires_at_ms, 111 + MEMBERSHIP_EXPIRY_MS);
    }

    #[test]
    fn a_membership_with_no_server_side_cleanup_expires_in_minutes() {
        // Without MSC4140, `expires` is the only cleanup (a killed client cannot
        // retract), so it must be minutes, not hours.
        let content = own_membership_content(
            "DEVICE", "@a:x", None, "audio", None,
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
        let member = parse_session_membership(&content, "@a:x", 5_000)
            .expect("valid");
        assert_eq!(
            member.expires_at_ms,
            5_000 + MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        );
        // And survivable: with a 60 s re-publish, the window must absorb several
        // failed refreshes, or live participants get dropped.
        assert!(MEMBERSHIP_EXPIRY_NO_DELAYED_MS >= 3 * 60 * 1000);
    }

    /// A peer's deadline is `created_ts + expires`; each refresh must move it,
    /// or the participant drops out one period after joining.
    #[test]
    fn refreshing_a_membership_moves_the_deadline_peers_compute() {
        let period = MEMBERSHIP_EXPIRY_NO_DELAYED_MS;
        let created = 1_000_000_u64;

        // First publish: no created_ts yet, peers date it from this event.
        assert_eq!(expires_for_refresh(period, None, created), period);

        // Refreshes every 60 s: the deadline stays a full period ahead each time.
        for elapsed in [60_000_u64, 120_000, 240_000, 299_000, 600_000] {
            let now = created + elapsed;
            let expires = expires_for_refresh(period, Some(created), now);
            let deadline = created + expires;
            assert_eq!(
                deadline,
                now + period,
                "a refresh at {elapsed} ms must put the deadline a full \
                 period ahead of NOW, not of the join"
            );
            assert!(
                deadline > now,
                "deadline {deadline} already passed at {now}"
            );
        }

        // Still live past join + period.
        let now = created + period + 1;
        let expires = expires_for_refresh(period, Some(created), now);
        assert!(created + expires > now,
                "the participant would vanish at exactly one period");
    }

    /// Clock skew must never produce a deadline in the past.
    #[test]
    fn a_created_ts_in_the_future_still_yields_a_live_deadline() {
        let period = MEMBERSHIP_EXPIRY_NO_DELAYED_MS;
        let now = 1_000_000_u64;
        // Server clock ahead of ours by a minute.
        let created = now + 60_000;
        let expires = expires_for_refresh(period, Some(created), now);
        assert_eq!(expires, period, "saturating_sub must floor at zero");
        assert!(created + expires > now);

        // A hostile value read back from our own state must not wrap.
        let expires = expires_for_refresh(period, Some(u64::MAX), now);
        assert_eq!(expires, period);
        assert!(MEMBERSHIP_EXPIRY_NO_DELAYED_MS < MEMBERSHIP_EXPIRY_MS);
    }

    #[test]
    fn an_expired_ghosts_join_time_is_not_inherited() {
        // An expired ghost's created_ts must not be inherited, or the new
        // membership is born expired and the person appears with no media.
        let ghost = json!({
            "created_ts": 1_000u64,
            "expires": 5 * 60 * 1000u64,
            "device_id": "DEVICE",
        });
        // now is well past created_ts + expires.
        assert_eq!(inheritable_created_ts(&ghost, None, 9_000_000), None);
        // Still inside its window: inherited, so a refresh keeps focus ordering.
        assert_eq!(
            inheritable_created_ts(&ghost, None, 200_000),
            Some(1_000)
        );
    }

    #[test]
    fn a_naked_retraction_is_only_sent_where_a_repair_write_follows() {
        // `schedule_delayed_leave` PUTs `{}` to our state key via the v3 state
        // endpoint, marked delayed only by a query parameter; a server that
        // ignores it retracts us immediately. publish_membership repairs that only
        // when it did not assume refusal, so a refresh would be unrepaired (GitHub
        // #10). Never arm where the repair cannot follow.
        for assumed_no_delayed in [false, true] {
            if delayed_retraction_is_repairable(assumed_no_delayed) {
                assert!(
                    !assumed_no_delayed,
                    "a delayed retraction was armed on a publish whose \
                     reconciliation cannot re-publish the membership, so an \
                     ignored MSC4140 parameter deletes us for a whole period"
                );
            }
        }
        // But it is armed somewhere, or an unclean exit strands a phantom for 4 h.
        assert!(delayed_retraction_is_repairable(false),
                "nothing arms a delayed retraction at all any more");
    }

    #[test]
    fn one_servers_refusal_does_not_disable_msc4140_for_another() {
        // MSC4140 support is per homeserver: one old server's refusal must not
        // disable crash cleanup (and scheduled send) for other accounts.
        let old = "https://old.example.org/";
        let modern = "https://modern.example.org/";

        // Start clean, whatever else touched the map.
        mark_delayed_refusal(old, false);
        mark_delayed_refusal(modern, false);
        assert!(!delayed_refusal_recorded(old));
        assert!(!delayed_refusal_recorded(modern));

        mark_delayed_refusal(old, true);
        assert!(delayed_refusal_recorded(old));
        assert!(
            !delayed_refusal_recorded(modern),
            "a refusal by one homeserver disabled delayed retractions for a \
             DIFFERENT one, so an account switch costs every later call its \
             only crash-safe cleanup"
        );

        // Clearing one leaves the other alone, both ways.
        mark_delayed_refusal(modern, true);
        mark_delayed_refusal(old, false);
        assert!(!delayed_refusal_recorded(old));
        assert!(delayed_refusal_recorded(modern));

        mark_delayed_refusal(modern, false);
    }

    #[test]
    fn only_an_absent_endpoint_disables_msc4140_for_a_server() {
        // A refusal disables MSC4140, the only crash-surviving cleanup, for a whole
        // server, so only "endpoint absent" may record one. Every failure arrives
        // as an empty delay id, and `classify_room_error` maps a timeout and a 5xx
        // alike to "network".
        for transient in ["network", "rate_limited", "forbidden", "invalid"] {
            assert!(
                !delayed_refusal_is_permanent(transient),
                "a '{transient}' refusal would permanently disable delayed                  retractions for every room in the process, so an unclean                  exit would strand a phantom participant from then on"
            );
        }
        // A server that ignores the delay parameter answers 200 with no
        // `delay_id`; treated as transient, every refresh would send a naked
        // retraction (GitHub #10).
        assert!(
            delayed_refusal_is_permanent("no_delay_id"),
            "a server that ANSWERED without a delay id was treated as a \
             passing blip, so a naked retraction is armed on every refresh"
        );
        // Synapse's 404 M_UNRECOGNIZED, and a plain 404.
        for absent in ["unrecognized", "not_found"] {
            assert!(
                delayed_refusal_is_permanent(absent),
                "'{absent}' means the endpoint is not there, and re-probing                  it on every refresh costs a second state event each time"
            );
        }
        // Synapse with `msc4140_enabled` off answers 400 M_UNKNOWN, not 404.
        assert!(
            delayed_refusal_is_permanent("delayed_unsupported"),
            "the server said outright that it does not do delayed events; \
             treating that as a blip re-arms a doomed request on every publish"
        );
    }

    #[test]
    fn synapse_says_delayed_events_are_unsupported_and_it_is_not_a_blip() {
        // Synapse's body with `msc4140_enabled` off (`org.matrix.msc4140: false`
        // in /versions):
        //   HTTP 400
        //   {"errcode":"M_UNKNOWN",
        //    "error":"Delayed events are not supported on this server",
        //    "org.matrix.msc4140.errcode":"M_MAX_DELAY_UNSUPPORTED"}
        // The state event is not applied in this case.
        let synapse = "the server returned an error: [400 / M_UNKNOWN] \
                       Delayed events are not supported on this server";
        assert!(
            delayed_failure_says_unsupported(synapse),
            "a permanent refusal fell through every branch of \
             classify_room_error into the 'network' catch-all, so the latch \
             never set and `delayed_reason= \"network\"` asserted the \
             opposite of what /versions already published"
        );
        assert_eq!(
            classify_room_error(synapse),
            "network",
            "this pins WHY the dedicated test is needed: the generic \
             classifier cannot see this refusal, and if it ever can, the \
             comment above is stale"
        );
        // The MSC's own errcode, for a client library that surfaces it.
        assert!(delayed_failure_says_unsupported(
            "org.matrix.msc4140.errcode M_MAX_DELAY_UNSUPPORTED"
        ));
        // M_MAX_DELAY_EXCEEDED must not match: delayed events are supported, the
        // delay was just too long.
        assert!(
            !delayed_failure_says_unsupported(
                "[400 / M_UNKNOWN] M_MAX_DELAY_EXCEEDED: delay too long"
            ),
            "a server that supports delayed events would have been recorded \
             as not supporting them"
        );
        for unrelated in [
            "the server returned an error: [404 / M_UNRECOGNIZED] Unrecognized",
            "error sending request for url",
        ] {
            assert!(!delayed_failure_says_unsupported(unrelated));
        }
    }

    /// Serialises every test touching the process-global publish set and,
    /// since `forget_all_memberships_published()` clears them together, the
    /// escalation and ring tables too. Cargo runs tests in parallel in one
    /// process, so without it one test's clear wipes another's marks.
    static PUBLISH_SET_TEST_LOCK: Mutex<()> = Mutex::new(());

    /// The published mark must not outlive its session. Two clearers are pinned:
    /// by room (needs only the room id, so no early return can skip it) and
    /// wholesale on teardown. Otherwise, after sign-out, the next join would
    /// read as a refresh and inherit a ghost's `created_ts`.
    #[test]
    fn a_published_mark_does_not_outlive_its_session() {
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let room = "!clearing:matrix.example";
        let other = "!untouched:matrix.example";
        let mine = membership_publish_key(room, "_@a:example_DEVICEA_m.call");
        let sibling = membership_publish_key(room, "_@a:example_DEVICEB_m.call");
        let elsewhere = membership_publish_key(other, "_@a:example_DEVICEA_m.call");

        for key in [&mine, &sibling, &elsewhere] {
            mark_membership_published(key);
            assert!(membership_published_in_this_process(key));
        }

        // By room: every state key in that room goes, and only that room.
        forget_room_memberships_published(room);
        assert!(
            !membership_published_in_this_process(&mine),
            "leaving a room must forget what this session published in it,              and it must do so without needing the session that is already gone"
        );
        assert!(
            !membership_published_in_this_process(&sibling),
            "the room prefix must cover every state key under it"
        );
        assert!(
            membership_published_in_this_process(&elsewhere),
            "leaving one room must not disturb another room's bookkeeping"
        );

        // Wholesale: a session ending clears everything.
        forget_all_memberships_published();
        assert!(
            !membership_published_in_this_process(&elsewhere),
            "teardown must clear the set, or a mark survives an account              switch and the next join inherits a ghost's created_ts"
        );
    }

    #[test]
    fn a_rejoin_is_a_join_until_this_process_has_published() {
        // Shares the process-global set with the test above; see the lock.
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        // `created_ts` is inherited only when this process already published the
        // state key. Inheriting a killed session's join time makes peers see no
        // new joiner, so matrix-js-sdk sends the rejoined device no media key.
        let room = "!probe:matrix.example";
        let key = membership_publish_key(room, "_@a:example_DEVICE_m.call");
        forget_membership_published(&key);

        assert!(
            !membership_published_in_this_process(&key),
            "a process that has published nothing must treat its first \
             publish as a JOIN, whatever is left in the room's state"
        );
        mark_membership_published(&key);
        assert!(
            membership_published_in_this_process(&key),
            "a refresh must inherit created_ts, or the membership's deadline \
             walks forward and oldest-membership focus selection reorders"
        );
        // A leave ends the session: an in-process rejoin is a new participant, and
        // a failed retraction leaves the same ghost a crash does.
        forget_membership_published(&key);
        assert!(!membership_published_in_this_process(&key));

        // Scoped to the state key, so other rooms and accounts are unaffected.
        mark_membership_published(&membership_publish_key(
            "!other:matrix.example",
            "_@a:example_DEVICE_m.call",
        ));
        mark_membership_published(&membership_publish_key(
            room,
            "_@b:example_DEVICE_m.call",
        ));
        assert!(
            !membership_published_in_this_process(&key),
            "another room's or another account's record answered for this one"
        );
        forget_membership_published(&membership_publish_key(
            "!other:matrix.example",
            "_@a:example_DEVICE_m.call",
        ));
        forget_membership_published(&membership_publish_key(
            room,
            "_@b:example_DEVICE_m.call",
        ));
    }

    #[test]
    fn inheritable_created_ts_falls_back_and_saturates() {
        // No created_ts: the event's own timestamp is the join time.
        let content = json!({ "expires": 600_000u64 });
        assert_eq!(
            inheritable_created_ts(&content, Some(2_000), 3_000),
            Some(2_000)
        );
        // A retracted membership is empty content: a fresh join.
        assert_eq!(inheritable_created_ts(&json!({}), Some(2_000), 3_000), None);
        // A hostile expires must not wrap and make a live membership look dead.
        let hostile = json!({ "created_ts": 10u64, "expires": u64::MAX });
        assert_eq!(
            inheritable_created_ts(&hostile, None, u64::MAX - 1),
            Some(10)
        );
    }

    #[test]
    fn a_membership_without_a_focus_is_still_valid() {
        // A join whose focus came from the server publishes no foci_preferred.
        let content =
            own_membership_content("DEVICE", "@a:x", None, "audio", None,
                                   MEMBERSHIP_EXPIRY_MS);
        assert_eq!(content["foci_preferred"], json!([]));
        assert!(parse_session_membership(&content, "@a:x", 1).is_some());
    }

    #[test]
    fn intent_is_a_closed_set() {
        assert_eq!(intent_str(Some(&json!("video"))), "video");
        assert_eq!(intent_str(Some(&json!("audio"))), "audio");
        // Free text: unknown values degrade to audio.
        assert_eq!(intent_str(Some(&json!("holodeck"))), "audio");
        assert_eq!(intent_str(None), "audio");
    }

    #[test]
    fn notification_content_serializes_the_way_element_reads_it() {
        let content = Msc4075RtcNotificationEventContent {
            notification_type: "ring".to_owned(),
            sender_ts: MilliSecondsSinceUnixEpoch(1234u32.into()),
            lifetime: 90_000,
            mentions: Some(Mentions::with_room_mention()),
            relates_to: None,
            call_intent: Some("video".to_owned()),
        };
        let value = serde_json::to_value(&content).expect("serializes");
        assert_eq!(value["notification_type"], json!("ring"));
        assert_eq!(value["sender_ts"], json!(1234));
        // A plain integer of milliseconds, not a duration object.
        assert_eq!(value["lifetime"], json!(90_000));
        assert_eq!(value["m.call.intent"], json!("video"));
        assert_eq!(value["m.mentions"]["room"], json!(true));
        // Absent rather than null: Element rejects a malformed relation.
        assert!(value.get("m.relates_to").is_none());
    }

    #[test]
    fn the_notification_event_type_is_the_one_element_sends() {
        // ruma types this event only as stable `m.rtc.notification`, deaf to
        // Element's rings.
        assert_eq!(
            Msc4075RtcNotificationEventContent::TYPE,
            "org.matrix.msc4075.rtc.notification"
        );
    }

    /// The raised-hand key must not drift: element-call's `ReactionsReader`
    /// compares it exactly, including U+FE0F. Asserted by bytes, since the two
    /// forms look identical.
    #[test]
    fn the_raised_hand_key_is_element_calls_own_bytes() {
        assert_eq!(
            HAND_RAISED_KEY.as_bytes(),
            &[0xF0, 0x9F, 0x96, 0x90, 0xEF, 0xB8, 0x8F]
        );
        // Two code points, not one: the selector is part of the key.
        assert_eq!(HAND_RAISED_KEY.chars().count(), 2);
        assert_eq!(HAND_RAISED_KEY.chars().next(), Some('\u{1F590}'));
        assert_eq!(HAND_RAISED_KEY.chars().nth(1), Some('\u{FE0F}'));
    }

    // ── Transient call reactions ─────────────────────────────────────────
    //
    // Asserted by bytes and serialized JSON, since visually identical emoji
    // can be different reactions.

    #[test]
    fn the_call_reaction_event_type_is_element_calls_own() {
        // `ElementCallReactionEventType` in element-call/src/reactions/index.ts.
        assert_eq!(
            ElementCallReactionEventContent::TYPE,
            "io.element.call.reaction"
        );
        // The derive is what the SDK matches; the constant is what readers grep.
        assert_eq!(ElementCallReactionEventContent::TYPE, EV_CALL_REACTION);
    }

    #[test]
    fn a_call_reaction_serializes_the_way_element_reads_it() {
        let content = ElementCallReactionEventContent {
            emoji: "\u{1F44D}".to_owned(),
            name: "thumbsup".to_owned(),
            relates_to: Reference::new(
                EventId::parse("$membership:example.org").expect("event id"),
            ),
        };
        let value = serde_json::to_value(&content).expect("serializes");
        // A reference to the membership, not an annotation; element-call reads
        // `m.relates_to.event_id` and matches it to a membership.
        assert_eq!(value["m.relates_to"]["rel_type"], json!("m.reference"));
        assert_eq!(
            value["m.relates_to"]["event_id"],
            json!("$membership:example.org")
        );
        assert_eq!(value["emoji"], json!("\u{1F44D}"));
        assert_eq!(value["name"], json!("thumbsup"));
    }

    #[test]
    fn a_malformed_call_reaction_never_reaches_the_handler() {
        // Content that cannot deserialize is dropped by matrix-sdk before any
        // handler runs.
        //
        // Not enforced: serde does not check an internally tagged struct's tag
        // inbound, so `rel_type: "m.annotation"` still deserializes as a
        // `Reference`. element-call ignores `rel_type` too; the sender-owns-the-
        // membership check in C++ is the protection. Asserted so a serde change is
        // noticed here.
        let annotation = json!({
            "emoji": "\u{1F44D}",
            "name": "thumbsup",
            "m.relates_to": {
                "rel_type": "m.annotation",
                "event_id": "$membership:example.org",
                "key": "\u{1F44D}",
            },
        });
        assert!(serde_json::from_value::<ElementCallReactionEventContent>(
            annotation
        )
        .is_ok());

        let unrelated = json!({ "emoji": "\u{1F44D}", "name": "thumbsup" });
        assert!(serde_json::from_value::<ElementCallReactionEventContent>(
            unrelated
        )
        .is_err());

        let no_emoji = json!({
            "name": "thumbsup",
            "m.relates_to": {
                "rel_type": "m.reference",
                "event_id": "$membership:example.org",
            },
        });
        assert!(serde_json::from_value::<ElementCallReactionEventContent>(
            no_emoji
        )
        .is_err());

        // Deliberately liberal: a reaction without `name` is still a reaction.
        let nameless = json!({
            "emoji": "\u{1F44D}",
            "m.relates_to": {
                "rel_type": "m.reference",
                "event_id": "$membership:example.org",
            },
        });
        let parsed =
            serde_json::from_value::<ElementCallReactionEventContent>(nameless)
                .expect("a nameless reaction is still a reaction");
        assert_eq!(parsed.name, "");
    }

    #[test]
    fn the_reaction_set_is_element_calls_own_bytes() {
        // By bytes, like the hand key: element-call keys the sound on `name` and
        // draws `emoji`.
        let thumbsup = ELEMENT_CALL_REACTIONS
            .iter()
            .find(|(name, _)| *name == "thumbsup")
            .expect("thumbsup is in element-call's set");
        assert_eq!(thumbsup.1.as_bytes(), &[0xF0, 0x9F, 0x91, 0x8D]);
        // The ZWJ sequence, easily mangled by copy-paste: U+1F635 ZWJ U+1F4AB.
        let dizzy = ELEMENT_CALL_REACTIONS
            .iter()
            .find(|(name, _)| *name == "dizzy")
            .expect("dizzy is in element-call's set");
        assert_eq!(
            dizzy.1.as_bytes(),
            &[0xF0, 0x9F, 0x98, 0xB5, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x92, 0xAB]
        );
        assert_eq!(dizzy.1.chars().count(), 3);

        // No duplicate names: element-call's lookup takes the first match.
        let mut names: Vec<&str> =
            ELEMENT_CALL_REACTIONS.iter().map(|(name, _)| *name).collect();
        names.sort_unstable();
        let before = names.len();
        names.dedup();
        assert_eq!(names.len(), before);

        // Every entry must survive our own inbound reduction unchanged, catching a
        // cluster rule that is too eager or too shy.
        for (_, emoji) in ELEMENT_CALL_REACTIONS {
            assert_eq!(
                reaction_emoji(emoji).as_deref(),
                Some(*emoji),
                "element-call's own reaction did not survive reduction"
            );
        }
    }

    #[test]
    fn an_inbound_reaction_emoji_is_bounded_and_reduced_to_one_cluster() {
        // Whole clusters survive, including shapes a first-char rule would split.
        assert_eq!(reaction_emoji("\u{1F44D}").as_deref(), Some("\u{1F44D}"));
        assert_eq!(
            reaction_emoji("\u{1F590}\u{FE0F}").as_deref(),
            Some("\u{1F590}\u{FE0F}")
        );
        assert_eq!(
            reaction_emoji("\u{1F44D}\u{1F3FF}").as_deref(),
            Some("\u{1F44D}\u{1F3FF}")
        );
        assert_eq!(
            reaction_emoji("\u{1F1EC}\u{1F1E7}").as_deref(),
            Some("\u{1F1EC}\u{1F1E7}")
        );

        // Everything after the first cluster is dropped, like `Intl.Segmenter`.
        assert_eq!(
            reaction_emoji("\u{1F44D}\u{1F389}\u{1F44F}").as_deref(),
            Some("\u{1F44D}")
        );
        assert_eq!(reaction_emoji("call me on +1 555").as_deref(), Some("c"));
        let long = "\u{1F44D}".repeat(64);
        assert_eq!(
            reaction_emoji(&long),
            None,
            "an oversized emoji field is dropped whole rather than trimmed"
        );

        // Nothing drawable is nothing.
        assert_eq!(reaction_emoji(""), None);
        assert_eq!(reaction_emoji("   "), None);
        assert_eq!(reaction_emoji("\u{1F44D}\u{0007}"), None);

        // A hostile run of continuations stops at the cap.
        let joined = format!("\u{1F44D}{}", "\u{200D}\u{1F44D}".repeat(8));
        assert_eq!(
            reaction_emoji(&joined).map(|value| value.chars().count()),
            Some(8)
        );
        let selectors = format!("\u{1F44D}{}", "\u{FE0F}".repeat(16));
        assert_eq!(
            reaction_emoji(&selectors).map(|value| value.chars().count()),
            Some(8)
        );
    }

    /// A membership carries its declaring state event's id, taken from the
    /// envelope; a raised hand addresses that event, not the user.
    #[test]
    fn a_membership_read_from_content_alone_claims_no_event_id() {
        let content = json!({
            "application": APPLICATION_CALL,
            "call_id": "",
            "device_id": "DEVICE",
            "focus_active": { "type": "livekit" },
            "expires": 3_600_000u64,
        });
        let member = parse_session_membership(&content, "@a:x", 1_000)
            .expect("a well-formed membership should parse");
        assert!(
            member.event_id.is_empty(),
            "content alone cannot know its own event id, and a fabricated \
             one would attribute somebody else's hand"
        );
        // It crosses to C++ as an empty string rather than a missing key.
        let wire = member.to_json();
        assert_eq!(wire.get("event_id").and_then(|v| v.as_str()), Some(""));
    }

    // ── SFU resolution guard ─────────────────────────────────────────────
    //
    // Literal checks (rooms.rs `public_ip`, sfu.rs `normalize_sfu_url`) cannot
    // see an ordinary hostname whose DNS answer is private, and focus names
    // are attacker-supplied. These resolve literals, which is what a hostile A
    // record looks like to `lookup_host`, keeping the test offline.

    #[tokio::test]
    async fn a_focus_that_resolves_into_private_space_is_refused() {
        for host in [
            "127.0.0.1",        // loopback
            "10.0.0.5",         // RFC1918
            "192.168.1.10",     // RFC1918
            "172.16.0.1",       // RFC1918
            "169.254.169.254",  // link-local, the cloud metadata endpoint
            "100.64.0.1",       // CGNAT
            "0.0.0.0",          // unspecified
            "::1",              // IPv6 loopback
            "fc00::1",          // unique local
            "fe80::1",          // IPv6 link-local
            "::ffff:127.0.0.1", // IPv4-mapped loopback, the coat-wearing one
            "64:ff9b::7f00:1",  // NAT64 with an embedded loopback
        ] {
            assert!(
                resolve_public_host(host, 443).await.is_none(),
                "{host} resolves into unroutable space and must be refused"
            );
        }
    }

    // Local-only names are refused before any lookup.
    #[tokio::test]
    async fn local_only_names_are_refused_without_resolving() {
        for host in [
            "localhost",
            "sfu.localhost",
            "livekit.local",
            "sfu.internal",
            "LOCALHOST",      // case must not be an escape
            "localhost.",     // nor a trailing root label
        ] {
            assert!(
                resolve_public_host(host, 443).await.is_none(),
                "{host} must be refused without resolving"
            );
            assert!(!public_hostname(host), "{host} must not read as public");
        }
    }

    // The address is pinned: the caller connects to the SocketAddr that was
    // checked, so a second lookup cannot rebind it.
    #[tokio::test]
    async fn an_approved_focus_returns_the_address_to_connect_to() {
        let approved = resolve_public_host("93.184.216.34", 8443)
            .await
            .expect("a public literal resolves");
        assert_eq!(approved.ip().to_string(), "93.184.216.34");
        assert_eq!(approved.port(), 8443, "the port must survive to the socket");

        let v6 = resolve_public_host("2606:2800:220:1:248:1893:25c8:1946", 443)
            .await
            .expect("a public v6 literal resolves");
        assert!(v6.is_ipv6());
        assert_eq!(v6.port(), 443);
    }
}
