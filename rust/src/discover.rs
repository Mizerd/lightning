//! Room discovery, preview, join and knock (v0.7.x).
//!
//! Same contract as `rooms.rs`: thin `extern "C"` wrappers in `lib.rs`,
//! synchronous validation on the caller's thread, network work spawned as a
//! managed task (`spawn_room_action`), and results enqueued as JSON events
//! stamped with the lifecycle generation and an operation id so C++ rejects
//! stale completions. Lightning implements no directory/join/knock protocol
//! of its own — every network behaviour below is the SDK call it names.
//!
//! Nothing here serializes tokens, raw events, or server error text into the
//! queue: failures cross as the coarse `classify_room_error` categories.

use std::sync::Arc;

use matrix_sdk::{
    ruma::{
        api::client::directory::get_public_rooms_filtered,
        matrix_uri::MatrixId,
        room::JoinRuleSummary,
        MatrixToUri, MatrixUri, OwnedRoomId, OwnedRoomOrAliasId, OwnedServerName,
        RoomAliasId, RoomId, UInt,
    },
    RoomState,
};
use matrix_sdk_ui::spaces::room_list::SpaceRoomListPaginationState;
use matrix_sdk_ui::spaces::SpaceRoomList;
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// Bound on one public-rooms page. The directory UI paginates; a giant page
/// buys latency, not coverage.
const PUBLIC_ROOMS_PAGE_CAP: u64 = 50;

/// Bounds on the space-children listing: `/hierarchy` pages fetched and rows
/// forwarded. A larger space reports `truncated` rather than walking the
/// entire federation graph.
const SPACE_CHILDREN_PAGE_CAP: usize = 10;
const SPACE_CHILDREN_ROW_CAP: usize = 200;

/// Per-request budget. These ops run on the room-action pool, which is
/// JOINED during sign-out — an unbounded retry loop there stalls shutdown.
const DISCOVER_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

fn join_rule_summary_str(rule: Option<&JoinRuleSummary>) -> &'static str {
    match rule {
        Some(JoinRuleSummary::Public) => "public",
        Some(JoinRuleSummary::Invite) => "invite",
        Some(JoinRuleSummary::Knock) => "knock",
        Some(JoinRuleSummary::Private) => "private",
        Some(JoinRuleSummary::Restricted(_)) => "restricted",
        Some(JoinRuleSummary::KnockRestricted(_)) => "knock_restricted",
        _ => "",
    }
}

fn membership_str(state: Option<RoomState>) -> &'static str {
    match state {
        Some(RoomState::Joined) => "joined",
        Some(RoomState::Invited) => "invited",
        Some(RoomState::Knocked) => "knocked",
        Some(RoomState::Banned) => "banned",
        Some(RoomState::Left) => "left",
        None => "",
    }
}

/// A normalized join target parsed from user input: a room id or alias plus
/// the routing servers a URI carried, and — for event permalinks — the event
/// the link pointed at.
struct ResolvedTarget {
    target: OwnedRoomOrAliasId,
    via: Vec<OwnedServerName>,
    event_id: String,
}

/// Parse any supported room identifier form. Pure; unit-tested below.
///
/// Accepted: `#alias:server`, `!roomid:server`, `matrix:` URIs
/// (`matrix:r/...`, `matrix:roomid/...`, with or without `/e/...`), and
/// `matrix.to` permalinks. User links are NOT rooms and are refused.
fn parse_room_target(input: &str) -> Result<ResolvedTarget, &'static str> {
    let trimmed = input.trim();
    if trimmed.is_empty() {
        return Err("empty");
    }
    if let Some(rest) = trimmed.strip_prefix('#') {
        // Reject "#" alone / garbage early; RoomAliasId does the real check.
        let _ = rest;
        return RoomAliasId::parse(trimmed)
            .map(|alias| ResolvedTarget {
                target: OwnedRoomOrAliasId::from(alias),
                via: Vec::new(),
                event_id: String::new(),
            })
            .map_err(|_| "invalid");
    }
    if trimmed.starts_with('!') {
        return RoomId::parse(trimmed)
            .map(|id| ResolvedTarget {
                target: OwnedRoomOrAliasId::from(id),
                via: Vec::new(),
                event_id: String::new(),
            })
            .map_err(|_| "invalid");
    }
    let (id, via) = if trimmed.starts_with("matrix:") {
        let uri = MatrixUri::parse(trimmed).map_err(|_| "invalid")?;
        (uri.id().clone(), uri.via().to_vec())
    } else if trimmed.contains("matrix.to/#/") {
        let uri = MatrixToUri::parse(trimmed).map_err(|_| "invalid")?;
        (uri.id().clone(), uri.via().to_vec())
    } else {
        return Err("invalid");
    };
    match id {
        MatrixId::Room(room_id) => Ok(ResolvedTarget {
            target: OwnedRoomOrAliasId::from(room_id),
            via,
            event_id: String::new(),
        }),
        MatrixId::RoomAlias(alias) => Ok(ResolvedTarget {
            target: OwnedRoomOrAliasId::from(alias),
            via,
            event_id: String::new(),
        }),
        MatrixId::Event(room_or_alias, event_id) => Ok(ResolvedTarget {
            target: room_or_alias,
            via,
            event_id: event_id.to_string(),
        }),
        // A user link opens a profile, never a room join surface.
        _ => Err("not_a_room"),
    }
}

