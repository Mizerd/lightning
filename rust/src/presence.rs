//! Matrix presence (v0.7.x): bounded polling of other users' presence and
//! publication of the local user's own state.
//!
//! Sliding Sync delivers NO presence events (MSC4186 has no presence
//! extension), so presence cannot ride the sync loop the way receipts and
//! typing do. Instead C++ drives a bounded polling loop over exactly the
//! users that are currently on screen (DM rows, the open People list, an
//! open profile popover) and this module answers each round with one
//! `presence_batch` poll event. The polling *policy* — who is watched, the
//! cadence, the give-up latch when a server refuses presence — lives in
//! C++ (`PresenceManager`); this module is deliberately stateless.
//!
//! Only presentation-safe fields cross the FFI: user id, coarse state
//! string, `currently_active`, and `last_active_ago` milliseconds. A status
//! message is NOT forwarded (it is free-form remote text with no current UI
//! consumer). Nothing here logs user ids or SDK error text; failures cross
//! as the shared coarse categories from `classify_room_error`.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::presence::{get_presence, set_presence},
        presence::PresenceState,
        OwnedUserId, UserId,
    },
};
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// Hard cap on one polling batch. C++ keeps its watched set below this and
/// round-robins anything larger; the cap here is defense in depth so a
/// malformed request can never fan out into hundreds of sequential GETs.
pub(crate) const PRESENCE_BATCH_CAP: usize = 40;

/// Per-request bound. Presence polls are periodic and disposable — a slow
/// answer is worthless (the next round supersedes it), and the room-action
/// pool is JOINED during sign-out, so an unbounded default-retry request
/// loop here would stall shutdown. No retries for the same reason.
const PRESENCE_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// Coarse state string for the bridge. `_Custom` (a server sending a
/// non-spec state) crosses as "unknown", which C++ renders as no indicator
/// — never as a fabricated offline.
pub(crate) fn presence_state_str(state: &PresenceState) -> &'static str {
    match state {
        PresenceState::Online => "online",
        PresenceState::Unavailable => "unavailable",
        PresenceState::Offline => "offline",
        _ => "unknown",
    }
}

/// Own-presence code from C++: 0 online, 1 unavailable, 2 offline.
pub(crate) fn own_presence_state(code: u32) -> Option<PresenceState> {
    match code {
        0 => Some(PresenceState::Online),
        1 => Some(PresenceState::Unavailable),
        2 => Some(PresenceState::Offline),
        _ => None,
    }
}

/// Parse the C++ batch (a JSON array of user-id strings) into validated
/// ids. Invalid entries are dropped rather than failing the whole batch —
/// one malformed id must not blind every other indicator — but an empty or
/// non-array payload is a caller bug and is refused.
pub(crate) fn parse_presence_batch(payload: &str) -> Result<Vec<OwnedUserId>, String> {
    let raw: Vec<String> = serde_json::from_str(payload)
        .map_err(|_| "invalid presence batch payload".to_owned())?;
    let ids: Vec<OwnedUserId> = raw
        .iter()
        .filter_map(|value| UserId::parse(value).ok())
        .take(PRESENCE_BATCH_CAP)
        .collect();
    if ids.is_empty() {
        return Err("empty presence batch".to_owned());
    }
    Ok(ids)
}

