//! Ignored users (`m.ignored_user_list`) and event reporting (v0.7.x).
//!
//! Both are SDK-owned Matrix mechanisms — never a Lightning-local database:
//! ignore/unignore are `Account::ignore_user`/`unignore_user` (an atomic
//! read-modify-write of the account-data event on the server), the list is
//! read back from that same account data, and reporting is
//! `Room::report_content` (the stable /v3 endpoint). Remote changes reach
//! C++ through the sync loop's subscriber (`ignored_users_changed`), so a
//! local ignore and one made in Element converge on the same path.
//!
//! Only Matrix user IDs cross the FFI here. A report carries exactly what
//! the API requires: room id, event id, and the user's optional reason.

use std::sync::Arc;

use matrix_sdk::ruma::{
    events::ignored_user_list::IgnoredUserListEventContent, EventId, UserId,
};
use serde_json::json;

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

/// Reports run on the room-action pool (joined during sign-out): bounded.
const REPORT_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// Ignore or unignore one user. Result event: `ignore_user_result
/// { op_id, lifecycle, user_id, ignored, ok, category }` where `ignored`
/// echoes the requested direction. Self-ignore is refused synchronously —
/// the SDK would refuse it too; failing fast keeps the UI honest.
pub(crate) fn set_user_ignored(
    bridge: &RustClient,
    user_id: String,
    ignored: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let target = UserId::parse(user_id.trim())
        .map_err(|_| "invalid user id".to_owned())?;
    if client.user_id() == Some(target.as_ref()) {
        return Err("you cannot ignore yourself".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = if ignored {
            client.account().ignore_user(&target).await
        } else {
            client.account().unignore_user(&target).await
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match result {
            Ok(()) => (true, ""),
            Err(err) => (false, classify_room_error(&err.to_string())),
        };
        enqueue(&events, json!({
            "type": "ignore_user_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "user_id": target.to_string(),
            "ignored": ignored,
            "ok": ok,
            "category": category,
        }));
    });
    Ok(())
}

/// Read the authoritative ignored-user list from account data. matrix-sdk
/// 0.18 has no `ignored_users()` accessor, so this reads the same content
/// its own helpers use. Result event: `ignored_users_list { op_id,
/// lifecycle, ok, users[] }`.
pub(crate) fn list_ignored_users(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let content = client
            .account()
            .account_data::<IgnoredUserListEventContent>()
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match content {
            Ok(raw) => {
                let users: Vec<String> = raw
                    .and_then(|c| c.deserialize().ok())
                    .map(|content| {
                        content
                            .ignored_users
                            .keys()
                            .map(ToString::to_string)
                            .collect()
                    })
                    .unwrap_or_default();
                enqueue(&events, json!({
                    "type": "ignored_users_list",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "users": users,
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "ignored_users_list",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                    "users": [],
                }));
            }
        }
    });
    Ok(())
}

/// Report one event to the homeserver's administrator
/// (`Room::report_content`, stable /v3; requires a joined room). Only the
/// event reference and the user's own reason are sent — no surrounding
/// context, no decrypted content. Result event: `report_message_result
/// { op_id, lifecycle, room_id, event_id, ok, category }`.
pub(crate) fn report_message(
    bridge: &RustClient,
    room_id: String,
    event_id: String,
    reason: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let event_id = EventId::parse(event_id.trim())
        .map_err(|_| "invalid event id".to_owned())?
        .to_owned();
    let reason = {
        let trimmed = reason.trim();
        if trimmed.is_empty() { None } else { Some(trimmed.to_owned()) }
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(
            REPORT_REQUEST_TIMEOUT,
            room.report_content(event_id.clone(), reason),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match result {
            Ok(Ok(_)) => (true, ""),
            Ok(Err(err)) => (false, classify_room_error(&err.to_string())),
            Err(_) => (false, "network"),
        };
        enqueue(&events, json!({
            "type": "report_message_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room.room_id().to_string(),
            "event_id": event_id.to_string(),
            "ok": ok,
            "category": category,
        }));
    });
    Ok(())
}
