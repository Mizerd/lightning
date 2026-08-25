//! LiveKit SFU authorization and signalling (MatrixRTC phase 2).
//!
//! This module is the SIGNALLING half of group calling. It obtains
//! authorization for the SFU the homeserver advertised, speaks LiveKit's
//! WebSocket/protobuf signalling protocol, and reports participants, tracks
//! and session descriptions across the FFI. **It owns no media**: the actual
//! RTP flows through Lightning's existing GStreamer `webrtcbin` engine on the
//! C++ side, exactly as the legacy 1:1 lane already works.
//!
//! ## Why not the `livekit` client crate
//!
//! The official Rust client would bring its own media stack: it depends on
//! `webrtc-sys`, which DOWNLOADS a prebuilt libwebrtc during the build. That
//! breaks this crate's `--offline --locked` contract outright, and measured
//! at 318 extra crates and ~1.7 GB of build artifacts on a tree that already
//! links a 2.1 GB debug staticlib into ~150 test binaries. `livekit-protocol`
//! is pure message definitions — no media, no download — so Lightning speaks
//! the same wire with the engine it already ships.
//!
//! ## Authorization, and what never leaves the client
//!
//! The SFU is authorized with a **Matrix OpenID token**, not the access
//! token: `POST {service_url}/sfu/get` with `{room, openid_token, device_id}`
//! answers `{url, jwt}`. So the user's Matrix credentials never reach the
//! SFU, and the SFU's JWT never reaches Matrix. Neither is logged, neither
//! crosses the FFI, and neither is persisted — the JWT lives in the
//! signalling task for the lifetime of one connection.
//!
//! ## Safety rules specific to this surface
//!
//! * The SFU is a party outside the homeserver's trust boundary. Everything
//!   it sends is bounded and sanitized before it crosses the FFI, and
//!   participant identities are compared, never rendered raw.
//! * SDP carries host IPs and ICE credentials. It crosses only in
//!   media-capable mode (the same `AtomicBool` the legacy lane uses), is
//!   never logged, and is never exposed to QML.
//! * One connection at a time, owned by an explicit generation counter, so a
//!   late frame from a closed session can never be attributed to the next.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use livekit_protocol as lkp;

/// The LiveKit `TrackSource` for a track we are about to publish.
///
/// Exists as a named function so the test and the production path cannot
/// disagree: the first version of this mapping was written inline with
/// literals and every value was off by one, which published our screen share
/// as SCREEN_SHARE_AUDIO — a track Element Call treats as audio and never
/// renders.
pub(crate) fn track_source_for(kind: i32, screen_share: bool) -> i32 {
    if screen_share {
        lkp::TrackSource::ScreenShare as i32
    } else if kind == lkp::TrackType::Video as i32 {
        lkp::TrackSource::Camera as i32
    } else {
        lkp::TrackSource::Microphone as i32
    }
}

/// LiveKit's per-track E2EE declaration. A receiving client reads this to
/// decide whether to run its frame decryptor at all, so encrypting the bytes
/// while declaring None renders as garbage at the far end.
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

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// Bound on the JWT-service response body. A `{url, jwt}` object is small.
const MAX_SFU_RESPONSE: usize = 64 * 1024;
const SFU_TIMEOUT: Duration = Duration::from_secs(20);
/// LiveKit signalling frames are small; a media-free protocol has no reason
/// to send megabytes, and this is a party outside the homeserver's trust.
const MAX_SIGNAL_FRAME: usize = 256 * 1024;
/// Cap on how many participants and tracks are tracked/reported.
const MAX_PARTICIPANTS: usize = 128;
const MAX_TRACKS_PER_PARTICIPANT: usize = 8;
/// The signalling protocol version this client implements.
const LK_PROTOCOL_VERSION: u32 = 15;

