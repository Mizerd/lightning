//! Room management, user search and the media bridge.
//!
//! Each function backs a thin `extern "C"` wrapper in `lib.rs`: it validates
//! synchronously, then runs network work as a managed task
//! (`spawn_room_action`) on the shared runtime so sign-out joins it. Results
//! carry the lifecycle generation and an op id; C++ rejects stale ones.
//!
//! Never puts key material, tokens, raw events, local paths or media bytes
//! into the JSON event queue; media bytes use the take/free bridge in
//! `lib.rs`.

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

/// Upper bound on one member-snapshot payload; larger rooms are truncated
/// (flagged in the event).
const MEMBER_SNAPSHOT_CAP: usize = 500;

/// Avatar uploads are small; refuse anything larger before reading it.
/// Shared with the own-avatar path in `profile.rs`.
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

/// Coarse, non-secret categories for room-management failures; raw SDK
/// error text may embed server detail. Pure and unit-tested.
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
    // Before the 404 branch: a server lacking an endpoint answers 404 with
    // M_UNRECOGNIZED, and "your server cannot do this" must not read as
    // "nothing there" (e.g. MSC3030 jump-to-date on pre-1.6 servers).
    } else if lc.contains("m_unrecognized") || lc.contains("unrecognized") {
        "unrecognized"
    } else if lc.contains("m_not_found") || lc.contains("not found") || lc.contains("404") {
        "not_found"
    } else {
        "network"
    }
}

/// Sniff a raster-image MIME type from magic bytes, so a mislabelled
/// extension cannot spoof the type. Pure and unit-tested.
///
/// This identifies; it does not promise the GUI can decode it (Qt image
/// formats are per-platform plugins; e.g. JPEG XL decodes only on Linux).
/// That is `lightning::imagefmt::canDecode` in C++; this table is mirrored
/// in `src/media/ImageFormatSupport.h`.
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
    // JPEG XL, both shapes (verified against `cjxl` output):
    //   ISOBMFF container: 00 00 00 0C "JXL " 0D 0A 87 0A, then "ftypjxl "
    //   bare codestream:   FF 0A
    // The 12-byte container test comes first. bytes[4..8] is "JXL ", not
    // "ftyp", so it cannot be mistaken for MP4. The two-byte codestream
    // signature is weak, but a false positive only yields a payload a decoder
    // refuses.
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

/// Profile lookups are decoration, and every room action is joined on the
/// GUI thread at teardown, so they are bounded (like the presence batch).
const PROFILE_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);

/// Exact profile lookup for one user id (GET /profile/{userId}). Backs the
/// bare-localpart invite search, since the directory may not list local
/// users. Only display name and avatar mxc cross; a missing user is ok=false
/// with category "not_found".
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
        // Bounded: matrix-sdk's default config retries and honours
        // M_LIMIT_EXCEEDED's retry_after_ms, and DirectAvatarResolver fires one per
        // DM peer right after restore. Room actions are joined on the GUI thread at
        // teardown, so an unbounded lookup freezes the next account switch. A
        // timeout is reported as an ordinary failure.
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
// Client-side URL previews
// ---------------------------------------------------------------------------

const MAX_HTML_BYTES: usize = 2 * 1_048_576;
const MAX_IMAGE_BYTES: usize = 5 * 1_048_576;
const MAX_IMAGE_PIXELS: u64 = 25_000_000;
const MAX_REDIRECTS: usize = 4;
const CONNECT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);
const REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(12);

pub(crate) fn public_ip(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(v) => !(v.is_private() || v.is_loopback()
            || v.is_link_local() || v.is_multicast() || v.is_unspecified()
            || v.octets()[0] == 0 || v.octets()[0] >= 224
            || (v.octets()[0] == 100 && (64..=127).contains(&v.octets()[1]))
            || (v.octets()[0] == 169 && v.octets()[1] == 254)),
        std::net::IpAddr::V6(v) => {
            // Unmap first, or every v4 rule above is bypassable: `::ffff:127.0.0.1` is
            // not loopback, multicast, fc00::/7 or fe80::/10 as v6, yet an AF_INET6
            // socket to it reaches the IPv4 host, so an `AAAA ::ffff:127.0.0.1` record
            // could aim a preview at loopback, RFC1918 or 169.254.169.254 (on every
            // redirect hop too). Also refused: the deprecated `::a.b.c.d` form and
            // NAT64's 64:ff9b::/96, which also embed an IPv4 destination.
            if let Some(v4) = v.to_ipv4_mapped() {
                return public_ip(std::net::IpAddr::V4(v4));
            }
            if v.segments()[0] == 0x0064 && v.segments()[1] == 0xff9b {
                return false;
            }
            // `to_ipv4()` also matches the compatible form (::a.b.c.d); judge it as the
            // embedded IPv4 address.
            if let Some(v4) = v.to_ipv4() {
                return public_ip(std::net::IpAddr::V4(v4));
            }
            !(v.is_loopback() || v.is_unspecified()
                || v.is_multicast() || (v.segments()[0] & 0xfe00) == 0xfc00
                || (v.segments()[0] & 0xffc0) == 0xfe80)
        }
    }
}

pub(crate) struct SafeResponse { pub status: reqwest::StatusCode, pub mime: String, pub location: Option<String>, pub bytes: Vec<u8> }
// Default Accept for HTML/image previews. GIF downloads and provider JSON
// pass their own so a `.gif` is never negotiated into webp.
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
    // No proxy: reqwest honours HTTPS_PROXY/ALL_PROXY by default, and a proxied
    // request asks the proxy to reach the origin by name, bypassing the pinned
    // address and every check above.
    let client = reqwest::Client::builder().redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .connect_timeout(CONNECT_TIMEOUT).timeout(REQUEST_TIMEOUT)
        .user_agent(crate::USER_AGENT).resolve(host, addresses[0]).build()
        .map_err(|_| "request_failure")?;
    let response = client.get(url.clone())
        .header(reqwest::header::ACCEPT, accept)
        // Some CDN/WAF layers treat requests without standard browser headers
        // (Accept-Language especially) as bots. Accept-Encoding comes from
        // reqwest's gzip/deflate features.
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
// Sanitized failure detail for one preview attempt, enough to tell a code
// regression from a site's own bot/WAF policy, never the URL, query or
// body. `status` is None when no HTTP response arrived.
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

// Follow redirects manually so every hop gets the same DNS/IP policy,
// pinning, scheme check, timeout and size bound. reqwest's own redirect
// following stays disabled in safe_get().
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

// Some CDNs mislabel images as application/octet-stream or text/html, so
// classification checks both the declared MIME and the bytes. A declared
// image must match its magic; a generic label may be promoted only when the
// bytes prove a supported raster. HTML stays HTML; SVG never matches.
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

// The first fetch cannot know whether it gets HTML or an image, so it uses
// the larger limit; MAX_HTML_BYTES would cap direct-image previews.
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
                // A failing thumbnail must not sink a preview that already has a title or
                // description; image errors are swallowed and the fields stay empty.
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
        // Accept the simple lossy ("VP8 ") and lossless ("VP8L") chunks as well
        // as extended "VP8X", which most direct WebP links do not use.
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
/// Tried first because a client-side fetch hands the linked site the
/// user's IP; via the server the site sees only the server, and the
/// thumbnail is an `mxc://` on the authenticated media path. The server does
/// learn the URL: in an unencrypted room it already saw the message, and
/// encrypted rooms still require an explicit per-message gesture.
///
/// Returns Ok(None), not an error, when previews are disabled or the
/// endpoint is unrecognised, so the caller falls back.
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

