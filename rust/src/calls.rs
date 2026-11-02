
use std::collections::{BTreeMap, VecDeque};
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
use matrix_sdk::ruma::events::call::negotiate::{
    CallNegotiateEventContent, OriginalSyncCallNegotiateEvent,
};
use matrix_sdk::ruma::events::call::sdp_stream_metadata_changed::{
    CallSdpStreamMetadataChangedEventContent, OriginalSyncCallSdpStreamMetadataChangedEvent,
};
use matrix_sdk::ruma::events::call::{SessionDescription, StreamMetadata, StreamPurpose};
use matrix_sdk::ruma::events::rtc::decline::OriginalSyncRtcDeclineEvent;
use matrix_sdk::ruma::events::rtc::notification::{
    CallIntent, OriginalSyncRtcNotificationEvent,
};
use matrix_sdk::ruma::events::Mentions;
use matrix_sdk::ruma::{EventId, OwnedVoipId, UInt, UserId, VoipVersionId};
use matrix_sdk::config::RequestConfig;
use matrix_sdk::{Client, Room};
use serde_json::json;

use matrix_sdk_ui::room_list_service::RoomListService;

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::timeline::TimelineRegistry;
use crate::warm_rooms::SubscriptionSet;
use crate::{enqueue, RustClient};

const CALL_SEND_TIMEOUT: Duration = Duration::from_secs(15);

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

fn session_type_str(session_type: &str) -> &'static str {
    match session_type {
        "offer" => "offer",
        "answer" => "answer",
        _ => "other",
    }
}

/// Whether a description carries video: a live `m=video` section. Port 0
/// rejects a section, except under `a=bundle-only`, which is how a max-bundle
/// offerer (webrtcbin) offers one; `a=inactive` declines it. Only this boolean
/// crosses, never the SDP.
fn sdp_offers_video(sdp: &str) -> bool {
    let mut lines = sdp.lines();
    while let Some(line) = lines.next() {
        let Some(rest) = line.strip_prefix("m=video ") else {
            continue;
        };
        let port_open = rest.split_whitespace().next().is_some_and(|port| port != "0");
        let attributes: Vec<&str> = lines
            .take_while(|attribute| !attribute.starts_with("m="))
            .map(str::trim)
            .collect();
        return !attributes.contains(&"a=inactive")
            && (port_open || attributes.contains(&"a=bundle-only"));
    }
    false
}

const MAX_STREAM_METADATA_ENTRIES: usize = 8;

fn parse_stream_metadata(json: &str) -> Result<BTreeMap<String, StreamMetadata>, String> {
    if json.trim().is_empty() {
        return Ok(BTreeMap::new());
    }
    let map: BTreeMap<String, StreamMetadata> =
        serde_json::from_str(json).map_err(|_| "invalid stream metadata".to_owned())?;
    if map.len() > MAX_STREAM_METADATA_ENTRIES
        || map.keys().any(|key| wire_id(key).is_none())
    {
        return Err("invalid stream metadata".to_owned());
    }
    Ok(map)
}

fn usermedia_muted(metadata: &BTreeMap<String, StreamMetadata>) -> Option<(bool, bool)> {
    metadata
        .values()
        .take(MAX_STREAM_METADATA_ENTRIES)
        .find(|stream| stream.purpose == StreamPurpose::UserMedia)
        .map(|stream| (stream.audio_muted, stream.video_muted))
}

fn add_stream_metadata(
    payload: &mut serde_json::Value,
    metadata: &BTreeMap<String, StreamMetadata>,
) {
    if let Some((audio_muted, video_muted)) = usermedia_muted(metadata) {
        payload["has_stream_metadata"] = json!(true);
        payload["remote_audio_muted"] = json!(audio_muted);
        payload["remote_video_muted"] = json!(video_muted);
    }
}

fn voip_version_str(version: &VoipVersionId) -> &'static str {
    match version {
        VoipVersionId::V0 => "0",
        VoipVersionId::V1 => "1",
        _ => "other",
    }
}

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