/// Bounded, control-character-free, or nothing. Same discipline as rtc.rs:
/// a `None` means drop the surrounding thing rather than repair it.
fn sane(value: &str, max: usize) -> Option<&str> {
    if value.is_empty() || value.len() > max {
        return None;
    }
    if value.chars().any(|c| c.is_control()) {
        return None;
    }
    Some(value)
}

/// Which peer connection a description or candidate belongs to.
///
/// LiveKit runs TWO: the client offers on PUBLISHER (its own tracks), the
/// server offers on SUBSCRIBER (everyone else's). Mixing them up wires audio
/// to the wrong direction, so the target rides every message.
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
// JWT service
// ---------------------------------------------------------------------------

/// What `POST {service_url}/sfu/get` answered.
#[derive(Clone, Debug)]
pub(crate) struct SfuCredentials {
    /// The SFU's own websocket URL (`wss://…`). NOT the JWT service URL.
    pub url: String,
    /// Short-lived SFU authorization. Never logged, never crosses the FFI.
    pub jwt: String,
}

/// Obtain SFU credentials for one room.
///
/// The OpenID token is minted by the homeserver for exactly this purpose:
/// it lets the JWT service verify who the user is, by asking their
/// homeserver, without ever seeing a Matrix access token.
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

    // Ask the homeserver to vouch for us. This token is scoped and
    // short-lived; it is not a credential for anything else.
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

    // The service URL was validated as https with a routable host when the
    // transport was parsed (rtc::sane_https_url); this joins the documented
    // path onto it without letting a crafted URL escape.
    let mut url = url::Url::parse(service_url)
        .map_err(|_| "invalid_transport".to_owned())?;
    url.path_segments_mut()
        .map_err(|_| "invalid_transport".to_owned())?
        .pop_if_empty()
        .extend(["sfu", "get"]);

    let response = client
        .http_client()
        .post(url.to_string())
        .timeout(SFU_TIMEOUT)
        .header(reqwest::header::CONTENT_TYPE, "application/json")
        .body(body)
        .send()
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;

    let status = response.status().as_u16();
    if !(200..300).contains(&status) {
        return Err(match status {
            401 | 403 => "forbidden".to_owned(),
            404 => "unsupported".to_owned(),
            429 => "rate_limited".to_owned(),
            500..=599 => "server_error".to_owned(),
            _ => "unknown".to_owned(),
        });
    }
    if response
        .content_length()
        .is_some_and(|len| len > MAX_SFU_RESPONSE as u64)
    {
        return Err("invalid".to_owned());
    }
    let text = response.text().await.unwrap_or_default();
    if text.len() > MAX_SFU_RESPONSE {
        return Err("invalid".to_owned());
    }
    let parsed: serde_json::Value =
        serde_json::from_str(&text).map_err(|_| "invalid".to_owned())?;

    // The SFU websocket URL is remote-supplied and is where media
    // authorization is presented, so cleartext is refused — the JWT travels
    // in this URL's query string.
    //
    // `https` is ACCEPTED and normalised to `wss`. lk-jwt-service echoes its
    // configured LIVEKIT_URL verbatim, and `https://…` is a perfectly normal
    // value there (livekit-client does the same conversion in
    // `toWebsocketUrl`). Requiring `wss` outright rejected a correctly
    // configured deployment and failed the call instantly with "invalid".
    let raw_url = parsed.get("url").and_then(|v| v.as_str()).unwrap_or("");
    let url_ok = sane(raw_url, 1024)
        .and_then(|value| url::Url::parse(value).ok())
        .and_then(|mut parsed| {
            match parsed.scheme() {
                "wss" => {}
                // set_scheme can only fail between incompatible special
                // schemes; https -> wss is allowed.
                "https" => parsed.set_scheme("wss").ok()?,
                // ws / http would carry the JWT in the clear.
                _ => return None,
            }
            // has_host() alone is TRUE for an empty host ("wss:///rtc"),
            // which normalises cleanly and then cannot connect to anything.
            parsed
                .host_str()
                .is_some_and(|host| !host.is_empty())
                .then(|| parsed.to_string())
        })
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