/// Reshape a server's OpenGraph object into our field set, or None when it
/// says nothing. Separate so a test can drive it.
pub(crate) fn server_preview_fields(data: &serde_json::Value) -> Option<serde_json::Value> {
    let obj = data.as_object()?;
    // A server that could not fetch the URL still returns 200 with an empty
    // object; no title and no description counts as no answer, so the caller
    // falls back instead of drawing an empty card.
    let text = |k: &str| -> String {
        obj.get(k).and_then(|v| v.as_str()).unwrap_or_default().to_owned()
    };
    let title = text("og:title");
    let description = text("og:description");
    if title.is_empty() && description.is_empty() {
        return None;
    }
    // Same field set as the client path. `og:image` is an mxc:// URI here,
    // resolved by the media bridge, so the thumbnail needs no direct contact.
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
    // The client that runs the server-side attempt below: one session.
    let sdk = require_client(bridge)?;
    let parsed = url::Url::parse(url.trim()).map_err(|_| "invalid URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("unsupported or credentialed URL".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        // Server first, client only if it cannot, so the site does not see the
        // user's IP. Whether a direct fetch is allowed is the caller's decision;
        // `preview_route` tells the UI which path produced the card.
        let mut result = match server_preview(&sdk, &parsed).await {
            Some(fields) => Ok(fields),
            None => preview(parsed.clone()).await,
        };
        // Anything the client path produced took the direct route. Stamped here,
        // in one place.
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

/// Existing joined DM rooms with `user_id`, from the SDK's m.direct
/// projection. Synchronous store lookup.
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

/// Create an encrypted DM with `user_id` via `Client::create_dm`
/// (TrustedPrivateChat preset, encryption state, `is_direct`, and the SDK's
/// locked m.direct update).
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

/// Build the ruma create-room request from validated options. The server
/// chooses the room id and version.
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
    // A Space is never an encrypted timeline, so no encryption state for it.
    let initial_state = if opts.encrypted && !opts.is_space {
        vec![InitialStateEvent::with_empty_state_key(
            RoomEncryptionEventContent::with_recommended_defaults(),
        )
        .to_raw_any()]
    } else {
        Vec::new()
    };
    // A room that can host a call must let its members publish their call
    // membership. `m.call.member` is a state event, and the default
    // `state_default` of 50 lets only moderators publish it: an ordinary member
    // who joins gets a call that looks connected but is one-way (their
    // membership never lands, so no media key is sent to them). Element Call
    // does the same for rooms it creates. Spaces are excluded (no calls).
    let power_levels = if opts.is_space {
        None
    } else {
        let mut events = serde_json::Map::new();
        for ev in [crate::rtc::EV_MEMBER_LEGACY, crate::rtc::EV_MEMBER_STICKY] {
            events.insert(ev.to_owned(), json!(0));
        }
        // Only these event types are lowered; `state_default` is unchanged.
        Some(json!({ "events": serde_json::Value::Object(events) }))
    };
    // Outside the request literal: `?` cannot cross the assign! macro.
    let power_level_content_override = match power_levels {
        // cast_unchecked: the value is built here from literals.
        Some(v) => Some(
            Raw::new(&v)
                .map_err(|e| format!("power level override: {e}"))?
                .cast_unchecked(),
        ),
        None => None,
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
        power_level_content_override,
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
                // Optional Space placement. A failure there is reported as a warning, not
                // a failed room creation.
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

/// Child removal: an `m.space.child` with content `{}`, as matrix-sdk-ui's
/// remove_child sends. `{"via": []}` is also "not a child" per spec, but the
/// SDK's space graph still counts it as an edge. Never leaves or deletes
/// the room itself.
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
    let child = RoomId::parse(child_room_id).map_err(|_| "invalid room id".to_owned())?;
    space
        .send_state_event_raw("m.space.child", child.as_str(), json!({}))
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

/// Toggle the MSC1772 `suggested` flag on an existing m.space.child,
/// preserving `via` and `order` (a blind rewrite would clobber routing). A
/// room that is not a child is refused, never made one as a side effect.
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

/// Participants crossing the FFI. The facepile shows 2-4; a small surplus
/// lets the UI choose without another request.
const THREAD_PARTICIPANT_CAP: usize = 8;

/// The real participants of a thread, for the summary-card facepile.
///
/// matrix-sdk-ui 0.18's `ThreadSummary` / `ThreadListItem` carry only the
/// root and latest senders and a reply count, so participants come from
/// `Room::load_or_fetch_event_with_relations` (cache-first; fetched
/// relations are cached).
///
/// Order: root sender first, then others by first appearance, deduplicated
/// by user id. Only user id, display name and avatar mxc cross, never
/// content. `truncated` says whether more participants exist.
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
            // Report a failure as such; the card keeps what it had.
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
            // Profile from synced member state, no request per face. Missing fields
            // stay empty and C++ falls back to localpart and colour avatar.
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

/// Max edit events one "remove edits" pass redacts. The report gives the
/// count, so a longer chain needs a second pass instead of an unbounded
/// burst.
const EDIT_REDACTION_CAP: usize = 50;

/// Remove a message's edits, restoring its original text.
///
/// Matrix has no "unedit": an edit is a separate `m.replace` event and can
/// only be taken back by redaction. The replacements are collected via
/// `Room::load_or_fetch_event_with_relations` (cache-first) and redacted,
/// which also drops the "edited" marker (derived from their presence).
///
/// Only the caller's own edits are touched; redacting others' edits needs
/// the redact power level and is not what this action means. Counts only.
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

        // Own replacement events only; all of them go. The original is never here.
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
        // Cache-first: emit a partial snapshot from the state store so the People
        // list renders immediately, then the full synced roster under the same op
        // with partial=false. An empty cache emits nothing (it would read as
        // "nobody").
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
                // Re-check after the builder's await; the guard sits right before the emit.
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, snapshot);
                }
            }
        }

        // Active (join+invite) plus banned: banned members must be visible for
        // unban. They are excluded from counts and mention suggestions downstream.
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
        // Guard right before the emit; the builder awaits a store read.
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, snapshot);
        }
    });
    Ok(())
}

// One snapshot shape for the partial and full emits.
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
        // `truncated` refers to the active roster. Banned members sort last, so the
        // cap drops them first, making those bans unreachable for unban (accepted
        // limit).
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
                    // Raw power level so the UI can hide actions against peers at or above the
                    // viewer's level; the server enforces regardless.
                    "power_level": power_level_int(member.power_level()),
                    "ambiguous": member.name_ambiguous(),
                    "is_own": Some(member.user_id()) == own_id,
                })
            })
            .collect();

        // Own permissions from the SDK's power-level helpers, never guessed from
        // role labels.
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
        // @room: the room's own required level (notifications.room), asked of the
        // SDK.
        let can_notify_room = own_member.as_ref().is_some_and(|m| {
            m.can_do(PowerLevelAction::TriggerNotification(
                NotificationPowerLevelType::Room,
            ))
        });
        let can_kick = own_member.as_ref().is_some_and(|m| m.can_kick());
        let can_ban = own_member.as_ref().is_some_and(|m| m.can_ban());
        // Unban requires max(ban, kick) (ruma PowerLevelAction::Unban), not the ban
        // level alone; ask the SDK.
        let can_unban = own_member
            .as_ref()
            .is_some_and(|m| m.can_do(PowerLevelAction::Unban));
        let own_power_level = own_member
            .as_ref()
            .map(|m| power_level_int(m.power_level()))
            .unwrap_or(0);
        // Each is the SDK's check against the room's real required level for that
        // state event; nothing assumes "admin only".
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
        // Space child management (add/remove/suggest all use m.space.child).
        let can_manage_space_children = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::SpaceChild));
        // The room's real m.room.power_levels thresholds, for the Permissions
        // matrix.
        //
        // Only integers under a fixed set of keys cross: the `events` map is keyed
        // by sender-chosen event type strings, which must never reach the bridge,
        // so rows are looked up by typed `StateEventType` and emitted under our own
        // keys. Each row is the effective level (explicit entry or
        // `state_default`), which is what the server enforces.
        let power_levels = room.power_levels_or_default().await;
        let state_level = |ty: StateEventType| -> i64 {
            power_levels
                .events
                .get(&TimelineEventType::from(ty))
                .map(|value| i64::from(*value))
                .unwrap_or_else(|| i64::from(power_levels.state_default))
        };
        // The room's default user level, so the UI can tell "at default" from
        // "pinned to the same number" (`update_power_levels` treats setting the
        // default as a removal).
        let users_default: i64 = i64::from(power_levels.users_default);
        // From `Room::version()`; empty while state has not settled, rendered as
        // nothing rather than a fabricated "1".
        let room_version = room
            .version()
            .map(|version| version.to_string())
            .unwrap_or_default();
        // Whether this account may send m.room.tombstone (the upgrade level).
        // Reported separately; it gates only a disclosure.
        let can_upgrade = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomTombstone));
        // Whether this account may publish a call membership, i.e. whether Join
        // can work. Checked with the string we actually send
        // (`rtc::EV_MEMBER_LEGACY`), not a typed ruma enum, whose aliasing could
        // govern a different wire string. Default power levels put state at 50.
        let can_publish_rtc_membership = own_member.as_ref().is_some_and(|m| {
            m.can_send_state(StateEventType::from(crate::rtc::EV_MEMBER_LEGACY))
        });
        let join_rule = join_rule_str(room.join_rule().as_ref());
        let canonical_alias = room
            .canonical_alias()
            .map(|a| a.to_string())
            .unwrap_or_default();
        // The restricted allow list as room ids, so a configuration another client
        // wrote can be shown and edited without loss; unknown allow-rule kinds are
        // carried as an opaque marker the editor must preserve (see
        // set_room_join_rule).
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

        // Built outside the snapshot: nesting it inside `json!` exceeds serde_json's
        // macro recursion limit (a compile error).
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
        // Outside the snapshot for the same macro-recursion reason.
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
            // A partial snapshot keeps the op open in C++ until the full roster lands.
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
            "own_can_publish_rtc_membership": can_publish_rtc_membership,
            "room_version": room_version,
            // Every key here is chosen by this file (see above); C++ mirrors them into
            // the Permissions matrix.
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

