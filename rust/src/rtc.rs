
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

pub(crate) const EV_MEMBER_LEGACY: &str = "org.matrix.msc3401.call.member";
#[allow(dead_code)]
pub(crate) const EV_MEMBER_STICKY: &str = "org.matrix.msc4143.rtc.member";
pub(crate) const EV_SLOT: &str = "org.matrix.msc4143.rtc.slot";
#[allow(dead_code)]
pub(crate) const EV_NOTIFICATION_UNSTABLE: &str =
    "org.matrix.msc4075.rtc.notification";

const APPLICATION_CALL: &str = "m.call";

const SLOT_ID_ROOM: &str = "ROOM";

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
const MAX_CREATED_TS_SKEW_MS: u64 = 5 * 60 * 1000;
const MAX_BACKDATE_MS: u64 = 24 * 60 * 60 * 1000;

const MAX_NOTIFICATION_LIFETIME_MS: u64 = 90_000;

const MAX_WIRE_LEN: usize = 255;
const HAND_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_HAND_PROBES: usize = 24;
const MAX_URL_LEN: usize = 1024;
const MAX_MEMBERS: usize = 128;
const MAX_RAW_MEMBER_EVENTS: usize = 512;
const MEMBERSHIP_FETCH_TIMEOUT: Duration = Duration::from_secs(10);
const SESSION_SIGNAL_HORIZON_MS: u64 = 15 * 60 * 1000;
const SERVER_ESCALATION_COOLDOWN_MS: u64 = 15_000;
const SERVER_ESCALATION_COOLDOWN_MAX_MS: u64 = 300_000;
const RING_ESCALATION_WINDOW_MS: u64 = 3 * 60 * 1000;
const MAX_ROOM_MARKS: usize = 256;
const MAX_TRANSPORTS: usize = 8;
#[allow(dead_code)]
const MAX_VERSIONS: usize = 8;

const DISCOVERY_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_DISCOVERY_BODY: usize = 64 * 1024;

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

fn sane_https_url(value: &str) -> Option<String> {
    let trimmed = sane(value, MAX_URL_LEN)?;
    // `Url::parse` drops surrounding whitespace, and we advertise this string
    // as written, so refuse it rather than publish a spelling nobody matches.
    if trimmed.trim() != trimmed {
        return None;
    }
    let parsed = url::Url::parse(trimmed).ok()?;
    if parsed.scheme() != "https" {
        return None;
    }
    if !parsed.username().is_empty() || parsed.password().is_some() {
        return None;
    }
    // This is an endpoint base. Appending /get_token or /sfu/get must
    // extend the path, never a query or fragment (including empty ?/#).
    if parsed.query().is_some() || parsed.fragment().is_some() {
        return None;
    }
    let host = parsed.host()?;
    // Foci come from remote participants, so refuse the obvious SSRF shapes
    // here; a DNS-resolution check happens at connection time. Uses the same
    // `public_ip` policy as link previews (which unmaps `::ffff:a.b.c.d` and
    // refuses CGNAT, multicast and the rest).
    match host {
        url::Host::Ipv4(addr) => {
            #[cfg(feature = "test-loopback-foci")]
            if !public_ip(std::net::IpAddr::V4(addr)) && !addr.is_loopback() {
                return None;
            }
            #[cfg(not(feature = "test-loopback-foci"))]
            if !public_ip(std::net::IpAddr::V4(addr)) {
                return None;
            }
        }
        url::Host::Ipv6(addr) => {
            #[cfg(feature = "test-loopback-foci")]
            if !public_ip(std::net::IpAddr::V6(addr)) && !addr.is_loopback() {
                return None;
            }
            #[cfg(not(feature = "test-loopback-foci"))]
            if !public_ip(std::net::IpAddr::V6(addr)) {
                return None;
            }
        }
        url::Host::Domain(name) => {
            #[cfg(feature = "test-loopback-foci")]
            if !public_hostname(name) && !loopback_hostname(name) {
                return None;
            }
            #[cfg(not(feature = "test-loopback-foci"))]
            if !public_hostname(name) {
                return None;
            }
        }
    }
    // Keep the validated spelling: Element Call keys transports by this
    // literal string, including whether a bare origin has a trailing slash.
    Some(trimmed.to_owned())
}

fn same_focus_url(left: &str, right: &str) -> bool {
    match (url::Url::parse(left), url::Url::parse(right)) {
        (Ok(left), Ok(right)) => left == right,
        _ => false,
    }
}

pub(crate) fn public_hostname(name: &str) -> bool {
    let lower = name.trim_end_matches('.').to_ascii_lowercase();
    !(lower == "localhost"
        || lower.ends_with(".localhost")
        || lower.ends_with(".local")
        || lower.ends_with(".internal"))
}

#[cfg(feature = "test-loopback-foci")]
pub(crate) fn loopback_hostname(name: &str) -> bool {
    let lower = name.trim_end_matches('.').to_ascii_lowercase();
    lower == "localhost" || lower.ends_with(".localhost")
}

#[cfg(feature = "test-loopback-foci")]
pub(crate) fn test_focus_ip(ip: std::net::IpAddr) -> bool {
    public_ip(ip) || ip.is_loopback()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum HostRefusal {
    PrivateName,
    Unresolved,
    /// At least one resolved address is not public. The whole name is refused
    /// (see docs/matrixrtc.md): a public A record beside a loopback AAAA is the
    /// rebinding shape this guards against.
    NonPublicAddress,
}

pub(crate) async fn resolve_public_hosts(
    host: &str,
    port: u16,
) -> Result<Vec<std::net::SocketAddr>, HostRefusal> {
    #[cfg(feature = "test-loopback-foci")]
    if !public_hostname(host) && !loopback_hostname(host) {
        return Err(HostRefusal::PrivateName);
    }
    #[cfg(not(feature = "test-loopback-foci"))]
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
    #[cfg(feature = "test-loopback-foci")]
    if !addresses.iter().all(|address| {
        if loopback_hostname(host) { address.ip().is_loopback() }
        else { test_focus_ip(address.ip()) }
    }) {
        return Err(HostRefusal::NonPublicAddress);
    }
    #[cfg(not(feature = "test-loopback-foci"))]
    if !addresses.iter().all(|address| public_ip(address.ip())) {
        return Err(HostRefusal::NonPublicAddress);
    }
    Ok(addresses)
}

pub(crate) async fn resolve_public_host(
    host: &str,
    port: u16,
) -> Option<std::net::SocketAddr> {
    resolve_public_hosts(host, port)
        .await
        .ok()
        .and_then(|addresses| addresses.into_iter().next())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct LivekitTransport {
    pub service_url: String,
    pub alias: Option<String>,
}

/// Legacy members publish on the oldest member's focus. Compatibility
/// members publish on their advertised focus. Unknown values are not treated
/// as multi-SFU, since that could route a subscriber to a wrong service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FocusSelection {
    OldestMembership,
    MultiSfu,
    Unknown,
}

impl FocusSelection {
    fn parse(value: Option<&str>) -> Self {
        match value {
            Some("oldest_membership") => Self::OldestMembership,
            Some("multi_sfu") => Self::MultiSfu,
            _ => Self::Unknown,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::OldestMembership => "oldest_membership",
            Self::MultiSfu => "multi_sfu",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct FocusPlan {
    pub publisher: Option<LivekitTransport>,
    pub subscribers: Vec<LivekitTransport>,
    pub mode: FocusSelection,
}

pub(crate) fn plan_foci(
    members: &[RtcMember],
    own: &str,
    own_preferred: Option<LivekitTransport>,
) -> FocusPlan {
    let remote: Vec<&RtcMember> = members.iter()
        .filter(|member| member.rtc_identity != own)
        .collect();
    let legacy_peer = remote.iter().any(|member| {
        member.focus_selection != FocusSelection::MultiSfu
    });
    let oldest = select_focus(members);
    let mode = if legacy_peer || (own_preferred.is_none() && oldest.is_some()) {
        FocusSelection::OldestMembership
    } else {
        FocusSelection::MultiSfu
    };
    let publisher = if mode == FocusSelection::MultiSfu {
        own_preferred.or_else(|| oldest.clone())
    } else {
        oldest.clone().or(own_preferred)
    };
    let mut subscribers = Vec::new();
    for member in remote {
        let focus = focus_for_member(member, oldest.as_ref());
        if let Some(focus) = focus {
            let same_transport = |other: &LivekitTransport| {
                same_focus_url(&other.service_url, &focus.service_url)
                    && other.alias == focus.alias
            };
            let distinct_with_publisher = subscribers.len()
                + usize::from(!publisher.as_ref().is_some_and(same_transport)
                    && !publisher.as_ref().is_some_and(|publisher| {
                        subscribers.iter().any(|existing: &LivekitTransport| {
                            same_focus_url(&existing.service_url, &publisher.service_url)
                                && existing.alias == publisher.alias
                        })
                    }));
            if distinct_with_publisher < MAX_TRANSPORTS
                && !subscribers.iter().any(same_transport) {
                subscribers.push(focus);
            }
        }
    }
    FocusPlan { publisher, subscribers, mode }
}

pub(crate) fn focus_for_member(
    member: &RtcMember,
    oldest: Option<&LivekitTransport>,
) -> Option<LivekitTransport> {
    match member.focus_selection {
        FocusSelection::MultiSfu => member.foci.first().cloned(),
        FocusSelection::OldestMembership => oldest.cloned(),
        FocusSelection::Unknown => None,
    }
}

impl LivekitTransport {
    fn to_json(&self) -> serde_json::Value {
        let mut value = json!({
            "type": "livekit",
            // Element keys its connection pool by this exact wire string.
            // Preserve both an explicitly supplied slash and its absence;
            // URL normalization belongs only in local comparisons.
            "livekit_service_url": self.service_url,
        });
        if let Some(alias) = &self.alias {
            value["livekit_alias"] = json!(alias);
        }
        value
    }
}

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
            if !out.iter().any(|existing: &LivekitTransport| {
                same_focus_url(&existing.service_url, &transport.service_url)
                    && existing.alias == transport.alias
            }) {
                out.push(transport);
            }
        }
    }
    out
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum MembershipKind {
    Session,
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct RtcMember {
    pub user_id: String,
    pub device_id: String,
    pub rtc_identity: String,
    /// The new LiveKit identity for a legacy session membership. The hash
    /// includes the envelope sender, so a copied membershipID cannot claim
    /// another sender's SFU identity.
    pub hashed_rtc_identity: Option<String>,
    pub slot_id: String,
    pub intent: &'static str,
    pub created_ts: u64,
    /// Absolute expiry in ms. Past this the membership is stale and dropped;
    /// the only defence against a client that died without cleaning up.
    pub expires_at_ms: u64,
    pub foci: Vec<LivekitTransport>,
    pub focus_selection: FocusSelection,
    pub kind: MembershipKind,
    pub display_name: String,
    pub avatar_mxc: String,
    pub event_id: String,
}

impl RtcMember {
    fn to_json(&self) -> serde_json::Value {
        json!({
            "user_id": self.user_id,
            "display_name": self.display_name,
            "avatar_mxc": self.avatar_mxc,
            "device_id": self.device_id,
            "rtc_identity": self.rtc_identity,
            "hashed_rtc_identity": self.hashed_rtc_identity,
            "slot_id": self.slot_id,
            "intent": self.intent,
            "created_ts": self.created_ts,
            "expires_at_ms": self.expires_at_ms,
            "event_id": self.event_id,
            "kind": self.kind.as_str(),
            "foci": self.foci.iter().map(LivekitTransport::to_json)
                .collect::<Vec<_>>(),
            "focus_selection": self.focus_selection.as_str(),
        })
    }
}

fn intent_str(value: Option<&serde_json::Value>) -> &'static str {
    match value.and_then(|value| value.as_str()) {
        Some("video") => "video",
        Some("audio") => "audio",
        _ => "audio",
    }
}

fn slot_id_for_call_id(application: &str, call_id: &str) -> String {
    let id = if call_id.is_empty() { SLOT_ID_ROOM } else { call_id };
    format!("{application}#{id}")
}

#[allow(dead_code)] // sticky lane only; see EV_MEMBER_STICKY.
pub(crate) fn rtc_identity_hash(user_id: &str, member_id: &str) -> String {
    let canonical = serde_json::to_string(&[user_id, member_id])
        .unwrap_or_default();
    let digest = Sha256::digest(canonical.as_bytes());
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD_NO_PAD.encode(digest)
}

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
    if let Some(scope) = object.get("scope") {
        if scope.as_str() != Some("m.room") {
            return None;
        }
    }
    let device_id = sane_string(object.get("device_id"), MAX_WIRE_LEN)?;
    let call_id = object.get("call_id")?.as_str()?;
    if call_id.len() > MAX_WIRE_LEN || call_id.chars().any(|c| c.is_control()) {
        return None;
    }
    let active = object.get("focus_active")?.as_object()?;
    if active.get("type")?.as_str()? != "livekit" {
        return None;
    }
    let focus_selection = FocusSelection::parse(
        active.get("focus_selection").and_then(|value| value.as_str()));

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

    let rtc_identity = format!("{user_id}:{device_id}");
    let member_id = object.get("membershipID")
        .and_then(|value| sane_string(Some(value), MAX_WIRE_LEN))
        .unwrap_or_else(|| rtc_identity.clone());
    let hashed_rtc_identity = Some(rtc_identity_hash(&user_id, &member_id));

    Some(RtcMember {
        user_id,
        device_id,
        rtc_identity,
        hashed_rtc_identity,
        slot_id: slot_id_for_call_id(&application, call_id),
        intent: intent_str(object.get("m.call.intent")),
        created_ts,
        expires_at_ms,
        foci: parse_transport_list(object.get("foci_preferred")),
        focus_selection,
        kind: MembershipKind::Session,
        display_name: String::new(),
        avatar_mxc: String::new(),
        event_id: String::new(),
    })
}

#[allow(dead_code)]
pub(crate) fn parse_rtc_membership(
    content: &serde_json::Value,
    sender: &str,
    event_ts: u64,
) -> Option<RtcMember> {
    let object = content.as_object()?;

    let application = object.get("application")?.as_object()?;
    let app_type = sane_string(application.get("type"), MAX_WIRE_LEN)?;
    if app_type.contains('#') {
        return None;
    }

    let slot_id = sane_string(object.get("slot_id"), MAX_WIRE_LEN)?;
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

    let transports = object.get("transports")?.as_object()?;
    transports.get("published")?.as_array()?;
    let foci = parse_transport_list(transports.get("published"));

    let versions = object.get("versions")?.as_array()?;
    if versions.len() > MAX_VERSIONS
        || !versions.iter().all(|value| value.is_string())
    {
        return None;
    }

    let sticky = object
        .get("sticky_key")
        .or_else(|| object.get("msc4354_sticky_key"))
        .and_then(|value| value.as_str())?;
    if sane(sticky, MAX_WIRE_LEN).is_none() {
        return None;
    }

    Some(RtcMember {
        rtc_identity: rtc_identity_hash(&user_id, &member_id),
        hashed_rtc_identity: None,
        user_id,
        device_id,
        slot_id,
        intent: intent_str(application.get("m.call.intent")),
        created_ts: event_ts,
        // Sticky memberships are retracted, never aged out (the reference gives no
        // expiry); a 4 h fallback would drop live participants.
        expires_at_ms: u64::MAX,
        foci,
        focus_selection: FocusSelection::MultiSfu,
        kind: MembershipKind::Rtc,
        display_name: String::new(),
        avatar_mxc: String::new(),
        event_id: String::new(),
    })
}

#[derive(Clone, Debug, Default)]
pub(crate) struct RtcSession {
    pub members: Vec<RtcMember>,
    pub slot_closed: bool,
    pub slot_present: bool,
    pub source: &'static str,
    pub raw_count: usize,
}

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
    out.sort_by(|a, b| {
        a.created_ts
            .cmp(&b.created_ts)
            .then_with(|| a.user_id.cmp(&b.user_id))
            .then_with(|| a.device_id.cmp(&b.device_id))
    });
    out.truncate(MAX_MEMBERS);
    out
}