/// Resolve user input into a normalized target and preview it.
///
/// Result event: `room_target_resolved { op_id, lifecycle, ok, target,
/// via[], event_id, preview_ok, preview_category, room_id, alias, name,
/// topic, avatar_url, members, join_rule, membership, is_space }`.
///
/// `ok=false` means the INPUT is not a room identifier (category `invalid` /
/// `not_a_room`). A failed preview is NOT a failed resolution: servers may
/// forbid previews of rooms that can still be joined, so the normalized
/// target always crosses and the UI offers Join with `preview_ok=false`.
pub(crate) fn resolve_room_target(
    bridge: &RustClient,
    input: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let resolved = match parse_room_target(&input) {
            Ok(resolved) => resolved,
            Err(category) => {
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, json!({
                        "type": "room_target_resolved",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": category,
                    }));
                }
                return;
            }
        };
        let preview = tokio::time::timeout(
            DISCOVER_REQUEST_TIMEOUT,
            client.get_room_preview(&resolved.target, resolved.via.clone()),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let via: Vec<String> = resolved.via.iter().map(ToString::to_string).collect();
        match preview {
            Ok(Ok(preview)) => {
                enqueue(&events, json!({
                    "type": "room_target_resolved",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "target": resolved.target.to_string(),
                    "via": via,
                    "event_id": resolved.event_id,
                    "preview_ok": true,
                    "room_id": preview.room_id.to_string(),
                    "alias": preview
                        .canonical_alias
                        .as_ref()
                        .map(ToString::to_string)
                        .unwrap_or_default(),
                    "name": preview.name.clone().unwrap_or_default(),
                    "topic": preview.topic.clone().unwrap_or_default(),
                    "avatar_url": preview
                        .avatar_url
                        .as_ref()
                        .map(ToString::to_string)
                        .unwrap_or_default(),
                    "members": preview.num_joined_members,
                    "join_rule": join_rule_summary_str(preview.join_rule.as_ref()),
                    "membership": membership_str(preview.state),
                    "is_space": preview.room_type
                        == Some(matrix_sdk::ruma::room::RoomType::Space),
                }));
            }
            other => {
                // Preview refused, failed, or timed out — the target still
                // resolved, and joining may well succeed (e.g. an
                // invite-only room forbids previews).
                let category = match other {
                    Ok(Err(err)) => classify_room_error(&err.to_string()),
                    _ => "network",
                };
                enqueue(&events, json!({
                    "type": "room_target_resolved",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "target": resolved.target.to_string(),
                    "via": via,
                    "event_id": resolved.event_id,
                    "preview_ok": false,
                    "preview_category": category,
                }));
            }
        }
    });
    Ok(())
}