// UserPowerLevel -> bridge integer. MSC4289 creators ("Infinite") map to a
// sentinel above every finite level that survives the JSON f64 hop exactly.
// Accepted edges: an explicit level above 1e9 would outrank a creator, and
// C++ reads it as a 64-bit integer.
fn power_level_int(
    level: matrix_sdk::ruma::events::room::power_levels::UserPowerLevel,
) -> i64 {
    use matrix_sdk::ruma::events::room::power_levels::UserPowerLevel;
    match level {
        UserPowerLevel::Infinite => 1_000_000_000,
        UserPowerLevel::Int(v) => v.into(),
        // Non-exhaustive enum: an unknown variant reads as an ordinary member.
        _ => 0,
    }
}

// Kick, ban or unban one user (`op`: 0 kick, 1 ban, 2 unban) through the
// SDK. The server enforces power levels. Empty reason means none. Result
// event: room_moderation_result { op_id, room_id, user_id, op, ok, category }.
pub(crate) fn moderate_member(
    bridge: &RustClient,
    room_id: String,
    user_id: String,
    reason: String,
    op: u8,
    op_id: u64,
) -> Result<(), String> {
    // An exhaustive enum: a dispatcher of destructive actions must refuse an
    // unknown op, never default to one.
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

/// Rooms one moderation plan may cover: the Space plus its rooms. A larger
/// Space is truncated and the event says so.
const MODERATION_PLAN_CAP: usize = 100;

/// Bound on reading one room's member list for a plan, so one slow room
/// cannot hold the dialog.
const MODERATION_PLAN_MEMBER_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(15);

/// Whether `op` (0 kick, 1 ban, 2 unban) against a target is worth
/// offering in one room: "" when it is, otherwise the reason it is not.
///
/// Mirrors Element's space-member filters: the target must have a membership
/// the action applies to, the viewer must hold the room's own threshold for
/// it (`can_act`, from the SDK), and the target must be strictly below the
/// viewer. The server enforces regardless. Pure and unit-tested.
fn moderation_verdict(
    op: u8,
    target: Option<&matrix_sdk::ruma::events::room::member::MembershipState>,
    can_act: bool,
    own_level: i64,
    target_level: i64,
) -> &'static str {
    use matrix_sdk::ruma::events::room::member::MembershipState;
    let Some(membership) = target else {
        return "not_a_member";
    };
    let banned = *membership == MembershipState::Ban;
    match op {
        0 => {
            if banned {
                return "already_banned";
            }
            if !matches!(membership, MembershipState::Join | MembershipState::Invite) {
                return "not_a_member";
            }
        }
        1 => {
            if banned {
                return "already_banned";
            }
        }
        2 => {
            if !banned {
                return "not_banned";
            }
        }
        _ => return "invalid",
    }
    if !can_act {
        return "no_permission";
    }
    if target_level >= own_level {
        return "outranked";
    }
    ""
}

fn membership_label(
    membership: &matrix_sdk::ruma::events::room::member::MembershipState,
) -> &'static str {
    use matrix_sdk::ruma::events::room::member::MembershipState;
    match membership {
        MembershipState::Join => "joined",
        MembershipState::Invite => "invited",
        MembershipState::Ban => "banned",
        MembershipState::Leave => "left",
        MembershipState::Knock => "knocking",
        _ => "other",
    }
}

