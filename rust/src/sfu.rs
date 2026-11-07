
use std::collections::{BTreeMap, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use livekit_protocol as lkp;

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

pub(crate) fn track_encryption_for(encrypted: bool) -> i32 {
    if encrypted {
        lkp::encryption::Type::Gcm as i32
    } else {
        lkp::encryption::Type::None as i32
    }
}

/// One logical share, two independently encrypted RTP senders. Explicit HIGH
/// layers prevent the server inferring three simulcast spatial layers.
fn share_simulcast_codecs(cid: &str, backup: Option<&str>, width: u32, height: u32)
    -> Vec<lkp::SimulcastCodec>
{
    let Some(backup) = backup else { return Vec::new() };
    [("h264", cid, width, height), ("vp8", backup, 640, 360)]
        .into_iter().map(|(codec, cid, width, height)| lkp::SimulcastCodec {
            codec: codec.to_owned(), cid: cid.to_owned(),
            layers: vec![lkp::VideoLayer {
                quality: lkp::VideoQuality::High as i32, width, height,
                ..Default::default()
            }],
            ..Default::default()
        }).collect()
}

pub(crate) fn leave_disposition(action: i32, can_reconnect: bool)
    -> &'static str
{
    use lkp::leave_request::Action;
    match Action::try_from(action) {
        Ok(Action::Resume) | Ok(Action::Reconnect) => "reconnecting",
        Ok(Action::Disconnect) | Err(_) => {
            if can_reconnect { "reconnecting" } else { "ended" }
        }
    }
}

pub(crate) fn signal_silence_limit(ping_timeout: i32, ping_interval: i32)
    -> Option<Duration>
{
    if ping_timeout <= 0 {
        return None;
    }
    let interval = ping_interval.clamp(1, 120) as u64;
    let timeout = (ping_timeout as u64).clamp(5, 120).max(interval + 2);
    Some(Duration::from_secs(timeout))
}
use matrix_sdk::ruma::api::client::account::request_openid_token;
use prost::Message as _;
use serde_json::json;
use tokio_tungstenite::tungstenite::Message as WsMessage;

use crate::rooms::{classify_room_error, require_client};
#[cfg(not(feature = "test-loopback-foci"))]
use crate::rooms::public_ip;
use crate::rtc::{
    focus_is_advertised, own_homeserver_transports, public_hostname,
    resolve_focus_hosts, HostRefusal, LivekitTransport,
};
#[cfg(feature = "test-loopback-foci")]
use crate::rtc::{loopback_hostname, test_focus_ip};
use crate::{enqueue, RustClient};

const MAX_SFU_RESPONSE: usize = 64 * 1024;
const SFU_TIMEOUT: Duration = Duration::from_secs(20);
/// Signalling frames are small; this party is outside the homeserver's
/// trust boundary.
const MAX_SIGNAL_FRAME: usize = 256 * 1024;
const MAX_PARTICIPANTS: usize = 128;
const MAX_TRACKS_PER_PARTICIPANT: usize = 8;
const MAX_FOCUS_CONNECTIONS: usize = 8;
const COMMAND_QUEUE_CAPACITY: usize = 128;
const LK_PROTOCOL_VERSION: u32 = 15;

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

fn resolve_refusal_category(refusal: HostRefusal) -> &'static str {
    match refusal {
        HostRefusal::PrivateName => "focus_private_name",
        HostRefusal::Unresolved => "focus_unresolved",
        HostRefusal::NonPublicAddress(_) => "focus_unroutable",
    }
}

/// Whether a refused non-public focus was judged under the own-homeserver LAN
/// allowance, and if not, why not.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum LanAllowance {
    Applied,
    NotApplied,
    NotAdvertised,
    Unknown,
}

impl LanAllowance {
    fn as_str(self) -> &'static str {
        match self {
            Self::Applied => "applied",
            Self::NotApplied => "not_applied",
            Self::NotAdvertised => "not_advertised",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Debug)]
struct FocusFailure {
    category: String,
    address_class: Option<&'static str>,
    lan_allowance: LanAllowance,
}

impl From<String> for FocusFailure {
    fn from(category: String) -> Self {
        Self { category, address_class: None, lan_allowance: LanAllowance::NotApplied }
    }
}

impl FocusFailure {
    fn refused(refusal: HostRefusal, lan_allowance: LanAllowance) -> Self {
        let address_class = match refusal {
            HostRefusal::NonPublicAddress(class) => Some(class),
            _ => None,
        };
        Self {
            category: resolve_refusal_category(refusal).to_owned(),
            address_class,
            lan_allowance,
        }
    }

    fn fields(&self, event: &mut serde_json::Value) {
        event["category"] = json!(self.category);
        if let Some(class) = self.address_class {
            event["address_class"] = json!(class);
            event["lan_allowance"] = json!(self.lan_allowance.as_str());
        }
    }
}

/// Resolves a focus host. A non-public answer is re-checked against the foci
/// the user's own homeserver advertises, since only those may live on a LAN.
async fn resolve_focus<F, Fut>(
    service_url: &str,
    host: &str,
    port: u16,
    own_transports: F,
) -> Result<(Vec<std::net::SocketAddr>, bool), FocusFailure>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Option<Vec<LivekitTransport>>>,
{
    let resolve = |own: bool| async move {
        let allowance = if own { LanAllowance::Applied } else { LanAllowance::NotApplied };
        match tokio::time::timeout(SFU_TIMEOUT, resolve_focus_hosts(host, port, own)).await {
            Ok(result) => result.map_err(|refusal| FocusFailure::refused(refusal, allowance)),
            Err(_) => Err(FocusFailure::from("focus_resolve_timeout".to_owned())),
        }
    };
    let mut failure = match resolve(false).await {
        Ok(addresses) => return Ok((addresses, false)),
        Err(failure) => failure,
    };
    if failure.address_class.is_none() {
        return Err(failure);
    }
    match own_transports().await {
        Some(own) if focus_is_advertised(service_url, &own) => {
            return resolve(true).await.map(|addresses| (addresses, true));
        }
        Some(_) => failure.lan_allowance = LanAllowance::NotAdvertised,
        None => failure.lan_allowance = LanAllowance::Unknown,
    }
    Err(failure)
}

