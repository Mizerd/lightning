//! MatrixRTC (MSC4143) — the Matrix half of modern calling.
//!
//! This module owns everything about a MatrixRTC session that lives on the
//! Matrix wire: who is in a call, which SFU ("transport"/"focus") they
//! advertise, ring notifications, and declines. It owns NO media. The SFU
//! client and the frame-level media encryption are a separate concern
//! layered on top of the facts this module reports.
//!
//! ## Interoperability is the whole point, so the exact wire is pinned here
//!
//! Everything below was read out of the reference implementation
//! (matrix-js-sdk `src/matrixrtc` @ 84fb28a, 2026-08-19, and element-call
//! @ b51a33c, 2026-08-21) rather than from an MSC document — the MatrixRTC
//! MSCs are unmerged PRs and the deployed behaviour is ahead of them.
//!
//! * Membership TODAY is a **state event**, `org.matrix.msc3401.call.member`
//!   ([`EV_MEMBER_LEGACY`]), carrying `SessionMembershipData`. This is what
//!   Element ships and what every deployed server supports, so it is what
//!   Lightning reads and (in a later round) writes.
//! * Membership NEXT is a **sticky event** (MSC4354),
//!   `org.matrix.msc4143.rtc.member` ([`EV_MEMBER_STICKY`]). Its parser
//!   lives here and is tested, but matrix-sdk 0.18 has no sticky-event
//!   support at all, so such a membership cannot currently be *observed*.
//!   That is a recorded SDK gap, not a defect here.
//! * Ring notifications are `org.matrix.msc4075.rtc.notification`
//!   ([`EV_NOTIFICATION_UNSTABLE`]). **This is why this module defines its
//!   own event content instead of using ruma's**: ruma 0.34 types that event
//!   as the stable `m.rtc.notification` with NO unstable alias, so a typed
//!   ruma handler never fires for a notification sent by a current Element.
//!   `calls.rs` had exactly that handler and therefore could not ring for
//!   Element; see [`Msc4075RtcNotificationEventContent`].
//!
//! ## Transport discovery is discovered, never assumed
//!
//! There is no hardcoded SFU anywhere in Lightning. Order of preference,
//! matching element-call's own resolution:
//!
//! 1. The homeserver's authenticated MSC4143 endpoint,
//!    `GET /_matrix/client/unstable/org.matrix.msc4143/rtc/transports`.
//! 2. Failing that, the focus the **existing participants advertise** in
//!    their own membership events (`foci_preferred`), which is what
//!    `focus_selection: "oldest_membership"` means and how every
//!    pre-endpoint deployment works. This is what lets Lightning join a
//!    call Element started on a server with no MSC4143 endpoint.
//! 3. Failing that, a URL the *user* configured. Never a default, never a
//!    vendor's server.
//!
//! An earlier MatrixRTC draft advertised foci through
//! `.well-known/matrix/client`. Current Element does not read it and this
//! module deliberately does not implement it — it would be a dead path.
//!
//! ## Safety rules specific to this surface
//!
//! Almost every string here is chosen by a remote sender, and several of
//! them end up on a control the user is invited to click. So:
//!
//! * Every inbound string is bounded and rejected if it carries control
//!   characters ([`sane`]); every collection is capped. A membership that
//!   fails validation is DROPPED, never partially trusted.
//! * `member.user_id` MUST equal the event sender. The reference
//!   implementation enforces this too, with the same reasoning: nothing
//!   defines what power level would let one user publish another's
//!   membership, so accepting it would let anyone forge a participant.
//! * A transport URL must be `https:`. An `http:` SFU would silently
//!   downgrade the signalling channel that carries call authorization.
//! * Nothing here logs a sender-chosen string, a URL, or a member id.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
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

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

/// Legacy (and currently the only *deployed*) membership state event.
pub(crate) const EV_MEMBER_LEGACY: &str = "org.matrix.msc3401.call.member";
/// MSC4143 sticky membership event. Parsed, not observable on matrix-sdk
/// 0.18 (no sticky-event support), so unused outside tests by design.
#[allow(dead_code)]
pub(crate) const EV_MEMBER_STICKY: &str = "org.matrix.msc4143.rtc.member";
/// MSC4143 slot state event, describing an open/closed session in a room.
pub(crate) const EV_SLOT: &str = "org.matrix.msc4143.rtc.slot";
/// The notification event Element actually sends (see module docs). The
/// string itself lives in the event-content derive; this is the documented
/// constant for readers and is asserted against the derive in tests.
#[allow(dead_code)]
pub(crate) const EV_NOTIFICATION_UNSTABLE: &str =
    "org.matrix.msc4075.rtc.notification";

/// The application every call-shaped session uses.
const APPLICATION_CALL: &str = "m.call";

/// `call_id: ""` is the room-wide call. The newer slot vocabulary spells the
/// same thing `"ROOM"`; the reference implementation converts between them
/// and so must we, or the same call read through the two formats looks like
/// two different calls.
const SLOT_ID_ROOM: &str = "ROOM";

/// Fallback membership validity when a membership carries no `expires`.
/// Matches the reference implementation's `DEFAULT_EXPIRE_DURATION`.
const DEFAULT_EXPIRE_MS: u64 = 4 * 60 * 60 * 1000;

/// Element caps notification lifetime at 90 s (`parseCallNotificationContent`).
const MAX_NOTIFICATION_LIFETIME_MS: u64 = 90_000;

// Bounds. Every one of these guards a value a remote party chooses.
const MAX_WIRE_LEN: usize = 255;
/// How long a raise/lower may take before it is reported as a failure. Short:
/// the control is a toggle the user is watching, and a hand that silently
/// never went up is worse than one that says it could not.
const HAND_TIMEOUT: Duration = Duration::from_secs(10);
/// How many memberships the join-time raised-hand pass will probe.
///
/// Each one is a cache-first relations load that may reach the network, and
/// this runs once per join. Beyond the cap a hand raised before we arrived is
/// simply not seen — the sync handler still catches every hand raised after,
/// so the bound costs history rather than function.
const MAX_HAND_PROBES: usize = 24;
const MAX_URL_LEN: usize = 1024;
const MAX_MEMBERS: usize = 128;
/// Bound on how many raw membership state events are PARSED. The 128-member
/// cap applies after aggregation, so without this a room carrying tens of
/// thousands of stale membership events would do all that work first.
const MAX_RAW_MEMBER_EVENTS: usize = 512;
/// Bound on the server's `/state` answer when the membership fallback runs.
/// A room's full state can be large; only membership events are wanted.
const MAX_SERVER_STATE_EVENTS: usize = 4096;
/// The membership fallback must not stall a call's key distribution.
const MEMBERSHIP_FETCH_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_TRANSPORTS: usize = 8;
#[allow(dead_code)]
const MAX_VERSIONS: usize = 8;

const DISCOVERY_TIMEOUT: Duration = Duration::from_secs(15);
/// Hard bound on the discovery response body. A transport list is a small
/// JSON object; anything larger is refused rather than buffered.
const MAX_DISCOVERY_BODY: usize = 64 * 1024;

// ---------------------------------------------------------------------------
// Sanitizers
// ---------------------------------------------------------------------------

/// Accept a bounded, control-character-free string, or nothing.
///
/// A `None` here always means "drop the surrounding thing". There is no
/// lossy repair: a membership with a mangled device id is not a membership
/// with a *slightly wrong* device id, it is untrustworthy input.
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