fn room_call_members(members: Vec<RtcMember>, now_ms: u64) -> Vec<RtcMember> {
    aggregate_session(members.into_iter()
        .filter(|member| member.slot_id == "m.call#ROOM")
        .collect(), now_ms)
}

/// A missing member record is unknown under lazy-loaded sliding sync. Only a
/// recorded non-join state proves that the advertiser has left the room.
fn sender_membership_allows_call(
    membership: Option<&matrix_sdk::ruma::events::room::member::MembershipState>,
) -> bool {
    !membership.is_some_and(|state| {
        state != &matrix_sdk::ruma::events::room::member::MembershipState::Join
    })
}

pub(crate) fn select_focus(members: &[RtcMember]) -> Option<LivekitTransport> {
    // The oldest membership's own first focus, and nothing else: the reference
    // reads `getOldestMembership()`'s `foci_preferred[0]` and yields nothing if
    // it has none. Walking on would diverge from Element.
    members.first().and_then(|member| member.foci.first().cloned())
}

mod transports_endpoint {
    use matrix_sdk::Client;

    pub(super) struct Answer {
        pub status: u16,
        pub transports: Vec<serde_json::Value>,
    }

    fn endpoints(client: &Client) -> Result<(String, Option<String>), String> {
        let server_name = client
            .user_id()
            .ok_or_else(|| "no session".to_owned())?
            .server_name()
            .as_str()
            .to_owned();
        Ok((endpoint_for(&server_name)?, fallback_for(&server_name)?))
    }

    pub(super) fn endpoint_for(server_name: &str) -> Result<String, String> {
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
        let parsed = url::Url::parse(&url).map_err(|_| "bad url".to_owned())?;
        let authority = match (parsed.host_str(), parsed.port()) {
            (Some(host), Some(port)) => format!("{host}:{port}"),
            (Some(host), None) => host.to_owned(),
            (None, _) => return Err("unusable server name".to_owned()),
        };
        let explicit_default = format!("{authority}:443") == server_name;
        if parsed.scheme() != "https" || (authority != server_name && !explicit_default) {
            return Err("unusable server name".to_owned());
        }
        Ok(parsed.to_string())
    }

    pub(super) fn fallback_for(server_name: &str) -> Result<Option<String>, String> {
        let primary = endpoint_for(server_name)?;
        let parsed = url::Url::parse(&primary).map_err(|_| "bad url".to_owned())?;
        let Some(port) = parsed.port() else {
            return Ok(None);
        };
        let host = server_name
            .strip_suffix(&format!(":{port}"))
            .ok_or_else(|| "unusable server name".to_owned())?;
        if parsed.host_str() != Some(host) {
            return Err("unusable server name".to_owned());
        }
        let fallback = endpoint_for(host)?;
        let fallback_url = url::Url::parse(&fallback).map_err(|_| "bad url".to_owned())?;
        if fallback_url.host_str() != Some(host) || fallback_url.port().is_some() {
            return Err("unusable server name".to_owned());
        }
        Ok(Some(fallback))
    }

    pub(super) async fn get(
        client: &Client,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let (url, fallback) = endpoints(client)?;
        let first = get_once(client, url, timeout).await;
        if let Some(fallback) = fallback {
            if !matches!(&first, Ok(answer)
                if (200..300).contains(&answer.status) && !answer.transports.is_empty())
            {
                return get_once(client, fallback, timeout).await;
            }
        }
        first
    }

    async fn get_once(
        client: &Client,
        url: String,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
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
        if response
            .content_length()
            .is_some_and(|len| len > super::MAX_DISCOVERY_BODY as u64)
        {
            return Ok(Answer { status, transports: Vec::new() });
        }
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

/// `org.matrix.msc4075.rtc.notification`, the ring. Defined here because
/// ruma 0.34 types only the stable `m.rtc.notification`, which is deaf to
/// Element's rings. Both are observed (see [`register_rtc_handlers`]); this
/// one is sent, matching Element.
#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4075.rtc.notification", kind = MessageLike)]
pub(crate) struct Msc4075RtcNotificationEventContent {
    pub notification_type: String,
    pub sender_ts: MilliSecondsSinceUnixEpoch,
    pub lifetime: u64,
    #[serde(rename = "m.mentions", default, skip_serializing_if = "Option::is_none")]
    pub mentions: Option<Mentions>,
    #[serde(rename = "m.relates_to", default, skip_serializing_if = "Option::is_none")]
    pub relates_to: Option<Reference>,
    #[serde(rename = "m.call.intent", default, skip_serializing_if = "Option::is_none")]
    pub call_intent: Option<String>,
}

fn msc4075_notification_payload(
    ev: &OriginalSyncMsc4075RtcNotificationEvent,
    room_id: &str,
    own_user: Option<&matrix_sdk::ruma::UserId>,
    lifecycle: u64,
) -> serde_json::Value {
    let own = own_user.is_some_and(|user| user == ev.sender);
    let intent = match ev.content.call_intent.as_deref() {
        Some("video") => "video",
        _ => "audio",
    };
    let mut payload = json!({
        "type": "call_rtc_notification",
        "lifecycle": lifecycle,
        "room_id": room_id,
        "event_id": ev.event_id.to_string(),
        "sender": ev.sender.to_string(),
        "own": own,
        "origin_server_ts": u64::from(ev.origin_server_ts.get()),
        "sender_ts": u64::from(ev.content.sender_ts.get()),
        "lifetime_ms": ev.content.lifetime.min(MAX_NOTIFICATION_LIFETIME_MS),
        "call_intent": intent,
        // MatrixRTC lane, not legacy m.call.invite, so one call cannot ring twice.
        "rtc": true,
    });
    if let Some(object) = payload.as_object_mut() {
        object.extend(crate::calls::rtc_notification_addressing(
            &ev.content.notification_type,
            ev.content.mentions.as_ref(),
            own_user,
        ));
    }
    payload
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(
    type = "org.matrix.msc3401.call.member",
    kind = State,
    state_key_type = String
)]
pub(crate) struct LegacyRtcMemberEventContent {}

fn raw_state_json(raw: &RawAnySyncOrStrippedState) -> Option<serde_json::Value> {
    let json = match raw {
        RawAnySyncOrStrippedState::Sync(raw) => raw.json(),
        RawAnySyncOrStrippedState::Stripped(raw) => raw.json(),
    };
    serde_json::from_str(json.get()).ok()
}

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

pub(crate) fn store_view_is_usable(
    events: &[serde_json::Value],
    now_ms: u64,
) -> bool {
    events
        .iter()
        .any(|value| membership_event_is_live(value, now_ms))
}

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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EscalationReason {
    /// We hold a published membership in this room, so we are in its call. The
    /// store can hold only stale retractions while the server has the live
    /// memberships, and media keys go to the devices these name, so an empty
    /// read would send the key to nobody.
    OwnCall,
    Ring,
    RecentActivity,
}

impl EscalationReason {
    fn cooling_word(self) -> &'static str {
        match self {
            EscalationReason::OwnCall => "store-cooling-own",
            EscalationReason::Ring => "store-cooling-ring",
            EscalationReason::RecentActivity => "store-cooling-recent",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum StoreVerdict {
    Answer,
    AnswerNoSession,
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
        _ => StoreVerdict::AnswerNoSession,
    }
}

fn escalation_cooldown_ms(consecutive_asks: u32) -> u64 {
    let mut ms = SERVER_ESCALATION_COOLDOWN_MS;
    for _ in 1..consecutive_asks {
        if ms >= SERVER_ESCALATION_COOLDOWN_MAX_MS {
            break;
        }
        ms = ms.saturating_mul(2);
    }
    ms.min(SERVER_ESCALATION_COOLDOWN_MAX_MS)
}

/// The key of an escalation or ring mark: the account AND the room, so one
/// account's session ending never clears another live account's marks
/// (design 2026-10-15 §3.2.10, R1-H1). U+001F cannot occur in either id.
fn account_room_mark_key(user_id: &str, room_id: &str) -> String {
    format!("{user_id}\u{1f}{room_id}")
}

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

pub(crate) fn note_rtc_ring(user_id: &str, room_id: &str) {
    let key = account_room_mark_key(user_id, room_id);
    if let Ok(mut marks) = rtc_ring_marks().lock() {
        if marks.len() >= MAX_ROOM_MARKS && !marks.contains_key(&key) {
            marks.clear();
        }
        marks.insert(key, std::time::Instant::now());
    }
    clear_server_escalation_backoff(user_id, room_id);
}

fn rtc_ring_is_recent(user_id: &str, room_id: &str) -> bool {
    rtc_ring_marks()
        .lock()
        .ok()
        .and_then(|marks| marks.get(&account_room_mark_key(user_id, room_id)).copied())
        .is_some_and(|at| {
            let since_ms = at.elapsed().as_millis() as u64;
            since_ms <= RING_ESCALATION_WINDOW_MS
        })
}

fn claim_server_escalation(user_id: &str, room_id: &str) -> bool {
    let room_id = account_room_mark_key(user_id, room_id);
    let room_id = room_id.as_str();
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

fn note_server_escalation_found_live(user_id: &str, room_id: &str) {
    if let Ok(mut marks) = server_escalation_marks().lock() {
        if let Some(entry) = marks.get_mut(&account_room_mark_key(user_id, room_id)) {
            entry.1 = 0;
        }
    }
}

fn clear_server_escalation_backoff(user_id: &str, room_id: &str) {
    if let Ok(mut marks) = server_escalation_marks().lock() {
        marks.remove(&account_room_mark_key(user_id, room_id));
    }
}

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

async fn read_membership_events(
    room: &Room,
    now_ms: u64,
    prefer_server: bool,
) -> (Vec<serde_json::Value>, &'static str) {
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
        let user_id = room.own_user_id().as_str();
        match store_read_verdict(
            &from_store,
            now_ms,
            room_has_published_membership(room_id),
            rtc_ring_is_recent(user_id, room_id),
        ) {
            StoreVerdict::Answer => return (from_store, "store"),
            StoreVerdict::AnswerNoSession => {
                return (from_store, "store-no-session")
            }
            StoreVerdict::AskServer(reason) => {
                if !claim_server_escalation(user_id, room_id) {
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
        return (from_store, "server-none");
    }
    let merged = merge_membership_events(from_store, from_server, now_ms);
    if !prefer_server && store_view_is_usable(&merged, now_ms) {
        note_server_escalation_found_live(
            room.own_user_id().as_str(),
            room.room_id().as_str(),
        );
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
            let Ok(user_id) = matrix_sdk::ruma::UserId::parse(&member.user_id)
            else { continue };
            // A state event can outlive the sender's room membership (Sable
            // likewise requires the advertiser to remain joined). Drop only a
            // POSITIVE record of not being joined: under sliding sync member
            // events are lazy-loaded, so a missing record is "unknown", and
            // treating it as "left" would drop a real participant from the
            // whole session (keys, facepile, audio), not just from focus
            // selection.
            let profile = room.get_member_no_sync(&user_id).await.ok().flatten();
            if !sender_membership_allows_call(
                profile.as_ref().map(|profile| profile.membership()),
            ) {
                continue;
            }
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

    let mut members = room_call_members(members, now_ms);
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

fn status_category(status: u16) -> &'static str {
    match status {
        400 | 404 | 405 => "unsupported",
        401 | 403 => "forbidden",
        429 => "rate_limited",
        500..=599 => "server_error",
        _ => "unknown",
    }
}

fn discovery_answer_is_definitive(category: &str) -> bool {
    category.is_empty() || category == "unsupported"
}

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
        "source": session.source,
        "raw_count": session.raw_count,
        "focus": focus.as_ref().map(LivekitTransport::to_json),
        "members": session.members.iter().map(RtcMember::to_json)
            .collect::<Vec<_>>(),
    })
}

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

pub(crate) fn request_transports(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
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
            Ok(answer) => Err(status_category(answer.status).to_owned()),
            Err(err) => Err(classify_room_error(&err).to_owned()),
        };

        let mut participant_focus = None;
        let mut session_members = Vec::new();
        if let Some(room) = room.as_ref() {
            let session = read_session(room, now_ms(), false).await;
            participant_focus = select_focus(&session.members);
            session_members = session.members;
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        let (server_transports, category) = match advertised {
            Ok(list) => (list, String::new()),
            Err(category) => (Vec::new(), category),
        };
        let own_identity = client.user_id().zip(client.device_id())
            .map(|(user, device)| format!("{user}:{device}"))
            .unwrap_or_default();
        let plan = plan_foci(&session_members, &own_identity,
                             server_transports.first().cloned());

        enqueue(&events, json!({
            "type": "rtc_transports",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "server_answered": discovery_answer_is_definitive(&category),
            "category": category,
            "server_transports": server_transports.iter()
                .map(LivekitTransport::to_json).collect::<Vec<_>>(),
            "participant_focus": participant_focus.as_ref()
                .map(LivekitTransport::to_json),
            "focus_plan": {
                "mode": plan.mode.as_str(),
                "publisher": plan.publisher.as_ref().map(LivekitTransport::to_json),
                "subscribers": plan.subscribers.iter()
                    .map(LivekitTransport::to_json).collect::<Vec<_>>(),
            },
        }));
    });
    Ok(())
}

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

/// Membership validity when the server can retract it for us (an MSC4140
/// delayed retraction is armed). 4 h is the reference's
/// `DEFAULT_EXPIRE_DURATION`; the delayed retraction does the real cleanup.
/// Mirrored as `kMembershipExpiryLongMs` in SfuCallController.cpp.
const MEMBERSHIP_EXPIRY_MS: u64 = 4 * 60 * 60 * 1000;

/// Membership validity when nothing server-side will retract it (no
/// MSC4140; Synapse has it off by default). `expires` is then the only
/// cleanup, so it must run out in minutes, with the client re-publishing
/// often enough that a live participant never ages out. 5 minutes against
/// `SfuCallController`'s 60 s re-publish survives five failed refreshes;
/// shortening it without that cadence would drop people mid-call. Mirrored
/// as `kMembershipExpiryNoDelayedMs` in SfuCallController.cpp, which keeps a
/// retraction that failed for network reasons owed until this has run out.
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
        None => period_ms,
        Some(created) => now_ms
            .saturating_sub(created)
            .saturating_add(period_ms),
    }
}

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

