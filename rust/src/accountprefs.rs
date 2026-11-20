
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    event_handler::EventHandlerHandle,
    ruma::{
        api::client::config::set_global_account_data,
        events::{
            macros::EventContent, GlobalAccountDataEvent, GlobalAccountDataEventType,
        },
        serde::Raw,
    },
    Client,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::backdrop::{classify_write_error, is_canonical_json_safe};
use crate::rooms::require_client;
use crate::{enqueue, EventQueueRef, RustClient};

pub(crate) const MEDIA_PREVIEW_TYPE: &str = "m.media_preview_config";
pub(crate) const RECENT_EMOJI_TYPE: &str = "m.recent_emoji";
pub(crate) const SAVED_MESSAGES_TYPE: &str = "org.lightning_matrix.saved_messages";
pub(crate) const INVITE_PERMISSION_TYPE: &str = "m.invite_permission_config";
pub(crate) const INVITE_PERMISSION_UNSTABLE_TYPE: &str =
    "org.matrix.msc4380.invite_permission_config";
pub(crate) const INVITE_PERMISSION_FEATURE: &str = "org.matrix.msc4380";

const MAX_CONTENT_BYTES: usize = 16 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

pub(crate) fn allowed_type(event_type: &str) -> Option<&'static str> {
    match event_type {
        MEDIA_PREVIEW_TYPE => Some(MEDIA_PREVIEW_TYPE),
        RECENT_EMOJI_TYPE => Some(RECENT_EMOJI_TYPE),
        SAVED_MESSAGES_TYPE => Some(SAVED_MESSAGES_TYPE),
        INVITE_PERMISSION_TYPE => Some(INVITE_PERMISSION_TYPE),
        INVITE_PERMISSION_UNSTABLE_TYPE => Some(INVITE_PERMISSION_UNSTABLE_TYPE),
        _ => None,
    }
}

pub(crate) fn parse_content(json: &str) -> Result<Value, &'static str> {
    if json.len() > MAX_CONTENT_BYTES {
        return Err("too_large");
    }
    let value: Value = serde_json::from_str(json).map_err(|_| "invalid_content")?;
    if !value.is_object() || !is_canonical_json_safe(&value) {
        return Err("invalid_content");
    }
    Ok(value)
}

pub(crate) fn check_saved_content(value: &Value) -> Result<(), &'static str> {
    let entries = value.get("entries").and_then(Value::as_array).ok_or("invalid_content")?;
    if value.as_object().map_or(true, |o| {
        o.len() != 2 || o.get("version") != Some(&json!(1))
    })
        || entries.len() > 100
    {
        return Err("invalid_content");
    }
    let valid_event = |item: Option<&Value>, empty_allowed: bool| {
        item.and_then(Value::as_str).is_some_and(|id| {
            (empty_allowed && id.is_empty())
                || (id.len() <= 255
                    && !id.contains('\u{1f}')
                    && matrix_sdk::ruma::EventId::parse(id).is_ok())
        })
    };
    for item in entries {
        let o = item.as_object().ok_or("invalid_content")?;
        if o.len() != 4
            || !o.get("room_id").and_then(Value::as_str).is_some_and(|id| {
                id.len() <= 255
                    && !id.contains('\u{1f}')
                    && matrix_sdk::ruma::RoomId::parse(id).is_ok()
            })
            || !valid_event(o.get("event_id"), false)
            || !valid_event(o.get("thread_root_id"), true)
            || !o.get("saved_at_ms").is_some_and(Value::is_u64)
        {
            return Err("invalid_content");
        }
    }
    Ok(())
}

pub(crate) fn read_payload(event_type: &str, content: Option<Value>) -> Value {
    match content {
        Some(content) => json!({
            "type": "account_pref", "pref_type": event_type,
            "present": true, "content": content,
        }),
        None => json!({
            "type": "account_pref", "pref_type": event_type,
            "present": false, "content": {},
        }),
    }
}