/// A transport URL must be absolute HTTPS with a host. `http:` is refused:
/// the URL is where a call's authorization is exchanged.
fn sane_https_url(value: &str) -> Option<String> {
    let trimmed = sane(value, MAX_URL_LEN)?;
    let parsed = url::Url::parse(trimmed).ok()?;
    if parsed.scheme() != "https" {
        return None;
    }
    // Embedded credentials in an advertised URL are never legitimate and
    // would be carried into a request in phase 2.
    if !parsed.username().is_empty() || parsed.password().is_some() {
        return None;
    }
    let host = parsed.host()?;
    // A focus is advertised by REMOTE participants, so it is attacker-
    // influenced input that phase 2 will connect to. Refuse the obvious
    // SSRF shapes here; a full DNS-resolution check belongs at the point of
    // connection, where the resolved address is actually known.
    match host {
        url::Host::Ipv4(addr) => {
            if addr.is_loopback()
                || addr.is_private()
                || addr.is_link_local()
                || addr.is_unspecified()
                || addr.is_broadcast()
                || addr.is_documentation()
            {
                return None;
            }
        }
        url::Host::Ipv6(addr) => {
            if addr.is_loopback() || addr.is_unspecified() {
                return None;
            }
            // Unique-local (fc00::/7) and link-local (fe80::/10).
            let seg = addr.segments()[0];
            if (seg & 0xfe00) == 0xfc00 || (seg & 0xffc0) == 0xfe80 {
                return None;
            }
        }
        url::Host::Domain(name) => {
            let lower = name.to_ascii_lowercase();
            if lower == "localhost" || lower.ends_with(".localhost") {
                return None;
            }
        }
    }
    Some(parsed.to_string())
}

// ---------------------------------------------------------------------------
// Transports (foci)
// ---------------------------------------------------------------------------

/// A LiveKit transport: where to obtain SFU authorization for a session.
///
/// `service_url` is the *JWT service*, not the SFU websocket. The SFU URL
/// comes back from `POST {service_url}/sfu/get`, so nothing here is a media
/// endpoint and nothing here is a credential.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct LivekitTransport {
    pub service_url: String,
    /// The SFU-side room alias, when the advertiser pinned one. Optional in
    /// the schema; the JWT service derives it from the Matrix room id when
    /// absent.
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

/// Parse one transport object. Only `type: "livekit"` is understood; any
/// other transport type is skipped rather than guessed at, because
/// advertising a transport Lightning cannot speak would make it look
/// reachable to nobody's benefit.
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

/// Which wire format a membership was read from. Diagnostics only — this
/// never reaches normal user-facing UI.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum MembershipKind {
    /// `org.matrix.msc3401.call.member` state event.
    Session,
    /// `org.matrix.msc4143.rtc.member` sticky event. Not constructible from
    /// sync on matrix-sdk 0.18; produced by the tested parser only.
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
    /// The identity this device uses on the SFU. Derived, never invented:
    /// `"{user_id}:{device_id}"` for session memberships (which is what the
    /// SFU assigns for that format), or the hashed member id for the sticky
    /// format.
    pub rtc_identity: String,
    /// `m.call#ROOM` for the room-wide call.
    pub slot_id: String,
    /// Sender-declared intent, sanitized to a closed set.
    pub intent: &'static str,
    /// When this device first joined, in ms. Falls back to the event ts.
    pub created_ts: u64,
    /// Absolute expiry in ms. A membership past this is stale and dropped —
    /// this is the only defence against a client that died without cleaning
    /// up, and it is why a stale call does not show phantom participants.
    pub expires_at_ms: u64,
    /// Foci this device advertises, in its own preference order.
    pub foci: Vec<LivekitTransport>,
    pub kind: MembershipKind,
    /// Room-resolved profile, filled in after parsing (see `read_session`).
    pub display_name: String,
    pub avatar_mxc: String,
    /// The `m.call.member` STATE EVENT that declared this membership.
    ///
    /// element-call addresses a raised hand to it: the hand is an `m.reaction`
    /// annotating the raiser's OWN membership event, so this id is the only
    /// thing that ties one to the other. Filled in by `read_session` from the
    /// event envelope — `parse_session_membership` sees content alone, and a
    /// membership read from a source that carries no envelope keeps it empty
    /// (which reads as "no hand can be matched", never as a wrong match).
    pub event_id: String,
}

impl RtcMember {
    fn to_json(&self) -> serde_json::Value {
        json!({
            "user_id": self.user_id,
            // Resolved from the ROOM, not the membership: an
            // m.call.member event carries no profile at all, and a
            // facepile of initials when the real avatar is known is a
            // quality gap. Empty when the member is not in the state
            // store, which the UI degrades to initials for.
            "display_name": self.display_name,
            "avatar_mxc": self.avatar_mxc,
            "device_id": self.device_id,
            "rtc_identity": self.rtc_identity,
            "slot_id": self.slot_id,
            "intent": self.intent,
            "created_ts": self.created_ts,
            "expires_at_ms": self.expires_at_ms,
            // The state event this membership came from. A raised hand is an
            // m.reaction annotating it, so this is what matches one to a
            // participant.
            "event_id": self.event_id,
            "kind": self.kind.as_str(),
            "foci": self.foci.iter().map(LivekitTransport::to_json)
                .collect::<Vec<_>>(),
        })
    }
}

/// Sender-declared call intent, collapsed to a closed set.
///
/// The field is free text in the schema ("may be any string"), and it drives
/// whether Lightning offers a *video* answer. An unrecognised value must
/// therefore degrade to audio rather than be forwarded verbatim.
fn intent_str(value: Option<&serde_json::Value>) -> &'static str {
    match value.and_then(|value| value.as_str()) {
        Some("video") => "video",
        Some("audio") => "audio",
        _ => "audio",
    }
}

/// Convert a legacy `call_id` to a slot id, applying the reference
/// implementation's `""` → `"ROOM"` rule so the same call read from either
/// format compares equal.
fn slot_id_for_call_id(application: &str, call_id: &str) -> String {
    let id = if call_id.is_empty() { SLOT_ID_ROOM } else { call_id };
    format!("{application}#{id}")
}

/// The SFU identity for the sticky membership format:
/// unpadded-base64(sha256(canonical JSON `[user_id, device_id, member_id]`)).
///
/// Must match `computeRtcIdentityRaw` byte for byte or Lightning and Element
/// disagree about which SFU participant is which Matrix device.
#[allow(dead_code)] // sticky lane only; see EV_MEMBER_STICKY.
pub(crate) fn rtc_identity(user_id: &str, device_id: &str, member_id: &str) -> String {
    let canonical = serde_json::to_string(&[user_id, device_id, member_id])
        .unwrap_or_default();
    let digest = Sha256::digest(canonical.as_bytes());
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD_NO_PAD.encode(digest)
}

