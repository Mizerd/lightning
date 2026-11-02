
use std::sync::Arc;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::state::get_state_event_for_key,
        events::StateEventType,
        OwnedUserId, UserId,
    },
};
use serde_json::json;

use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;

use crate::rooms::{classify_room_error, require_client, sniff_image_mime};
use crate::{enqueue, RustClient};

pub(crate) mod profile_field {
    use matrix_sdk::Client;

    pub(crate) struct Answer {
        pub status: u16,
        pub body: String,
    }

    fn endpoint(client: &Client, user_id: &str, field: &str) -> Result<String, String> {
        let mut url = client.homeserver();
        // Url::path_segments_mut percent-encodes, so a slash in a field name or
        // user id cannot escape the path.
        url.path_segments_mut()
            .map_err(|_| "homeserver url cannot carry a path".to_owned())?
            .pop_if_empty()
            .extend(["_matrix", "client", "v3", "profile", user_id, field]);
        Ok(url.to_string())
    }

    fn authorization(client: &Client) -> Result<String, String> {
        client
            .access_token()
            .map(|token| format!("Bearer {token}"))
            .ok_or_else(|| "no session".to_owned())
    }

    async fn run(
        client: &Client,
        request: reqwest::RequestBuilder,
    ) -> Result<Answer, String> {
        let response = request
            .header(reqwest::header::AUTHORIZATION, authorization(client)?)
            .send()
            .await
            .map_err(|err| err.to_string())?;
        let status = response.status().as_u16();
        let body = response.text().await.unwrap_or_default();
        Ok(Answer { status, body: body.chars().take(65_536).collect() })
    }

    pub(crate) async fn get(
        client: &Client,
        user_id: &str,
        field: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        run(client, client.http_client().get(url).timeout(timeout)).await
    }

    pub(crate) async fn set(
        client: &Client,
        user_id: &str,
        field: &str,
        value: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        let body = serde_json::to_vec(&serde_json::json!({ field: value }))
            .map_err(|_| "invalid_value".to_owned())?;
        run(
            client,
            client
                .http_client()
                .put(url)
                .timeout(timeout)
                .header(reqwest::header::CONTENT_TYPE, "application/json")
                .body(body),
        )
        .await
    }

    pub(crate) async fn set_json(
        client: &Client,
        user_id: &str,
        field: &str,
        value: &serde_json::Value,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        let body = serde_json::to_vec(&serde_json::json!({ field: value }))
            .map_err(|_| "invalid_value".to_owned())?;
        run(
            client,
            client
                .http_client()
                .put(url)
                .timeout(timeout)
                .header(reqwest::header::CONTENT_TYPE, "application/json")
                .body(body),
        )
        .await
    }

    pub(crate) async fn delete(
        client: &Client,
        user_id: &str,
        field: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        run(client, client.http_client().delete(url).timeout(timeout)).await
    }
}

const BANNER_FIELD: &str = "m.banner_url";
const BANNER_FIELD_UNSTABLE: &str = "chat.commet.profile_banner";

const BANNER_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

const MAX_BANNER_BYTES: u64 = 8 * 1024 * 1024;

/// Whether a profile field value is usable as a banner: `mxc://` only. An
/// http(s) URL would reveal the IP of everyone who opens the card to the
/// URL's owner; media goes through the authenticated media bridge.
pub(crate) fn is_usable_banner(value: &str) -> bool {
    value.starts_with("mxc://") && value.len() > "mxc://".len() && value.len() <= 512
}

pub(crate) fn resolve_banner(stable: Option<String>,
                             unstable: Option<String>) -> String {
    unstable.or(stable).unwrap_or_default()
}

fn banner_from_body(field: &str, body: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(body).ok()?;
    let text = value.get(field)?.as_str()?;
    is_usable_banner(text).then(|| text.to_owned())
}

pub(crate) fn is_unsupported(error: &str) -> bool {
    let lowered = error.to_ascii_lowercase();
    lowered.contains("m_unrecognized")
        || lowered.contains("unrecognized")
        || lowered.contains("unrecognised")
}