const MAX_RTC_MENTION_IDS: usize = 256;

pub(crate) fn rtc_notification_addressing(
    notification_type: &str,
    mentions: Option<&Mentions>,
    own_user: Option<&UserId>,
) -> serde_json::Map<String, serde_json::Value> {
    let notification_type = match notification_type {
        "ring" => "ring",
        _ => "notification",
    };
    let (user_ids, room) = match mentions {
        Some(mentions) => (
            mentions
                .user_ids
                .iter()
                .take(MAX_RTC_MENTION_IDS)
                .map(|user| user.to_string())
                .collect::<Vec<_>>(),
            mentions.room,
        ),
        None => (Vec::new(), false),
    };
    let mentions_me = match (mentions, own_user) {
        (Some(mentions), Some(own)) => mentions.user_ids.iter().any(|user| user == own),
        _ => false,
    };
    let mut fields = serde_json::Map::new();
    fields.insert("notification_type".to_owned(), json!(notification_type));
    fields.insert("mentions_user_ids".to_owned(), json!(user_ids));
    fields.insert("mentions_room".to_owned(), json!(room));
    fields.insert("mentions_me".to_owned(), json!(mentions_me));
    fields
}

fn rtc_notification_payload(
    ev: &OriginalSyncRtcNotificationEvent,
    room_id: &str,
    own_user: Option<&UserId>,
    lifecycle: u64,
) -> serde_json::Value {
    let own = own_user.is_some_and(|user| user == ev.sender);
    let mut payload = json!({
        "type": "call_rtc_notification",
        "lifecycle": lifecycle,
        "room_id": room_id,
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
    });
    if let Some(object) = payload.as_object_mut() {
        object.extend(rtc_notification_addressing(
            ev.content.notification_type.as_str(),
            ev.content.mentions.as_ref(),
            own_user,
        ));
    }
    payload
}

fn intent_str(intent: Option<&CallIntent>) -> &'static str {
    match intent {
        Some(CallIntent::Audio) => "audio",
        Some(CallIntent::Video) => "video",
        Some(_) => "unknown",
        None => "unspecified",
    }
}

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

fn is_fresh(origin_server_ts: matrix_sdk::ruma::MilliSecondsSinceUnixEpoch) -> bool {
    let now = u64::from(matrix_sdk::ruma::MilliSecondsSinceUnixEpoch::now().get());
    now.saturating_sub(u64::from(origin_server_ts.get())) <= MAX_INVITE_LIFETIME_MS
}

/// Subscribes the room a call is happening in. The room list's timeline limit
/// is 1: an offer's candidates, or an answer's, sent within one sync response
/// reach us as the last event only, and an encrypted room's first send, which
/// shares a key first, bunches them exactly so. Measured: the batch holding the
/// real candidates vanished on the first call in every encrypted DM, on both
/// ends, and ICE never started. A new subscription's first response replays the
/// recent events, so what was already lost comes back.
async fn subscribe_call_room(
    subscriptions: &Mutex<SubscriptionSet>,
    room_list: &Mutex<Option<Arc<RoomListService>>>,
    room_id: &matrix_sdk::ruma::RoomId,
) {
    let desired = subscriptions.lock().ok().and_then(|mut guard| {
        guard.warm(room_id.to_owned());
        guard.pending(false)
    });
    let Some(desired) = desired else { return };
    let service = room_list.lock().ok().and_then(|guard| guard.clone());
    if let Some(service) = service {
        let refs: Vec<&matrix_sdk::ruma::RoomId> =
            desired.iter().map(|r| r.as_ref()).collect();
        service.subscribe_to_rooms(&refs).await;
    }
}

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
    stream_metadata: String,
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
    content.sdp_stream_metadata = parse_stream_metadata(&stream_metadata)?;
    let subscriptions = Arc::clone(&bridge.active_room_subscription);
    let room_list = Arc::clone(&bridge.room_list_service);
    spawn_call_send(bridge, op_id, call_id, async move {
        subscribe_call_room(&subscriptions, &room_list, room.room_id()).await;
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

pub(crate) fn send_answer(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    answer_type: String,
    answer_sdp: String,
    stream_metadata: String,
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
    let mut content = CallAnswerEventContent::version_1(
        SessionDescription::new(answer_type, answer_sdp),
        call,
        party,
    );
    content.sdp_stream_metadata = parse_stream_metadata(&stream_metadata)?;
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send(content).await.map(|result| result.response.event_id)
    });
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_negotiate(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    description_type: String,
    sdp: String,
    lifetime_ms: u64,
    stream_metadata: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    require_sdp(&sdp)?;
    if !matches!(description_type.as_str(), "offer" | "answer") {
        return Err("negotiate session type must be offer or answer".to_owned());
    }
    let lifetime = lifetime_ms.clamp(MIN_INVITE_LIFETIME_MS, MAX_INVITE_LIFETIME_MS);
    let mut content = CallNegotiateEventContent::version_1(
        call,
        party,
        UInt::try_from(lifetime).map_err(|_| "invalid lifetime".to_owned())?,
        SessionDescription::new(description_type, sdp),
    );
    content.sdp_stream_metadata = parse_stream_metadata(&stream_metadata)?;
    let raw = with_explicit_metadata(&content, &stream_metadata)?;
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send_raw("m.call.negotiate", raw)
            .await
            .map(|result| result.response.event_id)
    });
    Ok(())
}

