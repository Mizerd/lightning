
use std::sync::Arc;

use matrix_sdk::{
    config::RequestConfig,
    deserialized_responses::RawAnySyncOrStrippedState,
    event_handler::EventHandlerHandle,
    ruma::{
        api::client::state::get_state_event_for_key,
        events::{macros::EventContent, EmptyStateKey, StateEventType},
    },
    Client, Room,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use crate::rooms::{require_client, sniff_image_mime};
use crate::{enqueue, EventQueueRef, RustClient};

pub(crate) const ROOM_BACKGROUND_EVENT: &str = "org.lightning_matrix.room.background";

pub(crate) const SCHEMA_VERSION: i64 = 1;

/// One state round trip. Runs on the room-action pool, which sign-out joins,
/// so no retry and a hard bound.
const BACKGROUND_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// Upload bound, checked before reading. Lightning's own uploads come out of
/// ImageCropper's "background" role (<= 2560 px, re-encoded), far below this.
pub(crate) const MAX_BACKGROUND_BYTES: u64 = 16 * 1024 * 1024;

const MAX_URL_LEN: usize = 512;

const DEFAULT_DIM: i64 = 20;
const DEFAULT_BLUR: i64 = 0;
const DEFAULT_TINT: i64 = 25;

const FITS: [&str; 3] = ["cover", "contain", "tile"];
const ALIGNS: [&str; 3] = ["center", "top", "bottom"];
const MIMES: [&str; 5] = ["image/jpeg", "image/png", "image/webp", "image/gif", "image/bmp"];

type WriteError = (&'static str, String, String);

#[derive(Debug, PartialEq)]
pub(crate) enum Parsed {
    None,
    UnsupportedVersion(i64),
    Background(Value),
}

/// Whether a value is usable as a background URL: `mxc://` only, bounded.
pub(crate) fn is_usable_url(value: &str) -> bool {
    value.starts_with("mxc://")
        && value.len() > "mxc://".len()
        && value.len() <= MAX_URL_LEN
        && !value.chars().any(|c| c.is_whitespace() || c.is_control())
}

fn is_hex_colour(value: &str) -> bool {
    value.len() == 7
        && value.starts_with('#')
        && value[1..].chars().all(|c| c.is_ascii_hexdigit())
}

fn percent(value: Option<&Value>, default: i64) -> i64 {
    let Some(value) = value else {
        return default;
    };
    if let Some(n) = value.as_i64() {
        return n.clamp(0, 100);
    }
    if value.as_u64().is_some() {
        return 100;
    }
    match value.as_f64() {
        Some(v) if v.is_finite() => (v.round() as i64).clamp(0, 100),
        _ => default,
    }
}

pub(crate) fn is_canonical_json_safe(value: &Value) -> bool {
    const MAX_SAFE: i64 = (1 << 53) - 1;
    match value {
        Value::Number(n) => match (n.as_i64(), n.as_u64()) {
            (Some(i), _) => (-MAX_SAFE..=MAX_SAFE).contains(&i),
            (None, Some(u)) => u <= MAX_SAFE as u64,
            _ => false,
        },
        Value::Array(items) => items.iter().all(is_canonical_json_safe),
        Value::Object(map) => map.values().all(is_canonical_json_safe),
        _ => true,
    }
}

pub(crate) fn classify_write_error(text: &str) -> (&'static str, String) {
    let status: Option<u16> = text
        .match_indices('[')
        .filter_map(|(at, _)| text.get(at + 1..at + 4))
        .find_map(|digits| digits.parse::<u16>().ok().filter(|n| (100..600).contains(n)));
    let errcode: Option<String> = text.find("M_").map(|at| {
        text[at..]
            .chars()
            .take_while(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || *c == '_')
            .take(40)
            .collect()
    });
    let lc = text.to_ascii_lowercase();
    let code = errcode.as_deref().unwrap_or("");
    let category = if code == "M_BAD_JSON" || code == "M_NOT_JSON" {
        "bad_json"
    } else if code == "M_TOO_LARGE" || status == Some(413) {
        "upload_too_large"
    } else if code == "M_LIMIT_EXCEEDED" || status == Some(429) {
        "rate_limited"
    } else if code == "M_FORBIDDEN" || status == Some(403) {
        "forbidden"
    } else if code == "M_UNKNOWN_TOKEN" || code == "M_MISSING_TOKEN" || status == Some(401) {
        "signed_out"
    } else if status.is_some_and(|s| s >= 500) {
        "server_error"
    } else if status.is_some() || !code.is_empty() {
        "server_refused"
    } else if lc.contains("timed out") || lc.contains("timeout") {
        "timeout"
    } else {
        "network"
    };
    let detail = match (status, errcode) {
        (Some(s), Some(c)) => format!("status={s} errcode={c}"),
        (Some(s), None) => format!("status={s}"),
        (None, Some(c)) => format!("errcode={c}"),
        (None, None) => format!("transport={category}"),
    };
    (category, detail)
}

fn choice(value: Option<&Value>, allowed: &[&'static str], default: &'static str) -> &'static str {
    value
        .and_then(Value::as_str)
        .and_then(|s| allowed.iter().copied().find(|a| *a == s))
        .unwrap_or(default)
}

fn bounded_count(value: Option<&Value>) -> Option<u64> {
    let n = value?.as_u64()?;
    (n <= i32::MAX as u64).then_some(n)
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, EventContent)]
#[ruma_event(
    type = "org.lightning_matrix.room.background",
    kind = State,
    state_key_type = EmptyStateKey
)]
pub(crate) struct RoomBackgroundEventContent {}

