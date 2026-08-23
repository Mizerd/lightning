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
    // authorization is presented, so it gets the same scheme discipline as
    // the transport itself: wss only.
    let raw_url = parsed.get("url").and_then(|v| v.as_str()).unwrap_or("");
    let url_ok = sane(raw_url, 1024)
        .and_then(|value| url::Url::parse(value).ok())
        .filter(|parsed| parsed.scheme() == "wss" && parsed.has_host())
        .map(|parsed| parsed.to_string())
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
        /// True for a screen share, which LiveKit sources separately.
        screen_share: bool,
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
                "source": match track.source {
                    2 => "camera",
                    3 => "microphone",
                    4 => "screen_share",
                    5 => "screen_share_audio",
                    _ => "unknown",
                },
                "muted": track.muted,
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
    url.set_path("/rtc");
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

    loop {
        tokio::select! {
            command = commands.recv() => {
                let Some(command) = command else { break };
                let message = match command {
                    SfuCommand::Offer { sdp, target } => {
                        lkp::signal_request::Message::Offer(
                            lkp::SessionDescription {
                                r#type: "offer".to_owned(), sdp,
                                id: target as u32, ..Default::default()
                            })
                    }
                    SfuCommand::Answer { sdp, target } => {
                        lkp::signal_request::Message::Answer(
                            lkp::SessionDescription {
                                r#type: "answer".to_owned(), sdp,
                                id: target as u32, ..Default::default()
                            })
                    }
                    SfuCommand::Candidate { candidate_init, target } => {
                        lkp::signal_request::Message::Trickle(
                            lkp::TrickleRequest {
                                candidate_init, target, r#final: false,
                            })
                    }
                    SfuCommand::AddTrack { cid, name, kind, screen_share } => {
                        lkp::signal_request::Message::AddTrack(
                            lkp::AddTrackRequest {
                                cid, name, r#type: kind,
                                source: if screen_share { 4 }
                                        else if kind == 1 { 2 } else { 3 },
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
                        let others: Vec<serde_json::Value> = join
                            .other_participants.iter()
                            .take(MAX_PARTICIPANTS)
                            .filter_map(participant_json).collect();
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
                            "participants": others,
                            "ice_servers": ice,
                        }));
                    }
                    lkp::signal_response::Message::Offer(sdp) => {
                        // The server offers on SUBSCRIBER: this is everyone
                        // else's media arriving.
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
                    lkp::signal_response::Message::Leave(_) => {
                        emit(json!({
                            "type": "sfu_state", "generation": generation,
                            "state": "ended", "category": "server_leave",
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
        let _ = session.commands.send(SfuCommand::Leave);
        // The task closes its socket on the Leave and exits; abort is the
        // backstop for a task already blocked on a dead connection.
        session.task.abort();
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

    #[test]
    fn track_kinds_and_sources_are_closed_sets() {
        let info = lkp::ParticipantInfo {
            identity: "@a:x:DEVICE".to_owned(),
            tracks: vec![
                lkp::TrackInfo {
                    sid: "TR_a".to_owned(),
                    r#type: 0,
                    source: 3,
                    ..Default::default()
                },
                lkp::TrackInfo {
                    sid: "TR_b".to_owned(),
                    r#type: 1,
                    source: 4,
                    ..Default::default()
                },
                lkp::TrackInfo {
                    sid: "TR_c".to_owned(),
                    r#type: 1,
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
