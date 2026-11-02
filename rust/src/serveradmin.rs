
use std::sync::Arc;

use matrix_sdk::ruma::{
    api::client::config::{get_global_account_data, set_global_account_data},
    events::{macros::EventContent, GlobalAccountDataEvent, GlobalAccountDataEventType},
    serde::Raw,
    RoomId,
};
use matrix_sdk::{event_handler::EventHandlerHandle, Client, RoomState};
use serde::{Deserialize, Serialize};
use matrix_sdk_base::RoomInfoNotableUpdateReasons;
use serde_json::json;

use crate::rooms::require_client;
use crate::{enqueue, RustClient};

const ADMIN_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
const ADMIN_DELETE_POLL: std::time::Duration = std::time::Duration::from_secs(2);
const ADMIN_DELETE_FOLLOW: std::time::Duration = std::time::Duration::from_secs(600);
const ADMIN_BODY_CAP: usize = 65_536;

pub(crate) async fn mark_deleted_room_left(
    room: &matrix_sdk::BaseRoom,
) -> Result<(), matrix_sdk::StoreError> {
    if matches!(room.state(), RoomState::Left | RoomState::Banned) {
        return Ok(());
    }
    room.update_and_save_room_info(|mut info| {
        info.mark_as_left();
        info.mark_state_partially_synced();
        info.mark_members_missing();
        (info, RoomInfoNotableUpdateReasons::MEMBERSHIP)
    })
    .await
}

pub(crate) const DELETED_ROOMS_TYPE: &str = "org.lightning_matrix.deleted_rooms";
const DELETED_ROOMS_CAP: usize = 100;

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.deleted_rooms", kind = GlobalAccountData)]
pub(crate) struct DeletedRoomsEventContent {
    #[serde(default)]
    pub rooms: Vec<String>,
}

pub(crate) fn merge_deleted_rooms(existing: &[String], room_id: &str) -> Vec<String> {
    let mut out: Vec<String> = existing.iter().filter(|r| r.as_str() != room_id).cloned().collect();
    out.push(room_id.to_owned());
    if out.len() > DELETED_ROOMS_CAP {
        out.drain(..out.len() - DELETED_ROOMS_CAP);
    }
    out
}

async fn record_deleted_room(client: &Client, room_id: &str) {
    let Some(own) = client.user_id().map(ToOwned::to_owned) else { return };
    let get = get_global_account_data::v3::Request::new(
        own.clone(),
        GlobalAccountDataEventType::from(DELETED_ROOMS_TYPE),
    );
    let existing: Vec<String> = match client.send(get).with_request_config(account_data_config()).await {
        Ok(r) => serde_json::from_str::<DeletedRoomsEventContent>(r.account_data.json().get())
            .map(|c| c.rooms)
            .unwrap_or_default(),
        Err(err) if err.as_client_api_error().is_some_and(|e| e.status_code.as_u16() == 404) => {
            Vec::new()
        }
        Err(_) => return,
    };
    let content = DeletedRoomsEventContent { rooms: merge_deleted_rooms(&existing, room_id) };
    let Ok(raw) = Raw::new(&content) else { return };
    let request = set_global_account_data::v3::Request::new_raw(
        own,
        GlobalAccountDataEventType::from(DELETED_ROOMS_TYPE),
        raw.cast_unchecked(),
    );
    let _ = client.send(request).with_request_config(account_data_config()).await;
}

fn account_data_config() -> matrix_sdk::config::RequestConfig {
    matrix_sdk::config::RequestConfig::new().disable_retry().timeout(ADMIN_REQUEST_TIMEOUT)
}

fn held_listed(
    get_room: &impl Fn(&RoomId) -> Option<matrix_sdk::BaseRoom>,
    listed: &[String],
) -> Vec<matrix_sdk::BaseRoom> {
    listed
        .iter()
        .filter_map(|id| RoomId::parse(id).ok())
        .filter_map(|rid| get_room(&rid))
        .filter(|room| room.state() == RoomState::Joined)
        .collect()
}

/// Mark every held listed room left UNLESS the server still lists it in
/// `GET /joined_rooms`. The list is never cleared (it is capped, not pruned),
/// and a room deleted without `block` can be joined again under the same id:
/// without this check that room would be hidden again on every delivery, on
/// every session.
async fn apply_deleted_rooms(
    held: &[matrix_sdk::BaseRoom],
    server_joined: &std::collections::HashSet<String>,
) -> bool {
    let mut changed = false;
    for room in held {
        if server_joined.contains(room.room_id().as_str()) {
            continue;
        }
        if mark_deleted_room_left(room).await.is_ok() {
            changed = true;
        }
    }
    changed
}