/// Parse a legacy `org.matrix.msc3401.call.member` content.
///
/// `sender` and `event_ts` come from the event envelope, never the content:
/// a membership may not claim to belong to somebody else.
pub(crate) fn parse_session_membership(
    content: &serde_json::Value,
    sender: &str,
    event_ts: u64,
) -> Option<RtcMember> {
    let object = content.as_object()?;

    // A membership whose content is `{}` is a LEAVE (that is how the state
    // event is retracted). Not an error, and not a participant.
    if object.is_empty() {
        return None;
    }
    let application = sane_string(object.get("application"), MAX_WIRE_LEN)?;
    if application != APPLICATION_CALL {
        return None;
    }
    let device_id = sane_string(object.get("device_id"), MAX_WIRE_LEN)?;
    // `call_id` is required but legitimately empty for the room call, so it
    // cannot go through `sane` (which rejects empty).
    let call_id = object.get("call_id")?.as_str()?;
    if call_id.len() > MAX_WIRE_LEN || call_id.chars().any(|c| c.is_control()) {
        return None;
    }
    // `focus_active.type` is required by the reference validator.
    object.get("focus_active")?.as_object()?.get("type")?.as_str()?;

    let user_id = sane(sender, MAX_WIRE_LEN)?.to_owned();

    let created_ts = object
        .get("created_ts")
        .and_then(|value| value.as_u64())
        .unwrap_or(event_ts);
    let expires = object
        .get("expires")
        .and_then(|value| value.as_u64())
        .unwrap_or(DEFAULT_EXPIRE_MS);
    // Saturating: a hostile `expires` of u64::MAX must not wrap into the past.
    let expires_at_ms = created_ts.saturating_add(expires);

    // `membershipID` is optional; the documented default is exactly this.
    let rtc_identity = object
        .get("membershipID")
        .and_then(|value| value.as_str())
        .and_then(|value| sane(value, MAX_WIRE_LEN))
        .map(ToOwned::to_owned)
        .unwrap_or_else(|| format!("{user_id}:{device_id}"));

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
        // Content alone; the envelope is the caller's. read_session fills it.
        event_id: String::new(),
    })
}

/// Parse an MSC4143 `org.matrix.msc4143.rtc.member` content.
///
/// Kept in step with the reference validator, including its deliberate
/// `member.user_id == sender` rule. Not reachable from sync on matrix-sdk
/// 0.18 (no sticky-event support) — exercised by tests so the format is
/// ready and cannot silently rot.
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

    // A sticky membership must carry a sticky key under one of the two
    // spellings; without it the event has no retraction identity.
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
        // A sticky membership is RETRACTED, never aged out — the reference
        // returns no absolute expiry for this kind. Using the session
        // format's 4h fallback here would silently drop live participants
        // once this lane becomes observable.
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
}

/// Aggregate parsed memberships into a session.
///
/// Two rules that are easy to get wrong and both matter:
///
/// * **Expired memberships are dropped.** A client that vanished leaves its
///   state event behind; counting it would show a call with participants
///   that are not there, and Element does the same filtering.
/// * **Dedup is per `(user_id, device_id)`**, keeping the NEWEST
///   `created_ts`. One device can legitimately hold two membership events
///   during the state-key migration, and the same user on two devices is two
///   real participants — collapsing by user would hide one.
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
    // Oldest first: this ordering IS the `oldest_membership` focus rule, so
    // it must be stable and it must be by join time.
    out.sort_by(|a, b| {
        a.created_ts
            .cmp(&b.created_ts)
            .then_with(|| a.user_id.cmp(&b.user_id))
            .then_with(|| a.device_id.cmp(&b.device_id))
    });
    out.truncate(MAX_MEMBERS);
    out
}

/// Pick the focus to connect to, following `focus_selection:
/// "oldest_membership"`: the SFU advertised by whoever joined first.
///
/// Everyone must reach the same answer or participants land on different
/// SFUs and cannot hear each other, which is exactly why this reads the
/// oldest membership rather than any local preference.
pub(crate) fn select_focus(members: &[RtcMember]) -> Option<LivekitTransport> {
    // The OLDEST membership's own first focus, and nothing else.
    //
    // Deliberately NOT "the first member who advertises one": the reference
    // implementation reads `getOldestMembership()`'s own `foci_preferred[0]`
    // and yields `undefined` when that member advertises none — it never
    // walks on. Walking would put Lightning on a different SFU than Element
    // whenever the oldest member advertises nothing, which is precisely the
    // disagreement this rule exists to prevent.
    members.first().and_then(|member| member.foci.first().cloned())
}

// ---------------------------------------------------------------------------
// Transport discovery endpoint (MSC4143)
// ---------------------------------------------------------------------------

/// `GET https://<server_name>/.well-known/matrix/client`, read for
/// `org.matrix.msc4143.rtc_foci`.
///
/// This is where MSC4143 actually advertises the SFU, and where Element
/// Call reads it. It is NOT a client-API endpoint: an earlier version of
/// this module invented
/// `/_matrix/client/unstable/org.matrix.msc4143/rtc/transports`, which
/// exists on no server, so discovery never answered anywhere. Every call
/// then fell back to the legacy 1:1 lane — which carries no video and no
/// screen share — and starting a MatrixRTC call was impossible on any
/// homeserver including matrix.org.
///
/// The authority is ruma's own model of the well-known response:
///   #[serde(rename = "org.matrix.msc4143.rtc_foci", alias = "m.rtc_foci")]
///   pub rtc_foci: Vec<RtcTransport>
/// Both spellings are read here, unstable first, because a server that has
/// moved to the stable key should still work.
///
/// Hand-rolled over the SDK's own HTTP client rather than through ruma:
/// `Client::well_known()` is private, and the field is behind matrix-sdk's
/// `unstable-msc4143` feature which this build does not enable — turning it
/// on is a dependency change for one field. `client.http_client()` keeps the
/// SDK's TLS/proxy configuration and adds nothing, the same trade
/// `banner.rs` already made for MSC4133 profile fields.
///
/// The well-known file is PUBLIC and unauthenticated, so unlike the old
/// endpoint this sends no access token at all.
///
/// Fetched from the MXID's server name, not the resolved homeserver base
/// URL: under .well-known delegation those differ, and the delegating
/// domain is the one that serves this file.
mod transports_endpoint {
    use matrix_sdk::Client;

    /// Transport objects are open-ended by design, so the body stays raw
    /// JSON and `parse_transport` remains the single place that decides what
    /// Lightning understands.
    pub(super) struct Answer {
        pub status: u16,
        pub transports: Vec<serde_json::Value>,
    }

