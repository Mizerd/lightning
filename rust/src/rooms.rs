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
    attachment::{AttachmentInfo, BaseImageInfo},
    media::{MediaFormat, MediaRequestParameters, MediaThumbnailSettings},
    ruma::{
        api::client::{
            media::get_content_thumbnail::v3::Method,
            room::{
                create_room::{self, v3::RoomPreset},
                Visibility,
            },
        },
        assign,
        events::{
            room::encryption::RoomEncryptionEventContent, space::child::SpaceChildEventContent,
            InitialStateEvent, StateEventType,
        },
        OwnedMxcUri, OwnedUserId, RoomId, UInt, UserId,
    },
    RoomMemberships, RoomState,
};
use matrix_sdk_ui::timeline::AttachmentSource;
use serde::Deserialize;
use serde_json::json;

use crate::{enqueue, RustClient};

/// Upper bound on one member-snapshot payload. Rooms larger than this are
/// truncated (flagged in the event) — the UI shows counts, not 10k rows.
const MEMBER_SNAPSHOT_CAP: usize = 500;

/// Avatar uploads are small; refuse anything larger before reading it.
const MAX_AVATAR_BYTES: u64 = 8 * 1024 * 1024;

/// Last-resort attachment bound when the server's m.upload.size is unknown.
/// C++ enforces the real (server-provided) limit before dispatching.
pub(crate) const FALLBACK_UPLOAD_LIMIT: u64 = 100 * 1024 * 1024;

fn require_client(bridge: &RustClient) -> Result<matrix_sdk::Client, String> {
    bridge
        .client
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
        .ok_or_else(|| "no active Matrix session".to_owned())
}

fn joined_room(
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
        let result = client.account().fetch_user_profile_of(&uid).await;
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

struct SafeResponse { status: reqwest::StatusCode, mime: String, location: Option<String>, bytes: Vec<u8> }
async fn safe_get(url: &url::Url, limit: usize) -> Result<SafeResponse, &'static str> {
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
        .user_agent("Lightning/0.5.14").resolve(host, addresses[0]).build()
        .map_err(|_| "request_failure")?;
    let response = client.get(url.clone())
        .header(reqwest::header::ACCEPT,
            "text/html,image/jpeg,image/png,image/webp,image/gif")
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
async fn safe_get_following_redirects(
    mut url: url::Url,
    limit: usize,
) -> Result<(SafeResponse, url::Url, u32), PreviewFailure> {
    for redirects in 0..=MAX_REDIRECTS {
        let redirects = redirects as u32;
        let response = safe_get(&url, limit)
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
        safe_get_following_redirects(page, MAX_INITIAL_FETCH_BYTES).await?;
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
                    safe_get_following_redirects(image, MAX_IMAGE_BYTES).await
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

pub(crate) fn fetch_url_preview(
    bridge: &RustClient,
    url: String,
    op_id: u64,
) -> Result<(), String> {
    require_client(bridge)?; // previews are account/session scoped
    let parsed = url::Url::parse(url.trim()).map_err(|_| "invalid URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("unsupported or credentialed URL".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = preview(parsed).await;
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
    let initial_state = if opts.encrypted {
        vec![InitialStateEvent::with_empty_state_key(
            RoomEncryptionEventContent::with_recommended_defaults(),
        )
        .to_raw_any()]
    } else {
        Vec::new()
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
        let members = match room.members(RoomMemberships::ACTIVE).await {
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

        let mut sorted: Vec<&matrix_sdk::room::RoomMember> = members.iter().collect();
        sorted.sort_by(|a, b| {
            use matrix_sdk::ruma::events::room::member::MembershipState;
            let joined_first = |m: &matrix_sdk::room::RoomMember| {
                matches!(m.membership(), MembershipState::Join)
            };
            joined_first(b)
                .cmp(&joined_first(a))
                .then(b.power_level().cmp(&a.power_level()))
                .then_with(|| a.name().to_lowercase().cmp(&b.name().to_lowercase()))
        });

        let truncated = sorted.len() > MEMBER_SNAPSHOT_CAP;
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
                        _ => "other",
                    },
                    "role": role,
                    "ambiguous": member.name_ambiguous(),
                    "is_own": Some(member.user_id())
                        == own_id.as_ref().map(|o| o.as_ref()),
                })
            })
            .collect();

        // Own permissions, from the SDK's power-level helpers — never
        // guessed from role labels or room-creator status.
        let own_member = match &own_id {
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

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_members",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": true,
            "truncated": truncated,
            "joined_count": joined_count,
            "invited_count": invited_count,
            "own_can_invite": can_invite,
            "own_can_edit_name": can_edit_name,
            "own_can_edit_topic": can_edit_topic,
            "own_can_edit_avatar": can_edit_avatar,
            "members": rows,
        }));
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
    let info = if mime.starts_with("image/") {
        image_info(width, height, metadata.len(), animated)
    } else {
        None
    };
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
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
    let info = if mime.starts_with("image/") {
        image_info(width, height, size, mime == "image/gif")
    } else {
        None
    };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        op_id,
    )
}

// ---------------------------------------------------------------------------
// Media retrieval (the download half of the media bridge)
// ---------------------------------------------------------------------------

fn emit_media_failed(
    events: &crate::EventQueueRef,
    op_id: u64,
    lifecycle: u64,
    key: &str,
    kind: u32,
    category: &str,
) {
    enqueue(events, json!({
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
/// thumbnail (falls back to full when no thumbnail exists). Bytes are parked
/// in `media_results` for `mx_rust_media_take`; they never enter the JSON
/// queue.
pub(crate) fn media_fetch(
    bridge: &RustClient,
    key: String,
    kind: u32,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let Some((source, filename, mimetype)) =
        bridge.timelines.media_source(&key, kind == 1)
    else {
        return Err("unknown media item".to_owned());
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = MediaRequestParameters { source, format: MediaFormat::File };
        let outcome = client.media().get_media_content(&request, true).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(bytes) => {
                let size = bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                enqueue(&events, json!({
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
            Err(err) => {
                emit_media_failed(
                    &events,
                    op_id,
                    lifecycle,
                    &key,
                    kind,
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
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
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
        let outcome = client.media().get_media_content(&request, true).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok(bytes) => {
                let size = bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                enqueue(&events, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": mxc,
                    "kind": 2u32,
                    "size": size,
                }));
            }
            Err(err) => {
                emit_media_failed(
                    &events,
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
            .unwrap_or(FALLBACK_UPLOAD_LIMIT);
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