/// One polling round: at most eight concurrent GETs (a batch is at most
/// `PRESENCE_BATCH_CAP` small requests)
/// answered as a single `presence_batch` event. Per-user failures are
/// per-entry `ok:false` with a coarse category so C++ can distinguish
/// "this server refuses presence" (forbidden) from "no data for this user"
/// (not_found) without ever seeing SDK error text.
pub(crate) fn fetch_presence(
    bridge: &RustClient,
    payload: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let ids = parse_presence_batch(&payload)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        const CONCURRENCY: usize = 8;
        let semaphore = Arc::new(tokio::sync::Semaphore::new(CONCURRENCY));
        let mut requests = tokio::task::JoinSet::new();
        let expected = ids.len();
        for uid in ids {
            let client = client.clone();
            let semaphore = Arc::clone(&semaphore);
            requests.spawn(async move {
                let Ok(_permit) = semaphore.acquire_owned().await else {
                    return json!({
                        "user_id": uid.to_string(),
                        "ok": false,
                        "category": "network",
                    });
                };
                let config = RequestConfig::new()
                    .disable_retry()
                    .timeout(PRESENCE_REQUEST_TIMEOUT);
                let result = client
                    .send(get_presence::v3::Request::new(uid.clone()))
                    .with_request_config(config)
                    .await;
                match result {
                    Ok(response) => {
                        let mut entry = json!({
                            "user_id": uid.to_string(),
                            "ok": true,
                            "state": presence_state_str(&response.presence),
                            "currently_active":
                                response.currently_active.unwrap_or(false),
                        });
                        if let Some(ago) = response.last_active_ago {
                            entry["last_active_ago_ms"] = json!(
                                ago.as_millis().min(u128::from(u32::MAX)) as u64
                            );
                        }
                        entry
                    }
                    Err(err) => json!({
                        "user_id": uid.to_string(),
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }),
                }
            });
        }
        let mut entries = Vec::with_capacity(expected);
        while let Some(result) = requests.join_next().await {
            if !timelines.lifecycle_current(lifecycle) {
                requests.abort_all();
                return;
            }
            if let Ok(entry) = result {
                entries.push(entry);
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "presence_batch",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "entries": entries,
        }));
    });
    Ok(())
}

/// Publish the local user's own presence. Fire-and-forget by design: the
/// UI claims nothing about publication, so there is nothing to report on
/// success, and a failure only surfaces as a `presence_publish_failed`
/// event (coarse category) for the retry/latch logic in C++. No status
/// message is ever sent.
pub(crate) fn publish_presence(bridge: &RustClient, state_code: u32) -> Result<(), String> {
    let client = require_client(bridge)?;
    let state =
        own_presence_state(state_code).ok_or_else(|| "invalid presence state".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let Some(uid) = client.user_id().map(ToOwned::to_owned) else {
            return;
        };
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(PRESENCE_REQUEST_TIMEOUT);
        let result = client
            .send(set_presence::v3::Request::new(uid, state))
            .with_request_config(config)
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        if let Err(err) = result {
            enqueue(&events, json!({
                "type": "presence_publish_failed",
                "lifecycle": lifecycle,
                "category": classify_room_error(&err.to_string()),
            }));
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn presence_state_maps_spec_states() {
        assert_eq!(presence_state_str(&PresenceState::Online), "online");
        assert_eq!(presence_state_str(&PresenceState::Unavailable), "unavailable");
        assert_eq!(presence_state_str(&PresenceState::Offline), "offline");
        // A non-spec state must cross as unknown, never as a fabricated
        // spec value.
        let custom = PresenceState::from("org.example.busy");
        assert_eq!(presence_state_str(&custom), "unknown");
    }

    #[test]
    fn own_presence_codes_are_closed() {
        assert_eq!(own_presence_state(0), Some(PresenceState::Online));
        assert_eq!(own_presence_state(1), Some(PresenceState::Unavailable));
        assert_eq!(own_presence_state(2), Some(PresenceState::Offline));
        assert_eq!(own_presence_state(3), None);
        assert_eq!(own_presence_state(u32::MAX), None);
    }

    #[test]
    fn batch_parse_drops_invalid_ids_but_refuses_empty() {
        let ids =
            parse_presence_batch(r#"["@alice:example.org","not a user id","@bob:example.org"]"#)
                .expect("valid batch");
        assert_eq!(ids.len(), 2);
        assert_eq!(ids[0].as_str(), "@alice:example.org");
        assert_eq!(ids[1].as_str(), "@bob:example.org");

        assert!(parse_presence_batch("[]").is_err());
        assert!(parse_presence_batch(r#"["not a user id"]"#).is_err());
        assert!(parse_presence_batch("{\"a\":1}").is_err());
        assert!(parse_presence_batch("garbage").is_err());
    }

    #[test]
    fn batch_parse_enforces_the_cap() {
        let ids: Vec<String> =
            (0..100).map(|i| format!("@user{i}:example.org")).collect();
        let payload = serde_json::to_string(&ids).expect("serialize");
        let parsed = parse_presence_batch(&payload).expect("valid batch");
        assert_eq!(parsed.len(), PRESENCE_BATCH_CAP);
    }
}
