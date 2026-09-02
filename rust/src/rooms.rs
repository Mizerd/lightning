//! Room management, user search and the media bridge (v0.5.9).
//!
//! Every function here is invoked from a thin `extern "C"` wrapper in
//! `lib.rs`, does its synchronous validation on the caller's thread, and
//! spawns the network work as a *managed* task (`spawn_room_action`) on the
//! shared runtime so sign-out joins it deterministically. Async completions
//! are stamped with the lifecycle generation and an operation id; C++
//! rejects stale generations and unknown operation ids.
//!
//! Nothing in this module serializes key material, access tokens, raw
//! events, local file paths (into logs), or media bytes into the JSON event
//! queue. Media bytes cross the FFI through the dedicated take/free binary
//! bridge in `lib.rs`.

use std::sync::Arc;

use matrix_sdk::{
    attachment::{self, AttachmentInfo, BaseImageInfo, Thumbnail},
    media::{MediaFormat, MediaRequestParameters, MediaThumbnailSettings},
    ruma::{
        api::client::{
            media::get_content_thumbnail::v3::Method,
            room::{
                create_room::{self, v3::{CreationContent, RoomPreset}},
                Visibility,
            },
        },
        assign,
        events::{
            room::MediaSource,
            room::encryption::RoomEncryptionEventContent,
            room::power_levels::{NotificationPowerLevelType, PowerLevelAction},
            space::child::SpaceChildEventContent,
            InitialStateEvent, StateEventType, SyncStateEvent, TimelineEventType,
        },
        room::RoomType,
        serde::Raw,
        EventId, OwnedMxcUri, OwnedUserId, RoomId, UInt, UserId,
    },
    RoomMemberships, RoomState,
};
use matrix_sdk::deserialized_responses::SyncOrStrippedState;
use matrix_sdk_ui::timeline::AttachmentSource;
use serde::Deserialize;
use serde_json::json;

use crate::{enqueue, RustClient};

/// Upper bound on one member-snapshot payload. Rooms larger than this are
/// truncated (flagged in the event) — the UI shows counts, not 10k rows.
const MEMBER_SNAPSHOT_CAP: usize = 500;

/// Avatar uploads are small; refuse anything larger before reading it.
///
/// `pub(crate)` so the OWN-avatar path in `profile.rs` enforces the same
/// ceiling rather than carrying a second copy of the number that could drift
/// away from this one.
pub(crate) const MAX_AVATAR_BYTES: u64 = 8 * 1024 * 1024;

pub(crate) fn require_client(bridge: &RustClient) -> Result<matrix_sdk::Client, String> {
    bridge
        .client
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
        .ok_or_else(|| "no active Matrix session".to_owned())
}

pub(crate) fn joined_room(
    client: &matrix_sdk::Client,
    room_id: &str,
) -> Result<matrix_sdk::Room, String> {
    RoomId::parse(room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown or not-joined room".to_owned())
}

/// Coarse, non-secret categories for room-management failures. The raw SDK
/// error text may embed server detail; only the category crosses to QML
/// user strings. Pure and unit-tested.
pub(crate) fn classify_room_error(message: &str) -> &'static str {
    let lc = message.to_lowercase();
    if lc.contains("m_limit_exceeded") || lc.contains("limit exceeded") || lc.contains("429") {
        "rate_limited"
    } else if lc.contains("m_forbidden") || lc.contains("forbidden") || lc.contains("403") {
        "forbidden"
    } else if lc.contains("m_room_in_use") || lc.contains("alias") && lc.contains("use") {
        "alias_taken"
    } else if lc.contains("m_invalid") || lc.contains("invalid") {
        "invalid"
    } else if lc.contains("m_not_found") || lc.contains("not found") || lc.contains("404") {
        "not_found"
    } else {
        "network"
    }
}

/// Sniff a raster-image MIME type from magic bytes. Used for avatar upload
/// and clipboard data so a mislabelled extension cannot spoof the type.
/// Pure and unit-tested.
///
/// This IDENTIFIES; it does not promise the GUI can draw the result. Qt image
/// formats are dlopen'd plugins, so what a build decodes is a packaging fact
/// that differs per platform (JPEG XL is reachable on Linux and on neither
/// Windows nor macOS — Qt has never shipped a JXL plugin and the only one that
/// exists, KDE's kimageformats, is packaged for neither). Deciding what THIS
/// build can render belongs to `lightning::imagefmt::canDecode` on the C++
/// side, which asks QImageReader; the mirror of this table lives in
/// `src/media/ImageFormatSupport.h`.
pub(crate) fn sniff_image_mime(bytes: &[u8]) -> Option<&'static str> {
    if bytes.len() < 12 {
        return None;
    }
    if bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]) {
        Some("image/png")
    } else if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some("image/jpeg")
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some("image/gif")
    } else if bytes.starts_with(b"RIFF") && &bytes[8..12] == b"WEBP" {
        Some("image/webp")
    // JPEG XL, both shapes. VERIFIED against real `cjxl` output rather than
    // read off a spec summary:
    //   ISOBMFF container: 00 00 00 0C "JXL " 0D 0A 87 0A, then "ftypjxl "
    //   bare codestream:   FF 0A            (lossy and `-d 0` lossless alike)
    // The container test comes FIRST and is 12 bytes, so it cannot be reached
    // by the two-byte codestream test. Note bytes[4..8] here is "JXL ", not
    // "ftyp", so nothing that keys on the ISO BMFF brand can mistake a .jxl
    // for an MP4.
    //
    // The codestream signature is two bytes, which is weak — no weaker than
    // the "BM" below, and the cost of a false positive is a payload declared
    // image/jxl that a decoder then refuses, never a wrong decode.
    } else if bytes.starts_with(&[0x00, 0x00, 0x00, 0x0C, b'J', b'X', b'L', b' ', 0x0D, 0x0A, 0x87, 0x0A])
    {
        Some("image/jxl")
    } else if bytes.starts_with(&[0xFF, 0x0A]) {
        Some("image/jxl")
    } else if bytes.starts_with(b"BM") {
        Some("image/bmp")
    } else {
        None
    }
}

// ---------------------------------------------------------------------------
// User search
// ---------------------------------------------------------------------------

pub(crate) fn search_users(
    bridge: &RustClient,
    query: String,
    limit: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.search_users(&query, limit.clamp(1, 50)).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(response) => {
                let results: Vec<serde_json::Value> = response
                    .results
                    .iter()
                    .map(|user| {
                        json!({
                            "user_id": user.user_id.to_string(),
                            "display_name": user.display_name.clone().unwrap_or_default(),
                            "avatar_url": user
                                .avatar_url
                                .as_ref()
                                .map(|a| a.to_string())
                                .unwrap_or_default(),
                        })
                    })
                    .collect();
                enqueue(&events, json!({
                    "type": "user_search_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "limited": response.limited,
                    "results": results,
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "user_search_result",
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

/// v0.5.11: exact profile lookup for one user id (GET /profile/{userId}).
/// Backs the bare-localpart invite search: the directory may not list local
/// users, so a plausible `@localpart:own-server` candidate is confirmed (or
/// refuted) against the homeserver before it is offered as a result. Only
/// display name and avatar mxc cross the FFI; a missing user surfaces as
/// ok=false with category "not_found".
/// A profile lookup is decoration — a face and a display name — and every room
/// action is joined on the GUI thread at teardown, so an unbounded one is an
/// unbounded freeze. Ten seconds matches the presence batch's per-request
/// bound in `presence.rs`.
const PROFILE_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);

pub(crate) fn fetch_user_profile(
    bridge: &RustClient,
    user_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId =
        UserId::parse(&user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        // BOUNDED, and it is the reason this one needed it more than most.
        //
        // This was the only network room action with NO timeout, on matrix-sdk's
        // default RequestConfig — which RETRIES and honours M_LIMIT_EXCEEDED's
        // retry_after_ms. DirectAvatarResolver fires one of these per DM peer
        // the moment an account finishes restoring, which is exactly the burst
        // a homeserver rate-limiter answers with a retry-after.
        //
        // And every room action is JOINED, on the GUI thread, when the session
        // is torn down (shutdown_managed_tasks). So an unbounded profile
        // request is an unbounded GUI freeze on the next account switch — the
        // reported "switch to one acc and back to the first acc it freezes for
        // about 3-5 seconds", which reproduces on the SECOND switch precisely
        // because that is when the fan-out is still in flight.
        //
        // A profile is decoration: a face and a display name. It is never
        // worth holding a teardown for, and a timed-out lookup is reported as
        // an ordinary failure the resolver already handles.
        let result = match tokio::time::timeout(
            PROFILE_REQUEST_TIMEOUT,
            client.account().fetch_user_profile_of(&uid),
        )
        .await
        {
            Ok(inner) => inner,
            Err(_) => {
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, json!({
                        "type": "user_profile_result",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "user_id": uid.to_string(),
                        "category": "timeout",
                    }));
                }
                return;
            }
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(profile) => {
                let display_name = profile
                    .get("displayname")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_owned();
                let avatar_url = profile
                    .get("avatar_url")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_owned();
                enqueue(&events, json!({
                    "type": "user_profile_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "user_id": uid.to_string(),
                    "display_name": display_name,
                    "avatar_url": avatar_url,
                }));
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "user_profile_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "user_id": uid.to_string(),
                    "category": classify_room_error(&err.to_string()),
                }));
            }
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Client-side URL previews (v0.5.12)
// ---------------------------------------------------------------------------

const MAX_HTML_BYTES: usize = 2 * 1_048_576;
const MAX_IMAGE_BYTES: usize = 5 * 1_048_576;
const MAX_IMAGE_PIXELS: u64 = 25_000_000;
const MAX_REDIRECTS: usize = 4;
const CONNECT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);
const REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(12);

fn public_ip(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(v) => !(v.is_private() || v.is_loopback()
            || v.is_link_local() || v.is_multicast() || v.is_unspecified()
            || v.octets()[0] == 0 || v.octets()[0] >= 224
            || (v.octets()[0] == 100 && (64..=127).contains(&v.octets()[1]))
            || (v.octets()[0] == 169 && v.octets()[1] == 254)),
        std::net::IpAddr::V6(v) => !(v.is_loopback() || v.is_unspecified()
            || v.is_multicast() || (v.segments()[0] & 0xfe00) == 0xfc00
            || (v.segments()[0] & 0xffc0) == 0xfe80),
    }
}

pub(crate) struct SafeResponse { pub status: reqwest::StatusCode, pub mime: String, pub location: Option<String>, pub bytes: Vec<u8> }
// Default Accept for HTML/image previews. GIF downloads and provider JSON pass
// their own via the `accept` parameter so a `.gif` URL is never
// content-negotiated into webp, and JSON endpoints get a JSON Accept.
pub(crate) const PREVIEW_ACCEPT: &str =
    "text/html,image/jpeg,image/png,image/webp,image/gif";

async fn safe_get(url: &url::Url, limit: usize, accept: &str)
    -> Result<SafeResponse, &'static str> {
    use futures_util::StreamExt;
    if url.scheme() != "https" || !url.username().is_empty() || url.password().is_some()
        || url.host_str().is_none() { return Err("invalid_url"); }
    let host = url.host_str().unwrap();
    let lower = host.to_ascii_lowercase();
    if lower == "localhost" || lower.ends_with(".localhost") || lower.ends_with(".local") {
        return Err("blocked_destination");
    }
    let port = url.port_or_known_default().unwrap_or(443);
    let addresses: Vec<_> = tokio::net::lookup_host((host, port)).await
        .map_err(|_| "dns_failure")?.collect();
    if addresses.is_empty() || addresses.iter().any(|a| !public_ip(a.ip())) {
        return Err("blocked_destination");
    }
    // Pin the validated DNS answer so a second lookup cannot rebind the host.
    let client = reqwest::Client::builder().redirect(reqwest::redirect::Policy::none())
        .connect_timeout(CONNECT_TIMEOUT).timeout(REQUEST_TIMEOUT)
        .user_agent(crate::USER_AGENT).resolve(host, addresses[0]).build()
        .map_err(|_| "request_failure")?;
    let response = client.get(url.clone())
        .header(reqwest::header::ACCEPT, accept)
        // A handful of ordinary sites' CDN/WAF layers treat a request
        // missing standard browser headers (Accept-Language in particular)
        // as suspicious and serve an interstitial or a terminal status
        // instead of the article. Accept-Encoding is handled by reqwest's
        // gzip/deflate features, not set manually here.
        .header(reqwest::header::ACCEPT_LANGUAGE, "en-US,en;q=0.9")
        .send().await.map_err(|e|
        if e.is_timeout() { "timeout" } else { "request_failure" })?;
    let status = response.status();
    let mime = response.headers().get(reqwest::header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok()).unwrap_or("").split(';').next().unwrap_or("")
        .trim().to_ascii_lowercase();
    let location = response.headers().get(reqwest::header::LOCATION)
        .and_then(|v| v.to_str().ok()).map(str::to_owned);
    let mut bytes = Vec::new();
    let mut stream = response.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(|_| "request_failure")?;
        if bytes.len() + chunk.len() > limit { return Err("response_too_large"); }
        bytes.extend_from_slice(&chunk);
    }
    Ok(SafeResponse { status, mime, location, bytes })
}

fn clipped(value: String, max: usize) -> String { value.chars().take(max).collect() }
#[derive(Default)]
struct PreviewSink { fields: std::collections::HashMap<String,String>, title: String, in_title: bool }
fn html_fields(input: &str) -> (std::collections::HashMap<String,String>, String) {
    use html5ever::tokenizer::{BufferQueue, TagKind, Token, TokenSink, TokenSinkResult, Tokenizer, TokenizerOpts};
    use tendril::StrTendril;
    struct Sink(std::cell::RefCell<PreviewSink>);
    impl TokenSink for Sink {
        type Handle = ();
        fn process_token(&self, token: Token, _: u64) -> TokenSinkResult<()> {
            let mut state = self.0.borrow_mut();
            match token {
                Token::TagToken(tag) if tag.kind == TagKind::StartTag && tag.name.as_ref() == "meta" => {
                    let mut key = ""; let mut content = "";
                    for attr in &tag.attrs { match attr.name.local.as_ref() {
                        "property"|"name" => key = attr.value.as_ref(), "content" => content = attr.value.as_ref(), _ => {} } }
                    if !key.is_empty() && !content.is_empty() { state.fields.entry(key.to_ascii_lowercase()).or_insert_with(|| content.trim().to_owned()); }
                }
                Token::TagToken(tag) if tag.name.as_ref() == "title" => state.in_title = tag.kind == TagKind::StartTag,
                Token::CharacterTokens(text) if state.in_title => state.title.push_str(&text),
                _ => {}
            }
            TokenSinkResult::Continue
        }
    }
    let sink = Sink(std::cell::RefCell::new(PreviewSink::default()));
    let mut input_queue = BufferQueue::default(); input_queue.push_back(StrTendril::from(input));
    let tok = Tokenizer::new(sink, TokenizerOpts::default()); let _ = tok.feed(&mut input_queue); tok.end();
    let state = tok.sink.0.into_inner(); (state.fields, state.title.trim().to_owned())
}
fn pick(fields: &std::collections::HashMap<String,String>, keys: &[&str]) -> String {
    keys.iter().find_map(|k| fields.get(*k).cloned()).unwrap_or_default()
}
// Sanitized failure detail for one preview attempt — carries only what is
// needed to distinguish a real code regression from live remote policy
// (a site's own bot/WAF layer) without ever including the URL, query
// string, or response body. `status` is None for failures that never got
// an HTTP response at all (DNS, timeout, blocked destination, ...).
#[derive(Debug, Clone, Copy)]
pub(crate) struct PreviewFailure {
    pub category: &'static str,
    pub status: Option<u16>,
    pub redirects: u32,
}
impl From<&'static str> for PreviewFailure {
    fn from(category: &'static str) -> Self {
        Self { category, status: None, redirects: 0 }
    }
}

