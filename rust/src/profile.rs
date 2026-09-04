//! The signed-in account's OWN profile (v0.7.4).
//!
//! Display-name writes are `Account::set_display_name`, the SDK's own wrapper
//! over the profile endpoints (`PUT .../profile/{user}/displayname`, or the
//! newer delete-profile-field request when the server advertises it and the
//! name is being cleared). Lightning implements no profile request of its own
//! here; this module contributes exactly three things the SDK does not: a
//! length bound, the session-generation guard every async command in this
//! bridge carries, and a sanitized result payload.
//!
//! The name itself is text the user typed. It is never logged, and it never
//! comes BACK across the FFI on the result: C++ already holds the string it
//! submitted, so echoing it would only widen the surface that can leak it.

use std::sync::Arc;

use matrix_sdk::ruma::api::error::ErrorBody;
use matrix_sdk::ruma::RoomId;
use serde_json::json;

use crate::rooms::{require_client, sniff_image_mime, MAX_AVATAR_BYTES};
use crate::{enqueue, RustClient};

/// The profile write rides the room-action pool, which sign-out joins.
/// Bounded so a hung request degrades to a reported failure rather than
/// stalling the account teardown behind it.
const DISPLAY_NAME_REQUEST_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(15);

/// Matrix does not specify a maximum display-name length and servers differ,
/// so 255 is a client-side ceiling, not a protocol one.
const DISPLAY_NAME_MAX_CHARS: usize = 255;

/// Bound a display name at [`DISPLAY_NAME_MAX_CHARS`] Unicode scalar values.
///
/// Deliberately `chars()`, never a byte slice: `&name[..255]` panics on a
/// multi-byte boundary and, where it does not, hands the server a string cut
/// through the middle of a UTF-8 sequence. Scalar values are also the right
/// unit for the C++ side to reason about — a UTF-16 code-unit bound would cut
/// an emoji in half between its surrogates.
///
/// This can still split a grapheme CLUSTER (a base character from its
/// combining marks, or a ZWJ sequence at the joiner). That is accepted and
/// stated rather than fixed with a segmentation crate: no such crate is in
/// this offline `--locked` build's dependency set, the result is always valid
/// UTF-8 and always a valid Matrix display name, and a 255-character name is
/// already far past anything a server or a UI will render whole.
pub(crate) fn bound_display_name(name: &str) -> String {
    name.chars().take(DISPLAY_NAME_MAX_CHARS).collect()
}

/// Collapse control characters and whitespace runs, then bound the length.
///
/// The message is server-authored, but it lands in a UI label, so it is
/// treated like any other remote string: no newlines, no control bytes, no
/// unbounded length.
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

/// The server's own human-readable sentence for a refusal, if it sent one.
///
/// Only `StandardErrorBody.message` is taken — the one field the spec defines
/// as prose meant for a person. The error's `Display` is deliberately NOT a
/// fallback: it can carry the request URL, and a URL is not an error message.
/// An empty return means "the server said nothing usable", and the
/// presentation layer supplies its own translated wording for that.
fn server_error_message(err: &matrix_sdk::Error) -> String {
    let Some(api) = err.as_client_api_error() else {
        return String::new();
    };
    let ErrorBody::Standard(body) = &api.body else {
        return String::new();
    };
    sanitize_error_message(&body.message)
}

/// Set — or CLEAR — the signed-in account's display name.
///
/// An empty `name` means CLEAR. `set_display_name` takes `Option<&str>` and
/// `Some("")` is NOT the same request: it asks the server to STORE an empty
/// name rather than to remove the field. The UI makes clearing a separate,
/// explicit action for the same reason, so an empty editor can never arrive
/// here by accident.
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
        // A completion that outlived its session must never be reported:
        // the next account's UI would take it as ITS answer.
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(())) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            // A timeout has no server body at all; the empty string is the
            // honest answer and C++ words it.
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
// A standard Matrix per-room member profile: the display name and avatar
// this account shows IN ONE ROOM, overriding the global one. Both live in
// that room's own `m.room.member` state event for this user.
//
// THE DANGEROUS PART IS EVERYTHING ELSE IN THAT EVENT. `RoomMemberEventContent`
// serialises every optional field with `skip_serializing_if`, so any field
// not copied forward is DELETED from the room's state — and one of them,
// `join_authorised_via_users_server`, is what makes a restricted-room
// membership valid. Dropping it can invalidate the membership.
//
// So the avatar path reads the RAW member event and edits only the one key,
// rather than deserialising into the typed content: the typed struct has no
// `#[serde(flatten)]` catch-all, so any field a future spec version or
// another client wrote would be silently lost on the round trip. The SDK's
// own `set_own_member_display_name` has that same defect, and additionally
// flattens a redacted event to `new(membership)` — discarding `reason`,
// `is_direct`, `third_party_invite` and the restricted-room authorisation.
// The name path still uses it, because for the name it is the supported API
// and the redacted case cannot arise for a joined member editing themselves.