/// One page of the public room directory (`public_rooms_filtered`).
///
/// Result event: `public_rooms_result { op_id, lifecycle, ok, category,
/// results[], next_batch, total_estimate }`. Each row carries membership so
/// the UI can label already-joined rooms without a second lookup.
pub(crate) fn search_public_rooms(
    bridge: &RustClient,
    query: String,
    server: String,
    since: String,
    limit: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    // A bad server name is caller error, refused synchronously.
    let server = if server.trim().is_empty() {
        None
    } else {
        Some(
            OwnedServerName::try_from(server.trim())
                .map_err(|_| "invalid directory server name".to_owned())?,
        )
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut request = get_public_rooms_filtered::v3::Request::default();
        request.server = server;
        request.limit = Some(UInt::from(
            u32::try_from(limit.clamp(1, PUBLIC_ROOMS_PAGE_CAP)).unwrap_or(20u32),
        ));
        if !since.is_empty() {
            request.since = Some(since);
        }
        if !query.trim().is_empty() {
            request.filter.generic_search_term = Some(query.trim().to_owned());
        }
        let result = tokio::time::timeout(
            DISCOVER_REQUEST_TIMEOUT,
            client.public_rooms_filtered(request),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(Ok(response)) => {
                let results: Vec<serde_json::Value> = response
                    .chunk
                    .iter()
                    .map(|room| {
                        let membership = client
                            .get_room(&room.room_id)
                            .map(|known| membership_str(Some(known.state())))
                            .unwrap_or("");
                        json!({
                            "room_id": room.room_id.to_string(),
                            "name": room.name.clone().unwrap_or_default(),
                            "alias": room
                                .canonical_alias
                                .as_ref()
                                .map(ToString::to_string)
                                .unwrap_or_default(),
                            "topic": room.topic.clone().unwrap_or_default(),
                            "avatar_url": room
                                .avatar_url
                                .as_ref()
                                .map(ToString::to_string)
                                .unwrap_or_default(),
                            "members": u64::from(room.num_joined_members),
                            "world_readable": room.world_readable,
                            "guest_can_join": room.guest_can_join,
                            "join_rule": match room.join_rule {
                                matrix_sdk::ruma::room::JoinRuleKind::Public => "public",
                                matrix_sdk::ruma::room::JoinRuleKind::Knock => "knock",
                                matrix_sdk::ruma::room::JoinRuleKind::Invite => "invite",
                                matrix_sdk::ruma::room::JoinRuleKind::Restricted =>
                                    "restricted",
                                matrix_sdk::ruma::room::JoinRuleKind::KnockRestricted =>
                                    "knock_restricted",
                                matrix_sdk::ruma::room::JoinRuleKind::Private => "private",
                                _ => "",
                            },
                            "is_space": room.room_type
                                == Some(matrix_sdk::ruma::room::RoomType::Space),
                            "membership": membership,
                        })
                    })
                    .collect();
                enqueue(&events, json!({
                    "type": "public_rooms_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "results": results,
                    "next_batch": response.next_batch.clone().unwrap_or_default(),
                    "total_estimate": response
                        .total_room_count_estimate
                        .map(u64::from)
                        .unwrap_or(0),
                }));
            }
            other => {
                let category = match other {
                    Ok(Err(err)) => classify_room_error(&err.to_string()),
                    _ => "network",
                };
                enqueue(&events, json!({
                    "type": "public_rooms_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
                    "results": [],
                }));
            }
        }
    });
    Ok(())
}

