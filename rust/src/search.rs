//! Server-side message search (v0.7.x): `POST /_matrix/client/v3/search`
//! through raw ruma via `Client::send` — matrix-sdk 0.18 has no high-level
//! wrapper for this endpoint. Same managed-task/op-id/lifecycle contract as
//! `rooms.rs`.
//!
//! HONESTY NOTE, load-bearing: the homeserver can only search what it can
//! read. In an encrypted room the server holds ciphertext, so `/search`
//! silently returns nothing for it. The UI must (and does) disclose that
//! server search covers unencrypted rooms only; the loaded-timeline find
//! remains the only search inside encrypted rooms. No decrypted content is
//! ever sent to the server by this module — the only thing that crosses is
//! the user's typed search term, which is inherent to server search and
//! disclosed in the UI.

use std::sync::Arc;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::search::search_events::{
            self,
            v3::{Categories, Criteria, EventContext, OrderBy, SearchKeys},
        },
        uint, RoomId,
    },
};
use serde::Deserialize;
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// Bound on one result page (server may return fewer).
const SEARCH_PAGE_CAP: u64 = 30;

/// One `/search` request budget; runs on the room-action pool which is
/// joined during sign-out, so no retry and a hard timeout.
const SEARCH_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(20);

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SearchFilters {
    #[serde(default)]
    from_user_ids: Vec<String>,
}

/// One page of server-side message search.
///
/// `room_id` empty = all (unencrypted) rooms the account can search;
/// otherwise the search is filtered to that room. `next_batch` empty = the
/// first page. Result event: `message_search_result { op_id, lifecycle, ok,
/// category, results[], next_batch, count }` where each result row carries
/// room_id, event_id, sender, sender_display_name, sender_avatar_url,
/// timestamp_ms, msgtype and body (the plain body only — no formatted
/// payloads cross the bridge).
pub(crate) fn search_messages(
    bridge: &RustClient,
    term: String,
    room_id: String,
    next_batch: String,
    filters_json: String,
    limit: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let term = term.trim().to_owned();
    if term.is_empty() {
        return Err("empty search term".to_owned());
    }
    // A room filter that does not parse is caller error, not a search miss.
    let room_filter = if room_id.trim().is_empty() {
        None
    } else {
        Some(
            RoomId::parse(room_id.trim())
                .map_err(|_| "invalid room id".to_owned())?
                .to_owned(),
        )
    };
    let filters: SearchFilters = serde_json::from_str(&filters_json)
        .map_err(|_| "invalid search filters".to_owned())?;
    // Matrix's RoomEventFilter can apply sender ids on the homeserver. Cap
    // the list before dispatch and reject malformed ids instead of silently
    // broadening the user's query.
    if filters.from_user_ids.len() > 50 {
        return Err("too many search senders".to_owned());
    }
    let sender_filter = filters
        .from_user_ids
        .iter()
        .map(|value| {
            value
                .parse::<matrix_sdk::ruma::OwnedUserId>()
                .map_err(|_| "invalid search sender".to_owned())
        })
        .collect::<Result<Vec<_>, _>>()?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut criteria = Criteria::new(term);
        // Message bodies only: name/topic hits would surface state events
        // the timeline UI cannot honestly present as "messages".
        criteria.keys = Some(vec![SearchKeys::ContentBody]);
        criteria.order_by = Some(OrderBy::Recent);
        // No surrounding events — the result row is a pointer, and event
        // context for navigation is the timeline's job. Historic profiles
        // ARE requested so rows can show real names without extra lookups.
        let mut context = EventContext::new();
        context.before_limit = uint!(0);
        context.after_limit = uint!(0);
        context.include_profile = true;
        criteria.event_context = context;
        criteria.filter.limit =
            Some(limit.clamp(1, SEARCH_PAGE_CAP).try_into().unwrap_or(uint!(20)));
        if let Some(room) = room_filter {
            criteria.filter.rooms = Some(vec![room]);
        }
        if !sender_filter.is_empty() {
            criteria.filter.senders = Some(sender_filter);
        }
        let mut categories = Categories::new();
        categories.room_events = Some(criteria);
        let mut request = search_events::v3::Request::new(categories);
        if !next_batch.trim().is_empty() {
            request.next_batch = Some(next_batch.trim().to_owned());
        }

        let result = client
            .send(request)
            .with_request_config(
                RequestConfig::new()
                    .disable_retry()
                    .timeout(SEARCH_REQUEST_TIMEOUT),
            )
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(response) => {
                let room_events = response.search_categories.room_events;
                let mut results = Vec::new();
                for hit in &room_events.results {
                    let Some(raw) = hit.result.as_ref() else { continue };
                    let Ok(value) =
                        serde_json::from_str::<serde_json::Value>(raw.json().get())
                    else {
                        continue; // tolerate shapes we do not understand
                    };
                    let event_id = value
                        .get("event_id")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let room = value
                        .get("room_id")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let sender = value
                        .get("sender")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let body = value
                        .pointer("/content/body")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let msgtype = value
                        .pointer("/content/msgtype")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let event_type = value
                        .get("type")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    let mention_user_ids = value
                        .pointer("/content/m.mentions/user_ids")
                        .and_then(|v| v.as_array())
                        .map(|values| {
                            values
                                .iter()
                                .filter_map(|v| v.as_str())
                                .map(str::to_owned)
                                .collect::<Vec<_>>()
                        })
                        .unwrap_or_default();
                    let formatted_body = value
                        .pointer("/content/formatted_body")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default();
                    // This is intentionally conservative: only explicit
                    // http(s) targets count as links. Media MXC URLs do not.
                    let has_link = body
                        .split_whitespace()
                        .any(|word| {
                            word.starts_with("https://")
                                || word.starts_with("http://")
                        })
                        || formatted_body.contains("href=\"https://")
                        || formatted_body.contains("href=\"http://");
                    if event_id.is_empty() || room.is_empty() || body.is_empty() {
                        // Redacted or non-message hits carry nothing a
                        // result row could honestly display.
                        continue;
                    }
                    let ts = value
                        .get("origin_server_ts")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0);
                    let (display_name, avatar_url) = sender
                        .parse::<matrix_sdk::ruma::OwnedUserId>()
                        .ok()
                        .and_then(|uid| hit.context.profile_info.get(&uid))
                        .map(|profile| {
                            (
                                profile.displayname.clone().unwrap_or_default(),
                                profile
                                    .avatar_url
                                    .as_ref()
                                    .map(ToString::to_string)
                                    .unwrap_or_default(),
                            )
                        })
                        .unwrap_or_default();
                    results.push(json!({
                        "room_id": room,
                        "event_id": event_id,
                        "sender": sender,
                        "sender_display_name": display_name,
                        "sender_avatar_url": avatar_url,
                        "timestamp_ms": ts,
                        "msgtype": msgtype,
                        "is_sticker": event_type == "m.sticker",
                        "mention_user_ids": mention_user_ids,
                        "has_link": has_link,
                        "body": body,
                    }));
                }
                enqueue(&events, json!({
                    "type": "message_search_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "results": results,
                    "next_batch": room_events.next_batch.clone().unwrap_or_default(),
                    "count": room_events.count.map(u64::from).unwrap_or(0),
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "message_search_result",
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
