
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

pub(crate) const PRESENCE_BATCH_CAP: usize = 40;

const PRESENCE_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

pub(crate) fn presence_state_str(state: &PresenceState) -> &'static str {
    match state {
        PresenceState::Online => "online",
        PresenceState::Unavailable => "unavailable",
        PresenceState::Offline => "offline",
        _ => "unknown",
    }
}

pub(crate) fn own_presence_state(code: u32) -> Option<PresenceState> {
    match code {
        0 => Some(PresenceState::Online),
        1 => Some(PresenceState::Unavailable),
        2 => Some(PresenceState::Offline),
        _ => None,
    }
}

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
                        if let Some(msg) = response.status_msg.as_deref() {
                            let cleaned = clean_status_msg(msg);
                            if !cleaned.is_empty() {
                                entry["status_msg"] = json!(cleaned);
                            }
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

pub(crate) fn clean_status_msg(raw: &str) -> String {
    const MAX_CHARS: usize = 256;
    raw.chars()
        .filter(|c| !c.is_control())
        .take(MAX_CHARS)
        .collect::<String>()
        .trim()
        .to_owned()
}

pub(crate) fn publish_presence(
    bridge: &RustClient,
    state_code: u32,
    status_msg: Option<String>,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let state =
        own_presence_state(state_code).ok_or_else(|| "invalid presence state".to_owned())?;
    let status_msg = status_msg.map(|m| clean_status_msg(&m)).filter(|m| !m.is_empty());
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
        let mut request = set_presence::v3::Request::new(uid, state);
        request.status_msg = status_msg;
        let result = client
            .send(request)
            .with_request_config(config)
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        if result.is_ok() {
            enqueue(&events, json!({
                "type": "presence_published",
                "lifecycle": lifecycle,
            }));
        }
        if let Err(err) = result {
            enqueue(&events, json!({
                "type": "presence_publish_failed",
                "lifecycle": lifecycle,
                "category": classify_room_error(&err.to_string()),
                // Keep the server's retry hint: flattened to "rate_limited", the keep-alive
                // waited a whole period, and with several devices on one account most
                // publishes were rejected for minutes while the account read offline.
                // Both RetryAfter variants are converted.
                "retry_after_ms": retry_after_ms(&err),
            }));
        }
    });
    Ok(())
}

fn retry_after_ms(error: &matrix_sdk::HttpError) -> Option<u64> {
    use matrix_sdk::ruma::api::error::{ErrorKind, LimitExceededErrorData};
    let ErrorKind::LimitExceeded(LimitExceededErrorData { retry_after, .. }) =
        error.client_api_error_kind()?
    else {
        return None;
    };
    retry_after_to_ms(retry_after.as_ref()?)
}

pub(crate) fn retry_after_to_ms(retry_after: &matrix_sdk::ruma::api::error::RetryAfter) -> Option<u64> {
    use matrix_sdk::ruma::api::error::RetryAfter;
    match retry_after {
        RetryAfter::Delay(duration) => u64::try_from(duration.as_millis()).ok(),
        RetryAfter::DateTime(at) => at
            .duration_since(std::time::SystemTime::now())
            .ok()
            .and_then(|d| u64::try_from(d.as_millis()).ok()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_delay_hint_converts_to_its_milliseconds() {
        use matrix_sdk::ruma::api::error::RetryAfter;
        assert_eq!(
            retry_after_to_ms(&RetryAfter::Delay(Duration::from_millis(800))),
            Some(800)
        );
        assert_eq!(
            retry_after_to_ms(&RetryAfter::Delay(Duration::from_secs(0))),
            Some(0)
        );
    }

    #[test]
    fn a_future_datetime_hint_becomes_the_wait_from_now() {
        use matrix_sdk::ruma::api::error::RetryAfter;
        let at = std::time::SystemTime::now() + Duration::from_secs(5);
        let ms = retry_after_to_ms(&RetryAfter::DateTime(at))
            .expect("a future instant converts");
        assert!(
            (4_000..=5_000).contains(&ms),
            "a five-second hint converted to {ms} ms"
        );
    }

    #[test]
    fn a_past_datetime_hint_is_not_a_negative_wait() {
        use matrix_sdk::ruma::api::error::RetryAfter;
        let at = std::time::SystemTime::now() - Duration::from_secs(5);
        assert_eq!(retry_after_to_ms(&RetryAfter::DateTime(at)), None);
    }

    #[test]
    fn presence_state_maps_spec_states() {
        assert_eq!(presence_state_str(&PresenceState::Online), "online");
        assert_eq!(presence_state_str(&PresenceState::Unavailable), "unavailable");
        assert_eq!(presence_state_str(&PresenceState::Offline), "offline");
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