/// How long `disconnect` lets the session task drain its queued Leave and
/// close the websocket before the abort backstop fires.
///
/// Comfortably inside `timeline::SHUTDOWN_JOIN_TIMEOUT_SECS` (15 s), which is
/// the budget that joins this task on quit: the graceful close must finish
/// well before the budget it rides, or quitting would abort it anyway and we
/// would be back to leaking a participant.
const LEAVE_FLUSH_TIMEOUT: Duration = Duration::from_secs(3);

/// Commands the C++ side sends into a running signalling session.
#[derive(Debug)]
pub(crate) enum SfuCommand {
    /// A local description for one peer connection.
    Offer { sdp: String, target: i32 },
    Answer { sdp: String, target: i32 },
    /// One trickled local ICE candidate (`candidate_init` is the JSON form
    /// LiveKit expects).
    Candidate { candidate_init: String, target: i32 },
    /// Declare a track before publishing it.
    AddTrack {
        cid: String,
        name: String,
        /// 0 = audio, 1 = video
        kind: i32,
        /// Video only, and 0 for audio. See the AddTrack arm: a video track
        /// declared with no size and no layer leaves the SFU to guess, and it
        /// guesses SIMULCAST.
        width: u32,
        height: u32,
        /// True for a screen share, which LiveKit sources separately.
        screen_share: bool,
        /// Whether the frames on this track are E2EE-encrypted. LiveKit
        /// carries this per TRACK, and a receiving client decides whether
        /// to decrypt from it — encrypting the bytes while declaring NONE
        /// means Element renders our frames as garbage rather than trying.
        encrypted: bool,
    },
    /// Mute/unmute a published track at the SFU, so other participants see
    /// the state even though the valve already stopped the bytes.
    MuteTrack { sid: String, muted: bool },
    Leave,
}