// Follow redirects manually so every hop receives the same DNS/IP policy,
// DNS pinning, scheme check, timeout, and response bound. Used for both the
// primary URL and HTML metadata images; reqwest redirect following remains
// disabled inside safe_get().
pub(crate) async fn safe_get_following_redirects(
    mut url: url::Url,
    limit: usize,
    accept: &str,
) -> Result<(SafeResponse, url::Url, u32), PreviewFailure> {
    for redirects in 0..=MAX_REDIRECTS {
        let redirects = redirects as u32;
        let response = safe_get(&url, limit, accept)
            .await
            .map_err(|category| PreviewFailure {
                category,
                status: None,
                redirects,
            })?;
        if !response.status.is_redirection() {
            return Ok((response, url, redirects));
        }
        if redirects as usize == MAX_REDIRECTS {
            return Err(PreviewFailure {
                category: "too_many_redirects",
                status: Some(response.status.as_u16()),
                redirects,
            });
        }
        let next = response.location.as_deref().ok_or(PreviewFailure {
            category: "invalid_redirect",
            status: Some(response.status.as_u16()),
            redirects,
        })?;
        url = url.join(next).map_err(|_| PreviewFailure {
            category: "invalid_redirect",
            status: Some(response.status.as_u16()),
            redirects,
        })?;
    }
    Err(PreviewFailure {
        category: "too_many_redirects",
        status: None,
        redirects: MAX_REDIRECTS as u32,
    })
}

// Some CDNs label passive image bytes as application/octet-stream or even
// text/html. Classification therefore checks both the final declared MIME
// and the bytes. A declared supported image must agree with its magic bytes;
// generic/mislabeled responses may be promoted only when recognized bytes
// prove a supported passive raster. Actual HTML remains HTML. SVG never
// matches the passive-image sniffer.
fn classify_preview_payload(
    declared_mime: &str,
    bytes: &[u8],
) -> Result<Option<&'static str>, &'static str> {
    let sniffed = sniff_image_mime(bytes);
    if matches!(
        declared_mime,
        "image/jpeg" | "image/png" | "image/webp" | "image/gif"
    ) {
        return if sniffed == Some(declared_mime) {
            Ok(sniffed)
        } else {
            Err("invalid_image")
        };
    }
    if let Some(mime) = sniffed {
        return Ok(Some(mime));
    }
    if declared_mime == "text/html" {
        return Ok(None);
    }
    Err("unsupported_mime")
}

// The redirect loop's own fetch doesn't know ahead of time whether the
// destination is an HTML page or a direct image — Content-Type is only
// known after the fetch completes — so it must accommodate the larger of
// the two byte limits. Using the smaller MAX_HTML_BYTES here (as 0.5.13
// did) silently capped every direct-image preview at that smaller ceiling.
const MAX_INITIAL_FETCH_BYTES: usize = MAX_IMAGE_BYTES;

async fn preview(page: url::Url) -> Result<serde_json::Value, PreviewFailure> {
    let (response, final_page, redirects) =
        safe_get_following_redirects(page, MAX_INITIAL_FETCH_BYTES, PREVIEW_ACCEPT).await?;
    if !response.status.is_success() {
        let category = if response.status.is_server_error() || response.status.as_u16() == 429 {
            "http_transient"
        } else {
            "http_terminal"
        };
        return Err(PreviewFailure {
            category,
            status: Some(response.status.as_u16()),
            redirects,
        });
    }
    match classify_preview_payload(&response.mime, &response.bytes).map_err(|category| {
        PreviewFailure {
            category,
            status: Some(response.status.as_u16()),
            redirects,
        }
    })? {
        Some(mime) => {
            let mut fields = image_fields(mime.to_owned(), response.bytes).map_err(|category| {
                PreviewFailure {
                    category,
                    status: Some(response.status.as_u16()),
                    redirects,
                }
            })?;
            fields["preview_kind"] = "direct_media".into();
            Ok(fields)
        }
        None => {
            if response.bytes.len() > MAX_HTML_BYTES {
                return Err(PreviewFailure {
                    category: "response_too_large",
                    status: Some(response.status.as_u16()),
                    redirects,
                });
            }
            let html = String::from_utf8_lossy(&response.bytes);
            let (metadata, html_title) = html_fields(&html);
            let title = pick(&metadata, &["og:title", "twitter:title"]);
            let description = pick(
                &metadata,
                &["og:description", "twitter:description", "description"],
            );
            let image_url = pick(&metadata, &["og:image", "twitter:image"]);
            let image = if image_url.is_empty() {
                None
            } else {
                final_page.join(&image_url).ok()
            };
            if title.is_empty()
                && html_title.is_empty()
                && description.is_empty()
                && image.is_none()
            {
                return Err(PreviewFailure {
                    category: "no_metadata",
                    status: Some(response.status.as_u16()),
                    redirects,
                });
            }
            let mut fields = json!({
                "preview_kind": "metadata",
                "title": clipped(if title.is_empty() { html_title } else { title }, 300),
                "description": clipped(description, 1000),
                "site_name": clipped(pick(&metadata, &["og:site_name"]), 120),
                "image_source": "", "image_mime": "", "image_width": 0,
                "image_height": 0, "image_size": 0
            });
            if let Some(image) = image {
                // A broken/protected/slow thumbnail CDN must not sink an
                // otherwise-valid preview that already has title/description —
                // fetch failures and unsupported/invalid image bytes here are
                // swallowed, leaving the image fields empty.
                if let Ok((fetched, _, _)) =
                    safe_get_following_redirects(image, MAX_IMAGE_BYTES, PREVIEW_ACCEPT).await
                {
                    if fetched.status.is_success() {
                        if let Ok(Some(mime)) =
                            classify_preview_payload(&fetched.mime, &fetched.bytes)
                        {
                            if let Ok(image_json) = image_fields(mime.to_owned(), fetched.bytes) {
                                for key in [
                                    "image_source",
                                    "image_mime",
                                    "image_width",
                                    "image_height",
                                    "image_size",
                                ] {
                                    fields[key] = image_json[key].clone();
                                }
                            }
                        }
                    }
                }
            }
            Ok(fields)
        }
    }
}
fn image_fields(mime: String, bytes: Vec<u8>) -> Result<serde_json::Value, &'static str> {
    use base64::Engine;
    if !matches!(mime.as_str(), "image/jpeg"|"image/png"|"image/webp"|"image/gif") { return Err("unsupported_mime"); }
    let (width, height) = image_dimensions(&mime, &bytes).ok_or("invalid_image")?;
    if u64::from(width) * u64::from(height) > MAX_IMAGE_PIXELS { return Err("image_dimensions"); }
    let source = format!("data:{mime};base64,{}", base64::engine::general_purpose::STANDARD.encode(&bytes));
    Ok(json!({"image_source":source,"image_mime":mime,"image_width":width,
        "image_height":height,"image_size":bytes.len()}))
}
fn image_dimensions(mime: &str, b: &[u8]) -> Option<(u32,u32)> {
    let be = |i| u32::from_be_bytes([b[i],b[i+1],b[i+2],b[i+3]]);
    if mime == "image/png" && b.len() >= 24 && &b[..8] == b"\x89PNG\r\n\x1a\n" { return Some((be(16),be(20))); }
    if mime == "image/gif" && b.len() >= 10 && (&b[..6] == b"GIF87a" || &b[..6] == b"GIF89a") {
        return Some((u16::from_le_bytes([b[6],b[7]]) as u32,u16::from_le_bytes([b[8],b[9]]) as u32));
    }
    if mime == "image/webp" && b.len() >= 16 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        let chunk = &b[12..16];
        // "VP8X" (extended: animation/alpha/exif) is the only variant the
        // original check handled — but most real-world direct WebP links
        // use the much more common simple lossy ("VP8 ") or lossless
        // ("VP8L") chunk, which would otherwise fail as "invalid_image".
        if chunk == b"VP8X" && b.len() >= 30 {
            let n = |i| 1 + u32::from_le_bytes([b[i],b[i+1],b[i+2],0]);
            return Some((n(24),n(27)));
        }
        if chunk == b"VP8L" && b.len() >= 25 && b[20] == 0x2f {
            let bits = u32::from_le_bytes([b[21],b[22],b[23],b[24]]);
            return Some(((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1));
        }
        if chunk == b"VP8 " && b.len() >= 30 && b[23] == 0x9d && b[24] == 0x01 && b[25] == 0x2a {
            let width = u16::from_le_bytes([b[26],b[27]]) & 0x3FFF;
            let height = u16::from_le_bytes([b[28],b[29]]) & 0x3FFF;
            return Some((width as u32, height as u32));
        }
    }
    if mime == "image/jpeg" && b.starts_with(&[0xff,0xd8]) { let mut i=2; while i+9 < b.len() {
        if b[i] != 0xff { i+=1; continue; } let marker=b[i+1]; if marker == 0xd9 || marker == 0xda { break; }
        let len=u16::from_be_bytes([b[i+2],b[i+3]]) as usize; if len<2 || i+2+len>b.len(){break;}
        if matches!(marker,0xc0..=0xc3|0xc5..=0xc7|0xc9..=0xcb|0xcd..=0xcf) { return Some((u16::from_be_bytes([b[i+7],b[i+8]]) as u32,u16::from_be_bytes([b[i+5],b[i+6]]) as u32)); } i+=2+len;
    }} None
}

/// The homeserver's own preview of a page, or None if it cannot supply one.
///
/// WHY THIS EXISTS AND RUNS FIRST. A client-side preview fetch contacts the
/// linked website directly, which hands that site the member's IP address —
/// a tracking pixel by another name, and the reason previews have shipped
/// default-OFF behind a consent box since the 2026-08-12 privacy audit. The
/// homeserver fetching on our behalf removes that exposure entirely: the site
/// sees the SERVER, and the returned thumbnail is an `mxc://` that rides the
/// existing authenticated media path.
///
/// WHAT IT COSTS INSTEAD, because it is not free. The homeserver learns which
/// URL was previewed. In an unencrypted room it already saw the message
/// carrying that link, so this discloses nothing new. In an ENCRYPTED room it
/// did not — which is exactly why the caller keeps the existing rule that an
/// encrypted room never previews without an explicit per-message gesture.
/// Server-side previewing does not weaken that gate; it changes only WHO sees
/// the URL once the user has asked for it.
///
/// Returns Ok(None) — not an error — when the server has previews disabled or
/// the endpoint is unrecognised, so the caller falls back to the client path
/// without treating a normal server configuration as a failure.
async fn server_preview(
    client: &matrix_sdk::Client,
    page: &url::Url,
) -> Option<serde_json::Value> {
    use matrix_sdk::ruma::api::client::authenticated_media::get_media_preview;
    let request = get_media_preview::v1::Request::new(page.to_string());
    let response = client.send(request).await.ok()?;
    let raw = response.data?;
    let data: serde_json::Value = serde_json::from_str(raw.get()).ok()?;
    server_preview_fields(&data)
}

/// The OpenGraph object a server returned, reshaped into our field set — or
/// None when it did not actually say anything.
///
/// SPLIT OUT SO A TEST CAN DRIVE IT. Left inline it was reachable only
/// through a live homeserver, and the test that "covered" it re-implemented
/// the predicate as a local closure — which asserts that the test agrees with
/// itself and would pass with this function deleted.
pub(crate) fn server_preview_fields(data: &serde_json::Value) -> Option<serde_json::Value> {
    let obj = data.as_object()?;
    // OPENGRAPH KEYS, and the server may legitimately supply none of them:
    // a URL it could not fetch still returns 200 with an empty object. An
    // answer with neither a title nor a description is no better than no
    // answer, so it counts as "the server could not do this" and the caller
    // falls back rather than drawing an empty card.
    let text = |k: &str| -> String {
        obj.get(k).and_then(|v| v.as_str()).unwrap_or_default().to_owned()
    };
    let title = text("og:title");
    let description = text("og:description");
    if title.is_empty() && description.is_empty() {
        return None;
    }
    // The SAME field set the client path emits, so nothing downstream needs to
    // know which route produced it. `og:image` is an mxc:// URI here rather
    // than an http(s) URL — the media bridge already resolves those, and it is
    // the whole reason the thumbnail costs no direct contact either.
    let image_source = text("og:image");
    let image_size = obj
        .get("matrix:image:size")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let dim = |k: &str| -> u64 {
        obj.get(k).and_then(|v| v.as_u64()).unwrap_or(0)
    };
    Some(json!({
        "preview_kind": "metadata",
        "preview_route": "server",
        "title": clipped(title, 300),
        "description": clipped(description, 1000),
        "site_name": clipped(text("og:site_name"), 120),
        "image_source": image_source,
        "image_mime": text("og:image:type"),
        "image_width": dim("og:image:width"),
        "image_height": dim("og:image:height"),
        "image_size": image_size,
    }))
}

