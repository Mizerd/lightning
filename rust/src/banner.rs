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
        api::client::state::get_state_event_for_key,
        events::StateEventType,
        OwnedUserId, UserId,
    },
};
use serde_json::json;

use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;

use crate::rooms::{classify_room_error, require_client, sniff_image_mime};
use crate::{enqueue, RustClient};

/// The extended-profile field endpoints, addressed directly.
///
/// Lightning used ruma's typed `get/set/delete_profile_field` until it turned
/// out they cannot reach a homeserver that actually implements the feature.
///
/// MSC4133's stable path is `/_matrix/client/v3/profile/{userId}/{keyName}`,
/// and ruma selects it only when the server advertises **spec version 1.16**
/// (`EXTENDED_PROFILE_FIELD_HISTORY`, ruma-client-api 0.24), or the unstable
/// feature `uk.tcpip.msc4133` for the unstable path. Synapse 1.156 signals a
/// stabilised MSC the way Synapse always does — `uk.tcpip.msc4133.stable:
/// true` in `unstable_features` — and its `versions` list stops at v1.12.
/// Neither of ruma's gates is met, so the typed request cannot select the
/// working path; every read came back M_UNRECOGNIZED and the client reported
/// "your homeserver does not support profile banners" about a homeserver that
/// does. ruma has the mechanism for this (`StablePathSelector::Feature`); it
/// is simply not wired to that endpoint.
///
/// So these three calls address the stable path themselves, over the SDK's OWN
/// configured transport (`Client::http_client()` — same TLS, proxy and
/// timeouts as every other request; nothing new is constructed). The ANSWER
/// then decides what the server supports, which is what `is_unsupported` was
/// always for: a server without extended profiles replies M_UNRECOGNIZED and
/// is reported exactly as before. Nothing is concluded from a version number
/// in either direction.
///
/// The access token is read from the SDK, used for one request, and never
/// logged, stored, or returned. Revisit when ruma wires the stable feature in:
/// this becomes a straight swap back to `ruma::api::client::profile`.
mod profile_field {
    use matrix_sdk::Client;

    /// Coarse outcome. The BODY of a successful read is the caller's problem;
    /// what matters here is telling "the server answered" apart from "the
    /// server does not know this endpoint".
    pub(super) struct Answer {
        pub status: u16,
        pub body: String,
    }

    fn endpoint(client: &Client, user_id: &str, field: &str) -> Result<String, String> {
        let mut url = client.homeserver();
        // Percent-encoding is done by Url::path_segments_mut, so a field name
        // or user id containing a slash cannot escape the path.
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
        // Bounded: a profile field response is a small JSON object, and this
        // is remote input. 64 KiB is far more than any field may hold.
        let body = response.text().await.unwrap_or_default();
        Ok(Answer { status, body: body.chars().take(65_536).collect() })
    }

    pub(super) async fn get(
        client: &Client,
        user_id: &str,
        field: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        run(client, client.http_client().get(url).timeout(timeout)).await
    }

