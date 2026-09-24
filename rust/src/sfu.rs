//! LiveKit SFU authorization and signalling for MatrixRTC.
//!
//! The signalling half of group calling: obtains SFU authorization, speaks
//! LiveKit's WebSocket/protobuf protocol, and reports participants, tracks
//! and session descriptions across the FFI. It owns no media; RTP flows
//! through the C++ GStreamer `webrtcbin` engine.
//!
//! Not the `livekit` client crate: it depends on `webrtc-sys`, which
//! downloads a prebuilt libwebrtc during the build (breaking
//! `--offline --locked`) and adds hundreds of crates. `livekit-protocol` is
//! message definitions only.
//!
//! The SFU is authorized with a Matrix OpenID token, never the access
//! token: `POST {service_url}/sfu/get` with `{room, openid_token,
//! device_id}` answers `{url, jwt}`. The JWT is never logged, persisted or
//! sent across the FFI; it lives in the signalling task for one connection.
//!
//! The SFU is outside the homeserver's trust boundary: everything it sends
//! is bounded and sanitized, and identities are compared, never rendered
//! raw. SDP (host IPs, ICE credentials) crosses only in media-capable mode,
//! is never logged and never reaches QML. One connection at a time, owned
//! by a generation counter, so a late frame cannot reach the next session.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use livekit_protocol as lkp;

/// The LiveKit `TrackSource` for a track we are about to publish. A named
/// function so tests and production share it (an inline literal mapping
/// was once off by one, publishing screen share as SCREEN_SHARE_AUDIO).
pub(crate) fn track_source_for(kind: i32, screen_share: bool) -> i32 {
    let video = kind == lkp::TrackType::Video as i32;
    match (screen_share, video) {
        // A share has two sources. Element renders share audio only as
        // SCREEN_SHARE_AUDIO, so labelling the audio half SCREEN_SHARE would send
        // it down the video path.
        (true, false) => lkp::TrackSource::ScreenShareAudio as i32,
        (true, true) => lkp::TrackSource::ScreenShare as i32,
        (false, true) => lkp::TrackSource::Camera as i32,
        (false, false) => lkp::TrackSource::Microphone as i32,
    }
}

/// LiveKit's per-track E2EE declaration. Receivers use it to decide whether
/// to decrypt, so encrypted bytes declared None render as garbage.
pub(crate) fn track_encryption_for(encrypted: bool) -> i32 {
    if encrypted {
        lkp::encryption::Type::Gcm as i32
    } else {
        lkp::encryption::Type::None as i32
    }
}
use matrix_sdk::ruma::api::client::account::request_openid_token;
use prost::Message as _;
use serde_json::json;
use tokio_tungstenite::tungstenite::Message as WsMessage;

use crate::rooms::{classify_room_error, public_ip, require_client};
use crate::rtc::{
    public_hostname, resolve_public_host, resolve_public_hosts, HostRefusal,
};
use crate::{enqueue, RustClient};

/// Bound on the JWT-service response body.
const MAX_SFU_RESPONSE: usize = 64 * 1024;
const SFU_TIMEOUT: Duration = Duration::from_secs(20);
/// Signalling frames are small; this party is outside the homeserver's
/// trust boundary.
const MAX_SIGNAL_FRAME: usize = 256 * 1024;
/// Cap on participants and tracks tracked and reported.
const MAX_PARTICIPANTS: usize = 128;
const MAX_TRACKS_PER_PARTICIPANT: usize = 8;
/// The signalling protocol version this client implements.
const LK_PROTOCOL_VERSION: u32 = 15;

/// Bounded and control-character free, or nothing (as in rtc.rs): `None`
/// means drop the surrounding thing.
fn sane(value: &str, max: usize) -> Option<&str> {
    if value.is_empty() || value.len() > max {
        return None;
    }
    if value.chars().any(|c| c.is_control()) {
        return None;
    }
    Some(value)
}

/// Shortest trailer forwarded; LiveKit's is ~43 bytes, and a short one could
/// match real frames by chance.
const MIN_SIF_TRAILER: usize = 16;

/// Longest server-injected-frame trailer forwarded. livekit-server's is
/// `base62(32 random bytes)`, about 43 bytes. Mirrors
/// `CallFrameCryptor::kMaxServerTrailerBytes`, which re-checks it.
const MAX_SIF_TRAILER: usize = 64;

/// `JoinResponse.sif_trailer`, base64 for the JSON bridge, or "" when absent
/// or unusable.
///
/// It marks the unencrypted blank frames the SFU injects into an encrypted
/// track (mute, unpublish, leave), which would otherwise read as decryption
/// failures. Oversized values are dropped, not truncated, since a truncated
/// trailer could match unmarked frames. Not secret, but never logged in
/// full.
fn sif_trailer_b64(trailer: &[u8]) -> String {
    if trailer.len() < MIN_SIF_TRAILER || trailer.len() > MAX_SIF_TRAILER
        || !trailer.iter().all(u8::is_ascii_alphanumeric)
    {
        return String::new();
    }
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(trailer)
}

/// Which peer connection a description or candidate belongs to. LiveKit runs
/// two: the client offers on publisher (its tracks), the server on
/// subscriber (everyone else's). Mixing them wires audio the wrong way.
fn target_str(target: i32) -> &'static str {
    match target {
        0 => "publisher",
        _ => "subscriber",
    }
}

pub(crate) fn target_from_str(value: &str) -> i32 {
    match value {
        "publisher" => 0,
        _ => 1,
    }
}

// ---------------------------------------------------------------------------
// Why the websocket connect did not happen
// ---------------------------------------------------------------------------
//
// Each step between `authorized` and `signalling` (DNS, TCP, TLS, HTTP
// status, websocket upgrade) gets its own category, since the error text
// can embed the URL with the JWT and is never logged; the category is all a
// remote report carries. Only the error's shape and a status code are read,
// both closed sets.

/// The category for one refused host resolution.
fn resolve_refusal_category(refusal: HostRefusal) -> &'static str {
    match refusal {
        // Local-only names: nothing to look up or retry.
        HostRefusal::PrivateName => "focus_private_name",
        HostRefusal::Unresolved => "focus_unresolved",
        // The case the `focus_unroutable` wording is true of, so it keeps it.
        HostRefusal::NonPublicAddress => "focus_unroutable",
    }
}

/// The category for one transport-level failure reaching the SFU.
fn classify_io_error(err: &std::io::Error) -> &'static str {
    use std::io::ErrorKind as Kind;
    match err.kind() {
        // The address answered and refused: nothing listening.
        Kind::ConnectionRefused => "sfu_refused_connection",
        // No route: typical of an AAAA record without working IPv6.
        Kind::HostUnreachable | Kind::NetworkUnreachable => "sfu_unreachable",
        Kind::TimedOut => "connect_timeout",
        Kind::PermissionDenied => "connect_blocked",
        Kind::ConnectionReset
        | Kind::ConnectionAborted
        | Kind::BrokenPipe
        | Kind::NotConnected
        | Kind::UnexpectedEof => "connection_lost",
        _ => "transport_failed",
    }
}

/// What one walk over a host's approved addresses ended in: connected, the
/// shared budget ran out, or every address failed (category from the last
/// error). Kept apart so a timeout and a dead SFU are reported differently.
enum ConnectWalk<S> {
    Connected(S),
    /// Every address was tried and failed. Carries the last error, or `None`
    /// for an empty list (not currently possible; handled anyway).
    Failed(Option<tokio_tungstenite::tungstenite::Error>),
    /// The shared budget expired mid-attempt.
    TimedOut,
}