async fn server_joined_rooms(client: &Client) -> Option<std::collections::HashSet<String>> {
    let response = client
        .send(matrix_sdk::ruma::api::client::membership::joined_rooms::v3::Request::new())
        .with_request_config(account_data_config())
        .await
        .ok()?;
    Some(response.joined_rooms.iter().map(ToString::to_string).collect())
}

pub(crate) fn install_deleted_room_handler(
    client: &Client,
    events: crate::EventQueueRef,
) -> EventHandlerHandle {
    client.add_event_handler(
        move |ev: GlobalAccountDataEvent<DeletedRoomsEventContent>, client: Client| {
            let events = Arc::clone(&events);
            let listed = ev.content.rooms.clone();
            async move {
                let held = held_listed(&|id| client.get_room(id).map(|r| (*r).clone()), &listed);
                if held.is_empty() {
                    return;
                }
                // SDK event handlers run inside sync processing: a network
                // round trip here would stall the sync loop, so hand it off.
                // The request is bounded (ADMIN_REQUEST_TIMEOUT). `is_active()`
                // is a best-effort check, not a lifecycle token: the work only
                // marks rooms left in the store this client holds.
                tokio::spawn(async move {
                    let Some(server_joined) = server_joined_rooms(&client).await else { return };
                    if !client.is_active() {
                        return;
                    }
                    if apply_deleted_rooms(&held, &server_joined).await {
                        crate::enqueue_rooms(&events, &client).await;
                    }
                });
            }
        },
    )
}

/// `base` plus `segments`, each percent-encoded as one path segment, so an id
/// cannot add or escape a segment. The url crate drops a "." or ".." segment
/// rather than encode it, so those are refused. Pure and unit-tested.
fn admin_url(base: &url::Url, segments: &[&str]) -> Result<String, String> {
    if segments.iter().any(|s| s.is_empty() || *s == "." || *s == "..") {
        return Err("invalid path segment".to_owned());
    }
    let mut url = base.clone();
    url.set_query(None);
    url.set_fragment(None);
    url.path_segments_mut()
        .map_err(|_| "homeserver url cannot carry a path".to_owned())?
        .pop_if_empty()
        .extend(segments);
    Ok(url.to_string())
}

fn admin_answer(status: u16, body: &str) -> (bool, &'static str) {
    if status == 200 {
        let admin = serde_json::from_str::<serde_json::Value>(body)
            .ok()
            .and_then(|v| v.get("admin").and_then(|a| a.as_bool()))
            == Some(true);
        return if admin { (true, "") } else { (false, "not_admin") };
    }
    if status == 403 {
        return (false, "not_admin");
    }
    (false, "unavailable")
}

fn delete_started(status: u16, body: Option<&str>) -> Result<String, &'static str> {
    match status {
        200 => body
            .and_then(|body| serde_json::from_str::<serde_json::Value>(body).ok())
            .and_then(|v| v.get("delete_id").and_then(|d| d.as_str()).map(str::to_owned))
            .filter(|id| {
                !id.is_empty()
                    && id.len() <= 255
                    && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
            })
            .ok_or("unknown"),
        401 => Err("unauthorized"),
        403 => Err("forbidden"),
        400 => Err("invalid"),
        404 | 405 => Err("unrecognized"),
        429 => Err("rate_limited"),
        502..=504 => Err("unknown"),
        _ => Err("network"),
    }
}

fn delete_status(body: &str) -> Option<(&'static str, usize, usize)> {
    let v: serde_json::Value = serde_json::from_str(body).ok()?;
    let status = match v.get("status")?.as_str()? {
        "scheduled" => "scheduled",
        "active" => "active",
        "complete" => "complete",
        "failed" => "failed",
        _ => return None,
    };
    let count = |key: &str| {
        v.get("shutdown_room")
            .and_then(|s| s.get(key))
            .and_then(|a| a.as_array())
            .map(|a| a.len())
            .unwrap_or(0)
    };
    Some((status, count("kicked_users"), count("failed_to_kick_users")))
}

fn transport_category(connect: bool) -> &'static str {
    if connect { "network" } else { "unknown" }
}