    pub(super) async fn set(
        client: &Client,
        user_id: &str,
        field: &str,
        value: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        // Built with serde rather than string formatting so a field name or
        // value can never inject JSON, and sent as an explicit body because
        // reqwest's `json` helper needs a feature this build does not enable.
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

    pub(super) async fn delete(
        client: &Client,
        user_id: &str,
        field: &str,
        timeout: std::time::Duration,
    ) -> Result<Answer, String> {
        let url = endpoint(client, user_id, field)?;
        run(client, client.http_client().delete(url).timeout(timeout)).await
    }
}

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

fn banner_from_body(field: &str, body: &str) -> Option<String> {
    // The body is `{ "<field>": <value> }`; the value is the only thing read,
    // and only as a string.
    let value: serde_json::Value = serde_json::from_str(body).ok()?;
    let text = value.get(field)?.as_str()?;
    is_usable_banner(text).then(|| text.to_owned())
}

/// True when the failure means "this server does not do extended profiles",
/// rather than "this user has no banner". The distinction is the difference
/// between rendering nothing and claiming something.
///
/// It keys on the ERRCODE, never on the status. Both answers are 404:
/// `M_NOT_FOUND` is the ORDINARY reply for a profile field that is simply not
/// set, and `M_UNRECOGNIZED` is a server that does not implement the endpoint
/// at all. An earlier version also matched "404" and "not found", so a
/// homeserver that fully supports extended profiles reported itself
/// unsupported the moment a user had no banner — which hid the entire
/// feature for everyone on it.
fn is_unsupported(error: &str) -> bool {
    let lowered = error.to_ascii_lowercase();
    lowered.contains("m_unrecognized")
        || lowered.contains("unrecognized")
        || lowered.contains("unrecognised")
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
        let mut banner = String::new();
        // Stable first, then the deployed unstable key. A server that answers
        // the stable field at all is one whose answer we trust, so an empty
        // stable answer still falls through — the two names coexist for now
        // and a banner set by Commet lives under the second one.
        //
        // `supported` is decided by ACCOUNTING, not by whichever field
        // happened to be asked last: the server is unsupported only when
        // every attempt came back unrecognised. An M_NOT_FOUND — no such
        // field — is a supported server answering "there is no banner".
        let mut any_answered = false;
        let mut any_unrecognised = false;
        for field in [BANNER_FIELD, BANNER_FIELD_UNSTABLE] {
            match profile_field::get(&client, uid.as_str(), field,
                                    BANNER_REQUEST_TIMEOUT).await {
                Ok(answer) if answer.status == 200 => {
                    any_answered = true;
                    if let Some(value) = banner_from_body(field, &answer.body) {
                        banner = value;
                        break;
                    }
                }
                Ok(answer) => {
                    if is_unsupported(&answer.body) {
                        any_unrecognised = true;
                    } else {
                        // A refusal, or a plain M_NOT_FOUND, both come from a
                        // server that KNOWS the endpoint.
                        any_answered = true;
                    }
                }
                // A transport failure says nothing about what the server
                // implements, so it must never latch "unsupported".
                Err(_) => any_answered = true,
            }
        }
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

// ─── Room / Space banners ────────────────────────────────────────────────
//
// Matrix specifies no room banner. MSC4427 covers user PROFILES only, and
// there is no equivalent for a room or a Space, so unlike the profile half
// above there is no deployed key to interoperate with — this is Lightning's
// own state event, in Lightning's own namespace, and it is named as such
// rather than squatting on the reserved `m.` prefix or on another client's
// unstable one. Any client that does not know it simply does not render a
// banner, which is the correct outcome for a decoration.
//
// It IS a real state event and not a local preference: a banner belongs to
// the room, everyone in it sees the same one, and it is set by whoever the
// room's own power levels allow to set it — never "whoever opened the panel".
/// The room/space banner state event, as Sable writes it.
///
/// Matrix specifies no room banner, so 0.7.5 shipped Lightning's own name for
/// it. That was the wrong call the moment another client already had one:
/// Sable (`src/types/matrix/room.ts`) uses
/// `page.codeberg.everypizza.room.banner`, state key "", content
/// `{ "url": "mxc://..." }`, gated on that event's own power level — the same
/// shape, under a name that is already in the wild. A banner nobody else can
/// see is not a banner, which is exactly the reasoning the PROFILE half of
/// this file already follows for `chat.commet.profile_banner`.
const ROOM_BANNER_EVENT: &str = "page.codeberg.everypizza.room.banner";
/// Read-only, for the banners 0.7.5 wrote under Lightning's own name. Never
/// written again: it exists so a banner set by the one release that used it
/// does not vanish. Only consulted when the interoperable key has none.
const ROOM_BANNER_EVENT_LEGACY: &str = "org.lightning_matrix.room_banner";

/// Read one room's banner, and whether this account may change it. Emits
/// `room_banner`.
///
/// The state store is consulted first and the homeserver second. Sliding sync
/// only delivers the state event types Lightning asks for in `required_state`,
/// and a custom type is not among them, so a store miss is the ORDINARY case
/// here and is not evidence that the room has no banner. A 404 from the direct
/// read is that evidence.
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
        // The interoperable key first, then the one 0.7.5 wrote. Store before
        // network for each: sliding sync only delivers the state types a
        // subscription names, so a store miss is the ORDINARY case here and is
        // not evidence that the room has no banner.
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

        // Offer policy is the room's OWN required level for this event type,
        // asked of the SDK — never a role label and never "is an admin".
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

/// Upload `local_path` and set it as the room's banner. An EMPTY path clears
/// it (an empty content object, which is how Matrix retires a state event).
/// Emits `room_banner_set`.
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
                // The CONTENT decides the type, never the file name.
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

/// Pull a usable banner mxc out of either a full state event or a bare
/// content object — the store hands over the event, the direct `/state` read
/// hands over the content, and both funnel through here so the same
/// mxc-only rule applies to both.
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
    fn the_room_banner_uses_the_name_other_clients_already_write() {
        // Sable writes page.codeberg.everypizza.room.banner with state key ""
        // and content {"url": "mxc://..."} — same shape Lightning had, under a
        // name that is already in the wild. Written under that name, so a
        // banner set here is visible there and the other way round.
        assert_eq!(ROOM_BANNER_EVENT, "page.codeberg.everypizza.room.banner");
        // ...and 0.7.5's own name is still READ, so the banners that release
        // wrote do not disappear. It must never be written again.
        assert_eq!(ROOM_BANNER_EVENT_LEGACY, "org.lightning_matrix.room_banner");
        assert_ne!(ROOM_BANNER_EVENT, ROOM_BANNER_EVENT_LEGACY);

        // Both names carry the same content shape, which is what makes
        // reading either of them one function.
        let content = r#"{"url":"mxc://example.org/banner"}"#;
        assert_eq!(
            banner_url_from_json(content).as_deref(),
            Some("mxc://example.org/banner")
        );
        // ...whether it arrives as a bare content object or a full event.
        let event = r#"{"type":"page.codeberg.everypizza.room.banner",
                        "content":{"url":"mxc://example.org/banner"}}"#;
        assert_eq!(
            banner_url_from_json(event).as_deref(),
            Some("mxc://example.org/banner")
        );
        // An http URL in room state is refused here exactly as in a profile.
        let unsafe_url = r#"{"url":"https://example.org/banner.png"}"#;
        assert_eq!(banner_url_from_json(unsafe_url), None);
    }

    #[test]
    fn an_unrecognised_endpoint_is_unsupported_not_absent() {
        assert!(is_unsupported("M_UNRECOGNIZED: Unrecognized request"));
        // Both of these are 404. M_NOT_FOUND is the ordinary answer for a
        // field nobody has set, and treating it as "this server cannot do
        // banners" hid the whole feature from every user without one.
        assert!(!is_unsupported("[404 / M_NOT_FOUND] Profile field not found"));
        assert!(!is_unsupported("the server returned 404 Not Found"));
        // A real refusal is NOT "the server cannot do banners".
        assert!(!is_unsupported("M_FORBIDDEN: not allowed"));
        assert!(!is_unsupported("connection reset"));
    }
}