pub(crate) fn change_payload(event_type: &str) -> Value {
    json!({ "type": "account_pref_changed", "pref_type": event_type })
}

pub(crate) fn reads_from_server(event_type: &str) -> bool {
    event_type == INVITE_PERMISSION_TYPE
        || event_type == INVITE_PERMISSION_UNSTABLE_TYPE
        || event_type == MEDIA_PREVIEW_TYPE
        || event_type == SAVED_MESSAGES_TYPE
}

pub(crate) fn failed_payload(event_type: &str, category: &str) -> Value {
    json!({ "type": "account_pref_failed", "pref_type": event_type, "category": category })
}

fn fetched_payload(event_type: &str, raw: Option<&str>) -> Value {
    let Some(raw) = raw else { return read_payload(event_type, None) };
    match serde_json::from_str::<Value>(raw) {
        Ok(content) if content.is_object()
            && (event_type != SAVED_MESSAGES_TYPE
                || check_saved_content(&content).is_ok()) =>
            read_payload(event_type, Some(content)),
        _ => failed_payload(event_type, "invalid_content"),
    }
}

pub(crate) fn read(bridge: &RustClient, event_type: String) -> Result<(), String> {
    let event_type = allowed_type(&event_type).ok_or_else(|| "unsupported_type".to_owned())?;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let account = client.account();
        let fetched = if reads_from_server(event_type) {
            match tokio::time::timeout(
                REQUEST_TIMEOUT,
                crate::optprobe::read_global(&client, event_type, None, false),
            )
            .await
            {
                Ok(Ok(content)) => Ok(content),
                Ok(Err(category)) => Err(category),
                Err(_) => Err("timeout".to_owned()),
            }
        } else {
            Ok(account
                .account_data_raw(GlobalAccountDataEventType::from(event_type))
                .await
                .ok()
                .flatten()
                .and_then(|r| serde_json::from_str::<Value>(r.json().get()).ok()))
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let mut payload = match fetched {
            // A malformed saved list must never masquerade as absent: a
            // subsequent write would replace it with an empty baseline.
            Ok(content) => fetched_payload(event_type, content.map(|c| c.to_string()).as_deref()),
            Err(category) => failed_payload(event_type, &category),
        };
        payload["lifecycle"] = json!(lifecycle);
        enqueue(&events, payload);
    });
    Ok(())
}

pub(crate) fn write(
    bridge: &RustClient,
    event_type: String,
    content_json: String,
) -> Result<(), String> {
    let event_type = allowed_type(&event_type).ok_or_else(|| "unsupported_type".to_owned())?;
    let content = parse_content(&content_json).map_err(str::to_owned)?;
    if event_type == SAVED_MESSAGES_TYPE {
        check_saved_content(&content).map_err(str::to_owned)?;
    }
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let result = put(&client, event_type, &content).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "account_pref_written", "pref_type": event_type,
                "lifecycle": lifecycle, "ok": result.is_ok(),
                "category": result.err().unwrap_or_default(),
            }),
        );
    });
    Ok(())
}

async fn put(client: &Client, event_type: &str, content: &Value) -> Result<(), String> {
    let _turn = crate::optprobe::write_turn(client, event_type).await;
    crate::optprobe::guard_write(client, event_type).await?;
    crate::optprobe::note_write(client, event_type, Some(content));
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let raw = Raw::new(content).map_err(|_| "invalid_content".to_owned())?.cast_unchecked();
    let request = set_global_account_data::v3::Request::new_raw(
        own,
        GlobalAccountDataEventType::from(event_type),
        raw,
    );
    client
        .send(request)
        .with_request_config(RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT))
        .await
        .map(|_| ())
        .map_err(|err| classify_write_error(&err.to_string()).0.to_owned())
}

pub(crate) fn invite_policy_support_from(
    versions: &std::collections::BTreeSet<matrix_sdk::ruma::api::MatrixVersion>,
    features: &std::collections::BTreeSet<matrix_sdk::ruma::api::FeatureFlag>,
) -> (bool, bool) {
    let stable = versions
        .iter()
        .any(|v| *v >= matrix_sdk::ruma::api::MatrixVersion::V1_18);
    let unstable = features
        .contains(&matrix_sdk::ruma::api::FeatureFlag::from(INVITE_PERMISSION_FEATURE));
    (stable, unstable)
}