/// Join a room by id or alias, optionally routed `via` the given servers.
/// SDK-owned end to end (`Client::join_room_by_id_or_alias`); on success the
/// authoritative room snapshot is re-enqueued so the room list learns of the
/// new membership without waiting for the next sync round.
///
/// Result event: `room_join_result { op_id, lifecycle, ok, room_id,
/// category }`.
pub(crate) fn join_room(
    bridge: &RustClient,
    target: String,
    via: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let target = OwnedRoomOrAliasId::try_from(target.trim())
        .map_err(|_| "invalid room id or alias".to_owned())?;
    let servers: Vec<OwnedServerName> = via
        .iter()
        .filter_map(|s| OwnedServerName::try_from(s.trim()).ok())
        .collect();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(
            DISCOVER_REQUEST_TIMEOUT,
            client.join_room_by_id_or_alias(&target, &servers),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        // Ordering (review L4): the authoritative snapshot only refreshes
        // for a still-current session, matching rooms.rs precedent.
        if matches!(&result, Ok(Ok(_))) {
            crate::enqueue_rooms(&events, &client).await;
        }
        match result {
            Ok(Ok(room)) => {
                enqueue(&events, json!({
                    "type": "room_join_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                }));
            }
            other => {
                let category = match other {
                    Ok(Err(err)) => classify_join_error(&err.to_string()),
                    _ => "network",
                };
                enqueue(&events, json!({
                    "type": "room_join_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
                }));
            }
        }
    });
    Ok(())
}

/// Join failures deserve finer categories than the generic room ops: the
/// server distinguishes a ban from plain lack of permission, and the UI must
/// not present either as a network problem. Pure and unit-tested.
pub(crate) fn classify_join_error(message: &str) -> &'static str {
    let lc = message.to_lowercase();
    if lc.contains("banned") {
        "banned"
    } else if lc.contains("restricted")
        && (lc.contains("membership") || lc.contains("join"))
    {
        // Synapse's restricted-rule rejection mentions the unmet condition.
        "restricted_denied"
    } else {
        classify_room_error(message)
    }
}

/// Knock on a room (`Client::knock`). Only offered by the UI when the join
/// rule says knocking is possible; the server remains the authority and a
/// rejection crosses as its category.
///
/// Result event: `room_knock_result { op_id, lifecycle, ok, room_id,
/// category }`.
pub(crate) fn knock_room(
    bridge: &RustClient,
    target: String,
    via: Vec<String>,
    reason: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let target = OwnedRoomOrAliasId::try_from(target.trim())
        .map_err(|_| "invalid room id or alias".to_owned())?;
    let servers: Vec<OwnedServerName> = via
        .iter()
        .filter_map(|s| OwnedServerName::try_from(s.trim()).ok())
        .collect();
    let reason = {
        let trimmed = reason.trim();
        if trimmed.is_empty() { None } else { Some(trimmed.to_owned()) }
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(
            DISCOVER_REQUEST_TIMEOUT,
            client.knock(target, reason, servers),
        )
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        // Ordering (review L4): the authoritative snapshot only refreshes
        // for a still-current session, matching rooms.rs precedent.
        if matches!(&result, Ok(Ok(_))) {
            crate::enqueue_rooms(&events, &client).await;
        }
        match result {
            Ok(Ok(room)) => {
                enqueue(&events, json!({
                    "type": "room_knock_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                }));
            }
            other => {
                let category = match other {
                    Ok(Err(err)) => classify_join_error(&err.to_string()),
                    _ => "network",
                };
                enqueue(&events, json!({
                    "type": "room_knock_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
                }));
            }
        }
    });
    Ok(())
}

/// Withdraw a pending knock. `Room::leave()` is the Matrix mechanism for
/// this; the existing `leave_room` path cannot be reused because its
/// `joined_room()` lookup filters to `RoomState::Joined` on purpose.
///
/// Result event: `knock_cancel_result { op_id, lifecycle, ok, room_id,
/// category }`.
pub(crate) fn cancel_knock(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let parsed = RoomId::parse(room_id.trim())
        .map_err(|_| "invalid room id".to_owned())?;
    let room = client
        .get_room(&parsed)
        .filter(|room| room.state() == RoomState::Knocked)
        .ok_or_else(|| "no pending knock for that room".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result =
            tokio::time::timeout(DISCOVER_REQUEST_TIMEOUT, room.leave()).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        // Ordering (review L4): the authoritative snapshot only refreshes
        // for a still-current session, matching rooms.rs precedent.
        if matches!(&result, Ok(Ok(_))) {
            crate::enqueue_rooms(&events, &client).await;
        }
        match result {
            Ok(Ok(())) => {
                enqueue(&events, json!({
                    "type": "knock_cancel_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                }));
            }
            other => {
                let category = match other {
                    Ok(Err(err)) => classify_room_error(&err.to_string()),
                    _ => "network",
                };
                enqueue(&events, json!({
                    "type": "knock_cancel_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
                }));
            }
        }
    });
    Ok(())
}

/// List a Space's children — joined AND unjoined — through the SDK's
/// `SpaceRoomList` (`/hierarchy`-backed). Bounded: at most
/// `SPACE_CHILDREN_PAGE_CAP` pages / `SPACE_CHILDREN_ROW_CAP` rows, with a
/// `truncated` flag rather than an unbounded federation walk.
///
/// Result event: `space_children_result { op_id, lifecycle, ok, space_id,
/// truncated, results[], category }`.
pub(crate) fn space_children(
    bridge: &RustClient,
    space_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let parsed: OwnedRoomId = RoomId::parse(space_id.trim())
        .map_err(|_| "invalid space id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let list = SpaceRoomList::new(client.clone(), parsed.clone()).await;
        let mut pages = 0usize;
        let mut failure: Option<&'static str> = None;
        loop {
            match list.pagination_state() {
                SpaceRoomListPaginationState::Idle { end_reached: true } => break,
                _ => {}
            }
            if pages >= SPACE_CHILDREN_PAGE_CAP {
                break;
            }
            if list.rooms().await.len() >= SPACE_CHILDREN_ROW_CAP {
                break;
            }
            let step =
                tokio::time::timeout(DISCOVER_REQUEST_TIMEOUT, list.paginate()).await;
            match step {
                Ok(Ok(())) => pages += 1,
                Ok(Err(err)) => {
                    failure = Some(classify_room_error(&err.to_string()));
                    break;
                }
                Err(_) => {
                    failure = Some("network");
                    break;
                }
            }
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let rooms = list.rooms().await;
        if rooms.is_empty() {
            if let Some(category) = failure {
                // Nothing fetched and the fetch failed: an honest failure,
                // never an authoritative "this space is empty".
                enqueue(&events, json!({
                    "type": "space_children_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "space_id": parsed.to_string(),
                    "category": category,
                    "results": [],
                }));
                return;
            }
        }
        // Review L5: filter the space's own row BEFORE applying the cap
        // (or a full listing silently loses one child), and only report
        // truncated when pagination genuinely stopped short of the end.
        let end_reached = matches!(
            list.pagination_state(),
            SpaceRoomListPaginationState::Idle { end_reached: true }
        );
        let children: Vec<_> =
            rooms.iter().filter(|room| room.room_id != parsed).collect();
        let truncated =
            !end_reached || children.len() > SPACE_CHILDREN_ROW_CAP;
        let results: Vec<serde_json::Value> = children
            .iter()
            .take(SPACE_CHILDREN_ROW_CAP)
            .map(|room| {
                json!({
                    "room_id": room.room_id.to_string(),
                    "name": room.display_name.clone(),
                    "alias": room
                        .canonical_alias
                        .as_ref()
                        .map(ToString::to_string)
                        .unwrap_or_default(),
                    "topic": room.topic.clone().unwrap_or_default(),
                    "avatar_url": room
                        .avatar_url
                        .as_ref()
                        .map(ToString::to_string)
                        .unwrap_or_default(),
                    "members": room.num_joined_members,
                    "join_rule": join_rule_summary_str(room.join_rule.as_ref()),
                    "membership": membership_str(room.state),
                    "is_space": room.room_type
                        == Some(matrix_sdk::ruma::room::RoomType::Space),
                    "children_count": room.children_count,
                    "suggested": room.suggested,
                    "via": room
                        .via
                        .iter()
                        .map(ToString::to_string)
                        .collect::<Vec<_>>(),
                })
            })
            .collect();
        enqueue(&events, json!({
            "type": "space_children_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "ok": true,
            "space_id": parsed.to_string(),
            "truncated": truncated,
            "results": results,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_alias() {
        let t = parse_room_target(" #lightning:matrix.org ").unwrap();
        assert_eq!(t.target.to_string(), "#lightning:matrix.org");
        assert!(t.via.is_empty());
        assert!(t.event_id.is_empty());
    }

    #[test]
    fn parses_room_id() {
        let t = parse_room_target("!abc123:example.org").unwrap();
        assert_eq!(t.target.to_string(), "!abc123:example.org");
    }

    #[test]
    fn parses_matrix_to_room_link() {
        let t = parse_room_target(
            "https://matrix.to/#/%23lightning%3Amatrix.org",
        )
        .unwrap();
        assert_eq!(t.target.to_string(), "#lightning:matrix.org");
    }

    #[test]
    fn parses_matrix_to_event_link_with_via() {
        let t = parse_room_target(
            "https://matrix.to/#/!abc123%3Aexample.org/$evt?via=example.org&via=second.example",
        )
        .unwrap();
        assert_eq!(t.target.to_string(), "!abc123:example.org");
        assert_eq!(t.event_id, "$evt");
        assert_eq!(t.via.len(), 2);
    }

    #[test]
    fn parses_matrix_uri_forms() {
        let r = parse_room_target("matrix:r/lightning:matrix.org").unwrap();
        assert_eq!(r.target.to_string(), "#lightning:matrix.org");
        let id = parse_room_target("matrix:roomid/abc123:example.org?via=example.org")
            .unwrap();
        assert_eq!(id.target.to_string(), "!abc123:example.org");
        assert_eq!(id.via.len(), 1);
        let ev = parse_room_target(
            "matrix:roomid/abc123:example.org/e/evt?action=join",
        )
        .unwrap();
        assert_eq!(ev.target.to_string(), "!abc123:example.org");
        assert_eq!(ev.event_id, "$evt");
    }

    #[test]
    fn refuses_user_links_and_garbage() {
        assert!(parse_room_target("https://matrix.to/#/@user:example.org").is_err());
        assert!(parse_room_target("matrix:u/user:example.org").is_err());
        assert!(parse_room_target("").is_err());
        assert!(parse_room_target("hello world").is_err());
        assert!(parse_room_target("#").is_err());
        assert!(parse_room_target("https://example.org/room").is_err());
    }

    #[test]
    fn classify_join_error_categories() {
        assert_eq!(classify_join_error("M_FORBIDDEN: you are banned from the room"), "banned");
        assert_eq!(classify_join_error("M_FORBIDDEN: not invited"), "forbidden");
        assert_eq!(
            classify_join_error("unable to authorise join: restricted room membership"),
            "restricted_denied"
        );
        assert_eq!(classify_join_error("M_NOT_FOUND"), "not_found");
        assert_eq!(classify_join_error("connection refused"), "network");
    }
}