fn with_explicit_metadata<T: serde::Serialize>(
    content: &T,
    stream_metadata: &str,
) -> Result<serde_json::Value, String> {
    let mut raw =
        serde_json::to_value(content).map_err(|_| "invalid call event".to_owned())?;
    if !stream_metadata.trim().is_empty() {
        raw["sdp_stream_metadata"] = serde_json::from_str(stream_metadata)
            .map_err(|_| "invalid stream metadata".to_owned())?;
    }
    Ok(raw)
}

pub(crate) fn send_stream_metadata(
    bridge: &RustClient,
    room_id: String,
    call_id: String,
    party_id: String,
    stream_metadata: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let call = parse_voip_id(&call_id, "call id")?;
    let party = parse_voip_id(&party_id, "party id")?;
    let metadata = parse_stream_metadata(&stream_metadata)?;
    if metadata.is_empty() {
        return Err("stream metadata is required".to_owned());
    }
    let content = CallSdpStreamMetadataChangedEventContent::new(
        call, party, VoipVersionId::V1, metadata,
    );
    let raw = with_explicit_metadata(&content, &stream_metadata)?;
    spawn_call_send(bridge, op_id, call_id, async move {
        room.send_raw("m.call.sdp_stream_metadata_changed", raw)
            .await
            .map(|result| result.response.event_id)
    });
    Ok(())
}

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

pub(crate) struct CallHandlerGuards {
    _guards: Vec<EventHandlerDropGuard>,
}

const MAX_CARRIED_SDP_LEN: usize = 128 * 1024;

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

const MAX_CANDIDATES_PER_EVENT: usize = 32;
const MAX_CANDIDATE_LINE_LEN: usize = 1024;
const MAX_SDP_MID_LEN: usize = 64;

/// A candidate line safe to carry: bounded and control-free. Empty is legal
/// (MSC2746 v1 ends gathering with an empty candidate).
fn carried_candidate_line(line: &str) -> Option<&str> {
    if line.len() > MAX_CANDIDATE_LINE_LEN
        || line.chars().any(char::is_control)
    {
        return None;
    }
    Some(line)
}

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
                    "category": classify_turn_failure(&err),
                }));
            }
        }
    });
    Ok(())
}