/// Try each approved address in turn under one shared deadline.
///
/// Resolvers order addresses differently (macOS offers AAAA first more
/// readily than glibc's RFC 6724 sort), and a host with AAAA but no working
/// IPv6 fails on the first address while its A record would connect.
/// Browsers and LiveKit clients walk the list too.
///
///   * The budget is shared (`timeout_at` against the caller's deadline), so
///     N addresses never cost N timeouts.
///   * Only transport failures move on. An HTTP status, TLS alert or refused
///     upgrade is the server answering, the same at every address.
///
/// The connect step is a parameter so the walk is testable offline.
async fn walk_addresses<S, F, Fut>(
    addresses: Vec<std::net::SocketAddr>,
    deadline: tokio::time::Instant,
    mut attempt: F,
) -> ConnectWalk<S>
where
    F: FnMut(std::net::SocketAddr) -> Fut,
    Fut: std::future::Future<
        Output = Result<S, tokio_tungstenite::tungstenite::Error>,
    >,
{
    let mut last = None;
    for address in addresses {
        match tokio::time::timeout_at(deadline, attempt(address)).await {
            Ok(Ok(stream)) => return ConnectWalk::Connected(stream),
            Ok(Err(err)) => {
                let transport = matches!(
                    err,
                    tokio_tungstenite::tungstenite::Error::Io(_));
                last = Some(err);
                if !transport {
                    break;
                }
            }
            Err(_) => return ConnectWalk::TimedOut,
        }
    }
    ConnectWalk::Failed(last)
}

/// Category for one failed websocket connect. Closed set; no error text.
fn classify_ws_error(
    err: &tokio_tungstenite::tungstenite::Error,
) -> &'static str {
    use tokio_tungstenite::tungstenite::Error as Ws;
    match err {
        Ws::Io(io) => classify_io_error(io),
        // rustls rejected the peer. Roots are compiled in (webpki-roots), so this
        // concerns the server's certificate or protocol, not the local store.
        Ws::Tls(_) => "tls_failed",
        // The SFU answered HTTP instead of upgrading; the status is the diagnosis.
        // The body is never read.
        Ws::Http(response) => match response.status().as_u16() {
            401 | 403 => "sfu_forbidden",
            404 => "sfu_not_found",
            429 => "rate_limited",
            500..=599 => "server_error",
            _ => "ws_rejected",
        },
        // Not a websocket upgrade: usually a proxy or captive portal.
        Ws::Protocol(_) | Ws::HttpFormat(_) | Ws::Utf8 | Ws::AttackAttempt => {
            "ws_handshake_failed"
        }
        Ws::Url(_) => "focus_url_invalid",
        Ws::Capacity(_) => "ws_frame_too_large",
        Ws::ConnectionClosed | Ws::AlreadyClosed => "connection_lost",
        // Non-exhaustive enum: unmapped shapes keep the generic word.
        _ => "connect_failed",
    }
}

// ---------------------------------------------------------------------------
// JWT service
// ---------------------------------------------------------------------------

/// What `POST {service_url}/sfu/get` answered.
#[derive(Clone, Debug)]
pub(crate) struct SfuCredentials {
    /// The SFU's websocket URL (`wss://…`), not the JWT service URL.
    pub url: String,
    /// Short-lived SFU authorization. Never logged, never crosses the FFI.
    pub jwt: String,
}

/// Normalize the SFU websocket URL from the JWT service to the one shape
/// the connect accepts, or `None`.
///
/// `https` is normalized to `wss` (lk-jwt-service echoes LIVEKIT_URL, often
/// `https://…`; livekit-client converts it the same way). `ws`/`http` would
/// send the JWT in the clear and are refused. The host must be non-empty
/// (`has_host()` accepts an empty one) and public: the URL is chosen by the
/// focus, which another participant chose. Tests call this function.
pub(crate) fn normalize_sfu_url(raw: &str) -> Option<String> {
    let mut parsed = url::Url::parse(raw).ok()?;
    match parsed.scheme() {
        "wss" => {}
        // https -> wss is an allowed scheme change.
        "https" => parsed.set_scheme("wss").ok()?,
        _ => return None,
    }
    if !parsed.host_str().is_some_and(|host| !host.is_empty())
        || !public_ws_host(&parsed)
    {
        return None;
    }
    Some(parsed.to_string())
}

/// The literal-host half of the SFU websocket policy; the resolved half is
/// `rtc::resolve_public_host` at connect time.
fn public_ws_host(url: &url::Url) -> bool {
    match url.host() {
        Some(url::Host::Ipv4(addr)) => public_ip(std::net::IpAddr::V4(addr)),
        Some(url::Host::Ipv6(addr)) => public_ip(std::net::IpAddr::V6(addr)),
        Some(url::Host::Domain(name)) => public_hostname(name),
        None => false,
    }
}

