
use std::sync::Arc;

use matrix_sdk::ruma::api::error::ErrorBody;
use matrix_sdk::ruma::RoomId;
use serde_json::json;

use crate::rooms::{require_client, sniff_image_mime, MAX_AVATAR_BYTES};
use crate::{enqueue, RustClient};

/// Runs on the room-action pool (joined at sign-out); bounded so a hung
/// request becomes a reported failure instead of stalling teardown.
const DISPLAY_NAME_REQUEST_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(15);

const DISPLAY_NAME_MAX_CHARS: usize = 255;

pub(crate) fn bound_display_name(name: &str) -> String {
    name.chars().take(DISPLAY_NAME_MAX_CHARS).collect()
}

pub(crate) fn sanitize_error_message(message: &str) -> String {
    let collapsed: String = message
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    collapsed
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(200)
        .collect()
}

/// The server's own human-readable refusal message, if any: only
/// `StandardErrorBody.message`, never the error's `Display` (which can carry
/// the request URL). Empty means C++ supplies its own translated wording.
fn server_error_message(err: &matrix_sdk::Error) -> String {
    let Some(api) = err.as_client_api_error() else {
        return String::new();
    };
    let ErrorBody::Standard(body) = &api.body else {
        return String::new();
    };
    sanitize_error_message(&body.message)
}

pub(crate) fn set_own_display_name(
    bridge: &RustClient,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let bounded = bound_display_name(&name);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let arg = if bounded.is_empty() {
            None
        } else {
            Some(bounded.as_str())
        };
        let result = tokio::time::timeout(
            DISPLAY_NAME_REQUEST_TIMEOUT,
            client.account().set_display_name(arg),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(())) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            Err(_) => (false, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "own_display_name_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "error": error,
            }),
        );
    });
    Ok(())
}

pub(crate) fn set_room_display_name(
    bridge: &RustClient,
    room_id: String,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let bounded = bound_display_name(&name);
    let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
    let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let arg = if bounded.is_empty() { None } else { Some(bounded.clone()) };
        let result = tokio::time::timeout(
            DISPLAY_NAME_REQUEST_TIMEOUT,
            room.set_own_member_display_name(arg),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(_)) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            Err(_) => (false, String::new()),
        };
        enqueue(&events, json!({
            "type": "room_profile_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "field": "displayname",
            "ok": ok,
            "error": error,
        }));
    });
    Ok(())
}

const PROFILE_KEYS: [&str; 2] = ["displayname", "avatar_url"];

