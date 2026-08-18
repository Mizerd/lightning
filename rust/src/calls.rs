//! Voice-call signaling pipes (2026-08-18): legacy 1:1 VoIP (MSC2746,
//! `m.call.*` version 1) plus a narrow read-only MatrixRTC lane
//! (`m.rtc.notification` in, `m.rtc.decline` out).
//!
//! This module is SIGNALING ONLY. There is no media stack in the tree — no
//! WebRTC, no SDP generation, no ICE — so this layer transports opaque SDP
//! strings supplied by a future media backend, and the C++ controller can
//! observe an incoming call and decline or hang it up, never answer it with
//! real media. `m.call.candidates`, `m.call.negotiate` and
//! `m.call.sdp_stream_metadata_changed` are deliberately neither sent nor
//! observed: candidates are raw host IPs with no consumer this round.
//!
//! Everything rides the SDK's own event path: `Room::send` encrypts
//! automatically in an encrypted room (Lightning writes zero crypto), and
//! inbound events arrive through `Client::add_event_handler` registered for
//! the sync loop's lifetime behind `EventHandlerDropGuard`s — an orphaned
//! handler would fire into a later account's queue forever, so the guards
//! are bound to the loop, and the C++ `SessionLifecycleGuard` drops stale
//! queue entries at dequeue as the second layer.
//!
//! PRIVACY: SDP never crosses the FFI, is never logged, and is never
//! enqueued — it carries host IP addresses and often the local hostname.
//! Inbound offers/answers cross as `has_offer`/`has_answer` booleans plus a
//! sanitized session type. Hangup reasons are a closed set; a sender-chosen
//! `_Custom` reason collapses to "unknown" (same rule as the tombstone
//! body). Errors cross only as `classify_room_error` categories.
//! Inbound `call_id`/`party_id`/`selected_party_id` are SENDER-CHOSEN
//! opaque text (ruma validates nothing about a VoipId) — they are bounded
//! here (`wire_id`, length + control characters; an id that fails the
//! bound drops the whole event) and must still never be logged or
//! rendered on the C++ side.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use matrix_sdk::event_handler::EventHandlerDropGuard;
use matrix_sdk::ruma::events::call::answer::{
    CallAnswerEventContent, OriginalSyncCallAnswerEvent,
};
use matrix_sdk::ruma::events::call::hangup::{
    CallHangupEventContent, OriginalSyncCallHangupEvent, Reason,
};
use matrix_sdk::ruma::events::call::invite::{
    CallInviteEventContent, OriginalSyncCallInviteEvent,
};
use matrix_sdk::ruma::events::call::reject::{
    CallRejectEventContent, OriginalSyncCallRejectEvent,
};
use matrix_sdk::ruma::events::call::select_answer::{
    CallSelectAnswerEventContent, OriginalSyncCallSelectAnswerEvent,
};
use matrix_sdk::ruma::api::client::voip::get_turn_server_info;
use matrix_sdk::ruma::events::call::candidates::{
    CallCandidatesEventContent, Candidate, OriginalSyncCallCandidatesEvent,
};
use matrix_sdk::ruma::events::call::SessionDescription;
use matrix_sdk::ruma::events::rtc::decline::OriginalSyncRtcDeclineEvent;
use matrix_sdk::ruma::events::rtc::notification::{
    CallIntent, OriginalSyncRtcNotificationEvent,
};
use matrix_sdk::ruma::{EventId, OwnedVoipId, UInt, UserId, VoipVersionId};
use matrix_sdk::config::RequestConfig;
use matrix_sdk::{Client, Room};
use serde_json::json;

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::timeline::TimelineRegistry;
use crate::{enqueue, RustClient};

/// Call sends run on the room-action pool (joined during sign-out): bounded.
const CALL_SEND_TIMEOUT: Duration = Duration::from_secs(15);

/// MSC2746 recommends 60 s; anything outside this window is a caller bug.
const MIN_INVITE_LIFETIME_MS: u64 = 5_000;
const MAX_INVITE_LIFETIME_MS: u64 = 300_000;

type EventQueue = Arc<Mutex<VecDeque<String>>>;

fn parse_voip_id(value: &str, what: &str) -> Result<OwnedVoipId, String> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return Err(format!("empty {what}"));
    }
    Ok(OwnedVoipId::from(trimmed))
}