    fn endpoint(client: &Client) -> Result<String, String> {
        // The server name from the MXID. Bounded and host-shaped before it
        // is pasted into a URL: this reaches the network, and a hostile
        // value must not be able to redirect the request elsewhere.
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
        // Always https: the well-known file is the root of MatrixRTC
        // discovery, and fetching it over cleartext would let anyone on the
        // path choose the SFU every call is routed through.
        let url = format!("https://{server_name}/.well-known/matrix/client");
        // Parsed rather than trusted, so a value that slipped the checks
        // above still cannot produce a request to another host.
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
        // No Authorization header: .well-known is a public file, and sending
        // a bearer token to it would leak the session to any host the MXID's
        // domain resolves to.
        let response = client
            .http_client()
            .get(url)
            .timeout(timeout)
            .send()
            .await
            .map_err(|err| err.to_string())?;
        let status = response.status().as_u16();
        // Refuse an oversized answer BEFORE reading it. `text()` buffers the
        // whole body first, so trimming afterwards enforces nothing: a
        // multi-hundred-megabyte reply would already be resident.
        if response
            .content_length()
            .is_some_and(|len| len > super::MAX_DISCOVERY_BODY as u64)
        {
            return Ok(Answer { status, transports: Vec::new() });
        }
        // A server may omit Content-Length, so also stop reading at the cap
        // rather than trusting the header.
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
        // Unstable key first, stable alias second — the same pair ruma
        // reads. A server that has moved on must still work.
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

/// `org.matrix.msc4075.rtc.notification` — the ring.
///
/// Defined here rather than taken from ruma because ruma 0.34 types this
/// event as the *stable* `m.rtc.notification` with no unstable alias, so a
/// ruma-typed handler is deaf to every notification current Element sends.
/// Both are observed (see [`register_rtc_handlers`]); this one is what gets
/// sent, matching Element.
#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4075.rtc.notification", kind = MessageLike)]
pub(crate) struct Msc4075RtcNotificationEventContent {
    /// `"ring"` for a DM-style ring, `"notification"` for a group call
    /// announcement that must not ring every member indefinitely.
    pub notification_type: String,
    pub sender_ts: MilliSecondsSinceUnixEpoch,
    /// Milliseconds. Serialized as a plain integer, which is what the
    /// reference implementation reads.
    pub lifetime: u64,
    #[serde(rename = "m.mentions", default, skip_serializing_if = "Option::is_none")]
    pub mentions: Option<Mentions>,
    #[serde(rename = "m.relates_to", default, skip_serializing_if = "Option::is_none")]
    pub relates_to: Option<Reference>,
    #[serde(rename = "m.call.intent", default, skip_serializing_if = "Option::is_none")]
    pub call_intent: Option<String>,
}

/// Legacy membership state event, typed only so the SDK will hand us a
/// change notification for it.
///
/// The content is intentionally a passthrough map: this handler's whole job
/// is to say "membership in this room changed", after which the session is
/// re-read from the state store through the single parser above. Duplicating
/// the parse here would give us two places to disagree with Element from —
/// the same payload-free-poke discipline `room_pinned_changed` already uses.
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

/// Reach the raw JSON of a state event the store handed us.
///
/// `RawAnySyncOrStrippedState` is an enum over two `Raw` types (a joined room
/// versus an invite's stripped state), so there is no single `Raw` to
/// deserialize; both carry the envelope fields this module needs.
fn raw_state_json(raw: &RawAnySyncOrStrippedState) -> Option<serde_json::Value> {
    let json = match raw {
        RawAnySyncOrStrippedState::Sync(raw) => raw.json(),
        RawAnySyncOrStrippedState::Stripped(raw) => raw.json(),
    };
    serde_json::from_str(json.get()).ok()
}

/// Read and parse every membership in a room from the state store.
///
/// State-store backed, so this is cheap and needs no request. It only sees
/// what sync has delivered, which is the same constraint every other
/// state-driven surface in Lightning lives with.
/// Every membership state event for a room, as raw JSON.
///
/// The STORE is asked first and the HOMESERVER second, and the second half is
/// not an optimisation — it is what makes the call work at all.
///
/// Sliding sync does list `CallMember` in its default `required_state`, so the
/// store does receive these events; what it does not guarantee is receiving
/// them PROMPTLY for a room the client is not actively subscribed to. Measured
/// against a real homeserver: during a live call the store held thirteen
/// membership events and every one of them was a stale RETRACTION, while the
/// server's own `/state` had the live memberships — including this device's
/// own, published seconds earlier and acknowledged.
///
/// That is not a cosmetic lag. Media keys are addressed to the devices named
/// by these events, so an empty read means a key is sent to NOBODY: both ends
/// encrypt, neither can decrypt, and every frame is dropped for want of a key
/// while the call otherwise looks perfectly connected. Audio, video and screen
/// share all fail together, which is exactly how it was reported.
///
/// The network read is therefore a FALLBACK, spent only when the store yields
/// no usable membership, so a healthy store still costs nothing. Same trade
/// `banner.rs` makes, and for the same reason.
async fn read_membership_events(
    room: &Room,
    now_ms: u64,
) -> Vec<serde_json::Value> {
    let from_store: Vec<serde_json::Value> = room
        .get_state_events(StateEventType::from(EV_MEMBER_LEGACY))
        .await
        .unwrap_or_default()
        .iter()
        .take(MAX_RAW_MEMBER_EVENTS)
        .filter_map(raw_state_json)
        .collect();

    // "Usable" means at least one membership that is neither retracted nor
    // expired. A room whose only memberships are retractions reads exactly
    // like a room the store has not caught up on, so both take the fallback.
    let store_has_live = from_store.iter().any(|value| {
        value
            .get("content")
            .and_then(|content| content.as_object())
            .is_some_and(|content| !content.is_empty())
    });
    if store_has_live {
        return from_store;
    }

    let client = room.client();
    let config = RequestConfig::new()
        .disable_retry()
        .timeout(MEMBERSHIP_FETCH_TIMEOUT);
    let request = get_state_events::v3::Request::new(room.room_id().to_owned());
    let Ok(response) = client.send(request).with_request_config(config).await
    else {
        return from_store;
    };
    let mut from_server = Vec::new();
    for raw in response.room_state.iter().take(MAX_SERVER_STATE_EVENTS) {
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
    let _ = now_ms;
    if from_server.is_empty() {
        from_store
    } else {
        from_server
    }
}

async fn read_session(room: &Room, now_ms: u64) -> RtcSession {
    let mut members = Vec::new();

    let raw_members = read_membership_events(room, now_ms).await;

    for value in raw_members.iter() {
        // Deserialize as loose JSON: the envelope fields we need are
        // `sender`, `content` and `origin_server_ts`, and a membership we
        // cannot read must not poison the ones we can.
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
            // The envelope's own id, which parse_session_membership cannot
            // see. A raised hand is an m.reaction annotating THIS event, so
            // without it a hand can never be matched to its participant.
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
    // Resolve profiles AFTER aggregation: expired and duplicate memberships
    // are already gone, so no lookup is spent on a member we will not report.
    // `_no_sync` deliberately: this must not trigger a network round trip
    // per participant just to draw a facepile.
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
    }
}

/// Read the MSC4143 slot state, when a room has one.
///
/// Absence is NOT "closed": almost no deployment publishes a slot yet, so
/// treating a missing slot as a closed session would hide every real call.
/// Only an explicit `status: "closed"` closes one.
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
        // EXACT match only. An empty state key is not the room call's slot,
        // and treating it as one would let anyone who can send this state
        // type suppress the room's entire call display.
        let Some(state_key) = value.get("state_key").and_then(|v| v.as_str())
        else {
            continue;
        };
        if state_key != room_slot {
            continue;
        }
        present = true;
        // FAIL-CLOSED, matching the reference: the session is open only if
        // it says so AND the slot is for this application. Anything else —
        // a missing status, an unknown status, a slot belonging to a
        // different application — is not an open m.call session.
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

/// Coarse, closed-set category for a discovery HTTP status. Never the body:
/// a server error message is remote text.
fn status_category(status: u16) -> &'static str {
    match status {
        400 | 404 | 405 => "unsupported",
        401 | 403 => "forbidden",
        429 => "rate_limited",
        500..=599 => "server_error",
        _ => "unknown",
    }
}

/// Local wall clock. Compared against server-supplied `created_ts` /
/// `origin_server_ts` for expiry, so a badly skewed device clock can drop
/// live participants (or keep dead ones). The reference implementation has
/// the same property; noted so a skew symptom is not misdiagnosed as a
/// parsing bug.
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
        "focus": focus.as_ref().map(LivekitTransport::to_json),
        "members": session.members.iter().map(RtcMember::to_json)
            .collect::<Vec<_>>(),
    })
}

// ---------------------------------------------------------------------------
// FFI-facing operations
// ---------------------------------------------------------------------------

/// Report the current MatrixRTC session for one room.
pub(crate) fn request_session(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let session = read_session(&room, now_ms()).await;
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

/// Discover the SFU transports this account can use.
///
/// Reports the homeserver's own answer plus the focus advertised by the
/// named room's existing participants, and says which source produced what,
/// so the C++ side can apply policy (and tell the user *why* calling is
/// unavailable) instead of receiving one opaque list.
pub(crate) fn request_transports(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    // A room is optional: discovery is account-scoped, the room only adds
    // the participant-advertised fallback.
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
            // A server with no MSC4143 support answers 404/400/M_UNRECOGNIZED.
            // That is "this homeserver has no MatrixRTC", NOT a transient
            // failure, and the two must stay distinguishable so the UI can
            // say which one happened.
            Ok(answer) => Err(status_category(answer.status).to_owned()),
            Err(err) => Err(classify_room_error(&err).to_owned()),
        };

        let mut participant_focus = None;
        if let Some(room) = room.as_ref() {
            let session = read_session(room, now_ms()).await;
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
            // Empty with an empty category means the server answered and
            // advertised nothing — genuinely "no MatrixRTC here", which is
            // a different fact from "the request failed".
            "server_answered": category.is_empty(),
            "category": category,
            "server_transports": server_transports.iter()
                .map(LivekitTransport::to_json).collect::<Vec<_>>(),
            "participant_focus": participant_focus.as_ref()
                .map(LivekitTransport::to_json),
        }));
    });
    Ok(())
}