/// Why `/voip/turnServer` gave no servers. Synapse without TURN answers 200
/// with `{}`, which ruma reports as a deserialization failure (every field is
/// required); that is "none configured", not a network fault.
fn classify_turn_failure(err: &matrix_sdk::HttpError) -> &'static str {
    use matrix_sdk::ruma::api::error::FromHttpResponseError;
    use matrix_sdk::HttpError;

    use matrix_sdk::ruma::api::error::DeserializationError;

    let empty_answer = match err {
        HttpError::Api(api) => match &**api {
            FromHttpResponseError::Deserialization(DeserializationError::Json(json)) => {
                json_error_is_empty_answer(json)
            }
            _ => false,
        },
        _ => false,
    };
    turn_failure_category(
        err.as_client_api_error().is_some(),
        empty_answer,
        &err.to_string(),
    )
}

fn turn_failure_category(
    server_error: bool,
    empty_answer: bool,
    message: &str,
) -> &'static str {
    if server_error {
        return classify_room_error(message);
    }
    if empty_answer {
        return "none_configured";
    }
    "network"
}

fn json_error_is_empty_answer(err: &serde_json::Error) -> bool {
    err.classify() == serde_json::error::Category::Data
}

fn invite_payload(
    ev: &OriginalSyncCallInviteEvent,
    room: &Room,
    own: bool,
    timelines: &TimelineRegistry,
    media_capable: &AtomicBool,
) -> Option<serde_json::Value> {
    let Some(call_id) = wire_id(ev.content.call_id.as_str())
    else {
        return None;
    };
    let Some(party_id) = optional_wire_id(
        ev.content.party_id.as_deref().map(AsRef::as_ref))
    else {
        return None;
    };
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
        "has_video": sdp_offers_video(&ev.content.offer.sdp),
    });
    add_stream_metadata(&mut payload, &ev.content.sdp_stream_metadata);
    if let Some(sdp) =
        carried_sdp(&media_capable, &ev.content.offer.sdp)
    {
        payload["offer_sdp"] = json!(sdp);
    }
    Some(payload)
}

fn answer_payload(
    ev: &OriginalSyncCallAnswerEvent,
    room: &Room,
    own: bool,
    timelines: &TimelineRegistry,
    media_capable: &AtomicBool,
) -> Option<serde_json::Value> {
    let Some(call_id) = wire_id(ev.content.call_id.as_str())
    else {
        return None;
    };
    let Some(party_id) = optional_wire_id(
        ev.content.party_id.as_deref().map(AsRef::as_ref))
    else {
        return None;
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
        "has_video": sdp_offers_video(&ev.content.answer.sdp),
    });
    add_stream_metadata(&mut payload, &ev.content.sdp_stream_metadata);
    if let Some(sdp) =
        carried_sdp(&media_capable, &ev.content.answer.sdp)
    {
        payload["answer_sdp"] = json!(sdp);
    }
    Some(payload)
}