/// Set or clear (empty) this account's display name IN ONE ROOM.
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

/// The member-event keys a per-room profile edit may touch. Everything else
/// in the event is carried forward verbatim.
const PROFILE_KEYS: [&str; 2] = ["displayname", "avatar_url"];

/// Set or clear this account's avatar IN ONE ROOM.
///
/// `mxc` empty clears the override. The value is validated as an `mxc:` URI
/// before it is written: it is going into room state, where every member
/// reads it, and a client that trusted it would fetch whatever it named.
pub(crate) fn set_room_avatar(
    bridge: &RustClient,
    room_id: String,
    source: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    // Three shapes, decided here so the async body has one job: empty
    // CLEARS the override, an `mxc:` is used as-is, and anything else is a
    // local file to upload first. The same size/regular-file checks the
    // global avatar path applies happen BEFORE the file is read, so an
    // absurd file is refused without being pulled into memory.
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
            // Upload first when the caller handed over a local file. This is
            // deliberately NOT `Account::upload_avatar`, which uploads AND
            // writes the GLOBAL avatar_url — the whole point here is that the
            // global profile is left alone.
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
            // READ THE RAW EVENT, not the typed content. See the note above:
            // the typed round trip drops anything it does not know about.
            let raw = room
                .get_state_event(
                    matrix_sdk::ruma::events::StateEventType::RoomMember,
                    user_id.as_str(),
                )
                .await
                .map_err(|e| server_error_message(&e))?
                .ok_or_else(|| "no membership to edit".to_owned())?;
            // A STRIPPED state event is an invite's preview, not a
            // membership this account can edit — refuse rather than write a
            // profile onto something that is not joined state.
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
            // Editing a membership that is not `join` would be writing a
            // profile onto a leave/ban event, which is not a profile change.
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
        // 254 ASCII characters then one astral emoji: the 255th CHARACTER is
        // the emoji, so it survives whole. A UTF-16 code-unit bound of 255
        // would have cut it between its surrogates, and a byte bound would
        // have cut it after one of its four UTF-8 bytes.
        let name = format!("{}{}", "a".repeat(254), '\u{1F98A}');
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded, name);
        assert!(bounded.ends_with('\u{1F98A}'));
        // std::str is UTF-8 by construction, so the real proof that nothing
        // was split is that the astral scalar came through as ONE char.
        assert_eq!(bounded.chars().last(), Some('\u{1F98A}'));

        // Push the same emoji one character past the bound: it must be
        // dropped ENTIRELY, never half-emitted.
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


/// Upload and set the signed-in account's OWN avatar.
///
/// `Account::upload_avatar` both uploads the media and writes `avatar_url`,
/// which is why nothing here touches the profile endpoint directly — the same
/// reason this module implements no display-name request of its own.
///
/// The MIME is SNIFFED from the bytes, never taken from the file name: the
/// path arrives from a file picker and an extension is a claim, not evidence.
/// `sniff_image_mime` is shared with the room-avatar path so the two cannot
/// accept different sets of formats, and it refuses SVG.
///
/// The path is never logged. It is a user's own filesystem, and a home
/// directory carries their name.
pub(crate) fn set_own_avatar(
    bridge: &RustClient,
    local_path: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    // Checked BEFORE the file is read, so an absurd file is refused without
    // being pulled into memory first.
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
        // A completion that outlived its session must never be reported.
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

/// Clear the account's avatar. `None` is the SDK's "remove it" argument.
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


/// How many mutual rooms to report. A profile card lists a handful; a user
/// sharing hundreds of rooms would otherwise build a menu nobody can use.
const MAX_MUTUAL_ROOMS: usize = 24;

/// Rooms this account and `user_id` are BOTH joined to.
///
/// Reads ONLY what the store already holds: `get_member_no_sync` never issues
/// a request, which is the whole reason it is used here. CLAUDE.md's standing
/// rule is that a profile/room-list surface must not be allowed to ask —
/// `read_membership_events` falls back to a full `/state` for any room whose
/// membership is not cached, which is the normal state of every idle room, so
/// the obvious implementation would issue one `/state` PER ROOM every time a
/// profile card opened.
///
/// The honest cost of that choice: a room the client has not synced members
/// for is not listed. Under-reporting is the right failure here — a card that
/// silently costs a request per room is worse than one that lists fewer rooms.
///
/// DMs are included and marked, so the caller can present them apart from
/// ordinary rooms the way Sable does.
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
            // A cached read. `Ok(None)` means "not a member as far as the
            // store knows"; an Err means the store could not answer, and
            // both are skipped rather than guessed at.
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