pub(crate) fn fetch_url_preview(
    bridge: &RustClient,
    url: String,
    op_id: u64,
) -> Result<(), String> {
    // Previews are account/session scoped — and the client this returns is
    // also what the server-side attempt below sends through, so there is one
    // session involved, not two.
    let sdk = require_client(bridge)?;
    let parsed = url::Url::parse(url.trim()).map_err(|_| "invalid URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("unsupported or credentialed URL".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        // SERVER FIRST, CLIENT ONLY IF IT CANNOT. The homeserver fetching the
        // page means the linked site never sees the member's IP. Falling back
        // is not a silent downgrade of that property — the caller decides
        // whether a direct fetch is permitted at all, and the result carries
        // `preview_route` so the UI can say which one produced the card
        // rather than implying the private path was used.
        let mut result = match server_preview(&sdk, &parsed).await {
            Some(fields) => Ok(fields),
            None => preview(parsed.clone()).await,
        };
        // Anything the client path produced took the direct route. Stamped
        // here rather than in preview() so there is exactly one place that
        // decides what the label means, and no way to return a card without
        // one.
        if let Ok(ref mut fields) = result {
            if fields.get("preview_route").is_none() {
                fields["preview_route"] = "client".into();
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(fields) => {
                let mut out = json!({
                    "type": "url_preview_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                });
                out["fields"] = fields;
                enqueue(&events, out);
            }
            Err(failure) => {
                enqueue(&events, json!({
                    "type": "url_preview_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": failure.category,
                    "status": failure.status,
                    "redirects": failure.redirects,
                }));
            }
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Direct messages
// ---------------------------------------------------------------------------

/// Existing joined DM rooms with `user_id`, straight from the SDK's
/// authoritative m.direct projection. Synchronous (store lookup only).
pub(crate) fn get_dm_rooms(bridge: &RustClient, user_id: &str) -> Result<String, String> {
    let client = require_client(bridge)?;
    let uid = UserId::parse(user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    let rooms: Vec<serde_json::Value> = client
        .get_dm_rooms(&uid)
        .map(|room| {
            json!({
                "room_id": room.room_id().to_string(),
                "name": room.name().unwrap_or_default(),
            })
        })
        .collect();
    serde_json::to_string(&json!({ "rooms": rooms }))
        .map_err(|_| "serialization failed".to_owned())
}

/// Create an encrypted DM with `user_id` via the pinned `Client::create_dm`
/// (TrustedPrivateChat preset, encryption initial state, `is_direct`, and
/// the SDK's locked read-modify-write m.direct update).
pub(crate) fn create_dm(bridge: &RustClient, user_id: String, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId =
        UserId::parse(&user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    if client.user_id() == Some(uid.as_ref()) {
        return Err("cannot start a direct message with yourself".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.create_dm(&uid).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room) => {
                enqueue(&events, json!({
                    "type": "dm_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                }));
                crate::enqueue_rooms(&events, &client).await;
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "dm_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }));
            }
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Room creation
// ---------------------------------------------------------------------------

#[derive(Debug, Default, Deserialize, PartialEq)]
pub(crate) struct CreateRoomOptions {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub topic: String,
    #[serde(default)]
    pub public: bool,
    #[serde(default)]
    pub encrypted: bool,
    #[serde(default)]
    pub alias: String,
    #[serde(default)]
    pub invites: Vec<String>,
    #[serde(default)]
    pub space_id: String,
    /// Create a Matrix Space (m.space) instead of an ordinary room.
    #[serde(default)]
    pub is_space: bool,
}

/// Build the pinned ruma create-room request from validated options.
/// No room id or room version is generated locally — the server decides.
pub(crate) fn build_create_room_request(
    opts: &CreateRoomOptions,
) -> Result<create_room::v3::Request, String> {
    if opts.name.trim().is_empty() {
        return Err("room name must not be empty".to_owned());
    }
    let mut invites: Vec<OwnedUserId> = Vec::new();
    for user in &opts.invites {
        let uid = UserId::parse(user)
            .map_err(|_| format!("invalid invite user id: {user}"))?;
        if !invites.contains(&uid) {
            invites.push(uid);
        }
    }
    // A Space is a room of type m.space; it is never an encrypted timeline,
    // so the encryption initial-state is skipped for Spaces.
    let initial_state = if opts.encrypted && !opts.is_space {
        vec![InitialStateEvent::with_empty_state_key(
            RoomEncryptionEventContent::with_recommended_defaults(),
        )
        .to_raw_any()]
    } else {
        Vec::new()
    };
    let creation_content = if opts.is_space {
        let cc = assign!(CreationContent::new(), { room_type: Some(RoomType::Space) });
        Some(Raw::new(&cc).map_err(|e| format!("space creation content: {e}"))?)
    } else {
        None
    };
    let request = assign!(create_room::v3::Request::new(), {
        name: Some(opts.name.trim().to_owned()),
        topic: (!opts.topic.trim().is_empty()).then(|| opts.topic.trim().to_owned()),
        invite: invites,
        is_direct: false,
        preset: Some(if opts.public {
            RoomPreset::PublicChat
        } else {
            RoomPreset::PrivateChat
        }),
        visibility: if opts.public { Visibility::Public } else { Visibility::Private },
        room_alias_name: (!opts.alias.trim().is_empty())
            .then(|| opts.alias.trim().to_owned()),
        initial_state,
        creation_content,
    });
    Ok(request)
}

pub(crate) fn create_room(
    bridge: &RustClient,
    options_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let opts: CreateRoomOptions = serde_json::from_str(&options_json)
        .map_err(|_| "invalid room options".to_owned())?;
    let request = build_create_room_request(&opts)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.create_room(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room) => {
                // Optional Space placement. Failure here must not read as a
                // failed room creation — it is reported as a warning.
                let mut warning = String::new();
                if !opts.space_id.is_empty() {
                    let placed = add_space_child(&client, &opts.space_id, room.room_id().as_str())
                        .await
                        .is_ok();
                    if !placed {
                        warning = "space_add_failed".to_owned();
                    }
                }
                enqueue(&events, json!({
                    "type": "room_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room.room_id().to_string(),
                    "warning": warning,
                }));
                crate::enqueue_rooms(&events, &client).await;
            }
            Err(err) => {
                enqueue(&events, json!({
                    "type": "room_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }));
            }
        }
    });
    Ok(())
}

async fn add_space_child(
    client: &matrix_sdk::Client,
    space_id: &str,
    child_room_id: &str,
) -> Result<(), String> {
    let space = RoomId::parse(space_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown space".to_owned())?;
    let server = client
        .user_id()
        .map(|u| u.server_name().to_owned())
        .ok_or_else(|| "no session".to_owned())?;
    space
        .send_state_event_for_key(
            &RoomId::parse(child_room_id).map_err(|_| "invalid room id".to_owned())?,
            SpaceChildEventContent::new(vec![server]),
        )
        .await
        .map(|_| ())
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())
}

/// MSC1772 child removal: an m.space.child state event whose `via` list is
/// empty means "not a child" — the room itself is never left or deleted.
async fn remove_space_child(
    client: &matrix_sdk::Client,
    space_id: &str,
    child_room_id: &str,
) -> Result<(), String> {
    let space = RoomId::parse(space_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown space".to_owned())?;
    space
        .send_state_event_for_key(
            &RoomId::parse(child_room_id).map_err(|_| "invalid room id".to_owned())?,
            SpaceChildEventContent::new(Vec::new()),
        )
        .await
        .map(|_| ())
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())
}

pub(crate) fn remove_room_from_space(
    bridge: &crate::RustClient,
    space_id: String,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let ok = remove_space_child(&client, &space_id, &room_id).await.is_ok();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "space_child_removed_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "space_id": space_id,
            "room_id": room_id,
            "ok": ok,
        }));
    });
    Ok(())
}

pub(crate) fn add_room_to_space(
    bridge: &RustClient,
    space_id: String,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let ok = add_space_child(&client, &space_id, &room_id).await.is_ok();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "space_child_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "space_id": space_id,
            "room_id": room_id,
            "ok": ok,
        }));
    });
    Ok(())
}

/// Toggle the MSC1772 `suggested` flag on an EXISTING m.space.child,
/// preserving the event's `via` list and `order` key — the flag rides the
/// same state event that makes the room a child, so a blind rewrite would
/// clobber routing. A room that is not currently a child (no event, or an
/// empty-via tombstone) is refused, never promoted to a child as a side
/// effect of suggesting it.
async fn set_space_child_suggested_inner(
    client: &matrix_sdk::Client,
    space_id: &str,
    child_room_id: &str,
    suggested: bool,
) -> Result<(), String> {
    let space = RoomId::parse(space_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined)
        .ok_or_else(|| "unknown space".to_owned())?;
    let child =
        RoomId::parse(child_room_id).map_err(|_| "invalid room id".to_owned())?;
    let current = space
        .get_state_event_static_for_key::<SpaceChildEventContent, _>(&child)
        .await
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?
        .ok_or_else(|| "not a child".to_owned())?;
    let mut content = match current.deserialize() {
        Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(event))) => {
            event.content
        }
        _ => return Err("not a child".to_owned()),
    };
    if content.via.is_empty() {
        return Err("not a child".to_owned());
    }
    content.suggested = suggested;
    space
        .send_state_event_for_key(&child, content)
        .await
        .map(|_| ())
        .map_err(|err| classify_room_error(&err.to_string()).to_owned())
}

pub(crate) fn set_space_child_suggested(
    bridge: &RustClient,
    space_id: String,
    room_id: String,
    suggested: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let ok =
            set_space_child_suggested_inner(&client, &space_id, &room_id, suggested)
                .await
                .is_ok();
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "space_child_suggested_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "space_id": space_id,
            "room_id": room_id,
            "suggested": suggested,
            "ok": ok,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Invites to existing rooms
// ---------------------------------------------------------------------------

pub(crate) fn invite_users(
    bridge: &RustClient,
    room_id: String,
    users_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let users: Vec<String> =
        serde_json::from_str(&users_json).map_err(|_| "invalid invite list".to_owned())?;
    if users.is_empty() {
        return Err("no users to invite".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut ok_count = 0u32;
        let mut fail_count = 0u32;
        let mut seen: Vec<String> = Vec::new();
        for user in users {
            if seen.contains(&user) {
                continue;
            }
            seen.push(user.clone());
            let outcome = match UserId::parse(&user) {
                Err(_) => Err("invalid_user".to_owned()),
                Ok(uid) => room
                    .invite_user_by_id(&uid)
                    .await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned()),
            };
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            match outcome {
                Ok(()) => ok_count += 1,
                Err(_) => fail_count += 1,
            }
            enqueue(&events, json!({
                "type": "room_invite_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "user_id": user,
                "ok": outcome.is_ok(),
                "category": outcome.err().unwrap_or_default(),
            }));
        }
        enqueue(&events, json!({
            "type": "room_invite_done",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok_count": ok_count,
            "fail_count": fail_count,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Thread participants (facepiles)
// ---------------------------------------------------------------------------

/// How many participants cross the FFI. The facepile shows 2-4; a small
/// surplus lets the UI dedupe/choose without a second request, and caps what
/// one payload can carry.
const THREAD_PARTICIPANT_CAP: usize = 8;

/// v0.7: the REAL participants of a thread, for the summary card facepile.
///
/// matrix-sdk-ui 0.18 does not expose this: `ThreadSummary` and
/// `ThreadListItem` both carry only the root sender, the latest reply's
/// sender and a reply COUNT — a count of replies, never of people. So the
/// participants are derived from the thread's actual events via
/// `Room::load_or_fetch_event_with_relations`, which is CACHE-FIRST and only
/// hits the network when the relations are not already known locally (and
/// writes what it fetches back into the event cache, so a second card for
/// the same root is free).
///
/// Ordering is deterministic: the root's sender first, then every other
/// sender in the order they first appear in the thread. Deduplicated by
/// Matrix user id, so the same person appearing ten times is one face.
///
/// Only presentation-safe fields cross the FFI — user id, display name,
/// avatar mxc — never event content, never message bodies. `truncated`
/// reports honestly whether more distinct participants exist than were
/// returned, so the UI is never invited to imply a total it does not know.
pub(crate) fn thread_participants(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::relation::RelationType;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let root = EventId::parse(&root_event_id)
        .map_err(|_| "invalid thread root event id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let loaded = room
            .load_or_fetch_event_with_relations(
                &root,
                Some(vec![RelationType::Thread]),
                None,
            )
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;   // account/session moved on: never touch the next one
        }
        let Ok((root_event, replies)) = loaded else {
            // A failure is reported as a failure. The card keeps whatever it
            // already had rather than being handed a fabricated empty set.
            enqueue(&events, json!({
                "type": "thread_participants",
                "lifecycle": lifecycle,
                "room_id": room_id,
                "root_event_id": root_event_id,
                "ok": false,
            }));
            return;
        };

        // Root sender first, then first-appearance order among the replies.
        let mut ordered: Vec<OwnedUserId> = Vec::new();
        let mut seen: std::collections::HashSet<OwnedUserId> =
            std::collections::HashSet::new();
        let push = |sender: OwnedUserId,
                        ordered: &mut Vec<OwnedUserId>,
                        seen: &mut std::collections::HashSet<OwnedUserId>| {
            if seen.insert(sender.clone()) {
                ordered.push(sender);
            }
        };
        if let Ok(parsed) = root_event.raw().deserialize() {
            push(parsed.sender().to_owned(), &mut ordered, &mut seen);
        }
        for reply in &replies {
            if let Ok(parsed) = reply.raw().deserialize() {
                push(parsed.sender().to_owned(), &mut ordered, &mut seen);
            }
        }

        let distinct = ordered.len();
        let truncated = distinct > THREAD_PARTICIPANT_CAP;
        let mut rows: Vec<serde_json::Value> = Vec::new();
        for user_id in ordered.into_iter().take(THREAD_PARTICIPANT_CAP) {
            // Profile from the already-synced member state; no extra network
            // call per face. Absent fields stay empty and the C++ side falls
            // back to the localpart + colour avatar, never a bare MXID label.
            let member = room.get_member_no_sync(&user_id).await.ok().flatten();
            rows.push(json!({
                "user_id": user_id.to_string(),
                "display_name": member
                    .as_ref()
                    .and_then(|m| m.display_name().map(|s| s.to_owned()))
                    .unwrap_or_default(),
                "avatar_url": member
                    .as_ref()
                    .and_then(|m| m.avatar_url().map(|a| a.to_string()))
                    .unwrap_or_default(),
            }));
        }
        enqueue(&events, json!({
            "type": "thread_participants",
            "lifecycle": lifecycle,
            "room_id": room_id,
            "root_event_id": root_event_id,
            "ok": true,
            "participants": rows,
            "distinct": distinct,
            "truncated": truncated,
        }));
    });
    Ok(())
}

/// How many edit events one "remove edits" pass will redact. An edit chain
/// this long is already pathological; the report says honestly how many were
/// removed, so a longer chain simply needs a second pass rather than an
/// unbounded burst of redactions.
const EDIT_REDACTION_CAP: usize = 50;

/// 2026-08-18 tester request ("add function remove all edits").
///
/// Matrix has no "unedit" primitive: an edit is a separate `m.replace` event,
/// and the ONLY way to take one back is to redact it. So this collects the
/// message's replacement events through
/// `Room::load_or_fetch_event_with_relations` (cache-first, exactly like the
/// thread facepile above) and redacts them, which returns the message to its
/// ORIGINAL text — including dropping the "edited" marker, since the marker
/// is derived from the presence of those events, not stored anywhere.
///
/// Only the caller's OWN edits are touched. A homeserver accepts a redaction
/// of someone else's event only with the redact power level, and quietly
/// redacting another person's edits from a message-menu entry is not what
/// this action says it does.
///
/// The result crosses as counts only — never event content.
pub(crate) fn remove_message_edits(
    bridge: &RustClient,
    room_id: String,
    target_event_id: String,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::relation::RelationType;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let target = EventId::parse(&target_event_id)
        .map_err(|_| "invalid message event id".to_owned())?;
    let own_user_id = client
        .user_id()
        .ok_or_else(|| "no active Matrix session".to_owned())?
        .to_owned();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let loaded = room
            .load_or_fetch_event_with_relations(
                &target,
                Some(vec![RelationType::Replacement]),
                None,
            )
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return; // the account moved on
        }
        let Ok((_original, relations)) = loaded else {
            enqueue(&events, json!({
                "type": "message_edits_removed",
                "lifecycle": lifecycle,
                "room_id": room_id,
                "event_id": target_event_id,
                "ok": false,
                "removed": 0,
                "failed": 0,
                "truncated": false,
            }));
            return;
        };

        // Own replacement events only, newest-first order is irrelevant: all
        // of them go. The original event is never in this list.
        let mut edit_ids: Vec<matrix_sdk::ruma::OwnedEventId> = Vec::new();
        for related in &relations {
            let Ok(parsed) = related.raw().deserialize() else {
                continue;
            };
            if parsed.sender() != own_user_id {
                continue;
            }
            let id = parsed.event_id().to_owned();
            if id == target {
                continue; // never redact the message itself
            }
            edit_ids.push(id);
        }
        let found = edit_ids.len();
        let truncated = found > EDIT_REDACTION_CAP;
        edit_ids.truncate(EDIT_REDACTION_CAP);

        let mut removed = 0usize;
        let mut failed = 0usize;
        for id in edit_ids {
            match room.redact(&id, None, None).await {
                Ok(_) => removed += 1,
                Err(_) => failed += 1,
            }
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
        }
        enqueue(&events, json!({
            "type": "message_edits_removed",
            "lifecycle": lifecycle,
            "room_id": room_id,
            "event_id": target_event_id,
            // The lookup succeeded; `removed`/`failed` say what happened.
            "ok": failed == 0,
            "removed": removed,
            "failed": failed,
            "truncated": truncated,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Member snapshot + permissions
// ---------------------------------------------------------------------------

pub(crate) fn room_members(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let own_id = client.user_id().map(|u| u.to_owned());
    bridge.spawn_room_action(async move {
        // Cache-first: an instant PARTIAL snapshot from the state store
        // (the members already known locally — typically timeline senders
        // and heroes on a lazy-loaded room) so the People list renders
        // immediately instead of sitting empty through the first
        // network-backed /members fetch. The synced full roster follows
        // under the SAME op with partial=false. An empty cache emits
        // nothing — an empty partial would read as "nobody".
        if let Ok(cached) = room
            .members_no_sync(RoomMemberships::ACTIVE | RoomMemberships::BAN)
            .await
        {
            if !cached.is_empty() && timelines.lifecycle_current(lifecycle) {
                let snapshot = members_snapshot_json(
                    &room, &cached, own_id.as_deref(), &room_id, op_id,
                    lifecycle, /*partial=*/ true,
                )
                .await;
                // Re-check after the await inside the builder (§9: the
                // guard sits immediately before the emit).
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, snapshot);
                }
            }
        }

        // ACTIVE (join+invite) plus BAN: banned members must be visible or
        // unban is unreachable from the client. They are excluded from the
        // joined/invited counts and from mention suggestions downstream.
        let members = match room
            .members(RoomMemberships::ACTIVE | RoomMemberships::BAN)
            .await
        {
            Ok(members) => members,
            Err(err) => {
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, json!({
                        "type": "room_members",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "room_id": room_id,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
                return;
            }
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let snapshot = members_snapshot_json(
            &room, &members, own_id.as_deref(), &room_id, op_id, lifecycle,
            /*partial=*/ false,
        )
        .await;
        // Guard immediately before the emit (§9) — the builder awaits a
        // store read for the own-member permissions.
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, snapshot);
        }
    });
    Ok(())
}

// One snapshot shape for both the instant cache-only (partial) emit and
// the synced full roster — sort, counts, cap, rows, own permissions.
async fn members_snapshot_json(
    room: &matrix_sdk::Room,
    members: &[matrix_sdk::room::RoomMember],
    own_id: Option<&matrix_sdk::ruma::UserId>,
    room_id: &str,
    op_id: u64,
    lifecycle: u64,
    partial: bool,
) -> serde_json::Value {
    {
        let mut sorted: Vec<&matrix_sdk::room::RoomMember> = members.iter().collect();
        sorted.sort_by(|a, b| {
            use matrix_sdk::ruma::events::room::member::MembershipState;
            // Joined first, then invited, banned last.
            let rank = |m: &matrix_sdk::room::RoomMember| match m.membership() {
                MembershipState::Join => 0,
                MembershipState::Invite => 1,
                MembershipState::Ban => 2,
                _ => 3,
            };
            rank(a)
                .cmp(&rank(b))
                .then(b.power_level().cmp(&a.power_level()))
                .then_with(|| a.name().to_lowercase().cmp(&b.name().to_lowercase()))
        });

        let mut joined_count = 0u64;
        let mut invited_count = 0u64;
        for member in &sorted {
            use matrix_sdk::ruma::events::room::member::MembershipState;
            match member.membership() {
                MembershipState::Join => joined_count += 1,
                MembershipState::Invite => invited_count += 1,
                _ => {}
            }
        }
        // `truncated` speaks about the ACTIVE roster (the population the
        // joined/invited counts and the UI notice describe). Banned
        // members sort last, so they are the first rows the cap drops —
        // silently: a ban list beyond the cap makes those bans
        // unreachable for unban, an accepted limit (review LU1/LU2).
        let truncated =
            (joined_count + invited_count) as usize > MEMBER_SNAPSHOT_CAP;

        let rows: Vec<serde_json::Value> = sorted
            .iter()
            .take(MEMBER_SNAPSHOT_CAP)
            .map(|member| {
                use matrix_sdk::ruma::events::room::member::MembershipState;
                let role = format!("{:?}", member.suggested_role_for_power_level())
                    .to_lowercase();
                json!({
                    "user_id": member.user_id().to_string(),
                    "display_name": member.display_name().unwrap_or_default(),
                    "avatar_url": member
                        .avatar_url()
                        .map(|a| a.to_string())
                        .unwrap_or_default(),
                    "membership": match member.membership() {
                        MembershipState::Join => "joined",
                        MembershipState::Invite => "invited",
                        MembershipState::Ban => "banned",
                        _ => "other",
                    },
                    "role": role,
                    // Raw power level so the UI can hide moderation actions
                    // against peers at or above the viewer's own level (the
                    // server enforces regardless; this only avoids offering
                    // an action that must fail).
                    "power_level": power_level_int(member.power_level()),
                    "ambiguous": member.name_ambiguous(),
                    "is_own": Some(member.user_id()) == own_id,
                })
            })
            .collect();

        // Own permissions, from the SDK's power-level helpers — never
        // guessed from role labels or room-creator status.
        let own_member = match own_id {
            Some(own) => room.get_member_no_sync(own).await.ok().flatten(),
            None => None,
        };
        let can_invite = own_member.as_ref().is_some_and(|m| m.can_invite());
        let can_edit_name = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomName));
        let can_edit_topic = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomTopic));
        let can_edit_avatar = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomAvatar));
        // @room: the room's OWN required level for a whole-room notification
        // (notifications.room, default 50), asked of the SDK rather than
        // assumed to be "moderator". A room may set it to anything.
        let can_notify_room = own_member.as_ref().is_some_and(|m| {
            m.can_do(PowerLevelAction::TriggerNotification(
                NotificationPowerLevelType::Room,
            ))
        });
        let can_kick = own_member.as_ref().is_some_and(|m| m.can_kick());
        let can_ban = own_member.as_ref().is_some_and(|m| m.can_ban());
        // Unban's required level is max(ban, kick) (ruma
        // PowerLevelAction::Unban), NOT the ban level alone — ask the SDK
        // rather than deriving it (review MU1).
        let can_unban = own_member
            .as_ref()
            .is_some_and(|m| m.can_do(PowerLevelAction::Unban));
        let own_power_level = own_member
            .as_ref()
            .map(|m| power_level_int(m.power_level()))
            .unwrap_or(0);
        // v0.7.x room administration. Each of these is the SDK's own
        // power-level check against the REAL required level for that state
        // event — a room may define any level it likes for any of them, so
        // nothing here assumes "admin only".
        let can_change_power_levels = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomPowerLevels));
        let can_pin = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomPinnedEvents));
        let can_change_join_rule = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomJoinRules));
        let can_change_alias = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomCanonicalAlias));
        // 2026-08-19: Space child management (add/remove/suggest all ride
        // m.space.child) — the same real-required-level rule as the rest.
        let can_manage_space_children = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::SpaceChild));
        // 2026-08-26: the room's REAL m.room.power_levels thresholds, read
        // ONCE (this used to be a `power_levels_or_default()` call solely
        // for `users_default`).
        //
        // Until now only the derived `own_can_*` booleans crossed, so a
        // Permissions surface could say whether YOU may do a thing and never
        // what the room REQUIRES for it — every row of a power-level matrix
        // had no source of truth on the C++ side, read OR write.
        //
        // ONLY INTEGERS UNDER A FIXED SET OF KEYS CROSS. The `events` map is
        // keyed by event TYPE, written by whoever last sent the state event,
        // i.e. an unbounded sender-chosen string: it must never reach the
        // bridge. Each row is therefore looked up by a TYPED
        // `StateEventType` and emitted under a key chosen here.
        //
        // Each per-event row is the EFFECTIVE level — the explicit entry
        // when the room has one, otherwise `state_default`, which is what
        // the server actually enforces. A row that merely inherits the
        // default is indistinguishable from an explicit one here, on
        // purpose: the number a person needs is the one that applies.
        let power_levels = room.power_levels_or_default().await;
        let state_level = |ty: StateEventType| -> i64 {
            power_levels
                .events
                .get(&TimelineEventType::from(ty))
                .map(|value| i64::from(*value))
                .unwrap_or_else(|| i64::from(power_levels.state_default))
        };
        // The room's default user level: without it the UI cannot tell a
        // member sitting AT the default from one explicitly pinned to the
        // same number, and `update_power_levels` treats a set-to-default as
        // a removal from the users map. Rooms may set it to any value.
        let users_default: i64 = i64::from(power_levels.users_default);
        // The room version, for the Advanced / Upgrade disclosure. Read from
        // the SDK (`Room::version()`), never parsed out of m.room.create by
        // hand; empty when the room state has not settled yet, which the UI
        // must render as nothing rather than as a fabricated "1".
        let room_version = room
            .version()
            .map(|version| version.to_string())
            .unwrap_or_default();
        // Whether this account may send m.room.tombstone — the level that
        // governs an UPGRADE. It is not among the flags computed above, and
        // an upgrade is irreversible, so it is reported separately and
        // deliberately gates nothing but a disclosure today.
        let can_upgrade = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomTombstone));
        let join_rule = join_rule_str(room.join_rule().as_ref());
        let canonical_alias = room
            .canonical_alias()
            .map(|a| a.to_string())
            .unwrap_or_default();
        // v0.9 room access (phase 4). The restricted allow list is reported
        // as ROOM IDS so the UI can render a configuration another client
        // wrote and edit it without dropping entries — an allow rule of an
        // unknown kind is carried through as an opaque marker the editor
        // must preserve (see set_room_join_rule).
        let (restricted_allow, restricted_has_unknown) = {
            use matrix_sdk::ruma::room::{AllowRule, JoinRule};
            match room.join_rule() {
                Some(JoinRule::Restricted(r)) | Some(JoinRule::KnockRestricted(r)) => {
                    let mut ids = Vec::new();
                    let mut unknown = false;
                    for rule in &r.allow {
                        match rule {
                            AllowRule::RoomMembership(m) => {
                                ids.push(m.room_id.to_string())
                            }
                            _ => unknown = true,
                        }
                    }
                    (ids, unknown)
                }
                _ => (Vec::new(), false),
            }
        };
        let history_visibility = match room.history_visibility() {
            Some(v) => v.as_str().to_owned(),
            None => String::new(),
        };
        let guest_access = room.guest_access().as_str().to_owned();
        let alt_aliases: Vec<String> =
            room.alt_aliases().iter().map(|a| a.to_string()).collect();
        let can_change_history_visibility = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomHistoryVisibility));
        let can_change_guest_access = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomGuestAccess));

        // Built BEFORE the snapshot rather than inline in it. `json!` expands
        // recursively, and this object pushed the already-large snapshot past
        // serde_json's macro recursion limit ("recursion limit reached while
        // expanding `$crate::json_internal!`") — a compile error, not a
        // runtime one, and one that says nothing about which key caused it.
        // Hoisting any nested object out of that macro is the fix.
        let power_levels_json = json!({
            "ban": i64::from(power_levels.ban),
            "invite": i64::from(power_levels.invite),
            "kick": i64::from(power_levels.kick),
            "redact": i64::from(power_levels.redact),
            "events_default": i64::from(power_levels.events_default),
            "state_default": i64::from(power_levels.state_default),
            "users_default": users_default,
            "m.space.child": state_level(StateEventType::SpaceChild),
            "m.room.name": state_level(StateEventType::RoomName),
            "m.room.avatar": state_level(StateEventType::RoomAvatar),
            "m.room.topic": state_level(StateEventType::RoomTopic),
            "m.room.join_rules": state_level(StateEventType::RoomJoinRules),
            "m.room.canonical_alias":
                state_level(StateEventType::RoomCanonicalAlias),
            "m.room.power_levels":
                state_level(StateEventType::RoomPowerLevels),
            "m.room.tombstone": state_level(StateEventType::RoomTombstone),
            "m.room.history_visibility":
                state_level(StateEventType::RoomHistoryVisibility),
            "m.room.guest_access": state_level(StateEventType::RoomGuestAccess),
        });
        // Hoisted for the same macro-recursion reason as the power levels.
        let access_json = json!({
            "history_visibility": history_visibility,
            "guest_access": guest_access,
            "alt_aliases": alt_aliases,
            "restricted_allow": restricted_allow,
            "restricted_has_unknown": restricted_has_unknown,
            "own_can_change_history_visibility": can_change_history_visibility,
            "own_can_change_guest_access": can_change_guest_access,
        });

        json!({
            "type": "room_members",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": true,
            // A partial snapshot keeps the op OPEN on the C++ side: the
            // panel renders it but stays "loading" until the synced
            // roster lands under the same op.
            "partial": partial,
            "truncated": truncated,
            "joined_count": joined_count,
            "invited_count": invited_count,
            "own_can_invite": can_invite,
            "own_can_edit_name": can_edit_name,
            "own_can_edit_topic": can_edit_topic,
            "own_can_edit_avatar": can_edit_avatar,
            "own_can_kick": can_kick,
            "own_can_notify_room": can_notify_room,
            "own_can_ban": can_ban,
            "own_can_unban": can_unban,
            "own_can_change_power_levels": can_change_power_levels,
            "own_can_pin": can_pin,
            "own_can_change_join_rule": can_change_join_rule,
            "own_can_change_alias": can_change_alias,
            "own_can_manage_space_children": can_manage_space_children,
            "own_power_level": own_power_level,
            "users_default_power_level": users_default,
            "own_can_upgrade": can_upgrade,
            "room_version": room_version,
            // Every key here is one this file chose; see the comment above.
            // The C++ side mirrors them verbatim into the Permissions matrix.
            "power_levels": power_levels_json,
            "join_rule": join_rule,
            "canonical_alias": canonical_alias,
            "access": access_json,
            "members": rows,
        })
    }
}