fn classify_io_error(err: &std::io::Error) -> &'static str {
    use std::io::ErrorKind as Kind;
    match err.kind() {
        Kind::ConnectionRefused => "sfu_refused_connection",
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

enum ConnectWalk<S> {
    Connected(S),
    Failed(Option<tokio_tungstenite::tungstenite::Error>),
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

fn classify_ws_error(
    err: &tokio_tungstenite::tungstenite::Error,
) -> &'static str {
    use tokio_tungstenite::tungstenite::Error as Ws;
    match err {
        Ws::Io(io) => classify_io_error(io),
        Ws::Tls(_) => "tls_failed",
        Ws::Http(response) => match response.status().as_u16() {
            401 | 403 => "sfu_forbidden",
            404 => "sfu_not_found",
            429 => "rate_limited",
            500..=599 => "server_error",
            _ => "ws_rejected",
        },
        Ws::Protocol(_) | Ws::HttpFormat(_) | Ws::Utf8 | Ws::AttackAttempt => {
            "ws_handshake_failed"
        }
        Ws::Url(_) => "focus_url_invalid",
        Ws::Capacity(_) => "ws_frame_too_large",
        Ws::ConnectionClosed | Ws::AlreadyClosed => "connection_lost",
        _ => "connect_failed",
    }
}

#[derive(Clone, Debug)]
pub(crate) struct SfuCredentials {
    pub url: String,
    /// Short-lived SFU authorization. Never logged, never crosses the FFI.
    pub jwt: String,
    pub can_publish: Option<bool>,
    /// Issued by a focus the user's own homeserver advertises.
    pub own_infrastructure: bool,
}

fn jwt_can_publish(jwt: &str) -> Option<bool> {
    use base64::Engine as _;
    let payload = jwt.split('.').nth(1)?;
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(payload).ok()?;
    if bytes.len() > 8192 { return None; }
    let claims: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
    claims.get("video")?.get("canPublish")?.as_bool()
}

pub(crate) fn normalize_sfu_url(raw: &str) -> Option<String> {
    let mut parsed = url::Url::parse(raw).ok()?;
    match parsed.scheme() {
        "wss" => {}
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

fn public_ws_host(url: &url::Url) -> bool {
    match url.host() {
        #[cfg(feature = "test-loopback-foci")]
        Some(url::Host::Ipv4(addr)) => test_focus_ip(std::net::IpAddr::V4(addr)),
        #[cfg(not(feature = "test-loopback-foci"))]
        Some(url::Host::Ipv4(addr)) => public_ip(std::net::IpAddr::V4(addr)),
        #[cfg(feature = "test-loopback-foci")]
        Some(url::Host::Ipv6(addr)) => test_focus_ip(std::net::IpAddr::V6(addr)),
        #[cfg(not(feature = "test-loopback-foci"))]
        Some(url::Host::Ipv6(addr)) => public_ip(std::net::IpAddr::V6(addr)),
        #[cfg(feature = "test-loopback-foci")]
        Some(url::Host::Domain(name)) => public_hostname(name) || loopback_hostname(name),
        #[cfg(not(feature = "test-loopback-foci"))]
        Some(url::Host::Domain(name)) => public_hostname(name),
        None => false,
    }
}

async fn fetch_openid_token(client: &matrix_sdk::Client)
    -> Result<serde_json::Value, String>
{
    let user_id = client
        .user_id()
        .ok_or_else(|| "no session".to_owned())?
        .to_owned();

    let openid = client
        .send(request_openid_token::v3::Request::new(user_id))
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;

    Ok(json!({
        "access_token": openid.access_token,
        "token_type": openid.token_type.to_string(),
        "matrix_server_name": openid.matrix_server_name.to_string(),
        "expires_in": openid.expires_in.as_secs(),
    }))
}

async fn fetch_sfu_credentials<F, Fut>(
    service_url: &str,
    room_id: &str,
    device_id: &str,
    openid_token: &serde_json::Value,
    own_transports: F,
) -> Result<SfuCredentials, FocusFailure>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Option<Vec<LivekitTransport>>>,
{
    let body = serde_json::to_vec(&json!({
        "room": room_id,
        "openid_token": openid_token,
        "device_id": device_id,
    }))
    .map_err(|_| "invalid_request".to_owned())?;

    let mut url = url::Url::parse(service_url)
        .map_err(|_| "invalid_transport".to_owned())?;
    url.path_segments_mut()
        .map_err(|_| "invalid_transport".to_owned())?
        .pop_if_empty()
        .extend(["sfu", "get"]);

    // The focus host is chosen by another participant, and this request
    // carries the user's OpenID token, device id and room id, so it uses a
    // dedicated client:
    //   * the name is resolved here, all addresses must be public (a focus the
    //     user's own homeserver advertises may also be LAN), and the first is
    //     pinned, so a private record or rebinding cannot reach the
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
    let (addresses, own_infrastructure) =
        resolve_focus(service_url, &host, port, own_transports).await?;
    let pinned = addresses[0];
    let mut http_builder = reqwest::Client::builder()
        .https_only(true)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .resolve(&host, pinned)
        .connect_timeout(Duration::from_secs(5))
        .timeout(SFU_TIMEOUT)
        .user_agent(format!("Lightning/{}", env!("CARGO_PKG_VERSION")));
    #[cfg(feature = "test-loopback-foci")]
    if std::env::var_os("SSL_CERT_FILE").is_some() {
        let certs = rustls_native_certs::load_native_certs();
        if certs.certs.is_empty() || !certs.errors.is_empty() {
            return Err("invalid_request".to_owned().into());
        }
        for cert in certs.certs {
            http_builder = http_builder.add_root_certificate(
                reqwest::Certificate::from_der(cert.as_ref())
                    .map_err(|_| "invalid_request".to_owned())?);
        }
    }
    let http = http_builder.build()
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
            300..=399 => "invalid".to_owned(),
            401 | 403 => "forbidden".to_owned(),
            404 => "unsupported".to_owned(),
            429 => "rate_limited".to_owned(),
            500..=599 => "server_error".to_owned(),
            _ => "unknown".to_owned(),
        }.into());
    }
    if response
        .content_length()
        .is_some_and(|len| len > MAX_SFU_RESPONSE as u64)
    {
        return Err("invalid".to_owned().into());
    }
    let mut bytes = Vec::with_capacity(1024);
    let mut stream = response;
    while let Some(chunk) = stream
        .chunk()
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?
    {
        if bytes.len() + chunk.len() > MAX_SFU_RESPONSE {
            return Err("invalid".to_owned().into());
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

    let can_publish = jwt_can_publish(&jwt);
    Ok(SfuCredentials { url: url_ok, jwt, can_publish, own_infrastructure })
}

const LEAVE_FLUSH_MARGIN_MS: u64 = 250;
const LEAVE_FLUSH_TIMEOUT: Duration =
    Duration::from_millis(crate::SHUTDOWN_ACTION_JOIN_MS - LEAVE_FLUSH_MARGIN_MS);

const _: () = assert!(
    LEAVE_FLUSH_MARGIN_MS < crate::SHUTDOWN_ACTION_JOIN_MS,
    "SHUTDOWN_ACTION_JOIN_MS no longer leaves room for the SFU leave drain; \
     a graceful LiveKit Leave would be aborted before it is sent, stranding \
     a participant in every call the user quits out of."
);

#[derive(Debug)]
pub(crate) enum SfuCommand {
    Offer { sdp: String, target: i32 },
    Answer { sdp: String, target: i32 },
    Candidate { candidate_init: String, target: i32 },
    AddTrack {
        cid: String,
        name: String,
        kind: i32,
        width: u32,
        height: u32,
        screen_share: bool,
        encrypted: bool,
        backup_cid: Option<String>,
    },
    MuteTrack { sid: String, muted: bool },
    Leave,
}

pub(crate) struct SfuSession {
    pub generation: u64,
    pub connection_id: u32,
    pub dropped_commands: AtomicU64,
    pub commands: tokio::sync::mpsc::Sender<SfuCommand>,
    pub leave: tokio::sync::oneshot::Sender<()>,
    pub task: tokio::task::JoinHandle<()>,
}

pub(crate) struct SfuState {
    pub sessions: Mutex<BTreeMap<(String, String), SfuSession>>,
    pub generation: AtomicU64,
}

impl Default for SfuState {
    fn default() -> Self {
        Self { sessions: Mutex::new(BTreeMap::new()), generation: AtomicU64::new(0) }
    }
}

impl SfuState {
    fn take_sessions(&self) -> BTreeMap<(String, String), SfuSession> {
        let mut guard = self.sessions.lock()
            .unwrap_or_else(|poison| poison.into_inner());
        std::mem::take(&mut *guard)
    }
}

fn command_generation_current(active: u64, session: u64, callback: u64) -> bool {
    active == callback && session == callback
}

fn command_allowed_on(connection_id: u32, command: &SfuCommand) -> bool {
    connection_id == 0 || matches!(
        command,
        SfuCommand::Offer { target: 1, .. }
            | SfuCommand::Answer { target: 1, .. }
            | SfuCommand::Candidate { target: 1, .. }
            | SfuCommand::Leave
    )
}

fn participant_json(info: &lkp::ParticipantInfo) -> Option<serde_json::Value> {
    let identity = sane(&info.identity, 512)?;
    let tracks: Vec<serde_json::Value> = info
        .tracks
        .iter()
        .take(MAX_TRACKS_PER_PARTICIPANT)
        .filter_map(|track| {
            let sid = sane(&track.sid, 256)?;
            Some(json!({
                "sid": sid,
                "kind": if track.r#type == 1 { "video" } else { "audio" },
                "source": match lkp::TrackSource::try_from(track.source) {
                    Ok(lkp::TrackSource::Camera) => "camera",
                    Ok(lkp::TrackSource::Microphone) => "microphone",
                    Ok(lkp::TrackSource::ScreenShare) => "screen_share",
                    Ok(lkp::TrackSource::ScreenShareAudio)
                        => "screen_share_audio",
                    _ => "unknown",
                },
                "muted": track.muted,
                "mid": sane(&track.mid, 128).unwrap_or_default(),
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

type SignalStream = tokio_tungstenite::WebSocketStream<
    tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

const RESUME_ANSWER_TIMEOUT: Duration = Duration::from_secs(5);
const RESUME_RETRY_DELAYS: [Duration; 3] = [
    Duration::from_millis(0),
    Duration::from_millis(500),
    Duration::from_millis(1500),
];
const MAX_RESUMES_PER_WINDOW: usize = 3;
const RESUME_WINDOW: Duration = Duration::from_secs(60);
const MAX_RESUME_CANDIDATES: usize = 32;
const MAX_REFRESHED_TOKEN: usize = 8 * 1024;

/// `resume` is (participant sid, LiveKit `ReconnectReason`). With it the server
/// reattaches this socket to the session it already holds; without it a socket
/// carrying the same identity makes the server drop that session as a duplicate,
/// which every other participant sees as a leave and a rejoin.
pub(crate) fn signal_url(
    base: &url::Url,
    token: &str,
    resume: Option<(&str, i32)>,
) -> url::Url {
    let mut url = base.clone();
    {
        let mut query = url.query_pairs_mut();
        query
            .append_pair("access_token", token)
            .append_pair("protocol", &LK_PROTOCOL_VERSION.to_string())
            .append_pair("auto_subscribe", "1")
            .append_pair("sdk", "cpp")
            .append_pair("version", env!("CARGO_PKG_VERSION"));
        if let Some((sid, reason)) = resume {
            query
                .append_pair("reconnect", "1")
                .append_pair("sid", sid)
                .append_pair("reconnect_reason", &reason.to_string());
        }
    }
    url
}

pub(crate) fn leave_asks_resume(action: i32) -> bool {
    matches!(lkp::leave_request::Action::try_from(action),
             Ok(lkp::leave_request::Action::Resume))
}

/// Bounded so a server that keeps asking for a resume it cannot complete ends
/// in a full rejoin rather than a loop.
pub(crate) fn may_resume(
    sid: Option<&str>,
    recent: &mut VecDeque<std::time::Instant>,
    now: std::time::Instant,
) -> bool {
    if sid.is_none_or(str::is_empty) {
        return false;
    }
    while recent
        .front()
        .is_some_and(|at| now.saturating_duration_since(*at) >= RESUME_WINDOW)
    {
        recent.pop_front();
    }
    if recent.len() >= MAX_RESUMES_PER_WINDOW {
        return false;
    }
    recent.push_back(now);
    true
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum ResumeAnswer {
    Resumed,
    Refused,
    Lost,
}

/// Anything before the `ReconnectResponse` is skipped, as livekit-client does.
async fn await_resume_answer<S>(source: &mut S) -> ResumeAnswer
where
    S: futures_util::Stream<
            Item = Result<WsMessage, tokio_tungstenite::tungstenite::Error>,
        > + Unpin,
{
    while let Some(frame) = source.next().await {
        let payload = match frame {
            Ok(WsMessage::Binary(bytes)) => bytes,
            Ok(WsMessage::Close(_)) | Err(_) => return ResumeAnswer::Lost,
            Ok(_) => continue,
        };
        if payload.len() > MAX_SIGNAL_FRAME {
            continue;
        }
        match lkp::SignalResponse::decode(&payload[..]).ok().and_then(|r| r.message) {
            Some(lkp::signal_response::Message::Reconnect(_)) => {
                return ResumeAnswer::Resumed;
            }
            Some(lkp::signal_response::Message::Leave(_))
            | Some(lkp::signal_response::Message::Join(_)) => {
                return ResumeAnswer::Refused;
            }
            _ => continue,
        }
    }
    ResumeAnswer::Lost
}

/// The server ICE-restarts the subscriber on every resume, and pion's restart
/// forgets every remote candidate. webrtcbin answers with its old ICE
/// credentials and gathers nothing new, so the candidates it already sent are
/// sent again after that answer, or the subscriber never reconnects.
#[derive(Default)]
pub(crate) struct SubscriberCandidates {
    sent: VecDeque<String>,
    pending: bool,
}

impl SubscriberCandidates {
    pub(crate) fn record(&mut self, candidate_init: &str) {
        if self.sent.len() >= MAX_RESUME_CANDIDATES {
            self.sent.pop_front();
        }
        self.sent.push_back(candidate_init.to_owned());
    }

    pub(crate) fn arm(&mut self) {
        self.pending = !self.sent.is_empty();
    }

    pub(crate) fn after_answer(&mut self) -> Vec<String> {
        if !std::mem::take(&mut self.pending) {
            return Vec::new();
        }
        self.sent.iter().cloned().collect()
    }
}

async fn dial_signal(
    target: &url::Url,
    addresses: Vec<std::net::SocketAddr>,
    budget: Duration,
) -> ConnectWalk<SignalStream> {
    walk_addresses(
        addresses,
        tokio::time::Instant::now() + budget,
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
            .map(|(stream, _)| stream)
        },
    )
    .await
}

async fn resume_signal(
    base: &url::Url,
    token: &str,
    sid: &str,
    reason: i32,
    addresses: &[std::net::SocketAddr],
) -> Result<SignalStream, &'static str> {
    let target = signal_url(base, token, Some((sid, reason)));
    for delay in RESUME_RETRY_DELAYS {
        tokio::time::sleep(delay).await;
        let mut stream = match dial_signal(
            &target, addresses.to_vec(), RESUME_ANSWER_TIMEOUT).await
        {
            ConnectWalk::Connected(stream) => stream,
            ConnectWalk::Failed(Some(tokio_tungstenite::tungstenite::Error::Http(_))) => {
                return Err("resume_refused");
            }
            ConnectWalk::Failed(_) | ConnectWalk::TimedOut => continue,
        };
        let answer = tokio::time::timeout(
            RESUME_ANSWER_TIMEOUT, await_resume_answer(&mut stream)).await;
        match answer {
            Ok(ResumeAnswer::Resumed) => return Ok(stream),
            Ok(ResumeAnswer::Refused) => {
                let _ = stream.close(None).await;
                return Err("resume_refused");
            }
            Ok(ResumeAnswer::Lost) | Err(_) => {
                let _ = stream.close(None).await;
            }
        }
    }
    Err("resume_failed")
}

#[allow(clippy::too_many_arguments)]
async fn run_session(
    events: Arc<Mutex<std::collections::VecDeque<String>>>,
    generation: u64,
    connection_id: u32,
    generation_now: Arc<AtomicU64>,
    media_capable: Arc<AtomicBool>,
    credentials: SfuCredentials,
    service_host_hash: String,
    mut commands: tokio::sync::mpsc::Receiver<SfuCommand>,
    mut leave: tokio::sync::oneshot::Receiver<()>,
) {
    let current = {
        let generation_now = Arc::clone(&generation_now);
        move || generation_now.load(Ordering::SeqCst) == generation
    };
    let emit = {
        let events = Arc::clone(&events);
        let current = current.clone();
        move |mut value: serde_json::Value| {
            if current() {
                value["connection_id"] = json!(connection_id);
                enqueue(&events, value);
            }
        }
    };

    let mut base = match url::Url::parse(&credentials.url) {
        Ok(url) => url,
        Err(_) => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "focus_url_invalid",
            }));
            return;
        }
    };
    {
        let existing = base.path().trim_end_matches('/').to_owned();
        base.set_path(&format!("{existing}/rtc"));
    }
    let mut token = credentials.jwt.clone();
    let url = signal_url(&base, &token, None);

    let host = url.host_str().unwrap_or_default().to_owned();
    let port = url.port_or_known_default().unwrap_or(443);
    let own_infrastructure = credentials.own_infrastructure;
    let addresses = match tokio::time::timeout(
        SFU_TIMEOUT,
        resolve_focus_hosts(&host, port, own_infrastructure),
    )
    .await
    {
        Ok(Ok(addresses)) => addresses,
        Ok(Err(refusal)) => {
            let mut event = json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "stage": "signal",
            });
            let allowance = if own_infrastructure {
                LanAllowance::Applied
            } else {
                LanAllowance::NotApplied
            };
            FocusFailure::refused(refusal, allowance).fields(&mut event);
            emit(event);
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
    let mut stream = match dial_signal(&url, addresses.clone(), SFU_TIMEOUT).await {
        ConnectWalk::Connected(stream) => stream,
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
    drop(url);

    emit(json!({
        "type": "sfu_state", "generation": generation, "state": "signalling",
        "category": "",
    }));

    let mut ping_ticker =
        tokio::time::interval(std::time::Duration::from_secs(3600));
    ping_ticker.set_missed_tick_behavior(
        tokio::time::MissedTickBehavior::Delay);
    ping_ticker.tick().await;
    let mut ping_armed = false;
    // LiveKit matches an answer to its offer by `SessionDescription.id`, not
    // the peer-connection target. Server offer ids start from a random base, so
    // echo the last remote offer id back and number our own offers with a
    // counter, as livekit-client does. Index 0 = publisher, 1 = subscriber.
    let mut remote_offer_id = [0u32; 2];
    let mut local_offer_id = [0u32; 2];
    // After the JoinResponse a lost socket is a session to resume, and failing
    // that a call to recover ("reconnecting"). Before it, a failure is a failed join.
    let mut joined = false;
    let mut participant_sid: Option<String> = None;
    let mut resumes: VecDeque<std::time::Instant> = VecDeque::new();
    let mut subscriber_candidates = SubscriberCandidates::default();
    let mut silence_limit: Option<Duration> = None;
    let mut last_heard = tokio::time::Instant::now();
    let reconnecting = |category: &str| json!({
        "type": "sfu_state", "generation": generation,
        "state": "reconnecting", "category": category,
    });
    let signal_lost = lkp::ReconnectReason::RrSignalDisconnected as i32;

    'session: loop {
        let (mut sink, mut source) = stream.split();
        let loss: Option<(&'static str, i32)> = loop {
            tokio::select! {
                biased;
                _ = &mut leave => {
                    let _ = send_request(&mut sink,
                        lkp::signal_request::Message::Leave(
                            lkp::LeaveRequest::default())).await;
                    break None;
                }
                _ = ping_ticker.tick(), if ping_armed => {
                    let now_ms = std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .map(|d| d.as_millis() as i64)
                        .unwrap_or(0);
                    if !send_request(
                        &mut sink,
                        lkp::signal_request::Message::Ping(now_ms)).await
                    {
                        break Some(("signal_lost", signal_lost));
                    }
                    if !send_request(
                        &mut sink,
                        lkp::signal_request::Message::PingReq(lkp::Ping {
                            timestamp: now_ms,
                            rtt: 0,
                        })).await
                    {
                        break Some(("signal_lost", signal_lost));
                    }
                }
                command = commands.recv() => {
                    let Some(command) = command else { break None };
                    let mut replay = Vec::new();
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
                            if slot == 1 {
                                replay = subscriber_candidates.after_answer();
                            }
                            lkp::signal_request::Message::Answer(
                                lkp::SessionDescription {
                                    r#type: "answer".to_owned(), sdp,
                                    id: remote_offer_id[slot],
                                    ..Default::default()
                                })
                        }
                        SfuCommand::Candidate { candidate_init, target } => {
                            if target == 1 {
                                subscriber_candidates.record(&candidate_init);
                            }
                            lkp::signal_request::Message::Trickle(
                                lkp::TrickleRequest {
                                    candidate_init, target, r#final: false,
                                })
                        }
                        SfuCommand::AddTrack {
                            cid, name, kind, width, height, screen_share, encrypted, backup_cid,
                        } => {
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
                                    backup_codec_policy: if backup_cid.is_some() {
                                        lkp::BackupCodecPolicy::Simulcast as i32
                                    } else { lkp::BackupCodecPolicy::PreferRegression as i32 },
                                    simulcast_codecs: share_simulcast_codecs(&cid, backup_cid.as_deref(), width, height),
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
                            break None;
                        }
                    };
                    let mut sent = send_request(&mut sink, message).await;
                    for candidate_init in replay {
                        if !sent {
                            break;
                        }
                        sent = send_request(&mut sink,
                            lkp::signal_request::Message::Trickle(
                                lkp::TrickleRequest {
                                    candidate_init, target: 1, r#final: false,
                                })).await;
                    }
                    if !sent {
                        if joined {
                            break Some(("signal_lost", signal_lost));
                        }
                        emit(json!({
                            "type": "sfu_state", "generation": generation,
                            "state": "failed", "category": "send_failed",
                        }));
                        break None;
                    }
                }
                frame = source.next() => {
                    let Some(frame) = frame else {
                        if joined {
                            break Some(("signal_lost", signal_lost));
                        }
                        break None;
                    };
                    if frame.is_ok() {
                        last_heard = tokio::time::Instant::now();
                    }
                    let payload = match frame {
                        Ok(WsMessage::Binary(bytes)) => bytes,
                        Ok(WsMessage::Close(_)) => {
                            if joined {
                                break Some(("signal_lost", signal_lost));
                            }
                            break None;
                        }
                        Ok(WsMessage::Ping(_)) | Ok(WsMessage::Pong(_))
                        | Ok(WsMessage::Text(_)) | Ok(WsMessage::Frame(_)) => continue,
                        Err(_) => {
                            if joined {
                                break Some(("signal_lost", signal_lost));
                            }
                            emit(json!({
                                "type": "sfu_state", "generation": generation,
                                "state": "failed", "category": "connection_lost",
                            }));
                            break None;
                        }
                    };
                    if payload.len() > MAX_SIGNAL_FRAME {
                        continue;
                    }
                    let Ok(response) = lkp::SignalResponse::decode(&payload[..])
                    else { continue };
                    let Some(message) = response.message else { continue };

                    match message {
                        lkp::signal_response::Message::Join(join) => {
                            let interval = join.ping_interval.clamp(1, 120) as u64;
                            ping_ticker = tokio::time::interval(
                                std::time::Duration::from_secs(interval));
                            ping_ticker.set_missed_tick_behavior(
                                tokio::time::MissedTickBehavior::Delay);
                            ping_ticker.tick().await;
                            ping_armed = true;
                            joined = true;
                            participant_sid = join.participant.as_ref()
                                .and_then(|p| sane(&p.sid, 256))
                                .map(ToOwned::to_owned);
                            silence_limit = signal_silence_limit(
                                join.ping_timeout, join.ping_interval);
                            last_heard = tokio::time::Instant::now();
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
                            let ice: Vec<serde_json::Value> = join.ice_servers
                                .iter().take(8).map(|server| json!({
                                    "urls": server.urls.iter().take(8)
                                        .filter_map(|u| sane(u, 512))
                                        .collect::<Vec<_>>(),
                                    "username": server.username,
                                    "credential": server.credential,
                                })).collect();
                            if connection_id == 0 && join.participant.as_ref()
                                .and_then(|p| p.permission.as_ref())
                                .is_some_and(|p| !p.can_publish)
                            {
                                emit(json!({
                                    "type": "sfu_permission", "generation": generation,
                                    "service_host_hash": service_host_hash,
                                    "can_publish": false,
                                    "source": "join",
                                }));
                                emit(json!({
                                    "type": "sfu_state", "generation": generation,
                                    "state": "failed", "category": "publish_denied",
                                }));
                                break None;
                            }
                            emit(json!({
                                "type": "sfu_joined",
                                "generation": generation,
                                "service_host_hash": service_host_hash,
                                "can_publish": join.participant.as_ref()
                                    .and_then(|p| p.permission.as_ref())
                                    .map(|p| p.can_publish),
                                "can_subscribe": join.participant.as_ref()
                                    .and_then(|p| p.permission.as_ref())
                                    .map(|p| p.can_subscribe),
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
                                "service_host_hash": service_host_hash,
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
                            if joined && leave_asks_resume(leave.action) {
                                emit(json!({
                                    "type": "sfu_state", "generation": generation,
                                    "state": "resume_requested",
                                    "category": "server_leave",
                                    "reason": leave.reason,
                                    "action": leave.action,
                                }));
                                break Some((
                                    "server_leave",
                                    lkp::ReconnectReason::RrUnknown as i32,
                                ));
                            }
                            emit(json!({
                                "type": "sfu_state", "generation": generation,
                                "state": leave_disposition(
                                    leave.action, leave.can_reconnect),
                                "category": "server_leave",
                                "reason": leave.reason,
                                "action": leave.action,
                            }));
                            break None;
                        }
                        lkp::signal_response::Message::Mute(mute) => {
                            emit(json!({
                                "type": "sfu_server_mute",
                                "generation": generation,
                                "sid": sane(&mute.sid, 256).unwrap_or_default(),
                                "muted": mute.muted,
                            }));
                        }
                        lkp::signal_response::Message::RefreshToken(fresh) => {
                            if !fresh.is_empty() && fresh.len() <= MAX_REFRESHED_TOKEN {
                                token = fresh;
                            }
                        }
                        _ => {}
                    }
                }
                // After the socket: a task that was not polled for a while must
                // read the pongs already waiting before it calls the server silent.
                _ = tokio::time::sleep_until(
                        last_heard
                            + silence_limit.unwrap_or(Duration::from_secs(3600))),
                    if silence_limit.is_some() =>
                {
                    break Some(("signal_timeout", signal_lost));
                }
            }
        };
        let _ = sink.close().await;
        drop(source);

        let Some((category, reason)) = loss else { break 'session };
        if !may_resume(participant_sid.as_deref(), &mut resumes,
                       std::time::Instant::now()) {
            emit(reconnecting(category));
            break 'session;
        }
        emit(json!({
            "type": "sfu_state", "generation": generation,
            "state": "resuming", "category": category,
        }));
        let sid = participant_sid.clone().unwrap_or_default();
        let resumed = tokio::select! {
            biased;
            _ = &mut leave => break 'session,
            result = resume_signal(&base, &token, &sid, reason, &addresses) => result,
        };
        match resumed {
            Ok(next) => {
                stream = next;
                last_heard = tokio::time::Instant::now();
                subscriber_candidates.arm();
                emit(json!({
                    "type": "sfu_state", "generation": generation,
                    "state": "resumed", "category": category,
                }));
            }
            Err(refusal) => {
                emit(reconnecting(refusal));
                break 'session;
            }
        }
    }

    emit(json!({
        "type": "sfu_state", "generation": generation, "state": "closed",
        "category": "",
    }));
}

pub(crate) fn connect_many(
    bridge: &RustClient,
    connections: Vec<(u32, String)>,
    room_id: String,
    op_id: u64,
) -> Result<u64, String> {
    if connections.is_empty() || connections.len() > MAX_FOCUS_CONNECTIONS {
        return Err("invalid focus count".to_owned());
    }
    let mut ids = std::collections::BTreeSet::new();
    let mut urls = std::collections::BTreeSet::new();
    for (id, service_url) in &connections {
        let url = url::Url::parse(service_url)
            .map_err(|_| "invalid transport".to_owned())?;
        if *id >= MAX_FOCUS_CONNECTIONS as u32 || url.scheme() != "https"
            || !public_ws_host(&url) || !ids.insert(*id)
            || !urls.insert(service_url.clone())
        {
            return Err("invalid or duplicate focus".to_owned());
        }
    }
    if !ids.contains(&0) || sane(&room_id, 512).is_none() {
        return Err("missing publisher or invalid room".to_owned());
    }
    let client = require_client(bridge)?;
    if crate::route::routed(&bridge.store_path) {
        return Err(crate::route::BLOCKED_BY_ROUTE.to_owned());
    }
    let device_id = client.device_id()
        .ok_or_else(|| "no session".to_owned())?.to_string();

    disconnect(bridge);
    let generation = bridge.sfu.generation.fetch_add(1, Ordering::SeqCst) + 1;
    let generation_now = Arc::clone(&bridge.sfu_generation);
    generation_now.store(generation, Ordering::SeqCst);
    let openid = Arc::new(tokio::sync::OnceCell::new());
    let mut sessions = BTreeMap::new();
    for (connection_id, service_url) in connections {
        let focus_key = (room_id.clone(), service_url.clone());
        let service_host_hash = url::Url::parse(&service_url).ok()
            .and_then(|url| url.host_str().map(ToOwned::to_owned))
            .map(|host| {
                use sha2::Digest as _;
                let digest = sha2::Sha256::digest(host.as_bytes());
                digest[..6].iter().map(|byte| format!("{byte:02x}"))
                    .collect::<String>()
            }).unwrap_or_default();
        let events = Arc::clone(&bridge.events);
        let media_capable = Arc::clone(&bridge.call_media_capable);
        let current = Arc::clone(&generation_now);
        let client = client.clone();
        let openid = Arc::clone(&openid);
        let device_id = device_id.clone();
        let store_path = bridge.store_path.clone();
        let room_id = room_id.clone();
        let (tx, rx) = tokio::sync::mpsc::channel(COMMAND_QUEUE_CAPACITY);
        let (leave_tx, leave_rx) = tokio::sync::oneshot::channel();
        let task = bridge.runtime.spawn(async move {
            let result = match openid.get_or_try_init(|| fetch_openid_token(&client)).await {
                Ok(token) => fetch_sfu_credentials(
                    &service_url, &room_id, &device_id, token,
                    || own_homeserver_transports(&client, &store_path)).await,
                Err(category) => Err(FocusFailure::from(category.clone())),
            };
            match result {
                Ok(credentials) => {
                    if current.load(Ordering::SeqCst) != generation { return; }
                    if connection_id == 0 && credentials.can_publish == Some(false) {
                        enqueue(&events, json!({
                            "type": "sfu_permission", "generation": generation,
                            "connection_id": connection_id, "op_id": op_id,
                            "service_host_hash": service_host_hash,
                            "can_publish": false, "source": "jwt",
                        }));
                        enqueue(&events, json!({
                            "type": "sfu_state", "generation": generation,
                            "connection_id": connection_id, "op_id": op_id,
                            "state": "failed", "category": "publish_denied",
                        }));
                        return;
                    }
                    enqueue(&events, json!({
                        "type": "sfu_state", "generation": generation,
                        "connection_id": connection_id, "op_id": op_id,
                        "state": "authorized", "category": "",
                    }));
                    run_session(events, generation, connection_id, current,
                                media_capable, credentials, service_host_hash,
                                rx, leave_rx).await;
                }
                Err(failure) => {
                    if current.load(Ordering::SeqCst) != generation { return; }
                    let mut event = json!({
                        "type": "sfu_state", "generation": generation,
                        "connection_id": connection_id, "op_id": op_id,
                        "state": "failed", "stage": "credentials",
                    });
                    failure.fields(&mut event);
                    enqueue(&events, event);
                }
            }
        });
        sessions.insert(focus_key, SfuSession {
            generation, connection_id, dropped_commands: AtomicU64::new(0),
            commands: tx, leave: leave_tx, task,
        });
    }
    let mut guard = bridge.sfu.sessions.lock()
        .unwrap_or_else(|poison| poison.into_inner());
    *guard = sessions;
    Ok(generation)
}

pub(crate) fn connect(
    bridge: &RustClient,
    service_url: String,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    connect_many(bridge, vec![(0, service_url)], room_id, op_id).map(|_| ())
}

pub(crate) fn send_command_for(
    bridge: &RustClient,
    generation: u64,
    connection_id: u32,
    command: SfuCommand,
) {
    if !command_allowed_on(connection_id, &command) {
        return;
    }
    if let Ok(guard) = bridge.sfu.sessions.lock() {
        if let Some(session) = guard.values()
            .find(|session| session.connection_id == connection_id) {
            if command_generation_current(
                bridge.sfu_generation.load(Ordering::SeqCst),
                session.generation, generation) {
                if let Some(category) = try_queue_command(&session.commands,
                    &session.dropped_commands, &bridge.events, generation,
                    connection_id, command) {
                    enqueue(&bridge.events, json!({
                        "type": "sfu_state", "generation": generation,
                        "connection_id": connection_id,
                        "state": "failed", "category": category,
                    }));
                }
            }
        }
    }
}

fn try_queue_command(
    queue: &tokio::sync::mpsc::Sender<SfuCommand>,
    dropped_count: &AtomicU64,
    events: &Arc<Mutex<VecDeque<String>>>,
    generation: u64,
    connection_id: u32,
    command: SfuCommand,
) -> Option<&'static str> {
    let negotiation = matches!(command,
        SfuCommand::Offer { .. } | SfuCommand::Answer { .. });
    let category = match queue.try_send(command) {
        Ok(()) => return None,
        Err(tokio::sync::mpsc::error::TrySendError::Full(_)) =>
            "command_queue_full",
        Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) =>
            "command_queue_closed",
    };
    let dropped = dropped_count.fetch_add(1, Ordering::Relaxed) + 1;
    enqueue(events, json!({
        "type": "sfu_command_dropped", "generation": generation,
        "connection_id": connection_id, "dropped": dropped,
        "category": category,
    }));
    negotiation.then_some(category)
}

pub(crate) fn send_command(bridge: &RustClient, command: SfuCommand) {
    let generation = bridge.sfu_generation.load(Ordering::SeqCst);
    send_command_for(bridge, generation, 0, command);
}

pub(crate) fn disconnect(bridge: &RustClient) {
    bridge.sfu_generation.fetch_add(1, Ordering::SeqCst);
    let sessions = bridge.sfu.take_sessions();
    for (_, session) in sessions {
        let SfuSession { commands, leave, task, .. } = session;
        let _ = leave.send(());
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

    #[test]
    fn jwt_publish_denial_is_read_without_exposing_the_token() {
        use base64::Engine as _;
        let payload = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .encode(br#"{"video":{"canPublish":false}}"#);
        assert_eq!(jwt_can_publish(&format!("header.{payload}.sig")), Some(false));
        assert_eq!(jwt_can_publish("malformed"), None);
    }

    #[test]
    fn callbacks_from_an_old_generation_are_rejected() {
        assert!(command_generation_current(4, 4, 4));
        assert!(!command_generation_current(5, 4, 4));
        assert!(!command_generation_current(5, 5, 4));
    }

    #[test]
    fn hardware_share_advertises_two_cids_on_one_publication() {
        let codecs = share_simulcast_codecs("main", Some("backup"), 1920, 1080);
        let request = lkp::AddTrackRequest {
            cid: "main".to_owned(), simulcast_codecs: codecs,
            encryption: track_encryption_for(true), ..Default::default()
        };
        let wire = request.encode_to_vec();
        let decoded = lkp::AddTrackRequest::decode(&wire[..]).unwrap();
        assert_eq!(decoded.encryption, lkp::encryption::Type::Gcm as i32);
        assert_eq!(decoded.simulcast_codecs.len(), 2);
        for (codec, mime, cid, width, height) in [
            (&decoded.simulcast_codecs[0], "h264", "main", 1920, 1080),
            (&decoded.simulcast_codecs[1], "vp8", "backup", 640, 360),
        ] {
            assert_eq!(codec.codec, mime);
            assert_eq!(codec.cid, cid);
            assert_eq!(codec.layers.len(), 1);
            assert_eq!(codec.layers[0].width, width);
            assert_eq!(codec.layers[0].height, height);
            assert_eq!(codec.layers[0].quality, lkp::VideoQuality::High as i32);
        }
        assert!(share_simulcast_codecs("main", None, 1920, 1080).is_empty());
    }

    #[test]
    fn subscriber_focus_cannot_publish_a_track() {
        let command = SfuCommand::AddTrack {
            cid: "cid".to_owned(), name: "microphone".to_owned(),
            kind: 0, width: 0, height: 0,
            screen_share: false, encrypted: true, backup_cid: None,
        };
        assert!(command_allowed_on(0, &command));
        assert!(!command_allowed_on(1, &command));
        assert!(command_allowed_on(1,
            &SfuCommand::Answer { sdp: "answer".to_owned(), target: 1 }));
    }

    #[tokio::test]
    async fn teardown_takes_every_focus_connection() {
        let state = SfuState::default();
        let mut receivers = Vec::new();
        {
            let mut sessions = state.sessions.lock().unwrap();
            for id in 0..3 {
                let (commands, _receiver) =
                    tokio::sync::mpsc::channel(COMMAND_QUEUE_CAPACITY);
                let (leave, receiver) = tokio::sync::oneshot::channel();
                receivers.push(receiver);
                sessions.insert(("!room:x".to_owned(),
                                 format!("https://focus{id}.example")), SfuSession {
                    generation: 7, connection_id: id,
                    dropped_commands: AtomicU64::new(0), commands, leave,
                    task: tokio::spawn(async { std::future::pending::<()>().await }),
                });
            }
        }
        let sessions = state.take_sessions();
        assert_eq!(sessions.len(), 3);
        assert!(state.take_sessions().is_empty());
        for (_, session) in sessions {
            assert!(session.leave.send(()).is_ok());
            session.task.abort();
        }
        for receiver in receivers {
            assert!(receiver.await.is_ok());
        }
    }

    #[tokio::test]
    async fn full_command_queue_counts_drops_and_identifies_lost_answer() {
        let (sender, _receiver) = tokio::sync::mpsc::channel(1);
        let drops = AtomicU64::new(0);
        let events = Arc::new(Mutex::new(VecDeque::new()));
        assert_eq!(try_queue_command(&sender, &drops, &events, 7, 1,
            SfuCommand::Candidate { candidate_init: String::new(), target: 1 }), None);
        assert_eq!(try_queue_command(&sender, &drops, &events, 7, 1,
            SfuCommand::Candidate { candidate_init: String::new(), target: 1 }), None);
        assert_eq!(try_queue_command(&sender, &drops, &events, 7, 1,
            SfuCommand::Answer { sdp: String::new(), target: 1 }),
            Some("command_queue_full"));
        assert_eq!(drops.load(Ordering::Relaxed), 2);
        let reports: Vec<serde_json::Value> = events.lock().unwrap().iter()
            .map(|entry| serde_json::from_str(entry).unwrap()).collect();
        assert_eq!(reports.len(), 2);
        assert_eq!(reports[0], json!({"type": "sfu_command_dropped",
            "generation": 7, "connection_id": 1, "dropped": 1,
            "category": "command_queue_full"}));
        assert_eq!(reports[1]["dropped"], 2);
    }

    #[test]
    fn sif_trailer_is_forwarded_base64_and_bounded() {
        use base64::Engine;
        let b64 = base64::engine::general_purpose::STANDARD;

        let trailer = b"6N9Qc1wR2vXz8KkLmP0aBcDeFgHiJkLmNoPqRsTuVwX".to_vec();
        let join = lkp::JoinResponse {
            sif_trailer: trailer.clone(),
            ..Default::default()
        };
        let decoded = lkp::JoinResponse::decode(&join.encode_to_vec()[..])
            .expect("a JoinResponse round-trips");
        let wire = sif_trailer_b64(&decoded.sif_trailer);
        assert_eq!(b64.decode(&wire).expect("valid base64"), trailer);

        assert_eq!(sif_trailer_b64(&[]), "");
        let at_bound = vec![b'A'; MAX_SIF_TRAILER];
        assert_eq!(b64.decode(sif_trailer_b64(&at_bound)).unwrap(), at_bound);
        assert_eq!(sif_trailer_b64(&vec![b'A'; MAX_SIF_TRAILER + 1]), "");
        assert_eq!(sif_trailer_b64(b"R"), "");
        assert_eq!(sif_trailer_b64(b"\x0cR"), "");
        assert_eq!(sif_trailer_b64(&vec![b'A'; MIN_SIF_TRAILER - 1]), "");
        let mut not_base62 = trailer.clone();
        not_base62[3] = b'+';
        assert_eq!(sif_trailer_b64(&not_base62), "");
    }

    #[test]
    fn every_connect_failure_shape_has_its_own_category() {
        use tokio_tungstenite::tungstenite::Error as Ws;
        use std::io::ErrorKind as Kind;

        let io = |kind: Kind| {
            classify_ws_error(&Ws::Io(std::io::Error::new(kind, "x")))
        };
        assert_eq!(io(Kind::NetworkUnreachable), "sfu_unreachable");
        assert_eq!(io(Kind::HostUnreachable), "sfu_unreachable");
        assert_eq!(io(Kind::ConnectionRefused), "sfu_refused_connection");
        assert_eq!(io(Kind::TimedOut), "connect_timeout");
        assert_eq!(io(Kind::PermissionDenied), "connect_blocked");
        assert_eq!(io(Kind::ConnectionReset), "connection_lost");
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
                    tokio::time::sleep(per_attempt).await;
                    Err(tokio_tungstenite::tungstenite::Error::Io(
                        std::io::Error::new(
                            std::io::ErrorKind::ConnectionRefused, "closed")))
                }
            },
        )
        .await;
        let elapsed = started.elapsed();

        assert!(matches!(walk, ConnectWalk::TimedOut),
                "six slow addresses ran to completion instead of being cut \
                 off at the shared deadline");
        assert!(attempts.load(Ordering::SeqCst) < 6,
                "every address was tried despite the budget running out");
        assert!(elapsed < budget + per_attempt * 2,
                "the walk took {elapsed:?} against a {budget:?} budget");

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

    #[test]
    fn each_host_refusal_keeps_its_own_reason() {
        assert_eq!(resolve_refusal_category(HostRefusal::PrivateName),
                   "focus_private_name");
        assert_eq!(resolve_refusal_category(HostRefusal::Unresolved),
                   "focus_unresolved");
        assert_eq!(resolve_refusal_category(HostRefusal::NonPublicAddress("ula")),
                   "focus_unroutable");
    }

    fn own(urls: &[&str]) -> Option<Vec<LivekitTransport>> {
        Some(urls.iter()
            .map(|url| LivekitTransport { service_url: (*url).to_owned(), alias: None })
            .collect())
    }

    async fn credentials_for(service_url: &str, advertised: &[&str])
        -> (Result<SfuCredentials, FocusFailure>, bool)
    {
        let asked = std::cell::Cell::new(false);
        let result = fetch_sfu_credentials(
            service_url, "!room:example.org", "DEVICE", &json!({}),
            || { asked.set(true); std::future::ready(own(advertised)) }).await;
        (result, asked.get())
    }

    #[tokio::test]
    async fn a_credentials_refusal_names_its_reason_instead_of_invalid_transport() {
        let (result, asked) = credentials_for("https://10.0.0.5/", &[]).await;
        let failure = result.expect_err("a private focus is refused");
        assert_eq!(failure.category, "focus_unroutable");
        assert_eq!(failure.address_class, Some("private"));
        assert_eq!(failure.lan_allowance, LanAllowance::NotAdvertised);
        assert!(asked, "a non-public answer must consult the own homeserver");

        let failure = fetch_sfu_credentials(
            "https://10.0.0.5/", "!room:example.org", "DEVICE", &json!({}),
            || std::future::ready(None)).await
            .expect_err("an unreadable well-known grants nothing");
        assert_eq!(failure.lan_allowance, LanAllowance::Unknown);

        let (result, _) = credentials_for(
            "https://10.0.0.5/", &["https://sfu.example.org/"]).await;
        assert_eq!(result.expect_err("another focus is not ours").category,
                   "focus_unroutable");

        let mut event = json!({});
        FocusFailure::refused(HostRefusal::NonPublicAddress("cgnat"), LanAllowance::Applied)
            .fields(&mut event);
        assert_eq!(event, json!({"category": "focus_unroutable",
                                 "address_class": "cgnat", "lan_allowance": "applied"}));
        let mut event = json!({});
        FocusFailure::refused(HostRefusal::Unresolved, LanAllowance::NotApplied)
            .fields(&mut event);
        assert_eq!(event, json!({"category": "focus_unresolved"}));

        for malformed in ["not a url", "https://"] {
            let (result, asked) = credentials_for(malformed, &[]).await;
            assert_eq!(result.expect_err(malformed).category, "invalid_transport");
            assert!(!asked);
        }
    }

    #[tokio::test]
    #[cfg(not(feature = "test-loopback-foci"))]
    async fn a_local_name_is_refused_as_a_name_without_asking_the_homeserver() {
        let (result, asked) = credentials_for(
            "https://sfu.internal/", &["https://sfu.internal/"]).await;
        let failure = result.expect_err("a local-only name is refused");
        assert_eq!(failure.category, "focus_private_name");
        assert_eq!(failure.address_class, None);
        assert!(!asked);
    }

    #[tokio::test]
    async fn only_the_own_homeserver_may_advertise_a_lan_focus() {
        for (host, class) in [("10.0.0.5", "private"), ("100.100.1.2", "cgnat"),
                              ("fd7a:115c:a1e0::1", "ula")] {
            let url = format!("https://{}/", if host.contains(':') {
                format!("[{host}]") } else { host.to_owned() });
            let refused = resolve_focus(&url, host, 443,
                || std::future::ready(own(&["https://other.example.org/"]))).await;
            let failure = refused.expect_err(host);
            assert_eq!((failure.address_class, failure.lan_allowance),
                       (Some(class), LanAllowance::NotAdvertised), "{host}");

            let (addresses, own_infrastructure) = resolve_focus(&url, host, 443,
                || std::future::ready(own(&[url.as_str()]))).await
                .unwrap_or_else(|_| panic!("{host} is ours"));
            assert!(own_infrastructure, "{host}");
            assert_eq!(addresses[0].port(), 443);
        }
    }

    #[tokio::test]
    #[cfg(not(feature = "test-loopback-foci"))]
    async fn the_own_homeserver_still_cannot_aim_at_loopback_or_link_local() {
        for (host, class) in [("127.0.0.1", "loopback"), ("::1", "loopback"),
                              ("169.254.169.254", "link_local"),
                              ("fe80::1", "link_local"),
                              ("64:ff9b::a00:5", "nat64_non_public")] {
            let url = format!("https://{}/", if host.contains(':') {
                format!("[{host}]") } else { host.to_owned() });
            let failure = resolve_focus(&url, host, 443,
                || std::future::ready(own(&[url.as_str()]))).await
                .expect_err(host);
            assert_eq!(failure.category, "focus_unroutable", "{host}");
            assert_eq!(failure.address_class, Some(class), "{host}");
            assert_eq!(failure.lan_allowance, LanAllowance::Applied, "{host}");
        }
    }

    #[test]
    #[cfg(not(feature = "test-loopback-foci"))]
    fn the_signal_host_is_relaxed_only_for_own_infrastructure() {
        let lan: Vec<std::net::SocketAddr> = vec!["10.0.0.5:443".parse().unwrap()];
        assert_eq!(crate::rtc::focus_address_refusal("sfu.example.org", &lan, false),
                   Some("private"));
        assert_eq!(crate::rtc::focus_address_refusal("sfu.example.org", &lan, true), None);
        let mixed: Vec<std::net::SocketAddr> =
            vec!["93.184.216.34:443".parse().unwrap(), "[::1]:443".parse().unwrap()];
        assert_eq!(crate::rtc::focus_address_refusal("sfu.example.org", &mixed, true),
                   Some("loopback"));
        let nat64: Vec<std::net::SocketAddr> =
            vec!["[64:ff9b::5db8:d822]:443".parse().unwrap()];
        assert_eq!(crate::rtc::focus_address_refusal("sfu.example.org", &nat64, false),
                   None);
    }

    #[test]
    fn signal_targets_are_a_closed_set_and_round_trip() {
        assert_eq!(target_str(0), "publisher");
        assert_eq!(target_str(1), "subscriber");
        assert_eq!(target_str(99), "subscriber");
        assert_eq!(target_from_str("publisher"), 0);
        assert_eq!(target_from_str("subscriber"), 1);
        assert_eq!(target_from_str("nonsense"), 1);
    }

    #[test]
    fn participant_without_a_sane_identity_is_dropped() {
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

    fn normalize_sfu_url(raw: &str) -> Option<String> {
        let normalized = super::normalize_sfu_url(raw)?;
        let mut parsed = url::Url::parse(&normalized).ok()?;
        let existing = parsed.path().trim_end_matches('/').to_owned();
        parsed.set_path(&format!("{existing}/rtc"));
        Some(parsed.to_string())
    }

    #[test]
    fn an_https_sfu_url_is_accepted_and_normalised_to_wss() {
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
        assert!(normalize_sfu_url("ws://livekit.example.net").is_none());
        assert!(normalize_sfu_url("http://livekit.example.net").is_none());
        assert!(normalize_sfu_url("not a url").is_none());
        assert!(normalize_sfu_url("wss:").is_none());
    }

    #[test]
    #[cfg(not(feature = "test-loopback-foci"))]
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
    #[cfg(feature = "test-loopback-foci")]
    fn test_sfu_url_allows_only_loopback_exception() {
        for good in ["wss://127.0.0.2:8443/", "wss://[::1]/", "wss://localhost/"] {
            assert!(normalize_sfu_url(good).is_some(), "{good}");
        }
        for bad in ["ws://127.0.0.1/", "wss://10.0.0.5/", "wss://169.254.1.1/",
                    "wss://[fc00::1]/", "wss://[::ffff:127.0.0.1]/"] {
            assert!(normalize_sfu_url(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn the_rtc_path_is_appended_never_substituted() {
        assert_eq!(
            normalize_sfu_url("https://host.example.net/livekit").as_deref(),
            Some("wss://host.example.net/livekit/rtc")
        );
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
        assert_eq!(lkp::TrackSource::Camera as i32, 1);
        assert_eq!(lkp::TrackSource::Microphone as i32, 2);
        assert_eq!(lkp::TrackSource::ScreenShare as i32, 3);
        assert_eq!(lkp::TrackSource::ScreenShareAudio as i32, 4);
        assert_eq!(lkp::TrackType::Audio as i32, 0);
        assert_eq!(lkp::TrackType::Video as i32, 1);
        assert_eq!(lkp::encryption::Type::None as i32, 0);
        assert_eq!(lkp::encryption::Type::Gcm as i32, 1);
    }

    #[test]
    fn added_tracks_carry_the_right_source() {
        assert_eq!(track_source_for(1, true), 3);
        assert_eq!(track_source_for(0, true), 4);
        assert_eq!(track_source_for(1, false), 1);
        assert_eq!(track_source_for(0, false), 2);

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
        assert_ne!(track_source_for(1, true),
                   lkp::TrackSource::ScreenShareAudio as i32);
        assert_ne!(track_source_for(1, true), track_source_for(1, false));

        assert_eq!(track_encryption_for(true), 1);
        assert_eq!(track_encryption_for(false), 0);
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
                    source: 99,
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
        assert_eq!(tracks[2]["source"], json!("unknown"));
    }

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
        assert_ne!(tracks[0]["mid"], tracks[1]["mid"]);
        assert_eq!(tracks[1]["stream"], json!(""));
    }

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
    fn a_leave_that_asks_us_back_keeps_the_call() {
        use lkp::leave_request::Action;
        let decode = |leave: lkp::LeaveRequest| {
            let response = lkp::SignalResponse {
                message: Some(lkp::signal_response::Message::Leave(leave)),
            };
            match lkp::SignalResponse::decode(&response.encode_to_vec()[..])
                .expect("a SignalResponse round-trips")
                .message
            {
                Some(lkp::signal_response::Message::Leave(leave)) => {
                    leave_disposition(leave.action, leave.can_reconnect)
                }
                _ => panic!("not a Leave"),
            }
        };
        assert_eq!(
            decode(lkp::LeaveRequest {
                reason: lkp::DisconnectReason::ConnectionTimeout as i32,
                action: Action::Resume as i32,
                ..Default::default()
            }),
            "reconnecting",
            "Leave{{CONNECTION_TIMEOUT, RESUME}} ended the call");
        assert_eq!(
            decode(lkp::LeaveRequest {
                action: Action::Reconnect as i32,
                ..Default::default()
            }),
            "reconnecting");
        assert_eq!(
            decode(lkp::LeaveRequest {
                reason: lkp::DisconnectReason::ParticipantRemoved as i32,
                action: Action::Disconnect as i32,
                ..Default::default()
            }),
            "ended",
            "a DISCONNECT (removed, room deleted) must end the call");
        assert_eq!(
            decode(lkp::LeaveRequest {
                can_reconnect: true,
                ..Default::default()
            }),
            "reconnecting");
        assert_eq!(decode(lkp::LeaveRequest::default()), "ended");
        assert_eq!(leave_disposition(99, false), "ended");
        assert_eq!(leave_disposition(99, true), "reconnecting");
        assert_eq!(Action::Resume as i32, 1);
        assert_eq!(lkp::DisconnectReason::ConnectionTimeout as i32, 14);
    }

    #[test]
    fn the_signalling_watchdog_follows_the_servers_ping_timeout() {
        assert_eq!(signal_silence_limit(0, 10), None);
        assert_eq!(signal_silence_limit(-5, 10), None);
        assert_eq!(signal_silence_limit(20, 10), Some(Duration::from_secs(20)));
        assert_eq!(signal_silence_limit(5, 10), Some(Duration::from_secs(12)));
        assert_eq!(signal_silence_limit(1, 1), Some(Duration::from_secs(5)));
        assert_eq!(signal_silence_limit(100_000, 1),
                   Some(Duration::from_secs(120)));
    }

    #[test]
    fn a_signal_request_round_trips_through_prost() {
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

    fn resume_query(url: &url::Url) -> BTreeMap<String, String> {
        url.query_pairs().map(|(k, v)| (k.into_owned(), v.into_owned())).collect()
    }

    #[test]
    fn a_resume_names_the_session_it_reattaches_to() {
        let base = url::Url::parse("wss://sfu.example.net/livekit/rtc").unwrap();
        let fresh = resume_query(&signal_url(&base, "tok", None));
        assert_eq!(fresh.get("access_token").map(String::as_str), Some("tok"));
        assert!(!fresh.contains_key("reconnect"));
        assert!(!fresh.contains_key("sid"));

        let resumed = resume_query(&signal_url(&base, "tok2", Some((
            "PA_abc", lkp::ReconnectReason::RrSignalDisconnected as i32))));
        assert_eq!(resumed.get("reconnect").map(String::as_str), Some("1"));
        assert_eq!(resumed.get("sid").map(String::as_str), Some("PA_abc"));
        assert_eq!(resumed.get("reconnect_reason").map(String::as_str), Some("1"));
        assert_eq!(resumed.get("access_token").map(String::as_str), Some("tok2"));
        assert_eq!(resumed.get("protocol").map(String::as_str),
                   Some(LK_PROTOCOL_VERSION.to_string().as_str()));
    }

    #[test]
    fn only_a_resume_leave_is_resumed() {
        use lkp::leave_request::Action;
        assert!(leave_asks_resume(Action::Resume as i32));
        assert!(!leave_asks_resume(Action::Reconnect as i32));
        assert!(!leave_asks_resume(Action::Disconnect as i32));
        assert!(!leave_asks_resume(99));
    }

    #[test]
    fn resumes_need_a_session_and_are_bounded() {
        let start = std::time::Instant::now();
        let mut recent = VecDeque::new();
        assert!(!may_resume(None, &mut recent, start));
        assert!(!may_resume(Some(""), &mut recent, start));
        assert!(recent.is_empty());
        for _ in 0..MAX_RESUMES_PER_WINDOW {
            assert!(may_resume(Some("PA_x"), &mut recent, start));
        }
        assert!(!may_resume(Some("PA_x"), &mut recent, start + Duration::from_secs(1)));
        assert!(may_resume(Some("PA_x"), &mut recent, start + RESUME_WINDOW));
    }

    fn frame(message: lkp::signal_response::Message)
        -> Result<WsMessage, tokio_tungstenite::tungstenite::Error>
    {
        let mut bytes = Vec::new();
        lkp::SignalResponse { message: Some(message) }.encode(&mut bytes).unwrap();
        Ok(WsMessage::Binary(bytes))
    }

    #[tokio::test]
    async fn the_resume_answer_is_the_reconnect_response_or_a_leave() {
        let mut stray_then_reconnect = futures_util::stream::iter(vec![
            Ok(WsMessage::Pong(Vec::new())),
            frame(lkp::signal_response::Message::Update(Default::default())),
            frame(lkp::signal_response::Message::Reconnect(Default::default())),
        ]);
        assert_eq!(await_resume_answer(&mut stray_then_reconnect).await,
                   ResumeAnswer::Resumed);

        let mut refused = futures_util::stream::iter(vec![
            frame(lkp::signal_response::Message::Leave(lkp::LeaveRequest {
                action: lkp::leave_request::Action::Reconnect as i32,
                reason: lkp::DisconnectReason::StateMismatch as i32,
                ..Default::default()
            })),
        ]);
        assert_eq!(await_resume_answer(&mut refused).await, ResumeAnswer::Refused);

        let mut closed = futures_util::stream::iter(vec![Ok(WsMessage::Close(None))]);
        assert_eq!(await_resume_answer(&mut closed).await, ResumeAnswer::Lost);
        let mut empty = futures_util::stream::iter(
            Vec::<Result<WsMessage, tokio_tungstenite::tungstenite::Error>>::new());
        assert_eq!(await_resume_answer(&mut empty).await, ResumeAnswer::Lost);
    }

    #[test]
    fn subscriber_candidates_are_replayed_once_after_a_resume() {
        let mut candidates = SubscriberCandidates::default();
        candidates.record("a");
        candidates.record("b");
        assert!(candidates.after_answer().is_empty(), "no resume, no replay");
        candidates.arm();
        assert_eq!(candidates.after_answer(), vec!["a".to_owned(), "b".to_owned()]);
        assert!(candidates.after_answer().is_empty(), "replayed once");
        for i in 0..(MAX_RESUME_CANDIDATES + 5) {
            candidates.record(&i.to_string());
        }
        candidates.arm();
        let replay = candidates.after_answer();
        assert_eq!(replay.len(), MAX_RESUME_CANDIDATES);
        assert_eq!(replay.last().map(String::as_str),
                   Some((MAX_RESUME_CANDIDATES + 4).to_string().as_str()));
    }

    async fn fake_sfu(answer: Option<lkp::signal_response::Message>)
        -> (std::net::SocketAddr, tokio::task::JoinHandle<String>)
    {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let task = tokio::spawn(async move {
            let (tcp, _) = listener.accept().await.unwrap();
            let mut seen = String::new();
            let mut ws = tokio_tungstenite::accept_hdr_async(tcp,
                |request: &tokio_tungstenite::tungstenite::handshake::server::Request,
                 response| {
                    seen = request.uri().to_string();
                    Ok(response)
                }).await.unwrap();
            if let Some(message) = answer {
                let mut bytes = Vec::new();
                lkp::SignalResponse { message: Some(message) }.encode(&mut bytes).unwrap();
                ws.send(WsMessage::Binary(bytes)).await.unwrap();
            }
            let _ = tokio::time::timeout(Duration::from_secs(2), ws.next()).await;
            seen
        });
        (address, task)
    }

    #[tokio::test]
    async fn a_resume_reattaches_with_the_session_id_and_takes_the_reconnect_response() {
        let (address, server) = fake_sfu(Some(
            lkp::signal_response::Message::Reconnect(Default::default()))).await;
        let base = url::Url::parse(&format!("ws://{address}/rtc")).unwrap();
        let result = resume_signal(&base, "tok", "PA_resume",
            lkp::ReconnectReason::RrSignalDisconnected as i32, &[address]).await;
        let mut stream = result.expect("resumed");
        let _ = stream.close(None).await;
        let uri = server.await.unwrap();
        assert!(uri.contains("reconnect=1"), "{uri}");
        assert!(uri.contains("sid=PA_resume"), "{uri}");
        assert!(uri.contains("reconnect_reason=1"), "{uri}");
    }

    #[tokio::test]
    async fn a_refused_resume_falls_back_to_a_full_rejoin() {
        let (address, server) = fake_sfu(Some(lkp::signal_response::Message::Leave(
            lkp::LeaveRequest {
                action: lkp::leave_request::Action::Reconnect as i32,
                reason: lkp::DisconnectReason::StateMismatch as i32,
                ..Default::default()
            }))).await;
        let base = url::Url::parse(&format!("ws://{address}/rtc")).unwrap();
        let result = resume_signal(&base, "tok", "PA_gone", 0, &[address]).await;
        assert_eq!(result.err(), Some("resume_refused"));
        server.await.unwrap();
    }
}