/// Send an `org.matrix.msc4075.rtc.notification`.
///
/// `notification_type` is clamped to the two values the reference
/// implementation defines. `lifetime` is clamped to Element's own 90 s cap:
/// a longer-lived ring would keep other clients ringing past the point they
/// consider the notification valid.
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

    // Relating the notification to our own membership event is what lets a
    // receiver connect the ring to a session (and lets a decline target it).
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
        // `room: true` is what makes the notification reach the room's
        // members through push rules; Element sends exactly this.
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

/// How long a published membership claims to be valid WHEN THE SERVER CAN
/// RETRACT IT FOR US.
///
/// 4 h is the reference implementation's `DEFAULT_EXPIRE_DURATION`, and it is
/// only defensible while an MSC4140 delayed retraction is armed: that is what
/// actually cleans up after a crash, and `expires` is then a distant backstop
/// nobody is expected to reach. The comment that used to sit here called this
/// "deliberately SHORT ... the refresh below runs at a third of it", which was
/// never true of any code — nothing refreshed the state event at all, so on a
/// server without delayed events a dead client sat in the call for FOUR HOURS.
/// That is the maintainer's "multiple same users sit in the call".
const MEMBERSHIP_EXPIRY_MS: u64 = 4 * 60 * 60 * 1000;

/// How long a published membership claims to be valid WHEN NOTHING SERVER-SIDE
/// WILL RETRACT IT.
///
/// On a homeserver without MSC4140 (Synapse gates it behind
/// `experimental_features.msc4140_enabled`, OFF by default) the client is the
/// only cleanup there is, and a client that is dead cannot send anything. The
/// only remaining mechanism is `expires` running out — so it must run out in
/// minutes, not hours, and the client must re-publish often enough that a live
/// participant never ages out.
///
/// 5 minutes against `SfuCallController`'s 60 s re-publish cadence: FIVE
/// consecutive failed refreshes are survivable before a live participant
/// disappears from anyone's list. Shortening this without that re-publish
/// cadence would be strictly WORSE than four hours — it would start removing
/// people who are still talking.
const MEMBERSHIP_EXPIRY_NO_DELAYED_MS: u64 = 5 * 60 * 1000;

/// The `expires` DURATION to write, given how old this membership already is.
///
/// `expires` is measured from `created_ts`, NOT from now: every client reads
/// the absolute deadline as `created_ts + expires` — including this file's own
/// parser, which does `created_ts.saturating_add(expires)`. A refresh
/// deliberately PRESERVES `created_ts` (so the oldest-membership focus
/// selection does not reorder), so writing the same constant on every refresh
/// republishes the very same absolute instant. The membership then dies a
/// fixed period after the JOIN however often it is refreshed.
///
/// THAT WAS THE REPORTED DEFECT: a Lightning participant vanished from every
/// other client's list exactly `MEMBERSHIP_EXPIRY_NO_DELAYED_MS` after joining
/// and reappeared a few seconds later, over and over. It also explains the
/// lopsided symptom — the peers aged the membership out and rotated media keys
/// WITHOUT that user, so they could still be heard (their own media kept
/// flowing to an SFU that had never disconnected them) while they could hear
/// nobody. The 60 s re-publish cadence was running correctly the whole time;
/// it was writing a value that could not extend anything.
///
/// Clock domains: `created_ts` comes from the server (the content field, or
/// `origin_server_ts` when the first publish omitted it) and `now_ms` is
/// local. The reference implementation mixes them the same way. Skew only
/// matters at the scale of the refresh slack — five refreshes at 60 s against
/// a 5 minute period — and a client whose clock runs BEHIND the server merely
/// re-publishes sooner than it had to.
fn expires_for_refresh(period_ms: u64, created_ts: Option<u64>, now_ms: u64)
    -> u64
{
    match created_ts {
        // A first publish carries no created_ts, so peers date it from this
        // event's own origin_server_ts: the duration IS the period.
        None => period_ms,
        // Saturating both ways: a created_ts in the FUTURE (skew, or a hostile
        // value read back from our own state) must not wrap, and yields the
        // plain period — an absolute deadline still at least a period out.
        Some(created) => now_ms
            .saturating_sub(created)
            .saturating_add(period_ms),
    }
}

/// Delayed-event (MSC4140) timeout — the server retracts our membership for
/// us if we stop restarting it. This is the ONLY cleanup that survives a
/// crash, a kill, or a lost network.
const DELAYED_LEAVE_TIMEOUT_MS: u64 = 8_000;

/// Has this process seen a homeserver REFUSE to arm a delayed retraction?
///
/// Purely an optimisation, and deliberately one-directional in effect: it
/// decides only which `expires` the FIRST write of a publish carries, so that
/// a server known to lack MSC4140 does not cost two state events on every
/// single refresh. Every publish still verifies the assumption against what
/// the server actually does and corrects it, so a wrong value here costs one
/// extra request, never a wrong membership.
static DELAYED_EVENTS_REFUSED: AtomicBool = AtomicBool::new(false);

/// Build the state key Element writes.
///
/// `{user}_{device}_{application}{slotId}`, with a LEADING UNDERSCORE except
/// on room versions that allow a user-scoped state key to be owned by its
/// user (`org.matrix.msc3757`/`msc3779`). Getting this wrong means the
/// server refuses the write, or worse, that our membership does not replace
/// our own previous one and we appear twice.
pub(crate) fn membership_state_key(
    user_id: &str,
    device_id: &str,
    room_version: &str,
) -> String {
    // The room call's slot id is "" in a state key (the "ROOM" spelling is
    // the newer vocabulary and is NOT what goes on the wire here).
    let key = format!("{user_id}_{device_id}_{APPLICATION_CALL}");
    if room_version.starts_with("org.matrix.msc3757")
        || room_version.starts_with("org.matrix.msc3779")
    {
        key
    } else {
        format!("_{key}")
    }
}

/// The membership content Lightning publishes.
///
/// Deliberately the LEGACY session format: it is what every deployed server
/// and every current Element understands. The sticky format is parsed but
/// not written, because matrix-sdk 0.18 cannot send a sticky event at all.
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
        // The SFU participant identity for this format. The SFU assigns
        // exactly this, so it must match or our media cannot be attributed
        // to our membership.
        "membershipID": format!("{user_id}:{device_id}"),
        // NOT a constant. How long this membership claims to live depends on
        // whether anything but us will ever retract it — see
        // MEMBERSHIP_EXPIRY_NO_DELAYED_MS.
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
    // On an UPDATE (a refresh), created_ts must keep pointing at the original
    // join or every refresh looks like a fresh join and reorders the
    // oldest-membership focus selection under everyone's feet.
    if let Some(created) = created_ts {
        content["created_ts"] = json!(created);
    }
    content
}