// For each room in `room_ids_json` (a JSON array, the Space first), whether a
// kick / ban / unban (`op` as in moderate_member) of `user_id` is worth
// offering there, and why not otherwise. Reads the store first; a room whose
// member list was never loaded is fetched once, bounded. Sends nothing.
// Result event: moderation_plan { op_id, user_id, op, truncated, rooms: [
// { room_id, name, is_space, membership, own_level, target_level, reason } ] }
// where an empty reason means the action is offered.
pub(crate) fn moderation_plan(
    bridge: &RustClient,
    room_ids_json: String,
    user_id: String,
    op: u8,
    op_id: u64,
) -> Result<(), String> {
    let op_name = match op {
        0 => "kick",
        1 => "ban",
        2 => "unban",
        _ => return Err("invalid moderation op".to_owned()),
    };
    let client = require_client(bridge)?;
    let uid = UserId::parse(&user_id).map_err(|_| "invalid user id".to_owned())?;
    let own_id = client
        .user_id()
        .map(|u| u.to_owned())
        .ok_or_else(|| "no active Matrix session".to_owned())?;
    let requested: Vec<String> = serde_json::from_str(&room_ids_json)
        .map_err(|_| "invalid room list".to_owned())?;
    let mut room_ids: Vec<String> = Vec::new();
    for id in requested {
        if !room_ids.contains(&id) {
            room_ids.push(id);
        }
    }
    let truncated = room_ids.len() > MODERATION_PLAN_CAP;
    room_ids.truncate(MODERATION_PLAN_CAP);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut rows: Vec<serde_json::Value> = Vec::new();
        for room_id in room_ids {
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let room = RoomId::parse(&room_id)
                .ok()
                .and_then(|id| client.get_room(&id))
                .filter(|room| room.state() == RoomState::Joined);
            let Some(room) = room else {
                rows.push(json!({
                    "room_id": room_id,
                    "name": "",
                    "is_space": false,
                    "membership": "unknown",
                    "reason": "not_joined",
                }));
                continue;
            };
            let name = room
                .cached_display_name()
                .map(|n| n.to_string())
                .unwrap_or_default();
            let own = room.get_member_no_sync(&own_id).await.ok().flatten();
            // The store first; a room whose members were never loaded is
            // fetched once (get_member syncs only when not synced yet).
            let target = match room.get_member_no_sync(&uid).await {
                Ok(Some(member)) => Ok(Some(member)),
                _ => match tokio::time::timeout(
                    MODERATION_PLAN_MEMBER_TIMEOUT,
                    room.get_member(&uid),
                )
                .await
                {
                    Ok(Ok(member)) => Ok(member),
                    _ => Err(()),
                },
            };
            let (reason, membership, own_level, target_level) = match (&own, &target) {
                (None, _) | (_, Err(())) => ("unknown", "unknown", 0, 0),
                (Some(own), Ok(target)) => {
                    let can_act = match op {
                        0 => own.can_kick(),
                        1 => own.can_ban(),
                        _ => own.can_do(PowerLevelAction::Unban),
                    };
                    let own_level = power_level_int(own.power_level());
                    let target_level = target
                        .as_ref()
                        .map(|m| power_level_int(m.power_level()))
                        .unwrap_or(0);
                    (
                        moderation_verdict(
                            op,
                            target.as_ref().map(|m| m.membership()),
                            can_act,
                            own_level,
                            target_level,
                        ),
                        target
                            .as_ref()
                            .map(|m| membership_label(m.membership()))
                            .unwrap_or("none"),
                        own_level,
                        target_level,
                    )
                }
            };
            rows.push(json!({
                "room_id": room_id,
                "name": name,
                "is_space": room.is_space(),
                "membership": membership,
                "own_level": own_level,
                "target_level": target_level,
                "reason": reason,
            }));
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "moderation_plan",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "user_id": user_id,
            "op": op_name,
            "truncated": truncated,
            "rooms": rows,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Room administration: member power levels, join rule, alias
// ---------------------------------------------------------------------------

/// Coarse join-rule label. `restricted` / `knock_restricted` are reported so
/// the UI can display them.
fn join_rule_str(rule: Option<&matrix_sdk::ruma::room::JoinRule>) -> &'static str {
    use matrix_sdk::ruma::room::JoinRule;
    match rule {
        Some(JoinRule::Invite) => "invite",
        Some(JoinRule::Public) => "public",
        Some(JoinRule::Knock) => "knock",
        Some(JoinRule::Private) => "private",
        Some(JoinRule::Restricted(_)) => "restricted",
        Some(JoinRule::KnockRestricted(_)) => "knock_restricted",
        // Unknown or not yet synced: the UI renders nothing.
        _ => "",
    }
}

/// Set one member's power level via the SDK's `update_power_levels`, which
/// reads the real `m.room.power_levels`, changes only this user, and keeps
/// every other level. A level equal to `users_default` is removed from the
/// users map. The server enforces permission. Result event:
/// room_power_level_result.
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
    // Refuse out-of-range before spawning: `Int` is the JSON-safe range.
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

/// Set one threshold in `m.room.power_levels` (the Permissions matrix).
/// `key` must be in a fixed allowlist, so this is never a generic "set any
/// event type's level" primitive.
///
/// Both paths read-modify-send the whole content (the event has no partial
/// update):
///   * scalar thresholds and name/avatar/topic/space_child go through the
///     SDK's `apply_power_level_changes`, which leaves other fields alone,
///     including per-event entries equal to the new default (removing them
///     would grant unintended privileges);
///   * other per-event rows, which `RoomPowerLevelChanges` cannot express,
///     are edited in `events` by hand.
///
/// Event types are always typed `StateEventType`, never `key` as a wire
/// string: ruma's aliasing could make a hand-built type govern an event
/// nobody sends. `m.call.member` is not in the allowlist (ruma aliases the
/// stable name onto the unstable one we send, and Spaces host no calls).
///
/// The server enforces permission. Result event: room_power_matrix_result.
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
    // Refuse out-of-range before spawning: `Int` is the JSON-safe range.
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

/// Set the room's join rule. `restricted` and `knock_restricted` take
/// `allowed_room_ids` (each an `m.room_membership` allow rule). Unknown
/// allow-rule kinds already present are preserved verbatim. An empty allow
/// list is refused: it would silently make the room invite-only.
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

/// Set or clear (empty `alias`) the room's canonical alias.
///
/// The alias must resolve to this room before `m.room.canonical_alias` may
/// name it, so one not yet pointing here is published first via
/// `Client::create_room_alias`. Clearing does not delete the directory
/// mapping; that is a separate, more destructive action.
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
            // Keep existing alternative aliases; this promotes one alias only.
            let alt_aliases = room.alt_aliases();
            if let Some(alias) = parsed.as_deref() {
                let already_here = match client.resolve_room_alias(alias).await {
                    Ok(response) => response.room_id.as_str() == room_id_for_task,
                    // Not resolvable yet: publish it below. Other failures also fall through;
                    // the create attempt's error is reported.
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
// Room access: history visibility, guest access, directory visibility,
// alternative aliases. Every write is the SDK's own request; the client only
// chooses values and reports the server's answer.
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

/// Directory visibility is not room state (it lives on the server's public
/// room list), so it is fetched on demand as its own poll event.
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

/// Replace the alternative-alias list. As with the canonical alias, aliases
/// not yet resolving here are published first. The canonical alias is
/// untouched, and removed aliases keep their directory mapping.
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
// Room upgrade. Versions come from /capabilities, the upgrade uses the
// standard /upgrade endpoint, and the server carries state, power levels
// and aliases into the replacement.
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
                // Numeric ids first, in order, then others lexically, so the picker is
                // ordered and the recommendation deterministic.
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
// Message edit history and event source.
//
// The history is the original plus its m.replace relations, from the event
// cache or /relations; the SDK decrypts on the way, and an undecryptable
// event is reported, not dropped. Bodies cross for display only; C++ keeps
// them in memory for the open dialog and never caches them.
// ---------------------------------------------------------------------------

pub(crate) fn request_edit_history(
    bridge: &RustClient,
    room_id: String,
    event_id: String,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::relation::RelationType;
    use matrix_sdk::ruma::events::AnySyncTimelineEvent;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let target = EventId::parse(&event_id).map_err(|_| "invalid event id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        use matrix_sdk::room::{IncludeRelations, RelationsOptions};
        use matrix_sdk::ruma::UInt;
        // Replacements are asked of the server first (/relations): the event cache
        // holds only edits that arrived over sync, so cache-first could show a
        // partial history. The cache is the fallback, and the answer says so.
        let loaded = room.load_or_fetch_event(&target, None).await;
        let Ok(original) = loaded else {
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, json!({
                    "type": "message_edit_history", "lifecycle": lifecycle,
                    "room_id": room_id, "event_id": event_id, "ok": false,
                    "revisions": [],
                }));
            }
            return;
        };
        let mut options = RelationsOptions::default();
        options.include_relations =
            IncludeRelations::RelationsOfType(RelationType::Replacement);
        options.limit = Some(UInt::from(200u32));
        let (relations, partial) = match room.relations(target.to_owned(), options).await {
            // A next_batch_token means more existed than the page. Relations default
            // to backwards, so the missing ones are the oldest; flagged partial like
            // the cache fallback.
            Ok(answer) => {
                let truncated = answer.next_batch_token.is_some();
                (answer.chunk, truncated)
            }
            Err(_) => match room
                .load_or_fetch_event_with_relations(
                    &target,
                    Some(vec![RelationType::Replacement]),
                    None,
                )
                .await
            {
                Ok((_, cached)) => (cached, true),
                Err(_) => (Vec::new(), true),
            },
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        // One revision row. Replacements carry the text in m.new_content; the
        // original at the top level.
        let revision = |raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>,
                        is_original: bool|
         -> Option<serde_json::Value> {
            let value: serde_json::Value = raw.deserialize_as().ok()?;
            let event_id = value["event_id"].as_str()?.to_owned();
            let sender = value["sender"].as_str().unwrap_or_default().to_owned();
            let ts = value["origin_server_ts"].as_u64().unwrap_or(0);
            let redacted = value["unsigned"]["redacted_because"].is_object()
                || (value["content"].as_object().map(|c| c.is_empty()).unwrap_or(false)
                    && value["type"] == "m.room.message");
            let undecryptable = value["type"] == "m.room.encrypted";
            let content = if is_original {
                value["content"].clone()
            } else {
                value["content"]["m.new_content"].clone()
            };
            let body = content["body"].as_str().unwrap_or_default().to_owned();
            let formatted = if content["format"] == "org.matrix.custom.html" {
                content["formatted_body"].as_str().unwrap_or_default().to_owned()
            } else {
                String::new()
            };
            Some(json!({
                "event_id": event_id, "sender": sender, "timestamp_ms": ts,
                "body": body, "formatted_body": formatted,
                "redacted": redacted, "undecryptable": undecryptable,
                "is_original": is_original,
            }))
        };

        let mut rows: Vec<serde_json::Value> = Vec::new();
        if let Some(row) = revision(original.raw(), true) {
            rows.push(row);
        } else {
            // An unreadable original is a failure, not an empty history.
            enqueue(&events, json!({
                "type": "message_edit_history", "lifecycle": lifecycle,
                "room_id": room_id, "event_id": event_id, "ok": false,
                "revisions": [],
            }));
            return;
        }
        let mut edits: Vec<serde_json::Value> = relations
            .iter()
            .filter_map(|related| {
                // Only replacements from the original sender count, as every client does.
                let parsed: AnySyncTimelineEvent = related.raw().deserialize().ok()?;
                let sender_ok = match (&parsed, rows.first()) {
                    (AnySyncTimelineEvent::MessageLike(_), Some(first)) => {
                        parsed.sender().as_str() == first["sender"].as_str().unwrap_or_default()
                    }
                    _ => false,
                };
                if !sender_ok {
                    return None;
                }
                revision(related.raw(), false)
            })
            .collect();
        edits.sort_by_key(|row| row["timestamp_ms"].as_u64().unwrap_or(0));
        rows.extend(edits);
        if let Some(last) = rows.last_mut() {
            last["is_latest"] = json!(true);
        }
        enqueue(&events, json!({
            "type": "message_edit_history", "lifecycle": lifecycle,
            "room_id": room_id, "event_id": event_id, "ok": true,
            "partial": partial,
            "revisions": rows,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Jump to date (MSC3030 `timestamp_to_event`, stable since Matrix 1.6).
//
// The server answers in one request; there is no client-side fallback
// (paginating until the dates match would be unbounded). A server without it
// answers 404/unrecognised and the client says so. Searches forward from the
// start of the chosen day, so it lands on that day's first message, or the
// next one after an empty day.
// ---------------------------------------------------------------------------

pub(crate) fn event_at_timestamp(
    bridge: &RustClient,
    room_id: String,
    timestamp_ms: i64,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::room::get_event_by_timestamp;
    use matrix_sdk::ruma::api::Direction;
    use matrix_sdk::ruma::MilliSecondsSinceUnixEpoch;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let parsed_room = room.room_id().to_owned();
    // Negative or absurd stamps are caller error. ruma's UInt is limited to the
    // JavaScript safe-integer range, so larger stamps cannot be serialized.
    let ts = MilliSecondsSinceUnixEpoch::from_system_time(
        std::time::SystemTime::UNIX_EPOCH
            + std::time::Duration::from_millis(
                u64::try_from(timestamp_ms)
                    .map_err(|_| "invalid timestamp".to_owned())?),
    )
    .ok_or_else(|| "invalid timestamp".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = get_event_by_timestamp::v1::Request::new(
            parsed_room, ts, Direction::Forward);
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match answer {
            Ok(response) => enqueue(&events, json!({
                "type": "timestamp_event", "lifecycle": lifecycle,
                "op_id": op_id, "room_id": room_id, "ok": true,
                "event_id": response.event_id.to_string(),
                "timestamp_ms": u64::from(response.origin_server_ts.0),
            })),
            // Sanitized category, never the server's prose.
            Err(error) => enqueue(&events, json!({
                "type": "timestamp_event", "lifecycle": lifecycle,
                "op_id": op_id, "room_id": room_id, "ok": false,
                "event_id": "",
                "category": classify_room_error(&error.to_string()),
            })),
        }
    });
    Ok(())
}

pub(crate) fn request_event_source(
    bridge: &RustClient,
    room_id: String,
    event_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let target = EventId::parse(&event_id).map_err(|_| "invalid event id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let loaded = room.load_or_fetch_event(&target, None).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let Ok(event) = loaded else {
            enqueue(&events, json!({
                "type": "event_source", "lifecycle": lifecycle,
                "room_id": room_id, "event_id": event_id, "ok": false,
            }));
            return;
        };
        // The JSON the SDK holds: for an encrypted event, the decrypted form, with
        // the ciphertext envelope described beside it. Session id and sender key
        // are public identifiers; no key material.
        let json_value: serde_json::Value =
            event.raw().deserialize_as().unwrap_or(serde_json::Value::Null);
        let pretty = serde_json::to_string_pretty(&json_value).unwrap_or_default();
        let encryption = match event.encryption_info() {
            Some(info) => json!({
                "encrypted": true,
                "sender": info.sender.to_string(),
                "sender_device": info.sender_device
                    .as_ref().map(|d| d.to_string()).unwrap_or_default(),
                "algorithm": format!("{:?}", info.algorithm_info),
                "verification": format!("{:?}", info.verification_state),
            }),
            None => json!({ "encrypted": false }),
        };
        enqueue(&events, json!({
            "type": "event_source", "lifecycle": lifecycle,
            "room_id": room_id, "event_id": event_id, "ok": true,
            "json": pretty, "encryption": encryption,
        }));
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Scheduled send: MSC4140 delayed message events.
//
// Uses the delayed-event endpoint the RTC lane relies on, with content from
// the same composed_content as an ordinary send. Protocol limits: the delay
// is a relative timeout, the server caps it, pending delayed events cannot
// be listed (the client must remember delay ids), and the server stores the
// content as given, so an encrypted room would need it encrypted now, which
// matrix-sdk 0.18 cannot do outside the send path. Encrypted rooms are
// therefore refused ("encrypted_unsupported") and use the client-side
// queue instead.
// ---------------------------------------------------------------------------

pub(crate) fn probe_delayed_events(bridge: &RustClient) -> Result<(), String> {
    use matrix_sdk::ruma::api::FeatureFlag;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let advertised = match client.supported_versions().await {
            Ok(versions) => versions.features.contains(&FeatureFlag::Msc4140),
            Err(_) => false,
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "delayed_events_support",
            "lifecycle": lifecycle,
            "advertised": advertised,
            "refused_before": crate::rtc::delayed_events_assumed_refused(&client),
            "supported": advertised
                && !crate::rtc::delayed_events_assumed_refused(&client),
        }));
    });
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn schedule_message(
    bridge: &RustClient,
    room_id: String,
    body: String,
    spec_json: String,
    mention_user_ids: Vec<String>,
    delay_ms: u64,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::delayed_events::{
        delayed_message_event, DelayParameters,
    };
    use matrix_sdk::ruma::events::{AnyMessageLikeEventContent, MessageLikeEventType};
    use matrix_sdk::ruma::TransactionId;
    use std::time::Duration;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    if delay_ms == 0 {
        return Err("a scheduled send needs a delay".to_owned());
    }
    let spec = crate::timeline::parse_body_spec(&spec_json)?;
    let mut content = crate::timeline::composed_content(&body, &spec);
    if let Some(mentions) = crate::timeline::mentions_from_ids(mention_user_ids) {
        content = content.add_mentions(mentions);
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let refuse = |events: &crate::EventQueueRef, category: &str| {
            enqueue(events, json!({
                "type": "scheduled_send_result", "op_id": op_id,
                "lifecycle": lifecycle, "room_id": room_id, "ok": false,
                "category": category,
            }));
        };
        // Encrypted, or not yet known to be unencrypted: refused (fail closed).
        let state = room.encryption_state();
        if state.is_unknown() || state.is_encrypted() {
            refuse(&events, "encrypted_unsupported");
            return;
        }
        let raw = match matrix_sdk::ruma::serde::Raw::new(
            &AnyMessageLikeEventContent::RoomMessage(content),
        ) {
            Ok(raw) => raw.cast_unchecked(),
            Err(_) => {
                refuse(&events, "rejected");
                return;
            }
        };
        let request = delayed_message_event::unstable::Request::new_raw(
            room.room_id().to_owned(),
            TransactionId::new(),
            MessageLikeEventType::RoomMessage,
            DelayParameters::Timeout { timeout: Duration::from_millis(delay_ms) },
            raw,
        );
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match answer {
            Ok(response) => enqueue(&events, json!({
                "type": "scheduled_send_result", "op_id": op_id,
                "lifecycle": lifecycle, "room_id": room_id, "ok": true,
                "delay_id": response.delay_id,
            })),
            Err(err) => refuse(&events, classify_room_error(&err.to_string())),
        }
    });
    Ok(())
}

/// cancel | send | restart on a delay id this client remembered.
pub(crate) fn update_scheduled(
    bridge: &RustClient,
    delay_id: String,
    action: String,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event;
    let client = require_client(bridge)?;
    let update = match action.as_str() {
        "cancel" => update_delayed_event::unstable::UpdateAction::Cancel,
        "send" => update_delayed_event::unstable::UpdateAction::Send,
        "restart" => update_delayed_event::unstable::UpdateAction::Restart,
        _ => return Err("unknown scheduled-send action".to_owned()),
    };
    if delay_id.trim().is_empty() {
        return Err("missing delay id".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let request = update_delayed_event::unstable::Request::new(delay_id.clone(), update);
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, match answer {
            Ok(_) => json!({
                "type": "scheduled_update_result", "op_id": op_id,
                "lifecycle": lifecycle, "delay_id": delay_id, "action": action,
                "ok": true,
            }),
            Err(err) => json!({
                "type": "scheduled_update_result", "op_id": op_id,
                "lifecycle": lifecycle, "delay_id": delay_id, "action": action,
                "ok": false, "category": classify_room_error(&err.to_string()),
            }),
        });
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

/// Image metadata for the media event: dimensions from C++ (bounded
/// decode), size from the filesystem.
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

/// Typed attachment metadata for every send. The size is authoritative;
/// dimensions and duration are best-effort and omitted when unknown, never
/// fabricated. Without declared metadata, receivers (including Lightning's
/// own prefetch and poster logic) cannot show videos properly.
///
/// `duration_ms` of 0 means unknown and is omitted, since a literal 0 would
/// render "0:00" beside a playable file.
///
/// Not a voice message: `AttachmentInfo::Audio` carries only the duration;
/// `voice_info` is the MSC3245 shape and would mark music as a recording.
pub(crate) fn attachment_info(
    mime: &str,
    width: u64,
    height: u64,
    size: u64,
    duration_ms: u64,
) -> Option<AttachmentInfo> {
    let duration = (duration_ms > 0)
        .then(|| std::time::Duration::from_millis(duration_ms));
    if mime.starts_with("image/") {
        return image_info(width, height, size, mime == "image/gif");
    }
    if mime.starts_with("video/") {
        return Some(AttachmentInfo::Video(attachment::BaseVideoInfo {
            duration,
            height: UInt::new(height).filter(|v| u64::from(*v) > 0),
            width: UInt::new(width).filter(|v| u64::from(*v) > 0),
            size: UInt::new(size),
            blurhash: None,
        }));
    }
    if mime.starts_with("audio/") {
        return Some(AttachmentInfo::Audio(attachment::BaseAudioInfo {
            duration,
            size: UInt::new(size),
            waveform: None,
        }));
    }
    Some(AttachmentInfo::File(attachment::BaseFileInfo {
        size: UInt::new(size),
    }))
}

/// A send-side video poster from C++ (a frame Lightning decoded from the
/// picked file). Re-validated by magic sniffing: `into_thumbnail` refuses
/// non-rasters, oversized data and nonsense dimensions, degrading to no
/// thumbnail. The event's content type is the sniffed one; no mime crosses
/// the FFI.
pub(crate) struct PosterBytes {
    pub data: Vec<u8>,
    pub width: u64,
    pub height: u64,
}

/// A poster is a small cover (VideoPosterExtractor emits a 640px JPEG);
/// anything larger is refused.
const MAX_POSTER_BYTES: usize = 2 * 1024 * 1024;

impl PosterBytes {
    fn into_thumbnail(self) -> Option<Thumbnail> {
        if self.data.is_empty() || self.data.len() > MAX_POSTER_BYTES {
            return None;
        }
        if self.width == 0 || self.height == 0 {
            return None;
        }
        // The bytes decide the type; SVG and other non-rasters are refused.
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

/// Video metadata for an outgoing video: like `attachment_info`'s video arm,
/// plus the duration learned while decoding the poster. Unknown values are
/// omitted.
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

/// MSC3245 voice-message metadata. The SDK turns AttachmentInfo::Voice into
/// the `org.matrix.msc3245.voice` marker plus `org.matrix.msc1767.audio`
/// (duration and 0..=1 waveform) and sends it like any audio file. `waveform`
/// is 0..=100 amplitudes and may be empty; the SDK emits the audio block
/// only when both duration and waveform are present.
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

/// Send a recorded voice message; like send_attachment_path but with
/// voice_info. A zero-length recording is a caller bug.
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

/// Thread twin of `send_voice_path`: identical validation and MSC3245
/// metadata, routed through the thread-focused timeline so the event gets a
/// real `m.thread` relation. No fallback to a room send.
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
    // Read the recording now rather than passing a path: the SDK reads
    // AttachmentSource::File inside the spawned task, after building the
    // thread timeline, and C++ deletes thread recordings when the panel closes.
    // The file could be gone by then and the failure suppressed as stale. A
    // voice message is small (mono 32 kbps Opus, 15-minute cap).
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

/// Send a video with a poster thumbnail. Same validation and routing as
/// `send_attachment_path`, plus duration and the `Thumbnail`; the SDK
/// uploads and encrypts the poster and fills the thumbnail fields. An
/// invalid poster is dropped and the video still sends.
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

/// Thread twin of `send_video_path`, via the thread-focused timeline.
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
    duration_ms: u64,
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
    // `animated` is only a caller hint; attachment_info re-derives it from the
    // mime.
    let _ = animated;
    let info = attachment_info(&mime, width, height, metadata.len(), duration_ms);
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

/// Clipboard image: bytes are passed directly (bounded by C++), so no
/// temporary file exists on disk.
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
    // Clipboard bytes: an image, never a timed medium.
    let info = attachment_info(&mime, width, height, size, 0);
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

/// Send an attachment to a room whose timeline is not open (forwarding).
/// `send_attachment_bytes` routes through the open timeline, so it refuses
/// every real forward; this uses `Room::send_attachment`, which still
/// encrypts for encrypted rooms. Same byte validation: an `image/*` payload
/// whose magic disagrees is refused.
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
    // Clipboard bytes: an image, never a timed medium.
    let info = attachment_info(&mime, width, height, size, 0);
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
                // Coarse category only; SDK errors may embed server detail.
                "ok": result.is_ok(),
                "category": if result.is_ok() { "" } else { "rejected" },
            }),
        );
    });
    Ok(())
}

/// Thread attachment (file): same validation and info as the room path,
/// routed through the thread-focused timeline so the SDK adds the m.thread
/// relation and encrypts.
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
    duration_ms: u64,
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
    // `animated` is only a caller hint; attachment_info re-derives it from the
    // mime.
    let _ = animated;
    let info = attachment_info(&mime, width, height, metadata.len(), duration_ms);
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

/// Thread attachment (clipboard bytes), via the thread-focused timeline; no
/// temporary file.
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
    // Clipboard bytes: an image, never a timed medium.
    let info = attachment_info(&mime, width, height, size, 0);
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

/// Media fetch timeout per class, each strictly below the C++ watchdog for
/// the same operation (40 < 45 s, 90 < 100 s, 270 < 300 s), so Rust emits
/// the terminal event. 0 = standard (thumbnails, avatars, viewer images),
/// 1 = playable video/audio, 2 = explicit Save As.
pub(crate) fn media_timeout_secs(class: u32) -> u64 {
    match class {
        2 => 270,
        1 => 90,
        _ => 40,
    }
}

/// Size cap for full-payload fetches. matrix-sdk 0.18 buffers media whole
/// (no streaming), so these bound what Lightning accepts and parks, not the
/// SDK's own peak memory. Save As is generous.
pub(crate) fn media_size_cap(timeout_class: u32) -> u64 {
    match timeout_class {
        2 => 2 * 1024 * 1024 * 1024, // Save As: 2 GiB
        _ => 512 * 1024 * 1024,      // inline/viewer/playable: 512 MiB
    }
}

/// Largest payload the SDK media store may cache (retention max_file_size,
/// set in build_client). Keeps avatars, thumbnails, stickers, images and
/// 20 MiB GIFs cacheable; videos and large audio bypass sqlite, since one
/// huge blob INSERT stalls every other fetch on the single write connection.
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

/// Fetch (and decrypt, in encrypted rooms) media for a timeline item whose
/// source the serializer captured. `kind`: 0 full, 1 timeline thumbnail
/// (with full fallback), 2 compact list thumbnail (never falls back to an
/// encrypted full payload). Bytes are parked for `mx_rust_media_take`,
/// never in the JSON queue.
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
    // Refuse a full fetch whose metadata declares an over-cap size; the SDK
    // would buffer it whole.
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
    // Skip the cache when the declared size exceeds the retention max: it can
    // never be cached, and use_cache would still take the store's single write
    // connection twice.
    //
    // Encrypted-room media is never written to the sqlite cache.
    // `get_media_content` stores the decrypted buffer when `use_cache` is set,
    // and the store is opened without a passphrase, so it would be plaintext on
    // disk, violating §6. A store passphrase is not the fix: the same setting
    // opens the crypto store, and changing it would orphan existing installs'
    // Megolm keys (no migration in 0.18). Cost: a re-download per session;
    // MediaBridge's RAM cache serves repeats within a session.
    let source_is_encrypted = matches!(&source, MediaSource::Encrypted(_));
    let use_cache = !source_is_encrypted
        && (kind != 0 || declared_size.map_or(true, |s| s <= MEDIA_STORE_MAX_FILE_BYTES));
    if kind == 2 && !has_embedded_thumbnail
        && matches!(&source, MediaSource::Encrypted(_))
    {
        // A server cannot thumbnail ciphertext, and downloading the full decrypted
        // file for a 40px list preview breaks the list's contract. Encrypted
        // thumbnails still take the decrypt path above.
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
        // Bounded: matrix-sdk 0.18 disables its HTTP timeout for media. The timeout
        // consumes the future, so exactly one terminal event is emitted per op.
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
                    // The metadata lied or was absent: reject rather than park an unbounded
                    // payload.
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

/// Fetch a server-side thumbnail for a plain (unencrypted) mxc URI: room,
/// user and space avatars. Encrypted media never comes here.
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
        // Standard class: 40 s, below the C++ 45 s watchdog.
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
/// `bytes: 0` means unknown (none advertised, or lookup failed); C++ then
/// skips preflight and the server decides. An invented ceiling would refuse
/// files the server accepts.
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

    // Each Rust timeout stays strictly below the C++ watchdog for the same
    // class, so Rust is the normal terminal emitter.
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

    // Attached audio must carry its duration (receivers drew "0:00").
    #[test]
    fn a_timed_attachment_carries_its_duration() {
        match attachment_info("audio/mpeg", 0, 0, 4096, 185_000) {
            Some(AttachmentInfo::Audio(info)) => {
                assert_eq!(
                    info.duration,
                    Some(std::time::Duration::from_millis(185_000)),
                    "an audio attachment lost the duration the sender decoded"
                );
                // Not a voice message: that shape would mark songs as recordings.
                assert!(info.waveform.is_none());
            }
            other => panic!("expected audio info, got {other:?}"),
        }
        match attachment_info("video/mp4", 640, 480, 4096, 12_000) {
            Some(AttachmentInfo::Video(info)) => {
                assert_eq!(
                    info.duration,
                    Some(std::time::Duration::from_millis(12_000))
                );
            }
            other => panic!("expected video info, got {other:?}"),
        }
    }

    // Unknown is not zero: a duration of 0 is omitted, since receivers render
    // a literal 0 as 0:00.
    #[test]
    fn an_unknown_duration_is_omitted_rather_than_sent_as_zero() {
        match attachment_info("audio/ogg", 0, 0, 10, 0) {
            Some(AttachmentInfo::Audio(info)) => assert!(info.duration.is_none()),
            other => panic!("expected audio info, got {other:?}"),
        }
        match attachment_info("video/webm", 0, 0, 10, 0) {
            Some(AttachmentInfo::Video(info)) => assert!(info.duration.is_none()),
            other => panic!("expected video info, got {other:?}"),
        }
        // A duration on an untimed medium is ignored.
        match attachment_info("image/png", 2, 2, 10, 999) {
            Some(AttachmentInfo::Image(_)) => {}
            other => panic!("expected image info, got {other:?}"),
        }
    }

    // Every send carries typed metadata with at least the size; receivers'
    // prefetch and poster logic need a declared size.
    #[test]
    fn attachment_info_declares_size_for_every_type() {
        match attachment_info("video/mp4", 1280, 720, 1000, 0) {
            Some(AttachmentInfo::Video(info)) => {
                assert_eq!(info.size, UInt::new(1000));
                assert_eq!(info.width, UInt::new(1280));
                assert_eq!(info.height, UInt::new(720));
            }
            other => panic!("expected video info, got {other:?}"),
        }
        // Unknown dimensions are omitted, never sent as zero.
        match attachment_info("video/webm", 0, 0, 42, 0) {
            Some(AttachmentInfo::Video(info)) => {
                assert_eq!(info.size, UInt::new(42));
                assert!(info.width.is_none());
                assert!(info.height.is_none());
            }
            other => panic!("expected video info, got {other:?}"),
        }
        match attachment_info("audio/flac", 0, 0, 7, 0) {
            Some(AttachmentInfo::Audio(info)) => {
                assert_eq!(info.size, UInt::new(7));
            }
            other => panic!("expected audio info, got {other:?}"),
        }
        match attachment_info("application/pdf", 0, 0, 9, 0) {
            Some(AttachmentInfo::File(info)) => {
                assert_eq!(info.size, UInt::new(9));
            }
            other => panic!("expected file info, got {other:?}"),
        }
        // Images keep the existing contract: no dimensions, no info.
        assert!(attachment_info("image/png", 0, 0, 5, 0).is_none());
        match attachment_info("image/gif", 100, 100, 5, 0) {
            Some(AttachmentInfo::Image(info)) => {
                assert_eq!(info.is_animated, Some(true));
            }
            other => panic!("expected image info, got {other:?}"),
        }
    }

    // An outgoing video declares its duration with geometry and size, and
    // omits an unknown duration.
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

    // The poster's type comes from its bytes, never the caller; a non-raster
    // is refused, which means "no thumbnail".
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

    // Bounds and nonsense geometry degrade to "no thumbnail".
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

    // Exactly one outcome: a never-completing fetch resolves to Elapsed, a fast
    // one passes through.
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

    // A server answer is usable only if it says something: Synapse returns 200
    // with an empty object for a URL it could not fetch, and accepting that
    // would draw a blank card and skip the client fallback. Calls the real
    // function.
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
        // The route label lets the UI drop the IP warning.
        assert_eq!(only_title["preview_route"], "server");
        let only_desc = f(&json!({ "og:description": "Some page" }))
            .expect("a description alone is a usable preview");
        assert_eq!(only_desc["description"], "Some page");
        // A server preview's mxc:// og:image must reach image_source.
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

        // IPv4-mapped v6 addresses must be judged by their embedded IPv4, or an
        // AAAA record could aim a preview at loopback or private space.
        assert!(!public_ip("::ffff:127.0.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:10.0.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:192.168.1.1".parse().unwrap()));
        assert!(!public_ip("::ffff:169.254.169.254".parse().unwrap()));
        assert!(!public_ip("::ffff:100.64.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:0.0.0.0".parse().unwrap()));
        // NAT64 and the v4-compatible form embed a destination too.
        assert!(!public_ip("64:ff9b::7f00:1".parse().unwrap()));
        assert!(!public_ip("::127.0.0.1".parse().unwrap()));
        // A mapped public address stays public.
        assert!(public_ip("::ffff:93.184.216.34".parse().unwrap()));
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
        // Recognised bytes may recover a generic or mislabelled CDN response.
        assert_eq!(classify_preview_payload("application/octet-stream", &gif),
                   Ok(Some("image/gif")));
        assert_eq!(classify_preview_payload("text/html", &gif), Ok(Some("image/gif")));
        // A .gif-looking URL that returned HTML stays metadata.
        assert_eq!(classify_preview_payload("text/html", b"<html><title>Giphy</title></html>"),
                   Ok(None));
        assert_eq!(classify_preview_payload("image/gif", b"<html>not gif</html>"),
                   Err("invalid_image"));
        assert_eq!(classify_preview_payload("image/svg+xml", b"<svg></svg>"),
                   Err("unsupported_mime"));
    }

    // Direct WebP links mostly use the simple "VP8 " or lossless "VP8L" chunk,
    // not only extended "VP8X".
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
        // The shared first fetch must use the larger (image) limit.
        assert_eq!(MAX_INITIAL_FETCH_BYTES, MAX_IMAGE_BYTES);
        assert!(MAX_INITIAL_FETCH_BYTES >= MAX_HTML_BYTES);
    }

    #[test]
    fn preview_image_fields_rejects_unsupported_mime_like_svg() {
        // SVG is active content and never previewable; image_fields() is the final
        // gate.
        assert!(image_fields("image/svg+xml".to_owned(), b"<svg></svg>".to_vec()).is_err());
    }

    #[test]
    fn classify_room_error_categories() {
        assert_eq!(classify_room_error("M_LIMIT_EXCEEDED: too many requests"), "rate_limited");
        assert_eq!(classify_room_error("M_FORBIDDEN: not allowed"), "forbidden");
        assert_eq!(classify_room_error("M_ROOM_IN_USE: alias taken"), "alias_taken");
        assert_eq!(classify_room_error("M_INVALID_PARAM: bad alias"), "invalid");
        assert_eq!(classify_room_error("M_NOT_FOUND"), "not_found");
        // Synapse answers an unimplemented endpoint with 404 M_UNRECOGNIZED, so
        // this must outrank the 404 branch; asserted with the 404 present.
        assert_eq!(
            classify_room_error("the server returned an error: [404 / M_UNRECOGNIZED] Unrecognized request"),
            "unrecognized");
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

        // JPEG XL: the first bytes `cjxl` (libjxl 0.11) emits for the container and
        // bare codestream forms.
        let mut jxl_box =
            b"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a\x00\x00\x00\x14ftypjxl ".to_vec();
        jxl_box.extend_from_slice(&[0; 4]);
        assert_eq!(sniff_image_mime(&jxl_box), Some("image/jxl"));

        let mut jxl_stream = vec![0xFF, 0x0A, 0x47, 0x06];
        jxl_stream.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&jxl_stream), Some("image/jxl"));

        // JPEG and JPEG XL codestream share only their first byte.
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
    /// A room Lightning creates must let members publish call membership: with
    /// stock power levels (`state_default` 50) only moderators can, and an
    /// ordinary member's call is silently one-way.
    #[test]
    fn a_created_room_lets_ordinary_members_publish_call_membership() {
        let opts = CreateRoomOptions {
            name: "Call room".to_owned(),
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        let raw = request
            .power_level_content_override
            .expect("a room that can host a call needs the override");
        let v: serde_json::Value = raw.deserialize_as_unchecked().unwrap();
        let events = v.get("events").and_then(|e| e.as_object()).unwrap();
        for ev in [crate::rtc::EV_MEMBER_LEGACY, crate::rtc::EV_MEMBER_STICKY] {
            assert_eq!(events.get(ev).and_then(|x| x.as_u64()), Some(0), "{ev}");
        }
        // Nothing else moves: `state_default` stays, only these two types are
        // lowered.
        assert!(v.get("state_default").is_none(),
                "state_default must be left at the server default");
    }

    /// A Space holds no calls, so its permissions are not widened.
    #[test]
    fn a_space_gets_no_power_level_override() {
        let opts = CreateRoomOptions {
            name: "Team".to_owned(),
            is_space: true,
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert!(request.power_level_content_override.is_none());
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
        // The server chooses the room version.
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

    // A Space cascade offers a room only where the membership fits the action,
    // the viewer holds that room's threshold, and the target is below them.
    #[test]
    fn moderation_plan_offers_only_actions_that_can_succeed() {
        use matrix_sdk::ruma::events::room::member::MembershipState as M;
        let join = M::Join;
        let invite = M::Invite;
        let ban = M::Ban;
        let leave = M::Leave;

        // Kick: joined or invited targets only.
        assert_eq!(moderation_verdict(0, Some(&join), true, 50, 0), "");
        assert_eq!(moderation_verdict(0, Some(&invite), true, 50, 0), "");
        assert_eq!(moderation_verdict(0, Some(&leave), true, 50, 0), "not_a_member");
        assert_eq!(moderation_verdict(0, Some(&ban), true, 50, 0), "already_banned");
        assert_eq!(moderation_verdict(0, None, true, 50, 0), "not_a_member");

        // Ban: anyone with a membership record who is not already banned.
        assert_eq!(moderation_verdict(1, Some(&leave), true, 50, 0), "");
        assert_eq!(moderation_verdict(1, Some(&ban), true, 50, 0), "already_banned");

        // Unban: banned targets only.
        assert_eq!(moderation_verdict(2, Some(&ban), true, 50, 0), "");
        assert_eq!(moderation_verdict(2, Some(&join), true, 50, 0), "not_banned");

        // The room's own threshold, from the SDK.
        assert_eq!(moderation_verdict(0, Some(&join), false, 100, 0), "no_permission");

        // Strictly below the viewer: an equal level is refused, as the server
        // would refuse it.
        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 50), "outranked");
        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 100), "outranked");
        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 49), "");
        // Negative levels are real (Element's "Restricted" is -1).
        assert_eq!(moderation_verdict(0, Some(&join), true, 0, -1), "");

        assert_eq!(moderation_verdict(9, Some(&join), true, 100, 0), "invalid");
    }
}