// ---------------------------------------------------------------------------
// Moderation (kick / ban)
// ---------------------------------------------------------------------------

// UserPowerLevel → bridge integer. MSC4289 room creators are "Infinite";
// they map to a sentinel every finite room power level sits below, chosen
// to survive the JSON f64 hop exactly. Known accepted edges: a pathological
// explicit power level above 1e9 would outrank a creator here, and the
// value is consumed as a 64-bit integer on the C++ side — neither occurs
// in real deployments.
fn power_level_int(
    level: matrix_sdk::ruma::events::room::power_levels::UserPowerLevel,
) -> i64 {
    use matrix_sdk::ruma::events::room::power_levels::UserPowerLevel;
    match level {
        UserPowerLevel::Infinite => 1_000_000_000,
        UserPowerLevel::Int(v) => v.into(),
        // The enum is non_exhaustive; an unknown future variant reads as
        // an ordinary member rather than inventing power.
        _ => 0,
    }
}

// Kick, ban or unban one user through the SDK's own moderation calls
// (`op`: 0 = kick, 1 = ban, 2 = unban). Power-level enforcement is the
// SERVER'S; the client only avoids offering actions that must fail and
// surfaces the result honestly. An empty reason means "no reason given".
// Result event:
// room_moderation_result { op_id, room_id, user_id, op, ok, category }.
pub(crate) fn moderate_member(
    bridge: &RustClient,
    room_id: String,
    user_id: String,
    reason: String,
    op: u8,
    op_id: u64,
) -> Result<(), String> {
    // Parse once into an exhaustive enum so the dispatch below has no
    // fallthrough arm — a dispatcher of destructive membership actions
    // must refuse an unknown op, never default to one (review LU4).
    enum ModOp {
        Kick,
        Ban,
        Unban,
    }
    let (mod_op, op_name) = match op {
        0 => (ModOp::Kick, "kick"),
        1 => (ModOp::Ban, "ban"),
        2 => (ModOp::Unban, "unban"),
        _ => return Err("invalid moderation op".to_owned()),
    };
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let uid = UserId::parse(&user_id).map_err(|_| "invalid user id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let reason_opt = (!reason.is_empty()).then_some(reason.as_str());
        let result = match mod_op {
            ModOp::Kick => room.kick_user(&uid, reason_opt).await,
            ModOp::Ban => room.ban_user(&uid, reason_opt).await,
            ModOp::Unban => room.unban_user(&uid, reason_opt).await,
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_moderation_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "user_id": user_id,
            "op": op_name,
            "ok": result.is_ok(),
            "category": result
                .err()
                .map(|err| classify_room_error(&err.to_string()))
                .unwrap_or(""),
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Room administration (v0.7.x): member power levels, join rule, alias
// ---------------------------------------------------------------------------

/// Coarse join-rule label for the bridge. `restricted` / `knock_restricted`
/// cross so the UI can DISPLAY them honestly; Lightning does not offer to
/// SET them (they carry an allow-rule list that needs a space picker — a
/// documented follow-up, not something to fake with an empty list, which
/// would lock the room to invite-only while claiming otherwise).
fn join_rule_str(rule: Option<&matrix_sdk::ruma::room::JoinRule>) -> &'static str {
    use matrix_sdk::ruma::room::JoinRule;
    match rule {
        Some(JoinRule::Invite) => "invite",
        Some(JoinRule::Public) => "public",
        Some(JoinRule::Knock) => "knock",
        Some(JoinRule::Private) => "private",
        Some(JoinRule::Restricted(_)) => "restricted",
        Some(JoinRule::KnockRestricted(_)) => "knock_restricted",
        // Unknown/custom or not yet synced: the UI renders nothing rather
        // than guessing a rule the room may not have.
        _ => "",
    }
}

/// Set ONE member's power level through the SDK's `update_power_levels`,
/// which reads the room's real `m.room.power_levels`, applies exactly this
/// user's change and sends the whole content back. Every other user's level
/// — including arbitrary custom numbers — is carried through untouched, and
/// a level equal to the room's `users_default` is REMOVED from the users map
/// rather than written redundantly (SDK behaviour, and the correct Matrix
/// semantics).
///
/// Permission is the SERVER'S to enforce; the client only avoids offering an
/// action that must fail. Result event: room_power_level_result.
pub(crate) fn set_member_power_level(
    bridge: &RustClient,
    room_id: String,
    user_id: String,
    level: i64,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::Int;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let uid = UserId::parse(&user_id).map_err(|_| "invalid user id".to_owned())?;
    // Refuse out-of-range before spawning: `Int` is the JSON-safe integer
    // range, and a value outside it cannot be a real Matrix power level.
    let target_level =
        Int::try_from(level).map_err(|_| "power level out of range".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .update_power_levels(vec![(uid.as_ref(), target_level)])
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_power_level_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "user_id": user_id,
            "level": level,
            "ok": result.is_ok(),
            "category": result
                .err()
                .map(|err| classify_room_error(&err.to_string()))
                .unwrap_or(""),
        }));
    });
    Ok(())
}