pub(crate) fn invite_policy_support(bridge: &RustClient) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let lifecycle = bridge.timelines.lifecycle();
    let timelines = Arc::clone(&bridge.timelines);
    bridge.spawn_room_action(async move {
        let answer = tokio::time::timeout(REQUEST_TIMEOUT, async {
            let versions = client.server_versions().await.ok()?;
            let features = client.unstable_features().await.ok()?;
            Some(invite_policy_support_from(&versions, &features))
        })
        .await
        .ok()
        .flatten();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, stable, unstable) = match answer {
            Some((stable, unstable)) => (true, stable, unstable),
            None => (false, false, false),
        };
        enqueue(
            &events,
            json!({
                "type": "invite_policy_support", "lifecycle": lifecycle,
                "ok": ok, "stable": stable, "unstable": unstable,
            }),
        );
    });
    Ok(())
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.media_preview_config", kind = GlobalAccountData)]
pub(crate) struct MediaPreviewConfigEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.recent_emoji", kind = GlobalAccountData)]
pub(crate) struct RecentEmojiEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.lightning_matrix.saved_messages", kind = GlobalAccountData)]
pub(crate) struct SavedMessagesEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "m.invite_permission_config", kind = GlobalAccountData)]
pub(crate) struct InvitePermissionEventContent {}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4380.invite_permission_config", kind = GlobalAccountData)]
pub(crate) struct UnstableInvitePermissionEventContent {}