/// Publish (or refresh) our own membership in a room's call.
///
/// Two writes, in this order, and the order matters:
///  1. The membership state event itself.
///  2. A DELAYED retraction (MSC4140) scheduled a few seconds out, which the
///     client then restarts periodically. If Lightning dies, the server
///     fires it and our membership disappears — without this, a crash leaves
///     a phantom participant in the call until the 4 h expiry.
///
/// A server without MSC4140 simply refuses step 2; that is reported as
/// `delayed_unsupported`, not as a failure, because the membership itself is
/// published and the call works — it just relies on `expires` for cleanup.
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

        // Preserve created_ts across a refresh: read our own membership back
        // and reuse its join time.
        let created_ts = read_own_created_ts(&room, &state_key).await;
        // CARRY THE ROOM AS `livekit_alias`, which is what Element publishes.
        //
        // The alias names the SFU room a client should be placed in, and
        // lk-jwt-service derives that name from it. Advertising a focus
        // WITHOUT one is not obviously wrong — the service falls back to the
        // room we send in `/sfu/get` — but it makes our membership a shape no
        // other client in the ecosystem produces, and a peer comparing the two
        // transports sees different objects for the same SFU. Matching Element
        // exactly costs nothing and removes the difference from the question.
        let focus = focus.map(|transport| LivekitTransport {
            service_url: transport.service_url.clone(),
            alias: transport
                .alias
                .clone()
                .or_else(|| Some(room.room_id().to_string())),
        });

        // The expiry we ASSUME is right, from what this process has already
        // learned about the server. Assumption, not fact — it is checked
        // against what the server does with the delayed retraction below, and
        // corrected there.
        let assumed_no_delayed = DELAYED_EVENTS_REFUSED.load(Ordering::Relaxed);
        let period_ms = if assumed_no_delayed {
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        } else {
            MEMBERSHIP_EXPIRY_MS
        };
        // Measured from created_ts, which a refresh preserves — see
        // expires_for_refresh. A constant here expires the membership a fixed
        // period after the JOIN, not after the refresh.
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

        // The delayed retraction. Only attempted once the membership is
        // actually published — scheduling a retraction for something that
        // does not exist is pointless.
        let mut delay_id = String::new();
        let mut delayed_category = String::new();
        if ok {
            match schedule_delayed_leave(&client, room.room_id().as_str(),
                                         &state_key).await
            {
                Ok(id) => delay_id = id,
                Err(category) => delayed_category = category,
            }
        }

        // RECONCILE THE ASSUMPTION WITH WHAT THE SERVER ACTUALLY DID.
        //
        // Only ONE of the two corrections rewrites the state event, and that
        // is deliberate. Rewriting a state event while a delayed retraction is
        // armed is a question nobody here has measured — MSC4140 may or may
        // not cancel delayed events for the same (room, type, state key) when
        // a new one is sent, and getting it wrong either strands an armed
        // retraction that fires mid-call or leaves us with none. So:
        //
        //   * assumed delayed events work, they do NOT  -> re-publish SHORT.
        //     There is provably no armed event to disturb (arming is what
        //     just failed), and without this the membership would claim four
        //     hours of validity that nothing will ever cut short.
        //   * assumed they do NOT work, they DO -> CANCEL the delayed event we
        //     just armed and report no delay id. This publish then behaves
        //     exactly like the no-MSC4140 case it was written for (short
        //     expiry, client re-publishes), which is correct if slower, and
        //     the NEXT publish carries the long expiry and a real delayed
        //     retraction. No state event is rewritten under an armed event.
        if ok && assumed_no_delayed && !delay_id.is_empty() {
            DELAYED_EVENTS_REFUSED.store(false, Ordering::Relaxed);
            cancel_delayed_leave(&client, &delay_id).await;
            delay_id.clear();
            delayed_category = "delayed_resynced".to_owned();
        } else if ok && !assumed_no_delayed && delay_id.is_empty() {
            DELAYED_EVENTS_REFUSED.store(true, Ordering::Relaxed);
            // A SECOND write, with the short expiry. It replaces our own
            // previous state event under the same state key, so the room sees
            // one membership, not two — and created_ts is unchanged, so
            // oldest-membership focus selection does not move.
            let short = own_membership_content(
                &device_id, &user_id, focus.as_ref(), intent, created_ts,
                MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
            let retry = tokio::time::timeout(
                DISCOVERY_TIMEOUT,
                room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key, short),
            )
            .await;
            match retry {
                Ok(Ok(response)) => event_id = response.event_id.to_string(),
                // The FIRST write landed, so we are in the call — but with a
                // four-hour expiry and no server-side cleanup, which is the
                // exact ghost this whole path exists to prevent. Report the
                // publish as failed so the caller does not proceed into a call
                // it cannot clean up after.
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
            // Empty means no server-side cleanup is armed; the caller must
            // then rely on `expires` and say so honestly in diagnostics.
            "delay_id": delay_id,
            "delayed_category": delayed_category,
        }));
    });
    Ok(())
}

/// Read the `created_ts` of our own existing membership, if it is STILL LIVE.
///
/// `created_ts` is preserved across a refresh because it orders
/// oldest-membership focus selection, and resetting it would reshuffle
/// everyone's chosen SFU. But every reader computes validity as
/// `created_ts + expires`, so inheriting the timestamp of a membership that
/// has ALREADY EXPIRED publishes a membership that is born expired: every
/// other client drops it before it renders, and the person shows up as a
/// member with no media — element-call's "waiting for media". A ghost left by
/// a previous session is exactly the case that produces one.
///
/// So the timestamp is inherited only while the membership it came from is
/// still live. A fresh join after an expired ghost starts its own clock.
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

/// The pure half of read_own_created_ts, so the rule is testable offline.
/// Reading a Room needs a live SDK; deciding whether a timestamp may be
/// inherited does not, and it is the part that was wrong.
fn inheritable_created_ts(
    content: &serde_json::Value,
    origin_server_ts: Option<u64>,
    now_ms: u64,
) -> Option<u64> {
    // An empty content is a retracted membership: this is a fresh join.
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
    // Saturating for the same reason the parser is: a hostile `expires` of
    // u64::MAX must not wrap into the past and make a live membership look
    // dead. A timestamp in the FUTURE is also "still live" and is inherited —
    // clock skew is a real condition and is not this function's to police.
    if created.saturating_add(expires) <= now_ms {
        return None;
    }
    Some(created)
}

/// Cancel one delayed event. Best effort and result-free on purpose: every
/// caller is already committed to whatever it is doing, and a delayed event
/// that cannot be cancelled fires a retraction of a membership we are about
/// to rewrite anyway.
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

/// Schedule the server-side retraction of our membership.
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
    // An EMPTY content is how a membership is retracted.
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
        Ok(Ok(response)) => Ok(response.delay_id),
        // A server without MSC4140 answers 404/400. That is not a failure of
        // the call — it only means cleanup falls back to `expires`.
        Ok(Err(err)) => Err(classify_room_error(&err.to_string()).to_owned()),
        Err(_) => Err("network".to_owned()),
    }
}

/// Restart the delayed retraction, so it keeps not-firing while we are alive.
pub(crate) fn restart_delayed_leave(
    bridge: &RustClient,
    delay_id: String,
    op_id: u64,
) -> Result<(), String> {
    update_delayed(bridge, delay_id, "restart", op_id)
}