/// 2026-08-26: set ONE threshold in the room's `m.room.power_levels` — the
/// Permissions matrix (Sable parity). `key` is one of a FIXED allowlist;
/// anything else is refused at this edge, so no caller can turn this into a
/// generic "write an arbitrary event type's level" primitive.
///
/// Two write paths, both a read-modify-send of the WHOLE content, which is
/// what Matrix requires (the event has no partial update):
///   * the seven scalar thresholds plus name/avatar/topic/space_child go
///     through the SDK's own `apply_power_level_changes`, which leaves every
///     field it was not given untouched — including per-event entries that
///     happen to equal the new default, deliberately (matrix-sdk
///     room/power_levels.rs:141: removing them would grant unintended
///     privileges when the default is changed in isolation);
///   * the remaining rows are per-event-type levels that
///     `RoomPowerLevelChanges` cannot express, so they take the same
///     read-modify-send by hand into `events`.
///
/// The event type is always a TYPED `StateEventType`, never `key` reinterpreted
/// as a wire string: ruma's event-type enums carry ALIASES (a string that
/// parses as one identifier and serializes back as another), so a hand-built
/// type could silently govern an event nobody sends.
///
/// `m.call.member` is NOT in the allowlist. The identifier Lightning actually
/// sends today is the MSC3401 unstable one (`rust/src/rtc.rs`), ruma aliases
/// the stable name onto it, and a Space has no timeline to hold a call — a row
/// that governs neither string honestly is worse than a missing row.
///
/// Permission remains the SERVER'S to enforce; the client only avoids
/// offering a write that must fail. Result event: room_power_matrix_result.
pub(crate) fn set_room_power_level_key(
    bridge: &RustClient,
    room_id: String,
    key: String,
    level: i64,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::room::power_levels::RoomPowerLevelChanges;
    use matrix_sdk::ruma::events::room::power_levels::RoomPowerLevelsEventContent;
    use matrix_sdk::ruma::Int;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    // Refuse out-of-range before spawning: `Int` is the JSON-safe integer
    // range, and a value outside it cannot be a real Matrix power level.
    let target_level =
        Int::try_from(level).map_err(|_| "power level out of range".to_owned())?;

    let mut changes = RoomPowerLevelChanges::new();
    let mut event_type: Option<StateEventType> = None;
    match key.as_str() {
        "ban" => changes.ban = Some(level),
        "invite" => changes.invite = Some(level),
        "kick" => changes.kick = Some(level),
        "redact" => changes.redact = Some(level),
        "events_default" => changes.events_default = Some(level),
        "state_default" => changes.state_default = Some(level),
        "users_default" => changes.users_default = Some(level),
        "m.room.name" => changes.room_name = Some(level),
        "m.room.avatar" => changes.room_avatar = Some(level),
        "m.room.topic" => changes.room_topic = Some(level),
        "m.space.child" => changes.space_child = Some(level),
        "m.room.join_rules" => event_type = Some(StateEventType::RoomJoinRules),
        "m.room.canonical_alias" => {
            event_type = Some(StateEventType::RoomCanonicalAlias)
        }
        "m.room.power_levels" => event_type = Some(StateEventType::RoomPowerLevels),
        "m.room.tombstone" => event_type = Some(StateEventType::RoomTombstone),
        _ => return Err("unsupported power level key".to_owned()),
    }

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let key_for_event = key.clone();
    bridge.spawn_room_action(async move {
        let result = async {
            match event_type {
                Some(state_type) => {
                    let mut levels = room
                        .power_levels()
                        .await
                        .map_err(|err| {
                            classify_room_error(&err.to_string()).to_owned()
                        })?;
                    levels
                        .events
                        .insert(TimelineEventType::from(state_type), target_level);
                    let content = RoomPowerLevelsEventContent::try_from(levels)
                        .map_err(|err| {
                            classify_room_error(&err.to_string()).to_owned()
                        })?;
                    room.send_state_event(content)
                        .await
                        .map(|_| ())
                        .map_err(|err| {
                            classify_room_error(&err.to_string()).to_owned()
                        })
                }
                None => room
                    .apply_power_level_changes(changes)
                    .await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned()),
            }
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_power_matrix_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "key": key_for_event,
            "level": level,
            "ok": result.is_ok(),
            "category": result.err().unwrap_or_default(),
        }));
    });
    Ok(())
}

/// Set the room's join rule.
///
/// v0.9 (phase 4): `restricted` and `knock_restricted` are accepted with
/// `allowed_room_ids` — the Spaces (or rooms) whose members may join, each
/// becoming an `m.room_membership` allow rule. UNKNOWN allow-rule kinds a
/// previous client wrote are PRESERVED verbatim from the current state,
/// never dropped: editing which spaces grant access must not silently strip
/// a rule this client cannot render. An empty allow list is refused — it
/// would lock the room to invite-only while claiming otherwise.
pub(crate) fn set_room_join_rule(
    bridge: &RustClient,
    room_id: String,
    rule: String,
    allowed_room_ids: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::join_rules::RoomJoinRulesEventContent;
    use matrix_sdk::ruma::room::{AllowRule, JoinRule, Restricted};

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let restricted = || -> Result<Restricted, String> {
        let mut allow: Vec<AllowRule> = Vec::new();
        for id in &allowed_room_ids {
            let room_id = RoomId::parse(id)
                .map_err(|_| "invalid allowed room id".to_owned())?;
            allow.push(AllowRule::room_membership(room_id));
        }
        // Carry through every rule of a kind this client does not render.
        if let Some(JoinRule::Restricted(current))
        | Some(JoinRule::KnockRestricted(current)) = room.join_rule()
        {
            for existing in current.allow {
                if !matches!(existing, AllowRule::RoomMembership(_)) {
                    allow.push(existing);
                }
            }
        }
        if allow.is_empty() {
            return Err("a restricted room needs at least one allowed space".to_owned());
        }
        Ok(Restricted::new(allow))
    };
    let join_rule = match rule.as_str() {
        "invite" => JoinRule::Invite,
        "public" => JoinRule::Public,
        "knock" => JoinRule::Knock,
        "restricted" => JoinRule::Restricted(restricted()?),
        "knock_restricted" => JoinRule::KnockRestricted(restricted()?),
        _ => return Err("unsupported join rule".to_owned()),
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .send_state_event(RoomJoinRulesEventContent::new(join_rule))
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "join_rule", result);
    });
    Ok(())
}

/// Set (or clear, with an empty `alias`) the room's canonical alias.
///
/// Two steps, because Matrix separates the directory MAPPING from the room's
/// own state: the alias must resolve to this room on the server before
/// `m.room.canonical_alias` may name it (a server rejects a canonical alias
/// it cannot resolve). So an alias that does not already point here is
/// published first via `Client::create_room_alias`, and only then does the
/// state event go out. Clearing sends the state event with no alias and
/// deliberately does NOT delete the directory mapping — removing a published
/// alias is a separate, more destructive action than demoting it.
pub(crate) fn set_room_canonical_alias(
    bridge: &RustClient,
    room_id: String,
    alias: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::canonical_alias::RoomCanonicalAliasEventContent;
    use matrix_sdk::ruma::RoomAliasId;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let trimmed = alias.trim().to_owned();
    let parsed = if trimmed.is_empty() {
        None
    } else {
        Some(
            RoomAliasId::parse(&trimmed)
                .map_err(|_| "invalid room alias".to_owned())?,
        )
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let room_id_for_task = room_id.clone();
    bridge.spawn_room_action(async move {
        let result = async {
            // Keep every alternative alias the room already advertises; this
            // action promotes one alias, it does not rewrite the list.
            let alt_aliases = room.alt_aliases();
            if let Some(alias) = parsed.as_deref() {
                let already_here = match client.resolve_room_alias(alias).await {
                    Ok(response) => response.room_id.as_str() == room_id_for_task,
                    // Not resolvable yet: publish it below. Any other
                    // failure also falls through to the create attempt,
                    // whose own error is what gets reported.
                    Err(_) => false,
                };
                if !already_here {
                    let room_id_parsed = room.room_id();
                    client
                        .create_room_alias(alias, room_id_parsed)
                        .await
                        .map_err(|err| {
                            classify_room_error(&err.to_string()).to_owned()
                        })?;
                }
            }
            let mut content = RoomCanonicalAliasEventContent::new();
            content.alias = parsed.clone();
            content.alt_aliases = alt_aliases;
            room.send_state_event(content)
                .await
                .map(|_| ())
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(
            &events, op_id, lifecycle, &room_id, "canonical_alias", result,
        );
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// v0.9 room access (phase 4): history visibility, guest access, directory
// visibility, alternative aliases. Every write is the SDK's own request
// (RoomPrivacySettings or a plain state event); the client only chooses
// values and reports the server's answer.
// ---------------------------------------------------------------------------

pub(crate) fn set_room_history_visibility(
    bridge: &RustClient,
    room_id: String,
    visibility: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility;
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let value = match visibility.as_str() {
        "invited" => HistoryVisibility::Invited,
        "joined" => HistoryVisibility::Joined,
        "shared" => HistoryVisibility::Shared,
        "world_readable" => HistoryVisibility::WorldReadable,
        _ => return Err("unsupported history visibility".to_owned()),
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .privacy_settings()
            .update_room_history_visibility(value)
            .await
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(
            &events, op_id, lifecycle, &room_id, "history_visibility", result,
        );
    });
    Ok(())
}

pub(crate) fn set_room_guest_access(
    bridge: &RustClient,
    room_id: String,
    access: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::guest_access::{
        GuestAccess, RoomGuestAccessEventContent,
    };
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let value = match access.as_str() {
        "can_join" => GuestAccess::CanJoin,
        "forbidden" => GuestAccess::Forbidden,
        _ => return Err("unsupported guest access".to_owned()),
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .send_state_event(RoomGuestAccessEventContent::new(value))
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "guest_access", result);
    });
    Ok(())
}

/// Directory visibility is not room state — it lives on the server's
/// public room list — so it is fetched on demand and answered as its own
/// poll event rather than riding the member snapshot.
pub(crate) fn request_room_directory_visibility(
    bridge: &RustClient,
    room_id: String,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::room::Visibility;
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let answer = room.privacy_settings().get_room_visibility().await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match answer {
            Ok(visibility) => enqueue(
                &events,
                json!({
                    "type": "room_directory_visibility",
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": true,
                    "published": visibility == Visibility::Public,
                }),
            ),
            Err(err) => enqueue(
                &events,
                json!({
                    "type": "room_directory_visibility",
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }),
            ),
        }
    });
    Ok(())
}

pub(crate) fn set_room_directory_visibility(
    bridge: &RustClient,
    room_id: String,
    published: bool,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::room::Visibility;
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .privacy_settings()
            .update_room_visibility(if published {
                Visibility::Public
            } else {
                Visibility::Private
            })
            .await
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(
            &events, op_id, lifecycle, &room_id, "directory_visibility", result,
        );
    });
    Ok(())
}

/// Replace the alternative-alias list. Same two-step rule as the canonical
/// alias: an alias must resolve to this room before m.room.canonical_alias
/// may list it, so any alias not already pointing here is published first.
/// The canonical alias is preserved untouched, and a removed alias keeps its
/// directory mapping (demoting is not deleting — see the canonical path).
pub(crate) fn set_room_alt_aliases(
    bridge: &RustClient,
    room_id: String,
    aliases: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::canonical_alias::RoomCanonicalAliasEventContent;
    use matrix_sdk::ruma::{OwnedRoomAliasId, RoomAliasId};

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let mut parsed: Vec<OwnedRoomAliasId> = Vec::new();
    for alias in &aliases {
        let trimmed = alias.trim();
        if trimmed.is_empty() {
            continue;
        }
        let id = RoomAliasId::parse(trimmed).map_err(|_| "invalid room alias".to_owned())?;
        if !parsed.contains(&id) {
            parsed.push(id);
        }
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let room_id_for_task = room_id.clone();
    bridge.spawn_room_action(async move {
        let result = async {
            for alias in &parsed {
                let already_here = match client.resolve_room_alias(alias).await {
                    Ok(response) => response.room_id.as_str() == room_id_for_task,
                    Err(_) => false,
                };
                if !already_here {
                    client
                        .create_room_alias(alias, room.room_id())
                        .await
                        .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                }
            }
            let mut content = RoomCanonicalAliasEventContent::new();
            content.alias = room.canonical_alias();
            content.alt_aliases = parsed;
            room.send_state_event(content)
                .await
                .map(|_| ())
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "alt_aliases", result);
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// v0.9 room upgrade (phase 8). The version list comes from the homeserver's
// /capabilities (never a hard-coded table), the upgrade is the standard
// /upgrade endpoint via ruma, and the server is what carries state, power
// levels and aliases into the replacement (the endpoint's contract); this
// client never approximates that by hand.
// ---------------------------------------------------------------------------

pub(crate) fn request_room_versions(bridge: &RustClient) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::discovery::get_capabilities::v3::RoomVersionStability;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let answer = client.homeserver_capabilities().room_versions().await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match answer {
            Ok(versions) => {
                let mut available: Vec<serde_json::Value> = versions
                    .available
                    .iter()
                    .map(|(id, stability)| {
                        json!({
                            "version": id.to_string(),
                            "stable": *stability == RoomVersionStability::Stable,
                        })
                    })
                    .collect();
                // Sort numerically where the ids are numbers ("1".."12"),
                // otherwise lexically after them, so the picker reads in
                // order and the recommendation is deterministic.
                available.sort_by(|a, b| {
                    let key = |v: &serde_json::Value| {
                        let s = v["version"].as_str().unwrap_or_default();
                        (s.parse::<u32>().map(|n| (0, n)).unwrap_or((1, 0)),
                         s.to_owned())
                    };
                    key(a).cmp(&key(b))
                });
                enqueue(
                    &events,
                    json!({
                        "type": "room_versions",
                        "lifecycle": lifecycle,
                        "ok": true,
                        "default": versions.default.to_string(),
                        "available": available,
                    }),
                );
            }
            Err(err) => enqueue(
                &events,
                json!({
                    "type": "room_versions",
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }),
            ),
        }
    });
    Ok(())
}

pub(crate) fn upgrade_room(
    bridge: &RustClient,
    room_id: String,
    new_version: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::room::upgrade_room;
    use matrix_sdk::ruma::RoomVersionId;
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let version: RoomVersionId = new_version
        .trim()
        .parse()
        .map_err(|_| "invalid room version".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = upgrade_room::v3::Request::new(room.room_id().to_owned(), version);
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match answer {
            Ok(response) => enqueue(
                &events,
                json!({
                    "type": "room_upgrade_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": true,
                    "replacement_room_id": response.replacement_room.to_string(),
                }),
            ),
            Err(err) => enqueue(
                &events,
                json!({
                    "type": "room_upgrade_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": false,
                    "category": classify_room_error(&err.to_string()),
                }),
            ),
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Room profile editing + leave
// ---------------------------------------------------------------------------

fn emit_edit_result(
    events: &crate::EventQueueRef,
    op_id: u64,
    lifecycle: u64,
    room_id: &str,
    field: &str,
    result: Result<(), String>,
) {
    enqueue(events, json!({
        "type": "room_edit_result",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "room_id": room_id,
        "field": field,
        "ok": result.is_ok(),
        "category": result.err().unwrap_or_default(),
    }));
}

pub(crate) fn set_room_name(
    bridge: &RustClient,
    room_id: String,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    if name.trim().is_empty() {
        return Err("room name must not be empty".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .set_name(name.trim().to_owned())
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "name", result);
    });
    Ok(())
}

pub(crate) fn set_room_topic(
    bridge: &RustClient,
    room_id: String,
    topic: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .set_room_topic(topic.trim())
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "topic", result);
    });
    Ok(())
}