/// Obtain SFU credentials for one room. The homeserver-minted OpenID token
/// lets the JWT service verify the user without a Matrix access token.
async fn fetch_sfu_credentials(
    client: &matrix_sdk::Client,
    service_url: &str,
    room_id: &str,
    device_id: &str,
) -> Result<SfuCredentials, String> {
    let user_id = client
        .user_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_owned();

    // Scoped, short-lived; not a credential for anything else.
    let openid = client
        .send(request_openid_token::v3::Request::new(user_id))
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;

    let body = serde_json::to_vec(&json!({
        "room": room_id,
        "openid_token": {
            "access_token": openid.access_token,
            "token_type": openid.token_type.to_string(),
            "matrix_server_name": openid.matrix_server_name.to_string(),
            "expires_in": openid.expires_in.as_secs(),
        },
        "device_id": device_id,
    }))
    .map_err(|_| "invalid_request".to_owned())?;

    // The service URL was validated as https with a routable host at parse
    // time (rtc::sane_https_url); join the documented path without letting a
    // crafted URL escape.
    let mut url = url::Url::parse(service_url)
        .map_err(|_| "invalid_transport".to_owned())?;
    url.path_segments_mut()
        .map_err(|_| "invalid_transport".to_owned())?
        .pop_if_empty()
        .extend(["sfu", "get"]);

    // The focus host is chosen by another participant, and this request
    // carries the user's OpenID token, device id and room id, so it uses a
    // dedicated client:
    //   * the name is resolved here, all addresses must be public, and the
    //     first is pinned, so a private record or rebinding cannot reach the
    //     loopback;
    //   * redirects are refused (the SDK client would follow cross-host and
    //     replay the body, e.g. to http://169.254.169.254/);
    //   * the environment proxy is ignored, or the pin would be advisory;
    //   * https only, with the SDK client's timeouts.
    let host = url
        .host_str()
        .map(ToOwned::to_owned)
        .ok_or_else(|| "invalid_transport".to_owned())?;
    let port = url.port_or_known_default().unwrap_or(443);
    // Resolution is bounded too, so a silent resolver cannot hold the join.
    let pinned = tokio::time::timeout(SFU_TIMEOUT, resolve_public_host(&host, port))
        .await
        .ok()
        .flatten()
        .ok_or_else(|| "invalid_transport".to_owned())?;
    let http = reqwest::Client::builder()
        .https_only(true)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .resolve(&host, pinned)
        .connect_timeout(Duration::from_secs(5))
        .timeout(SFU_TIMEOUT)
        .user_agent(format!("Lightning/{}", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|_| "invalid_request".to_owned())?;

    let response = http
        .post(url.to_string())
        .header(reqwest::header::CONTENT_TYPE, "application/json")
        .body(body)
        .send()
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;

    let status = response.status().as_u16();
    if !(200..300).contains(&status) {
        return Err(match status {
            // Redirects are refused above, so a 3xx means misconfigured or hostile.
            300..=399 => "invalid".to_owned(),
            401 | 403 => "forbidden".to_owned(),
            404 => "unsupported".to_owned(),
            429 => "rate_limited".to_owned(),
            500..=599 => "server_error".to_owned(),
            _ => "unknown".to_owned(),
        });
    }
    // Refuse an oversized answer before reading, and stop at the cap without a
    // Content-Length: `text()` buffers everything first.
    if response
        .content_length()
        .is_some_and(|len| len > MAX_SFU_RESPONSE as u64)
    {
        return Err("invalid".to_owned());
    }
    let mut bytes = Vec::with_capacity(1024);
    let mut stream = response;
    while let Some(chunk) = stream
        .chunk()
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?
    {
        if bytes.len() + chunk.len() > MAX_SFU_RESPONSE {
            return Err("invalid".to_owned());
        }
        bytes.extend_from_slice(&chunk);
    }
    let text = String::from_utf8_lossy(&bytes);
    let parsed: serde_json::Value =
        serde_json::from_str(&text).map_err(|_| "invalid".to_owned())?;

    // The SFU URL carries the JWT in its query string, so cleartext is refused;
    // `https` is normalized to `wss` (see normalize_sfu_url).
    let raw_url = parsed.get("url").and_then(|v| v.as_str()).unwrap_or("");
    let url_ok = sane(raw_url, 1024)
        .and_then(normalize_sfu_url)
        .ok_or_else(|| "invalid".to_owned())?;
    let jwt = parsed
        .get("jwt")
        .and_then(|v| v.as_str())
        .and_then(|value| sane(value, 8192))
        .ok_or_else(|| "invalid".to_owned())?
        .to_owned();

    Ok(SfuCredentials { url: url_ok, jwt })
}

// ---------------------------------------------------------------------------
// Live session
// ---------------------------------------------------------------------------

/// How long `disconnect` lets the session drain its queued Leave and close
/// the websocket before the abort backstop.
///
/// Derived from `crate::SHUTDOWN_ACTION_JOIN_MS`, the budget that joins
/// this drain at teardown: it must finish inside that, or the teardown
/// aborts the waiter first and the Leave never goes out, leaving a ghost
/// participant for peers. The margin lets the pool abort cleanly.
const LEAVE_FLUSH_MARGIN_MS: u64 = 250;
const LEAVE_FLUSH_TIMEOUT: Duration =
    Duration::from_millis(crate::SHUTDOWN_ACTION_JOIN_MS - LEAVE_FLUSH_MARGIN_MS);

const _: () = assert!(
    LEAVE_FLUSH_MARGIN_MS < crate::SHUTDOWN_ACTION_JOIN_MS,
    "SHUTDOWN_ACTION_JOIN_MS no longer leaves room for the SFU leave drain; \
     a graceful LiveKit Leave would be aborted before it is sent, stranding \
     a participant in every call the user quits out of."
);

/// Commands the C++ side sends into a running signalling session.
#[derive(Debug)]
pub(crate) enum SfuCommand {
    /// A local description for one peer connection.
    Offer { sdp: String, target: i32 },
    Answer { sdp: String, target: i32 },
    /// One trickled local ICE candidate (`candidate_init` in LiveKit's JSON
    /// form).
    Candidate { candidate_init: String, target: i32 },
    /// Declare a track before publishing it.
    AddTrack {
        cid: String,
        name: String,
        /// 0 = audio, 1 = video
        kind: i32,
        /// Video only, 0 for audio. A video track declared without size and layer
        /// makes the SFU assume simulcast.
        width: u32,
        height: u32,
        /// True for a screen share, which LiveKit sources separately.
        screen_share: bool,
        /// Whether this track's frames are E2EE-encrypted. Declared per track;
        /// receivers use it to decide whether to decrypt.
        encrypted: bool,
    },
    /// Mute/unmute a published track at the SFU so others see the state (the
    /// valve already stopped the bytes).
    MuteTrack { sid: String, muted: bool },
    Leave,
}

/// The one live signalling session, guarded by a generation.
pub(crate) struct SfuSession {
    pub generation: u64,
    pub commands: tokio::sync::mpsc::UnboundedSender<SfuCommand>,
    pub task: tokio::task::JoinHandle<()>,
}

pub(crate) struct SfuState {
    pub session: Mutex<Option<SfuSession>>,
    pub generation: AtomicU64,
}

impl Default for SfuState {
    fn default() -> Self {
        Self { session: Mutex::new(None), generation: AtomicU64::new(0) }
    }
}

fn participant_json(info: &lkp::ParticipantInfo) -> Option<serde_json::Value> {
    // `identity` maps an SFU participant to a Matrix device (MatrixRTC sets it
    // to the membership's rtc identity).
    let identity = sane(&info.identity, 512)?;
    let tracks: Vec<serde_json::Value> = info
        .tracks
        .iter()
        .take(MAX_TRACKS_PER_PARTICIPANT)
        .filter_map(|track| {
            let sid = sane(&track.sid, 256)?;
            Some(json!({
                "sid": sid,
                // Closed set, never the raw wire enum.
                "kind": if track.r#type == 1 { "video" } else { "audio" },
                // Named constants, not literals (see track_source_for).
                "source": match lkp::TrackSource::try_from(track.source) {
                    Ok(lkp::TrackSource::Camera) => "camera",
                    Ok(lkp::TrackSource::Microphone) => "microphone",
                    Ok(lkp::TrackSource::ScreenShare) => "screen_share",
                    Ok(lkp::TrackSource::ScreenShareAudio)
                        => "screen_share_audio",
                    _ => "unknown",
                },
                "muted": track.muted,
                // The media-section id this track was negotiated on: the subscriber SDP's
                // `a=mid:` names exactly this, so a receiver can tell a camera from a
                // screen share. Bounded.
                "mid": sane(&track.mid, 128).unwrap_or_default(),
                // LiveKit's stream id for the track, when stated. Fallback key only.
                "stream": sane(&track.stream, 256).unwrap_or_default(),
            }))
        })
        .collect();
    Some(json!({
        "identity": identity,
        "sid": sane(&info.sid, 256).unwrap_or_default(),
        "state": match info.state {
            0 => "joining",
            1 => "joined",
            2 => "active",
            _ => "disconnected",
        },
        "tracks": tracks,
    }))
}

/// Encode and send one signalling request; `false` means the socket is
/// gone.
async fn send_request<S>(
    sink: &mut S,
    message: lkp::signal_request::Message,
) -> bool
where
    S: SinkExt<WsMessage> + Unpin,
{
    let request = lkp::SignalRequest { message: Some(message) };
    let mut buffer = Vec::with_capacity(256);
    if request.encode(&mut buffer).is_err() {
        return false;
    }
    sink.send(WsMessage::Binary(buffer)).await.is_ok()
}

/// Run one signalling session to completion. Every enqueue checks the
/// session generation, so nothing reaches a later call.
#[allow(clippy::too_many_arguments)]
async fn run_session(
    events: Arc<Mutex<std::collections::VecDeque<String>>>,
    generation: u64,
    generation_now: Arc<AtomicU64>,
    media_capable: Arc<AtomicBool>,
    credentials: SfuCredentials,
    mut commands: tokio::sync::mpsc::UnboundedReceiver<SfuCommand>,
) {
    let current = {
        let generation_now = Arc::clone(&generation_now);
        move || generation_now.load(Ordering::SeqCst) == generation
    };
    let emit = {
        let events = Arc::clone(&events);
        let current = current.clone();
        move |value: serde_json::Value| {
            if current() {
                enqueue(&events, value);
            }
        }
    };

    // LiveKit takes the JWT in the signalling URL's query string. The JWT is
    // short-lived, the connection wss, and the URL never logged or enqueued.
    let mut url = match url::Url::parse(&credentials.url) {
        Ok(url) => url,
        Err(_) => {
            // Its own category: the SFU URL is unparseable, reachable only if
            // normalize_sfu_url accepted something this cannot re-parse.
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "focus_url_invalid",
            }));
            return;
        }
    };
    // Append `/rtc` to the existing path rather than replacing it, so a
    // LiveKit behind a reverse proxy prefix still works.
    {
        let existing = url.path().trim_end_matches('/').to_owned();
        url.set_path(&format!("{existing}/rtc"));
    }
    url.query_pairs_mut()
        .append_pair("access_token", &credentials.jwt)
        .append_pair("protocol", &LK_PROTOCOL_VERSION.to_string())
        .append_pair("auto_subscribe", "1")
        .append_pair("sdk", "cpp")
        .append_pair("version", env!("CARGO_PKG_VERSION"));

    // Resolve, check every address, and connect the TCP stream ourselves so the
    // socket goes to the approved address (TLS still verifies the hostname).
    // Frame ceilings go in the websocket config; a check after receipt would
    // only see what tungstenite already buffered (up to 64 MiB).
    //
    // Resolution has its own category: a focus resolving to a private,
    // loopback or link-local address is refused by policy (docs/matrixrtc.md),
    // and that deserves a clearer reason than "network down".
    let host = url.host_str().unwrap_or_default().to_owned();
    let port = url.port_or_known_default().unwrap_or(443);
    let addresses = match tokio::time::timeout(
        SFU_TIMEOUT,
        resolve_public_hosts(&host, port),
    )
    .await
    {
        Ok(Ok(addresses)) => addresses,
        Ok(Err(refusal)) => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed",
                "category": resolve_refusal_category(refusal),
            }));
            return;
        }
        Err(_) => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "focus_resolve_timeout",
            }));
            return;
        }
    };
    // Every approved address, in resolver order, under one budget (see
    // `walk_addresses`). `target` is borrowed so the URL, which carries the
    // JWT, is not cloned per attempt.
    let target = &url;
    let stream = match walk_addresses(
        addresses,
        tokio::time::Instant::now() + SFU_TIMEOUT,
        move |address| async move {
            let tcp = tokio::net::TcpStream::connect(address)
                .await
                .map_err(tokio_tungstenite::tungstenite::Error::Io)?;
            let mut config =
                tokio_tungstenite::tungstenite::protocol::WebSocketConfig::default();
            config.max_message_size = Some(MAX_SIGNAL_FRAME);
            config.max_frame_size = Some(MAX_SIGNAL_FRAME);
            tokio_tungstenite::client_async_tls_with_config(
                target.as_str(),
                tcp,
                Some(config),
                None,
            )
            .await
        },
    )
    .await
    {
        ConnectWalk::Connected((stream, _)) => stream,
        ConnectWalk::TimedOut => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "connect_timeout",
            }));
            return;
        }
        ConnectWalk::Failed(err) => {
            // The error string can embed the URL (and JWT); only a closed-set category
            // leaves this scope.
            let category = err
                .as_ref()
                .map(classify_ws_error)
                .unwrap_or("connect_failed");
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": category,
            }));
            return;
        }
    };

    let (mut sink, mut source) = stream.split();
    emit(json!({
        "type": "sfu_state", "generation": generation, "state": "signalling",
        "category": "",
    }));

    // LiveKit's application-level keepalive (not websocket ping/pong): the
    // server disconnects clients that stop sending `ping`/`ping_req` signals.
    // The interval comes from JoinResponse; until then the ticker is parked.
    // Both the deprecated `ping` and `ping_req` are sent, as livekit-client
    // does.
    let mut ping_ticker =
        tokio::time::interval(std::time::Duration::from_secs(3600));
    ping_ticker.set_missed_tick_behavior(
        tokio::time::MissedTickBehavior::Delay);
    // A tokio interval ticks immediately; consume it so nothing is sent before
    // the join.
    ping_ticker.tick().await;
    let mut ping_armed = false;
    // LiveKit matches an answer to its offer by `SessionDescription.id`, not
    // the peer-connection target. Server offer ids start from a random base, so
    // echo the last remote offer id back and number our own offers with a
    // counter, as livekit-client does. Index 0 = publisher, 1 = subscriber.
    let mut remote_offer_id = [0u32; 2];
    let mut local_offer_id = [0u32; 2];

    loop {
        tokio::select! {
            _ = ping_ticker.tick(), if ping_armed => {
                let now_ms = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_millis() as i64)
                    .unwrap_or(0);
                // Write failure: the socket is gone, stop.
                if !send_request(
                    &mut sink,
                    lkp::signal_request::Message::Ping(now_ms)).await
                {
                    break;
                }
                if !send_request(
                    &mut sink,
                    lkp::signal_request::Message::PingReq(lkp::Ping {
                        timestamp: now_ms,
                        rtt: 0,
                    })).await
                {
                    break;
                }
            }
            command = commands.recv() => {
                let Some(command) = command else { break };
                let message = match command {
                    SfuCommand::Offer { sdp, target } => {
                        let slot = (target as usize).min(1);
                        local_offer_id[slot] =
                            local_offer_id[slot].wrapping_add(1).max(1);
                        lkp::signal_request::Message::Offer(
                            lkp::SessionDescription {
                                r#type: "offer".to_owned(), sdp,
                                id: local_offer_id[slot],
                                ..Default::default()
                            })
                    }
                    SfuCommand::Answer { sdp, target } => {
                        let slot = (target as usize).min(1);
                        lkp::signal_request::Message::Answer(
                            lkp::SessionDescription {
                                r#type: "answer".to_owned(), sdp,
                                // The id of the offer being answered; 0 means none seen, which the
                                // server treats as unset.
                                id: remote_offer_id[slot],
                                ..Default::default()
                            })
                    }
                    SfuCommand::Candidate { candidate_init, target } => {
                        lkp::signal_request::Message::Trickle(
                            lkp::TrickleRequest {
                                candidate_init, target, r#final: false,
                            })
                    }
                    SfuCommand::AddTrack {
                        cid, name, kind, width, height, screen_share, encrypted,
                    } => {
                        // One explicit video layer with real dimensions, as livekit-client does
                        // (the proto: single-layer tracks should use HIGH). Without it the SFU
                        // assumes three-layer simulcast while we send a single stream.
                        let layers = if kind == lkp::TrackType::Video as i32 {
                            vec![lkp::VideoLayer {
                                quality: lkp::VideoQuality::High as i32,
                                width,
                                height,
                                ..Default::default()
                            }]
                        } else {
                            Vec::new()
                        };
                        lkp::signal_request::Message::AddTrack(
                            lkp::AddTrackRequest {
                                cid, name, r#type: kind,
                                width, height, layers,
                                source: track_source_for(kind, screen_share),
                                encryption: track_encryption_for(encrypted),
                                // RED and frame E2EE are mutually exclusive (livekit-client:
                                // `disableRed: this.isE2EEEnabled || …`): RED wraps the Opus payload, so
                                // the decryptor gets a RED packet instead of the clear Opus TOC byte, and
                                // every frame fails authentication.
                                disable_red: encrypted,
                                ..Default::default()
                            })
                    }
                    SfuCommand::MuteTrack { sid, muted } => {
                        lkp::signal_request::Message::Mute(
                            lkp::MuteTrackRequest { sid, muted })
                    }
                    SfuCommand::Leave => {
                        let _ = send_request(
                            &mut sink,
                            lkp::signal_request::Message::Leave(
                                lkp::LeaveRequest::default())).await;
                        break;
                    }
                };
                if !send_request(&mut sink, message).await {
                    emit(json!({
                        "type": "sfu_state", "generation": generation,
                        "state": "failed", "category": "send_failed",
                    }));
                    break;
                }
            }
            frame = source.next() => {
                let Some(frame) = frame else { break };
                let payload = match frame {
                    Ok(WsMessage::Binary(bytes)) => bytes,
                    Ok(WsMessage::Close(_)) => break,
                    Ok(WsMessage::Ping(_)) | Ok(WsMessage::Pong(_))
                    | Ok(WsMessage::Text(_)) | Ok(WsMessage::Frame(_)) => continue,
                    Err(_) => {
                        emit(json!({
                            "type": "sfu_state", "generation": generation,
                            "state": "failed", "category": "connection_lost",
                        }));
                        break;
                    }
                };
                if payload.len() > MAX_SIGNAL_FRAME {
                    continue; // remote party; bounded, dropped, not fatal
                }
                let Ok(response) = lkp::SignalResponse::decode(&payload[..])
                else { continue };
                let Some(message) = response.message else { continue };

                match message {
                    lkp::signal_response::Message::Join(join) => {
                        // Arm the keepalive at the server's interval, clamped (0 would spin; a huge
                        // value is no keepalive).
                        let interval = join.ping_interval.clamp(1, 120) as u64;
                        ping_ticker = tokio::time::interval(
                            std::time::Duration::from_secs(interval));
                        ping_ticker.set_missed_tick_behavior(
                            tokio::time::MissedTickBehavior::Delay);
                        ping_ticker.tick().await;   // consume the immediate one
                        ping_armed = true;
                        // Our own row first, then the others. JoinResponse keeps the local
                        // participant separate; without it in the list the stage cannot draw the
                        // local tile, and `ownParticipantRow()` (used to send mutes) finds nothing.
                        let mut participants: Vec<serde_json::Value> =
                            Vec::with_capacity(MAX_PARTICIPANTS);
                        if let Some(own) = join.participant.as_ref()
                            .and_then(participant_json)
                        {
                            participants.push(own);
                        }
                        participants.extend(join
                            .other_participants.iter()
                            .take(MAX_PARTICIPANTS.saturating_sub(1))
                            .filter_map(participant_json));
                        // ICE servers from the SFU; their credentials are short-lived and
                        // engine-only, like the homeserver's TURN answer.
                        let ice: Vec<serde_json::Value> = join.ice_servers
                            .iter().take(8).map(|server| json!({
                                "urls": server.urls.iter().take(8)
                                    .filter_map(|u| sane(u, 512))
                                    .collect::<Vec<_>>(),
                                "username": server.username,
                                "credential": server.credential,
                            })).collect();
                        emit(json!({
                            "type": "sfu_joined",
                            "generation": generation,
                            "identity": join.participant.as_ref()
                                .map(|p| p.identity.clone())
                                .unwrap_or_default(),
                            "subscriber_primary": join.subscriber_primary,
                            "participants": participants,
                            "ice_servers": ice,
                            "sif_trailer": sif_trailer_b64(&join.sif_trailer),
                        }));
                    }
                    lkp::signal_response::Message::Offer(sdp) => {
                        // The server offers on subscriber (everyone else's media). Keep its id for
                        // the answer.
                        remote_offer_id[1] = sdp.id;
                        if media_capable.load(Ordering::SeqCst) {
                            emit(json!({
                                "type": "sfu_remote_description",
                                "generation": generation,
                                "kind": "offer",
                                "target": target_str(1),
                                "sdp": sdp.sdp,
                            }));
                        }
                    }
                    lkp::signal_response::Message::Answer(sdp) => {
                        if media_capable.load(Ordering::SeqCst) {
                            emit(json!({
                                "type": "sfu_remote_description",
                                "generation": generation,
                                "kind": "answer",
                                "target": target_str(0),
                                "sdp": sdp.sdp,
                            }));
                        }
                    }
                    lkp::signal_response::Message::Trickle(trickle) => {
                        if media_capable.load(Ordering::SeqCst) {
                            if let Some(init) =
                                sane(&trickle.candidate_init, 4096)
                            {
                                emit(json!({
                                    "type": "sfu_remote_candidate",
                                    "generation": generation,
                                    "target": target_str(trickle.target),
                                    "candidate_init": init,
                                }));
                            }
                        }
                    }
                    lkp::signal_response::Message::Update(update) => {
                        let participants: Vec<serde_json::Value> = update
                            .participants.iter().take(MAX_PARTICIPANTS)
                            .filter_map(participant_json).collect();
                        emit(json!({
                            "type": "sfu_participants",
                            "generation": generation,
                            "participants": participants,
                        }));
                    }
                    lkp::signal_response::Message::TrackPublished(published) => {
                        emit(json!({
                            "type": "sfu_track_published",
                            "generation": generation,
                            "cid": sane(&published.cid, 256)
                                .unwrap_or_default(),
                            "sid": published.track.as_ref()
                                .and_then(|t| sane(&t.sid, 256))
                                .unwrap_or_default(),
                        }));
                    }
                    lkp::signal_response::Message::SpeakersChanged(speakers) => {
                        let active: Vec<serde_json::Value> = speakers
                            .speakers.iter().take(MAX_PARTICIPANTS)
                            .filter_map(|speaker| {
                                let sid = sane(&speaker.sid, 256)?;
                                Some(json!({
                                    "sid": sid,
                                    "active": speaker.active,
                                    // 0.0..1.0; presentation-safe.
                                    "level": speaker.level,
                                }))
                            }).collect();
                        emit(json!({
                            "type": "sfu_speakers",
                            "generation": generation,
                            "speakers": active,
                        }));
                    }
                    lkp::signal_response::Message::ConnectionQuality(quality) => {
                        let updates: Vec<serde_json::Value> = quality
                            .updates.iter().take(MAX_PARTICIPANTS)
                            .filter_map(|update| {
                                let identity =
                                    sane(&update.participant_sid, 256)?;
                                Some(json!({
                                    "sid": identity,
                                    "quality": match update.quality {
                                        0 => "poor",
                                        1 => "good",
                                        2 => "excellent",
                                        _ => "unknown",
                                    },
                                }))
                            }).collect();
                        emit(json!({
                            "type": "sfu_quality",
                            "generation": generation,
                            "updates": updates,
                        }));
                    }
                    lkp::signal_response::Message::Leave(leave) => {
                        // Include the reason (a closed enum, not content); "told to leave" alone is
                        // unactionable.
                        emit(json!({
                            "type": "sfu_state", "generation": generation,
                            "state": "ended", "category": "server_leave",
                            "reason": leave.reason,
                            "action": leave.action,
                        }));
                        break;
                    }
                    lkp::signal_response::Message::Mute(mute) => {
                        emit(json!({
                            "type": "sfu_server_mute",
                            "generation": generation,
                            "sid": sane(&mute.sid, 256).unwrap_or_default(),
                            "muted": mute.muted,
                        }));
                    }
                    _ => {}
                }
            }
        }
    }

    let _ = sink.close().await;
    emit(json!({
        "type": "sfu_state", "generation": generation, "state": "closed",
        "category": "",
    }));
}