fn delayed_refusal_recorded(server: &str) -> bool {
    DELAYED_EVENTS_REFUSED
        .lock()
        .map(|seen| seen.contains(server))
        .unwrap_or(false)
}

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

/// A bridge's membership writes share one lock. Operation tombstones survive
/// a successful leave, so a queued old publish cannot recreate that call and
/// a queued old leave cannot retract a newer join. Op ids come from C++'s
/// monotonically increasing nextOpId, scoped by room + account/device key.
#[derive(Default)]
pub(crate) struct MembershipDelayRegistry {
    delays: BTreeMap<String, String>,
    operations: BTreeMap<String, u64>,
}

impl MembershipDelayRegistry {
    fn accept_operation(&mut self, key: &str, operation: u64) -> bool {
        let latest = self.operations.entry(key.to_owned()).or_default();
        if operation < *latest { return false; }
        *latest = operation;
        true
    }
}

fn membership_publish_key(room_id: &str, state_key: &str) -> String {
    format!("{room_id}\u{1f}{state_key}")
}

fn mark_membership_published(user_id: &str, key: &str) {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.insert(key.to_owned());
    }
    if let Some(room_id) = key.split('\u{1f}').next() {
        clear_server_escalation_backoff(user_id, room_id);
    }
}

fn forget_membership_published(key: &str) {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.remove(key);
    }
}

fn forget_room_memberships_published(room_id: &str, account: Option<(&str, &str)>) {
    let prefix = format!("{room_id}\u{1f}");
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.retain(|key| {
            key.strip_prefix(&prefix).is_none_or(|state_key| {
                account.is_some_and(|(user, device)| {
                    !state_key_belongs_to(state_key, user, device)
                })
            })
        });
    }
}

fn state_key_belongs_to(state_key: &str, user_id: &str, device_id: &str) -> bool {
    let bare = format!("{user_id}_{device_id}_{APPLICATION_CALL}");
    state_key == bare || state_key.strip_prefix('_') == Some(bare.as_str())
}

/// Forget what ONE account (user, device) published, and that account's
/// escalation and ring marks, leaving every other account's alone (design
/// §3.2.10, DECISION 11: also on the flag-off path). A session ending runs
/// this for its own identity; the wholesale clear it replaces cleared every
/// account, which in an A -> B overlap or with concurrent live accounts
/// cleared a session that was still running.
pub(crate) fn forget_account_memberships_published(user_id: &str, device_id: &str) {
    if let Ok(mut seen) = OWN_MEMBERSHIP_PUBLISHED.lock() {
        seen.retain(|key| {
            key.split_once('\u{1f}')
                .is_none_or(|(_, state_key)| !state_key_belongs_to(state_key, user_id, device_id))
        });
    }
    let prefix = format!("{user_id}\u{1f}");
    if let Ok(mut marks) = server_escalation_marks().lock() {
        marks.retain(|key, _| !key.starts_with(&prefix));
    }
    if let Ok(mut marks) = rtc_ring_marks().lock() {
        marks.retain(|key, _| !key.starts_with(&prefix));
    }
}

fn membership_published_in_this_process(key: &str) -> bool {
    OWN_MEMBERSHIP_PUBLISHED
        .lock()
        .map(|seen| seen.contains(key))
        .unwrap_or(false)
}

fn room_has_published_membership(room_id: &str) -> bool {
    let prefix = format!("{room_id}\u{1f}");
    OWN_MEMBERSHIP_PUBLISHED
        .lock()
        .map(|seen| seen.iter().any(|key| key.starts_with(&prefix)))
        .unwrap_or(false)
}

pub(crate) fn delayed_refusal_is_permanent(category: &str) -> bool {
    matches!(
        category,
        "unrecognized" | "not_found" | "no_delay_id" | "delayed_unsupported"
    )
}

const MSC4140_FEATURE: &str = "org.matrix.msc4140";

async fn server_advertises_delayed_events(client: &Client) -> bool {
    use matrix_sdk::ruma::api::FeatureFlag;
    match tokio::time::timeout(DISCOVERY_TIMEOUT, client.unstable_features())
        .await
    {
        Ok(Ok(features)) => features.contains(&FeatureFlag::from(MSC4140_FEATURE)),
        _ => true,
    }
}

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
    let key = format!("{user_id}_{device_id}_{APPLICATION_CALL}");
    if room_version.starts_with("org.matrix.msc3757")
        || room_version.starts_with("org.matrix.msc3779")
    {
        key
    } else {
        format!("_{key}")
    }
}

fn own_membership_content_with_mode(
    device_id: &str,
    user_id: &str,
    focus: Option<&LivekitTransport>,
    selection: FocusSelection,
    intent: &str,
    created_ts: Option<u64>,
    expires_ms: u64,
) -> serde_json::Value {
    let mut content = json!({
        "application": APPLICATION_CALL,
        "call_id": "",
        "scope": "m.room",
        "device_id": device_id,
        "membershipID": format!("{user_id}:{device_id}"),
        // Not a constant: depends on whether anything else will retract this
        // membership (see MEMBERSHIP_EXPIRY_NO_DELAYED_MS).
        "expires": expires_ms,
        "m.call.intent": intent,
        "focus_active": {
            "type": "livekit",
            "focus_selection": selection.as_str(),
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

#[cfg(test)]
fn own_membership_content(
    device_id: &str, user_id: &str, focus: Option<&LivekitTransport>,
    intent: &str, created_ts: Option<u64>, expires_ms: u64,
) -> serde_json::Value {
    own_membership_content_with_mode(device_id, user_id, focus,
        FocusSelection::OldestMembership, intent, created_ts, expires_ms)
}

pub(crate) fn publish_membership(
    bridge: &RustClient,
    room_id: String,
    focus_url: String,
    intent: String,
    multi_sfu: bool,
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

    let delays = Arc::clone(&bridge.rtc_membership_delays);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let mut registry = delays.lock().await;
        if !timelines.lifecycle_current(lifecycle) { return; }
        let selection = if multi_sfu {
            FocusSelection::MultiSfu
        } else {
            FocusSelection::OldestMembership
        };
        let room_version = room
            .clone_info()
            .room_version()
            .map(|v| v.to_string())
            .unwrap_or_default();
        let state_key =
            membership_state_key(&user_id, &device_id, &room_version);

        let publish_key =
            membership_publish_key(room.room_id().as_str(), &state_key);
        if !registry.accept_operation(&publish_key, op_id) {
            enqueue(&events, json!({
                "type": "rtc_membership_published", "op_id": op_id,
                "lifecycle": lifecycle, "room_id": room_id, "ok": false,
                "category": "cancelled", "event_id": "", "delay_id": "",
                "delayed_category": "",
            }));
            return;
        }
        let delays = &mut registry.delays;
        let previous_delay = delays.get(&publish_key).cloned().unwrap_or_default();
        let reusable_delay = match reuse_membership_delay(&previous_delay, |id| {
            restart_membership_delay(&client, id)
        }).await {
            Ok(id) => id,
            Err(category) => {
                enqueue(&events, json!({
                    "type": "rtc_membership_published", "op_id": op_id,
                    "lifecycle": lifecycle, "room_id": room_id, "ok": false,
                    "category": category, "event_id": "", "delay_id": previous_delay,
                    "delayed_category": "",
                }));
                return;
            }
        };
        if reusable_delay.is_empty() { delays.remove(&publish_key); }
        let is_refresh = membership_published_in_this_process(&publish_key);
        let created_ts = if is_refresh {
            read_own_created_ts(&room, &state_key).await
        } else {
            None
        };
        let focus = focus.map(|transport| LivekitTransport {
            service_url: transport.service_url.clone(),
            alias: transport
                .alias
                .clone()
                .or_else(|| Some(room.room_id().to_string())),
        });

        let assumed_no_delayed = reusable_delay.is_empty()
            && delayed_refusal_recorded_for_client(&client);
        let period_ms = if assumed_no_delayed {
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        } else {
            MEMBERSHIP_EXPIRY_MS
        };
        let expires_ms = expires_for_refresh(period_ms, created_ts, now_ms());
        let content = own_membership_content_with_mode(
            &device_id, &user_id, focus.as_ref(), selection, intent, created_ts,
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

        if ok {
            mark_membership_published(&user_id, &publish_key);
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
        if ok {
            match ensure_membership_delay(&reusable_delay,
                delayed_retraction_is_repairable(assumed_no_delayed),
                |id| restart_membership_delay(&client, id),
                || schedule_delayed_leave(&client, room.room_id().as_str(), &state_key)).await
            {
                Ok(id) => {
                    if !id.is_empty() {
                        mark_delayed_refusal_for_client(&client, false);
                    }
                    delays.insert(publish_key.clone(), id.clone());
                    delay_id = id;
                }
                Err(reason) if !reusable_delay.is_empty() => {
                    ok = false;
                    category = reason;
                    delay_id = reusable_delay.clone();
                }
                Err(category) => delayed_category = category,
            }
        }

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
                if unadvertised && !delayed_refusal_is_permanent(&delayed_category)
                {
                    delayed_category = "delayed_unsupported".to_owned();
                }
            }
            let short = own_membership_content_with_mode(
                &device_id, &user_id, focus.as_ref(), selection, intent, created_ts,
                fallback_expires_ms(created_ts, now_ms()));
            let retry = tokio::time::timeout(
                DISCOVERY_TIMEOUT,
                room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key, short),
            )
            .await;
            match retry {
                Ok(Ok(response)) => event_id = response.event_id.to_string(),
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
            "delay_id": delay_id,
            "delayed_category": delayed_category,
        }));
    });
    Ok(())
}

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

fn inheritable_created_ts(
    content: &serde_json::Value,
    origin_server_ts: Option<u64>,
    now_ms: u64,
) -> Option<u64> {
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
    if created.saturating_add(expires) <= now_ms {
        return None;
    }
    Some(created)
}

async fn reuse_membership_delay<F, Fut>(existing: &str, restart: F) -> Result<String, String>
where
    F: FnOnce(String) -> Fut,
    Fut: std::future::Future<Output = Result<(), String>>,
{
    if existing.is_empty() { return Ok(String::new()); }
    match restart(existing.to_owned()).await {
        Ok(()) => Ok(existing.to_owned()),
        Err(reason) if reason == "not_found" => Ok(String::new()),
        Err(reason) => Err(reason),
    }
}

async fn ensure_membership_delay<F, Fut, G, Gut>(
    existing: &str, allow_new_timer: bool, restart: F, schedule: G,
) -> Result<String, String>
where
    F: FnOnce(String) -> Fut,
    Fut: std::future::Future<Output = Result<(), String>>,
    G: FnOnce() -> Gut,
    Gut: std::future::Future<Output = Result<String, String>>,
{
    if !existing.is_empty() {
        restart(existing.to_owned()).await?;
        return Ok(existing.to_owned());
    }
    if allow_new_timer { schedule().await } else { Ok(String::new()) }
}

