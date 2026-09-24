//! The signed-in account's own profile.
//!
//! Display-name writes use the SDK's `Account::set_display_name` (the
//! displayname endpoint, or delete-profile-field when clearing and the server
//! supports it). This module adds a length bound, the session-generation
//! guard, and a sanitized result.
//!
//! The name is never logged and never echoed back across the FFI; C++
//! already has the string it submitted.

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

/// Client-side ceiling; Matrix specifies no maximum display-name length.
const DISPLAY_NAME_MAX_CHARS: usize = 255;

/// Bound a display name to [`DISPLAY_NAME_MAX_CHARS`] Unicode scalar values.
///
/// `chars()`, never a byte slice (which panics on a multi-byte boundary or
/// sends a split UTF-8 sequence). Scalar values also avoid cutting a
/// surrogate pair, as a UTF-16 bound would. A grapheme cluster may still be
/// split; accepted, since there is no segmentation crate in this offline
/// build and the result is always valid UTF-8.
pub(crate) fn bound_display_name(name: &str) -> String {
    name.chars().take(DISPLAY_NAME_MAX_CHARS).collect()
}

/// Collapse control characters and whitespace runs, then bound the length.
/// The message is server-authored and shown in a UI label.
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

/// Set or clear the account's display name. An empty `name` clears it:
/// `Some("")` would ask the server to store an empty name instead. The UI
/// makes clearing an explicit action, so an empty editor cannot arrive here
/// by accident.
///
/// Result event: `own_display_name_result { op_id, lifecycle, ok, error }`.
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
        // A completion that outlived its session must not be reported to the next
        // account.
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(())) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            // A timeout has no server message; C++ words it.
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

// ── Per-room profiles ────────────────────────────────────────────────────
//
// The display name and avatar this account shows in one room, stored in
// its own `m.room.member` event there.
//
// Every field of that event not copied forward is deleted
// (`RoomMemberEventContent` skips absent fields), including
// `join_authorised_via_users_server`, which keeps a restricted-room
// membership valid. So the avatar path edits the raw event's one key rather
// than round-tripping the typed struct (which has no catch-all for unknown
// fields). The SDK's `set_own_member_display_name` has the same weakness
// and also flattens a redacted event; it is still used for the name, where
// it is the supported API and a joined member editing themselves cannot hit
// the redacted case.

/// Set or clear (empty) this account's display name in one room.
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

/// The member-event keys a per-room profile edit may touch; everything else
/// is carried forward verbatim.
const PROFILE_KEYS: [&str; 2] = ["displayname", "avatar_url"];

/// Set or clear this account's avatar in one room. Empty `mxc` clears the
/// override. Validated as an `mxc:` URI first, since every member of the
/// room reads it.
pub(crate) fn set_room_avatar(
    bridge: &RustClient,
    room_id: String,
    source: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    // Three shapes: empty clears, an `mxc:` is used as-is, anything else is a
    // local file to upload. The global avatar path's size and regular-file
    // checks run before the file is read.
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
            // Upload first for a local file. Not `Account::upload_avatar`, which would
            // also change the global avatar.
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
            // Stripped state is an invite preview, not an editable membership.
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
            // Only a `join` membership carries a profile to edit.
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
            "e\u{0301}\u{0327}", // e + combining acute + combining cedilla
            "Ω mixed Ωmega 漢字 🦊",
        ] {
            assert_eq!(bound_display_name(name), name, "mangled {name:?}");
        }
    }

    #[test]
    fn bound_cuts_at_a_scalar_boundary_never_inside_one() {
        // 254 ASCII characters then one astral emoji: the emoji is the 255th
        // character and survives whole (a UTF-16 or byte bound would split it).
        let name = format!("{}{}", "a".repeat(254), '\u{1F98A}');
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded, name);
        assert!(bounded.ends_with('\u{1F98A}'));
        // The astral scalar came through as one char.
        assert_eq!(bounded.chars().last(), Some('\u{1F98A}'));

        // One character past the bound, the emoji is dropped entirely.
        let over = format!("{}{}", "a".repeat(255), '\u{1F98A}');
        let bounded_over = bound_display_name(&over);
        assert_eq!(bounded_over.chars().count(), 255);
        assert!(!bounded_over.contains('\u{1F98A}'));
        assert_eq!(bounded_over, "a".repeat(255));
    }

    #[test]
    fn bound_truncates_long_non_latin_names_at_255_scalars() {
        let name = "\u{65E5}".repeat(300); // 300 × U+65E5, 3 bytes each
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded.len(), 255 * 3); // no partial 3-byte sequence
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


/// Upload and set the account's own avatar via `Account::upload_avatar`,
/// which uploads and writes `avatar_url`.
///
/// The MIME is sniffed from the bytes, never the file name, with the same
/// `sniff_image_mime` as the room-avatar path (which refuses SVG). The path
/// is never logged: it contains the user's home directory.
pub(crate) fn set_own_avatar(
    bridge: &RustClient,
    local_path: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    // Checked before reading, so an absurd file never reaches memory.
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
        // A completion that outlived its session must not be reported.
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

/// Clear the account's avatar (`None` is the SDK's "remove").
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


/// Mutual rooms reported; a profile card lists a handful.
const MAX_MUTUAL_ROOMS: usize = 24;

/// Rooms this account and `user_id` are both joined to, from the store only
/// (`get_member_no_sync` never issues a request). Asking the server would
/// cost a `/state` per idle room each time a profile card opens, so rooms
/// whose members were never synced are omitted: under-reporting is the
/// better failure. DMs are included and marked.
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
            // `Ok(None)` (not a member as far as the store knows) and `Err` are both
            // skipped rather than guessed at.
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