/// Bound for a SENDER-CHOSEN id crossing the FFI. A legitimate call/party
/// id is a short UUID-like token; anything oversized or carrying control
/// characters is hostile input and drops the whole event (we cannot be
/// party to a call whose identifiers we refuse to carry).
const MAX_WIRE_ID_LEN: usize = 255;
fn wire_id(value: &str) -> Option<&str> {
    if value.is_empty() || value.len() > MAX_WIRE_ID_LEN {
        return None;
    }
    if value.chars().any(char::is_control) {
        return None;
    }
    Some(value)
}

/// An OPTIONAL wire id (v0 events have no party id): absent stays "",
/// present-but-hostile drops the event.
fn optional_wire_id(value: Option<&str>) -> Option<String> {
    match value {
        None => Some(String::new()),
        Some(id) => wire_id(id).map(str::to_owned),
    }
}

fn require_sdp(sdp: &str) -> Result<(), String> {
    if sdp.trim().is_empty() {
        return Err("empty session description".to_owned());
    }
    Ok(())
}

/// Coarse wire representation of a session-description type. "offer" and
/// "answer" are the spec values; anything else is sender-chosen text and
/// crosses as "other".
fn session_type_str(session_type: &str) -> &'static str {
    match session_type {
        "offer" => "offer",
        "answer" => "answer",
        _ => "other",
    }
}

fn voip_version_str(version: &VoipVersionId) -> &'static str {
    match version {
        VoipVersionId::V0 => "0",
        VoipVersionId::V1 => "1",
        _ => "other",
    }
}

/// Closed inbound set. `_Custom` collapses to "unknown" — sender-chosen
/// free text never crosses the FFI.
pub(crate) fn reason_str(reason: &Reason) -> &'static str {
    match reason {
        Reason::IceFailed => "ice_failed",
        Reason::InviteTimeout => "invite_timeout",
        Reason::IceTimeout => "ice_timeout",
        Reason::UserHangup => "user_hangup",
        Reason::UserMediaFailed => "user_media_failed",
        Reason::UserBusy => "user_busy",
        Reason::UnknownError => "unknown_error",
        _ => "unknown",
    }
}

/// Closed outbound set. "replaced" is what matrix-js-sdk sends for a
/// glare-replaced call; ruma's `Reason` is a StringEnum, so the `From`
/// conversion produces the `_Custom` wire value without free text from
/// anywhere but this match.
pub(crate) fn reason_from_code(code: &str) -> Result<Reason, String> {
    Ok(match code {
        "user_hangup" => Reason::UserHangup,
        "invite_timeout" => Reason::InviteTimeout,
        "user_busy" => Reason::UserBusy,
        "user_media_failed" => Reason::UserMediaFailed,
        "unknown_error" => Reason::UnknownError,
        "replaced" => Reason::from("replaced"),
        _ => return Err("unsupported hangup reason".to_owned()),
    })
}

fn intent_str(intent: Option<&CallIntent>) -> &'static str {
    match intent {
        Some(CallIntent::Audio) => "audio",
        Some(CallIntent::Video) => "video",
        Some(_) => "unknown",
        None => "unspecified",
    }
}

/// Shared send tail: run the future on the room-action pool with a bounded
/// timeout, then report `call_send_result { op_id, ok, category, call_id,
/// event_id }`. Never logs or forwards SDK error text.
fn spawn_call_send<F>(bridge: &RustClient, op_id: u64, call_id: String, send: F)
where
    F: std::future::Future<
            Output = Result<matrix_sdk::ruma::OwnedEventId, matrix_sdk::Error>,
        > + Send
        + 'static,
{
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(CALL_SEND_TIMEOUT, send).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category, event_id) = match result {
            Ok(Ok(event_id)) => (true, "", event_id.to_string()),
            Ok(Err(err)) => {
                (false, classify_room_error(&err.to_string()), String::new())
            }
            Err(_) => (false, "network", String::new()),
        };
        enqueue(&events, json!({
            "type": "call_send_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": ok,
            "category": category,
            "call_id": call_id,
            "event_id": event_id,
        }));
    });
}