pub(crate) fn set_room_avatar(
    bridge: &RustClient,
    room_id: String,
    local_path: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let metadata = std::fs::metadata(&local_path)
        .map_err(|_| "avatar file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("avatar path is not a regular file".to_owned());
    }
    if metadata.len() == 0 || metadata.len() > MAX_AVATAR_BYTES {
        return Err("avatar file size is out of range".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = async {
            let data = tokio::fs::read(&local_path)
                .await
                .map_err(|_| "read_failed".to_owned())?;
            let mime_str =
                sniff_image_mime(&data).ok_or_else(|| "unsupported_image".to_owned())?;
            let mime: mime::Mime =
                mime_str.parse().map_err(|_| "unsupported_image".to_owned())?;
            room.upload_avatar(&mime, data, None)
                .await
                .map(|_| ())
                .map_err(|err| classify_room_error(&err.to_string()).to_owned())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "avatar", result);
    });
    Ok(())
}

pub(crate) fn remove_room_avatar(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .remove_avatar()
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()).to_owned());
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        emit_edit_result(&events, op_id, lifecycle, &room_id, "avatar", result);
    });
    Ok(())
}

pub(crate) fn leave_room(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room.leave().await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_leave_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": result.is_ok(),
            "category": result
                .err()
                .map(|err| classify_room_error(&err.to_string()))
                .unwrap_or(""),
        }));
        crate::enqueue_rooms(&events, &client).await;
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Attachment sending
// ---------------------------------------------------------------------------

/// Image metadata for the media event; dimensions come from C++ (bounded
/// Qt-side decode), size from the filesystem.
fn image_info(width: u64, height: u64, size: u64, animated: bool) -> Option<AttachmentInfo> {
    if width == 0 && height == 0 {
        return None;
    }
    Some(AttachmentInfo::Image(BaseImageInfo {
        width: UInt::new(width),
        height: UInt::new(height),
        size: UInt::new(size),
        blurhash: None,
        is_animated: Some(animated),
    }))
}

/// Typed attachment metadata for every send. Non-image sends used to go
/// out with `info: None` — no size, dimensions, or duration on the wire —
/// which broke every receiver-side feature keyed on declared metadata:
/// Lightning's own bounded playable prefetch and first-frame poster both
/// (deliberately) decline when an event declares no size, so every
/// Lightning-sent video rendered as a bare placeholder forever. The size
/// is authoritative here (filesystem / byte count); dimensions and
/// duration are best-effort from the caller and omitted when unknown
/// rather than fabricated.
pub(crate) fn attachment_info(
    mime: &str,
    width: u64,
    height: u64,
    size: u64,
) -> Option<AttachmentInfo> {
    if mime.starts_with("image/") {
        return image_info(width, height, size, mime == "image/gif");
    }
    if mime.starts_with("video/") {
        return Some(AttachmentInfo::Video(attachment::BaseVideoInfo {
            duration: None,
            height: UInt::new(height).filter(|v| u64::from(*v) > 0),
            width: UInt::new(width).filter(|v| u64::from(*v) > 0),
            size: UInt::new(size),
            blurhash: None,
        }));
    }
    if mime.starts_with("audio/") {
        return Some(AttachmentInfo::Audio(attachment::BaseAudioInfo {
            duration: None,
            size: UInt::new(size),
            waveform: None,
        }));
    }
    Some(AttachmentInfo::File(attachment::BaseFileInfo {
        size: UInt::new(size),
    }))
}

/// v0.7 video round: the send-side poster handed over by C++.
///
/// The bytes come from a frame Lightning decoded itself out of the file the
/// user picked (VideoPosterExtractor). They are re-validated here by MAGIC
/// SNIFFING, never by the caller's label: `into_thumbnail` refuses anything
/// that is not a supported raster, is unreasonably large, or declares
/// nonsense dimensions, and a refusal degrades to "no thumbnail" rather
/// than failing the video send. No mime crosses the FFI at all: the
/// content type on the event is the SNIFFED one, so the thumbnail can
/// never advertise a type its bytes are not.
pub(crate) struct PosterBytes {
    pub data: Vec<u8>,
    pub width: u64,
    pub height: u64,
}

/// A poster is a small timeline cover (VideoPosterExtractor emits a 640px
/// JPEG). Anything beyond this is not a poster; refuse rather than upload a
/// second full-size image alongside the video.
const MAX_POSTER_BYTES: usize = 2 * 1024 * 1024;

impl PosterBytes {
    fn into_thumbnail(self) -> Option<Thumbnail> {
        if self.data.is_empty() || self.data.len() > MAX_POSTER_BYTES {
            return None;
        }
        if self.width == 0 || self.height == 0 {
            return None;
        }
        // The bytes decide the type — SVG and every non-raster is refused
        // here exactly as it is on the saved-media path.
        let sniffed = sniff_image_mime(&self.data)?;
        let content_type: mime::Mime = sniffed.parse().ok()?;
        let size = UInt::new(self.data.len() as u64)?;
        Some(Thumbnail {
            data: self.data,
            content_type,
            width: UInt::new(self.width)?,
            height: UInt::new(self.height)?,
            size,
        })
    }
}

/// Video metadata for an outgoing video event. Same shape as the generic
/// `attachment_info` video arm, plus the duration Lightning learned while
/// decoding the poster frame. Unknown values are omitted, never fabricated.
fn video_info(
    width: u64,
    height: u64,
    size: u64,
    duration_ms: u64,
) -> AttachmentInfo {
    AttachmentInfo::Video(attachment::BaseVideoInfo {
        duration: (duration_ms > 0)
            .then(|| std::time::Duration::from_millis(duration_ms)),
        height: UInt::new(height).filter(|v| u64::from(*v) > 0),
        width: UInt::new(width).filter(|v| u64::from(*v) > 0),
        size: UInt::new(size),
        blurhash: None,
    })
}

/// MSC3245 voice-message metadata. The SDK converts AttachmentInfo::Voice
/// into the `org.matrix.msc3245.voice` marker plus the
/// `org.matrix.msc1767.audio` block (duration + waveform normalized to
/// 0..=1) and sends through the ordinary attachment path — uploaded and,
/// in encrypted rooms, encrypted exactly like any audio file. `waveform`
/// carries 0..=100 amplitudes (the same scale the receive path emits);
/// empty is allowed — the voice marker still applies and receivers fall
/// back to a plain progress track (the SDK emits the audio block only
/// when BOTH duration and waveform are present).
pub(crate) fn voice_info(
    duration_ms: u64,
    size: u64,
    waveform: &[u8],
) -> AttachmentInfo {
    let normalized = if waveform.is_empty() {
        None
    } else {
        Some(
            waveform
                .iter()
                .map(|v| f32::from((*v).min(100)) / 100.0)
                .collect(),
        )
    };
    AttachmentInfo::Voice(attachment::BaseAudioInfo {
        duration: Some(std::time::Duration::from_millis(duration_ms)),
        size: UInt::new(size),
        waveform: normalized,
    })
}

/// v0.7 voice round: send a recorded voice message. Same validation and
/// routing as send_attachment_path; only the info differs (see voice_info).
/// The waveform is bounded by the FFI layer; duration must be real — a
/// zero-length recording is a caller bug, not a sendable message.
pub(crate) fn send_voice_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    duration_ms: u64,
    waveform: Vec<u8>,
    op_id: u64,
) -> Result<(), String> {
    let metadata = std::fs::metadata(&path)
        .map_err(|_| "voice file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("voice path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("voice file is empty".to_owned());
    }
    if duration_ms == 0 {
        return Err("voice duration is unknown".to_owned());
    }
    let info = Some(voice_info(duration_ms, metadata.len(), &waveform));
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        None,
        info,
        None,
        op_id,
    )
}

/// The thread twin of `send_voice_path`.
///
/// Validation and MSC3245 metadata are IDENTICAL — the same `voice_info`
/// builds the same `AttachmentInfo::Voice`, so a voice message sent into a
/// thread carries exactly the marker, duration and waveform a room voice
/// message does. The only difference is the routing: this goes through
/// `send_thread_attachment`, the thread-focused SDK timeline, so the event
/// carries a real `m.thread` relation to `root_event_id`. Encryption is
/// unchanged and entirely SDK-owned; nothing here touches crypto, and there
/// is deliberately no fallback to an ordinary room send — a thread voice
/// message that cannot be routed into its thread must fail, not silently
/// land in the main timeline.
pub(crate) fn send_thread_voice_path(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    path: String,
    mime: String,
    duration_ms: u64,
    waveform: Vec<u8>,
    op_id: u64,
) -> Result<(), String> {
    let metadata = std::fs::metadata(&path)
        .map_err(|_| "voice file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("voice path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("voice file is empty".to_owned());
    }
    if duration_ms == 0 {
        return Err("voice duration is unknown".to_owned());
    }
    let info = Some(voice_info(duration_ms, metadata.len(), &waveform));
    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };
    // Read the recording NOW, on the caller's thread, rather than handing
    // over a path for the spawned task to read later.
    //
    // The SDK resolves AttachmentSource::File with fs::read INSIDE the
    // spawned task — after it has resolved (and possibly BUILT) the
    // thread-focused timeline. C++ reclaims a thread recording when the
    // panel closes, which is one click away from Send, so a path handed
    // over here could be deleted before that read ever happened: the send
    // would fail with InvalidAttachmentData AND the result would be
    // suppressed by the advanced thread generation, silently losing a
    // message the user believed they had sent.
    //
    // Taking the bytes up front removes the window entirely instead of
    // racing it. A voice message is hard-bounded by the recorder (mono
    // 32 kbps Opus under a 15-minute cap, a few MB), so this is a small,
    // predictable allocation — not the unbounded read that makes the File
    // variant the right choice for ordinary attachments.
    let bytes = std::fs::read(&path)
        .map_err(|_| "voice file is not readable".to_owned())?;
    if bytes.is_empty() {
        return Err("voice file is empty".to_owned());
    }
    let filename = std::path::Path::new(&path)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "voice-message".to_owned());
    bridge.timelines.send_thread_attachment(
        &bridge.runtime,
        client,
        room_id,
        root_event_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        None,   // a voice message has no thumbnail
        op_id,
    )
}

/// v0.7 video round: send a video WITH a poster thumbnail.
///
/// Identical validation and routing to `send_attachment_path`; the only
/// differences are the richer video info (duration) and the `Thumbnail`
/// handed to the SDK. The SDK owns everything after this point: it uploads
/// the poster as its own media request, encrypts it with the payload in an
/// encrypted room, and fills `info.thumbnail_url` / `info.thumbnail_file`
/// plus `info.thumbnail_info` on the outgoing `m.video` event. A poster
/// that fails validation is DROPPED — the video still sends, exactly as it
/// did before this path existed.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_video_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    duration_ms: u64,
    poster: Option<PosterBytes>,
    op_id: u64,
) -> Result<(), String> {
    let metadata = std::fs::metadata(&path)
        .map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    let info = Some(video_info(width, height, metadata.len(), duration_ms));
    let thumbnail = poster.and_then(PosterBytes::into_thumbnail);
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        thumbnail,
        op_id,
    )
}

/// v0.7 video round: the thread twin of `send_video_path`. Routed through
/// the thread-focused SDK timeline so the m.thread relation and encryption
/// stay SDK-owned; the poster rides the same AttachmentConfig.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_thread_video_path(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    duration_ms: u64,
    poster: Option<PosterBytes>,
    op_id: u64,
) -> Result<(), String> {
    let metadata = std::fs::metadata(&path)
        .map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    let info = Some(video_info(width, height, metadata.len(), duration_ms));
    let thumbnail = poster.and_then(PosterBytes::into_thumbnail);
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };
    bridge.timelines.send_thread_attachment(
        &bridge.runtime,
        client,
        room_id,
        root_event_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        thumbnail,
        op_id,
    )
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_attachment_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    animated: bool,
    op_id: u64,
) -> Result<(), String> {
    let metadata =
        std::fs::metadata(&path).map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    // `animated` marks GIF images; attachment_info re-derives it from the
    // mime, so the flag stays purely a caller-side hint.
    let _ = animated;
    let info = attachment_info(&mime, width, height, metadata.len());
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        None,
        op_id,
    )
}

/// Clipboard image path: bytes are handed over directly (bounded by C++),
/// so no temporary file ever exists on disk.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_attachment_bytes(
    bridge: &RustClient,
    room_id: String,
    bytes: Vec<u8>,
    filename: String,
    mime: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    // Never trust the caller's label alone for raster images.
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("clipboard data is not a supported image".to_owned());
    }
    let size = bytes.len() as u64;
    let info = attachment_info(&mime, width, height, size);
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        None,
        op_id,
    )
}

/// v0.7.x forwarding: send an attachment to a room WITHOUT its live
/// timeline being open.
///
/// `send_attachment_bytes` above routes through the open SDK timeline, which
/// is right for the composer — the user is looking at that room. Forwarding
/// is the opposite case by definition: the target is a room the user is not
/// in yet, so that path refuses every real forward. This one goes straight
/// to `Room::send_attachment`, which the SDK still encrypts for the target
/// room when that room is encrypted; nothing about the crypto changes.
///
/// It carries the SAME byte validation as the timeline path — a payload
/// labelled `image/*` whose magic disagrees is refused rather than uploaded.
pub(crate) fn send_attachment_bytes_to_room(
    bridge: &RustClient,
    room_id: String,
    bytes: Vec<u8>,
    filename: String,
    mime: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("attachment data is not a supported image".to_owned());
    }
    let content_type: mime::Mime = mime
        .parse()
        .map_err(|_| "attachment mime is not valid".to_owned())?;
    let size = bytes.len() as u64;
    let info = attachment_info(&mime, width, height, size);
    let room = joined_room(
        &require_client(bridge)?,
        &room_id,
    )?;
    let events = Arc::clone(&bridge.events);
    bridge.spawn_room_action(async move {
        let mut config = attachment::AttachmentConfig::new();
        if let Some(info) = info {
            config = config.info(info);
        }
        let result = room
            .send_attachment(filename, &content_type, bytes, config)
            .await;
        enqueue(
            &events,
            json!({
                "type": "attachment_send_result",
                "op_id": op_id,
                "room_id": room_id,
                // Coarse category only; SDK errors may embed server detail
                // that must not cross the FFI.
                "ok": result.is_ok(),
                "category": if result.is_ok() { "" } else { "rejected" },
            }),
        );
    });
    Ok(())
}

/// v0.6.1: thread attachment (file) — same validation and info as the room
/// path, but routed through the thread-focused SDK timeline so the SDK
/// attaches the m.thread relation and encrypts for encrypted rooms.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_thread_attachment_path(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    animated: bool,
    op_id: u64,
) -> Result<(), String> {
    let metadata =
        std::fs::metadata(&path).map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    // `animated` marks GIF images; attachment_info re-derives it from the
    // mime, so the flag stays purely a caller-side hint.
    let _ = animated;
    let info = attachment_info(&mime, width, height, metadata.len());
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };
    bridge.timelines.send_thread_attachment(
        &bridge.runtime,
        client,
        room_id,
        root_event_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        None,
        op_id,
    )
}