async fn restart_membership_delay(client: &Client, id: String) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event;
    let request = update_delayed_event::unstable::Request::new(
        id, update_delayed_event::unstable::UpdateAction::Restart);
    match tokio::time::timeout(DISCOVERY_TIMEOUT, client.send(request)).await {
        Ok(Ok(_)) => Ok(()),
        Ok(Err(err)) => Err(classify_room_error(&err.to_string()).to_owned()),
        Err(_) => Err("network".to_owned()),
    }
}

async fn cancel_membership_delay(client: &Client, id: &str) -> bool {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event;
    if id.is_empty() { return true; }
    let request = update_delayed_event::unstable::Request::new(
        id.to_owned(), update_delayed_event::unstable::UpdateAction::Cancel);
    match tokio::time::timeout(DISCOVERY_TIMEOUT, client.send(request)).await {
        Ok(Ok(_)) => true,
        Ok(Err(err)) => classify_room_error(&err.to_string()) == "not_found",
        Err(_) => false,
    }
}

pub(crate) fn delayed_retraction_is_repairable(assumed_no_delayed: bool) -> bool {
    !assumed_no_delayed
}

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
                Err("no_delay_id".to_owned())
            } else {
                Ok(response.delay_id)
            }
        }
        Ok(Err(err)) => Err(classify_delayed_leave_failure(&err)),
        Err(_) => Err("network".to_owned()),
    }
}

pub(crate) fn delayed_failure_says_unsupported(message: &str) -> bool {
    let lc = message.to_lowercase();
    lc.contains("m_max_delay_unsupported")
        || lc.contains("delayed events are not supported")
}

fn classify_delayed_leave_failure(err: &matrix_sdk::HttpError) -> String {
    use matrix_sdk::ruma::api::error::FromHttpResponseError;
    use matrix_sdk::HttpError;

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
    if let HttpError::Api(api) = err {
        if matches!(**api, FromHttpResponseError::Deserialization(_)) {
            return "no_delay_id".to_owned();
        }
    }
    "network".to_owned()
}

pub(crate) fn restart_delayed_leave(
    bridge: &RustClient,
    delay_id: String,
    op_id: u64,
) -> Result<(), String> {
    update_delayed(bridge, delay_id, "restart", op_id)
}

pub(crate) fn retract_membership(
    bridge: &RustClient,
    room_id: String,
    delay_id: String,
    op_id: u64,
) -> Result<(), String> {
    let account = if crate::labs::active() {
        bridge.client.lock().ok().and_then(|guard| {
            guard.as_ref().and_then(|client| {
                Some((client.user_id()?.to_string(), client.device_id()?.to_string()))
            })
        })
    } else {
        None
    };
    forget_room_memberships_published(
        &room_id,
        account.as_ref().map(|(user, device)| (user.as_str(), device.as_str())),
    );

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

    let delays = Arc::clone(&bridge.rtc_membership_delays);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();

    bridge.spawn_room_action(async move {
        let mut registry = delays.lock().await;
        let room_version = room
            .clone_info()
            .room_version()
            .map(|v| v.to_string())
            .unwrap_or_default();
        let state_key =
            membership_state_key(&user_id, &device_id, &room_version);

        let publish_key = membership_publish_key(room.room_id().as_str(), &state_key);
        if !registry.accept_operation(&publish_key, op_id) {
            enqueue(&events, json!({
                "type": "rtc_membership_retracted", "op_id": op_id,
                "lifecycle": lifecycle, "room_id": room_id, "ok": false,
                "category": "cancelled",
            }));
            return;
        }
        let delays = &mut registry.delays;

        forget_membership_published(&membership_publish_key(
            room.room_id().as_str(), &state_key));

        let result = tokio::time::timeout(
            DISCOVERY_TIMEOUT,
            room.send_state_event_raw(EV_MEMBER_LEGACY, &state_key, json!({})),
        )
        .await;

        let current_delay = delays.get(&publish_key).cloned().unwrap_or(delay_id);
        if !current_delay.is_empty() {
            delays.insert(publish_key.clone(), current_delay.clone());
        }
        if cancel_membership_delay(&client, &current_delay).await {
            delays.remove(&publish_key);
        }

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

/// The reaction key element-call uses for a raised hand: U+1F590 plus
/// U+FE0F. Taken from element-call (`useReactionsSender.tsx`,
/// `ReactionsReader.ts`), which compares exactly this string.
pub(crate) const HAND_RAISED_KEY: &str = "\u{1F590}\u{FE0F}";

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
                break;
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

#[allow(dead_code)]
pub(crate) const EV_CALL_REACTION: &str = "io.element.call.reaction";

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
    ("bell", "\u{1F6CE}\u{FE0F}"),
];

const REACTION_TIMEOUT: Duration = Duration::from_secs(10);

const MAX_REACTION_EMOJI_LEN: usize = 64;
const MAX_REACTION_CLUSTER_CHARS: usize = 8;

#[derive(Clone, Debug, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "io.element.call.reaction", kind = MessageLike)]
pub(crate) struct ElementCallReactionEventContent {
    pub emoji: String,
    #[serde(default)]
    pub name: String,
    #[serde(rename = "m.relates_to")]
    pub relates_to: Reference,
}

fn reaction_emoji(raw: &str) -> Option<String> {
    let bounded = sane(raw.trim(), MAX_REACTION_EMOJI_LEN)?;
    let cluster = first_emoji_cluster(bounded);
    if cluster.is_empty() {
        return None;
    }
    Some(cluster)
}

fn is_regional_indicator(value: char) -> bool {
    ('\u{1F1E6}'..='\u{1F1FF}').contains(&value)
}

fn first_emoji_cluster(value: &str) -> String {
    let mut out = String::new();
    let mut chars = value.chars().peekable();
    let Some(first) = chars.next() else {
        return out;
    };
    out.push(first);
    let mut after_join = false;
    while out.chars().count() < MAX_REACTION_CLUSTER_CHARS {
        let Some(&next) = chars.peek() else { break };
        let continues = after_join
            || matches!(next,
                '\u{200D}'
                | '\u{FE00}'..='\u{FE0F}'
                | '\u{1F3FB}'..='\u{1F3FF}'
                | '\u{20E3}'
                | '\u{0300}'..='\u{036F}'
                | '\u{E0020}'..='\u{E007F}'
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

fn reaction_wire_name(emoji: &str, name: &str) -> Option<String> {
    if name.is_empty() {
        let preset = ELEMENT_CALL_REACTIONS
            .iter()
            .find(|(_, known_emoji)| *known_emoji == emoji)
            .map(|(known_name, _)| *known_name)
            .unwrap_or("");
        return Some(preset.to_owned());
    }
    ELEMENT_CALL_REACTIONS
        .iter()
        .any(|(known_name, known_emoji)| *known_name == name && *known_emoji == emoji)
        .then(|| name.to_owned())
}

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
    if sane(&emoji, MAX_REACTION_EMOJI_LEN).is_none() || emoji.trim() != emoji {
        return Err("unknown reaction".to_owned());
    }
    let name = reaction_wire_name(&emoji, &name)
        .ok_or_else(|| "unknown reaction".to_owned())?;

    let content = ElementCallReactionEventContent {
        emoji,
        name,
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

pub(crate) const EV_CALL_KEYS: &str = "io.element.call.encryption_keys";
/// Media keys we send are 16 raw bytes, the length matrix-js-sdk,
/// element-call and sable-core mint; LiveKit's HKDF turns them into
/// AES-128-GCM. HKDF takes any length, but sable-core drops a key that is not
/// 16 bytes, so one of 32 left Sable unable to hear us.
const MEDIA_KEY_BYTES: usize = 16;

pub(crate) fn outbound_media_key_ok(key_base64: &str) -> bool {
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD
        .decode(key_base64)
        .is_ok_and(|raw| raw.len() == MEDIA_KEY_BYTES)
}
/// Highest key index our sender stamps (SfuCallController allocates
/// `(index + 1) % 16`).
///
/// Received keys are not held to this: matrix-js-sdk rotates modulo 256 and
/// element-call's ring holds 256, so the received index is a whole `u8`.
const MAX_SEND_KEY_INDEX: u8 = 15;

pub(crate) const DEVICE_REFRESH_TTL_SECS: u64 = 60;

pub(crate) fn device_refresh_due(elapsed_secs: Option<u64>) -> bool {
    match elapsed_secs {
        None => true,
        Some(secs) => secs >= DEVICE_REFRESH_TTL_SECS,
    }
}

fn device_refresh_marks()
    -> &'static std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

const DEVICE_MISS_BASE_SECS: u64 = 10;
const DEVICE_MISS_CEILING_SECS: u64 = 300;
const MAX_DEVICE_MISS_MARKS: usize = 4 * MAX_MEMBERS;

pub(crate) fn device_miss_refresh_due(elapsed_secs: Option<u64>, misses: u32) -> bool {
    let Some(secs) = elapsed_secs else {
        return true;
    };
    if misses == 0 {
        return true;
    }
    let shift = (misses - 1).min(16);
    let gap = DEVICE_MISS_BASE_SECS
        .saturating_mul(1u64 << shift)
        .min(DEVICE_MISS_CEILING_SECS);
    secs >= gap
}

fn device_miss_marks()
    -> &'static std::sync::Mutex<std::collections::HashMap<String, (std::time::Instant, u32)>>
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, (std::time::Instant, u32)>>,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

pub(crate) fn note_device_lookup(
    marks: &mut std::collections::HashMap<String, (std::time::Instant, u32)>,
    key: &str,
    found: bool,
    queried: bool,
    now: std::time::Instant,
) {
    if found {
        marks.remove(key);
        return;
    }
    if !queried {
        return;
    }
    if marks.len() >= MAX_DEVICE_MISS_MARKS && !marks.contains_key(key) {
        if let Some(oldest) = marks
            .iter()
            .min_by_key(|(_, (at, _))| *at)
            .map(|(k, _)| k.clone())
        {
            marks.remove(&oldest);
        }
    }
    let misses = marks.get(key).map_or(0, |(_, m)| *m);
    marks.insert(key.to_owned(), (now, misses.saturating_add(1)));
}

fn undecryptable_report_marks()
    -> &'static std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>
{
    static MARKS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, std::time::Instant>>,
    > = std::sync::OnceLock::new();
    MARKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

const MAX_REPORT_MARKS: usize = 256;

pub(crate) fn undecryptable_key_report_due(
    is_media_key_peer: bool,
    since_last_report_secs: Option<u64>,
) -> bool {
    is_media_key_peer && device_refresh_due(since_last_report_secs)
}

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
    if !outbound_media_key_ok(&key_base64) {
        return Err("media key must be 16 bytes".to_owned());
    }
    let rows = serde_json::from_str::<Vec<serde_json::Value>>(&targets_json)
        .map_err(|_| "invalid targets".to_owned())?;
    let (targets, dropped) = split_key_targets(&rows);
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
        // substituted: sending a key to the wrong device is worse. It is
        // reported as undelivered, so the caller does not count it a holder.
        let mut resolved = Vec::new();
        let mut unresolved: Vec<(String, String)> = dropped;
        for (user, device) in &targets {
            if user == &own_user && device == &own_device {
                continue;
            }
            let Ok(user_id) = matrix_sdk::ruma::UserId::parse(user) else {
                unresolved.push((user.clone(), device.clone()));
                continue;
            };
            let device_id: matrix_sdk::ruma::OwnedDeviceId =
                device.as_str().into();
            let refresh_due = {
                let marks = device_refresh_marks().lock().ok();
                match marks {
                    Some(marks) => device_refresh_due(
                        marks.get(user).map(|at| at.elapsed().as_secs()),
                    ),
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
            // A device the server keeps not having backs off (doubling, reset
            // when found): see device_miss_refresh_due().
            let miss_key = format!("{user}\u{1f}{device}");
            let mut queried = false;
            if !matches!(found, Ok(Some(_))) {
                let due = match device_miss_marks().lock() {
                    Ok(marks) => match marks.get(&miss_key) {
                        Some((at, misses)) => device_miss_refresh_due(
                            Some(at.elapsed().as_secs()),
                            *misses,
                        ),
                        None => true,
                    },
                    Err(_) => true,
                };
                if due {
                    let _ = client.encryption().request_user_identity(&user_id).await;
                    found =
                        client.encryption().get_device(&user_id, &device_id).await;
                    queried = true;
                }
            }
            let found_now = matches!(found, Ok(Some(_)));
            if let Ok(mut marks) = device_miss_marks().lock() {
                note_device_lookup(
                    &mut marks,
                    &miss_key,
                    found_now,
                    queried,
                    std::time::Instant::now(),
                );
            }
            if let Ok(Some(device)) = found {
                resolved.push(device);
            } else {
                unresolved.push((user.clone(), device.clone()));
            }
        }

        let report = if resolved.is_empty() {
            key_send_report(0, &[], &unresolved, Some("no_devices"))
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
                        Ok(failures) => {
                            let failures: Vec<(String, String)> = failures
                                .into_iter()
                                .map(|(user, device)| {
                                    (user.to_string(), device.to_string())
                                })
                                .collect();
                            key_send_report(total, &failures, &unresolved, None)
                        }
                        Err(err) => key_send_report(
                            0,
                            &[],
                            &unresolved,
                            Some(classify_room_error(&err.to_string())),
                        ),
                    }
                }
                None => key_send_report(0, &[], &unresolved, Some("invalid")),
            }
        };
        let KeySendReport {
            ok,
            category,
            delivered,
            undelivered,
        } = report;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "rtc_key_sent",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": ok,
            "category": category,
            "delivered": delivered,
            "undelivered": undelivered,
            "key_index": key_index,
        }));
    });
    Ok(())
}

const MAX_KEY_TARGET_ROWS: usize = 4 * MAX_MEMBERS;

