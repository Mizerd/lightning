//! Room management, user search and the media bridge (v0.5.9).
//!
//! Every function here is invoked from a thin `extern "C"` wrapper in
//! `lib.rs`, does its synchronous validation on the caller's thread, and
//! spawns the network work as a *managed* task (`spawn_room_action`) on the
//! shared runtime so sign-out joins it deterministically. Async completions
//! are stamped with the lifecycle generation and an operation id; C++
//! rejects stale generations and unknown operation ids.
//!
//! Nothing in this module serializes key material, access tokens, raw
//! events, local file paths (into logs), or media bytes into the JSON event
//! queue. Media bytes cross the FFI through the dedicated take/free binary
//! bridge in `lib.rs`.

use std::sync::Arc;

use matrix_sdk::{
    attachment::{AttachmentInfo, BaseImageInfo},
    media::{MediaFormat, MediaRequestParameters, MediaThumbnailSettings},
    ruma::{
        api::client::{
            media::get_content_thumbnail::v3::Method,
            room::{
                create_room::{self, v3::RoomPreset},
                Visibility,
            },
        },
        assign,
        events::{
            room::encryption::RoomEncryptionEventContent, space::child::SpaceChildEventContent,
            InitialStateEvent, StateEventType,
        },
        OwnedMxcUri, OwnedUserId, RoomId, UInt, UserId,
    },
    RoomMemberships, RoomState,
};
use matrix_sdk_ui::timeline::AttachmentSource;
use serde::Deserialize;
use serde_json::json;

use crate::{enqueue, RustClient};

/// Upper bound on one member-snapshot payload. Rooms larger than this are
/// truncated (flagged in the event) — the UI shows counts, not 10k rows.
const MEMBER_SNAPSHOT_CAP: usize = 500;

/// Avatar uploads are small; refuse anything larger before reading it.
const MAX_AVATAR_BYTES: u64 = 8 * 1024 * 1024;

/// Last-resort attachment bound when the server's m.upload.size is unknown.
/// C++ enforces the real (server-provided) limit before dispatching.
pub(crate) const FALLBACK_UPLOAD_LIMIT: u64 = 100 * 1024 * 1024;

fn require_client(bridge: &RustClient) -> Result<matrix_sdk::Client, String> {
    bridge
        .client
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
        .ok_or_else(|| "no active Matrix session".to_owned())
}

fn joined_room(
    client: &matrix_sdk::Client,
    room_id: &str,
) -> Result<matrix_sdk::Room, String> {
    RoomId::parse(room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown or not-joined room".to_owned())
}

/// Coarse, non-secret categories for room-management failures. The raw SDK
/// error text may embed server detail; only the category crosses to QML
/// user strings. Pure and unit-tested.
pub(crate) fn classify_room_error(message: &str) -> &'static str {
    let lc = message.to_lowercase();
    if lc.contains("m_limit_exceeded") || lc.contains("limit exceeded") || lc.contains("429") {
        "rate_limited"
    } else if lc.contains("m_forbidden") || lc.contains("forbidden") || lc.contains("403") {
        "forbidden"
    } else if lc.contains("m_room_in_use") || lc.contains("alias") && lc.contains("use") {
        "alias_taken"
    } else if lc.contains("m_invalid") || lc.contains("invalid") {
        "invalid"
    } else if lc.contains("m_not_found") || lc.contains("not found") || lc.contains("404") {
        "not_found"
    } else {
        "network"
    }
}

/// Sniff a raster-image MIME type from magic bytes. Used for avatar upload
/// and clipboard data so a mislabelled extension cannot spoof the type.
/// Pure and unit-tested.
pub(crate) fn sniff_image_mime(bytes: &[u8]) -> Option<&'static str> {
    if bytes.len() < 12 {
        return None;
    }
    if bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]) {
        Some("image/png")
    } else if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some("image/jpeg")
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some("image/gif")
    } else if bytes.starts_with(b"RIFF") && &bytes[8..12] == b"WEBP" {
        Some("image/webp")
    } else if bytes.starts_with(b"BM") {
        Some("image/bmp")
    } else {
        None
    }
}