pub(crate) fn change_payload(room_id: &str) -> Value {
    json!({ "type": "room_background_changed", "room_id": room_id })
}

pub(crate) fn install_change_handler(client: &Client, events: EventQueueRef) -> EventHandlerHandle {
    client.add_event_handler(move |_ev: SyncRoomBackgroundEvent, room: Room| {
        let events = Arc::clone(&events);
        async move {
            enqueue(&events, change_payload(room.room_id().as_str()));
        }
    })
}

pub(crate) fn parse_content(raw: &Value) -> Parsed {
    let content = match raw.get("content") {
        Some(inner) if inner.is_object() && raw.get("type").is_some() => inner,
        _ => raw,
    };
    let Some(object) = content.as_object() else {
        return Parsed::None;
    };
    let version = match object.get("version") {
        None => SCHEMA_VERSION,
        Some(v) => match v.as_i64() {
            Some(n) if n >= 1 => n,
            _ => return Parsed::None,
        },
    };
    let Some(url) = object.get("url").and_then(Value::as_str) else {
        return Parsed::None;
    };
    if !is_usable_url(url) {
        return Parsed::None;
    }
    if version > SCHEMA_VERSION {
        return Parsed::UnsupportedVersion(version);
    }

    let mut out = Map::new();
    out.insert("version".into(), json!(SCHEMA_VERSION));
    out.insert("url".into(), json!(url));
    normalise_picture_fields(object, &mut out);
    Parsed::Background(Value::Object(out))
}

pub(crate) fn normalise_picture_fields(object: &Map<String, Value>, out: &mut Map<String, Value>) {
    if let Some(info) = object.get("info").and_then(Value::as_object) {
        let mut clean = Map::new();
        if let Some(mime) = info.get("mimetype").and_then(Value::as_str) {
            if MIMES.contains(&mime) {
                clean.insert("mimetype".into(), json!(mime));
            }
        }
        for key in ["size", "w", "h"] {
            if let Some(n) = bounded_count(info.get(key)) {
                clean.insert(key.into(), json!(n));
            }
        }
        if !clean.is_empty() {
            out.insert("info".into(), Value::Object(clean));
        }
    }

    if let Some(colour) = object.get("color").and_then(Value::as_str) {
        if is_hex_colour(colour) {
            out.insert("color".into(), json!(colour.to_ascii_uppercase()));
        }
    }

    let presentation = object.get("presentation").and_then(Value::as_object);
    let field = |key: &str| presentation.and_then(|p| p.get(key));
    out.insert(
        "presentation".into(),
        json!({
            "dim": percent(field("dim"), DEFAULT_DIM),
            "blur": percent(field("blur"), DEFAULT_BLUR),
            "tint": percent(field("tint"), DEFAULT_TINT),
            "fit": choice(field("fit"), &FITS, "cover"),
            "align": choice(field("align"), &ALIGNS, "center"),
        }),
    );
}