async fn send(
    client: &matrix_sdk::Client,
    request: reqwest::RequestBuilder,
) -> Result<(u16, Option<String>), &'static str> {
    let token = client.access_token().ok_or("network")?;
    let response = request
        .header(reqwest::header::AUTHORIZATION, format!("Bearer {token}"))
        .send()
        .await
        .map_err(|err| transport_category(err.is_connect()))?;
    let status = response.status().as_u16();
    let mut response = response;
    let mut bytes: Vec<u8> = Vec::new();
    loop {
        match response.chunk().await {
            Ok(Some(chunk)) => {
                if bytes.len() + chunk.len() > ADMIN_BODY_CAP {
                    return Ok((status, None));
                }
                bytes.extend_from_slice(&chunk);
            }
            Ok(None) => break,
            Err(_) => return Ok((status, None)),
        }
    }
    Ok((status, Some(String::from_utf8_lossy(&bytes).into_owned())))
}

pub(crate) fn server_admin_status(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let own = client
        .user_id()
        .map(|u| u.to_string())
        .ok_or_else(|| "no active Matrix session".to_owned())?;
    let url = admin_url(
        &client.homeserver(),
        &["_synapse", "admin", "v1", "users", own.as_str(), "admin"],
    )?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = client.http_client().get(url).timeout(ADMIN_REQUEST_TIMEOUT);
        let (admin, detail) = match send(&client, request).await {
            Ok((200, None)) => (false, "unavailable"),
            Ok((status, body)) => admin_answer(status, body.as_deref().unwrap_or("")),
            Err(_) => (false, "unavailable"),
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "server_admin_status",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "admin": admin,
            "detail": detail,
        }));
    });
    Ok(())
}

