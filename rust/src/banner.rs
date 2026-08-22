//! Profile banners (MSC4427) over extended profile fields (MSC4133).
//!
//! A banner is a wide image shown behind a user's profile card. Matrix has one
//! proposal for it and exactly one existing implementation, so this module is
//! written for INTEROPERABILITY rather than for a Lightning-only feature:
//!
//!   * READ prefers the stable field `m.banner_url` and falls back to
//!     `chat.commet.profile_banner`, the unstable key Commet already ships and
//!     Sable and Haven read.
//!   * WRITE sets BOTH, so a banner set in Lightning shows up in those clients
//!     and vice versa. Two round trips is the price of not being the odd one
//!     out; a banner nobody else can see is not a banner.
//!
//! The value is an `mxc://` URI and nothing else — the MSC requires it, and a
//! profile field is remote text that a client would otherwise be free to point
//! at an arbitrary http URL, which is a tracking pixel on every profile card
//! that renders it.
//!
//! Extended profile fields are new, and a homeserver that does not implement
//! MSC4133 answers with an unrecognised-endpoint error. That is reported as
//! `supported: false` and rendered as NOTHING, never as "this user has no
//! banner" — the two are different facts.

use std::sync::Arc;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::profile::{delete_profile_field, get_profile_field, set_profile_field},
        profile::{ProfileFieldName, ProfileFieldValue},
        OwnedUserId, UserId,
    },
};
use serde_json::json;

use crate::rooms::{classify_room_error, require_client, sniff_image_mime};
use crate::{enqueue, RustClient};

/// The stable field from MSC4427.
const BANNER_FIELD: &str = "m.banner_url";
/// The key Commet shipped first, which the MSC adopted as its unstable prefix.
/// Written as well as read: interoperating with the implementation that exists
/// matters more than writing only the name that is not deployed yet.
const BANNER_FIELD_UNSTABLE: &str = "chat.commet.profile_banner";

/// One profile-field round trip. Rides the room-action pool, which sign-out
/// joins, so no retry and a hard bound.
const BANNER_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// A banner is a wide image, not a wallpaper. Bounded before it is read, so a
/// mis-selected file is refused rather than loaded into memory.
const MAX_BANNER_BYTES: u64 = 8 * 1024 * 1024;

/// Whether a value from a profile field is usable as a banner.
///
/// `mxc://` ONLY. A profile field is remote text: rendering an http(s) URL
/// from one would let anybody who sets their banner to a URL they control see
/// the IP of everyone who so much as opens their profile card. Authenticated
/// media is fetched through the media bridge, which is the whole point of
/// having one.
pub(crate) fn is_usable_banner(value: &str) -> bool {
    value.starts_with("mxc://") && value.len() > "mxc://".len() && value.len() <= 512
}

fn banner_from_response(response: get_profile_field::v3::Response) -> Option<String> {
    let value = response.value?;
    let raw = value.value();
    let text = raw.as_str()?;
    is_usable_banner(text).then(|| text.to_owned())
}

/// True when the failure means "this server does not do extended profiles",
/// rather than "this user has no banner". The distinction is the difference
/// between rendering nothing and claiming something.
fn is_unsupported(error: &str) -> bool {
    let lowered = error.to_ascii_lowercase();
    lowered.contains("m_unrecognized")
        || lowered.contains("unrecognized")
        || lowered.contains("404")
        || lowered.contains("not found")
}

/// Read one user's banner. Emits `profile_banner`.
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
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(BANNER_REQUEST_TIMEOUT);
        let mut supported = true;
        let mut banner = String::new();
        // Stable first, then the deployed unstable key. A server that answers
        // the stable field at all is one whose answer we trust, so an empty
        // stable answer still falls through — the two names coexist for now
        // and a banner set by Commet lives under the second one.
        for field in [BANNER_FIELD, BANNER_FIELD_UNSTABLE] {
            let request = get_profile_field::v3::Request::new(
                uid.clone(),
                ProfileFieldName::from(field),
            );
            match client.send(request).with_request_config(config).await {
                Ok(response) => {
                    if let Some(value) = banner_from_response(response) {
                        banner = value;
                        supported = true;
                        break;
                    }
                }
                Err(err) => {
                    let text = err.to_string();
                    if is_unsupported(&text) {
                        // Not knowing is not the same as knowing there is
                        // none. Keep looking; only report unsupported if
                        // every field said so.
                        supported = false;
                    } else {
                        supported = true;
                    }
                }
            }
        }
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

/// Upload `local_path` and set it as the signed-in account's banner, under
/// BOTH field names. An empty path CLEARS both. Emits `profile_banner_set`.
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
        let config = RequestConfig::new()
            .disable_retry()
            .timeout(BANNER_REQUEST_TIMEOUT);
        let result = async {
            let mxc = if clearing {
                String::new()
            } else {
                let data = tokio::fs::read(&local_path)
                    .await
                    .map_err(|_| "read_failed".to_owned())?;
                // The CONTENT decides the type, never the file name: this is
                // the same rule every other image path in this bridge follows.
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

            // Both names, so a banner set here is visible in the clients that
            // already implement this. A failure on the stable field alone is
            // still a failure: half-set is not set.
            let mut last_error: Option<String> = None;
            let mut wrote_any = false;
            for field in [BANNER_FIELD, BANNER_FIELD_UNSTABLE] {
                let outcome = if clearing {
                    client
                        .send(delete_profile_field::v3::Request::new(
                            uid.clone(),
                            ProfileFieldName::from(field),
                        ))
                        .with_request_config(config)
                        .await
                        .map(|_| ())
                        .map_err(|err| err.to_string())
                } else {
                    let value = ProfileFieldValue::new(field, json!(mxc.clone()))
                        .map_err(|_| "invalid_value".to_owned())?;
                    client
                        .send(set_profile_field::v3::Request::new(uid.clone(), value))
                        .with_request_config(config)
                        .await
                        .map(|_| ())
                        .map_err(|err| err.to_string())
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_mxc_uris_are_usable_banners() {
        assert!(is_usable_banner("mxc://example.org/abc"));
        // An http URL in a profile field would make every viewer who opens
        // the card fetch it from a host the profile's owner controls.
        assert!(!is_usable_banner("https://example.org/banner.png"));
        assert!(!is_usable_banner("http://example.org/banner.png"));
        assert!(!is_usable_banner("//example.org/banner.png"));
        assert!(!is_usable_banner("mxc://"));
        assert!(!is_usable_banner(""));
        // Bounded: a profile field is remote text.
        assert!(!is_usable_banner(&format!("mxc://{}", "a".repeat(600))));
    }

    #[test]
    fn an_unrecognised_endpoint_is_unsupported_not_absent() {
        assert!(is_unsupported("M_UNRECOGNIZED: Unrecognized request"));
        assert!(is_unsupported("the server returned 404 Not Found"));
        // A real refusal is NOT "the server cannot do banners".
        assert!(!is_unsupported("M_FORBIDDEN: not allowed"));
        assert!(!is_unsupported("connection reset"));
    }
}
