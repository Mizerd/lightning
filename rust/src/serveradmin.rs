//! Deleting a room as a homeserver administrator (Synapse's admin API).
//!
//! Matrix has no client-server API for deleting a room. A Synapse server
//! administrator can: `DELETE /_synapse/admin/v2/rooms/{roomId}` removes every
//! member on that server, purges the server's copy of the room and can block
//! it from being joined again. Members on other servers keep the room and
//! their copy of its history.
//!
//! Only reached when the user opens a delete surface, never in the
//! background. Whether the account is an administrator is asked of the server
//! (`/_synapse/admin/v1/users/{self}/admin`), which refuses non-admins; any
//! answer but an explicit `{"admin": true}` reads as "not an administrator".
//! Deployments that hide `/_synapse/admin` behind their proxy (Synapse's own
//! advice) or use MAS answer an error, so the delete is simply not offered.
//!
//! The access token goes to the same homeserver as every other request, over
//! the SDK's own transport, and is never logged or stored.

use std::sync::Arc;

use matrix_sdk::ruma::RoomId;
use serde_json::json;

use crate::rooms::require_client;
use crate::{enqueue, RustClient};

/// One admin API round trip.
const ADMIN_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
/// Between delete-status reads.
const ADMIN_DELETE_POLL: std::time::Duration = std::time::Duration::from_secs(2);
/// How long to follow one delete. Past it the server keeps going and the UI
/// says the outcome is not known yet.
const ADMIN_DELETE_FOLLOW: std::time::Duration = std::time::Duration::from_secs(600);
/// Admin API bodies are small; anything larger is cut.
const ADMIN_BODY_CAP: usize = 65_536;

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

/// Reads the answer to "is this account a server administrator?":
/// `(admin, detail)`. Only a 200 with `"admin": true` is yes; 403 is the
/// server saying no; anything else means the API is not reachable here.
/// Pure and unit-tested.
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

/// The delete id from the answer to a delete request, or a coarse category.
/// `body` is None when it could not be read. A 200 means Synapse accepted
/// and scheduled the purge, so a 200 without a usable id is "unknown" (it
/// started, but cannot be followed), never a refusal; so is a 502/503/504,
/// which a proxy in front of Synapse answers for a request that may have
/// reached it. Pure and unit-tested.
fn delete_started(status: u16, body: Option<&str>) -> Result<String, &'static str> {
    match status {
        200 => body
            .and_then(|body| serde_json::from_str::<serde_json::Value>(body).ok())
            .and_then(|v| v.get("delete_id").and_then(|d| d.as_str()).map(str::to_owned))
            // Synapse's ids are random letters; anything else is not used
            // in a URL.
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

/// One delete-status answer: `(status, removed, failed_to_remove)`. The
/// status is one of Synapse's four words; anything else is `None`. Pure and
/// unit-tested.
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

/// Why a request produced no answer: "network" when it never reached the
/// server (no connection), "unknown" when it may have (a timeout or a lost
/// response), so the outcome cannot be known.
fn transport_category(connect: bool) -> &'static str {
    if connect { "network" } else { "unknown" }
}

/// The status and body of one admin request. The body is None when it could
/// not be read whole or is longer than ADMIN_BODY_CAP (not an answer Synapse
/// gives); the status still counts.
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

// Whether the signed-in account is a server administrator. Result event:
// server_admin_status { op_id, admin, detail } where detail is "",
// "not_admin" or "unavailable".
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
            // An unreadable 200 is asked again later, not believed.
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

// Delete `room_id` from this homeserver through the admin API: every local
// member is removed and the server's copy is purged; with `block`, local
// users cannot join it again. Follows the delete until it completes, fails or
// ADMIN_DELETE_FOLLOW passes. Result events: admin_room_delete_progress
// { op_id, room_id, status } on each change, then admin_room_delete_result
// { op_id, room_id, status, ok, removed, failed_to_remove, category } where
// status "following" means the server had not finished when we stopped
// following it, and "unknown" that Lightning cannot say what happened: the
// delete request got no answer (or a gateway error), the server accepted it
// without a usable id, or no status read ever answered.
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
    // Built with serde so nothing can inject a field. purge is Synapse's
    // default, stated so the request says what it does.
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
            // Unreachable for a validated id; still never silent.
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, result("following", false, 0, 0, ""));
            }
            return;
        };
        let until = tokio::time::Instant::now() + ADMIN_DELETE_FOLLOW;
        let mut last = "";
        // Whether any status read answered: without one, "still deleting"
        // would be a guess.
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
    use super::*;

    // Every id is one percent-encoded path segment: nothing can climb out of
    // the admin path or add a query.
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
        // A homeserver behind a path prefix keeps it.
        let prefixed = url::Url::parse("https://example.org/matrix/").unwrap();
        assert_eq!(
            admin_url(&prefixed, &["_synapse", "admin"]).unwrap(),
            "https://example.org/matrix/_synapse/admin"
        );
    }

    // Only an explicit true is an administrator.
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
        // A proxy that hides the admin API, or a server without it.
        assert_eq!(admin_answer(404, ""), (false, "unavailable"));
        assert_eq!(admin_answer(400, ""), (false, "unavailable"));
        assert_eq!(admin_answer(500, r#"{"admin": true}"#), (false, "unavailable"));
    }

    // A request that may have reached the server is "unknown", not a failure.
    #[test]
    fn only_a_refused_connection_is_a_plain_network_failure() {
        assert_eq!(transport_category(true), "network");
        assert_eq!(transport_category(false), "unknown");
    }

    #[test]
    fn a_delete_starts_only_with_a_delete_id() {
        assert_eq!(delete_started(200, Some(r#"{"delete_id":"abc"}"#)), Ok("abc".to_owned()));
        // A 200 is an accepted delete: without a usable id it is unknown,
        // never "refused".
        assert_eq!(delete_started(200, Some(r#"{"delete_id":""}"#)), Err("unknown"));
        assert_eq!(delete_started(200, Some("{}")), Err("unknown"));
        assert_eq!(delete_started(200, Some(r#"{"delete_id":".."}"#)), Err("unknown"));
        assert_eq!(delete_started(200, Some(r#"{"delete_id":"a/b"}"#)), Err("unknown"));
        assert_eq!(delete_started(200, None), Err("unknown"));
        assert_eq!(delete_started(400, None), Err("invalid"));
        // A proxy's gateway answers may hide a delete that reached Synapse.
        assert_eq!(delete_started(502, None), Err("unknown"));
        assert_eq!(delete_started(503, Some("")), Err("unknown"));
        assert_eq!(delete_started(504, None), Err("unknown"));
        assert_eq!(delete_started(500, None), Err("network"));
        assert_eq!(delete_started(401, Some("")), Err("unauthorized"));
        assert_eq!(delete_started(403, Some("")), Err("forbidden"));
        assert_eq!(delete_started(404, Some("")), Err("unrecognized"));
        assert_eq!(delete_started(429, Some("")), Err("rate_limited"));
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