pub(crate) fn install_change_handlers(
    client: &Client,
    events: EventQueueRef,
) -> Vec<EventHandlerHandle> {
    let media_events = Arc::clone(&events);
    let media = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<MediaPreviewConfigEventContent>| {
            let events = Arc::clone(&media_events);
            async move {
                enqueue(&events, change_payload(MEDIA_PREVIEW_TYPE));
            }
        },
    );
    let emoji_events = Arc::clone(&events);
    let emoji = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<RecentEmojiEventContent>| {
            let events = Arc::clone(&emoji_events);
            async move {
                enqueue(&events, change_payload(RECENT_EMOJI_TYPE));
            }
        },
    );
    let invite_events = Arc::clone(&events);
    let invite = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<InvitePermissionEventContent>| {
            let events = Arc::clone(&invite_events);
            async move {
                enqueue(&events, change_payload(INVITE_PERMISSION_TYPE));
            }
        },
    );
    let unstable_events = Arc::clone(&events);
    let unstable = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<UnstableInvitePermissionEventContent>| {
            let events = Arc::clone(&unstable_events);
            async move {
                enqueue(&events, change_payload(INVITE_PERMISSION_UNSTABLE_TYPE));
            }
        },
    );
    let saved_events = Arc::clone(&events);
    let saved = client.add_event_handler(
        move |_ev: GlobalAccountDataEvent<SavedMessagesEventContent>| {
            let events = Arc::clone(&saved_events);
            async move {
                enqueue(&events, change_payload(SAVED_MESSAGES_TYPE));
            }
        },
    );
    vec![media, emoji, invite, unstable, saved]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn saved_messages_account_data_rejects_plaintext_and_composites() {
        let reference = json!({"room_id":"!room:example.org", "event_id":"$reply",
            "thread_root_id":"$root", "saved_at_ms":42});
        assert_eq!(allowed_type(SAVED_MESSAGES_TYPE), Some(SAVED_MESSAGES_TYPE));
        assert!(reads_from_server(SAVED_MESSAGES_TYPE));
        assert!(check_saved_content(&json!({"version":1,"entries":[reference.clone()]})).is_ok());
        let mut with_body = reference.clone();
        with_body["body"] = json!("secret");
        assert!(check_saved_content(&json!({"version":1,"entries":[with_body]})).is_err());
        let mut composite = reference;
        composite["room_id"] = json!("!room:example.org\u{1f}thread\u{1f}$root");
        assert!(check_saved_content(&json!({"version":1,"entries":[composite]})).is_err());
    }

    #[test]
    fn malformed_fetched_saved_list_is_a_failure_not_absence() {
        let failed = fetched_payload(SAVED_MESSAGES_TYPE,
            Some(r#"{"version":1,"entries":[{"body":"private"}]}"#));
        assert_eq!(failed["type"], json!("account_pref_failed"));
        assert_eq!(failed["category"], json!("invalid_content"));
        assert_eq!(fetched_payload(SAVED_MESSAGES_TYPE, None)["present"], json!(false));
    }

    #[test]
    fn the_policy_types_are_read_from_the_server() {
        assert!(reads_from_server(INVITE_PERMISSION_TYPE));
        assert!(reads_from_server(INVITE_PERMISSION_UNSTABLE_TYPE));
        assert!(reads_from_server(MEDIA_PREVIEW_TYPE));
        assert!(!reads_from_server(RECENT_EMOJI_TYPE));
        let failed = failed_payload(INVITE_PERMISSION_TYPE, "timeout");
        assert_eq!(failed["type"], json!("account_pref_failed"));
        assert_eq!(failed["pref_type"], json!(INVITE_PERMISSION_TYPE));
    }

    #[test]
    fn the_invite_policy_types_cross_and_support_reads_versions() {
        assert_eq!(allowed_type(INVITE_PERMISSION_TYPE), Some(INVITE_PERMISSION_TYPE));
        assert_eq!(
            allowed_type(INVITE_PERMISSION_UNSTABLE_TYPE),
            Some(INVITE_PERMISSION_UNSTABLE_TYPE)
        );
        use matrix_sdk::ruma::api::{FeatureFlag, MatrixVersion};
        use std::collections::BTreeSet;
        let old: BTreeSet<MatrixVersion> = [MatrixVersion::V1_11].into_iter().collect();
        let new: BTreeSet<MatrixVersion> =
            [MatrixVersion::V1_11, MatrixVersion::V1_18].into_iter().collect();
        let none: BTreeSet<FeatureFlag> = BTreeSet::new();
        let msc: BTreeSet<FeatureFlag> =
            [FeatureFlag::from(INVITE_PERMISSION_FEATURE)].into_iter().collect();
        assert_eq!(invite_policy_support_from(&old, &none), (false, false));
        assert_eq!(invite_policy_support_from(&new, &none), (true, false));
        assert_eq!(invite_policy_support_from(&old, &msc), (false, true));
    }

    #[test]
    fn only_allowlisted_account_data_types_can_cross() {
        assert_eq!(allowed_type("m.media_preview_config"), Some("m.media_preview_config"));
        assert_eq!(allowed_type("m.recent_emoji"), Some("m.recent_emoji"));
        for other in [
            "m.direct", "m.push_rules", "m.secret_storage.default_key",
            "m.cross_signing.master", "org.lightning_matrix.backgrounds", "", "M.RECENT_EMOJI",
        ] {
            assert_eq!(allowed_type(other), None, "{other} must be refused");
        }
    }

    #[test]
    fn a_write_must_be_a_bounded_json_object() {
        assert!(parse_content(r#"{"invite_avatars":"off"}"#).is_ok());
        assert_eq!(parse_content("[1]"), Err("invalid_content"));
        assert_eq!(parse_content("\"x\""), Err("invalid_content"));
        assert_eq!(parse_content("not json"), Err("invalid_content"));
        let big = format!(r#"{{"a":"{}"}}"#, "x".repeat(MAX_CONTENT_BYTES));
        assert_eq!(parse_content(&big), Err("too_large"));
    }

    #[test]
    fn an_absent_preference_is_not_an_empty_one() {
        let absent = read_payload(MEDIA_PREVIEW_TYPE, None);
        assert_eq!(absent["present"], json!(false));
        let empty = read_payload(MEDIA_PREVIEW_TYPE, Some(json!({})));
        assert_eq!(empty["present"], json!(true));
        assert_eq!(empty["pref_type"], json!("m.media_preview_config"));
    }

    fn serve_existing_media_config() -> (String, Arc<std::sync::atomic::AtomicUsize>) {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let puts = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let seen = Arc::clone(&puts);
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                std::thread::spawn(move || {
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 4096];
                    while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
                        match stream.read(&mut chunk) {
                            Ok(0) | Err(_) => return,
                            Ok(n) => buf.extend_from_slice(&chunk[..n]),
                        }
                    }
                    let head = String::from_utf8_lossy(&buf).to_string();
                    let line = head.lines().next().unwrap_or("").to_owned();
                    let body = if line.contains("/versions") {
                        r#"{"versions":["v1.1","v1.11"]}"#
                    } else if line.starts_with("PUT") {
                        seen.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                        "{}"
                    } else if line.contains("/account_data/") {
                        std::thread::sleep(Duration::from_millis(300));
                        r#"{"media_previews":"off","org.other_client.flag":7}"#
                    } else {
                        "{}"
                    };
                    let _ = stream.write_all(
                        format!(
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\
                             Connection: close\r\nContent-Length: {}\r\n\r\n{body}",
                            body.len()
                        )
                        .as_bytes(),
                    );
                });
            }
        });
        (format!("http://{addr}"), puts)
    }

    #[tokio::test]
    async fn a_second_write_cannot_pass_while_the_first_verifies_an_inferred_absence() {
        use matrix_sdk::{
            authentication::matrix::MatrixSession,
            ruma::{api::client::sync::sync_events::v5, OwnedDeviceId, OwnedUserId},
            store::RoomLoadSettings,
            SessionMeta, SessionTokens,
        };
        let (homeserver, puts) = serve_existing_media_config();
        let client = Client::builder().homeserver_url(&homeserver).build().await.expect("client");
        client
            .matrix_auth()
            .restore_session(
                MatrixSession {
                    meta: SessionMeta {
                        user_id: OwnedUserId::try_from("@prefsrace:example.org").unwrap(),
                        device_id: OwnedDeviceId::from("TESTDEVICE"),
                    },
                    tokens: SessionTokens { access_token: "access-0".to_owned(), refresh_token: None },
                },
                RoomLoadSettings::default(),
            )
            .await
            .expect("restore");
        let mut response = v5::Response::new("pos-1".to_owned());
        response.extensions.account_data.global = vec![Raw::from_json_string(
            json!({ "type": "m.push_rules", "content": { "global": {
                "override": [], "content": [], "room": [], "sender": [], "underride": [] } } })
            .to_string(),
        )
        .unwrap()];
        client
            .process_sliding_sync_test_helper(&response, &matrix_sdk_base::RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        crate::optprobe::settle(&client).await;
        let read = crate::optprobe::read_global(&client, MEDIA_PREVIEW_TYPE, None, false).await;
        assert_eq!(read, Ok(None), "the store inferred absence");

        let (one, both) = (
            json!({"media_previews": "on"}),
            json!({"media_previews": "on", "invite_avatars": "off"}),
        );
        let (first, second) = tokio::join!(
            put(&client, MEDIA_PREVIEW_TYPE, &one),
            put(&client, MEDIA_PREVIEW_TYPE, &both),
        );
        crate::optprobe::forget_account_id("@prefsrace:example.org");
        assert_eq!(puts.load(std::sync::atomic::Ordering::SeqCst), 0, "a write replaced the server's copy");
        assert_eq!(first, Err("stale_absence".to_owned()));
        assert_eq!(second, Err("stale_absence".to_owned()));
    }

    #[test]
    fn a_change_names_only_the_type() {
        assert_eq!(
            change_payload(RECENT_EMOJI_TYPE),
            json!({"type": "account_pref_changed", "pref_type": "m.recent_emoji"})
        );
    }
}