pub(crate) fn compose_content(requested: &Value, url: &str) -> Option<Value> {
    let mut candidate = match requested.as_object() {
        Some(object) => Value::Object(object.clone()),
        None => json!({}),
    };
    candidate["url"] = json!(url);
    candidate["version"] = json!(SCHEMA_VERSION);
    match parse_content(&candidate) {
        Parsed::Background(clean) if is_canonical_json_safe(&clean) => Some(clean),
        _ => None,
    }
}

fn answer_fields(parsed: &Parsed) -> (Value, i64) {
    match parsed {
        Parsed::Background(content) => (content.clone(), 0),
        Parsed::UnsupportedVersion(version) => (Value::Null, *version),
        Parsed::None => (Value::Null, 0),
    }
}

pub(crate) fn fetch_room_background(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = crate::rooms::joined_room(&client, &room_id)?;
    let own_id = client.user_id().map(ToOwned::to_owned);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(BACKGROUND_REQUEST_TIMEOUT);
        let request = get_state_event_for_key::v3::Request::new(
            room.room_id().to_owned(),
            StateEventType::from(ROOM_BACKGROUND_EVENT),
            String::new(),
        );
        let parsed = match client.send(request).with_request_config(config).await {
            Ok(response) => serde_json::from_str::<Value>(response.event_or_content.get())
                .map(|v| parse_content(&v))
                .unwrap_or(Parsed::None),
            Err(err) => {
                let text = err.to_string().to_ascii_lowercase();
                if text.contains("m_not_found") || text.contains("404") {
                    Parsed::None
                } else {
                    let stored = room
                        .get_state_event(StateEventType::from(ROOM_BACKGROUND_EVENT), "")
                        .await
                        .ok()
                        .flatten();
                    match stored {
                        Some(raw) => {
                            let json = match &raw {
                                RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                                RawAnySyncOrStrippedState::Stripped(ev) => {
                                    ev.json().get().to_owned()
                                }
                            };
                            serde_json::from_str::<Value>(&json)
                                .map(|v| parse_content(&v))
                                .unwrap_or(Parsed::None)
                        }
                        None => Parsed::None,
                    }
                }
            }
        };

        let can_set = match own_id {
            Some(own) => room
                .get_member_no_sync(&own)
                .await
                .ok()
                .flatten()
                .is_some_and(|m| m.can_send_state(StateEventType::from(ROOM_BACKGROUND_EVENT))),
            None => false,
        };

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (content, newer_version) = answer_fields(&parsed);
        enqueue(
            &events,
            json!({
                "type": "room_background",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "content": content,
                "can_set": can_set,
                "unsupported_version": newer_version > 0,
                "schema_version": newer_version,
            }),
        );
    });
    Ok(())
}