fn candidates_payload(
    ev: &OriginalSyncCallCandidatesEvent,
    room: &Room,
    own: bool,
    timelines: &TimelineRegistry,
) -> Option<serde_json::Value> {
    let Some(call_id) = wire_id(ev.content.call_id.as_str())
    else {
        return None;
    };
    let Some(party_id) = optional_wire_id(
        ev.content.party_id.as_deref().map(AsRef::as_ref))
    else {
        return None;
    };
    let mut entries = Vec::new();
    for candidate in
        ev.content.candidates.iter().take(MAX_CANDIDATES_PER_EVENT)
    {
        let Some(line) =
            carried_candidate_line(&candidate.candidate)
        else {
            continue;
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
        return None;
    }
    Some(json!({
        "type": "call_candidates",
        "lifecycle": timelines.lifecycle(),
        "room_id": room.room_id().to_string(),
        "event_id": ev.event_id.to_string(),
        "sender": ev.sender.to_string(),
        "own": own,
        "call_id": call_id,
        "party_id": party_id,
        "candidates": entries,
    }))}

pub(crate) fn register_handlers(
    client: &Client,
    events: &EventQueue,
    timelines: &Arc<TimelineRegistry>,
    media_capable: &Arc<AtomicBool>,
    subscriptions: &Arc<Mutex<SubscriptionSet>>,
    room_list: &Arc<Mutex<Option<Arc<RoomListService>>>>,
) -> CallHandlerGuards {
    let mut guards = Vec::new();

    {
        let events = Arc::clone(events);
        let timelines = Arc::clone(timelines);
        let media_capable = Arc::clone(media_capable);
        let subscriptions = Arc::clone(subscriptions);
        let room_list = Arc::clone(room_list);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallInviteEvent, room: Room, client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                let media_capable = Arc::clone(&media_capable);
                let subscriptions = Arc::clone(&subscriptions);
                let room_list = Arc::clone(&room_list);
                async move {
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    if let Some(payload) =
                        invite_payload(&ev, &room, own, &timelines, &media_capable)
                    {
                        if !own && is_fresh(ev.origin_server_ts) {
                            subscribe_call_room(&subscriptions, &room_list, room.room_id())
                                .await;
                        }
                        enqueue(&events, payload);
                    }
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
                    if let Some(payload) =
                        answer_payload(&ev, &room, own, &timelines, &media_capable)
                    {
                        enqueue(&events, payload);
                    }
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
            move |ev: OriginalSyncCallNegotiateEvent, room: Room, client: Client| {
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
                    let Some(party_id) = wire_id(ev.content.party_id.as_str())
                    else {
                        return;
                    };
                    let mut payload = json!({
                        "type": "call_negotiate",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                        "description_type":
                            session_type_str(&ev.content.description.session_type),
                        "has_description":
                            !ev.content.description.sdp.trim().is_empty(),
                        "has_video": sdp_offers_video(&ev.content.description.sdp),
                    });
                    add_stream_metadata(&mut payload, &ev.content.sdp_stream_metadata);
                    if let Some(sdp) =
                        carried_sdp(&media_capable, &ev.content.description.sdp)
                    {
                        payload["description_sdp"] = json!(sdp);
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
            move |ev: OriginalSyncCallSdpStreamMetadataChangedEvent,
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
                    let Some(party_id) = wire_id(ev.content.party_id.as_str())
                    else {
                        return;
                    };
                    let mut payload = json!({
                        "type": "call_stream_metadata",
                        "lifecycle": timelines.lifecycle(),
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "own": own,
                        "call_id": call_id,
                        "party_id": party_id,
                    });
                    add_stream_metadata(&mut payload, &ev.content.sdp_stream_metadata);
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
        let subscriptions = Arc::clone(subscriptions);
        let room_list = Arc::clone(room_list);
        let handle = client.add_event_handler(
            move |ev: OriginalSyncCallCandidatesEvent,
                  room: Room,
                  client: Client| {
                let events = Arc::clone(&events);
                let timelines = Arc::clone(&timelines);
                let media_capable = Arc::clone(&media_capable);
                let subscriptions = Arc::clone(&subscriptions);
                let room_list = Arc::clone(&room_list);
                async move {
                    if !media_capable.load(Ordering::Relaxed) {
                        return;
                    }
                    let own = client
                        .user_id()
                        .is_some_and(|user| user == ev.sender);
                    // Maybe all that is left of a burst, its invite lost to the
                    // timeline limit (measured, 1 first call in 6): the
                    // subscription's first response brings the invite back.
                    if let Some(payload) = candidates_payload(&ev, &room, own, &timelines) {
                        if !own && is_fresh(ev.origin_server_ts) {
                            subscribe_call_room(&subscriptions, &room_list, room.room_id())
                                .await;
                        }
                        enqueue(&events, payload);
                    }
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
                    enqueue(&events, rtc_notification_payload(
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

    fn stable_notification(content: serde_json::Value) -> OriginalSyncRtcNotificationEvent {
        serde_json::from_value(json!({
            "type": "m.rtc.notification",
            "event_id": "$announce:x",
            "sender": "@caller:x",
            "origin_server_ts": 1_000,
            "content": content,
        }))
        .expect("a valid m.rtc.notification")
    }

    #[test]
    fn the_stable_notification_forwards_its_type_and_mentions() {
        let ev = stable_notification(json!({
            "notification_type": "notification",
            "sender_ts": 1_000,
            "lifetime": 30_000,
            "m.mentions": { "user_ids": ["@me:x"], "room": false },
        }));
        let payload = rtc_notification_payload(&ev, "!r:x", None, 7);
        assert_eq!(payload["type"], json!("call_rtc_notification"));
        assert_eq!(payload["notification_type"], json!("notification"));
        assert_eq!(payload["mentions_user_ids"], json!(["@me:x"]));
        assert_eq!(payload["mentions_room"], json!(false));
        assert_eq!(payload["event_id"], json!("$announce:x"));
        assert_eq!(payload["sender"], json!("@caller:x"));
        assert_eq!(payload["lifetime_ms"], json!(30_000));
        assert_eq!(payload["lifecycle"], json!(7));

        let ring = stable_notification(json!({
            "notification_type": "ring",
            "sender_ts": 1_000,
            "lifetime": 30_000,
            "m.mentions": { "room": true },
        }));
        let payload = rtc_notification_payload(&ring, "!r:x", None, 7);
        assert_eq!(payload["notification_type"], json!("ring"));
        assert_eq!(payload["mentions_user_ids"], json!([]));
        assert_eq!(payload["mentions_room"], json!(true));
    }

    #[test]
    fn mentions_me_is_decided_on_the_whole_list_before_the_bound() {
        let me = UserId::parse("@me:x").expect("valid user id");
        let mut ids: Vec<_> = (0..(MAX_RTC_MENTION_IDS + 10))
            .map(|i| UserId::parse(format!("@u{i}:x")).expect("valid user id"))
            .collect();
        ids.push(me.clone());
        let many = Mentions::with_user_ids(ids);
        let fields = rtc_notification_addressing("ring", Some(&many), Some(&me));
        assert_eq!(
            fields["mentions_user_ids"].as_array().map(Vec::len),
            Some(MAX_RTC_MENTION_IDS)
        );
        assert_eq!(fields["mentions_me"], json!(true));
        let others = Mentions::with_user_ids([UserId::parse("@them:x").unwrap()]);
        let fields = rtc_notification_addressing("ring", Some(&others), Some(&me));
        assert_eq!(fields["mentions_me"], json!(false));
        let fields = rtc_notification_addressing("ring", None, Some(&me));
        assert_eq!(fields["mentions_me"], json!(false));
        let fields = rtc_notification_addressing("ring", Some(&many), None);
        assert_eq!(fields["mentions_me"], json!(false));
        let ev = stable_notification(json!({
            "notification_type": "ring",
            "sender_ts": 1_000,
            "lifetime": 30_000,
            "m.mentions": { "user_ids": ["@me:x"] },
        }));
        let payload = rtc_notification_payload(&ev, "!r:x", Some(&me), 7);
        assert_eq!(payload["mentions_me"], json!(true));
        assert_eq!(payload["own"], json!(false));
    }

    #[test]
    fn notification_addressing_is_a_closed_set_and_bounded() {
        let fields = rtc_notification_addressing("RING!", None, None);
        assert_eq!(fields["notification_type"], json!("notification"));
        assert_eq!(fields["mentions_user_ids"], json!([]));
        assert_eq!(fields["mentions_room"], json!(false));

        let many = Mentions::with_user_ids((0..(MAX_RTC_MENTION_IDS + 44)).map(|i| {
            UserId::parse(format!("@u{i}:x")).expect("valid user id")
        }));
        let fields = rtc_notification_addressing("ring", Some(&many), None);
        assert_eq!(fields["notification_type"], json!("ring"));
        assert_eq!(
            fields["mentions_user_ids"].as_array().map(Vec::len),
            Some(MAX_RTC_MENTION_IDS)
        );
    }

    #[test]
    fn only_json_naming_no_server_counts_as_an_empty_answer() {
        #[derive(serde::Deserialize, Debug)]
        #[allow(dead_code)]
        struct Turn {
            username: String,
            password: String,
            uris: Vec<String>,
            ttl: u64,
        }
        let empty = serde_json::from_str::<Turn>("{}").unwrap_err();
        assert!(json_error_is_empty_answer(&empty));
        let html = serde_json::from_str::<Turn>("<html><body>Login</body></html>")
            .unwrap_err();
        assert!(!json_error_is_empty_answer(&html));
    }

    #[test]
    fn an_empty_turn_answer_is_none_configured_not_network() {
        assert_eq!(
            turn_failure_category(false, true, "missing field `username`"),
            "none_configured"
        );
        assert_eq!(
            turn_failure_category(false, false, "error sending request"),
            "network"
        );
        assert_eq!(
            turn_failure_category(true, false, "M_LIMIT_EXCEEDED: slow down"),
            "rate_limited"
        );
    }

    #[test]
    fn a_rejected_video_section_is_not_a_video_offer() {
        assert!(sdp_offers_video("v=0\r\nm=audio 9 UDP\r\nm=video 9 UDP\r\n"));
        assert!(!sdp_offers_video("v=0\r\nm=audio 9 UDP\r\nm=video 0 UDP\r\n"));
        assert!(!sdp_offers_video("v=0\r\nm=audio 9 UDP\r\n"));
        assert!(sdp_offers_video(
            "m=audio 9 UDP\r\nm=video 0 UDP\r\na=mid:1\r\na=bundle-only\r\n"
        ));
        assert!(!sdp_offers_video(
            "m=video 0 UDP\r\na=mid:1\r\nm=audio 9 UDP\r\na=bundle-only\r\n"
        ));
        assert!(!sdp_offers_video("m=video 9 UDP\r\na=inactive\r\n"));
    }

    #[test]
    fn stream_metadata_is_bounded_and_reaches_the_wire_shape() {
        assert!(parse_stream_metadata("").unwrap().is_empty());
        assert!(parse_stream_metadata("[]").is_err());
        let map = parse_stream_metadata(
            r#"{"s1":{"purpose":"m.usermedia","audio_muted":true,"video_muted":false}}"#,
        )
        .unwrap();
        assert_eq!(usermedia_muted(&map), Some((true, false)));
        let many: serde_json::Map<String, serde_json::Value> = (0..9)
            .map(|n| (format!("s{n}"), json!({"purpose": "m.usermedia"})))
            .collect();
        assert!(parse_stream_metadata(&serde_json::Value::Object(many).to_string()).is_err());
    }

    #[test]
    fn an_unmute_is_written_out_because_the_peer_merges() {
        let json = r#"{"s1":{"purpose":"m.usermedia","audio_muted":false,"video_muted":false}}"#;
        let content = CallSdpStreamMetadataChangedEventContent::new(
            "call".into(), "party".into(), VoipVersionId::V1,
            parse_stream_metadata(json).unwrap(),
        );
        let raw = with_explicit_metadata(&content, json).unwrap();
        assert_eq!(raw["sdp_stream_metadata"]["s1"]["audio_muted"], json!(false));
        assert_eq!(raw["sdp_stream_metadata"]["s1"]["video_muted"], json!(false));
        assert_eq!(raw["call_id"], json!("call"));
        assert_eq!(raw["version"], json!("1"));
    }

    #[test]
    fn only_a_usermedia_stream_speaks_for_the_camera() {
        let map = parse_stream_metadata(
            r#"{"s1":{"purpose":"m.screenshare","video_muted":true}}"#,
        )
        .unwrap();
        assert_eq!(usermedia_muted(&map), None);
    }

    #[test]
    fn inbound_reasons_are_a_closed_set() {
        assert_eq!(reason_str(&Reason::UserHangup), "user_hangup");
        assert_eq!(reason_str(&Reason::InviteTimeout), "invite_timeout");
        assert_eq!(reason_str(&Reason::UserBusy), "user_busy");
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
        let description = SessionDescription::new(
            "offer".to_owned(),
            "v=0\r\nc=IN IP4 192.168.1.4".to_owned(),
        );
        assert_eq!(session_type_str(&description.session_type), "offer");
        assert!(!description.sdp.is_empty());
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
        assert!(carried_candidate_line("").is_some());
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