pub(crate) fn set_room_avatar(
    bridge: &RustClient,
    room_id: String,
    source: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let upload_from = if source.is_empty() || source.starts_with("mxc://") {
        None
    } else {
        let metadata = std::fs::metadata(&source)
            .map_err(|_| "avatar file is not readable".to_owned())?;
        if !metadata.is_file() {
            return Err("avatar path is not a regular file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_AVATAR_BYTES {
            return Err("avatar file size is out of range".to_owned());
        }
        Some(source.clone())
    };
    let mxc = if upload_from.is_some() { String::new() } else { source };
    if !mxc.is_empty() && (!mxc.starts_with("mxc://") || mxc.len() <= "mxc://".len()) {
        return Err("avatar must be an mxc URI".to_owned());
    }
    let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
    let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
    let user_id = client.user_id().ok_or_else(|| "not signed in".to_owned())?.to_owned();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let outcome = async {
            let mut mxc = mxc;
            if let Some(path) = upload_from {
                let data = tokio::fs::read(&path)
                    .await
                    .map_err(|_| "avatar file is not readable".to_owned())?;
                let mime_str = sniff_image_mime(&data)
                    .ok_or_else(|| "unsupported image".to_owned())?;
                let mime: mime::Mime = mime_str
                    .parse()
                    .map_err(|_| "unsupported image".to_owned())?;
                let uploaded = client
                    .media()
                    .upload(&mime, data, None)
                    .await
                    .map_err(|e| server_error_message(&e))?;
                mxc = uploaded.content_uri.to_string();
            }
            // Read the raw event, not the typed content (see above).
            let raw = room
                .get_state_event(
                    matrix_sdk::ruma::events::StateEventType::RoomMember,
                    user_id.as_str(),
                )
                .await
                .map_err(|e| server_error_message(&e))?
                .ok_or_else(|| "no membership to edit".to_owned())?;
            let matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(
                sync_raw,
            ) = raw
            else {
                return Err("membership is not editable here".to_owned());
            };
            let value: serde_json::Value = sync_raw
                .deserialize_as_unchecked::<serde_json::Value>()
                .map_err(|_| "membership could not be read".to_owned())?;
            let mut content = value
                .get("content")
                .cloned()
                .unwrap_or_else(|| json!({}));
            let object = content
                .as_object_mut()
                .ok_or_else(|| "membership has no content".to_owned())?;
            if object.get("membership").and_then(|v| v.as_str()) != Some("join") {
                return Err("not a joined membership".to_owned());
            }
            if mxc.is_empty() {
                object.remove("avatar_url");
            } else {
                object.insert("avatar_url".to_owned(), json!(mxc));
            }
            room.send_state_event_raw("m.room.member", user_id.as_str(), content)
                .await
                .map(|_| ())
                .map_err(|e| server_error_message(&e))
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match outcome {
            Ok(()) => (true, String::new()),
            Err(message) => (false, message),
        };
        enqueue(&events, json!({
            "type": "room_profile_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "field": "avatar_url",
            "ok": ok,
            "error": error,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bound_keeps_short_names_byte_identical() {
        for name in [
            "Alice",
            "Rokas Smetonis",
            "Ąžuolas Užupis",
            "日本語の名前",
            "🦊",
            "👨‍👩‍👧‍👦",
            "e\u{0301}\u{0327}",
            "Ω mixed Ωmega 漢字 🦊",
        ] {
            assert_eq!(bound_display_name(name), name, "mangled {name:?}");
        }
    }

    #[test]
    fn bound_cuts_at_a_scalar_boundary_never_inside_one() {
        let name = format!("{}{}", "a".repeat(254), '\u{1F98A}');
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded, name);
        assert!(bounded.ends_with('\u{1F98A}'));
        assert_eq!(bounded.chars().last(), Some('\u{1F98A}'));

        let over = format!("{}{}", "a".repeat(255), '\u{1F98A}');
        let bounded_over = bound_display_name(&over);
        assert_eq!(bounded_over.chars().count(), 255);
        assert!(!bounded_over.contains('\u{1F98A}'));
        assert_eq!(bounded_over, "a".repeat(255));
    }

    #[test]
    fn bound_truncates_long_non_latin_names_at_255_scalars() {
        let name = "\u{65E5}".repeat(300);
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded.len(), 255 * 3);
    }

    #[test]
    fn empty_name_stays_empty_so_the_caller_can_map_it_to_none() {
        assert!(bound_display_name("").is_empty());
    }

    #[test]
    fn error_messages_are_collapsed_and_bounded() {
        assert_eq!(
            sanitize_error_message("Display name too\n\tlong   for  server"),
            "Display name too long for server"
        );
        assert_eq!(sanitize_error_message("   "), "");
        let long = "x".repeat(500);
        assert_eq!(sanitize_error_message(&long).chars().count(), 200);
    }
}

pub(crate) fn set_own_avatar(
    bridge: &RustClient,
    local_path: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
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
            client
                .account()
                .upload_avatar(&mime, data)
                .await
                .map(|_| ())
                .map_err(|err| server_error_message(&err))
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(()) => (true, String::new()),
            Err(err) => (false, err),
        };
        enqueue(
            &events,
            json!({
                "type": "own_avatar_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "error": error,
            }),
        );
    });
    Ok(())
}

pub(crate) fn clear_own_avatar(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(
            DISPLAY_NAME_REQUEST_TIMEOUT,
            client.account().set_avatar_url(None),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(())) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            Err(_) => (false, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "own_avatar_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "error": error,
            }),
        );
    });
    Ok(())
}

const MAX_MUTUAL_ROOMS: usize = 24;

pub(crate) fn mutual_rooms(
    bridge: &RustClient,
    user_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let target = matrix_sdk::ruma::UserId::parse(user_id.as_str())
        .map_err(|_| "invalid user id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut rooms: Vec<serde_json::Value> = Vec::new();
        for room in client.joined_rooms() {
            if rooms.len() >= MAX_MUTUAL_ROOMS {
                break;
            }
            let joined = matches!(
                room.get_member_no_sync(&target).await,
                Ok(Some(ref m))
                    if m.membership()
                        == &matrix_sdk::ruma::events::room::member::MembershipState::Join
            );
            if !joined {
                continue;
            }
            rooms.push(json!({
                "room_id": room.room_id().to_string(),
                "name": room.cached_display_name()
                    .map(|n| n.to_string())
                    .unwrap_or_default(),
                "avatar_url": room.avatar_url().map(|u| u.to_string())
                    .unwrap_or_default(),
                "is_direct": room.is_direct().await.unwrap_or(false),
            }));
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "mutual_rooms_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "user_id": user_id,
                "rooms": rooms,
            }),
        );
    });
    Ok(())
}