pub(crate) fn split_key_targets(
    rows: &[serde_json::Value],
) -> (Vec<(String, String)>, Vec<(String, String)>) {
    let mut tried = Vec::new();
    let mut dropped = Vec::new();
    for value in rows.iter().take(MAX_KEY_TARGET_ROWS) {
        let (Some(user), Some(device)) = (
            value.get("user_id").and_then(|v| v.as_str()),
            value.get("device_id").and_then(|v| v.as_str()),
        ) else {
            continue;
        };
        match (sane(user, MAX_WIRE_LEN), sane(device, MAX_WIRE_LEN)) {
            (Some(user), Some(device)) if tried.len() < MAX_MEMBERS => {
                tried.push((user.to_owned(), device.to_owned()));
            }
            _ => dropped.push((user.to_owned(), device.to_owned())),
        }
    }
    (tried, dropped)
}

#[derive(Debug, PartialEq)]
pub(crate) struct KeySendReport {
    pub ok: bool,
    pub category: String,
    pub delivered: usize,
    pub undelivered: Vec<serde_json::Value>,
}

pub(crate) fn key_send_report(
    sent: usize,
    failures: &[(String, String)],
    unresolved: &[(String, String)],
    failed_category: Option<&str>,
) -> KeySendReport {
    let delivered = if failed_category.is_some() {
        0
    } else {
        sent.saturating_sub(failures.len())
    };
    let undelivered: Vec<serde_json::Value> = unresolved
        .iter()
        .chain(failures.iter())
        .take(MAX_KEY_TARGET_ROWS)
        .map(|(user, device)| json!({ "user_id": user, "device_id": device }))
        .collect();
    let category = match failed_category {
        Some(category) => category.to_owned(),
        None if undelivered.is_empty() => String::new(),
        None => "partial".to_owned(),
    };
    KeySendReport {
        ok: failed_category.is_none() && delivered > 0,
        category,
        delivered,
        undelivered,
    }
}

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

pub(crate) struct ValidatedMediaKey<'a> {
    pub index: u8,
    pub key: &'a str,
    pub room_id: &'a str,
    pub device_id: &'a str,
}

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