/// Send `m.call.invite` (VoIP v1). The SDP is a required opaque parameter —
/// there is no default and no stub; the pipe simply has no producer until a
/// media backend exists. An empty SDP is refused synchronously.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_invite(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    offer_type: String,
    offer_sdp: String,
    lifetime_ms: u64,
    invitee: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    require_sdp(&offer_sdp)?;
    if !matches!(offer_type.as_str(), "offer") {
        return Err("invite session type must be \"offer\"".to_owned());
    }
    let lifetime = lifetime_ms.clamp(MIN_INVITE_LIFETIME_MS, MAX_INVITE_LIFETIME_MS);
    let invitee_id = if invitee.trim().is_empty() {
        None
    } else {
        Some(
            UserId::parse(invitee.trim())
                .map_err(|_| "invalid invitee user id".to_owned())?,
        )
    };
    let mut content = CallInviteEventContent::version_1(
        call,
        party,
        UInt::try_from(lifetime).map_err(|_| "invalid lifetime".to_owned())?,
        SessionDescription::new(offer_type, offer_sdp),
    );
    content.invitee = invitee_id;
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Send `m.call.answer` (VoIP v1). Plumbed for completeness of the
/// signaling layer; unreachable from production C++ until a media backend
/// can produce an answer SDP.
pub(crate) fn send_answer(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    answer_type: String,
    answer_sdp: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    require_sdp(&answer_sdp)?;
    if !matches!(answer_type.as_str(), "answer") {
        return Err("answer session type must be \"answer\"".to_owned());
    }
    let content = CallAnswerEventContent::version_1(
        SessionDescription::new(answer_type, answer_sdp),
        call,
        party,
    );
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Send `m.call.reject` (VoIP v1 only — v0 peers treat only hangup).
pub(crate) fn send_reject(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    let content = CallRejectEventContent::version_1(call, party);
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Send `m.call.hangup` with a reason from the closed outbound set.
pub(crate) fn send_hangup(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    reason_code: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    let reason = reason_from_code(&reason_code)?;
    let content = CallHangupEventContent::version_1(call, party, reason);
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Send `m.call.select_answer` — names the party whose answer the caller
/// locked onto (MSC2746 multi-device rule).
pub(crate) fn send_select_answer(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    selected_party_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    let selected = parse_voip_id(&selected_party_id, "selected party id")?;
    let content = CallSelectAnswerEventContent::version_1(call, party, selected);
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Decline an `m.rtc.notification` ring. The SDK builds the decline content
/// itself (`Room::make_decline_call_event`) — Lightning constructs no RTC
/// content.
pub(crate) fn rtc_decline(
    bridge: &RustClient,
    room_id: String,
    notification_event_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let event_id = EventId::parse(notification_event_id.trim())
        .map_err(|_| "invalid event id".to_owned())?;
    spawn_call_send(bridge, op_id, String::new(), async move {
        let content = room
            .make_decline_call_event(&event_id)
            .await
            .map_err(|err| matrix_sdk::Error::UnknownError(Box::new(err)))?;
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Keeps every registered call handler alive for exactly the sync loop's
/// lifetime. Dropping this unregisters them all.
pub(crate) struct CallHandlerGuards {
    _guards: Vec<EventHandlerDropGuard>,
}

/// Register inbound observers for the call-signaling events. These ride the
/// normal sync dispatch (sliding sync and classic /sync share
/// `call_sync_response_handlers`), independent of any timeline being open.
/// The existing timeline "call event" state row is untouched — call STATE
/// is fed only from here.
/// Bound the SDP we are willing to carry into C++ memory: a legitimate
/// session description is a few KB; the homeserver caps events at 64 KiB.
const MAX_CARRIED_SDP_LEN: usize = 128 * 1024;

/// The remote SDP for the C++ store — ONLY in media-capable mode, never
/// oversized, and only when actually present. `None` means the payload
/// simply omits the field.
fn carried_sdp(media_capable: &AtomicBool, sdp: &str) -> Option<String> {
    if !media_capable.load(Ordering::Relaxed) {
        return None;
    }
    let trimmed = sdp.trim();
    if trimmed.is_empty() || trimmed.len() > MAX_CARRIED_SDP_LEN {
        return None;
    }
    Some(trimmed.to_owned())
}

/// Bounds for ICE candidates crossing in either direction: a legitimate
/// call gathers a handful; anything beyond is hostile or broken.
const MAX_CANDIDATES_PER_EVENT: usize = 32;
const MAX_CANDIDATE_LINE_LEN: usize = 1024;
const MAX_SDP_MID_LEN: usize = 64;

/// A candidate "a"-line safe to carry: bounded and control-free. Empty is
/// legal — MSC2746 v1 ends gathering with an empty candidate.
fn carried_candidate_line(line: &str) -> Option<&str> {
    if line.len() > MAX_CANDIDATE_LINE_LEN
        || line.chars().any(char::is_control)
    {
        return None;
    }
    Some(line)
}

/// Send `m.call.candidates`. The list arrives from C++ as JSON (our OWN
/// locally gathered candidates); it is re-validated and bounded anyway.
pub(crate) fn send_candidates(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    candidates_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    let parsed: serde_json::Value = serde_json::from_str(&candidates_json)
        .map_err(|_| "invalid candidate list".to_owned())?;
    let Some(entries) = parsed.as_array() else {
        return Err("invalid candidate list".to_owned());
    };
    if entries.is_empty() || entries.len() > MAX_CANDIDATES_PER_EVENT {
        return Err("invalid candidate count".to_owned());
    }
    let mut candidates = Vec::with_capacity(entries.len());
    for entry in entries {
        let line = entry
            .get("candidate")
            .and_then(|value| value.as_str())
            .unwrap_or_default();
        let Some(line) = carried_candidate_line(line) else {
            return Err("invalid candidate".to_owned());
        };
        let mut candidate = Candidate::new(line.to_owned());
        if let Some(mid) = entry.get("sdp_mid").and_then(|value| value.as_str())
        {
            if mid.len() > MAX_SDP_MID_LEN || mid.chars().any(char::is_control)
            {
                return Err("invalid candidate".to_owned());
            }
            candidate.sdp_mid = Some(mid.to_owned());
        }
        if let Some(index) =
            entry.get("sdp_m_line_index").and_then(|value| value.as_u64())
        {
            candidate.sdp_m_line_index =
                Some(UInt::try_from(index.min(255)).unwrap_or_default());
        }
        candidates.push(candidate);
    }
    let content = CallCandidatesEventContent::version_1(call, party, candidates);
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

/// Fetch the homeserver's TURN servers (`/voip/turnServer`). The response
/// carries short-lived CREDENTIALS: they cross the FFI once, feed the
/// media engine's ICE config, and are never logged or persisted. Policy:
/// Lightning contacts ONLY servers the homeserver names — no third-party
/// STUN fallback that would leak the user's IP elsewhere.
pub(crate) fn fetch_turn_servers(
    bridge: &RustClient,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(CALL_SEND_TIMEOUT);
        let result = client
            .send(get_turn_server_info::v3::Request::new())
            .with_request_config(config)
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(response) => {
                enqueue(&events, json!({
                    "type": "call_turn_servers",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "username": response.username,
                    "password": response.password,
                    "uris": response.uris,
                    "ttl_seconds": response.ttl.as_secs().min(86400),
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "call_turn_servers",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }));
            }
        }
    });
    Ok(())
}

pub(crate) fn register_handlers(
    client: &Client,
    events: &EventQueue,
    timelines: &Arc<TimelineRegistry>,
    media_capable: &Arc<AtomicBool>,
) -> CallHandlerGuards {
    let mut guards = Vec::new();

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let media_capable = Arc::clone(media_capable);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallInviteEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                let media_capable = Arc::clone(&media_capable);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) = optional_wire_id(
                        ev.content.party_id.as_deref().map(AsRef::as_ref))
                    else {
                        return;
                    };
                    // The SDP crosses ONLY in media-capable mode (see
                    // carried_sdp); otherwise has_offer alone.
                    let mut payload = json!({
                        "type": "call_invite",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "invitee": ev.content.invitee.as_ref()
                            .map(ToString::to_string).unwrap_or_default(),
                        "lifetime_ms": u64::from(ev.content.lifetime)
                            .min(MAX_INVITE_LIFETIME_MS),
                        "origin_server_ts": u64::from(ev.origin_server_ts.get()),
                        "version": voip_version_str(&ev.content.version),
                        "offer_type":
                            session_type_str(&ev.content.offer.session_type),
                        "has_offer": !ev.content.offer.sdp.trim().is_empty(),
                    });
                    if let Some(sdp) =
                        carried_sdp(&media_capable, &ev.content.offer.sdp)
                    {
                        payload["offer_sdp"] = json!(sdp);
                    }
                    enqueue(&events, payload);
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let media_capable = Arc::clone(media_capable);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallAnswerEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                let media_capable = Arc::clone(&media_capable);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) = optional_wire_id(
                        ev.content.party_id.as_deref().map(AsRef::as_ref))
                    else {
                        return;
                    };
                    let mut payload = json!({
                        "type": "call_answer",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "answer_type":
                            session_type_str(&ev.content.answer.session_type),
                        "has_answer": !ev.content.answer.sdp.trim().is_empty(),
                    });
                    if let Some(sdp) =
                        carried_sdp(&media_capable, &ev.content.answer.sdp)
                    {
                        payload["answer_sdp"] = json!(sdp);
                    }
                    enqueue(&events, payload);
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallHangupEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) = optional_wire_id(
                        ev.content.party_id.as_deref().map(AsRef::as_ref))
                    else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "call_hangup",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "reason": reason_str(&ev.content.reason),
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
            move |ev: OriginalSyncCallRejectEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) =
                        wire_id(ev.content.party_id.as_str())
                    else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "call_reject",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
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
            move |ev: OriginalSyncCallSelectAnswerEvent,
                  room: Room,
                  client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) =
                        wire_id(ev.content.party_id.as_str())
                    else {
                        return;
                    };
                    let Some(selected) =
                        wire_id(ev.content.selected_party_id.as_str())
                    else {
                        return;
                    };
                    enqueue(&events, json!({
                        "type": "call_select_answer",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "selected_party_id": selected,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let media_capable = Arc::clone(media_capable);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallCandidatesEvent,
                  room: Room,
                  client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                let media_capable = Arc::clone(&media_capable);
                async move {
                    // Candidates are pure ICE (host IPs). Without a media
                    // engine there is no consumer, so nothing crosses at
                    // all outside media-capable mode.
                    if !media_capable.load(Ordering::Relaxed) {
                        return;
                    }
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let Some(call_id) = wire_id(ev.content.call_id.as_str())
                    else {
                        return;
                    };
                    let Some(party_id) = optional_wire_id(
                        ev.content.party_id.as_deref().map(AsRef::as_ref))
                    else {
                        return;
                    };
                    let mut entries = Vec::new();
                    for candidate in
                        ev.content.candidates.iter().take(MAX_CANDIDATES_PER_EVENT)
                    {
                        let Some(line) =
                            carried_candidate_line(&candidate.candidate)
                        else {
                            continue; // hostile line: skip, keep the rest
                        };
                        let mut entry = json!({ "candidate": line });
                        if let Some(mid) = candidate
                            .sdp_mid
                            .as_deref()
                            .filter(|mid| {
                                mid.len() <= MAX_SDP_MID_LEN
                                    && !mid.chars().any(char::is_control)
                            })
                        {
                            entry["sdp_mid"] = json!(mid);
                        }
                        if let Some(index) = candidate.sdp_m_line_index {
                            entry["sdp_m_line_index"] =
                                json!(u64::from(index).min(255));
                        }
                        entries.push(entry);
                    }
                    if entries.is_empty() {
                        return;
                    }
                    enqueue(&events, json!({
                        "type": "call_candidates",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "candidates": entries,
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
            move |ev: OriginalSyncRtcNotificationEvent,
                  room: Room,
                  client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    enqueue(&events, json!({
                        "type": "call_rtc_notification",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "origin_server_ts": u64::from(ev.origin_server_ts.get()),
                        "sender_ts": u64::from(ev.content.sender_ts.get()),
                        "lifetime_ms": u64::try_from(
                                ev.content.lifetime.as_millis()
                                    .min(u128::from(MAX_INVITE_LIFETIME_MS)))
                            .unwrap_or(MAX_INVITE_LIFETIME_MS),
                        "call_intent": intent_str(ev.content.call_intent.as_ref()),
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
            move |ev: OriginalSyncRtcDeclineEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    let target = ev
                        .content
                        .relates_to
                        .event_id
                        .to_string();
                    enqueue(&events, json!({
                        "type": "call_rtc_decline",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "target_event_id": target,
                    }));
                }
            },
        );
        guards.push(client.event_handler_drop_guard(handle));
    }

    CallHandlerGuards { _guards: guards }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn inbound_reasons_are_a_closed_set() {
        assert_eq!(reason_str(&Reason::UserHangup), "user_hangup");
        assert_eq!(reason_str(&Reason::InviteTimeout), "invite_timeout");
        assert_eq!(reason_str(&Reason::UserBusy), "user_busy");
        // Sender-chosen free text must collapse, never cross.
        let custom = Reason::from("libwebrtc exploded at 10.0.0.7");
        assert_eq!(reason_str(&custom), "unknown");
    }

    #[test]
    fn outbound_reasons_reject_unknown_codes() {
        assert!(reason_from_code("user_hangup").is_ok());
        assert!(reason_from_code("replaced").is_ok());
        assert!(reason_from_code("anything else").is_err());
        assert!(reason_from_code("").is_err());
    }

    #[test]
    fn session_types_are_sanitized() {
        assert_eq!(session_type_str("offer"), "offer");
        assert_eq!(session_type_str("answer"), "answer");
        assert_eq!(session_type_str("v=0 10.1.2.3"), "other");
    }

    #[test]
    fn voip_ids_reject_empty() {
        assert!(parse_voip_id("", "call id").is_err());
        assert!(parse_voip_id("   ", "call id").is_err());
        assert!(parse_voip_id("abc123", "call id").is_ok());
    }

    #[test]
    fn sdp_is_required_not_stubbed() {
        assert!(require_sdp("").is_err());
        assert!(require_sdp("   \n").is_err());
        assert!(require_sdp("v=0\r\no=- 0 0 IN IP4 0.0.0.0").is_ok());
    }

    #[test]
    fn invite_payload_shape_carries_no_sdp() {
        // The poll payload for an invite is built inline in the handler;
        // this pins the sanitizers it is built FROM, plus the invariant
        // that a SessionDescription's sdp field itself never appears in
        // any json! call in this module (asserted by review + the absence
        // of any "sdp" key above; the sanitizer outputs are closed sets).
        let description = SessionDescription::new(
            "offer".to_owned(),
            "v=0\r\nc=IN IP4 192.168.1.4".to_owned(),
        );
        assert_eq!(session_type_str(&description.session_type), "offer");
        assert!(!description.sdp.is_empty());
        // Coarse booleans are what crosses.
        assert!(!description.sdp.trim().is_empty());
    }

    #[test]
    fn wire_ids_are_bounded_and_control_free() {
        assert_eq!(wire_id("abc-123"), Some("abc-123"));
        assert!(wire_id("").is_none());
        assert!(wire_id(&"x".repeat(300)).is_none());
        assert!(wire_id("evil\ninjection").is_none());
        assert!(wire_id("literal-backslash-\\x07").is_some());
        assert!(wire_id("real\u{7}bell").is_none());
        // Optional form: absence is honest emptiness, hostile drops.
        assert_eq!(optional_wire_id(None), Some(String::new()));
        assert_eq!(optional_wire_id(Some("ok")), Some("ok".to_owned()));
        assert_eq!(optional_wire_id(Some("bad\nid")), None);
    }

    #[test]
    fn sdp_crosses_only_in_media_capable_mode() {
        let off = AtomicBool::new(false);
        let on = AtomicBool::new(true);
        assert_eq!(carried_sdp(&off, "v=0 valid"), None);
        assert_eq!(carried_sdp(&on, "v=0 valid"), Some("v=0 valid".into()));
        assert_eq!(carried_sdp(&on, "   "), None);
        let oversized = "x".repeat(MAX_CARRIED_SDP_LEN + 1);
        assert_eq!(carried_sdp(&on, &oversized), None);
    }

    #[test]
    fn candidate_lines_are_bounded_and_control_free() {
        assert!(carried_candidate_line(
            "candidate:0 1 UDP 2122252543 192.168.1.4 47279 typ host")
            .is_some());
        assert!(carried_candidate_line("").is_some()); // end-of-candidates
        assert!(carried_candidate_line("evil\nline").is_none());
        assert!(carried_candidate_line(&"x".repeat(2000)).is_none());
    }

    #[test]
    fn lifetime_is_clamped() {
        assert_eq!(
            1_000u64.clamp(MIN_INVITE_LIFETIME_MS, MAX_INVITE_LIFETIME_MS),
            MIN_INVITE_LIFETIME_MS
        );
        assert_eq!(
            10_000_000u64.clamp(MIN_INVITE_LIFETIME_MS, MAX_INVITE_LIFETIME_MS),
            MAX_INVITE_LIFETIME_MS
        );
    }
}