// ---------------------------------------------------------------------------
// Scheduled send: a room-level send. The timeline sends need the room's
// live timeline open, which a scheduled message's room usually is not.
// `Room::send` reaches any joined room and encrypts like the timeline path.
// ---------------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_room_message(
    bridge: &RustClient,
    room_id: String,
    body: String,
    spec: crate::timeline::SendBodySpec,
    mention_user_ids: Vec<String>,
    reply_to: Option<String>,
    thread_root: Option<String>,
    op_id: u64,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::relation::{InReplyTo, Reply, Thread};
    use matrix_sdk::ruma::events::room::message::Relation;
    use matrix_sdk::ruma::events::AnyMessageLikeEventContent;

    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let reply_id = reply_to
        .map(|id| EventId::parse(id).map_err(|_| "invalid reply target".to_owned()))
        .transpose()?;
    let root_id = thread_root
        .map(|id| EventId::parse(id).map_err(|_| "invalid thread root".to_owned()))
        .transpose()?;
    let mut content = crate::timeline::composed_content(&body, &spec);
    if let Some(mentions) = crate::timeline::mentions_from_ids(mention_user_ids) {
        content = content.add_mentions(mentions);
    }
    content.relates_to = match (root_id, reply_id) {
        // In a thread: an explicit in-thread reply target if given, otherwise the
        // spec fallback (in_reply_to = root, marked falling-back so thread-aware
        // clients show no quote).
        (Some(root), Some(reply)) => Some(Relation::Thread(Thread::reply(root, reply))),
        (Some(root), None) => Some(Relation::Thread(Thread::plain(root.clone(), root))),
        (None, Some(reply)) => Some(Relation::Reply(Reply::new(InReplyTo::new(reply)))),
        (None, None) => None,
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = room
            .send(AnyMessageLikeEventContent::RoomMessage(content))
            .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(_) => enqueue(&events, json!({
                "type": "room_send_result", "lifecycle": lifecycle,
                "op_id": op_id, "room_id": room_id, "ok": true,
            })),
            Err(err) => enqueue(&events, json!({
                "type": "room_send_result", "lifecycle": lifecycle,
                "op_id": op_id, "room_id": room_id, "ok": false,
                "category": classify_room_error(&err.to_string()),
            })),
        }
    });
    Ok(())
}