pub(crate) fn set_room_background(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    local_path: String,
    requested_json: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = crate::rooms::joined_room(&client, &room_id)?;
    let requested: Value = if requested_json.trim().is_empty() {
        json!({})
    } else {
        serde_json::from_str(&requested_json).map_err(|_| "invalid_content".to_owned())?
    };
    let reuse_url = requested
        .get("url")
        .and_then(Value::as_str)
        .filter(|u| is_usable_url(u))
        .map(ToOwned::to_owned);
    let uploading = !local_path.is_empty();
    let clearing = !uploading && reuse_url.is_none();
    if uploading {
        let metadata = std::fs::metadata(&local_path).map_err(|_| "read_failed".to_owned())?;
        if !metadata.is_file() {
            return Err("not_a_file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_BACKGROUND_BYTES {
            return Err("file_too_large".to_owned());
        }
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result: Result<Value, WriteError> = async {
            let failed = |stage: &'static str, err: String| {
                let (category, detail) = classify_write_error(&err);
                (stage, category.to_owned(), detail)
            };
            if clearing {
                room.send_state_event_raw(ROOM_BACKGROUND_EVENT, "", json!({}))
                    .await
                    .map_err(|err| failed("send_state", err.to_string()))?;
                return Ok::<Value, WriteError>(Value::Null);
            }
            let url = if uploading {
                let data = tokio::fs::read(&local_path).await.map_err(|_| {
                    ("read", "read_failed".to_owned(), "io".to_owned())
                })?;
                let mime_str = sniff_image_mime(&data).ok_or_else(|| {
                    ("sniff", "unsupported_image".to_owned(), "magic".to_owned())
                })?;
                let mime: mime::Mime = mime_str.parse().map_err(|_| {
                    ("sniff", "unsupported_image".to_owned(), "mime".to_owned())
                })?;
                let upload = client
                    .media()
                    .upload(&mime, data, None)
                    .await
                    .map_err(|err| failed("upload", err.to_string()))?;
                upload.content_uri.to_string()
            } else {
                reuse_url.clone().unwrap_or_default()
            };
            let content = compose_content(&requested, &url).ok_or_else(|| {
                ("compose", "invalid_content".to_owned(), "schema".to_owned())
            })?;
            room.send_state_event_raw(ROOM_BACKGROUND_EVENT, "", content.clone())
                .await
                .map_err(|err| failed("send_state", err.to_string()))?;
            Ok::<Value, WriteError>(content)
        }
        .await;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(content) => enqueue(
                &events,
                json!({
                    "type": "room_background_set",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": true,
                    "content": content,
                    "category": "",
                    "stage": "done",
                    "detail": "",
                }),
            ),
            Err((stage, category, detail)) => enqueue(
                &events,
                json!({
                    "type": "room_background_set",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": false,
                    "content": Value::Null,
                    "category": category,
                    "stage": stage,
                    "detail": detail,
                }),
            ),
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn background(value: Value) -> Value {
        match parse_content(&value) {
            Parsed::Background(v) => v,
            other => panic!("expected a background, got {other:?} for {value}"),
        }
    }

    #[test]
    fn the_event_type_is_namespaced_and_stable() {
        // Other Lightning builds read exactly this name; renaming it would make
        // every shared background vanish.
        assert_eq!(ROOM_BACKGROUND_EVENT, "org.lightning_matrix.room.background");
        assert_eq!(SCHEMA_VERSION, 1);
        use matrix_sdk::ruma::events::StaticEventContent;
        assert_eq!(RoomBackgroundEventContent::TYPE, ROOM_BACKGROUND_EVENT);
    }

    #[test]
    fn only_mxc_urls_are_usable() {
        assert!(is_usable_url("mxc://example.org/abc"));
        assert!(!is_usable_url("https://example.org/bg.png"));
        assert!(!is_usable_url("http://example.org/bg.png"));
        assert!(!is_usable_url("mxc://"));
        assert!(!is_usable_url(""));
        assert!(!is_usable_url("mxc://example.org/a b"));
        assert!(!is_usable_url(&format!("mxc://{}", "a".repeat(600))));
        assert_eq!(parse_content(&json!({"url": "https://evil.example/x.png"})), Parsed::None);
    }

    #[test]
    fn an_empty_object_clears_and_is_not_an_error() {
        assert_eq!(parse_content(&json!({})), Parsed::None);
        assert_eq!(parse_content(&json!(null)), Parsed::None);
        assert_eq!(parse_content(&json!("mxc://example.org/abc")), Parsed::None);
        assert_eq!(parse_content(&json!({"version": 1})), Parsed::None);
    }

    #[test]
    fn a_minimal_event_gets_every_presentation_default() {
        let v = background(json!({"url": "mxc://example.org/abc"}));
        assert_eq!(v["version"], json!(1));
        assert_eq!(v["url"], json!("mxc://example.org/abc"));
        assert_eq!(v["presentation"]["dim"], json!(20));
        assert_eq!(v["presentation"]["blur"], json!(0));
        assert_eq!(v["presentation"]["tint"], json!(25));
        assert!(is_canonical_json_safe(&v));
        assert_eq!(v["presentation"]["fit"], json!("cover"));
        assert_eq!(v["presentation"]["align"], json!("center"));
        assert!(v.get("info").is_none());
        assert!(v.get("color").is_none());
    }

    #[test]
    fn hostile_values_are_clamped_or_dropped_never_passed_through() {
        let v = background(json!({
            "url": "mxc://example.org/abc",
            "info": { "mimetype": "image/svg+xml", "size": -4, "w": 1e12, "h": "tall" },
            "color": "red; background: url(x)",
            "presentation": { "dim": 750, "blur": -1, "tint": "lots",
                              "fit": "stretch", "align": "<b>left</b>" },
            "script": "alert(1)"
        }));
        assert!(v.get("info").is_none(), "{v}");
        assert!(v.get("color").is_none());
        assert_eq!(v["presentation"]["dim"], json!(100));
        assert_eq!(v["presentation"]["blur"], json!(0));
        assert_eq!(v["presentation"]["tint"], json!(25));
        assert_eq!(v["presentation"]["fit"], json!("cover"));
        assert_eq!(v["presentation"]["align"], json!("center"));
        assert!(v.get("script").is_none());
    }

    #[test]
    fn known_good_values_round_trip() {
        let v = background(json!({
            "version": 1,
            "url": "mxc://example.org/abc",
            "info": { "mimetype": "image/jpeg", "size": 412345, "w": 2560, "h": 1440 },
            "color": "#2b3a4f",
            "presentation": { "dim": 40, "blur": 50, "tint": 10,
                              "fit": "tile", "align": "top" },
            "future_field": { "anything": true }
        }));
        assert_eq!(v["info"]["mimetype"], json!("image/jpeg"));
        assert_eq!(v["info"]["w"], json!(2560));
        assert_eq!(v["color"], json!("#2B3A4F"));
        assert_eq!(v["presentation"]["fit"], json!("tile"));
        assert_eq!(v["presentation"]["align"], json!("top"));
        assert_eq!(v["presentation"]["blur"], json!(50));
        assert_eq!(background(v.clone()), v);
    }

    #[test]
    fn a_full_store_event_reads_its_content() {
        let v = background(json!({
            "type": ROOM_BACKGROUND_EVENT,
            "state_key": "",
            "content": { "url": "mxc://example.org/abc" }
        }));
        assert_eq!(v["url"], json!("mxc://example.org/abc"));
    }

    #[test]
    fn a_newer_schema_is_reported_not_guessed_at() {
        assert_eq!(
            parse_content(&json!({"version": 2, "url": "mxc://example.org/abc"})),
            Parsed::UnsupportedVersion(2)
        );
        let (content, newer) =
            answer_fields(&parse_content(&json!({"version": 3, "url": "mxc://e.org/a"})));
        assert!(content.is_null());
        assert_eq!(newer, 3);
        let (_, none) = answer_fields(&parse_content(&json!({"url": "mxc://e.org/a"})));
        assert_eq!(none, 0);
        assert_eq!(parse_content(&json!({"version": 0, "url": "mxc://e.org/a"})), Parsed::None);
        assert_eq!(parse_content(&json!({"version": "1", "url": "mxc://e.org/a"})), Parsed::None);
        assert_eq!(parse_content(&json!({"version": 1.5, "url": "mxc://e.org/a"})), Parsed::None);
    }

    #[test]
    fn composing_uses_the_read_normaliser() {
        let requested = json!({
            "url": "mxc://ignored.example/old",
            "presentation": { "dim": 200, "fit": "contain" },
            "color": "#ABCDEF",
        });
        let sent = compose_content(&requested, "mxc://example.org/new").expect("content");
        assert_eq!(sent["url"], json!("mxc://example.org/new"));
        assert_eq!(sent["version"], json!(1));
        assert_eq!(sent["presentation"]["dim"], json!(100));
        assert_eq!(sent["presentation"]["fit"], json!("contain"));
        assert_eq!(background(sent.clone()), sent);
        assert!(compose_content(&requested, "https://example.org/x.png").is_none());
        assert!(compose_content(&json!(42), "mxc://example.org/new").is_some());
    }

    #[test]
    fn what_we_send_is_canonical_json_with_no_float_anywhere() {
        let requested = json!({
            "version": 1,
            "presentation": { "dim": 0.2, "blur": 0.0, "tint": 0.25 },
            "info": { "mimetype": "image/jpeg", "size": 784600, "w": 2560, "h": 1440 },
            "color": "#102030",
        });
        let sent = compose_content(&requested, "mxc://example.org/new").expect("content");
        assert!(is_canonical_json_safe(&sent), "a float would be refused: {sent}");
        for key in ["dim", "blur", "tint"] {
            assert!(sent["presentation"][key].is_i64(), "{key} is not an integer: {sent}");
        }
        assert!(!is_canonical_json_safe(&json!({"a": [1, {"b": 0.5}]})));
        assert!(!is_canonical_json_safe(&json!(9_007_199_254_740_993_u64)));
        assert!(is_canonical_json_safe(&json!({"a": [1, -2, "x", null, true]})));
    }

    #[tokio::test]
    async fn a_background_change_in_sync_reaches_cpp_with_its_room() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;
        use std::collections::VecDeque;
        use std::sync::Mutex;

        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let events: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let _handle = install_change_handler(&client, Arc::clone(&events));

        let room = room_id!("!open:example.org");
        let state_event = |event_type: &str, id: &str, content: Value| {
            Raw::from_json_string(
                json!({
                    "type": event_type,
                    "state_key": "",
                    "event_id": id,
                    "sender": "@other:example.org",
                    "origin_server_ts": 1_700_000_000_000_u64,
                    "content": content,
                })
                .to_string(),
            )
            .expect("raw event")
        };
        let mut update = v5::response::Room::default();
        update.timeline = vec![
            state_event("m.room.topic", "$topic", json!({ "topic": "t" })),
            state_event(
                ROOM_BACKGROUND_EVENT,
                "$bg",
                json!({ "version": 1, "url": "mxc://example.org/abc" }),
            ),
            state_event(ROOM_BACKGROUND_EVENT, "$clear", json!({})),
        ];
        let mut response = v5::Response::new("pos-1".to_owned());
        response.rooms.insert(room.to_owned(), update);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");

        let queued: Vec<Value> = events
            .lock()
            .unwrap()
            .iter()
            .map(|s| serde_json::from_str(s).unwrap())
            .collect();
        let expected = change_payload(room.as_str());
        assert_eq!(queued, vec![expected.clone(), expected], "{queued:?}");
        assert_eq!(queued[0].as_object().map(|o| o.len()), Some(2));
    }

    #[test]
    fn write_errors_get_a_category_and_a_detail_without_server_text() {
        let (category, detail) = classify_write_error(
            "the server returned an error: [400 / M_BAD_JSON] Bad JSON value: float",
        );
        assert_eq!(category, "bad_json");
        assert_eq!(detail, "status=400 errcode=M_BAD_JSON");
        assert!(!detail.contains("float"), "server text must not reach the log");
        assert_eq!(classify_write_error("[413 / M_TOO_LARGE] too big").0, "upload_too_large");
        assert_eq!(classify_write_error("[403 / M_FORBIDDEN] no").0, "forbidden");
        assert_eq!(classify_write_error("[429 / M_LIMIT_EXCEEDED] slow").0, "rate_limited");
        assert_eq!(classify_write_error("[401 / M_UNKNOWN_TOKEN] gone").0, "signed_out");
        assert_eq!(classify_write_error("[502 / M_UNKNOWN] bad gateway").0, "server_error");
        assert_eq!(classify_write_error("[400 / M_UNKNOWN] what").0, "server_refused");
        assert_eq!(classify_write_error("operation timed out").0, "timeout");
        let (category, detail) = classify_write_error("error sending request: connect refused");
        assert_eq!(category, "network");
        assert_eq!(detail, "transport=network");
    }
}