pub(crate) fn admin_delete_room(
    bridge: &RustClient,
    room_id: String,
    block: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
    let base = client.homeserver();
    let delete_url = admin_url(&base, &["_synapse", "admin", "v2", "rooms", parsed.as_str()])?;
    let body = serde_json::to_vec(&json!({ "block": block, "purge": true }))
        .map_err(|_| "invalid request".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = |status: &str, ok: bool, removed: usize, failed: usize, category: &str| {
            json!({
                "type": "admin_room_delete_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "status": status,
                "ok": ok,
                "removed": removed,
                "failed_to_remove": failed,
                "category": category,
            })
        };
        let request = client
            .http_client()
            .delete(delete_url)
            .timeout(ADMIN_REQUEST_TIMEOUT)
            .header(reqwest::header::CONTENT_TYPE, "application/json")
            .body(body);
        let started = match send(&client, request).await {
            Ok((status, body)) => delete_started(status, body.as_deref()),
            Err(category) => Err(category),
        };
        let delete_id = match started {
            Ok(id) => id,
            Err(category) => {
                // No answer to the DELETE itself: whether the server started
                // it is not known, and must not read as "failed".
                let status = if category == "unknown" { "unknown" } else { "failed" };
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, result(status, false, 0, 0, category));
                }
                return;
            }
        };
        let Ok(status_url) = admin_url(
            &base,
            &["_synapse", "admin", "v2", "rooms", "delete_status", delete_id.as_str()],
        ) else {
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, result("following", false, 0, 0, ""));
            }
            return;
        };
        let until = tokio::time::Instant::now() + ADMIN_DELETE_FOLLOW;
        let mut last = "";
        let mut heard = false;
        loop {
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let request = client
                .http_client()
                .get(status_url.clone())
                .timeout(ADMIN_REQUEST_TIMEOUT);
            let answer = match send(&client, request).await {
                Ok((200, Some(body))) => delete_status(&body),
                _ => None,
            };
            if let Some((status, removed, failed)) = answer {
                heard = true;
                if status != last && timelines.lifecycle_current(lifecycle) {
                    last = status;
                    enqueue(&events, json!({
                        "type": "admin_room_delete_progress",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "room_id": room_id,
                        "status": status,
                    }));
                }
                if status == "complete" || status == "failed" {
                    if timelines.lifecycle_current(lifecycle) {
                        if status == "complete" {
                            if let Some(room) = RoomId::parse(&room_id)
                                .ok()
                                .and_then(|id| client.get_room(&id))
                            {
                                let _ = mark_deleted_room_left(&room).await;
                            }
                            record_deleted_room(&client, &room_id).await;
                        }
                        let category = if status == "failed" { "server" } else { "" };
                        enqueue(
                            &events,
                            result(status, status == "complete", removed, failed, category),
                        );
                        crate::enqueue_rooms(&events, &client).await;
                    }
                    return;
                }
            }
            if tokio::time::Instant::now() >= until {
                if timelines.lifecycle_current(lifecycle) {
                    let outcome = if heard {
                        result("following", false, 0, 0, "")
                    } else {
                        result("unknown", false, 0, 0, "unknown")
                    };
                    enqueue(&events, outcome);
                }
                return;
            }
            tokio::time::sleep(ADMIN_DELETE_POLL).await;
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn deleted_rooms_merge_dedups_caps_and_keeps_newest_last() {
        let a = vec!["!a:x".to_owned(), "!b:x".to_owned()];
        assert_eq!(merge_deleted_rooms(&a, "!c:x"), ["!a:x", "!b:x", "!c:x"]);
        assert_eq!(merge_deleted_rooms(&a, "!a:x"), ["!b:x", "!a:x"]);
        let many: Vec<String> = (0..DELETED_ROOMS_CAP).map(|i| format!("!r{i}:x")).collect();
        let out = merge_deleted_rooms(&many, "!new:x");
        assert_eq!(out.len(), DELETED_ROOMS_CAP);
        assert_eq!(out.first().map(String::as_str), Some("!r1:x"));
        assert_eq!(out.last().map(String::as_str), Some("!new:x"));
    }

    use super::*;

    #[test]
    fn admin_urls_keep_each_id_in_one_segment() {
        let base = url::Url::parse("https://matrix.example.org/").unwrap();
        assert_eq!(
            admin_url(&base, &["_synapse", "admin", "v2", "rooms", "!abc:example.org"]).unwrap(),
            "https://matrix.example.org/_synapse/admin/v2/rooms/!abc:example.org"
        );
        let hostile =
            admin_url(&base, &["_synapse", "admin", "v2", "rooms", "!a/../../x?y#z"]).unwrap();
        assert!(hostile.starts_with("https://matrix.example.org/_synapse/admin/v2/rooms/"));
        assert!(!hostile.contains("/../"));
        assert!(!hostile.contains('?'));
        assert!(!hostile.contains('#'));
        assert!(admin_url(&base, &["_synapse", ".."]).is_err());
        assert!(admin_url(&base, &["_synapse", "."]).is_err());
        assert!(admin_url(&base, &["_synapse", ""]).is_err());
        let prefixed = url::Url::parse("https://example.org/matrix/").unwrap();
        assert_eq!(
            admin_url(&prefixed, &["_synapse", "admin"]).unwrap(),
            "https://example.org/matrix/_synapse/admin"
        );
    }

    #[test]
    fn only_an_explicit_true_is_an_administrator() {
        assert_eq!(admin_answer(200, r#"{"admin": true}"#), (true, ""));
        assert_eq!(admin_answer(200, r#"{"admin": false}"#), (false, "not_admin"));
        assert_eq!(admin_answer(200, r#"{"admin": "true"}"#), (false, "not_admin"));
        assert_eq!(admin_answer(200, r#"{"admin": 1}"#), (false, "not_admin"));
        assert_eq!(admin_answer(200, "not json"), (false, "not_admin"));
        assert_eq!(
            admin_answer(403, r#"{"errcode":"M_FORBIDDEN","error":"You are not a server admin"}"#),
            (false, "not_admin")
        );
        assert_eq!(admin_answer(404, ""), (false, "unavailable"));
        assert_eq!(admin_answer(400, ""), (false, "unavailable"));
        assert_eq!(admin_answer(500, r#"{"admin": true}"#), (false, "unavailable"));
    }

    #[test]
    fn only_a_refused_connection_is_a_plain_network_failure() {
        assert_eq!(transport_category(true), "network");
        assert_eq!(transport_category(false), "unknown");
    }

    #[test]
    fn a_delete_starts_only_with_a_delete_id() {
        assert_eq!(delete_started(200, Some(r#"{"delete_id":"abc"}"#)), Ok("abc".to_owned()));
        assert_eq!(delete_started(200, Some(r#"{"delete_id":""}"#)), Err("unknown"));
        assert_eq!(delete_started(200, Some("{}")), Err("unknown"));
        assert_eq!(delete_started(200, Some(r#"{"delete_id":".."}"#)), Err("unknown"));
        assert_eq!(delete_started(200, Some(r#"{"delete_id":"a/b"}"#)), Err("unknown"));
        assert_eq!(delete_started(200, None), Err("unknown"));
        assert_eq!(delete_started(400, None), Err("invalid"));
        assert_eq!(delete_started(502, None), Err("unknown"));
        assert_eq!(delete_started(503, Some("")), Err("unknown"));
        assert_eq!(delete_started(504, None), Err("unknown"));
        assert_eq!(delete_started(500, None), Err("network"));
        assert_eq!(delete_started(401, Some("")), Err("unauthorized"));
        assert_eq!(delete_started(403, Some("")), Err("forbidden"));
        assert_eq!(delete_started(404, Some("")), Err("unrecognized"));
        assert_eq!(delete_started(429, Some("")), Err("rate_limited"));
    }

    #[tokio::test]
    async fn a_deleted_room_is_left_in_the_local_store() {
        use matrix_sdk::cross_process_lock::CrossProcessLockConfig;
        use matrix_sdk::ruma::UserId;
        use matrix_sdk_base::store::{RoomLoadSettings, StoreConfig};
        use matrix_sdk_base::{
            BaseClient, DmRoomDefinition, RoomStateFilter, SessionMeta, StateStore,
            ThreadingSupport,
        };

        let client = BaseClient::new(
            StoreConfig::new(CrossProcessLockConfig::multi_process("lightning-test")),
            ThreadingSupport::Disabled,
            DmRoomDefinition::default(),
        );
        client
            .activate(
                SessionMeta {
                    user_id: UserId::parse("@admin:example.org").unwrap(),
                    device_id: "DEVICE".into(),
                },
                RoomLoadSettings::default(),
                None,
            )
            .await
            .expect("activate");
        let gone = RoomId::parse("!gone:example.org").unwrap();
        let kept = RoomId::parse("!kept:example.org").unwrap();
        let room = client.get_or_create_room(&gone, RoomState::Joined);
        client.get_or_create_room(&kept, RoomState::Joined);
        assert_eq!(client.rooms_filtered(RoomStateFilter::JOINED).len(), 2);

        mark_deleted_room_left(&room).await.expect("marked left");

        assert_eq!(room.state(), RoomState::Left);
        let joined: Vec<_> = client
            .rooms_filtered(RoomStateFilter::JOINED)
            .iter()
            .map(|r| r.room_id().to_owned())
            .collect();
        assert_eq!(joined, vec![kept.clone()]);
        let stored = client
            .state_store()
            .get_room_infos(&RoomLoadSettings::default())
            .await
            .expect("room infos");
        let info = stored
            .iter()
            .find(|i| i.room_id() == &*gone)
            .expect("stored");
        assert_eq!(info.state(), RoomState::Left);
        mark_deleted_room_left(&room).await.expect("again");
        assert_eq!(room.state(), RoomState::Left);

        let other = RoomId::parse("!other:example.org").unwrap();
        let other_room = client.get_or_create_room(&other, RoomState::Joined);
        let back = RoomId::parse("!back:example.org").unwrap();
        let back_room = client.get_or_create_room(&back, RoomState::Joined);
        let listed = vec![
            "!other:example.org".to_owned(),
            "!back:example.org".to_owned(),
            "!nope:example.org".to_owned(),
            "junk".to_owned(),
        ];
        let held = held_listed(&|id| client.get_room(id), &listed);
        assert_eq!(held.len(), 2);
        let server_joined: std::collections::HashSet<String> =
            ["!back:example.org", "!kept:example.org"].iter().map(|s| (*s).to_owned()).collect();
        assert!(apply_deleted_rooms(&held, &server_joined).await);
        assert_eq!(other_room.state(), RoomState::Left);
        assert_eq!(back_room.state(), RoomState::Joined);
        assert_eq!(client.get_room(&kept).unwrap().state(), RoomState::Joined);
        let held = held_listed(&|id| client.get_room(id), &listed);
        assert_eq!(held.len(), 1);
        assert!(!apply_deleted_rooms(&held, &server_joined).await);
    }

    #[test]
    fn delete_status_reads_synapses_four_states() {
        let complete = r#"{"status":"complete","delete_id":"x","room_id":"!r:s",
            "shutdown_room":{"kicked_users":["@a:s","@b:s"],"failed_to_kick_users":["@c:s"],
            "local_aliases":[],"new_room_id":null}}"#;
        assert_eq!(delete_status(complete), Some(("complete", 2, 1)));
        assert_eq!(delete_status(r#"{"status":"active"}"#), Some(("active", 0, 0)));
        assert_eq!(delete_status(r#"{"status":"scheduled"}"#), Some(("scheduled", 0, 0)));
        assert_eq!(delete_status(r#"{"status":"failed","error":"x"}"#), Some(("failed", 0, 0)));
        assert_eq!(delete_status(r#"{"status":"purging"}"#), None);
        assert_eq!(delete_status("[]"), None);
    }
}