/// Connect to the SFU at `service_url` for `room_id`. Tears down any
/// existing session first (one call at a time); the generation bump stops
/// the old session's reports immediately.
pub(crate) fn connect(
    bridge: &RustClient,
    service_url: String,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let device_id = client
        .device_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_string();

    disconnect(bridge);

    let generation = bridge.sfu.generation.fetch_add(1, Ordering::SeqCst) + 1;
    let events = Arc::clone(&bridge.events);
    let media_capable = Arc::clone(&bridge.call_media_capable);
    let generation_now = Arc::clone(&bridge.sfu_generation);
    generation_now.store(generation, Ordering::SeqCst);
    let (tx, rx) = tokio::sync::mpsc::unbounded_channel();

    let task = bridge.runtime.spawn(async move {
        match fetch_sfu_credentials(&client, &service_url, &room_id, &device_id)
            .await
        {
            Ok(credentials) => {
                if generation_now.load(Ordering::SeqCst) != generation {
                    return;
                }
                enqueue(&events, json!({
                    "type": "sfu_state", "generation": generation,
                    "op_id": op_id, "state": "authorized", "category": "",
                }));
                run_session(events, generation, generation_now, media_capable,
                            credentials, rx)
                    .await;
            }
            Err(category) => {
                if generation_now.load(Ordering::SeqCst) != generation {
                    return;
                }
                enqueue(&events, json!({
                    "type": "sfu_state", "generation": generation,
                    "op_id": op_id, "state": "failed", "category": category,
                }));
            }
        }
    });

    if let Ok(mut guard) = bridge.sfu.session.lock() {
        *guard = Some(SfuSession { generation, commands: tx, task });
    }
    Ok(())
}