/// v0.6.1: thread attachment (clipboard bytes) — no temporary file, routed
/// through the thread-focused SDK timeline.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_thread_attachment_bytes(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    bytes: Vec<u8>,
    filename: String,
    mime: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("clipboard data is not a supported image".to_owned());
    }
    let size = bytes.len() as u64;
    let info = attachment_info(&mime, width, height, size);
    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };
    bridge.timelines.send_thread_attachment(
        &bridge.runtime,
        client,
        room_id,
        root_event_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        None,
        op_id,
    )
}

// ---------------------------------------------------------------------------
// Media retrieval (the download half of the media bridge)
// ---------------------------------------------------------------------------

/// v0.7 defense-in-depth: media fetch timeout classes. Each Rust timeout is
/// STRICTLY below the C++ watchdog class that covers the same operation
/// (standard 40s < 45s; playable 90s < 100s; save 270s < 300s), so Rust is
/// the normal terminal emitter and the watchdog stays a last resort.
/// 0 = standard (thumbnails, avatars, viewer images), 1 = playable
/// video/audio materialization, 2 = explicit Save As.
pub(crate) fn media_timeout_secs(class: u32) -> u64 {
    match class {
        2 => 270,
        1 => 90,
        _ => 40,
    }
}

/// Size policy for full-payload fetches. matrix-sdk 0.18 buffers media
/// responses whole (no streaming API), so peak memory INSIDE the SDK stays
/// unbounded until upstream streams — these caps bound what Lightning
/// accepts, parks, and materializes. Save As is deliberately generous.
pub(crate) fn media_size_cap(timeout_class: u32) -> u64 {
    match timeout_class {
        2 => 2 * 1024 * 1024 * 1024, // Save As: 2 GiB
        _ => 512 * 1024 * 1024,      // inline/viewer/playable: 512 MiB
    }
}

/// Largest payload the SDK media store may cache (the retention policy's
/// max_file_size, set in build_client). 24 MiB keeps avatars, thumbnails,
/// stickers, images and the whole 20 MiB animated-GIF class cacheable
/// across sessions while videos and large audio bypass sqlite entirely —
/// one giant blob INSERT on the store's single write connection is what
/// stalled every other media fetch behind it.
pub(crate) const MEDIA_STORE_MAX_FILE_BYTES: u64 = 24 * 1024 * 1024;

fn emit_media_failed(
    terminal: &crate::EventQueueRef,
    parked: &std::sync::Arc<std::sync::Mutex<std::collections::HashMap<u64, Vec<u8>>>>,
    op_id: u64,
    lifecycle: u64,
    key: &str,
    kind: u32,
    category: &str,
) {
    crate::enqueue_terminal(terminal, parked, json!({
        "type": "media_failed",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "key": key,
        "kind": kind,
        "category": category,
    }));
}

/// Fetch (and for encrypted rooms, decrypt) media for a timeline item whose
/// source was captured by the timeline serializer. `kind`: 0 = full, 1 =
/// timeline thumbnail (preserves the historical full fallback), 2 = compact
/// list thumbnail (never fetches an encrypted full payload as a fallback).
/// Bytes are parked in `media_results` for `mx_rust_media_take`; they never
/// enter the JSON queue.
pub(crate) fn media_fetch(
    bridge: &RustClient,
    key: String,
    kind: u32,
    op_id: u64,
    timeout_class: u32,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let Some((source, filename, mimetype, declared_size,
              has_embedded_thumbnail)) =
        bridge.timelines.media_source(&key, kind != 0)
    else {
        return Err("unknown media item".to_owned());
    };
    let terminal = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    let cap = media_size_cap(timeout_class);
    // Pre-flight: refuse a full-payload fetch whose Matrix metadata already
    // declares an over-cap size — the SDK would buffer it whole.
    if kind == 0 {
        if let Some(declared) = declared_size {
            if declared > cap {
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &key, kind,
                    "too_large",
                );
                return Ok(());
            }
        }
    }
    // A payload whose Matrix metadata already declares it larger than the
    // retention policy's max_file_size can never be served from or admitted
    // to the sqlite media cache — but use_cache=true would still take the
    // store's single write connection for the guaranteed-miss read (reads
    // bump last_access first) and again after the download. Bypass the
    // cache round-trip entirely for those; everything else keeps the cache.
    let use_cache =
        kind != 0 || declared_size.map_or(true, |s| s <= MEDIA_STORE_MAX_FILE_BYTES);
    if kind == 2 && !has_embedded_thumbnail
        && matches!(&source, MediaSource::Encrypted(_))
    {
        // A homeserver cannot thumbnail ciphertext and downloading the full
        // decrypted attachment for a 40px list preview violates the list's
        // bandwidth/security contract. Encrypted events carrying their
        // normal encrypted thumbnail still take the SDK decrypt path above.
        emit_media_failed(
            &terminal, &results, op_id, lifecycle, &key, kind, "unavailable",
        );
        return Ok(());
    }
    let aborts = Arc::clone(&bridge.media_fetch_aborts);
    bridge.spawn_media_fetch(op_id, async move {
        let format = if kind == 2 && !has_embedded_thumbnail {
            MediaFormat::Thumbnail(MediaThumbnailSettings::with_method(
                Method::Scale,
                UInt::new_saturating(96),
                UInt::new_saturating(72),
            ))
        } else {
            MediaFormat::File
        };
        let request = MediaRequestParameters { source, format };
        // Bounded await: matrix-sdk 0.18 deliberately disables its own HTTP
        // timeout for media, so an unresponsive server would otherwise hang
        // this task forever (and pin its shutdown join). The timeout
        // consumes the future, so exactly one terminal event can ever be
        // emitted per op.
        let outcome = tokio::time::timeout(
            std::time::Duration::from_secs(media_timeout_secs(timeout_class)),
            client.media().get_media_content(&request, use_cache),
        )
        .await;
        if let Ok(mut guard) = aborts.lock() {
            guard.remove(&op_id);
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Err(_elapsed) => {
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &key, kind,
                    "timeout",
                );
            }
            Ok(Ok(bytes)) => {
                let size = bytes.len() as u64;
                if size > cap {
                    // The metadata lied (or was absent): reject post-hoc
                    // instead of parking an unbounded decrypted payload.
                    emit_media_failed(
                        &terminal, &results, op_id, lifecycle, &key, kind,
                        "too_large",
                    );
                    return;
                }
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                crate::enqueue_terminal(&terminal, &results, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": key,
                    "kind": kind,
                    "size": size,
                    "mimetype": mimetype.unwrap_or_default(),
                    "filename": filename,
                }));
            }
            Ok(Err(err)) => {
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &key, kind,
                    classify_room_error(&err.to_string()),
                );
            }
        }
    });
    Ok(())
}

/// Fetch a server-side thumbnail for a plain (unencrypted) mxc URI — room,
/// user and space avatars. Encrypted media never routes through here.
pub(crate) fn media_fetch_mxc(
    bridge: &RustClient,
    mxc: String,
    width: u64,
    height: u64,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    if !mxc.starts_with("mxc://") {
        return Err("not an mxc URI".to_owned());
    }
    let uri: OwnedMxcUri = OwnedMxcUri::from(mxc.as_str());
    let terminal = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    let aborts = Arc::clone(&bridge.media_fetch_aborts);
    bridge.spawn_media_fetch(op_id, async move {
        let format = if width == 0 || height == 0 {
            MediaFormat::File
        } else {
            MediaFormat::Thumbnail(MediaThumbnailSettings::with_method(
                Method::Scale,
                UInt::new_saturating(width),
                UInt::new_saturating(height),
            ))
        };
        let request = MediaRequestParameters {
            source: matrix_sdk::ruma::events::room::MediaSource::Plain(uri),
            format,
        };
        // Standard class: avatars/thumbnails are small; 40s stays strictly
        // below the C++ 45s watchdog so Rust emits the terminal first.
        let outcome = tokio::time::timeout(
            std::time::Duration::from_secs(media_timeout_secs(0)),
            client.media().get_media_content(&request, true),
        )
        .await;
        if let Ok(mut guard) = aborts.lock() {
            guard.remove(&op_id);
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Err(_elapsed) => {
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &mxc, 2, "timeout",
                );
            }
            Ok(Ok(bytes)) => {
                let size = bytes.len() as u64;
                if size > media_size_cap(0) {
                    emit_media_failed(
                        &terminal, &results, op_id, lifecycle, &mxc, 2,
                        "too_large",
                    );
                    return;
                }
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                crate::enqueue_terminal(&terminal, &results, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": mxc,
                    "kind": 2u32,
                    "size": size,
                }));
            }
            Ok(Err(err)) => {
                emit_media_failed(
                    &terminal, &results,
                    op_id,
                    lifecycle,
                    &mxc,
                    2,
                    classify_room_error(&err.to_string()),
                );
            }
        }
    });
    Ok(())
}