/// The one live signalling session. Guarded by a generation so a late frame
/// from a closed session can never be attributed to the next one.
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
    // `identity` is what maps an SFU participant back to a Matrix device:
    // MatrixRTC sets it to the membership's rtc identity.
    let identity = sane(&info.identity, 512)?;
    let tracks: Vec<serde_json::Value> = info
        .tracks
        .iter()
        .take(MAX_TRACKS_PER_PARTICIPANT)
        .filter_map(|track| {
            let sid = sane(&track.sid, 256)?;
            Some(json!({
                "sid": sid,
                // Closed set: never the raw enum from the wire.
                "kind": if track.r#type == 1 { "video" } else { "audio" },
                // Named constants, never literals: these were once written
                // out by hand and every one was off by one, which made our
                // screen share arrive at Element as SCREEN_SHARE_AUDIO and
                // never render.
                "source": match lkp::TrackSource::try_from(track.source) {
                    Ok(lkp::TrackSource::Camera) => "camera",
                    Ok(lkp::TrackSource::Microphone) => "microphone",
                    Ok(lkp::TrackSource::ScreenShare) => "screen_share",
                    Ok(lkp::TrackSource::ScreenShareAudio)
                        => "screen_share_audio",
                    _ => "unknown",
                },
                "muted": track.muted,
                // The media-section id this track was negotiated on. THE
                // authoritative pad-to-track mapping: the subscriber SDP's
                // `a=mid:` for a section names exactly this, so a receiver
                // can tell a participant's camera from their screen share
                // instead of guessing from an msid that carries only the
                // sending participant. Bounded like every other wire string.
                "mid": sane(&track.mid, 128).unwrap_or_default(),
                // LiveKit's own stream id for the track, when the server
                // states it. Used only as a fallback key.
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

/// Encode and send one signalling request. `false` means the socket is gone.
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

/// Run one signalling session to completion.
///
/// Every enqueue is gated on the session generation still being current, so
/// nothing from a closed call reaches a later one.
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

    // LiveKit takes its authorization in the query string of the signalling
    // URL. That is the protocol; the JWT is short-lived, the connection is
    // wss, and this URL is never logged or enqueued.
    let mut url = match url::Url::parse(&credentials.url) {
        Ok(url) => url,
        Err(_) => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "invalid",
            }));
            return;
        }
    };
    // `/rtc` is APPENDED to whatever path the SFU URL already has, never
    // substituted for it. `set_path("/rtc")` discarded the prefix, so a
    // LiveKit behind a reverse proxy at, say, `https://host/livekit` was
    // asked for `/rtc` at the root and the handshake could not succeed.
    // livekit-client appends too; the known double-slash bug in its own
    // issue tracker is the same join being done less carefully.
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

    let connect = tokio_tungstenite::connect_async(url.as_str());
    let stream = match tokio::time::timeout(SFU_TIMEOUT, connect).await {
        Ok(Ok((stream, _))) => stream,
        Ok(Err(_)) => {
            // The error string can embed the URL, which carries the JWT.
            // Only a closed-set category ever leaves this scope.
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "connect_failed",
            }));
            return;
        }
        Err(_) => {
            emit(json!({
                "type": "sfu_state", "generation": generation,
                "state": "failed", "category": "network",
            }));
            return;
        }
    };

    let (mut sink, mut source) = stream.split();
    emit(json!({
        "type": "sfu_state", "generation": generation, "state": "signalling",
        "category": "",
    }));

    // LiveKit's APPLICATION-LEVEL keepalive.
    //
    // This is not the WebSocket ping/pong frame handled below — the server
    // requires a `ping`/`ping_req` SIGNAL and disconnects a client that stops
    // sending them, which is exactly what happened here: the session reached
    // `signalling` and then the SFU sent Leave, every time, in every room.
    //
    // The interval comes from JoinResponse (`ping_interval` seconds); until
    // the join lands, the ticker is parked far in the future so nothing is
    // sent before the server has told us what it wants. livekit-client sends
    // BOTH the deprecated `ping` and the newer `ping_req` on every tick, and
    // this does the same: the pair costs nothing and covers servers on either
    // side of that change.
    let mut ping_ticker =
        tokio::time::interval(std::time::Duration::from_secs(3600));
    ping_ticker.set_missed_tick_behavior(
        tokio::time::MissedTickBehavior::Delay);
    // The first tick of a tokio interval fires immediately; consume it so the
    // parked interval does not ping before the join.
    ping_ticker.tick().await;
    let mut ping_armed = false;
    // LiveKit correlates an answer with the offer it answers through
    // `SessionDescription.id`. It is NOT the peer-connection target, which is
    // what this used to send: the server generates its offer ids from a
    // RANDOM base (`rand.Intn(1<<8)+1`, then incremented), so a hardcoded
    // value matched only by accident and the server logged an "answer id
    // mismatch" against every answer we ever sent. livekit-client keeps the
    // last remote offer id and echoes it back (`latestRemoteOfferId`), and
    // numbers its OWN offers with a counter; both are done here.
    // Index 0 = publisher, 1 = subscriber, matching `target_str`.
    let mut remote_offer_id = [0u32; 2];
    let mut local_offer_id = [0u32; 2];

    loop {
        tokio::select! {
            _ = ping_ticker.tick(), if ping_armed => {
                let now_ms = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_millis() as i64)
                    .unwrap_or(0);
                // send_request returns false on a write failure — the
                // socket is gone, so stop rather than tick into a dead sink.
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
                                // The id of the offer this answers, echoed
                                // back. 0 means "we never saw an offer", and
                                // the server treats 0 as unset rather than as
                                // a mismatch.
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
                        // ONE explicit layer for video, with real dimensions.
                        //
                        // livekit-client always sets `req.width`/`req.height`
                        // (it waits on `track.waitForDimensions()` to do it),
                        // and the proto says of VideoLayer.quality: "for
                        // tracks with a single layer, this should be HIGH".
                        // Declaring neither leaves the SFU to infer the shape
                        // of the track — it logs `sdpRids ["q","h","f"]`, the
                        // three-layer simulcast default — while we publish a
                        // single untagged stream, so what it forwards and what
                        // we send do not describe the same track.
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
                                // RED (RFC 2198 redundant audio) and frame
                                // E2EE are mutually exclusive, and the
                                // reference says so in one line:
                                // livekit-client sets
                                // `disableRed: this.isE2EEEnabled || …`.
                                // RED wraps the Opus payload, so a receiver
                                // hands its frame decryptor a RED packet
                                // whose first byte is not the Opus TOC the
                                // format leaves in the clear — every frame
                                // then fails its authentication tag and is
                                // dropped. Left false, an SFU is free to
                                // apply RED to our audio and nobody can
                                // decrypt a word of it.
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
                        // Arm the keepalive at the interval the SERVER asked
                        // for, clamped: a hostile or misconfigured 0 would
                        // spin, and an absurd value would be no keepalive at
                        // all. livekit's own default is 15s/30s.
                        let interval = join.ping_interval.clamp(1, 120) as u64;
                        ping_ticker = tokio::time::interval(
                            std::time::Duration::from_secs(interval));
                        ping_ticker.set_missed_tick_behavior(
                            tokio::time::MissedTickBehavior::Delay);
                        ping_ticker.tick().await;   // consume the immediate one
                        ping_armed = true;
                        // OUR OWN row FIRST, then the others.
                        //
                        // `JoinResponse.participant` is the local
                        // participant and `other_participants` is everyone
                        // else — livekit-client keeps them apart, but this
                        // bridge carries ONE list, and leaving ours out of it
                        // meant the call stage could never draw the local
                        // tile from the join alone, and `ownParticipantRow()`
                        // (which is how a mute reaches the SFU) had nothing
                        // to find until the server happened to send an update
                        // about us.
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
                        // ICE servers the SFU names. Credentials among them
                        // are short-lived and engine-only, exactly like the
                        // homeserver's TURN answer.
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
                        }));
                    }
                    lkp::signal_response::Message::Offer(sdp) => {
                        // The server offers on SUBSCRIBER: this is everyone
                        // else's media arriving. Its id has to survive until
                        // our answer is built, or the answer cannot name the
                        // offer it answers.
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
                        // The REASON, not just the fact. Discarding it cost a
                        // whole debugging round: "the server told us to
                        // leave" with no reason is unactionable, and the
                        // reason is a closed enum, not content.
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

/// Connect to the SFU named by `service_url` for `room_id`.
///
/// Tears down any existing session first: one media call at a time, and the
/// generation bump means the old session's frames stop being reported the
/// instant this is called.
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

/// Send one command into the live session. Silently ignored when there is
/// none — a command for a call that has ended is not an error.
pub(crate) fn send_command(bridge: &RustClient, command: SfuCommand) {
    if let Ok(guard) = bridge.sfu.session.lock() {
        if let Some(session) = guard.as_ref() {
            let _ = session.commands.send(command);
        }
    }
}

/// Tear down the live session.
///
/// Bumping the generation FIRST is what makes this safe: the running task
/// may already be mid-`await`, and every enqueue it can still reach checks
/// the generation, so nothing from the old call reaches the next one.
pub(crate) fn disconnect(bridge: &RustClient) {
    bridge.sfu_generation.fetch_add(1, Ordering::SeqCst);
    let session = bridge.sfu.session.lock().ok().and_then(|mut g| g.take());
    if let Some(session) = session {
        let SfuSession { commands, task, .. } = session;
        let _ = commands.send(SfuCommand::Leave);

        // THE LEAVE HAS TO REACH THE WIRE, AND ABORTING HERE GUARANTEED IT
        // NEVER DID.
        //
        // `commands.send` only queues onto an unbounded channel — it schedules
        // nothing. The session task was still parked on its `select!` and had
        // not been polled since, so the immediate `task.abort()` that used to
        // stand here cancelled it at that await point with the Leave still
        // sitting unread in the channel. The LiveKit SFU therefore never
        // learned we had gone, held the participant open until its own peer
        // timeout, and every rejoin added ANOTHER copy of us: the maintainer's
        // "multiple same users sit in the call", each labelled waiting for
        // media because a stale publisher has no tracks.
        //
        // So give the task a bounded window to drain the Leave and close its
        // socket, and keep abort as what it always claimed to be — the backstop
        // for a task blocked on a dead connection.
        //
        // Two details this depends on:
        //  * `abort_handle()` is taken BEFORE the JoinHandle moves into the
        //    timeout. A timed-out `timeout(d, handle)` DROPS the handle, and
        //    dropping a JoinHandle DETACHES the task rather than cancelling it
        //    — the backstop would be silently gone.
        //  * `commands` is held alive for the window. Dropping the sender is a
        //    second end-of-stream signal; the queued Leave would still be
        //    delivered first, but there is no reason to run that race.
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
    fn signal_targets_are_a_closed_set_and_round_trip() {
        // Publisher and subscriber must never be confused: the client offers
        // on publisher (its own tracks) and the server offers on subscriber
        // (everyone else's). Swapping them wires audio the wrong way.
        assert_eq!(target_str(0), "publisher");
        assert_eq!(target_str(1), "subscriber");
        // Anything unrecognised degrades to subscriber, never to publisher —
        // guessing "publisher" would attach a remote description to our own
        // outgoing peer connection.
        assert_eq!(target_str(99), "subscriber");
        assert_eq!(target_from_str("publisher"), 0);
        assert_eq!(target_from_str("subscriber"), 1);
        assert_eq!(target_from_str("nonsense"), 1);
    }

    #[test]
    fn participant_without_a_sane_identity_is_dropped() {
        // The identity is what maps an SFU participant to a Matrix device.
        // A control character in it means the whole participant is refused,
        // never repaired.
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

    /// The two URL transforms the LiveKit signalling connect depends on,
    /// extracted so they can be asserted without a live SFU.
    fn normalize_sfu_url(raw: &str) -> Option<String> {
        let mut parsed = url::Url::parse(raw).ok()?;
        match parsed.scheme() {
            "wss" => {}
            "https" => parsed.set_scheme("wss").ok()?,
            _ => return None,
        }
        if !parsed.host_str().is_some_and(|host| !host.is_empty()) {
            return None;
        }
        let existing = parsed.path().trim_end_matches('/').to_owned();
        parsed.set_path(&format!("{existing}/rtc"));
        Some(parsed.to_string())
    }

    #[test]
    fn an_https_sfu_url_is_accepted_and_normalised_to_wss() {
        // lk-jwt-service echoes its configured LIVEKIT_URL verbatim, and
        // `https://…` is a normal value there — livekit-client converts it in
        // `toWebsocketUrl`. Requiring `wss` outright rejected a correctly
        // configured deployment and failed the call instantly with "invalid",
        // which is what "calls insta fail" looked like.
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
        // The JWT travels in this URL's query string, so ws/http would put a
        // media authorization credential on the wire in the clear. Accepting
        // https is a normalisation; accepting ws would be a downgrade.
        assert!(normalize_sfu_url("ws://livekit.example.net").is_none());
        assert!(normalize_sfu_url("http://livekit.example.net").is_none());
        assert!(normalize_sfu_url("not a url").is_none());
        // NOT `wss:///rtc` — measured, that parses with host "rtc" (the
        // triple slash collapses), so it is a well-formed URL for a host
        // that simply will not resolve. A genuinely hostless form is what
        // has to be refused.
        assert!(normalize_sfu_url("wss:").is_none());
        assert!(normalize_sfu_url("wss://").is_none());
    }

    #[test]
    fn the_rtc_path_is_appended_never_substituted() {
        // A LiveKit behind a reverse proxy carries a path prefix.
        // `set_path("/rtc")` discarded it and asked the root for /rtc, so the
        // handshake could not succeed.
        assert_eq!(
            normalize_sfu_url("https://host.example.net/livekit").as_deref(),
            Some("wss://host.example.net/livekit/rtc")
        );
        // A trailing slash must not produce a double slash — the exact shape
        // livekit-client has its own bug report about.
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
        // lk-jwt-service declares its request types with
        // #[serde(deny_unknown_fields)], so ONE extra field is a 400. The
        // legacy /sfu/get body is exactly `room`, `openid_token` and
        // `device_id`; the openid token is the homeserver's response verbatim.
        // Probed live against a real deployment: an empty body answers
        // `M_BAD_JSON: Missing room parameter`, which is this handler.
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
        // The values Element Call and every livekit-client read. They were
        // hand-written once and every one was off by one, which put our
        // screen share on SCREEN_SHARE_AUDIO — a track Element renders as
        // audio and never shows. Pinned as NUMBERS on purpose: an
        // assertion written in terms of the same enum could not have
        // caught the original defect.
        assert_eq!(lkp::TrackSource::Camera as i32, 1);
        assert_eq!(lkp::TrackSource::Microphone as i32, 2);
        assert_eq!(lkp::TrackSource::ScreenShare as i32, 3);
        assert_eq!(lkp::TrackSource::ScreenShareAudio as i32, 4);
        assert_eq!(lkp::TrackType::Audio as i32, 0);
        assert_eq!(lkp::TrackType::Video as i32, 1);
        // The E2EE declaration. Element Call publishes GCM when the room is
        // encrypted, and a receiver reads this to decide whether to run the
        // frame decryptor at all.
        assert_eq!(lkp::encryption::Type::None as i32, 0);
        assert_eq!(lkp::encryption::Type::Gcm as i32, 1);
    }

    #[test]
    fn added_tracks_carry_the_right_source() {
        // Calls the SAME function the AddTrack path calls, in the raw numbers
        // that go on the wire. Asserting through the enum would pass against
        // the original off-by-one defect, which is why these are literals.
        assert_eq!(track_source_for(1, true), 3);   // screen share
        assert_eq!(track_source_for(1, false), 1);  // camera
        assert_eq!(track_source_for(0, false), 2);  // microphone
        // A screen share is a VIDEO track whose source is the screen, never
        // SCREEN_SHARE_AUDIO — the original bug, in one assertion.
        assert_ne!(track_source_for(1, true),
                   lkp::TrackSource::ScreenShareAudio as i32);
        // Screen share must not be mistaken for a camera either: Element
        // lays the two out differently and pins a share to the stage.
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
        // An unrecognised source must not be forwarded verbatim; it becomes
        // "unknown" so the UI cannot act on a value it does not understand.
        assert_eq!(tracks[2]["source"], json!("unknown"));
    }

    // A camera and a screen share from ONE participant are two tracks, and a
    // receiver keyed only on the participant can feed exactly one surface —
    // which is why a remote screen share never rendered. `mid` is the
    // media-section id LiveKit states per track and the subscriber SDP repeats
    // as `a=mid:`, so it is what tells the two apart.
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
        // A server that states no stream id must not produce a null the UI
        // would have to special-case; absent is the empty string.
        assert_eq!(tracks[1]["stream"], json!(""));
    }

    // Wire strings are bounded like every other one: a mid is an SDP token,
    // and an absurd or control-laden value is dropped rather than forwarded
    // into a routing key.
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
        // Guards the wire encoding itself: if the protocol crate's tags ever
        // shift under us, this fails rather than the SFU silently ignoring
        // our offers.
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