pub(crate) struct RtcHandlerGuards {
    _guards: Vec<EventHandlerDropGuard>,
}

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
            move |ev: OriginalSyncElementCallReactionEvent, room: Room| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
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
                    note_rtc_ring(room.own_user_id().as_str(), room.room_id().as_str());
                    enqueue(&events, msc4075_notification_payload(
                        &ev,
                        room.room_id().as_str(),
                        client.user_id(),
                        timelines.lifecycle(),
                    ));
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

    #[tokio::test]
    async fn membership_refresh_keeps_one_cleanup_timer() {
        use std::cell::{Cell, RefCell};
        let now = Cell::new(0_u64);
        let timers = RefCell::new(BTreeMap::<String, u64>::new());
        let armed = Cell::new(0);
        let mut live = String::new();
        for at in [0, 1000, 5000] {
            now.set(at);
            live = ensure_membership_delay(&live, true,
                |id| {
                    let result = match timers.borrow_mut().get_mut(&id) {
                        Some(deadline) => { *deadline = now.get() + 8000; Ok(()) },
                        None => Err("not_found".to_owned()),
                    };
                    std::future::ready(result)
                },
                || {
                    armed.set(armed.get() + 1);
                    let id = format!("timer-{}", armed.get());
                    timers.borrow_mut().insert(id.clone(), now.get() + 8000);
                    std::future::ready(Ok(id))
                }).await.unwrap();
        }
        assert_eq!(armed.get(), 1, "a refresh orphaned the original cleanup timer");
        assert_eq!(live, "timer-1");
        assert!(timers.borrow().values().all(|deadline| *deadline > 9000),
                "an old timer removes the live membership after eight seconds");
    }

    #[tokio::test]
    async fn membership_missing_timer_after_write_requires_state_repair() {
        let id = ensure_membership_delay("expired", true,
            |_| std::future::ready(Err("not_found".to_owned())),
            || { panic!("must restore membership state before replacing its timer");
                 #[allow(unreachable_code)]
                 std::future::ready(Ok(String::new())) }).await;
        assert_eq!(id, Err("not_found".to_owned()));
    }

    #[tokio::test]
    async fn membership_missing_timer_before_write_allows_fresh_publish() {
        let id = reuse_membership_delay("expired",
            |_| std::future::ready(Err("not_found".to_owned()))).await.unwrap();
        assert!(id.is_empty());
    }

    #[tokio::test]
    async fn membership_ambiguous_restart_never_arms_a_second_timer() {
        for reason in ["network", "rate_limited", "forbidden", "unrecognized"] {
            let id = ensure_membership_delay("possibly-live", true,
                |_| std::future::ready(Err(reason.to_owned())),
                || { panic!("an ambiguous restart must not orphan the old timer");
                     #[allow(unreachable_code)]
                     std::future::ready(Ok(String::new())) }).await;
            assert_eq!(id, Err(reason.to_owned()));
        }
    }

    #[tokio::test]
    async fn membership_refusal_cache_does_not_abandon_a_live_timer() {
        let id = ensure_membership_delay("working", false,
            |_| std::future::ready(Ok(())),
            || std::future::ready(Err("must not schedule".to_owned()))).await.unwrap();
        assert_eq!(id, "working");
        let id = ensure_membership_delay("", false,
            |_| std::future::ready(Err("not_found".to_owned())),
            || std::future::ready(Err("must not schedule".to_owned()))).await.unwrap();
        assert!(id.is_empty());
    }

    #[test]
    fn membership_retraction_is_not_skipped_by_a_stale_lifecycle() {
        let src = include_str!("rtc.rs");
        let start = src.find("pub(crate) fn retract_membership(").expect("retract fn");
        let body = &src[start..];
        let end = body.find("\nfn update_delayed(").expect("next fn");
        let body = &body[..end];
        let write = body.find("json!({}))").expect("the retraction write");
        assert!(!body[..write].contains("lifecycle_current"),
                "a stale lifecycle must not skip the retraction write");
        assert!(body[write..].contains("lifecycle_current"),
                "the answer must still be suppressed for a stale lifecycle");
    }

    #[test]
    fn membership_out_of_order_leave_cannot_cancel_a_new_join() {
        let mut registry = MembershipDelayRegistry::default();
        let key = "room\u{1f}device";
        assert!(registry.accept_operation(key, 20));
        registry.delays.insert(key.to_owned(), "current-timer".to_owned());
        assert!(!registry.accept_operation(key, 19));
        assert_eq!(registry.delays.get(key).map(String::as_str), Some("current-timer"));
        assert!(registry.accept_operation("other-room\u{1f}device", 18));
        assert!(registry.accept_operation("room\u{1f}other-device", 17));
    }

    #[test]
    fn membership_out_of_order_publish_cannot_resurrect_a_left_call() {
        let mut registry = MembershipDelayRegistry::default();
        let key = "room\u{1f}device";
        assert!(registry.accept_operation(key, 30));
        registry.delays.remove(key);
        assert!(!registry.accept_operation(key, 29));
        assert!(registry.accept_operation(key, 31));
    }

    #[test]
    fn a_partial_key_delivery_names_every_device_it_missed() {
        let pair = |u: &str, d: &str| (u.to_owned(), d.to_owned());
        let failed = [pair("@b:x", "BDEV")];
        let skipped = [pair("@c:x", "CDEV")];
        let report = key_send_report(2, &failed, &skipped, None);
        assert!(report.ok, "one of two devices got it");
        assert_eq!(report.category, "partial");
        assert_eq!(report.delivered, 1);
        assert_eq!(
            report.undelivered,
            vec![
                json!({ "user_id": "@c:x", "device_id": "CDEV" }),
                json!({ "user_id": "@b:x", "device_id": "BDEV" }),
            ]
        );

        let clean = key_send_report(2, &[], &[], None);
        assert!(clean.ok);
        assert_eq!(clean.category, "");
        assert!(clean.undelivered.is_empty());

        let none = key_send_report(1, &failed, &[], None);
        assert!(!none.ok);
        assert_eq!(none.delivered, 0);
        assert_eq!(none.undelivered.len(), 1);

        let refused = key_send_report(0, &[], &skipped, Some("no_devices"));
        assert!(!refused.ok);
        assert_eq!(refused.category, "no_devices");
        assert_eq!(refused.undelivered.len(), 1);

        let many: Vec<(String, String)> = (0..(MAX_KEY_TARGET_ROWS + 5))
            .map(|i| pair("@u:x", &format!("D{i}")))
            .collect();
        assert_eq!(
            key_send_report(1, &many, &[], None).undelivered.len(),
            MAX_KEY_TARGET_ROWS
        );
    }

    #[test]
    fn targets_a_send_drops_are_still_named_back() {
        let row = |u: &str, d: &str| json!({ "user_id": u, "device_id": d });
        let long = "D".repeat(MAX_WIRE_LEN + 1);
        let mut rows = vec![
            row("@a:x", "ADEV"),
            row("@b:x", &long),
            json!({ "user_id": "@c:x" }),
            json!(42),
        ];
        for i in 0..(MAX_MEMBERS + 2) {
            rows.push(row("@m:x", &format!("M{i}")));
        }
        let (tried, dropped) = split_key_targets(&rows);
        assert_eq!(tried.len(), MAX_MEMBERS);
        assert_eq!(tried[0], ("@a:x".to_owned(), "ADEV".to_owned()));
        assert!(dropped.contains(&("@b:x".to_owned(), long.clone())));
        let overflow = dropped.iter().filter(|(u, _)| u == "@m:x").count();
        assert_eq!(overflow, 3);
        assert_eq!(dropped.len(), 4);
        let report = key_send_report(1, &[], &dropped, None);
        assert_eq!(report.undelivered.len(), 4);
        assert_eq!(report.category, "partial");
    }

    #[test]
    fn only_a_query_actually_made_counts_as_a_miss() {
        let mut marks = std::collections::HashMap::new();
        let t0 = std::time::Instant::now();
        note_device_lookup(&mut marks, "u\u{1f}D", false, true, t0);
        assert_eq!(marks.get("u\u{1f}D").map(|(_, m)| *m), Some(1));
        let later = t0 + std::time::Duration::from_secs(5);
        note_device_lookup(&mut marks, "u\u{1f}D", false, false, later);
        let (at, misses) = marks["u\u{1f}D"];
        assert_eq!(misses, 1, "a skipped query was counted");
        assert_eq!(at, t0, "a skipped query moved the clock");
        note_device_lookup(&mut marks, "u\u{1f}D", false, true, later);
        assert_eq!(marks["u\u{1f}D"].1, 2);
        note_device_lookup(&mut marks, "u\u{1f}D", true, false, later);
        assert!(marks.is_empty(), "finding the device did not reset it");
    }

    #[test]
    fn a_missing_device_is_re_queried_with_a_doubling_backoff() {
        assert!(device_miss_refresh_due(None, 0));
        assert!(device_miss_refresh_due(Some(0), 0));
        assert!(!device_miss_refresh_due(Some(9), 1));
        assert!(device_miss_refresh_due(Some(10), 1));
        assert!(!device_miss_refresh_due(Some(19), 2));
        assert!(device_miss_refresh_due(Some(20), 2));
        assert!(!device_miss_refresh_due(Some(39), 3));
        assert!(!device_miss_refresh_due(Some(DEVICE_MISS_CEILING_SECS - 1), 30));
        assert!(device_miss_refresh_due(Some(DEVICE_MISS_CEILING_SECS), 30));
        assert!(device_miss_refresh_due(Some(DEVICE_MISS_CEILING_SECS), u32::MAX));
    }

    #[test]
    fn only_a_sixteen_byte_media_key_goes_on_the_wire() {
        use base64::Engine as _;
        let b64 = |n: usize| base64::engine::general_purpose::STANDARD.encode(vec![7u8; n]);
        assert!(outbound_media_key_ok(&b64(16)));
        assert!(!outbound_media_key_ok(&b64(32)), "a 32-byte key reaches Sable and is dropped");
        assert!(!outbound_media_key_ok(&b64(15)));
        assert!(!outbound_media_key_ok(""));
        assert!(!outbound_media_key_ok("not base64!"));
    }

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
        assert_eq!(MAX_SEND_KEY_INDEX, 15);
    }

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
        assert_eq!(member.hashed_rtc_identity,
                   Some(rtc_identity_hash("@a:x", "@a:x:DEVICE")));
        assert_eq!(member.slot_id, "m.call#ROOM");
        assert_eq!(member.expires_at_ms, 1_000 + 14_400_000);
        assert_eq!(member.foci.len(), 1);
        assert_eq!(member.foci[0].service_url, "https://sfu.example.org");
        assert_eq!(member.kind, MembershipKind::Session);
    }

    #[test]
    fn membership_identity_defaults_to_user_colon_device() {
        let mut content = session_content();
        content.as_object_mut().unwrap().remove("membershipID");
        let member =
            parse_session_membership(&content, "@a:x", 1_000).expect("valid");
        assert_eq!(member.rtc_identity, "@a:x:DEVICE");
        assert_eq!(member.hashed_rtc_identity,
                   Some(rtc_identity_hash("@a:x", "@a:x:DEVICE")));
    }

    #[test]
    fn session_membership_hashes_its_custom_member_id_with_the_sender() {
        let mut content = session_content();
        content["membershipID"] = json!("memberABC");
        let member = parse_session_membership(&content, "@alice:example.com", 1_000)
            .expect("valid");
        assert_eq!(member.rtc_identity, "@alice:example.com:DEVICE");
        assert_eq!(member.hashed_rtc_identity.as_deref(),
                   Some("M3VnsLXrkbxIDINXO64u8PZzFyD9ZYFiQULR2dshWYs"));
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
        assert_eq!(member.expires_at_ms, 5_000 + MAX_EXPIRE_MS);
        assert_eq!(aggregate_session(vec![member.clone()], 10_000).len(), 1);
        assert_eq!(
            aggregate_session(vec![member], 5_000 + MAX_EXPIRE_MS + 1).len(),
            0
        );
    }

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

    #[test]
    fn a_refresh_of_a_day_old_call_is_still_live() {
        let created = 1_700_000_000_000u64;
        let now = created + 25 * 60 * 60 * 1000;
        let mut content = session_content();
        content["created_ts"] = json!(created);
        content["expires"] = json!(expires_for_refresh(MEMBERSHIP_EXPIRY_MS, Some(created), now));
        let member = parse_session_membership(&content, "@a:x", now).expect("valid");
        assert!(member.expires_at_ms > now, "refresh must extend past now");
        assert_eq!(aggregate_session(vec![member.clone()], now).len(), 1);
        let younger = parse_session_membership(&session_content(), "@b:y", now).expect("valid");
        let ordered = aggregate_session(vec![younger, member], now);
        assert_eq!(ordered[0].user_id, "@a:x");
    }

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

    #[test]
    fn a_membership_cannot_claim_another_participants_identity() {
        let mut content = session_content();
        content["membershipID"] = json!("@alice:x:ALICEDEV");
        let member = parse_session_membership(&content, "@mallory:x", 1_000)
            .expect("valid");
        assert_eq!(member.rtc_identity, "@mallory:x:DEVICE");
        assert_eq!(member.hashed_rtc_identity,
                   Some(rtc_identity_hash("@mallory:x", "@alice:x:ALICEDEV")));
        assert_ne!(member.hashed_rtc_identity,
                   Some(rtc_identity_hash("@alice:x", "@alice:x:ALICEDEV")));
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

        let stable = serde_json::json!({
            "m.rtc_foci": [
                { "type": "livekit",
                  "livekit_service_url": "https://sfu.example.org/" },
            ],
        });
        assert_eq!(pick(&stable).len(), 1);

        let wrong = serde_json::json!({
            "rtc_transports": [
                { "type": "livekit",
                  "livekit_service_url": "https://sfu.example.org/" },
            ],
        });
        assert!(pick(&wrong).is_empty());
    }

    #[test]
    fn published_focus_does_not_synthesize_authorization_endpoint_slashes() {
        for (advertised, prefix) in [
            ("https://sfu.example.org", ""),
            ("https://sfu.example.org/", "/"),
            ("https://sfu.example.org/matrix/rtc", "/matrix/rtc"),
            ("https://sfu.example.org/matrix/rtc/", "/matrix/rtc/"),
        ] {
            let transport = parse_transport(&json!({
                "type": "livekit", "livekit_service_url": advertised,
            })).expect("valid focus");
            let content = own_membership_content(
                "DEVICE", "@a:example.org", Some(&transport), "audio", None,
                DEFAULT_EXPIRE_MS,
            );
            let base = content["foci_preferred"][0]["livekit_service_url"]
                .as_str().expect("published service URL");
            assert_eq!(base, advertised);
            for endpoint in ["get_token", "sfu/get"] {
                let request = url::Url::parse(&format!("{base}/{endpoint}"))
                    .expect("Element endpoint");
                assert_eq!(request.path(), format!("{prefix}/{endpoint}"),
                           "advertised focus {advertised}");
            }
        }
    }

    #[test]
    fn published_focus_preserves_explicit_trailing_slashes() {
        for advertised in ["https://sfu.example.org/", "https://sfu.example.org/matrix/rtc/"] {
            let transport = parse_transport(&json!({
                "type": "livekit", "livekit_service_url": advertised,
            })).expect("valid focus");
            assert_eq!(transport.to_json()["livekit_service_url"], advertised);
        }
    }

    #[test]
    fn published_focus_preserves_elements_service_connection_key() {
        let advertised = "https://sfu.example.org";
        let transport = parse_transport(&json!({
            "type": "livekit", "livekit_service_url": advertised,
        })).expect("valid focus");
        assert_eq!(transport.to_json()["livekit_service_url"], advertised);
    }

    #[test]
    fn published_focus_round_trips_and_reuses_a_shared_service() {
        let parse = |url: &str| parse_transport(&json!({
            "type": "livekit", "livekit_service_url": url,
        })).expect("valid focus");
        for base in ["https://sfu.example.org", "https://sfu.example.org/matrix/rtc"] {
            let own = parse(base);
            let wire = own.to_json();
            let refreshed = parse_transport(&wire).expect("refreshed focus");
            assert_eq!(refreshed, own);
            assert_eq!(refreshed.to_json(), wire);
        }
        let mut a = member_at("@a:x", "A", 10, 9_000);
        a.focus_selection = FocusSelection::MultiSfu;
        a.foci = vec![parse("https://sfu.example.org/")];
        let mut b = member_at("@b:x", "B", 20, 9_000);
        b.focus_selection = FocusSelection::MultiSfu;
        b.foci = vec![parse("https://sfu.example.org")];
        let plan = plan_foci(&[a, b], "@me:x:ME", Some(parse("https://sfu.example.org")));
        assert_eq!(plan.subscribers.len(), 1);
        assert!(same_focus_url(
            &plan.publisher.as_ref().unwrap().service_url,
            &plan.subscribers.first().unwrap().service_url,
        ));
        assert_eq!(plan.publisher.unwrap().to_json()["livekit_service_url"], "https://sfu.example.org");
    }

    #[test]
    fn authorization_service_bases_cannot_hide_endpoints_in_queries_or_fragments() {
        for base in [
            "https://sfu.example.org?", "https://sfu.example.org?next=/",
            "https://sfu.example.org#", "https://sfu.example.org/#ignored",
        ] {
            assert!(parse_transport(&json!({
                "type": "livekit", "livekit_service_url": base,
            })).is_none(), "not an endpoint base: {base}");
        }
        assert!(parse_transport(&json!({
            "type": "livekit", "livekit_service_url": "https://sfu.example.org/prefix%3Fpart",
        })).is_some());
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
    fn a_homeserver_without_matrixrtc_counts_as_having_answered() {
        for status in [400u16, 404, 405] {
            assert_eq!(status_category(status), "unsupported");
            assert!(
                discovery_answer_is_definitive(status_category(status)),
                "HTTP {status} is a homeserver saying it has no MatrixRTC, \
                 not a check that failed"
            );
        }
        assert!(discovery_answer_is_definitive(""));

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
            hashed_rtc_identity: None,
            slot_id: "m.call#ROOM".to_owned(),
            intent: "audio",
            created_ts: created,
            expires_at_ms: expires,
            foci: Vec::new(),
            focus_selection: FocusSelection::OldestMembership,
            kind: MembershipKind::Session,
            display_name: String::new(),
            avatar_mxc: String::new(),
            event_id: String::new(),
        }
    }

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

    #[test]
    fn the_fallback_write_is_measured_from_created_ts_not_from_zero() {
        let join = 1_000_000u64;
        let now = join + 20 * 60 * 1000;
        let expires = fallback_expires_ms(Some(join), now);
        let deadline = join + expires;
        assert!(
            deadline > now,
            "the fallback write published a membership that was already dead: \
             deadline {deadline} is not after now {now}"
        );
        assert_eq!(deadline, now + MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
    }

    #[test]
    fn a_fresh_join_still_gets_the_plain_period() {
        assert_eq!(
            fallback_expires_ms(None, 5_000_000),
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        );
    }

    #[test]
    fn a_membership_the_fallback_wrote_survives_its_own_parser() {
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
        let ghost = membership_event("@ghost:x", "D9", 1_000, 300_000);
        assert!(!ghost["content"].as_object().unwrap().is_empty());
        assert!(!store_view_is_usable(&[ghost], 1_000 + 300_001));
    }

    #[test]
    fn an_unparseable_membership_does_not_make_the_store_authoritative() {
        let junk = json!({
            "type": EV_MEMBER_LEGACY,
            "sender": "@a:x",
            "state_key": "_@a:x_D1_m.call",
            "origin_server_ts": 1_000,
            "content": {"unrelated": true},
        });
        assert!(!store_view_is_usable(&[junk], 2_000));
    }

    const A_WEEK_MS: u64 = 7 * 24 * 60 * 60 * 1000;

    #[test]
    fn an_idle_room_full_of_dead_memberships_costs_no_request() {
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
        assert_eq!(
            store_read_verdict(&[], A_WEEK_MS, false, false),
            StoreVerdict::AnswerNoSession
        );
        assert_eq!(newest_session_signal_ms(&[], A_WEEK_MS), None);
    }

    #[test]
    fn a_live_membership_is_still_answered_by_the_store_alone() {
        let now = 60_000u64;
        let store = vec![membership_event("@a:x", "D1", 1_000, 300_000)];
        assert_eq!(
            store_read_verdict(&store, now, false, false),
            StoreVerdict::Answer
        );
    }

    #[test]
    fn a_membership_that_lapsed_moments_ago_still_buys_one_request() {
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
        let now = A_WEEK_MS;
        let fresh = vec![retraction_event("@a:x", "D1", now - 30_000)];
        assert_eq!(
            store_read_verdict(&fresh, now, false, false),
            StoreVerdict::AskServer(EscalationReason::RecentActivity)
        );
        let stale = vec![retraction_event("@a:x", "D1", now - 60 * 60 * 1000)];
        assert_eq!(
            store_read_verdict(&stale, now, false, false),
            StoreVerdict::AnswerNoSession
        );
    }

    #[test]
    fn the_deadline_counts_even_when_the_event_itself_is_old() {
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
        assert_eq!(
            store_read_verdict(&[], A_WEEK_MS, false, true),
            StoreVerdict::AskServer(EscalationReason::Ring)
        );
    }

    #[test]
    fn the_escalation_backoff_doubles_and_stops_at_the_ceiling() {
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
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        const ME: &str = "@me:matrix.example";
        let room = "!backoff:matrix.example";
        let other = "!neighbour:matrix.example";
        clear_server_escalation_backoff(ME, room);
        clear_server_escalation_backoff(ME, other);

        assert!(claim_server_escalation(ME, room), "the first ask is free");
        assert!(
            !claim_server_escalation(ME, room),
            "a second ask inside the gap is exactly the storm this bounds"
        );
        assert!(
            claim_server_escalation(ME, other),
            "one room's backoff must never silence another's — a per-process \
             gate would make a real call wait on an idle room's ghost"
        );

        note_rtc_ring(ME, room);
        assert!(rtc_ring_is_recent(ME, room));
        assert!(!rtc_ring_is_recent(ME, other));
        assert!(
            claim_server_escalation(ME, room),
            "a ring must not have to wait out a backoff the room earned \
             while it was idle: the Answer button is gated on this read"
        );

        forget_account_memberships_published(ME, "DEVICE");
        assert!(!rtc_ring_is_recent(ME, room));
        assert!(
            claim_server_escalation(ME, room),
            "an account switch must not inherit the previous account's gap"
        );
        clear_server_escalation_backoff(ME, room);
        clear_server_escalation_backoff(ME, other);
    }

    #[test]
    fn a_bound_cuts_the_dead_memberships_before_the_live_ones() {
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
    fn only_a_recorded_non_join_state_rejects_a_call_advertiser() {
        use matrix_sdk::ruma::events::room::member::MembershipState;

        for (membership, expected) in [
            (Some(MembershipState::Join), true),
            (None, true),
            (Some(MembershipState::Leave), false),
            (Some(MembershipState::Ban), false),
            (Some(MembershipState::Invite), false),
        ] {
            assert_eq!(
                sender_membership_allows_call(membership.as_ref()),
                expected,
                "membership {membership:?}"
            );
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
    fn expired_or_different_call_membership_cannot_select_remote_focus() {
        let focus = |host: &str| LivekitTransport {
            service_url: format!("https://{host}/"), alias: None,
        };
        let mut expired = member_at("@expired:x", "D1", 10, 100);
        expired.focus_selection = FocusSelection::MultiSfu;
        expired.foci = vec![focus("expired.example")];
        let mut other_call = member_at("@other:x", "D2", 20, 9_000);
        other_call.slot_id = "m.call#another-call".to_owned();
        other_call.focus_selection = FocusSelection::MultiSfu;
        other_call.foci = vec![focus("other-call.example")];
        let members = room_call_members(vec![expired, other_call], 1_000);
        assert!(members.is_empty(), "neither is a participant in this call");
        let own = focus("own.example");
        let plan = plan_foci(&members, "@me:x:ME", Some(own.clone()));
        assert_eq!(plan.publisher, Some(own));
        assert!(plan.subscribers.is_empty());
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
    fn report_shaped_multi_sfu_plan_publishes_locally_and_reuses_subscribers() {
        let focus = |host: &str| LivekitTransport {
            service_url: format!("https://{host}/"), alias: None,
        };
        let mut c = member_at("@c:server-c", "C", 10, 9_000);
        c.focus_selection = FocusSelection::MultiSfu;
        c.foci = vec![focus("c.example")];
        let mut b = member_at("@b:server-a", "B", 20, 9_000);
        b.focus_selection = FocusSelection::MultiSfu;
        b.foci = vec![focus("a.example")];
        let mut b2 = member_at("@b2:server-a", "B2", 25, 9_000);
        b2.focus_selection = FocusSelection::MultiSfu;
        b2.foci = b.foci.clone();
        let own = member_at("@me:server-a", "ME", 30, 9_000);
        let members = aggregate_session(vec![own, b2, b, c], 100);
        let plan = plan_foci(&members, "@me:server-a:ME", Some(focus("a.example")));
        assert_eq!(plan.mode, FocusSelection::MultiSfu);
        assert_eq!(plan.publisher, Some(focus("a.example")));
        assert_eq!(plan.subscribers, vec![focus("c.example"), focus("a.example")]);
    }

    #[test]
    fn no_own_focus_uses_oldest_publisher_and_subscribes_to_each_peer() {
        let focus = |host: &str| LivekitTransport {
            service_url: format!("https://{host}/"), alias: None,
        };
        let mut older = member_at("@a:x", "A", 10, 9_000);
        older.focus_selection = FocusSelection::MultiSfu;
        older.foci = vec![focus("old.example")];
        let mut newer = member_at("@b:x", "B", 20, 9_000);
        newer.focus_selection = FocusSelection::MultiSfu;
        newer.foci = vec![focus("new.example")];
        let plan = plan_foci(&[older, newer], "@me:x:ME", None);
        assert_eq!(plan.mode, FocusSelection::OldestMembership);
        assert_eq!(plan.publisher, Some(focus("old.example")));
        assert_eq!(plan.subscribers,
                   vec![focus("old.example"), focus("new.example")]);
        let content = own_membership_content_with_mode("ME", "@me:x",
            plan.publisher.as_ref(), FocusSelection::OldestMembership,
            "audio", None, MEMBERSHIP_EXPIRY_MS);
        assert_eq!(content["focus_active"]["focus_selection"], "oldest_membership");
    }

    #[test]
    fn mixed_focus_modes_keep_oldest_publisher_and_subscribe_to_both() {
        let focus = |host: &str| LivekitTransport {
            service_url: format!("https://{host}/"), alias: None,
        };
        let mut oldest = member_at("@c:server-c", "C", 10, 9_000);
        oldest.foci = vec![focus("c.example")];
        let mut modern = member_at("@b:server-a", "B", 20, 9_000);
        modern.focus_selection = FocusSelection::MultiSfu;
        modern.foci = vec![focus("a.example")];
        let members = aggregate_session(vec![modern, oldest], 100);
        let plan = plan_foci(&members, "@me:server-a:ME", Some(focus("a.example")));
        assert_eq!(plan.mode, FocusSelection::OldestMembership);
        assert_eq!(plan.publisher, Some(focus("c.example")));
        assert_eq!(plan.subscribers, vec![focus("c.example"), focus("a.example")]);
    }

    #[test]
    fn first_member_advertises_multi_sfu_on_own_focus() {
        let own = LivekitTransport {
            service_url: "https://own.example/".to_owned(), alias: None,
        };
        let plan = plan_foci(&[], "@me:x:DEV", Some(own.clone()));
        assert_eq!(plan.mode, FocusSelection::MultiSfu);
        assert_eq!(plan.publisher, Some(own.clone()));
        let content = own_membership_content_with_mode(
            "DEV", "@me:x", Some(&own), plan.mode, "audio", None,
            MEMBERSHIP_EXPIRY_MS);
        assert_eq!(content["focus_active"]["focus_selection"], "multi_sfu");
    }

    #[test]
    fn focus_plan_caps_distinct_services_including_publisher() {
        let members: Vec<RtcMember> = (0..12).map(|n| {
            let mut member = member_at("@peer:x", &format!("D{n}"),
                                       n + 1, 9_000);
            member.focus_selection = FocusSelection::MultiSfu;
            member.foci = vec![LivekitTransport {
                service_url: format!("https://focus{n}.example/"), alias: None,
            }];
            member
        }).collect();
        let own = LivekitTransport {
            service_url: "https://own.example/".to_owned(), alias: None,
        };
        let plan = plan_foci(&members, "@me:x:DEV", Some(own));
        assert!(plan.subscribers.len() <= 7);
    }

    #[test]
    fn membership_fixture_parses_multi_sfu_selection() {
        let content = json!({
            "application": "m.call", "call_id": "", "scope": "m.room",
            "device_id": "B", "focus_active": {
                "type": "livekit", "focus_selection": "multi_sfu"
            },
            "foci_preferred": [{"type": "livekit",
                "livekit_service_url": "https://a.example"}]
        });
        let member = parse_session_membership(&content, "@b:server-a", 100)
            .expect("membership");
        assert_eq!(member.focus_selection, FocusSelection::MultiSfu);
        assert_eq!(member.foci[0].service_url, "https://a.example");
    }

    #[test]
    fn remote_tracks_route_by_membership_across_two_connections() {
        let focus = |host: &str| LivekitTransport {
            service_url: format!("https://{host}/"), alias: None,
        };
        let mut b = member_at("@b:server-a", "B", 20, 9_000);
        b.focus_selection = FocusSelection::MultiSfu;
        b.foci = vec![focus("a.example")];
        let mut c = member_at("@c:server-c", "C", 10, 9_000);
        c.focus_selection = FocusSelection::MultiSfu;
        c.foci = vec![focus("c.example")];
        let members = aggregate_session(vec![b, c], 100);
        let oldest = select_focus(&members);
        assert_eq!(focus_for_member(&members[0], oldest.as_ref()), Some(focus("c.example")));
        assert_eq!(focus_for_member(&members[1], oldest.as_ref()), Some(focus("a.example")));
        assert_ne!(members[0].rtc_identity, members[1].rtc_identity);
    }

    #[test]
    fn focus_does_not_walk_past_a_silent_oldest_member() {
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
    #[cfg(not(feature = "test-loopback-foci"))]
    fn transport_url_refuses_credentials_and_unroutable_hosts() {
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
            "https://[::ffff:127.0.0.1]/",
            "https://[::ffff:169.254.169.254]/",
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
    #[cfg(feature = "test-loopback-foci")]
    fn test_transport_accepts_https_loopback_only() {
        for good in ["https://127.0.0.2:8443/", "https://[::1]/", "https://localhost/"] {
            assert!(sane_https_url(good).is_some(), "{good}");
        }
        for bad in ["http://127.0.0.1/", "https://10.0.0.5/",
                    "https://169.254.1.1/", "https://[fc00::1]/",
                    "https://[::ffff:127.0.0.1]/", "https://sfu.local/"] {
            assert!(sane_https_url(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn no_members_means_no_focus() {
        assert!(select_focus(&[]).is_none());
    }

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
        assert_eq!(member.rtc_identity,
                   rtc_identity_hash("@a:x", "member-1"));
        assert_eq!(member.hashed_rtc_identity, None);
        assert_eq!(member.foci.len(), 1);
    }

    #[test]
    fn sticky_membership_for_another_user_is_forgery() {
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
    fn a_device_refresh_is_owed_before_the_first_send_and_then_rate_limited() {
        assert!(device_refresh_due(None));
        assert!(!device_refresh_due(Some(0)));
        assert!(!device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS - 1)));
        assert!(device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS)));
        assert!(device_refresh_due(Some(DEVICE_REFRESH_TTL_SECS * 10)));
        assert!(DEVICE_REFRESH_TTL_SECS >= 10 && DEVICE_REFRESH_TTL_SECS <= 600);
    }

    #[test]
    fn an_undecryptable_message_is_only_a_call_fault_for_a_call_peer() {
        assert!(!undecryptable_key_report_due(false, None));
        assert!(!undecryptable_key_report_due(false, Some(0)));
        assert!(!undecryptable_key_report_due(
            false,
            Some(DEVICE_REFRESH_TTL_SECS * 10),
        ));
    }

    #[test]
    fn a_lost_key_is_reported_once_and_then_paced() {
        assert!(undecryptable_key_report_due(true, None));
        assert!(!undecryptable_key_report_due(true, Some(0)));
        assert!(!undecryptable_key_report_due(
            true,
            Some(DEVICE_REFRESH_TTL_SECS - 1),
        ));
        assert!(undecryptable_key_report_due(
            true,
            Some(DEVICE_REFRESH_TTL_SECS),
        ));
    }

    #[test]
    fn rtc_identity_is_the_hash_the_reference_implementation_computes() {
        assert_eq!(rtc_identity_hash("@alice:example.com", "memberABC"),
                   "M3VnsLXrkbxIDINXO64u8PZzFyD9ZYFiQULR2dshWYs");
        let identity = rtc_identity_hash("@a:x", "member-1");
        let formula = Sha256::digest(br#"["@a:x","member-1"]"#);
        use base64::Engine as _;
        assert_eq!(identity,
                   base64::engine::general_purpose::STANDARD_NO_PAD.encode(formula));
        assert!(!identity.contains('='), "must be unpadded");
        assert_eq!(identity.len(), 43, "sha256 in unpadded base64");
        assert_ne!(identity, rtc_identity_hash("@a:x", "member-2"));
        assert_ne!(identity, rtc_identity_hash("@b:x", "member-1"));
    }

    #[test]
    fn membership_state_key_matches_what_element_writes() {
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
        assert_eq!(member.created_ts, 5_000);
        assert_eq!(member.expires_at_ms, 5_000 + MEMBERSHIP_EXPIRY_MS);
    }

    #[test]
    fn a_refresh_preserves_the_original_join_time() {
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
        let content = own_membership_content(
            "DEVICE", "@a:x", None, "audio", None,
            MEMBERSHIP_EXPIRY_NO_DELAYED_MS);
        let member = parse_session_membership(&content, "@a:x", 5_000)
            .expect("valid");
        assert_eq!(
            member.expires_at_ms,
            5_000 + MEMBERSHIP_EXPIRY_NO_DELAYED_MS
        );
        assert!(MEMBERSHIP_EXPIRY_NO_DELAYED_MS >= 3 * 60 * 1000);
    }

    #[test]
    fn refreshing_a_membership_moves_the_deadline_peers_compute() {
        let period = MEMBERSHIP_EXPIRY_NO_DELAYED_MS;
        let created = 1_000_000_u64;

        assert_eq!(expires_for_refresh(period, None, created), period);

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

        let now = created + period + 1;
        let expires = expires_for_refresh(period, Some(created), now);
        assert!(created + expires > now,
                "the participant would vanish at exactly one period");
    }

    #[test]
    fn a_created_ts_in_the_future_still_yields_a_live_deadline() {
        let period = MEMBERSHIP_EXPIRY_NO_DELAYED_MS;
        let now = 1_000_000_u64;
        let created = now + 60_000;
        let expires = expires_for_refresh(period, Some(created), now);
        assert_eq!(expires, period, "saturating_sub must floor at zero");
        assert!(created + expires > now);

        let expires = expires_for_refresh(period, Some(u64::MAX), now);
        assert_eq!(expires, period);
        assert!(MEMBERSHIP_EXPIRY_NO_DELAYED_MS < MEMBERSHIP_EXPIRY_MS);
    }

    #[test]
    fn an_expired_ghosts_join_time_is_not_inherited() {
        let ghost = json!({
            "created_ts": 1_000u64,
            "expires": 5 * 60 * 1000u64,
            "device_id": "DEVICE",
        });
        assert_eq!(inheritable_created_ts(&ghost, None, 9_000_000), None);
        assert_eq!(
            inheritable_created_ts(&ghost, None, 200_000),
            Some(1_000)
        );
    }

    #[test]
    fn a_naked_retraction_is_only_sent_where_a_repair_write_follows() {
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
        assert!(delayed_retraction_is_repairable(false),
                "nothing arms a delayed retraction at all any more");
    }

    #[test]
    fn one_servers_refusal_does_not_disable_msc4140_for_another() {
        let old = "https://old.example.org/";
        let modern = "https://modern.example.org/";

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

        mark_delayed_refusal(modern, true);
        mark_delayed_refusal(old, false);
        assert!(!delayed_refusal_recorded(old));
        assert!(delayed_refusal_recorded(modern));

        mark_delayed_refusal(modern, false);
    }

    #[test]
    fn only_an_absent_endpoint_disables_msc4140_for_a_server() {
        for transient in ["network", "rate_limited", "forbidden", "invalid"] {
            assert!(
                !delayed_refusal_is_permanent(transient),
                "a '{transient}' refusal would permanently disable delayed                  retractions for every room in the process, so an unclean                  exit would strand a phantom participant from then on"
            );
        }
        assert!(
            delayed_refusal_is_permanent("no_delay_id"),
            "a server that ANSWERED without a delay id was treated as a \
             passing blip, so a naked retraction is armed on every refresh"
        );
        for absent in ["unrecognized", "not_found"] {
            assert!(
                delayed_refusal_is_permanent(absent),
                "'{absent}' means the endpoint is not there, and re-probing                  it on every refresh costs a second state event each time"
            );
        }
        assert!(
            delayed_refusal_is_permanent("delayed_unsupported"),
            "the server said outright that it does not do delayed events; \
             treating that as a blip re-arms a doomed request on every publish"
        );
    }

    #[test]
    fn synapse_says_delayed_events_are_unsupported_and_it_is_not_a_blip() {
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
        assert!(delayed_failure_says_unsupported(
            "org.matrix.msc4140.errcode M_MAX_DELAY_UNSUPPORTED"
        ));
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

    static PUBLISH_SET_TEST_LOCK: Mutex<()> = Mutex::new(());

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
            mark_membership_published("@a:example", key);
            assert!(membership_published_in_this_process(key));
        }

        forget_room_memberships_published(room, None);
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

        forget_account_memberships_published("@a:example", "DEVICEA");
        assert!(
            !membership_published_in_this_process(&elsewhere),
            "teardown must clear the set, or a mark survives an account              switch and the next join inherits a ghost's created_ts"
        );
    }

    #[test]
    fn a_rejoin_is_a_join_until_this_process_has_published() {
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let room = "!probe:matrix.example";
        let key = membership_publish_key(room, "_@a:example_DEVICE_m.call");
        forget_membership_published(&key);

        assert!(
            !membership_published_in_this_process(&key),
            "a process that has published nothing must treat its first \
             publish as a JOIN, whatever is left in the room's state"
        );
        mark_membership_published("@a:example", &key);
        assert!(
            membership_published_in_this_process(&key),
            "a refresh must inherit created_ts, or the membership's deadline \
             walks forward and oldest-membership focus selection reorders"
        );
        forget_membership_published(&key);
        assert!(!membership_published_in_this_process(&key));

        mark_membership_published("@a:example", &membership_publish_key(
            "!other:matrix.example",
            "_@a:example_DEVICE_m.call",
        ));
        mark_membership_published("@b:example", &membership_publish_key(
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
        let content = json!({ "expires": 600_000u64 });
        assert_eq!(
            inheritable_created_ts(&content, Some(2_000), 3_000),
            Some(2_000)
        );
        assert_eq!(inheritable_created_ts(&json!({}), Some(2_000), 3_000), None);
        let hostile = json!({ "created_ts": 10u64, "expires": u64::MAX });
        assert_eq!(
            inheritable_created_ts(&hostile, None, u64::MAX - 1),
            Some(10)
        );
    }

    #[test]
    fn a_membership_without_a_focus_is_still_valid() {
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
        assert_eq!(value["lifetime"], json!(90_000));
        assert_eq!(value["m.call.intent"], json!("video"));
        assert_eq!(value["m.mentions"]["room"], json!(true));
        assert!(value.get("m.relates_to").is_none());
    }

    #[test]
    fn the_unstable_notification_forwards_its_type_and_mentions() {
        let ev: OriginalSyncMsc4075RtcNotificationEvent = serde_json::from_value(json!({
            "type": "org.matrix.msc4075.rtc.notification",
            "event_id": "$announce:x",
            "sender": "@caller:x",
            "origin_server_ts": 1_000,
            "content": {
                "notification_type": "notification",
                "sender_ts": 1_000,
                "lifetime": 90_000,
                "m.mentions": { "user_ids": ["@me:x"], "room": true },
            },
        }))
        .expect("a valid unstable notification");
        let payload = msc4075_notification_payload(&ev, "!r:x", None, 3);
        assert_eq!(payload["notification_type"], json!("notification"));
        assert_eq!(payload["mentions_user_ids"], json!(["@me:x"]));
        assert_eq!(payload["mentions_room"], json!(true));
        assert_eq!(payload["rtc"], json!(true));
        assert_eq!(payload["lifetime_ms"], json!(90_000));
        assert_eq!(payload["call_intent"], json!("audio"));
    }

    #[test]
    fn the_notification_event_type_is_the_one_element_sends() {
        assert_eq!(
            Msc4075RtcNotificationEventContent::TYPE,
            "org.matrix.msc4075.rtc.notification"
        );
    }

    #[test]
    fn the_raised_hand_key_is_element_calls_own_bytes() {
        assert_eq!(
            HAND_RAISED_KEY.as_bytes(),
            &[0xF0, 0x9F, 0x96, 0x90, 0xEF, 0xB8, 0x8F]
        );
        assert_eq!(HAND_RAISED_KEY.chars().count(), 2);
        assert_eq!(HAND_RAISED_KEY.chars().next(), Some('\u{1F590}'));
        assert_eq!(HAND_RAISED_KEY.chars().nth(1), Some('\u{FE0F}'));
    }

    #[test]
    fn the_call_reaction_event_type_is_element_calls_own() {
        assert_eq!(
            ElementCallReactionEventContent::TYPE,
            "io.element.call.reaction"
        );
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
        assert_eq!(value["m.relates_to"]["rel_type"], json!("m.reference"));
        assert_eq!(
            value["m.relates_to"]["event_id"],
            json!("$membership:example.org")
        );
        assert_eq!(value["emoji"], json!("\u{1F44D}"));
        assert_eq!(value["name"], json!("thumbsup"));

        let custom = ElementCallReactionEventContent {
            emoji: "🦊".to_owned(),
            name: String::new(),
            relates_to: Reference::new(
                EventId::parse("$membership:example.org").expect("event id"),
            ),
        };
        let custom_value = serde_json::to_value(&custom).expect("serializes");
        assert_eq!(custom_value["emoji"], json!("🦊"));
        assert_eq!(custom_value["name"], json!(""));
    }

    #[test]
    fn a_malformed_call_reaction_never_reaches_the_handler() {
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
    fn a_nameless_preset_reaction_is_sent_with_its_preset_name() {
        assert_eq!(reaction_wire_name("\u{1F44D}", "").as_deref(), Some("thumbsup"));
        assert_eq!(
            reaction_wire_name("\u{1F6CE}\u{FE0F}", "").as_deref(),
            Some("bell")
        );
        assert_eq!(reaction_wire_name("\u{1F98A}", "").as_deref(), Some(""));
        assert_eq!(
            reaction_wire_name("\u{1F44D}", "thumbsup").as_deref(),
            Some("thumbsup")
        );
        assert_eq!(reaction_wire_name("\u{1F98A}", "thumbsup"), None);
        assert_eq!(reaction_wire_name("\u{1F44D}", "fox"), None);
    }

    #[test]
    fn the_reaction_set_is_element_calls_own_bytes() {
        let thumbsup = ELEMENT_CALL_REACTIONS
            .iter()
            .find(|(name, _)| *name == "thumbsup")
            .expect("thumbsup is in element-call's set");
        assert_eq!(thumbsup.1.as_bytes(), &[0xF0, 0x9F, 0x91, 0x8D]);
        let dizzy = ELEMENT_CALL_REACTIONS
            .iter()
            .find(|(name, _)| *name == "dizzy")
            .expect("dizzy is in element-call's set");
        assert_eq!(
            dizzy.1.as_bytes(),
            &[0xF0, 0x9F, 0x98, 0xB5, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x92, 0xAB]
        );
        assert_eq!(dizzy.1.chars().count(), 3);

        let mut names: Vec<&str> =
            ELEMENT_CALL_REACTIONS.iter().map(|(name, _)| *name).collect();
        names.sort_unstable();
        let before = names.len();
        names.dedup();
        assert_eq!(names.len(), before);

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

        assert_eq!(reaction_emoji(""), None);
        assert_eq!(reaction_emoji("   "), None);
        assert_eq!(reaction_emoji("\u{1F44D}\u{0007}"), None);

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

    #[test]
    fn the_well_known_url_follows_the_server_name_port_included() {
        use super::transports_endpoint::endpoint_for;
        assert_eq!(
            endpoint_for("example.org").as_deref(),
            Ok("https://example.org/.well-known/matrix/client")
        );
        assert_eq!(
            endpoint_for("example.org:8448").as_deref(),
            Ok("https://example.org:8448/.well-known/matrix/client")
        );
        assert_eq!(
            endpoint_for("example.org:443").as_deref(),
            Ok("https://example.org/.well-known/matrix/client")
        );
        assert_eq!(
            endpoint_for("127.0.0.2:18101").as_deref(),
            Ok("https://127.0.0.2:18101/.well-known/matrix/client")
        );
        for hostile in [
            "evil.example/x",
            "user@evil.example",
            "evil.example?x",
            "evil.example#x",
            "a b",
            "",
            "example.org:notaport",
        ] {
            assert!(endpoint_for(hostile).is_err(), "{hostile:?} must be refused");
        }
    }

    #[test]
    fn a_non_default_port_has_one_validated_portless_fallback() {
        use super::transports_endpoint::fallback_for;
        assert_eq!(
            fallback_for("example.org:8448"),
            Ok(Some("https://example.org/.well-known/matrix/client".to_owned()))
        );
        assert_eq!(
            fallback_for("127.0.0.2:18101"),
            Ok(Some("https://127.0.0.2/.well-known/matrix/client".to_owned()))
        );
        assert_eq!(fallback_for("example.org"), Ok(None));
        assert_eq!(fallback_for("example.org:443"), Ok(None));
        assert!(fallback_for("evil.example/x:8448").is_err());
    }

    #[test]
    fn a_focus_url_is_kept_as_written_and_compared_by_meaning() {
        for spelled in [
            "https://sfu.example.org",
            "https://sfu.example.org/",
            "https://sfu.example.org:8443",
            "https://sfu.example.org/livekit/jwt",
        ] {
            assert_eq!(sane_https_url(spelled).as_deref(), Some(spelled));
        }
        assert!(same_focus_url("https://sfu.example.org", "https://sfu.example.org/"));
        assert!(!same_focus_url("https://sfu.example.org", "https://other.example.org"));
        assert!(!same_focus_url("https://sfu.example.org:8443", "https://sfu.example.org"));
        assert!(sane_https_url("http://sfu.example.org").is_none());
        assert!(sane_https_url("https://sfu.example.org ").is_none());
        assert!(sane_https_url(" https://sfu.example.org").is_none());
    }

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
        let wire = member.to_json();
        assert_eq!(wire.get("event_id").and_then(|v| v.as_str()), Some(""));
    }

    #[tokio::test]
    #[cfg(not(feature = "test-loopback-foci"))]
    async fn a_focus_that_resolves_into_private_space_is_refused() {
        for host in [
            "127.0.0.1",
            "10.0.0.5",
            "192.168.1.10",
            "172.16.0.1",
            "169.254.169.254",
            "100.64.0.1",
            "0.0.0.0",
            "::1",
            "fc00::1",
            "fe80::1",
            "::ffff:127.0.0.1",
            "64:ff9b::7f00:1",
        ] {
            assert!(
                resolve_public_host(host, 443).await.is_none(),
                "{host} resolves into unroutable space and must be refused"
            );
        }
    }

    #[tokio::test]
    #[cfg(not(feature = "test-loopback-foci"))]
    async fn local_only_names_are_refused_without_resolving() {
        for host in [
            "localhost",
            "sfu.localhost",
            "livekit.local",
            "sfu.internal",
            "LOCALHOST",
            "localhost.",
        ] {
            assert!(
                resolve_public_host(host, 443).await.is_none(),
                "{host} must be refused without resolving"
            );
            assert!(!public_hostname(host), "{host} must not read as public");
        }
    }

    #[tokio::test]
    #[cfg(feature = "test-loopback-foci")]
    async fn test_resolution_allows_only_loopback_exception() {
        for host in ["127.0.0.1", "127.2.3.4", "::1"] {
            assert!(resolve_public_host(host, 443).await.is_some(), "{host}");
        }
        for host in ["10.0.0.5", "169.254.169.254", "fc00::1",
                     "fe80::1", "::ffff:127.0.0.1"] {
            assert!(resolve_public_host(host, 443).await.is_none(), "{host}");
        }
    }

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

    #[test]
    fn retiring_one_account_keeps_another_accounts_rtc_state() {
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let room = "!shared-call:matrix.example";
        let (x, y) = ("@x:matrix.example", "@y:matrix.example");
        let x_key = membership_publish_key(room, &membership_state_key(x, "XDEV", "10"));
        let x_other_device =
            membership_publish_key(room, &membership_state_key(x, "XDEV2", "org.matrix.msc3757.10"));
        let y_key = membership_publish_key(room, &membership_state_key(y, "YDEV", "10"));
        let y_new_shape =
            membership_publish_key(room, &membership_state_key(y, "YDEV", "org.matrix.msc3757.10"));
        for (user, key) in [(x, &x_key), (x, &x_other_device), (y, &y_key), (y, &y_new_shape)] {
            mark_membership_published(user, key);
        }
        for user in [x, y] {
            clear_server_escalation_backoff(user, room);
            assert!(claim_server_escalation(user, room));
            note_rtc_ring(user, room);
            assert!(claim_server_escalation(user, room), "the ring cleared the gap");
        }

        forget_account_memberships_published(x, "XDEV");

        assert!(!membership_published_in_this_process(&x_key), "X's own mark must go");
        assert!(
            membership_published_in_this_process(&x_other_device),
            "another device of X is another session"
        );
        assert!(membership_published_in_this_process(&y_key), "Y's call was cleared by X's exit");
        assert!(membership_published_in_this_process(&y_new_shape));
        assert!(!rtc_ring_is_recent(x, room), "X's ring mark must go");
        assert!(rtc_ring_is_recent(y, room), "Y's ring mark was cleared by X's exit");
        assert!(claim_server_escalation(x, room), "X starts with no gap");
        assert!(
            !claim_server_escalation(y, room),
            "Y's backoff must survive: its claim above is still inside the gap"
        );

        forget_account_memberships_published(x, "XDEV2");
        forget_account_memberships_published(y, "YDEV");
        for key in [&x_other_device, &y_key, &y_new_shape] {
            assert!(!membership_published_in_this_process(key));
        }
        clear_server_escalation_backoff(x, room);
        clear_server_escalation_backoff(y, room);
    }

    #[test]
    fn a_leave_with_an_account_forgets_only_that_accounts_marks_in_the_room() {
        let _serialised = PUBLISH_SET_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let room = "!leave-one:matrix.example";
        let mine = membership_publish_key(room, &membership_state_key("@a:x", "A", "10"));
        let theirs = membership_publish_key(room, &membership_state_key("@b:x", "B", "10"));
        mark_membership_published("@a:x", &mine);
        mark_membership_published("@b:x", &theirs);
        forget_room_memberships_published(room, Some(("@a:x", "A")));
        assert!(!membership_published_in_this_process(&mine));
        assert!(membership_published_in_this_process(&theirs), "another account's call");
        forget_room_memberships_published(room, None);
        assert!(!membership_published_in_this_process(&theirs), "None keeps today's room-wide clear");
    }

    #[test]
    fn a_state_key_belongs_to_exactly_its_user_and_device() {
        assert!(state_key_belongs_to("_@a:x_DEV_m.call", "@a:x", "DEV"));
        assert!(state_key_belongs_to("@a:x_DEV_m.call", "@a:x", "DEV"));
        assert!(!state_key_belongs_to("_@a:x_DEV2_m.call", "@a:x", "DEV"));
        assert!(!state_key_belongs_to("_@a:x_DEV_m.call", "@a:x", "DE"));
        assert!(!state_key_belongs_to("_@a:x.evil_DEV_m.call", "@a:x", "DEV"));
        assert!(!state_key_belongs_to("__@a:x_DEV_m.call", "@a:x", "DEV"));
    }
}