// ---------------------------------------------------------------------------
// Activity Center seed: a fresh session has seen no sync, so ask the
// server's notification list (`only=highlight`: mentions and keywords).
// Bounded, once per session. Encrypted events cross without a body; the
// event id is enough to navigate.
// ---------------------------------------------------------------------------

pub(crate) fn request_activity_seed(bridge: &RustClient, limit: u32) -> Result<(), String> {
    use matrix_sdk::ruma::api::client::push::get_notifications;
    use matrix_sdk::ruma::UInt;

    let client = require_client(bridge)?;
    let own = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut request = get_notifications::v3::Request::new();
        request.limit = Some(UInt::from(limit.clamp(1, 100)));
        request.only = Some("highlight".to_owned());
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let Ok(response) = answer else {
            enqueue(&events, json!({
                "type": "activity_seed", "lifecycle": lifecycle, "ok": false, "entries": [],
            }));
            return;
        };
        let mut entries: Vec<serde_json::Value> = Vec::new();
        for notification in response.notifications {
            let value: serde_json::Value =
                notification.event.deserialize_as().unwrap_or(serde_json::Value::Null);
            let event_id = value["event_id"].as_str().unwrap_or_default().to_owned();
            let sender = value["sender"].as_str().unwrap_or_default().to_owned();
            if event_id.is_empty() || sender.is_empty() || sender == own {
                continue;
            }
            let event_type = value["type"].as_str().unwrap_or_default();
            let encrypted = event_type == "m.room.encrypted";
            let body: String = if event_type == "m.room.message" {
                value["content"]["body"]
                    .as_str()
                    .unwrap_or_default()
                    .chars()
                    .take(200)
                    .collect()
            } else {
                String::new()
            };
            let relates = &value["content"]["m.relates_to"];
            let thread_root = if relates["rel_type"] == "m.thread" {
                relates["event_id"].as_str().unwrap_or_default().to_owned()
            } else {
                String::new()
            };
            entries.push(json!({
                "event_id": event_id,
                "room_id": notification.room_id.to_string(),
                "sender": sender,
                "timestamp_ms": u64::from(notification.ts.get()),
                "read": notification.read,
                "encrypted": encrypted,
                "body": body,
                "thread_root_id": thread_root,
            }));
        }
        enqueue(&events, json!({
            "type": "activity_seed", "lifecycle": lifecycle, "ok": true, "entries": entries,
        }));
    });
    Ok(())
}