/// Send one command into the live session; ignored when there is none.
pub(crate) fn send_command(bridge: &RustClient, command: SfuCommand) {
    if let Ok(guard) = bridge.sfu.session.lock() {
        if let Some(session) = guard.as_ref() {
            let _ = session.commands.send(command);
        }
    }
}

/// Tear down the live session. The generation is bumped first, so a task
/// mid-await cannot report into the next call.
pub(crate) fn disconnect(bridge: &RustClient) {
    bridge.sfu_generation.fetch_add(1, Ordering::SeqCst);
    let session = bridge.sfu.session.lock().ok().and_then(|mut g| g.take());
    if let Some(session) = session {
        let SfuSession { commands, task, .. } = session;
        let _ = commands.send(SfuCommand::Leave);

        // Let the Leave reach the wire. `commands.send` only queues; aborting
        // immediately cancelled the task with the Leave unread, so the SFU kept the
        // participant until its own timeout and every rejoin added another copy.
        // Give the task a bounded window to drain, keeping abort as the backstop
        // for a dead connection.
        //
        //  * `abort_handle()` is taken before the JoinHandle moves into the
        //    timeout: a timed-out `timeout(d, handle)` drops the handle, which
        //    detaches the task instead of cancelling it.
        //  * `commands` is kept alive for the window, so sender drop does not race
        //    the queued Leave.
        let abort = task.abort_handle();
        bridge.spawn_room_action(async move {
            let _commands = commands;
            if tokio::time::timeout(LEAVE_FLUSH_TIMEOUT, task).await.is_err() {
                abort.abort();
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The server-injected-frame trailer crosses the bridge, bounded.
    // Round-trips a real JoinResponse through prost so the field itself is
    // exercised.
    #[test]
    fn sif_trailer_is_forwarded_base64_and_bounded() {
        use base64::Engine;
        let b64 = base64::engine::general_purpose::STANDARD;

        // A livekit-shaped trailer: ~43 base62 characters.
        let trailer = b"6N9Qc1wR2vXz8KkLmP0aBcDeFgHiJkLmNoPqRsTuVwX".to_vec();
        let join = lkp::JoinResponse {
            sif_trailer: trailer.clone(),
            ..Default::default()
        };
        let decoded = lkp::JoinResponse::decode(&join.encode_to_vec()[..])
            .expect("a JoinResponse round-trips");
        let wire = sif_trailer_b64(&decoded.sif_trailer);
        assert_eq!(b64.decode(&wire).expect("valid base64"), trailer);

        // Absent: nothing armed.
        assert_eq!(sif_trailer_b64(&[]), "");
        // Exactly at the bound: forwarded whole.
        let at_bound = vec![b'A'; MAX_SIF_TRAILER];
        assert_eq!(b64.decode(sif_trailer_b64(&at_bound)).unwrap(), at_bound);
        // One over: dropped, never truncated.
        assert_eq!(sif_trailer_b64(&vec![b'A'; MAX_SIF_TRAILER + 1]), "");
        // Too short, or not base62: dropped.
        assert_eq!(sif_trailer_b64(b"R"), "");
        assert_eq!(sif_trailer_b64(b"\x0cR"), "");
        assert_eq!(sif_trailer_b64(&vec![b'A'; MIN_SIF_TRAILER - 1]), "");
        let mut not_base62 = trailer.clone();
        not_base62[3] = b'+';
        assert_eq!(sif_trailer_b64(&not_base62), "");
    }

    // Each failure between `authorized` and `signalling` gets a distinct
    // category, and none is derived from an error message (which could leak
    // the JWT).
    #[test]
    fn every_connect_failure_shape_has_its_own_category() {
        use tokio_tungstenite::tungstenite::Error as Ws;
        use std::io::ErrorKind as Kind;

        let io = |kind: Kind| {
            classify_ws_error(&Ws::Io(std::io::Error::new(kind, "x")))
        };
        // No route (AAAA without IPv6) must not read as a refusal.
        assert_eq!(io(Kind::NetworkUnreachable), "sfu_unreachable");
        assert_eq!(io(Kind::HostUnreachable), "sfu_unreachable");
        assert_eq!(io(Kind::ConnectionRefused), "sfu_refused_connection");
        assert_eq!(io(Kind::TimedOut), "connect_timeout");
        assert_eq!(io(Kind::PermissionDenied), "connect_blocked");
        assert_eq!(io(Kind::ConnectionReset), "connection_lost");
        // An unmapped socket error is a transport failure; `connect_failed` means
        // an unknown websocket error shape.
        assert_eq!(io(Kind::InvalidData), "transport_failed");

        let http = |status: u16| {
            let response =
                tokio_tungstenite::tungstenite::http::Response::builder()
                .status(status)
                .body(None::<Vec<u8>>)
                .expect("a status-only response");
            classify_ws_error(&Ws::Http(response))
        };
        assert_eq!(http(401), "sfu_forbidden");
        assert_eq!(http(403), "sfu_forbidden");
        assert_eq!(http(404), "sfu_not_found");
        assert_eq!(http(429), "rate_limited");
        assert_eq!(http(503), "server_error");
        assert_eq!(http(418), "ws_rejected");

        // A proxy or captive portal: the remedy is on the network, not the SFU.
        assert_eq!(
            classify_ws_error(&Ws::Protocol(
                tokio_tungstenite::tungstenite::error::ProtocolError::
                    WrongHttpMethod)),
            "ws_handshake_failed");
        assert_eq!(
            classify_ws_error(&Ws::Capacity(
                tokio_tungstenite::tungstenite::error::CapacityError::
                    TooManyHeaders)),
            "ws_frame_too_large");
        assert_eq!(classify_ws_error(&Ws::ConnectionClosed),
                   "connection_lost");

        // Distinct causes must have distinct words.
        let distinct = [
            io(Kind::NetworkUnreachable),
            io(Kind::ConnectionRefused),
            io(Kind::TimedOut),
            io(Kind::PermissionDenied),
            io(Kind::InvalidData),
            http(403),
            http(404),
            http(418),
            classify_ws_error(&Ws::Tls(
                tokio_tungstenite::tungstenite::error::TlsError::
                    InvalidDnsName)),
        ];
        let mut sorted = distinct.to_vec();
        sorted.sort_unstable();
        let before = sorted.len();
        sorted.dedup();
        assert_eq!(sorted.len(), before,
                   "two different connect failures share one category");
    }

    // ── The address walk ─────────────────────────────────────────────────
    //
    // Affects every platform, so it is pinned: three properties, one case each.

    // 1. An unreachable first address is not the answer for the host: the
    // second address must be tried and connect (AAAA-first without IPv6).
    #[tokio::test]
    async fn an_unreachable_first_address_is_not_the_whole_host() {
        use std::sync::{Arc, Mutex};

        let v6: std::net::SocketAddr = "[2001:db8::1]:443".parse().unwrap();
        let v4: std::net::SocketAddr = "203.0.113.7:443".parse().unwrap();
        let tried = Arc::new(Mutex::new(Vec::new()));
        let seen = Arc::clone(&tried);

        let walk = walk_addresses(
            vec![v6, v4],
            tokio::time::Instant::now() + Duration::from_secs(5),
            move |address| {
                let seen = Arc::clone(&seen);
                async move {
                    seen.lock().expect("the walk is single-threaded")
                        .push(address);
                    if address.is_ipv6() {
                        return Err(tokio_tungstenite::tungstenite::Error::Io(
                            std::io::Error::new(
                                std::io::ErrorKind::NetworkUnreachable,
                                "no route")));
                    }
                    Ok(address)
                }
            },
        )
        .await;

        assert!(matches!(walk, ConnectWalk::Connected(address)
                               if address == v4),
                "the walk did not fall through to the reachable address");
        assert_eq!(*tried.lock().expect("poisoned"), vec![v6, v4],
                   "both addresses must be tried, in resolver order");
    }

    // 2. N addresses cost one budget, not N. Only a slow failure distinguishes
    // the two: each address fails slowly but legitimately (connect sits, then
    // ECONNREFUSED), which a per-address timeout lets run to completion while a
    // shared `timeout_at` cuts it at the deadline. (A hanging address returns
    // `TimedOut` either way.)
    #[tokio::test]
    async fn the_whole_address_walk_shares_one_budget() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::Arc;

        let addresses: Vec<std::net::SocketAddr> = (1..=6u8)
            .map(|i| format!("203.0.113.{i}:443").parse().unwrap())
            .collect();
        let attempts = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&attempts);
        let budget = Duration::from_millis(500);
        let per_attempt = Duration::from_millis(200);

        let started = std::time::Instant::now();
        let walk: ConnectWalk<()> = walk_addresses(
            addresses.clone(),
            tokio::time::Instant::now() + budget,
            move |_| {
                let counter = Arc::clone(&counter);
                async move {
                    counter.fetch_add(1, Ordering::SeqCst);
                    // Slow, then a retryable transport failure, so the walk moves on.
                    tokio::time::sleep(per_attempt).await;
                    Err(tokio_tungstenite::tungstenite::Error::Io(
                        std::io::Error::new(
                            std::io::ErrorKind::ConnectionRefused, "closed")))
                }
            },
        )
        .await;
        let elapsed = started.elapsed();

        // The deadline ends this, not the address list.
        assert!(matches!(walk, ConnectWalk::TimedOut),
                "six slow addresses ran to completion instead of being cut \
                 off at the shared deadline");
        assert!(attempts.load(Ordering::SeqCst) < 6,
                "every address was tried despite the budget running out");
        // 6 x 200 ms = 1200 ms with per-address budgets versus ~500 ms shared; wide
        // enough for a loaded machine, narrow enough to catch the broken shape.
        assert!(elapsed < budget + per_attempt * 2,
                "the walk took {elapsed:?} against a {budget:?} budget");

        // The hanging case is bounded by the early return rather than the
        // deadline: one attempt, then give up.
        let hangs = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&hangs);
        let walk: ConnectWalk<()> = walk_addresses(
            addresses,
            tokio::time::Instant::now() + Duration::from_millis(200),
            move |_| {
                let counter = Arc::clone(&counter);
                async move {
                    counter.fetch_add(1, Ordering::SeqCst);
                    tokio::time::sleep(Duration::from_secs(30)).await;
                    Ok(())
                }
            },
        )
        .await;
        assert!(matches!(walk, ConnectWalk::TimedOut));
        assert_eq!(hangs.load(Ordering::SeqCst), 1,
                   "an address that never answers was followed by five more");
    }

    // 3. A server refusal (HTTP status, TLS alert, refused upgrade) is not
    // retried elsewhere: it would repeat at every address and could be masked
    // by a later transient failure.
    #[tokio::test]
    async fn a_refusal_from_the_server_is_not_retried_at_another_address() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::Arc;

        for (status, expected) in
            [(403u16, "sfu_forbidden"), (404, "sfu_not_found")]
        {
            let addresses: Vec<std::net::SocketAddr> = (1..=4u8)
                .map(|i| format!("203.0.113.{i}:443").parse().unwrap())
                .collect();
            let attempts = Arc::new(AtomicUsize::new(0));
            let counter = Arc::clone(&attempts);

            let walk: ConnectWalk<()> = walk_addresses(
                addresses,
                tokio::time::Instant::now() + Duration::from_secs(5),
                move |_| {
                    let counter = Arc::clone(&counter);
                    async move {
                        counter.fetch_add(1, Ordering::SeqCst);
                        let response =
                            tokio_tungstenite::tungstenite::http::Response::
                                builder()
                                .status(status)
                                .body(None::<Vec<u8>>)
                                .expect("a status-only response");
                        Err(tokio_tungstenite::tungstenite::Error::Http(
                            response))
                    }
                },
            )
            .await;

            assert_eq!(attempts.load(Ordering::SeqCst), 1,
                       "a {status} was retried at another address");
            let ConnectWalk::Failed(Some(err)) = walk else {
                panic!("a {status} did not end the walk as a failure");
            };
            assert_eq!(classify_ws_error(&err), expected);
        }
    }

    // An unresolvable name is not a private address.
    #[test]
    fn each_host_refusal_keeps_its_own_reason() {
        assert_eq!(resolve_refusal_category(HostRefusal::PrivateName),
                   "focus_private_name");
        assert_eq!(resolve_refusal_category(HostRefusal::Unresolved),
                   "focus_unresolved");
        // The case the existing sentence is true of keeps its category.
        assert_eq!(resolve_refusal_category(HostRefusal::NonPublicAddress),
                   "focus_unroutable");
    }

    #[test]
    fn signal_targets_are_a_closed_set_and_round_trip() {
        // Publisher and subscriber must never be swapped.
        assert_eq!(target_str(0), "publisher");
        assert_eq!(target_str(1), "subscriber");
        // Unknown degrades to subscriber, never publisher (which would attach a
        // remote description to our outgoing connection).
        assert_eq!(target_str(99), "subscriber");
        assert_eq!(target_from_str("publisher"), 0);
        assert_eq!(target_from_str("subscriber"), 1);
        assert_eq!(target_from_str("nonsense"), 1);
    }

    #[test]
    fn participant_without_a_sane_identity_is_dropped() {
        // A control character in the identity drops the whole participant.
        let mut info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            sid: "PA_1".to_owned(),
            ..Default::default()
        };
        assert!(participant_json(&info).is_some());
        info.identity = "@a:x\u{0}DEVICE".to_owned();
        assert!(participant_json(&info).is_none());
        info.identity = String::new();
        assert!(participant_json(&info).is_none());
    }

    /// The production normalisation plus the connect's `/rtc` join, so the
    /// assertions hold for what fetch_sfu_credentials does.
    fn normalize_sfu_url(raw: &str) -> Option<String> {
        let normalized = super::normalize_sfu_url(raw)?;
        let mut parsed = url::Url::parse(&normalized).ok()?;
        let existing = parsed.path().trim_end_matches('/').to_owned();
        parsed.set_path(&format!("{existing}/rtc"));
        Some(parsed.to_string())
    }

    #[test]
    fn an_https_sfu_url_is_accepted_and_normalised_to_wss() {
        // lk-jwt-service echoes LIVEKIT_URL verbatim, often `https://…`; rejecting
        // it failed calls instantly.
        assert_eq!(
            normalize_sfu_url("https://livekit.example.net").as_deref(),
            Some("wss://livekit.example.net/rtc")
        );
        assert_eq!(
            normalize_sfu_url("wss://livekit.example.net").as_deref(),
            Some("wss://livekit.example.net/rtc")
        );
    }

    #[test]
    fn cleartext_signalling_is_still_refused() {
        // The JWT is in the query string: ws/http would send it in the clear.
        assert!(normalize_sfu_url("ws://livekit.example.net").is_none());
        assert!(normalize_sfu_url("http://livekit.example.net").is_none());
        assert!(normalize_sfu_url("not a url").is_none());
        // Not `wss:///rtc`, which parses with host "rtc"; a genuinely hostless
        // form must be refused.
        assert!(normalize_sfu_url("wss:").is_none());
    }

    /// The focus chooses this URL and another participant chooses the focus.
    /// Loopback, private ranges, IPv4-mapped loopback and local-only names are
    /// refused at the literal, before resolution.
    #[test]
    fn an_sfu_url_on_an_unroutable_host_is_refused() {
        for bad in [
            "wss://127.0.0.1:8443/",
            "wss://[::1]/",
            "wss://[::ffff:127.0.0.1]/",
            "wss://10.0.0.5/",
            "wss://169.254.169.254/",
            "wss://100.64.1.1/",
            "wss://localhost/",
            "wss://livekit.local/",
            "wss://sfu.internal/",
        ] {
            assert!(normalize_sfu_url(bad).is_none(), "{bad} must be refused");
        }
        assert!(normalize_sfu_url("wss://livekit.example.net").is_some());
        assert!(normalize_sfu_url("wss://").is_none());
    }

    #[test]
    fn the_rtc_path_is_appended_never_substituted() {
        // A reverse-proxy path prefix must be kept.
        assert_eq!(
            normalize_sfu_url("https://host.example.net/livekit").as_deref(),
            Some("wss://host.example.net/livekit/rtc")
        );
        // No double slash after a trailing slash.
        assert_eq!(
            normalize_sfu_url("https://host.example.net/livekit/").as_deref(),
            Some("wss://host.example.net/livekit/rtc")
        );
        assert_eq!(
            normalize_sfu_url("https://host.example.net/").as_deref(),
            Some("wss://host.example.net/rtc")
        );
    }

    #[test]
    fn the_jwt_service_request_body_matches_the_reference_service() {
        // lk-jwt-service uses #[serde(deny_unknown_fields)], so any extra field is
        // a 400. The /sfu/get body is exactly `room`, `openid_token` (the
        // homeserver's response verbatim) and `device_id`.
        let body = json!({
            "room": "!room:example.org",
            "openid_token": {
                "access_token": "tok",
                "token_type": "Bearer",
                "matrix_server_name": "example.org",
                "expires_in": 3600,
            },
            "device_id": "DEVICE",
        });
        let object = body.as_object().expect("object");
        let mut keys: Vec<&str> = object.keys().map(String::as_str).collect();
        keys.sort_unstable();
        assert_eq!(keys, vec!["device_id", "openid_token", "room"]);
        let token = object["openid_token"].as_object().expect("token");
        let mut token_keys: Vec<&str> =
            token.keys().map(String::as_str).collect();
        token_keys.sort_unstable();
        assert_eq!(
            token_keys,
            vec![
                "access_token",
                "expires_in",
                "matrix_server_name",
                "token_type"
            ]
        );
    }

    #[test]
    fn track_source_numbers_match_the_livekit_wire() {
        // The numbers Element Call and livekit-client read, pinned as literals:
        // an assertion through the enum could not catch an off-by-one mapping.
        assert_eq!(lkp::TrackSource::Camera as i32, 1);
        assert_eq!(lkp::TrackSource::Microphone as i32, 2);
        assert_eq!(lkp::TrackSource::ScreenShare as i32, 3);
        assert_eq!(lkp::TrackSource::ScreenShareAudio as i32, 4);
        assert_eq!(lkp::TrackType::Audio as i32, 0);
        assert_eq!(lkp::TrackType::Video as i32, 1);
        // E2EE declaration: Element Call publishes GCM in encrypted rooms.
        assert_eq!(lkp::encryption::Type::None as i32, 0);
        assert_eq!(lkp::encryption::Type::Gcm as i32, 1);
    }

    #[test]
    fn added_tracks_carry_the_right_source() {
        // The same function as the AddTrack path, asserted in raw wire numbers.
        assert_eq!(track_source_for(1, true), 3);   // screen share video
        assert_eq!(track_source_for(0, true), 4);   // screen share AUDIO
        assert_eq!(track_source_for(1, false), 1);  // camera
        assert_eq!(track_source_for(0, false), 2);  // microphone

        // All four combinations are distinct; two collapsing was the bug.
        let all = [
            track_source_for(1, true),
            track_source_for(0, true),
            track_source_for(1, false),
            track_source_for(0, false),
        ];
        let mut seen = all.to_vec();
        seen.sort_unstable();
        seen.dedup();
        assert_eq!(seen.len(), 4, "two track sources collapsed onto one");
        // A screen share is a video track sourced from the screen, never
        // SCREEN_SHARE_AUDIO.
        assert_ne!(track_source_for(1, true),
                   lkp::TrackSource::ScreenShareAudio as i32);
        // Nor a camera: Element lays them out differently.
        assert_ne!(track_source_for(1, true), track_source_for(1, false));

        assert_eq!(track_encryption_for(true), 1);  // GCM
        assert_eq!(track_encryption_for(false), 0); // NONE
    }

    #[test]
    fn track_kinds_and_sources_are_closed_sets() {
        let info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            tracks: vec![
                lkp::TrackInfo {
                    sid: "TR_a".to_owned(),
                    r#type: lkp::TrackType::Audio as i32,
                    source: lkp::TrackSource::Microphone as i32,
                    ..Default::default()
                },
                lkp::TrackInfo {
                    sid: "TR_b".to_owned(),
                    r#type: lkp::TrackType::Video as i32,
                    source: lkp::TrackSource::ScreenShare as i32,
                    ..Default::default()
                },
                lkp::TrackInfo {
                    sid: "TR_c".to_owned(),
                    r#type: lkp::TrackType::Video as i32,
                    source: 99, // unknown to us
                    ..Default::default()
                },
            ],
            ..Default::default()
        };
        let value = participant_json(&info).expect("valid");
        let tracks = value["tracks"].as_array().expect("tracks");
        assert_eq!(tracks[0]["kind"], json!("audio"));
        assert_eq!(tracks[0]["source"], json!("microphone"));
        assert_eq!(tracks[1]["kind"], json!("video"));
        assert_eq!(tracks[1]["source"], json!("screen_share"));
        // An unrecognised source becomes "unknown", never forwarded verbatim.
        assert_eq!(tracks[2]["source"], json!("unknown"));
    }

    // A participant's camera and screen share are two tracks; `mid` (repeated
    // as the subscriber SDP's `a=mid:`) tells them apart.
    #[test]
    fn tracks_carry_their_media_section_id() {
        let info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            tracks: vec![
                lkp::TrackInfo {
                    sid: "TR_cam".to_owned(),
                    r#type: lkp::TrackType::Video as i32,
                    source: lkp::TrackSource::Camera as i32,
                    mid: "1".to_owned(),
                    stream: "PA_sender".to_owned(),
                    ..Default::default()
                },
                lkp::TrackInfo {
                    sid: "TR_screen".to_owned(),
                    r#type: lkp::TrackType::Video as i32,
                    source: lkp::TrackSource::ScreenShare as i32,
                    mid: "2".to_owned(),
                    ..Default::default()
                },
            ],
            ..Default::default()
        };
        let value = participant_json(&info).expect("valid");
        let tracks = value["tracks"].as_array().expect("tracks");
        assert_eq!(tracks[0]["mid"], json!("1"));
        assert_eq!(tracks[0]["stream"], json!("PA_sender"));
        assert_eq!(tracks[1]["mid"], json!("2"));
        // The two video tracks are distinguishable, which is the whole point.
        assert_ne!(tracks[0]["mid"], tracks[1]["mid"]);
        // No stream id is the empty string, not null.
        assert_eq!(tracks[1]["stream"], json!(""));
    }

    // A mid is an SDP token; absurd or control-laden values are dropped.
    #[test]
    fn an_absurd_media_section_id_is_dropped_not_forwarded() {
        let info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            tracks: vec![lkp::TrackInfo {
                sid: "TR_a".to_owned(),
                r#type: lkp::TrackType::Video as i32,
                source: lkp::TrackSource::Camera as i32,
                mid: "x".repeat(500),
                stream: "s\u{7}p".to_owned(),
                ..Default::default()
            }],
            ..Default::default()
        };
        let value = participant_json(&info).expect("valid");
        let tracks = value["tracks"].as_array().expect("tracks");
        assert_eq!(tracks[0]["mid"], json!(""));
        assert_eq!(tracks[0]["stream"], json!(""));
    }

    #[test]
    fn tracks_are_capped_per_participant() {
        let info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            tracks: (0..64)
                .map(|i| lkp::TrackInfo {
                    sid: format!("TR_{i}"),
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        };
        let value = participant_json(&info).expect("valid");
        assert_eq!(
            value["tracks"].as_array().expect("tracks").len(),
            MAX_TRACKS_PER_PARTICIPANT
        );
    }

    #[test]
    fn a_signal_request_round_trips_through_prost() {
        // Guards the wire encoding: shifted tags would otherwise make the SFU
        // silently ignore our offers.
        let request = lkp::SignalRequest {
            message: Some(lkp::signal_request::Message::Offer(
                lkp::SessionDescription {
                    r#type: "offer".to_owned(),
                    sdp: "v=0".to_owned(),
                    ..Default::default()
                },
            )),
        };
        let mut buffer = Vec::new();
        request.encode(&mut buffer).expect("encodes");
        let decoded =
            lkp::SignalRequest::decode(&buffer[..]).expect("decodes");
        match decoded.message {
            Some(lkp::signal_request::Message::Offer(sdp)) => {
                assert_eq!(sdp.r#type, "offer");
                assert_eq!(sdp.sdp, "v=0");
            }
            _ => panic!("wrong variant"),
        }
    }
}