/// Server upload limit (m.upload.size), fetched once per session by C++.
///
/// `bytes: 0` means UNKNOWN, not unlimited and not a default: either the
/// homeserver advertises no maximum or the capability lookup failed. C++
/// treats 0 as "no preflight is possible" and lets the send path proceed,
/// so the server itself rejects an oversized upload. Reporting an invented
/// ceiling here would be worse than reporting nothing — it would refuse
/// files the server would have accepted, while looking like a real
/// server-advertised limit to everything downstream.
pub(crate) fn fetch_upload_limit(bridge: &RustClient) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let bytes = client
            .load_or_fetch_max_upload_size()
            .await
            .map(u64::from)
            .unwrap_or(0);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "upload_limit",
            "lifecycle": lifecycle,
            "bytes": bytes,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // v0.7 media defense-in-depth: the Rust timeout for every class stays
    // STRICTLY below the C++ watchdog class covering the same operation
    // (45s standard / 100s playable / 300s Save As), so Rust is the normal
    // terminal emitter and the watchdog is last-resort only.
    #[test]
    fn media_timeout_classes_stay_below_watchdog() {
        assert_eq!(media_timeout_secs(0), 40);
        assert_eq!(media_timeout_secs(1), 90);
        assert_eq!(media_timeout_secs(2), 270);
        assert!(media_timeout_secs(0) < 45);
        assert!(media_timeout_secs(1) < 100);
        assert!(media_timeout_secs(2) < 300);
        // Unknown classes fall back to the strictest bound.
        assert_eq!(media_timeout_secs(99), 40);
    }

    #[test]
    fn media_size_caps_by_class() {
        assert_eq!(media_size_cap(0), 512 * 1024 * 1024);
        assert_eq!(media_size_cap(1), 512 * 1024 * 1024);
        assert_eq!(media_size_cap(2), 2 * 1024 * 1024 * 1024);
    }

    // Every send carries typed metadata with at least the authoritative
    // size — `info: None` on non-image sends is what shipped every
    // Lightning-sent video with no declared size, which the receiver-side
    // prefetch/poster path (deliberately) refuses to work without.
    #[test]
    fn attachment_info_declares_size_for_every_type() {
        match attachment_info("video/mp4", 1280, 720, 1000) {
            Some(AttachmentInfo::Video(info)) => {
                assert_eq!(info.size, UInt::new(1000));
                assert_eq!(info.width, UInt::new(1280));
                assert_eq!(info.height, UInt::new(720));
            }
            other => panic!("expected video info, got {other:?}"),
        }
        // Unknown dimensions are omitted, never sent as zero.
        match attachment_info("video/webm", 0, 0, 42) {
            Some(AttachmentInfo::Video(info)) => {
                assert_eq!(info.size, UInt::new(42));
                assert!(info.width.is_none());
                assert!(info.height.is_none());
            }
            other => panic!("expected video info, got {other:?}"),
        }
        match attachment_info("audio/flac", 0, 0, 7) {
            Some(AttachmentInfo::Audio(info)) => {
                assert_eq!(info.size, UInt::new(7));
            }
            other => panic!("expected audio info, got {other:?}"),
        }
        match attachment_info("application/pdf", 0, 0, 9) {
            Some(AttachmentInfo::File(info)) => {
                assert_eq!(info.size, UInt::new(9));
            }
            other => panic!("expected file info, got {other:?}"),
        }
        // Images keep the existing contract: no dimensions, no info.
        assert!(attachment_info("image/png", 0, 0, 5).is_none());
        match attachment_info("image/gif", 100, 100, 5) {
            Some(AttachmentInfo::Image(info)) => {
                assert_eq!(info.is_animated, Some(true));
            }
            other => panic!("expected image info, got {other:?}"),
        }
    }

    // v0.7 video round: an outgoing video declares its duration alongside
    // the geometry and size, and omits a duration it does not know rather
    // than sending zero.
    #[test]
    fn video_info_carries_duration_and_geometry() {
        match video_info(1920, 1080, 5000, 4200) {
            AttachmentInfo::Video(info) => {
                assert_eq!(info.width, UInt::new(1920));
                assert_eq!(info.height, UInt::new(1080));
                assert_eq!(info.size, UInt::new(5000));
                assert_eq!(
                    info.duration,
                    Some(std::time::Duration::from_millis(4200))
                );
            }
            other => panic!("expected video info, got {other:?}"),
        }
        match video_info(0, 0, 5000, 0) {
            AttachmentInfo::Video(info) => {
                assert!(info.width.is_none());
                assert!(info.height.is_none());
                assert!(info.duration.is_none());
                assert_eq!(info.size, UInt::new(5000));
            }
            other => panic!("expected video info, got {other:?}"),
        }
    }

    fn poster(data: Vec<u8>, width: u64, height: u64) -> PosterBytes {
        PosterBytes { data, width, height }
    }

    fn jpeg_bytes(len: usize) -> Vec<u8> {
        let mut bytes = vec![0xFF, 0xD8, 0xFF, 0xE0];
        bytes.resize(len.max(12), 0);
        bytes
    }

    // The poster's TYPE comes from its bytes, never from the caller. The
    // send path hands over what Lightning decoded itself, but the same
    // magic check that guards saved media guards this: anything that is not
    // a supported raster is refused, and a refusal means "no thumbnail",
    // never a video event advertising a type its bytes are not.
    #[test]
    fn poster_is_validated_by_magic_not_by_claim() {
        let thumb = poster(jpeg_bytes(64), 320, 180)
            .into_thumbnail()
            .expect("a real JPEG is accepted");
        assert_eq!(thumb.content_type.to_string(), "image/jpeg");
        assert_eq!(thumb.width, UInt::new(320).unwrap());
        assert_eq!(thumb.height, UInt::new(180).unwrap());
        assert_eq!(thumb.size, UInt::new(64).unwrap());

        // Not a raster at all (an SVG document, plain text, truncated).
        assert!(poster(b"<svg xmlns=\"http://www.w3.org/2000/svg\"/>".to_vec(),
                       320, 180)
            .into_thumbnail()
            .is_none());
        assert!(poster(b"not an image at all".to_vec(), 320, 180)
            .into_thumbnail()
            .is_none());
        assert!(poster(vec![0xFF, 0xD8], 320, 180).into_thumbnail().is_none());
    }

    // Bounds and nonsense geometry degrade to "no thumbnail"; the caller
    // still sends the video.
    #[test]
    fn poster_bounds_and_geometry_are_enforced() {
        assert!(poster(Vec::new(), 320, 180).into_thumbnail().is_none());
        assert!(poster(jpeg_bytes(64), 0, 180).into_thumbnail().is_none());
        assert!(poster(jpeg_bytes(64), 320, 0).into_thumbnail().is_none());
        assert!(poster(jpeg_bytes(MAX_POSTER_BYTES + 1), 320, 180)
            .into_thumbnail()
            .is_none());
        assert!(poster(jpeg_bytes(MAX_POSTER_BYTES), 320, 180)
            .into_thumbnail()
            .is_some());
    }

    // The timeout wrapper emits exactly one outcome: a never-completing
    // fetch resolves to Elapsed (one "timeout" terminal), a fast fetch
    // passes its value through untouched.
    #[tokio::test]
    async fn media_timeout_wrapper_yields_single_outcome() {
        let hung: Result<(), tokio::time::error::Elapsed> = tokio::time::timeout(
            std::time::Duration::from_millis(25),
            std::future::pending::<()>(),
        )
        .await;
        assert!(hung.is_err());

        let quick = tokio::time::timeout(
            std::time::Duration::from_millis(25),
            std::future::ready(7u32),
        )
        .await;
        assert_eq!(quick, Ok(7));
    }

    // The server's answer is only USABLE if it actually says something.
    //
    // Synapse returns 200 with an empty object for a URL it could not fetch,
    // so "the request succeeded" is not the same as "there is a preview".
    // Accepting an empty answer would draw a blank card AND skip the client
    // fallback that could have produced a real one — the worst of both.
    //
    // THIS CALLS THE REAL FUNCTION. An earlier version of this case
    // re-implemented the predicate as a local closure, which asserts only
    // that the test agrees with itself and would have passed with
    // server_preview_fields() deleted entirely.
    #[test]
    fn server_preview_answer_is_usable_only_with_title_or_description() {
        use super::server_preview_fields as f;
        assert!(f(&json!({})).is_none(), "an empty object must fall back");
        assert!(
            f(&json!({ "matrix:image:size": 1234 })).is_none(),
            "an image size with no text is not a preview"
        );
        assert!(
            f(&json!({ "og:title": "", "og:description": "" })).is_none(),
            "present-but-empty strings must fall back, not draw a blank card"
        );
        let only_title = f(&json!({ "og:title": "Example" }))
            .expect("a title alone is a usable preview");
        assert_eq!(only_title["title"], "Example");
        // The route label is what lets the UI drop the IP warning honestly,
        // so a card without it would be worse than no card.
        assert_eq!(only_title["preview_route"], "server");
        let only_desc = f(&json!({ "og:description": "Some page" }))
            .expect("a description alone is a usable preview");
        assert_eq!(only_desc["description"], "Some page");
        // og:image is an mxc:// URI from a server preview — it must survive
        // into image_source, or the thumbnail silently disappears.
        let with_image = f(&json!({
            "og:title": "T",
            "og:image": "mxc://example.org/abc",
            "matrix:image:size": 4096
        }))
        .expect("usable");
        assert_eq!(with_image["image_source"], "mxc://example.org/abc");
        assert_eq!(with_image["image_size"], 4096);
    }

    #[test]
    fn preview_destination_policy_blocks_internal_networks() {
        use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
        assert!(!public_ip(IpAddr::V4(Ipv4Addr::LOCALHOST)));
        assert!(!public_ip(IpAddr::V4(Ipv4Addr::new(10, 0, 0, 1))));
        assert!(!public_ip(IpAddr::V4(Ipv4Addr::new(100, 64, 0, 1))));
        assert!(!public_ip(IpAddr::V4(Ipv4Addr::new(169, 254, 169, 254))));
        assert!(!public_ip(IpAddr::V6(Ipv6Addr::LOCALHOST)));
        assert!(!public_ip("fc00::1".parse().unwrap()));
        assert!(!public_ip("fe80::1".parse().unwrap()));
        assert!(public_ip("93.184.216.34".parse().unwrap()));
        assert!(public_ip("2606:2800:220:1:248:1893:25c8:1946".parse().unwrap()));
    }

    #[test]
    fn preview_html_metadata_fallbacks_are_inert() {
        let (fields, title) = html_fields(r#"<html><head>
          <meta property="og:title" content="Open Graph">
          <meta name="twitter:description" content="Twitter fallback">
          <meta property="og:image" content="/image.png">
          <title>HTML fallback</title><script>alert(1)</script></head></html>"#);
        assert_eq!(pick(&fields, &["og:title", "twitter:title"]), "Open Graph");
        assert_eq!(pick(&fields, &["og:description", "twitter:description"]), "Twitter fallback");
        assert_eq!(pick(&fields, &["og:image"]), "/image.png");
        assert_eq!(title, "HTML fallback");
        let (_, fallback) = html_fields("<title>Only title</title><b>ignored</b>");
        assert_eq!(fallback, "Only title");
    }

    #[test]
    fn preview_image_dimensions_require_matching_magic() {
        let mut png = vec![0; 24];
        png[..8].copy_from_slice(b"\x89PNG\r\n\x1a\n");
        png[16..20].copy_from_slice(&640u32.to_be_bytes());
        png[20..24].copy_from_slice(&480u32.to_be_bytes());
        assert_eq!(image_dimensions("image/png", &png), Some((640, 480)));
        assert_eq!(image_dimensions("image/gif", &png), None);
        let mut gif = b"GIF89a".to_vec(); gif.extend_from_slice(&320u16.to_le_bytes()); gif.extend_from_slice(&200u16.to_le_bytes());
        assert_eq!(image_dimensions("image/gif", &gif), Some((320, 200)));
        assert_eq!(image_dimensions("image/gif", b"<html>not a gif</html>"), None);
    }

    #[test]
    fn preview_payload_classification_uses_mime_and_magic() {
        let mut png = vec![0; 24];
        png[..8].copy_from_slice(b"\x89PNG\r\n\x1a\n");
        let mut gif = b"GIF89a".to_vec();
        gif.extend_from_slice(&1u16.to_le_bytes());
        gif.extend_from_slice(&1u16.to_le_bytes());
        gif.extend_from_slice(&[0; 2]);
        let jpeg = [0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0, 0, 0, 0, 0];
        let webp = b"RIFF\0\0\0\0WEBPVP8X";

        assert_eq!(classify_preview_payload("image/png", &png), Ok(Some("image/png")));
        assert_eq!(classify_preview_payload("image/gif", &gif), Ok(Some("image/gif")));
        assert_eq!(classify_preview_payload("image/jpeg", &jpeg), Ok(Some("image/jpeg")));
        assert_eq!(classify_preview_payload("image/webp", webp), Ok(Some("image/webp")));
        // Safely recognized bytes may recover a generic or mislabeled CDN response.
        assert_eq!(classify_preview_payload("application/octet-stream", &gif),
                   Ok(Some("image/gif")));
        assert_eq!(classify_preview_payload("text/html", &gif), Ok(Some("image/gif")));
        // A .gif-looking request that actually returned HTML remains metadata.
        assert_eq!(classify_preview_payload("text/html", b"<html><title>Giphy</title></html>"),
                   Ok(None));
        assert_eq!(classify_preview_payload("image/gif", b"<html>not gif</html>"),
                   Err("invalid_image"));
        assert_eq!(classify_preview_payload("image/svg+xml", b"<svg></svg>"),
                   Err("unsupported_mime"));
    }

    // 0.5.14 checkpoint 4: the original check only handled the "VP8X"
    // (extended: animation/alpha/exif) chunk — real-world direct WebP
    // links overwhelmingly use the simple lossy ("VP8 ") or lossless
    // ("VP8L") chunk instead, which returned None (→ "invalid_image")
    // before this fix even though the bytes are a perfectly valid image.
    #[test]
    fn preview_webp_dimensions_cover_all_three_chunk_types() {
        // VP8X (extended): chunk size(4) + flags(4) + width-1 24-bit LE(3)
        // + height-1 24-bit LE(3).
        let mut vp8x = b"RIFF\x00\x00\x00\x00WEBPVP8X".to_vec();
        vp8x.extend_from_slice(&0u32.to_le_bytes()); // chunk size (unused)
        vp8x.extend_from_slice(&[0u8; 4]); // flags
        vp8x.extend_from_slice(&[99, 0, 0]); // width-1 = 99 -> 100
        vp8x.extend_from_slice(&[49, 0, 0]); // height-1 = 49 -> 50
        assert_eq!(image_dimensions("image/webp", &vp8x), Some((100, 50)));

        // VP8L (lossless): signature 0x2f + 14-bit width-1 | 14-bit height-1 (LE u32).
        let bits: u32 = (99) | ((49) << 14); // width-1=99, height-1=49
        let mut vp8l = b"RIFF\x00\x00\x00\x00WEBPVP8L".to_vec();
        vp8l.extend_from_slice(&0u32.to_le_bytes()); // chunk size (unused)
        vp8l.push(0x2f);
        vp8l.extend_from_slice(&bits.to_le_bytes());
        assert_eq!(image_dimensions("image/webp", &vp8l), Some((100, 50)));

        // VP8 (simple lossy): 3-byte frame tag + 3-byte sync (9d 01 2a) +
        // 14-bit width LE + 14-bit height LE.
        let mut vp8 = b"RIFF\x00\x00\x00\x00WEBPVP8 ".to_vec();
        vp8.extend_from_slice(&0u32.to_le_bytes()); // chunk size (unused)
        vp8.extend_from_slice(&[0, 0, 0]); // frame tag (unused by parser)
        vp8.extend_from_slice(&[0x9d, 0x01, 0x2a]);
        vp8.extend_from_slice(&100u16.to_le_bytes());
        vp8.extend_from_slice(&50u16.to_le_bytes());
        assert_eq!(image_dimensions("image/webp", &vp8), Some((100, 50)));

        // A VP8 chunk missing the sync code is not a valid keyframe.
        let mut bad_vp8 = b"RIFF\x00\x00\x00\x00WEBPVP8 ".to_vec();
        bad_vp8.extend_from_slice(&[0u8; 10]);
        assert_eq!(image_dimensions("image/webp", &bad_vp8), None);
    }

    #[test]
    fn preview_failure_from_str_has_no_status_or_redirects() {
        let failure: PreviewFailure = "dns_failure".into();
        assert_eq!(failure.category, "dns_failure");
        assert_eq!(failure.status, None);
        assert_eq!(failure.redirects, 0);
    }

    #[test]
    fn preview_initial_fetch_limit_accommodates_direct_images() {
        // The redirect loop's shared fetch must use the larger of the two
        // byte limits — using the smaller MAX_HTML_BYTES here (as 0.5.13
        // did) silently capped every direct-image preview below its real
        // ceiling before Content-Type was ever inspected.
        assert_eq!(MAX_INITIAL_FETCH_BYTES, MAX_IMAGE_BYTES);
        assert!(MAX_INITIAL_FETCH_BYTES >= MAX_HTML_BYTES);
    }

    #[test]
    fn preview_image_fields_rejects_unsupported_mime_like_svg() {
        // SVG is active content and must stay non-previewable regardless of
        // how it's classified — image_fields() is the final gate even if a
        // caller ever passed it a non-raster mime by mistake.
        assert!(image_fields("image/svg+xml".to_owned(), b"<svg></svg>".to_vec()).is_err());
    }

    #[test]
    fn classify_room_error_categories() {
        assert_eq!(classify_room_error("M_LIMIT_EXCEEDED: too many requests"), "rate_limited");
        assert_eq!(classify_room_error("M_FORBIDDEN: not allowed"), "forbidden");
        assert_eq!(classify_room_error("M_ROOM_IN_USE: alias taken"), "alias_taken");
        assert_eq!(classify_room_error("M_INVALID_PARAM: bad alias"), "invalid");
        assert_eq!(classify_room_error("M_NOT_FOUND"), "not_found");
        assert_eq!(classify_room_error("connection reset by peer"), "network");
    }

    #[test]
    fn sniff_image_mime_by_magic_bytes() {
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        png.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&png), Some("image/png"));

        let mut jpg = vec![0xFF, 0xD8, 0xFF, 0xE0];
        jpg.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&jpg), Some("image/jpeg"));

        let mut gif = b"GIF89a".to_vec();
        gif.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&gif), Some("image/gif"));

        let mut webp = b"RIFF\x00\x00\x00\x00WEBP".to_vec();
        webp.extend_from_slice(&[0; 4]);
        assert_eq!(sniff_image_mime(&webp), Some("image/webp"));

        // JPEG XL. Both byte strings below are the real first bytes emitted by
        // `cjxl` (libjxl 0.11) on a 32x32 PNG: the container form, and the
        // bare codestream form that `cjxl` produces by default and with -d 0.
        let mut jxl_box =
            b"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a\x00\x00\x00\x14ftypjxl ".to_vec();
        jxl_box.extend_from_slice(&[0; 4]);
        assert_eq!(sniff_image_mime(&jxl_box), Some("image/jxl"));

        let mut jxl_stream = vec![0xFF, 0x0A, 0x47, 0x06];
        jxl_stream.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&jxl_stream), Some("image/jxl"));

        // A JPEG must not be read as a JPEG XL codestream and vice versa: the
        // two share only their first byte.
        let mut not_jxl = vec![0xFF, 0xD8, 0xFF, 0x0A];
        not_jxl.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&not_jxl), Some("image/jpeg"));

        assert_eq!(sniff_image_mime(b"plain text, not an image"), None);
        assert_eq!(sniff_image_mime(b"tiny"), None);
    }

    #[test]
    fn create_room_options_parse_defaults() {
        let opts: CreateRoomOptions = serde_json::from_str("{\"name\":\"Test\"}").unwrap();
        assert_eq!(opts.name, "Test");
        assert!(!opts.public);
        assert!(!opts.encrypted);
        assert!(opts.invites.is_empty());
        assert!(opts.space_id.is_empty());
    }

    #[test]
    fn create_room_request_private_encrypted() {
        let opts = CreateRoomOptions {
            name: "Secret".to_owned(),
            encrypted: true,
            invites: vec!["@a:example.org".to_owned(), "@a:example.org".to_owned()],
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.name.as_deref(), Some("Secret"));
        assert_eq!(request.preset, Some(RoomPreset::PrivateChat));
        assert_eq!(request.visibility, Visibility::Private);
        // Encryption initial state present; duplicate invites deduplicated.
        assert_eq!(request.initial_state.len(), 1);
        assert_eq!(request.invite.len(), 1);
        assert!(!request.is_direct);
        // Server chooses the room version — never pinned locally.
        assert!(request.room_version.is_none());
    }

    #[test]
    fn create_room_request_public_with_alias() {
        let opts = CreateRoomOptions {
            name: "Town Square".to_owned(),
            public: true,
            alias: " town-square ".to_owned(),
            topic: " hello ".to_owned(),
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.preset, Some(RoomPreset::PublicChat));
        assert_eq!(request.visibility, Visibility::Public);
        assert_eq!(request.room_alias_name.as_deref(), Some("town-square"));
        assert_eq!(request.topic.as_deref(), Some("hello"));
        assert!(request.initial_state.is_empty());
    }

    #[test]
    fn create_space_request_sets_space_creation_content_and_no_encryption() {
        let opts = CreateRoomOptions {
            name: "Team".to_owned(),
            topic: "Our team".to_owned(),
            is_space: true,
            // Even if encryption were requested, a Space is never encrypted.
            encrypted: true,
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.name.as_deref(), Some("Team"));
        // No encryption initial-state for a Space.
        assert!(request.initial_state.is_empty());
        // creation_content carries room_type: m.space.
        let raw = request.creation_content.expect("creation_content present");
        let cc = raw.deserialize().expect("valid creation content");
        assert_eq!(cc.room_type, Some(RoomType::Space));
    }

    #[test]
    fn ordinary_room_has_no_creation_content() {
        let opts = CreateRoomOptions { name: "Chat".to_owned(), ..Default::default() };
        let request = build_create_room_request(&opts).unwrap();
        assert!(request.creation_content.is_none());
    }

    #[test]
    fn create_room_request_rejects_empty_name_and_bad_invite() {
        let opts = CreateRoomOptions { name: "   ".to_owned(), ..Default::default() };
        assert!(build_create_room_request(&opts).is_err());

        let opts = CreateRoomOptions {
            name: "ok".to_owned(),
            invites: vec!["not-a-user-id".to_owned()],
            ..Default::default()
        };
        assert!(build_create_room_request(&opts).is_err());
    }
}