// ---------------------------------------------------------------------------
// User search
// ---------------------------------------------------------------------------

pub(crate) fn search_users(
    bridge: &RustClient,
    query: String,
    limit: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.search_users(&query, limit.clamp(1, 50)).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(response) => {
                let results: Vec<serde_json::Value> = response
                    .results
                    .iter()
                    .map(|user| {
                        json!({
                            "user_id": user.user_id.to_string(),
                            "display_name": user.display_name.clone().unwrap_or_default(),
                            "avatar_url": user
                                .avatar_url
                                .as_ref()
                                .map(|a| a.to_string())
                                .unwrap_or_default(),
                        })
                    })
                    .collect();
                enqueue(&events, json!({
                    "type": "user_search_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "limited": response.limited,
                    "results": results,
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "user_search_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                    "results": [],
                }));
            }
        }
    });
    Ok(())
}

/// v0.5.11: exact profile lookup for one user id (GET /profile/{userId}).
/// Backs the bare-localpart invite search: the directory may not list local
/// users, so a plausible `@localpart:own-server` candidate is confirmed (or
/// refuted) against the homeserver before it is offered as a result. Only
/// display name and avatar mxc cross the FFI; a missing user surfaces as
/// ok=false with category "not_found".
pub(crate) fn fetch_user_profile(
    bridge: &RustClient,
    user_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId =
        UserId::parse(&user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.account().fetch_user_profile_of(&uid).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(profile) => {
                let display_name = profile
                    .get("displayname")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_owned();
                let avatar_url = profile
                    .get("avatar_url")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_owned();
                enqueue(&events, json!({
                    "type": "user_profile_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "user_id": uid.to_string(),
                    "display_name": display_name,
                    "avatar_url": avatar_url,
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "user_profile_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "user_id": uid.to_string(),
                    "category": classify_room_error(&err.to_string()),
                }));
            }
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// URL previews (v0.5.11)
// ---------------------------------------------------------------------------

/// Fields extracted from a homeserver OpenGraph preview response. Only these
/// whitelisted values ever cross the FFI — the raw response may echo the
/// URL, which must not be logged or forwarded wholesale.
///
/// og:image is an MXC URI (per the Matrix preview_url contract) suitable for
/// the existing media bridge; og:image:type is the server-validated MIME
/// type that GIF classification trusts INSTEAD of the URL suffix.
fn preview_fields(data: &serde_json::Value) -> serde_json::Value {
    let text = |key: &str| {
        data.get(key)
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_owned()
    };
    // OpenGraph numeric fields arrive as numbers or numeric strings
    // depending on the homeserver; accept both.
    let number = |key: &str| -> u64 {
        match data.get(key) {
            Some(serde_json::Value::Number(n)) => n.as_u64().unwrap_or(0),
            Some(serde_json::Value::String(s)) => s.parse().unwrap_or(0),
            _ => 0,
        }
    };
    let image = text("og:image");
    json!({
        "title": text("og:title"),
        "description": text("og:description"),
        "site_name": text("og:site_name"),
        // Only MXC references are forwarded; the media bridge cannot (and
        // must not) fetch arbitrary HTTP image URLs from the client.
        "image_mxc": if image.starts_with("mxc://") { image } else { String::new() },
        "image_mime": text("og:image:type"),
        "image_width": number("og:image:width"),
        "image_height": number("og:image:height"),
        "image_size": number("matrix:image:size"),
    })
}

/// Ask the homeserver for a URL preview (GET /_matrix/client/v1/media/
/// preview_url). The homeserver performs the outbound fetch, so no
/// local-network or arbitrary-URL access ever happens from Lightning
/// itself. The URL is never logged and never echoed on the result event —
/// C++ correlates by op_id.
pub(crate) fn fetch_url_preview(
    bridge: &RustClient,
    url: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let lowered = url.trim().to_lowercase();
    if !(lowered.starts_with("https://") || lowered.starts_with("http://")) {
        return Err("unsupported URL scheme".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        use matrix_sdk::ruma::api::client::authenticated_media::get_media_preview;
        let request = get_media_preview::v1::Request::new(url);
        let result = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(response) => {
                let data = response
                    .data
                    .as_deref()
                    .and_then(|raw| serde_json::from_str::<serde_json::Value>(raw.get()).ok())
                    .unwrap_or_else(|| json!({}));
                let mut out = json!({
                    "type": "url_preview_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                });
                out["fields"] = preview_fields(&data);
                enqueue(&events, out);
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "url_preview_result",
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

// ---------------------------------------------------------------------------
// Direct messages
// ---------------------------------------------------------------------------

/// Existing joined DM rooms with `user_id`, straight from the SDK's
/// authoritative m.direct projection. Synchronous (store lookup only).
pub(crate) fn get_dm_rooms(bridge: &RustClient, user_id: &str) -> Result<String, String> {
    let client = require_client(bridge)?;
    let uid = UserId::parse(user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    let rooms: Vec<serde_json::Value> = client
        .get_dm_rooms(&uid)
        .map(|room| {
            json!({
                "room_id": room.room_id().to_string(),
                "name": room.name().unwrap_or_default(),
            })
        })
        .collect();
    serde_json::to_string(&json!({ "rooms": rooms }))
        .map_err(|_| "serialization failed".to_owned())
}

/// Create an encrypted DM with `user_id` via the pinned `Client::create_dm`
/// (TrustedPrivateChat preset, encryption initial state, `is_direct`, and
/// the SDK's locked read-modify-write m.direct update).
pub(crate) fn create_dm(bridge: &RustClient, user_id: String, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId =
        UserId::parse(&user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    if client.user_id() == Some(uid.as_ref()) {
        return Err("cannot start a direct message with yourself".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.create_dm(&uid).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room) => {
                enqueue(&events, json!({
                    "type": "dm_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                }));
                crate::enqueue_rooms(&events, &client).await;
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "dm_create_result",
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

// ---------------------------------------------------------------------------
// Room creation
// ---------------------------------------------------------------------------

#[derive(Debug, Default, Deserialize, PartialEq)]
pub(crate) struct CreateRoomOptions {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub public: bool,
    #[serde(default)]
    pub encrypted: bool,
    #[serde(default)]
    pub alias: String,
    #[serde(default)]
    pub invites: Vec<String>,
    #[serde(default)]
    pub space_id: String,
}

/// Build the pinned ruma create-room request from validated options.
/// No room id or room version is generated locally — the server decides.
pub(crate) fn build_create_room_request(
    opts: &CreateRoomOptions,
) -> Result<create_room::v3::Request, String> {
    if opts.name.trim().is_empty() {
        return Err("room name must not be empty".to_owned());
    }
    let mut invites: Vec<OwnedUserId> = Vec::new();
    for user in &opts.invites {
        let uid = UserId::parse(user)
            .map_err(|_| format!("invalid invite user id: {user}"))?;
        if !invites.contains(&uid) {
            invites.push(uid);
        }
    }
    let initial_state = if opts.encrypted {
        vec![InitialStateEvent::with_empty_state_key(
            RoomEncryptionEventContent::with_recommended_defaults(),
        )
        .to_raw_any()]
    } else {
        Vec::new()
    };
    let request = assign!(create_room::v3::Request::new(), {
        name: Some(opts.name.trim().to_owned()),
        topic: (!opts.topic.trim().is_empty()).then(|| opts.topic.trim().to_owned()),
        invite: invites,
        is_direct: false,
        preset: Some(if opts.public {
            RoomPreset::PublicChat
        } else {
            RoomPreset::PrivateChat
        }),
        visibility: if opts.public { Visibility::Public } else { Visibility::Private },
        room_alias_name: (!opts.alias.trim().is_empty())
            .then(|| opts.alias.trim().to_owned()),
        initial_state,
    });
    Ok(request)
}

pub(crate) fn create_room(
    bridge: &RustClient,
    options_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let opts: CreateRoomOptions = serde_json::from_str(&options_json)
        .map_err(|_| "invalid room options".to_owned())?;
    let request = build_create_room_request(&opts)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.create_room(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room) => {
                // Optional Space placement. Failure here must not read as a
                // failed room creation — it is reported as a warning.
                let mut warning = String::new();
                if !opts.space_id.is_empty() {
                    let placed = add_space_child(&client, &opts.space_id, room.room_id().as_str())
                        .await
                        .is_ok();
                    if !placed {
                        warning = "space_add_failed".to_owned();
                    }
                }
                enqueue(&events, json!({
                    "type": "room_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                    "warning": warning,
                }));
                crate::enqueue_rooms(&events, &client).await;
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "room_create_result",
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

async fn add_space_child(
    client: &matrix_sdk::Client,
    space_id: &str,
    child_room_id: &str,
) -> Result<(), String> {
    let space = RoomId::parse(space_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown space".to_owned())?;
    let server = client
        .user_id()
        .map(|u| u.server_name().to_owned())
        .ok_or_else(|| "no session".to_owned())?;
    space
        .send_state_event_for_key(
            &RoomId::parse(child_room_id).map_err(|_| "invalid room id".to_owned())?,
            SpaceChildEventContent::new(vec![server]),
        )
        .await
        .map(|_| ())
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())
}

pub(crate) fn add_room_to_space(
    bridge: &RustClient,
    space_id: String,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let ok = add_space_child(&client, &space_id, &room_id).await.is_ok();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "space_child_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "space_id": space_id,
            "room_id": room_id,
            "ok": ok,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Invites to existing rooms
// ---------------------------------------------------------------------------

pub(crate) fn invite_users(
    bridge: &RustClient,
    room_id: String,
    users_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let users: Vec<String> =
        serde_json::from_str(&users_json).map_err(|_| "invalid invite list".to_owned())?;
    if users.is_empty() {
        return Err("no users to invite".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut ok_count = 0u32;
        let mut fail_count = 0u32;
        let mut seen: Vec<String> = Vec::new();
        for user in users {
            if seen.contains(&user) {
                continue;
            }
            seen.push(user.clone());
            let outcome = match UserId::parse(&user) {
                Err(_) => Err("invalid_user".to_owned()),
                Ok(uid) => room
                    .invite_user_by_id(&uid)
                    .await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned()),
            };
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            match outcome {
                Ok(()) => ok_count += 1,
                Err(_) => fail_count += 1,
            }
            enqueue(&events, json!({
                "type": "room_invite_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "user_id": user,
                "ok": outcome.is_ok(),
                "category": outcome.err().unwrap_or_default(),
            }));
        }
        enqueue(&events, json!({
            "type": "room_invite_done",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok_count": ok_count,
            "fail_count": fail_count,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Member snapshot + permissions
// ---------------------------------------------------------------------------

pub(crate) fn room_members(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let own_id = client.user_id().map(|u| u.to_owned());
    bridge.spawn_room_action(async move {
        let members = match room.members(RoomMemberships::ACTIVE).await {
            Ok(members) => members,
            Err(err) => {
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, json!({
                        "type": "room_members",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "room_id": room_id,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
                return;
            }
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        let mut sorted: Vec<&matrix_sdk::room::RoomMember> = members.iter().collect();
        sorted.sort_by(|a, b| {
            use matrix_sdk::ruma::events::room::member::MembershipState;
            let joined_first = |m: &matrix_sdk::room::RoomMember| {
                matches!(m.membership(), MembershipState::Join)
            };
            joined_first(b)
                .cmp(&joined_first(a))
                .then(b.power_level().cmp(&a.power_level()))
                .then_with(|| a.name().to_lowercase().cmp(&b.name().to_lowercase()))
        });

        let truncated = sorted.len() > MEMBER_SNAPSHOT_CAP;
        let mut joined_count = 0u64;
        let mut invited_count = 0u64;
        for member in &sorted {
            use matrix_sdk::ruma::events::room::member::MembershipState;
            match member.membership() {
                MembershipState::Join => joined_count += 1,
                MembershipState::Invite => invited_count += 1,
                _ => {}
            }
        }

        let rows: Vec<serde_json::Value> = sorted
            .iter()
            .take(MEMBER_SNAPSHOT_CAP)
            .map(|member| {
                use matrix_sdk::ruma::events::room::member::MembershipState;
                let role = format!("{:?}", member.suggested_role_for_power_level())
                    .to_lowercase();
                json!({
                    "user_id": member.user_id().to_string(),
                    "display_name": member.display_name().unwrap_or_default(),
                    "avatar_url": member
                        .avatar_url()
                        .map(|a| a.to_string())
                        .unwrap_or_default(),
                    "membership": match member.membership() {
                        MembershipState::Join => "joined",
                        MembershipState::Invite => "invited",
                        _ => "other",
                    },
                    "role": role,
                    "ambiguous": member.name_ambiguous(),
                    "is_own": Some(member.user_id())
                        == own_id.as_ref().map(|o| o.as_ref()),
                })
            })
            .collect();

        // Own permissions, from the SDK's power-level helpers — never
        // guessed from role labels or room-creator status.
        let own_member = match &own_id {
            Some(own) => room.get_member_no_sync(own).await.ok().flatten(),
            None => None,
        };
        let can_invite = own_member.as_ref().is_some_and(|m| m.can_invite());
        let can_edit_name = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomName));
        let can_edit_topic = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomTopic));
        let can_edit_avatar = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomAvatar));

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_members",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": true,
            "truncated": truncated,
            "joined_count": joined_count,
            "invited_count": invited_count,
            "own_can_invite": can_invite,
            "own_can_edit_name": can_edit_name,
            "own_can_edit_topic": can_edit_topic,
            "own_can_edit_avatar": can_edit_avatar,
            "members": rows,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Room profile editing + leave
// ---------------------------------------------------------------------------

fn emit_edit_result(
    events: &crate::EventQueueRef,
    op_id: u64,
    lifecycle: u64,
    room_id: &str,
    field: &str,
    result: Result<(), String>,
) {
    enqueue(events, json!({
        "type": "room_edit_result",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "room_id": room_id,
        "field": field,
        "ok": result.is_ok(),
        "category": result.err().unwrap_or_default(),
    }));
}

pub(crate) fn set_room_name(
    bridge: &RustClient,
    room_id: String,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    if name.trim().is_empty() {
        return Err("room name must not be empty".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .set_name(name.trim().to_owned())
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "name", result);
    });
    Ok(())
}

pub(crate) fn set_room_topic(
    bridge: &RustClient,
    room_id: String,
    topic: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .set_room_topic(topic.trim())
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "topic", result);
    });
    Ok(())
}

pub(crate) fn set_room_avatar(
    bridge: &RustClient,
    room_id: String,
    local_path: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let metadata = std::fs::metadata(&local_path)
        .map_err(|_| "avatar file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("avatar path is not a regular file".to_owned());
    }
    if metadata.len() == 0 || metadata.len() > MAX_AVATAR_BYTES {
        return Err("avatar file size is out of range".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = async {
            let data = tokio::fs::read(&local_path)
                .await
                .map_err(|_| "read_failed".to_owned())?;
            let mime_str =
                sniff_image_mime(&data).ok_or_else(|| "unsupported_image".to_owned())?;
            let mime: mime::Mime =
                mime_str.parse().map_err(|_| "unsupported_image".to_owned())?;
            room.upload_avatar(&mime, data, None)
                .await
                .map(|_| ())
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "avatar", result);
    });
    Ok(())
}

pub(crate) fn remove_room_avatar(
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
        let result = room
            .remove_avatar()
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "avatar", result);
    });
    Ok(())
}

pub(crate) fn leave_room(
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
        let result = room.leave().await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_leave_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": result.is_ok(),
            "category": result
                .err()
                .map(|err| classify_room_error(&err.to_string()))
                .unwrap_or(""),
        }));
        crate::enqueue_rooms(&events, &client).await;
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Attachment sending
// ---------------------------------------------------------------------------

/// Image metadata for the media event; dimensions come from C++ (bounded
/// Qt-side decode), size from the filesystem.
fn image_info(width: u64, height: u64, size: u64, animated: bool) -> Option<AttachmentInfo> {
    if width == 0 && height == 0 {
        return None;
    }
    Some(AttachmentInfo::Image(BaseImageInfo {
        width: UInt::new(width),
        height: UInt::new(height),
        size: UInt::new(size),
        blurhash: None,
        is_animated: Some(animated),
    }))
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_attachment_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    animated: bool,
    op_id: u64,
) -> Result<(), String> {
    let metadata =
        std::fs::metadata(&path).map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    let info = if mime.starts_with("image/") {
        image_info(width, height, metadata.len(), animated)
    } else {
        None
    };
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        op_id,
    )
}

/// Clipboard image path: bytes are handed over directly (bounded by C++),
/// so no temporary file ever exists on disk.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_attachment_bytes(
    bridge: &RustClient,
    room_id: String,
    bytes: Vec<u8>,
    filename: String,
    mime: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    // Never trust the caller's label alone for raster images.
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("clipboard data is not a supported image".to_owned());
    }
    let size = bytes.len() as u64;
    let info = if mime.starts_with("image/") {
        image_info(width, height, size, mime == "image/gif")
    } else {
        None
    };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        op_id,
    )
}

// ---------------------------------------------------------------------------
// Media retrieval (the download half of the media bridge)
// ---------------------------------------------------------------------------

fn emit_media_failed(
    events: &crate::EventQueueRef,
    op_id: u64,
    lifecycle: u64,
    key: &str,
    kind: u32,
    category: &str,
) {
    enqueue(events, json!({
        "type": "media_failed",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "key": key,
        "kind": kind,
        "category": category,
    }));
}

/// Fetch (and for encrypted rooms, decrypt) media for a timeline item whose
/// source was captured by the timeline serializer. `kind`: 0 = full, 1 =
/// thumbnail (falls back to full when no thumbnail exists). Bytes are parked
/// in `media_results` for `mx_rust_media_take`; they never enter the JSON
/// queue.
pub(crate) fn media_fetch(
    bridge: &RustClient,
    key: String,
    kind: u32,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let Some((source, filename, mimetype)) =
        bridge.timelines.media_source(&key, kind == 1)
    else {
        return Err("unknown media item".to_owned());
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = MediaRequestParameters { source, format: MediaFormat::File };
        let outcome = client.media().get_media_content(&request, true).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(bytes) => {
                let size = bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                enqueue(&events, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": key,
                    "kind": kind,
                    "size": size,
                    "mimetype": mimetype.unwrap_or_default(),
                    "filename": filename,
                }));
            }
            Err(err) => {
                emit_media_failed(
                    &events,
                    op_id,
                    lifecycle,
                    &key,
                    kind,
                    classify_room_error(&err.to_string()),
                );
            }
        }
    });
    Ok(())
}

/// Fetch a server-side thumbnail for a plain (unencrypted) mxc URI — room,
/// user and space avatars. Encrypted media never routes through here.
pub(crate) fn media_fetch_mxc(
    bridge: &RustClient,
    mxc: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    if !mxc.starts_with("mxc://") {
        return Err("not an mxc URI".to_owned());
    }
    let uri: OwnedMxcUri = OwnedMxcUri::from(mxc.as_str());
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let format = if width == 0 || height == 0 {
            MediaFormat::File
        } else {
            MediaFormat::Thumbnail(MediaThumbnailSettings::with_method(
                Method::Scale,
                UInt::new_saturating(width),
                UInt::new_saturating(height),
            ))
        };
        let request = MediaRequestParameters {
            source: matrix_sdk::ruma::events::room::MediaSource::Plain(uri),
            format,
        };
        let outcome = client.media().get_media_content(&request, true).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(bytes) => {
                let size = bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                enqueue(&events, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": mxc,
                    "kind": 2u32,
                    "size": size,
                }));
            }
            Err(err) => {
                emit_media_failed(
                    &events,
                    op_id,
                    lifecycle,
                    &mxc,
                    2,
                    classify_room_error(&err.to_string()),
                );
            }
        }
    });
    Ok(())
}

/// Server upload limit (m.upload.size), fetched once per session by C++.
pub(crate) fn fetch_upload_limit(bridge: &RustClient) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let bytes = client
            .load_or_fetch_max_upload_size()
            .await
            .map(u64::from)
            .unwrap_or(FALLBACK_UPLOAD_LIMIT);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "upload_limit",
            "lifecycle": lifecycle,
            "bytes": bytes,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classify_room_error_categories() {
        assert_eq!(classify_room_error("M_LIMIT_EXCEEDED: too many requests"), "rate_limited");
        assert_eq!(classify_room_error("M_FORBIDDEN: not allowed"), "forbidden");
        assert_eq!(classify_room_error("M_ROOM_IN_USE: alias taken"), "alias_taken");
        assert_eq!(classify_room_error("M_INVALID_PARAM: bad alias"), "invalid");
        assert_eq!(classify_room_error("M_NOT_FOUND"), "not_found");
        assert_eq!(classify_room_error("connection reset by peer"), "network");
    }

    #[test]
    fn sniff_image_mime_by_magic_bytes() {
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        png.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&png), Some("image/png"));

        let mut jpg = vec![0xFF, 0xD8, 0xFF, 0xE0];
        jpg.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&jpg), Some("image/jpeg"));

        let mut gif = b"GIF89a".to_vec();
        gif.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&gif), Some("image/gif"));

        let mut webp = b"RIFF\x00\x00\x00\x00WEBP".to_vec();
        webp.extend_from_slice(&[0; 4]);
        assert_eq!(sniff_image_mime(&webp), Some("image/webp"));

        assert_eq!(sniff_image_mime(b"plain text, not an image"), None);
        assert_eq!(sniff_image_mime(b"tiny"), None);
    }

    #[test]
    fn create_room_options_parse_defaults() {
        let opts: CreateRoomOptions = serde_json::from_str("{\"name\":\"Test\"}").unwrap();
        assert_eq!(opts.name, "Test");
        assert!(!opts.public);
        assert!(!opts.encrypted);
        assert!(opts.invites.is_empty());
        assert!(opts.space_id.is_empty());
    }

    #[test]
    fn create_room_request_private_encrypted() {
        let opts = CreateRoomOptions {
            name: "Secret".to_owned(),
            encrypted: true,
            invites: vec!["@a:example.org".to_owned(), "@a:example.org".to_owned()],
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.name.as_deref(), Some("Secret"));
        assert_eq!(request.preset, Some(RoomPreset::PrivateChat));
        assert_eq!(request.visibility, Visibility::Private);
        // Encryption initial state present; duplicate invites deduplicated.
        assert_eq!(request.initial_state.len(), 1);
        assert_eq!(request.invite.len(), 1);
        assert!(!request.is_direct);
        // Server chooses the room version — never pinned locally.
        assert!(request.room_version.is_none());
    }

    #[test]
    fn create_room_request_public_with_alias() {
        let opts = CreateRoomOptions {
            name: "Town Square".to_owned(),
            public: true,
            alias: " town-square ".to_owned(),
            topic: " hello ".to_owned(),
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.preset, Some(RoomPreset::PublicChat));
        assert_eq!(request.visibility, Visibility::Public);
        assert_eq!(request.room_alias_name.as_deref(), Some("town-square"));
        assert_eq!(request.topic.as_deref(), Some("hello"));
        assert!(request.initial_state.is_empty());
    }

    #[test]
    fn create_room_request_rejects_empty_name_and_bad_invite() {
        let opts = CreateRoomOptions { name: "   ".to_owned(), ..Default::default() };
        assert!(build_create_room_request(&opts).is_err());

        let opts = CreateRoomOptions {
            name: "ok".to_owned(),
            invites: vec!["not-a-user-id".to_owned()],
            ..Default::default()
        };
        assert!(build_create_room_request(&opts).is_err());
    }
}