/// Retract our membership immediately: send the empty content ourselves AND
/// cancel the pending delayed event, so nothing fires later against a
/// membership we already removed.
pub(crate) fn retract_membership(
    bridge: &RustClient,
    room_id: String,
    delay_id: String,
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

        // Retract FIRST. If the delayed cancel fails afterwards the worst
        // case is a redundant no-op retraction; doing it the other way round
        // would leave a window with neither.
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

/// The reaction key element-call uses for a raised hand.
///
/// U+1F590 RAISED HAND WITH FINGERS SPLAYED followed by U+FE0F VARIATION
/// SELECTOR-16. READ OUT OF element-call's own source, not chosen here:
/// `src/reactions/useReactionsSender.tsx` sends exactly this string and
/// `src/reactions/ReactionsReader.ts` compares against exactly this string,
/// so a different hand emoji — or the same one without the variation
/// selector — is a hand no Element client will ever see.
pub(crate) const HAND_RAISED_KEY: &str = "\u{1F590}\u{FE0F}";

/// Raise or lower this device's hand.
///
/// THE WIRE FORMAT IS element-call's, and there is nothing of Lightning's own
/// invention in it:
///
///   raise  → an `m.reaction` whose `m.relates_to` is
///            `{ rel_type: "m.annotation", event_id: <MY OWN m.call.member
///            state event>, key: "🖐️" }`
///   lower  → a REDACTION of that reaction
///
/// The target is the sender's own MEMBERSHIP state event, not a timeline
/// message, which is what scopes the hand to one call rather than to the
/// room's history: a new membership (rejoining, refreshing) is a new event,
/// so an old hand cannot follow the user into the next call.
///
/// `reaction_event_id` is required to LOWER and ignored to raise — a hand can
/// only be lowered by redacting the specific event that raised it, and this
/// device is the only thing that knows which one that was.
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
                // The id the redaction will need. Without it a raised hand
                // can never be lowered by this device.
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

/// Read the hands already raised in a room's call.
///
/// A hand raised BEFORE this client joined produces no sync event for us, so
/// without this pass a participant who raised early is invisible for the rest
/// of the call. element-call solves it the same way, walking the annotations
/// of each membership event.
///
/// BOUNDED, because this is one request per membership in the worst case:
/// `MAX_HAND_PROBES` memberships, cache-first
/// (`load_or_fetch_event_with_relations` only reaches the network on a miss),
/// and the whole pass is spent ONCE per join rather than per poke. A poke is
/// answered by the sync handler below, which costs nothing.
///
/// A membership whose annotations cannot be read contributes NOTHING rather
/// than a lowered hand: absence of evidence is not evidence that a hand is
/// down, and reporting one as lowered would clear a hand that is really up.
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
        let session = read_session(&room, now_ms).await;
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
                // THE SENDER MUST BE THE MEMBER THEMSELVES. Anyone may
                // annotate anyone's membership event; only the owner of that
                // membership raising their own hand means anything, and
                // without this check one user could raise everybody's.
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
                // A redacted reaction keeps its envelope and loses its
                // content, so the key is simply absent — which reads as "not
                // a raised hand", exactly as it should.
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
// Media encryption keys (io.element.call.encryption_keys)
// ---------------------------------------------------------------------------

/// The to-device event type Element uses for call media keys. Unstable and
/// element-prefixed on the wire; that is what interoperates.
pub(crate) const EV_CALL_KEYS: &str = "io.element.call.encryption_keys";
/// Media keys are 32 raw bytes; LiveKit's HKDF turns them into AES-128-GCM.
const MEDIA_KEY_BYTES: usize = 32;
/// The key ring has 16 slots (LiveKit's `keyringSize`), so an index must fit.
const MAX_KEY_INDEX: u8 = 15;

/// Send our current media key to the devices in the call.
///
/// Encrypted per device through Olm (`encrypt_and_send_raw_to_device`), so
/// the homeserver never sees the key. `targets` are `(user_id, device_id)`
/// pairs taken from the observed membership — we send only to devices that
/// have actually declared themselves present in this call.
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
    if key_index > MAX_KEY_INDEX {
        return Err("key index out of range".to_owned());
    }
    // Parsed here so a malformed list fails synchronously rather than
    // half-sending.
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

        // Resolve the target devices. A device we cannot resolve is SKIPPED,
        // never substituted: sending one participant's key to the wrong
        // device would be worse than that participant not hearing us.
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
            let mut found =
                client.encryption().get_device(&user_id, &device_id).await;
            // `get_device` is a STORE lookup and does not fetch anything.
            //
            // A peer whose device keys this client has never downloaded is
            // simply absent from it, and the key was then sent to NOBODY —
            // reported as `no_devices`, after which both ends encrypt and
            // neither can decrypt, so audio, video and screen share all fail
            // together while the call looks connected. That is reachable in
            // ordinary use: the first call in a room, a device that joined
            // the call after our last `/keys/query`, or any peer the crypto
            // store has not caught up on.
            //
            // `request_user_identity` performs a real `/keys/query` for that
            // user, so the store is populated and the SECOND lookup succeeds.
            // Spent only on a miss, so a warm store costs nothing extra.
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
                        // The SDK answers with the devices it could NOT
                        // reach, so a partial delivery is visible rather
                        // than silently successful.
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
        // The KEY ITSELF is never enqueued, never logged. Only counts.
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

/// Inbound media key, decrypted by Olm.
///
/// `sender` and the Olm-verified device are what we trust; the `member`
/// block in the content is CLAIMED and is used only to fill in an id, never
/// to decide who sent it.
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

/// Register MatrixRTC observation.
///
/// Two handlers, both deliberately thin:
///
/// * membership changes enqueue a payload-free poke; the C++ side answers by
///   re-reading the session, so local and remote converge on ONE parse path.
/// * the MSC4075 notification is decoded into the same `call_rtc_*` lane
///   `calls.rs` already owns, so the ring policy, the ignore check and the
///   incoming-call surface need no second implementation.
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

    // Raised hands, in both directions.
    //
    // element-call represents a raised hand as an `m.reaction` annotating the
    // raiser's own `m.call.member` state event, and a lowered one as a
    // REDACTION of that reaction. Both arrive here as ordinary timeline
    // events, so observing them costs one filter each and no requests.
    //
    // DELIBERATELY UNFILTERED BY ROOM. A reaction is cheap to inspect and the
    // vast majority are ordinary message reactions rejected by the key check
    // on the first comparison; filtering by "the room we are in a call in"
    // would need this handler to know about call state it has no business
    // holding, and would drop a hand raised in the window between joining and
    // that state being written.
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
                    // The C++ side matches this against the membership it
                    // already holds, and REQUIRES the sender to be that
                    // membership's own user — anyone may annotate anyone's
                    // state event, and without that check one user could
                    // raise everybody's hand.
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
                    // A redaction names what it removed, and nothing else
                    // here can say WHAT was redacted — the event is gone. So
                    // every redaction is forwarded and the C++ side answers
                    // "was that one of the reactions I am tracking?", which
                    // it can, because it holds the reaction ids.
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
                        // Marks this as the MatrixRTC lane rather than a
                        // legacy m.call.invite, so one call cannot ring twice.
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
            move |ev: CallEncryptionKeysEvent| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    // Bound and validate every field: this arrives from
                    // another device and is used to key a cipher.
                    if ev.content.keys.index > MAX_KEY_INDEX {
                        return;
                    }
                    let Some(key) = sane(&ev.content.keys.key, 512) else {
                        return;
                    };
                    let Some(room_id) = sane(&ev.content.room_id, MAX_WIRE_LEN)
                    else {
                        return;
                    };
                    let Some(device_id) =
                        sane(&ev.content.member.claimed_device_id,
                             MAX_WIRE_LEN)
                    else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "rtc_key_received",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room_id,
                        // The SENDER is what the SDK vouches for after Olm
                        // decryption. `member.id` is a claim and is not used
                        // to decide identity.
                        "sender": ev.sender.to_string(),
                        "claimed_device_id": device_id,
                        "key_index": ev.content.keys.index,
                        // The key itself: C++ memory only, never QML, never
                        // logged. It is base64 exactly as it arrived.
                        "key": key,
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
        // `call_id: ""` is the room call and MUST normalise to the slot
        // vocabulary, or the same call read two ways looks like two calls.
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
        assert_eq!(member.expires_at_ms, u64::MAX);
        // Still live, which is the point: saturation must not underflow.
        assert_eq!(aggregate_session(vec![member], 10_000).len(), 1);
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
        // The discovery LOCATION was wrong for the whole of the MatrixRTC
        // work: an invented
        // `/_matrix/client/unstable/org.matrix.msc4143/rtc/transports`
        // endpoint that exists on no server. Discovery therefore never
        // answered anywhere, every call fell back to the legacy 1:1 lane
        // (no video, no screen share), and starting a MatrixRTC call was
        // impossible on any homeserver.
        //
        // The real place is `.well-known/matrix/client`, under
        // `org.matrix.msc4143.rtc_foci` with `m.rtc_foci` as the stable
        // alias — exactly what ruma models and what Element Call reads.
        // Pinned as the literal KEY NAMES, because the mistake was a name,
        // not a shape: the transport objects parsed correctly all along.
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

        // And the key we used to read is NOT a source. A server that
        // happens to carry it must not resurrect the wrong mechanism.
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
        // Everyone must independently reach the same SFU, so the rule is
        // "oldest membership wins" and NOT "first one we happened to parse".
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
        // The reference reads the OLDEST membership's own foci and yields
        // nothing when it advertises none — it never walks on to a later
        // member. Walking would put Lightning on a different SFU than
        // Element in exactly this case, which is the disagreement the
        // oldest-membership rule exists to prevent.
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
        // A focus is advertised by REMOTE participants, so it is
        // attacker-influenced and phase 2 will connect to it.
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
        // The sender is who the server vouched for; member.user_id is only a
        // claim. Accepting a mismatch would let anyone invent participants.
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
    fn rtc_identity_is_the_hash_the_reference_implementation_computes() {
        // unpadded base64 of sha256 over the canonical JSON array. Pinned
        // because Lightning and Element must agree which SFU participant is
        // which Matrix device; a different encoding silently mismatches.
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
        // Element writes `_{user}_{device}_{application}` and drops the
        // leading underscore only on room versions that let a user own a
        // user-scoped state key. Getting this wrong means either the server
        // refuses the write, or our refresh does not replace our own
        // previous membership and we appear TWICE in the call.
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
        // The strongest interop check available offline: what we WRITE must
        // parse back as a valid membership under the same rules we apply to
        // Element's.
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
        // created_ts is what orders oldest-membership focus selection. If a
        // refresh reset it, everyone's chosen SFU would reshuffle every few
        // minutes and participants would drift onto different servers.
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
        // THE FOUR-HOUR GHOST. `expires` is the ONLY cleanup a homeserver
        // without MSC4140 has, because a client that was killed cannot send a
        // retraction. Publishing four hours there is what left "multiple same
        // users sit in the call".
        //
        // FAILS ON THE OLD CODE: `expires` was the MEMBERSHIP_EXPIRY_MS
        // constant unconditionally and this function took no expiry at all.
        let content = own_membership_content(
            "DEVICE", "@a:x", None, "audio", None,
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
        let member = parse_session_membership(&content, "@a:x", 5_000)
            .expect("valid");
        assert_eq!(
            member.expires_at_ms,
            5_000 + MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        );
        // And it must be survivable: SfuCallController re-publishes every
        // 60 s, so the window has to absorb several consecutive failures.
        // If this ever drops below ~3 refresh intervals it starts removing
        // people who are still talking, which is worse than the ghost.
        assert!(MEMBERSHIP_EXPIRY_NO_DELAYED_MS >= 3 * 60 * 1000);
    }

    /// The reported defect, stated as the peers see it.
    ///
    /// A peer's deadline is `created_ts + expires`. Refreshing must MOVE that
    /// deadline; before the fix it was constant, so the participant dropped
    /// out exactly one period after joining however many refreshes ran.
    #[test]
    fn refreshing_a_membership_moves_the_deadline_peers_compute() {
        let period = MEMBERSHIP_EXPIRY_NO_DELAYED_MS;
        let created = 1_000_000_u64;

        // First publish: no created_ts yet, peers date it from this event.
        assert_eq!(expires_for_refresh(period, None, created), period);

        // Refreshes at 60 s. The ABSOLUTE deadline must stay a full period
        // ahead of the moment of the refresh, every time.
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

        // The exact reported case: still live well past the join + period
        // instant that used to kill it.
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
        // A membership left behind by a previous session is EXPIRED. Reusing
        // its created_ts makes the next join's expires_at land in the past —
        // the membership is born dead, every other client drops it, and the
        // person appears in the call with no media ("waiting for media").
        //
        // FAILS ON THE OLD CODE: read_own_created_ts returned created_ts for
        // any non-empty content, with no liveness check at all.
        let ghost = json!({
            "created_ts": 1_000u64,
            "expires": 5 * 60 * 1000u64,
            "device_id": "DEVICE",
        });
        // now is well past created_ts + expires.
        assert_eq!(inheritable_created_ts(&ghost, None, 9_000_000), None);
        // Still inside its window: inherited, because focus ordering depends
        // on it and a refresh must not reshuffle everyone's SFU.
        assert_eq!(
            inheritable_created_ts(&ghost, None, 200_000),
            Some(1_000)
        );
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
        // A hostile expires must not wrap into the past and make a live
        // membership look dead.
        let hostile = json!({ "created_ts": 10u64, "expires": u64::MAX });
        assert_eq!(
            inheritable_created_ts(&hostile, None, u64::MAX - 1),
            Some(10)
        );
    }

    #[test]
    fn a_membership_without_a_focus_is_still_valid() {
        // Joining a call whose focus came from the server endpoint (rather
        // than from a peer) publishes no foci_preferred of its own.
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
        // Free text in the schema, so anything unrecognised must degrade to
        // audio rather than reach the UI and offer a video answer.
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
        // Absent rather than null: Element's validator rejects a malformed
        // relation, and `null` is malformed.
        assert!(value.get("m.relates_to").is_none());
    }

    #[test]
    fn the_notification_event_type_is_the_one_element_sends() {
        // Regression guard for the actual defect this module fixes: ruma
        // types this event as the stable `m.rtc.notification`, so a
        // ruma-typed handler is deaf to every current Element ring.
        assert_eq!(
            Msc4075RtcNotificationEventContent::TYPE,
            "org.matrix.msc4075.rtc.notification"
        );
    }

    /// The one thing about a raised hand that MUST NOT DRIFT.
    ///
    /// element-call's `ReactionsReader` compares `m.relates_to.key` against
    /// this exact string, so a different hand emoji — or the same one without
    /// the U+FE0F variation selector — is a hand no Element client will ever
    /// see, and one of theirs is a hand we will never show. Asserted by BYTES
    /// because that is what goes on the wire and because the two forms are
    /// visually identical in every editor.
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

    /// A membership carries the id of the state event that declared it, and
    /// that id comes from the ENVELOPE — `parse_session_membership` sees
    /// content alone. Without it a raised hand can never be matched to a
    /// participant, because the reaction addresses the event, not the user.
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
        // ...and it survives the round trip to the C++ side as an empty
        // string rather than being dropped from the payload, so the reader
        // sees "unknown" rather than a missing key.
        let wire = member.to_json();
        assert_eq!(wire.get("event_id").and_then(|v| v.as_str()), Some(""));
    }
}