pub(crate) fn fetch_profile_banner(
    bridge: &RustClient,
    op_id: u64,
    user_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId = UserId::parse(user_id.as_str())
        .map_err(|_| "invalid user id".to_owned())?
        .into();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let (stable_answer, unstable_answer) = tokio::join!(
            profile_field::get(&client, uid.as_str(), BANNER_FIELD,
                               BANNER_REQUEST_TIMEOUT),
            profile_field::get(&client, uid.as_str(), BANNER_FIELD_UNSTABLE,
                               BANNER_REQUEST_TIMEOUT),
        );

        let mut stable_value: Option<String> = None;
        let mut unstable_value: Option<String> = None;
        let mut any_answered = false;
        let mut any_unrecognised = false;
        for (field, result) in [(BANNER_FIELD, stable_answer),
                                (BANNER_FIELD_UNSTABLE, unstable_answer)] {
            match result {
                Ok(answer) if answer.status == 200 => {
                    any_answered = true;
                    if let Some(value) = banner_from_body(field, &answer.body) {
                        if field == BANNER_FIELD {
                            stable_value = Some(value);
                        } else {
                            unstable_value = Some(value);
                        }
                    }
                }
                Ok(answer) => {
                    if is_unsupported(&answer.body) {
                        any_unrecognised = true;
                    } else {
                        any_answered = true;
                    }
                }
                // A transport failure says nothing about support; never latch unsupported.
                Err(_) => any_answered = true,
            }
        }
        if let (Some(s), Some(u)) = (stable_value.as_deref(),
                                     unstable_value.as_deref()) {
            if s != u {
                eprintln!(
                    "lightning: profile banner fields disagree for {uid}: \
                     m.banner_url={s} chat.commet.profile_banner={u} \
                     — taking the deployed key"
                );
            }
        }
        let banner = resolve_banner(stable_value, unstable_value);
        let supported = any_answered || !any_unrecognised;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "profile_banner",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "user_id": uid.to_string(),
            "mxc": banner,
            "supported": supported,
        }));
    });
    Ok(())
}

pub(crate) fn set_own_profile_banner(
    bridge: &RustClient,
    op_id: u64,
    local_path: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid = client
        .user_id()
        .map(ToOwned::to_owned)
        .ok_or_else(|| "no session".to_owned())?;
    let clearing = local_path.is_empty();
    if !clearing {
        let metadata = std::fs::metadata(&local_path)
            .map_err(|_| "banner file is not readable".to_owned())?;
        if !metadata.is_file() {
            return Err("banner path is not a regular file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_BANNER_BYTES {
            return Err("banner file size is out of range".to_owned());
        }
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = async {
            let mxc = if clearing {
                String::new()
            } else {
                let data = tokio::fs::read(&local_path)
                    .await
                    .map_err(|_| "read_failed".to_owned())?;
                let mime_str =
                    sniff_image_mime(&data).ok_or_else(|| "unsupported_image".to_owned())?;
                let mime: mime::Mime =
                    mime_str.parse().map_err(|_| "unsupported_image".to_owned())?;
                let upload = client
                    .media()
                    .upload(&mime, data, None)
                    .await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                upload.content_uri.to_string()
            };

            let mut last_error: Option<String> = None;
            let mut wrote_any = false;
            for field in [BANNER_FIELD, BANNER_FIELD_UNSTABLE] {
                let answer = if clearing {
                    profile_field::delete(&client, uid.as_str(), field,
                                          BANNER_REQUEST_TIMEOUT).await
                } else {
                    profile_field::set(&client, uid.as_str(), field, &mxc,
                                       BANNER_REQUEST_TIMEOUT).await
                };
                let outcome = match answer {
                    Ok(a) if (200..300).contains(&a.status) => Ok(()),
                    Ok(a) => Err(a.body),
                    Err(text) => Err(text),
                };
                match outcome {
                    Ok(()) => wrote_any = true,
                    Err(text) => last_error = Some(text),
                }
            }
            if wrote_any {
                Ok(mxc)
            } else {
                let text = last_error.unwrap_or_else(|| "unknown".to_owned());
                Err(if is_unsupported(&text) {
                    "unsupported".to_owned()
                } else {
                    classify_room_error(&text).to_owned()
                })
            }
        }
        .await;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(mxc) => enqueue(&events, json!({
                "type": "profile_banner_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": true,
                "mxc": mxc,
                "category": "",
            })),
            Err(category) => enqueue(&events, json!({
                "type": "profile_banner_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": false,
                "mxc": "",
                "category": category,
            })),
        }
    });
    Ok(())
}

const ROOM_BANNER_EVENT: &str = "page.codeberg.everypizza.room.banner";
const ROOM_BANNER_EVENT_LEGACY: &str = "org.lightning_matrix.room_banner";

/// Read one room's banner and whether this account may change it. Emits
/// `room_banner`. Sliding sync does not deliver custom state types, so a
/// store miss is normal; the homeserver read decides (404 means no banner).
pub(crate) fn fetch_room_banner(
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
        let mut banner = String::new();
        for event_type in [ROOM_BANNER_EVENT, ROOM_BANNER_EVENT_LEGACY] {
            if let Ok(Some(raw)) = room
                .get_state_event(StateEventType::from(event_type), "")
                .await
            {
                let json = match &raw {
                    RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                    RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
                };
                if let Some(url) = banner_url_from_json(&json) {
                    banner = url;
                    break;
                }
            }
            let config = RequestConfig::new()
                .disable_retry()
                .timeout(BANNER_REQUEST_TIMEOUT);
            let request = get_state_event_for_key::v3::Request::new(
                room.room_id().to_owned(),
                StateEventType::from(event_type),
                String::new(),
            );
            if let Ok(response) = client.send(request).with_request_config(config).await {
                if let Some(url) = banner_url_from_json(response.event_or_content.get()) {
                    banner = url;
                    break;
                }
            }
        }

        let can_set = match own_id {
            Some(own) => room
                .get_member_no_sync(&own)
                .await
                .ok()
                .flatten()
                .is_some_and(|m| {
                    m.can_send_state(StateEventType::from(ROOM_BANNER_EVENT))
                }),
            None => false,
        };

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_banner",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "mxc": banner,
            "can_set": can_set,
        }));
    });
    Ok(())
}

pub(crate) fn set_room_banner(
    bridge: &RustClient,
    op_id: u64,
    room_id: String,
    local_path: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = crate::rooms::joined_room(&client, &room_id)?;
    let clearing = local_path.is_empty();
    if !clearing {
        let metadata = std::fs::metadata(&local_path)
            .map_err(|_| "banner file is not readable".to_owned())?;
        if !metadata.is_file() {
            return Err("banner path is not a regular file".to_owned());
        }
        if metadata.len() == 0 || metadata.len() > MAX_BANNER_BYTES {
            return Err("banner file size is out of range".to_owned());
        }
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = async {
            let mxc = if clearing {
                String::new()
            } else {
                let data = tokio::fs::read(&local_path)
                    .await
                    .map_err(|_| "read_failed".to_owned())?;
                let mime_str =
                    sniff_image_mime(&data).ok_or_else(|| "unsupported_image".to_owned())?;
                let mime: mime::Mime =
                    mime_str.parse().map_err(|_| "unsupported_image".to_owned())?;
                let upload = client
                    .media()
                    .upload(&mime, data, None)
                    .await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                upload.content_uri.to_string()
            };
            let content = if mxc.is_empty() {
                json!({})
            } else {
                json!({ "url": mxc.clone() })
            };
            room.send_state_event_raw(ROOM_BANNER_EVENT, "", content)
                .await
                .map(|_| mxc)
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())
        }
        .await;

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(mxc) => enqueue(&events, json!({
                "type": "room_banner_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "ok": true,
                "mxc": mxc,
                "category": "",
            })),
            Err(category) => enqueue(&events, json!({
                "type": "room_banner_set",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "ok": false,
                "mxc": "",
                "category": category,
            })),
        }
    });
    Ok(())
}

fn banner_url_from_json(raw: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(raw).ok()?;
    let content = value.get("content").unwrap_or(&value);
    let url = content.get("url")?.as_str()?;
    is_usable_banner(url).then(|| url.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_mxc_uris_are_usable_banners() {
        assert!(is_usable_banner("mxc://example.org/abc"));
        assert!(!is_usable_banner("https://example.org/banner.png"));
        assert!(!is_usable_banner("http://example.org/banner.png"));
        assert!(!is_usable_banner("//example.org/banner.png"));
        assert!(!is_usable_banner("mxc://"));
        assert!(!is_usable_banner(""));
        assert!(!is_usable_banner(&format!("mxc://{}", "a".repeat(600))));
    }

    #[test]
    fn a_disagreement_between_the_two_names_resolves_to_the_deployed_one() {
        let stable = "mxc://example.org/old".to_owned();
        let unstable = "mxc://example.org/new".to_owned();

        assert_eq!(
            resolve_banner(Some(stable.clone()), Some(unstable.clone())),
            unstable,
            "the stable name won a disagreement, which is the defect: \
             Lightning writes BOTH names together, so only a client that \
             writes exactly one can make them differ — and the deployed key \
             is the one such clients write"
        );

        assert_eq!(resolve_banner(Some(stable.clone()), None), stable);
        assert_eq!(resolve_banner(None, Some(unstable.clone())), unstable);
        assert_eq!(resolve_banner(Some(stable.clone()), Some(stable.clone())),
                   stable);
        assert_eq!(resolve_banner(None, None), "");
    }

    #[test]
    fn the_room_banner_uses_the_name_other_clients_already_write() {
        assert_eq!(ROOM_BANNER_EVENT, "page.codeberg.everypizza.room.banner");
        assert_eq!(ROOM_BANNER_EVENT_LEGACY, "org.lightning_matrix.room_banner");
        assert_ne!(ROOM_BANNER_EVENT, ROOM_BANNER_EVENT_LEGACY);

        let content = r#"{"url":"mxc://example.org/banner"}"#;
        assert_eq!(
            banner_url_from_json(content).as_deref(),
            Some("mxc://example.org/banner")
        );
        let event = r#"{"type":"page.codeberg.everypizza.room.banner",
                        "content":{"url":"mxc://example.org/banner"}}"#;
        assert_eq!(
            banner_url_from_json(event).as_deref(),
            Some("mxc://example.org/banner")
        );
        let unsafe_url = r#"{"url":"https://example.org/banner.png"}"#;
        assert_eq!(banner_url_from_json(unsafe_url), None);
    }

    #[test]
    fn an_unrecognised_endpoint_is_unsupported_not_absent() {
        assert!(is_unsupported("M_UNRECOGNIZED: Unrecognized request"));
        assert!(!is_unsupported("[404 / M_NOT_FOUND] Profile field not found"));
        assert!(!is_unsupported("the server returned 404 Not Found"));
        assert!(!is_unsupported("M_FORBIDDEN: not allowed"));
        assert!(!is_unsupported("connection reset"));
    }
}
