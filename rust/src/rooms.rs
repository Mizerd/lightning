
use std::sync::atomic::{AtomicBool, Ordering};
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
        room::{JoinRule, RoomType},
        serde::Raw,
        EventId, OwnedMxcUri, OwnedRoomId, OwnedUserId, RoomId, UInt, UserId,
    },
    RoomMemberships, RoomState,
};
use matrix_sdk::deserialized_responses::SyncOrStrippedState;
use matrix_sdk_ui::timeline::AttachmentSource;
use serde::Deserialize;
use serde_json::json;

use crate::{enqueue, RustClient};

const MEMBER_SNAPSHOT_CAP: usize = 500;

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

const MAX_HTML_BYTES: usize = 2 * 1_048_576;
pub(crate) const MAX_IMAGE_BYTES: usize = 5 * 1_048_576;
const MAX_IMAGE_PIXELS: u64 = 25_000_000;
const MAX_REDIRECTS: usize = 4;
const CONNECT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);
const REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(12);
const PREVIEW_HEAD_BYTES: usize = 64 * 1024;

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
            if let Some(v4) = v.to_ipv4() {
                return public_ip(std::net::IpAddr::V4(v4));
            }
            let s = v.segments();
            !(v.is_loopback() || v.is_unspecified()
                || v.is_multicast() || (s[0] & 0xfe00) == 0xfc00
                || (s[0] & 0xffc0) == 0xfe80
                || (s[0] & 0xffc0) == 0xfec0
                || s[0] == 0x2002
                || (s[0] == 0x2001 && s[1] == 0x0000))
        }
    }
}

pub(crate) struct SafeResponse { pub status: reqwest::StatusCode, pub mime: String, pub location: Option<String>, pub bytes: Vec<u8> }
pub(crate) const PREVIEW_ACCEPT: &str =
    "text/html,image/jpeg,image/png,image/webp,image/gif";

async fn safe_get(url: &url::Url, limit: usize, accept: &str)
    -> Result<SafeResponse, &'static str> {
    use futures_util::StreamExt;
    let response = safe_open(url, accept, REQUEST_TIMEOUT).await?;
    let status = response.status();
    let mime = response_mime(&response);
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

pub(crate) fn response_mime(response: &reqwest::Response) -> String {
    response.headers().get(reqwest::header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok()).unwrap_or("").split(';').next().unwrap_or("")
        .trim().to_ascii_lowercase()
}

pub(crate) async fn safe_open(url: &url::Url, accept: &str,
                              timeout: std::time::Duration)
    -> Result<reqwest::Response, &'static str> {
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
        .connect_timeout(CONNECT_TIMEOUT).timeout(timeout)
        .user_agent(crate::USER_AGENT).resolve(host, addresses[0]).build()
        .map_err(|_| "request_failure")?;
    client.get(url.clone())
        .header(reqwest::header::ACCEPT, accept)
        .header(reqwest::header::ACCEPT_LANGUAGE, "en-US,en;q=0.9")
        .send().await.map_err(|e|
        if e.is_timeout() { "timeout" } else { "request_failure" })
}

fn body_error(e: reqwest::Error) -> &'static str {
    if e.is_timeout() { "timeout" } else { "request_failure" }
}

pub(crate) async fn read_head(response: &mut reqwest::Response, want: usize)
    -> Result<Vec<u8>, &'static str> {
    let mut bytes = Vec::new();
    while bytes.len() < want {
        match response.chunk().await.map_err(body_error)? {
            Some(chunk) => bytes.extend_from_slice(&chunk),
            None => break,
        }
    }
    Ok(bytes)
}

pub(crate) async fn read_rest(response: &mut reqwest::Response, bytes: &mut Vec<u8>,
                              limit: usize) -> Result<(), &'static str> {
    if bytes.len() > limit { return Err("response_too_large"); }
    while let Some(chunk) = response.chunk().await.map_err(body_error)? {
        if bytes.len() + chunk.len() > limit { return Err("response_too_large"); }
        bytes.extend_from_slice(&chunk);
    }
    Ok(())
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

pub(crate) async fn safe_open_following_redirects(
    mut url: url::Url,
    accept: &str,
    timeout: std::time::Duration,
) -> Result<(reqwest::Response, url::Url, u32), PreviewFailure> {
    for redirects in 0..=MAX_REDIRECTS {
        let redirects = redirects as u32;
        let response = safe_open(&url, accept, timeout)
            .await
            .map_err(|category| PreviewFailure {
                category,
                status: None,
                redirects,
            })?;
        let status = response.status();
        if !status.is_redirection() {
            return Ok((response, url, redirects));
        }
        let fail = |category: &'static str| PreviewFailure {
            category,
            status: Some(status.as_u16()),
            redirects,
        };
        if redirects as usize == MAX_REDIRECTS {
            return Err(fail("too_many_redirects"));
        }
        let next = response.headers().get(reqwest::header::LOCATION)
            .and_then(|v| v.to_str().ok())
            .ok_or_else(|| fail("invalid_redirect"))?;
        url = url.join(next).map_err(|_| fail("invalid_redirect"))?;
    }
    Err(PreviewFailure {
        category: "too_many_redirects",
        status: None,
        redirects: MAX_REDIRECTS as u32,
    })
}

pub(crate) fn classify_preview_payload(
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

const MAX_INITIAL_FETCH_BYTES: usize = MAX_IMAGE_BYTES;

async fn preview(page: url::Url) -> Result<serde_json::Value, PreviewFailure> {
    let (mut response, final_page, redirects) =
        safe_open_following_redirects(page, PREVIEW_ACCEPT, REQUEST_TIMEOUT).await?;
    let status = response.status();
    if !status.is_success() {
        let category = if status.is_server_error() || status.as_u16() == 429 {
            "http_transient"
        } else {
            "http_terminal"
        };
        return Err(PreviewFailure {
            category,
            status: Some(status.as_u16()),
            redirects,
        });
    }
    let fail = |category: &'static str| PreviewFailure {
        category,
        status: Some(status.as_u16()),
        redirects,
    };
    let declared = response_mime(&response);
    let declared_len = response.content_length();
    let mut bytes = read_head(&mut response, PREVIEW_HEAD_BYTES).await.map_err(fail)?;
    use crate::linkmedia::{judge_head, HeadVerdict};
    let head_image = match judge_head(&declared, &bytes, declared_len) {
        HeadVerdict::Video(mime) => {
            return Ok(crate::linkmedia::direct_video_fields(mime, declared_len));
        }
        HeadVerdict::LargeImage(mime) => {
            return Ok(crate::linkmedia::large_image_fields(
                mime, &bytes, declared_len.unwrap_or(0)));
        }
        HeadVerdict::ReadRest { image } => image,
    };
    match read_rest(&mut response, &mut bytes, MAX_INITIAL_FETCH_BYTES).await {
        Ok(()) => {}
        Err("response_too_large") if head_image.is_some() => {
            return Ok(crate::linkmedia::large_image_fields(
                head_image.unwrap_or_default(), &bytes, declared_len.unwrap_or(0)));
        }
        Err(category) => return Err(fail(category)),
    }
    drop(response);
    match classify_preview_payload(&declared, &bytes).map_err(fail)? {
        Some(mime) => {
            let mut fields = image_fields(mime.to_owned(), bytes).map_err(fail)?;
            fields["preview_kind"] = "direct_media".into();
            Ok(fields)
        }
        None => {
            if bytes.len() > MAX_HTML_BYTES {
                return Err(fail("response_too_large"));
            }
            let html = String::from_utf8_lossy(&bytes);
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
                return Err(fail("no_metadata"));
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
pub(crate) fn image_dimensions(mime: &str, b: &[u8]) -> Option<(u32,u32)> {
    let be = |i| u32::from_be_bytes([b[i],b[i+1],b[i+2],b[i+3]]);
    if mime == "image/png" && b.len() >= 24 && &b[..8] == b"\x89PNG\r\n\x1a\n" { return Some((be(16),be(20))); }
    if mime == "image/gif" && b.len() >= 10 && (&b[..6] == b"GIF87a" || &b[..6] == b"GIF89a") {
        return Some((u16::from_le_bytes([b[6],b[7]]) as u32,u16::from_le_bytes([b[8],b[9]]) as u32));
    }
    if mime == "image/webp" && b.len() >= 16 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        let chunk = &b[12..16];
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

fn looks_like_image_file_name(description: &str) -> bool {
    if description.len() > 255 || description.chars().any(char::is_control) {
        return false;
    }
    let plain = !description.chars().any(char::is_whitespace) && description.contains('.');
    let lower = description.to_ascii_lowercase();
    let image_ext = [".jpg", ".jpeg", ".png", ".webp", ".gif"]
        .iter()
        .any(|ext| lower.ends_with(ext) && lower.len() > ext.len());
    plain || image_ext
}

pub(crate) fn server_preview_fields(data: &serde_json::Value) -> Option<serde_json::Value> {
    let obj = data.as_object()?;
    let text = |k: &str| -> String {
        obj.get(k).and_then(|v| v.as_str()).unwrap_or_default().to_owned()
    };
    let title = text("og:title");
    let description = text("og:description");
    let image_source = text("og:image");
    let image_mime = text("og:image:type");
    let file_name_only = description.is_empty() || looks_like_image_file_name(&description);
    let direct_image = title.is_empty()
        && file_name_only
        && text("og:site_name").is_empty()
        && image_source.starts_with("mxc://")
        && matches!(image_mime.as_str(),
                    "image/jpeg" | "image/png" | "image/webp" | "image/gif");
    if title.is_empty() && description.is_empty() && !direct_image {
        return None;
    }
    let image_size = obj
        .get("matrix:image:size")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let dim = |k: &str| -> u64 {
        obj.get(k).and_then(|v| v.as_u64()).unwrap_or(0)
    };
    let preview_kind = if direct_image { "direct_media" } else { "metadata" };
    Some(json!({
        "preview_kind": preview_kind,
        "preview_route": "server",
        "title": clipped(title, 300),
        "description": clipped(description, 1000),
        "site_name": clipped(text("og:site_name"), 120),
        "image_source": image_source,
        "image_mime": image_mime,
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
    let sdk = require_client(bridge)?;
    let parsed = url::Url::parse(url.trim()).map_err(|_| "invalid URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("unsupported or credentialed URL".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let routed = crate::route::routed(&bridge.store_path);
    bridge.spawn_room_action(async move {
        let mut result = match server_preview(&sdk, &parsed).await {
            Some(fields) => Ok(fields),
            None if routed => Err(PreviewFailure {
                category: crate::route::BLOCKED_BY_ROUTE,
                status: None,
                redirects: 0,
            }),
            None => preview(parsed.clone()).await,
        };
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

pub(crate) async fn payload_is_direct(room: &matrix_sdk::Room, has_direct_targets: bool) -> bool {
    if has_direct_targets {
        return true;
    }
    room.state() == RoomState::Invited && room.is_direct().await.unwrap_or(false)
}

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

const CREATE_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(120);

pub(crate) fn create_request_config() -> matrix_sdk::config::RequestConfig {
    matrix_sdk::config::RequestConfig::new()
        .disable_retry()
        .timeout(CREATE_REQUEST_TIMEOUT)
}

async fn send_create_room(
    client: &matrix_sdk::Client,
    request: create_room::v3::Request,
) -> Result<OwnedRoomId, matrix_sdk::HttpError> {
    client
        .send(request)
        .with_request_config(create_request_config())
        .await
        .map(|r| r.room_id)
}

pub(crate) fn call_membership_power_levels(creator_power: bool) -> serde_json::Value {
    let mut events = serde_json::Map::new();
    for (ev, level) in [
        ("m.room.name", 50),
        ("m.room.power_levels", 100),
        ("m.room.history_visibility", 100),
        ("m.room.canonical_alias", 50),
        ("m.room.avatar", 50),
        ("m.room.tombstone", if creator_power { 150 } else { 100 }),
        ("m.room.server_acl", 100),
        ("m.room.encryption", 100),
    ] {
        events.insert(ev.to_owned(), json!(level));
    }
    for ev in [crate::rtc::EV_MEMBER_LEGACY, crate::rtc::EV_MEMBER_STICKY] {
        events.insert(ev.to_owned(), json!(0));
    }
    json!({ "events": serde_json::Value::Object(events) })
}

const ROOM_VERSION_PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(3);

async fn privileges_creators_within<F>(probe: F, limit: std::time::Duration) -> bool
where
    F: std::future::Future<Output = bool>,
{
    tokio::time::timeout(limit, probe).await.unwrap_or(false)
}

async fn default_room_version_privileges_creators(client: &matrix_sdk::Client) -> bool {
    privileges_creators_within(
        async {
            match client.homeserver_capabilities().room_versions().await {
                Ok(versions) => versions
                    .default
                    .rules()
                    .map(|r| r.authorization.explicitly_privilege_room_creators)
                    .unwrap_or(false),
                Err(_) => false,
            }
        },
        ROOM_VERSION_PROBE_TIMEOUT,
    )
    .await
}

fn with_call_override(
    mut request: create_room::v3::Request,
    creator_power: bool,
) -> create_room::v3::Request {
    request.power_level_content_override =
        Raw::new(&call_membership_power_levels(creator_power))
            .ok()
            .map(|raw| raw.cast_unchecked());
    request
}

pub(crate) fn dm_create_request(
    uid: &UserId,
    creator_power: bool,
) -> create_room::v3::Request {
    let initial_state = vec![InitialStateEvent::with_empty_state_key(
        RoomEncryptionEventContent::with_recommended_defaults(),
    )
    .to_raw_any()];
    with_call_override(
        assign!(create_room::v3::Request::new(), {
            invite: vec![uid.to_owned()],
            is_direct: true,
            preset: Some(RoomPreset::TrustedPrivateChat),
            initial_state,
        }),
        creator_power,
    )
}

#[derive(Default)]
pub(crate) struct DmCreateRegistry {
    state: std::sync::Mutex<DmCreateState>,
}

#[derive(Default)]
struct DmCreateState {
    in_flight: usize,
    created: std::collections::HashSet<OwnedRoomId>,
    unanswered_ends: Vec<u64>,
}

const UNANSWERED_BUSY_MS: u64 = 10 * 60 * 1000;

/// Counts one in-flight create until dropped (including on task abort).
pub(crate) struct DmCreateGuard(Arc<DmCreateRegistry>);

impl Drop for DmCreateGuard {
    fn drop(&mut self) {
        if let Ok(mut st) = self.0.state.lock() {
            st.in_flight = st.in_flight.saturating_sub(1);
        }
    }
}

impl DmCreateRegistry {
    pub(crate) fn begin(self: &Arc<Self>) -> DmCreateGuard {
        if let Ok(mut st) = self.state.lock() {
            st.in_flight += 1;
        }
        DmCreateGuard(Arc::clone(self))
    }
    pub(crate) fn record_created(&self, room_id: &RoomId) {
        if let Ok(mut st) = self.state.lock() {
            st.created.insert(room_id.to_owned());
        }
    }
    pub(crate) fn record_unanswered_end(&self, ended_ms: u64) {
        if let Ok(mut st) = self.state.lock() {
            st.unanswered_ends.push(ended_ms);
            let len = st.unanswered_ends.len();
            if len > 16 {
                st.unanswered_ends.drain(..len - 16);
            }
        }
    }
    pub(crate) fn busy(&self, now_ms: u64) -> bool {
        self.state.lock().map_or(true, |st| {
            st.in_flight > 0
                || st
                    .unanswered_ends
                    .iter()
                    .any(|&end| now_ms.saturating_sub(end) < UNANSWERED_BUSY_MS)
        })
    }
    #[cfg(test)]
    pub(crate) fn in_flight(&self) -> usize {
        self.state.lock().map(|st| st.in_flight).unwrap_or(1)
    }
    pub(crate) fn returned_by_a_create(&self, room_id: &RoomId) -> bool {
        self.state.lock().map(|st| st.created.contains(room_id)).unwrap_or(true)
    }
}

pub(crate) fn create_dm(bridge: &RustClient, user_id: String, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId =
        UserId::parse(&user_id).map_err(|_| "invalid Matrix user id".to_owned())?;
    if client.user_id() == Some(uid.as_ref()) {
        return Err("cannot start a direct message with yourself".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let registry = Arc::clone(&bridge.dm_creates);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let before: std::collections::HashSet<OwnedRoomId> =
            client.rooms().iter().map(|r| r.room_id().to_owned()).collect();
        let started_ms = now_ms();
        let guard = registry.begin();
        let creator_power = default_room_version_privileges_creators(&client).await;
        let result =
            send_create_room(&client, dm_create_request(&uid, creator_power)).await;
        let ended_ms = now_ms();
        let answered = match &result {
            Ok(room_id) => {
                registry.record_created(room_id);
                true
            }
            Err(err) => {
                let answer = err.as_client_api_error();
                matrix_answered(
                    answer.map(|e| e.status_code.as_u16()),
                    answer.is_some_and(|e| {
                        matches!(e.body, matrix_sdk::ruma::api::error::ErrorBody::Standard(_))
                    }),
                )
            }
        };
        // No Matrix answer (transport error, proxy status): the server may be
        // federating this invite still, so it keeps blocking OTHER requests'
        // sweeps. Recorded BEFORE the guard drops so there is no gap.
        if !answered {
            registry.record_unanswered_end(ended_ms);
        }
        drop(guard);
        if let Ok(room_id) = &result {
            let _ = tokio::time::timeout(
                std::time::Duration::from_secs(15),
                client.account().mark_as_dm(room_id, std::slice::from_ref(&uid)),
            )
            .await;
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room_id) => {
                enqueue(&events, json!({
                    "type": "dm_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "room_id": room_id.to_string(),
                }));
                let echo = tokio::time::timeout(
                    std::time::Duration::from_secs(10),
                    client.await_room_remote_echo(&room_id),
                )
                .await;
                if echo.is_ok() && timelines.lifecycle_current(lifecycle) {
                    crate::enqueue_rooms(&events, &client).await;
                }
            }
            Err(err) => {
                let category = classify_room_error(&err.to_string());
                enqueue(&events, json!({
                    "type": "dm_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
                }));
                if answered && !refused_before_creation(category) {
                    sweep_failed_dm_orphans(
                        &client, &before, started_ms, ended_ms, &registry, &timelines, lifecycle,
                    )
                    .await;
                }
            }
        }
    });
    Ok(())
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

pub(crate) fn refused_before_creation(category: &str) -> bool {
    matches!(category, "forbidden" | "rate_limited" | "invalid" | "unrecognized" | "not_found")
}

pub(crate) fn sweep_allowed(status: Option<u16>, standard_body: bool, category: &str) -> bool {
    matrix_answered(status, standard_body) && !refused_before_creation(category)
}

pub(crate) fn matrix_answered(status: Option<u16>, standard_body: bool) -> bool {
    status.is_some_and(|s| standard_body && !matches!(s, 408 | 499 | 502 | 503 | 504))
}

const CREATE_CLOCK_SKEW_MS: u64 = 30_000;

pub(crate) fn created_in_window(create_ts_ms: Option<u64>, started_ms: u64, ended_ms: u64) -> bool {
    create_ts_ms.is_some_and(|ts| {
        ts >= started_ms.saturating_sub(CREATE_CLOCK_SKEW_MS)
            && ts <= ended_ms.saturating_add(CREATE_CLOCK_SKEW_MS)
    })
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct DmOrphanProbe {
    pub joined: bool,
    pub joined_members: u64,
    pub invited_members: u64,
    pub has_name: bool,
    pub is_space: bool,
    pub known_before_request: bool,
    pub created_by_us: bool,
    pub marked_direct: bool,
    pub encrypted: bool,
    pub join_rule_invite: bool,
    pub created_in_window: bool,
    pub returned_by_a_create: bool,
}

pub(crate) fn is_failed_dm_orphan(p: &DmOrphanProbe) -> bool {
    p.joined
        && !p.known_before_request
        && !p.returned_by_a_create
        && p.created_in_window
        && p.created_by_us
        && p.joined_members == 1
        && p.invited_members == 0
        && !p.has_name
        && !p.is_space
        && !p.marked_direct
        && p.encrypted
        && p.join_rule_invite
}

async fn create_ts_ms(room: &matrix_sdk::Room) -> Option<u64> {
    use matrix_sdk::ruma::events::room::create::RoomCreateEventContent;
    match room.get_state_event_static::<RoomCreateEventContent>().await.ok()??.deserialize().ok()? {
        SyncOrStrippedState::Sync(SyncStateEvent::Original(ev)) => {
            Some(u64::from(ev.origin_server_ts.get()))
        }
        _ => None,
    }
}

async fn sleep_while_current(
    secs: u64,
    timelines: &Arc<crate::timeline::TimelineRegistry>,
    lifecycle: u64,
) -> bool {
    for _ in 0..secs {
        if !timelines.lifecycle_current(lifecycle) {
            return false;
        }
        tokio::time::sleep(std::time::Duration::from_secs(1)).await;
    }
    timelines.lifecycle_current(lifecycle)
}

async fn sweep_failed_dm_orphans(
    client: &matrix_sdk::Client,
    before: &std::collections::HashSet<OwnedRoomId>,
    started_ms: u64,
    ended_ms: u64,
    registry: &Arc<DmCreateRegistry>,
    timelines: &Arc<crate::timeline::TimelineRegistry>,
    lifecycle: u64,
) {
    let me = client.user_id().map(ToOwned::to_owned);
    for delay in [3u64, 7, 15, 35] {
        if !sleep_while_current(delay, timelines, lifecycle).await {
            return;
        }
        if registry.busy(now_ms()) {
            continue;
        }
        for room in client.joined_rooms() {
            if before.contains(room.room_id()) {
                continue;
            }
            let creators = room.creators().unwrap_or_default();
            let probe = DmOrphanProbe {
                joined: room.state() == RoomState::Joined,
                joined_members: room.joined_members_count(),
                invited_members: room.invited_members_count(),
                has_name: room.name().is_some_and(|n| !n.trim().is_empty()),
                is_space: room.is_space(),
                known_before_request: false,
                created_by_us: me.as_ref().is_some_and(|m| creators.contains(m)),
                marked_direct: room.is_direct().await.unwrap_or(true),
                encrypted: room.encryption_state().is_encrypted(),
                join_rule_invite: matches!(room.join_rule(), Some(JoinRule::Invite)),
                created_in_window: created_in_window(
                    create_ts_ms(&room).await,
                    started_ms,
                    ended_ms,
                ),
                returned_by_a_create: registry.returned_by_a_create(room.room_id()),
            };
            if !is_failed_dm_orphan(&probe) {
                continue;
            }
            // Re-check immediately before the destructive step.
            if !timelines.lifecycle_current(lifecycle) || registry.busy(now_ms()) {
                return;
            }
            if room.leave().await.is_ok() {
                let _ = room.forget().await;
            }
            return;
        }
    }
}

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
    #[serde(default)]
    pub is_space: bool,
}

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
    let power_level_content_override = if opts.is_space {
        None
    } else {
        Some(
            Raw::new(&call_membership_power_levels(false))
                .map_err(|e| format!("power level override: {e}"))?
                .cast_unchecked(),
        )
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
    let mut request = build_create_room_request(&opts)?;
    let is_space = opts.is_space;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        if !is_space {
            let creator_power = default_room_version_privileges_creators(&client).await;
            request = with_call_override(request, creator_power);
        }
        let result = send_create_room(&client, request)
            .await
            .map_err(|e| classify_room_error(&e.to_string()));
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(room_id) => {
                let mut warning = String::new();
                if !opts.space_id.is_empty() {
                    let placed = add_space_child(&client, &opts.space_id, room_id.as_str())
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
                    "room_id": room_id.to_string(),
                    "warning": warning,
                }));
                let echo = tokio::time::timeout(
                    std::time::Duration::from_secs(10),
                    client.await_room_remote_echo(&room_id),
                )
                .await;
                if echo.is_ok() && timelines.lifecycle_current(lifecycle) {
                    crate::enqueue_rooms(&events, &client).await;
                }
            }
            Err(category) => {
                enqueue(&events, json!({
                    "type": "room_create_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "category": category,
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

fn ordered_child_content(mut content: serde_json::Value, position: usize) -> Result<serde_json::Value, String> {
    let mut check = content.clone();
    check.as_object_mut().ok_or_else(|| "invalid child".to_owned())?.remove("order");
    let parsed: SpaceChildEventContent = serde_json::from_value(check)
        .map_err(|_| "invalid child".to_owned())?;
    if parsed.via.is_empty() { return Err("not a child".into()); }
    content["order"] = json!(format!("{position:06}"));
    Ok(content)
}

#[cfg(test)]
mod default_order_tests {
    use super::*;
    #[test]
    fn one_unwritable_child_means_an_empty_plan() {
        let ids: Vec<String> = ["!a:x", "!b:x", "!parentonly:x"].map(String::from).to_vec();
        let ok = || Some(json!({"via": ["x"]}));
        assert!(plan_child_orders(&ids, vec![ok(), ok(), None]).is_err());
        assert!(plan_child_orders(&ids, vec![ok(), Some(json!({"via": []})), ok()]).is_err());
        let plan = plan_child_orders(&ids, vec![ok(), ok(), ok()]).unwrap();
        assert_eq!(plan.len(), 3);
        assert_eq!(plan[2].1["order"], "000002");
    }

    #[test]
    fn changing_order_preserves_routing_suggestions_and_extensions() {
        let content = json!({"via": ["example.org"], "suggested": true,
            "order": 4, "org.example.hint": {"keep": true}});
        let mut wanted = content.clone();
        wanted["order"] = json!("000012");
        assert_eq!(ordered_child_content(content, 12).unwrap(), wanted);
        for removed in [json!({}), json!({"via": []}), json!({"via": [42]})] {
            assert!(ordered_child_content(removed, 0).is_err());
        }
    }
}

pub(crate) async fn set_space_child_order_inner(
    client: &matrix_sdk::Client,
    space_id: &str,
    ids: &[String],
    current: impl Fn() -> bool,
) -> Result<(), String> {
    let space = RoomId::parse(space_id).ok()
        .and_then(|id| client.get_room(&id))
        .filter(|r| r.state() == RoomState::Joined && r.is_space())
        .ok_or_else(|| "unknown space".to_owned())?;
    let mut contents = Vec::with_capacity(ids.len());
    for id in ids {
        if !current() { return Err("cancelled".into()); }
        let child = RoomId::parse(id).map_err(|_| "invalid child".to_owned())?;
        let state = space.get_state_event_static_for_key::<SpaceChildEventContent, _>(&child)
            .await.map_err(|_| "state unavailable".to_owned())?;
        contents.push(state.and_then(|state| {
            use matrix_sdk::deserialized_responses::RawSyncOrStrippedState;
            let RawSyncOrStrippedState::Sync(raw) = state else { return None };
            let value: serde_json::Value = serde_json::from_str(raw.json().get()).ok()?;
            value.get("content").cloned()
        }));
    }
    for (id, content) in plan_child_orders(ids, contents)? {
        if !current() { return Err("cancelled".into()); }
        space.send_state_event_raw("m.space.child", &id, content).await
            .map_err(|_| "order write failed".to_owned())?;
    }
    Ok(())
}

fn plan_child_orders(
    ids: &[String],
    contents: Vec<Option<serde_json::Value>>,
) -> Result<Vec<(String, serde_json::Value)>, String> {
    let mut plan = Vec::with_capacity(ids.len());
    for (position, (id, content)) in ids.iter().zip(contents).enumerate() {
        let content = content.ok_or_else(|| "not a child".to_owned())?;
        plan.push((id.clone(), ordered_child_content(content, position)?));
    }
    Ok(plan)
}

pub(crate) fn set_space_child_order(
    bridge: &RustClient,
    space_id: String,
    ids: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    if ids.is_empty() || ids.len() > 1000
        || ids.iter().collect::<std::collections::BTreeSet<_>>().len() != ids.len()
        || ids.iter().any(|id| RoomId::parse(id).is_err()) {
        return Err("invalid child list".to_owned());
    }
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = set_space_child_order_inner(&client, &space_id, &ids,
            || timelines.lifecycle_current(lifecycle)).await;
        if !timelines.lifecycle_current(lifecycle) { return; }
        enqueue(&events, json!({"type": "space_child_order_result", "op_id": op_id,
            "lifecycle": lifecycle, "space_id": space_id, "ok": result.is_ok()}));
    });
    Ok(())
}

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

const INVITE_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(40);

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
                Ok(uid) => match tokio::time::timeout(
                    INVITE_REQUEST_TIMEOUT,
                    room.invite_user_by_id(&uid),
                )
                .await
                {
                    Ok(done) => done
                        .map_err(|err| classify_room_error(&err.to_string()).to_owned()),
                    // A federated invite can outlive any answer while the
                    // server still performs it: ask the store before saying
                    // it did not happen.
                    Err(_) => {
                        use matrix_sdk::ruma::events::room::member::MembershipState;
                        let member = room.get_member_no_sync(&uid).await.ok().flatten();
                        match member.map(|m| m.membership().clone()) {
                            Some(MembershipState::Invite) | Some(MembershipState::Join) => Ok(()),
                            _ => Err("timeout".to_owned()),
                        }
                    }
                },
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

const THREAD_PARTICIPANT_CAP: usize = 8;

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
            return;
        }
        let Ok((root_event, replies)) = loaded else {
            enqueue(&events, json!({
                "type": "thread_participants",
                "lifecycle": lifecycle,
                "room_id": room_id,
                "root_event_id": root_event_id,
                "ok": false,
            }));
            return;
        };

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

const EDIT_REDACTION_CAP: usize = 50;

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
            return;
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
                continue;
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
            "ok": failed == 0,
            "removed": removed,
            "failed": failed,
            "truncated": truncated,
        }));
    });
    Ok(())
}

const MEMBER_REFRESH_GAP_PER_MEMBER: std::time::Duration =
    std::time::Duration::from_millis(10);
const MEMBER_REFRESH_GAP_MAX: std::time::Duration = std::time::Duration::from_secs(60);
const MEMBER_REFRESH_GAP_ENCRYPTED_MIN: std::time::Duration =
    std::time::Duration::from_secs(5);
const MEMBER_REFRESH_GAP_ENCRYPTED_PER_MEMBER: std::time::Duration =
    std::time::Duration::from_millis(30);
const MEMBER_REFRESH_GAP_ENCRYPTED_MAX: std::time::Duration =
    std::time::Duration::from_secs(300);
const MEMBER_REFRESH_BACKOFF_MIN: std::time::Duration = std::time::Duration::from_secs(30);
const MEMBER_REFRESH_BACKOFF_MAX: std::time::Duration = std::time::Duration::from_secs(600);
const MEMBER_SCAN_LIMIT: usize = 2_000;

fn member_refresh_gap(active: u64, encrypted: bool) -> std::time::Duration {
    let active = u32::try_from(active).unwrap_or(u32::MAX);
    if encrypted {
        MEMBER_REFRESH_GAP_ENCRYPTED_PER_MEMBER
            .saturating_mul(active)
            .clamp(MEMBER_REFRESH_GAP_ENCRYPTED_MIN, MEMBER_REFRESH_GAP_ENCRYPTED_MAX)
    } else {
        MEMBER_REFRESH_GAP_PER_MEMBER
            .saturating_mul(active)
            .min(MEMBER_REFRESH_GAP_MAX)
    }
}

fn member_refresh_backoff(failures: u32, gap: std::time::Duration) -> std::time::Duration {
    let doublings = failures.saturating_sub(1).min(16);
    gap.max(MEMBER_REFRESH_BACKOFF_MIN)
        .saturating_mul(1u32 << doublings)
        .min(MEMBER_REFRESH_BACKOFF_MAX)
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct MemberFacts {
    membership: String,
    display_name: String,
    avatar_url: String,
}

impl MemberFacts {
    fn of_member(member: &matrix_sdk::room::RoomMember) -> Self {
        Self {
            membership: member.membership().as_str().to_owned(),
            display_name: member.display_name().unwrap_or_default().to_owned(),
            avatar_url: member.avatar_url().map(|a| a.to_string()).unwrap_or_default(),
        }
    }

    fn of_content(content: &serde_json::Value) -> Self {
        let text = |key: &str| content.get(key).and_then(|v| v.as_str()).unwrap_or("").to_owned();
        Self {
            membership: text("membership"),
            display_name: text("displayname"),
            avatar_url: text("avatar_url"),
        }
    }
}

fn store_agrees(stored: Option<&MemberFacts>, seen: &MemberFacts) -> bool {
    match stored {
        None => matches!(seen.membership.as_str(), "leave" | "knock"),
        Some(stored) if stored.membership != seen.membership => false,
        Some(stored) => {
            !matches!(seen.membership.as_str(), "join" | "invite")
                || (stored.display_name == seen.display_name
                    && stored.avatar_url == seen.avatar_url)
        }
    }
}

#[derive(Debug, Default)]
struct MemberScan {
    newest: Option<matrix_sdk::ruma::OwnedEventId>,
    stale: usize,
}

async fn scan_member_events(
    room: &matrix_sdk::Room,
    since: Option<&EventId>,
) -> MemberScan {
    let Ok((cache, _drop_handles)) = room.event_cache().await else {
        return MemberScan::default();
    };
    let mut newest: Option<matrix_sdk::ruma::OwnedEventId> = None;
    let mut latest: std::collections::HashMap<matrix_sdk::ruma::OwnedUserId, MemberFacts> =
        std::collections::HashMap::new();
    let mut walked = 0usize;
    let _ = cache
        .rfind_map_event_in_memory_by(|event| {
            walked += 1;
            if walked > MEMBER_SCAN_LIMIT {
                return Some(());
            }
            let raw = event.raw();
            let is_member = raw
                .get_field::<String>("type")
                .ok()
                .flatten()
                .is_some_and(|ty| ty == "m.room.member");
            if !is_member {
                return None;
            }
            let id = event.event_id();
            if id.is_some() && id.as_deref() == since {
                return Some(());
            }
            if newest.is_none() {
                newest = id;
            }
            if let Some(user) = raw
                .get_field::<matrix_sdk::ruma::OwnedUserId>("state_key")
                .ok()
                .flatten()
            {
                latest.entry(user).or_insert_with(|| {
                    MemberFacts::of_content(
                        &raw.get_field::<serde_json::Value>("content")
                            .ok()
                            .flatten()
                            .unwrap_or_default(),
                    )
                });
            }
            None
        })
        .await;
    let mut stale = 0;
    for (user, seen) in &latest {
        let stored = room
            .get_member_no_sync(user)
            .await
            .ok()
            .flatten()
            .map(|member| MemberFacts::of_member(&member));
        if !store_agrees(stored.as_ref(), seen) {
            stale += 1;
        }
    }
    MemberScan { newest, stale }
}

#[derive(Default)]
struct MemberRefreshState {
    owner: std::sync::Weak<crate::timeline::TimelineRegistry>,
    started: Option<std::time::Instant>,
    in_flight: bool,
    watermark: Option<matrix_sdk::ruma::OwnedEventId>,
    failures: u32,
    retry_after: Option<std::time::Instant>,
    left_missing: bool,
    deferred: bool,
    poke_owed: bool,
}

type MemberRefreshKey = (usize, u64, OwnedRoomId);

type MemberRefreshRegistry = std::collections::HashMap<MemberRefreshKey, MemberRefreshState>;

fn member_refresh_registry() -> &'static std::sync::Mutex<MemberRefreshRegistry> {
    static REGISTRY: std::sync::OnceLock<std::sync::Mutex<MemberRefreshRegistry>> =
        std::sync::OnceLock::new();
    REGISTRY.get_or_init(Default::default)
}

fn member_refresh_entry<'a>(
    registry: &'a mut MemberRefreshRegistry,
    key: &MemberRefreshKey,
    owner: &Arc<crate::timeline::TimelineRegistry>,
) -> &'a mut MemberRefreshState {
    let (bridge, lifecycle, _) = key;
    registry.retain(|(b, l, _), state| {
        state.owner.strong_count() > 0 && (b != bridge || l == lifecycle)
    });
    registry.entry(key.clone()).or_insert_with(|| MemberRefreshState {
        owner: Arc::downgrade(owner),
        ..Default::default()
    })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MemberStep {
    Skip,
    SdkSync,
    ServeWhileRunning,
    Defer(std::time::Instant),
    Refresh,
}

fn member_step(
    state: &MemberRefreshState,
    now: std::time::Instant,
    synced: bool,
    stale: bool,
    gap: std::time::Duration,
) -> MemberStep {
    if state.in_flight {
        return MemberStep::ServeWhileRunning;
    }
    let left_missing = state.left_missing && !synced;
    if !synced && !left_missing {
        return MemberStep::SdkSync;
    }
    if !stale && !left_missing {
        return MemberStep::Skip;
    }
    let mut at = now;
    if let Some(started) = state.started {
        at = at.max(started + gap);
    }
    if let Some(retry_after) = state.retry_after {
        at = at.max(retry_after);
    }
    if at > now { MemberStep::Defer(at) } else { MemberStep::Refresh }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MemberFreshness {
    Store,
    StoreNoSync,
    Refreshed,
    FailedUseStore,
    Failed(&'static str),
}

fn members_trace_enabled() -> bool {
    std::env::var_os("LIGHTNING_MEMBERS_TRACE").is_some()
}

fn member_fingerprints(
    members: &[matrix_sdk::room::RoomMember],
) -> std::collections::HashMap<String, MemberFacts> {
    members
        .iter()
        .map(|m| (m.user_id().to_string(), MemberFacts::of_member(m)))
        .collect()
}

fn count_changed_members(
    before: &std::collections::HashMap<String, MemberFacts>,
    after: &std::collections::HashMap<String, MemberFacts>,
) -> usize {
    let mut changed = after
        .iter()
        .filter(|(user, value)| before.get(*user) != Some(*value))
        .count();
    changed += before.keys().filter(|user| !after.contains_key(*user)).count();
    changed
}

#[derive(Clone)]
struct MemberRefreshCtx {
    timelines: Arc<crate::timeline::TimelineRegistry>,
    lifecycle: u64,
    events: crate::EventQueueRef,
}

impl MemberRefreshCtx {
    fn key(&self, room: &matrix_sdk::Room) -> MemberRefreshKey {
        (
            Arc::as_ptr(&self.timelines) as *const () as usize,
            self.lifecycle,
            room.room_id().to_owned(),
        )
    }

    fn trace(&self, room: &matrix_sdk::Room, line: String) {
        enqueue(&self.events, json!({
            "type": "rust_trace",
            "line": format!(
                "members-trace room={} {line}",
                crate::redact_matrix_id(room.room_id().as_str())
            ),
        }));
    }

    fn poke(&self, room: &matrix_sdk::Room) {
        if self.timelines.lifecycle_current(self.lifecycle) {
            enqueue(&self.events, json!({
                "type": "room_members_changed",
                "room_id": room.room_id().to_string(),
            }));
        }
    }
}

async fn decide_member_step(
    room: &matrix_sdk::Room,
    ctx: &MemberRefreshCtx,
    deferred: bool,
) -> (MemberStep, MemberScan, std::time::Duration, bool) {
    let key = ctx.key(room);
    let since = member_refresh_registry()
        .lock()
        .unwrap()
        .get(&key)
        .and_then(|state| state.watermark.clone());
    let scan = scan_member_events(room, since.as_deref()).await;
    let gap = member_refresh_gap(
        room.active_members_count(),
        room.encryption_state().is_encrypted(),
    );
    let mut registry = member_refresh_registry().lock().unwrap();
    let state = member_refresh_entry(&mut registry, &key, &ctx.timelines);
    let synced = room.are_members_synced();
    if synced {
        state.left_missing = false;
    }
    let step = member_step(state, std::time::Instant::now(), synced, scan.stale > 0, gap);
    let mut schedule = false;
    match step {
        MemberStep::Skip => {
            if let Some(newest) = &scan.newest {
                state.watermark = Some(newest.clone());
            }
        }
        MemberStep::ServeWhileRunning => state.poke_owed = true,
        MemberStep::Defer(_) => {
            schedule = !state.deferred && !deferred;
            if schedule {
                state.deferred = true;
            }
        }
        MemberStep::Refresh => {
            state.started = Some(std::time::Instant::now());
            state.in_flight = true;
            room.mark_members_missing();
        }
        MemberStep::SdkSync => {}
    }
    (step, scan, gap, schedule)
}

async fn forced_member_refresh(
    room: &matrix_sdk::Room,
    ctx: &MemberRefreshCtx,
    scan: &MemberScan,
    gap: std::time::Duration,
    origin: &str,
) -> (bool, usize) {
    let key = ctx.key(room);
    struct InFlight(MemberRefreshKey);
    impl Drop for InFlight {
        fn drop(&mut self) {
            if let Ok(mut registry) = member_refresh_registry().lock() {
                if let Some(state) = registry.get_mut(&self.0) {
                    state.in_flight = false;
                }
            }
        }
    }
    let _in_flight = InFlight(key.clone());
    let before = room
        .members_no_sync(RoomMemberships::all())
        .await
        .map(|m| member_fingerprints(&m))
        .unwrap_or_default();
    // Signed out while deciding or reading `before`: no request from a dead
    // session (sign-out reads the final tokens on the premise that none is
    // sent). The state is left as it was; the in-flight mark clears on drop.
    if !ctx.timelines.lifecycle_current(ctx.lifecycle) {
        return (false, 0);
    }
    let started = std::time::Instant::now();
    let result = room.sync_members().await;
    let ok = result.is_ok();
    let changed = if ok {
        let after = room
            .members_no_sync(RoomMemberships::all())
            .await
            .map(|m| member_fingerprints(&m))
            .unwrap_or_default();
        count_changed_members(&before, &after)
    } else {
        0
    };
    let poke_owed = {
        let mut registry = member_refresh_registry().lock().unwrap();
        match registry.get_mut(&key) {
            Some(state) => {
                if ok {
                    state.failures = 0;
                    state.retry_after = None;
                    state.left_missing = false;
                    if let Some(newest) = &scan.newest {
                        state.watermark = Some(newest.clone());
                    }
                } else {
                    state.failures = state.failures.saturating_add(1);
                    state.retry_after = Some(
                        std::time::Instant::now() + member_refresh_backoff(state.failures, gap),
                    );
                    state.left_missing = !room.are_members_synced();
                }
                std::mem::take(&mut state.poke_owed)
            }
            None => false,
        }
    };
    if members_trace_enabled() {
        ctx.trace(room, format!(
            "refetch origin={origin} ok={ok} ms={} stale={} changed={changed} gap_ms={}",
            started.elapsed().as_millis(),
            scan.stale,
            gap.as_millis(),
        ));
    }
    if ok && changed > 0 && (poke_owed || origin == "deferred") {
        ctx.poke(room);
    }
    (ok, changed)
}

struct DeferredMemberRefresh {
    key: MemberRefreshKey,
    at: std::time::Instant,
}

impl Drop for DeferredMemberRefresh {
    fn drop(&mut self) {
        if let Ok(mut registry) = member_refresh_registry().lock() {
            if let Some(state) = registry.get_mut(&self.key) {
                state.deferred = false;
            }
        }
    }
}

impl DeferredMemberRefresh {
    async fn run(self, room: &matrix_sdk::Room, ctx: &MemberRefreshCtx) {
        let mut at = self.at;
        for _ in 0..4 {
            while std::time::Instant::now() < at {
                if !ctx.timelines.lifecycle_current(ctx.lifecycle) {
                    return;
                }
                let left = at.saturating_duration_since(std::time::Instant::now());
                tokio::time::sleep(left.min(std::time::Duration::from_millis(200))).await;
            }
            if !ctx.timelines.lifecycle_current(ctx.lifecycle) {
                return;
            }
            let (step, scan, gap, _) = decide_member_step(room, ctx, true).await;
            match step {
                MemberStep::Defer(next) => at = next,
                MemberStep::Refresh => {
                    forced_member_refresh(room, ctx, &scan, gap, "deferred").await;
                    return;
                }
                MemberStep::SdkSync => {
                    ctx.poke(room);
                    return;
                }
                MemberStep::Skip | MemberStep::ServeWhileRunning => return,
            }
        }
    }
}

async fn ensure_fresh_members(
    room: &matrix_sdk::Room,
    ctx: &MemberRefreshCtx,
) -> (MemberFreshness, Option<DeferredMemberRefresh>) {
    if !ctx.timelines.lifecycle_current(ctx.lifecycle) {
        return (MemberFreshness::Store, None);
    }
    let (step, scan, gap, schedule) = decide_member_step(room, ctx, false).await;
    let deferred = match step {
        MemberStep::Defer(at) if schedule => Some(DeferredMemberRefresh { key: ctx.key(room), at }),
        _ => None,
    };
    let freshness = match step {
        MemberStep::Skip => MemberFreshness::Store,
        MemberStep::SdkSync => match room.sync_members().await {
            Ok(()) => {
                let key = ctx.key(room);
                if let Some(state) = member_refresh_registry().lock().unwrap().get_mut(&key) {
                    if let Some(newest) = &scan.newest {
                        state.watermark = Some(newest.clone());
                    }
                }
                MemberFreshness::Refreshed
            }
            Err(err) => MemberFreshness::Failed(classify_room_error(&err.to_string())),
        },
        MemberStep::ServeWhileRunning => MemberFreshness::StoreNoSync,
        MemberStep::Defer(at) => {
            if members_trace_enabled() {
                ctx.trace(room, format!(
                    "deferred stale={} in_ms={} scheduled={schedule}",
                    scan.stale,
                    at.saturating_duration_since(std::time::Instant::now()).as_millis(),
                ));
            }
            MemberFreshness::StoreNoSync
        }
        MemberStep::Refresh => {
            let (ok, _) = forced_member_refresh(room, ctx, &scan, gap, "read").await;
            if ok { MemberFreshness::Refreshed } else { MemberFreshness::FailedUseStore }
        }
    };
    (freshness, deferred)
}

fn own_membership_str(state: RoomState) -> &'static str {
    match state {
        RoomState::Joined => "joined",
        RoomState::Invited => "invited",
        RoomState::Left => "left",
        RoomState::Knocked => "knocked",
        RoomState::Banned => "banned",
    }
}

fn own_send_and_redact(
    room: &matrix_sdk::Room,
    own_member: Option<&matrix_sdk::room::RoomMember>,
) -> (Option<bool>, Option<bool>) {
    use matrix_sdk::ruma::events::MessageLikeEventType;
    let can_send_message = own_member.map(|m| {
        m.can_send_message(MessageLikeEventType::RoomMessage)
            && (!room.encryption_state().is_encrypted()
                || m.can_send_message(MessageLikeEventType::RoomEncrypted))
    });
    let can_redact_other = own_member.map(|m| m.can_redact_other());
    (can_send_message, can_redact_other)
}

fn own_moderation_facts(
    own_member: Option<&matrix_sdk::room::RoomMember>,
) -> [Option<bool>; 6] {
    [
        own_member.map(|m| m.can_kick()),
        own_member.map(|m| m.can_ban()),
        own_member.map(|m| m.can_do(PowerLevelAction::Unban)),
        own_member.map(|m| m.can_invite()),
        own_member.map(|m| m.can_send_state(StateEventType::RoomTopic)),
        own_member.map(|m| m.can_send_state(StateEventType::RoomName)),
    ]
}

pub(crate) fn room_own_permissions(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = RoomId::parse(&room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .ok_or_else(|| "unknown room".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let own_id = client.user_id().map(|u| u.to_owned());
    bridge.spawn_room_action(async move {
        let mut payload = own_permissions_payload(&room, own_id.as_deref()).await;
        if timelines.lifecycle_current(lifecycle) {
            payload["type"] = json!("room_own_permissions");
            payload["op_id"] = json!(op_id);
            payload["lifecycle"] = json!(lifecycle);
            payload["room_id"] = json!(room_id);
            enqueue(&events, payload);
        }
    });
    Ok(())
}

async fn own_permissions_payload(
    room: &matrix_sdk::Room,
    own_id: Option<&matrix_sdk::ruma::UserId>,
) -> serde_json::Value {
    let own_membership = own_membership_str(room.state());
    if room.state() != RoomState::Joined {
        return json!({
            "own_membership": own_membership,
            "own_can_send_message": null,
            "own_can_redact_other": null,
            "own_can_kick": null,
            "own_can_ban": null,
            "own_can_unban": null,
            "own_can_invite": null,
            "own_can_edit_topic": null,
            "own_can_edit_name": null,
        });
    }
    let own_member = match own_id {
        Some(own) => room.get_member_no_sync(own).await.ok().flatten(),
        None => None,
    };
    let (can_send_message, can_redact_other) =
        own_send_and_redact(room, own_member.as_ref());
    let [kick, ban, unban, invite, edit_topic, edit_name] =
        own_moderation_facts(own_member.as_ref());
    json!({
        "own_membership": own_membership,
        "own_can_send_message": can_send_message,
        "own_can_redact_other": can_redact_other,
        "own_can_kick": kick,
        "own_can_ban": ban,
        "own_can_unban": unban,
        "own_can_invite": invite,
        "own_can_edit_topic": edit_topic,
        "own_can_edit_name": edit_name,
    })
}

pub(crate) fn room_members(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = RoomId::parse(&room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .ok_or_else(|| "unknown or not-joined room".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    if room.state() != RoomState::Joined {
        let own_membership = own_membership_str(room.state());
        bridge.spawn_room_action(async move {
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, json!({
                    "type": "room_members",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id,
                    "ok": false,
                    "category": "not_joined",
                    "own_membership": own_membership,
                }));
            }
        });
        return Ok(());
    }
    let own_id = client.user_id().map(|u| u.to_owned());
    let refresh_ctx = MemberRefreshCtx {
        timelines: Arc::clone(&timelines),
        lifecycle,
        events: Arc::clone(&events),
    };
    bridge.spawn_room_action(async move {
        if let Ok(cached) = room
            .members_no_sync(RoomMemberships::ACTIVE | RoomMemberships::BAN)
            .await
        {
            if !cached.is_empty() && timelines.lifecycle_current(lifecycle) {
                let snapshot = members_snapshot_json(
                    &room, &cached, own_id.as_deref(), &room_id, op_id,
                    lifecycle, true,
                )
                .await;
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, snapshot);
                }
            }
        }

        let (freshness, deferred) = ensure_fresh_members(&room, &refresh_ctx).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }

        let wanted = RoomMemberships::ACTIVE | RoomMemberships::BAN;
        let read = match freshness {
            MemberFreshness::Store => room.members(wanted).await,
            MemberFreshness::Refreshed
            | MemberFreshness::FailedUseStore
            | MemberFreshness::StoreNoSync => room.members_no_sync(wanted).await,
            MemberFreshness::Failed(category) => {
                if timelines.lifecycle_current(lifecycle) {
                    enqueue(&events, json!({
                        "type": "room_members",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "room_id": room_id,
                        "ok": false,
                        "category": category,
                    }));
                }
                return;
            }
        };
        let members = match read {
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
 false,
        )
        .await;
        if timelines.lifecycle_current(lifecycle) {
            enqueue(&events, snapshot);
        }
        if let Some(deferred) = deferred {
            deferred.run(&room, &refresh_ctx).await;
        }
    });
    Ok(())
}

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
                    "power_level": power_level_int(member.power_level()),
                    "ambiguous": member.name_ambiguous(),
                    "is_own": Some(member.user_id()) == own_id,
                })
            })
            .collect();

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
        let can_notify_room = own_member.as_ref().is_some_and(|m| {
            m.can_do(PowerLevelAction::TriggerNotification(
                NotificationPowerLevelType::Room,
            ))
        });
        let can_kick = own_member.as_ref().is_some_and(|m| m.can_kick());
        let can_ban = own_member.as_ref().is_some_and(|m| m.can_ban());
        let can_unban = own_member
            .as_ref()
            .is_some_and(|m| m.can_do(PowerLevelAction::Unban));
        let own_power_level = own_member
            .as_ref()
            .map(|m| power_level_int(m.power_level()))
            .unwrap_or(0);
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
        let can_manage_space_children = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::SpaceChild));
        let power_levels = room.power_levels_or_default().await;
        let state_level = |ty: StateEventType| -> i64 {
            power_levels
                .events
                .get(&TimelineEventType::from(ty))
                .map(|value| i64::from(*value))
                .unwrap_or_else(|| i64::from(power_levels.state_default))
        };
        let users_default: i64 = i64::from(power_levels.users_default);
        let room_version = room
            .version()
            .map(|version| version.to_string())
            .unwrap_or_default();
        let can_upgrade = own_member
            .as_ref()
            .is_some_and(|m| m.can_send_state(StateEventType::RoomTombstone));
        let can_publish_rtc_membership: Option<bool> = own_member.as_ref().map(|m| {
            m.can_send_state(StateEventType::from(crate::rtc::EV_MEMBER_LEGACY))
        });
        let (can_send_message, can_redact_other) =
            own_send_and_redact(&room, own_member.as_ref());
        let join_rule = join_rule_str(room.join_rule().as_ref());
        let canonical_alias = room
            .canonical_alias()
            .map(|a| a.to_string())
            .unwrap_or_default();
        let canonical_space_parent = canonical_space_parent_json(&room.client(), room).await;
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
            "notifications.room": i64::from(power_levels.notifications.room),
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
            "own_can_send_message": can_send_message,
            "own_can_redact_other": can_redact_other,
            "own_membership": own_membership_str(room.state()),
            "room_version": room_version,
            "power_levels": power_levels_json,
            "join_rule": join_rule,
            "canonical_alias": canonical_alias,
            "canonical_space_parent": canonical_space_parent,
            "access": access_json,
            "members": rows,
        })
    }
}

pub(crate) async fn canonical_space_parent_json(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::Room,
) -> serde_json::Value {
    use matrix_sdk::ruma::events::space::parent::SpaceParentEventContent;
    let Ok(events) = room.get_state_events_static::<SpaceParentEventContent>().await else {
        return serde_json::Value::Null;
    };
    for raw in events {
        let Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(event))) = raw.deserialize()
        else {
            continue;
        };
        if event.content.canonical != true || event.content.via.is_empty() {
            continue;
        }
        let space = client.get_room(&event.state_key);
        let joined = space.as_ref().is_some_and(|s| s.state() == RoomState::Joined);
        let name = space.as_ref().and_then(|s| s.name()).unwrap_or_default();
        return json!({
            "space_id": event.state_key.as_str(),
            "via": event.content.via.iter().map(|v| v.to_string()).collect::<Vec<_>>(),
            "joined": joined,
            "name": name,
        });
    }
    serde_json::Value::Null
}

/// The bridge integer for an MSC4289 creator ("Infinite"): 2^53, one above
/// the largest finite level (a canonical-JSON integer is at most 2^53 - 1),
/// and exactly representable in the JSON f64 hop. C++ mirrors it as
/// kCreatorPowerLevel in RoomInfoController.cpp.
pub(crate) const CREATOR_POWER_LEVEL: i64 = 1 << 53;

fn power_level_int(
    level: matrix_sdk::ruma::events::room::power_levels::UserPowerLevel,
) -> i64 {
    use matrix_sdk::ruma::events::room::power_levels::UserPowerLevel;
    match level {
        UserPowerLevel::Infinite => CREATOR_POWER_LEVEL,
        UserPowerLevel::Int(v) => v.into(),
        _ => 0,
    }
}

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

const MODERATION_PLAN_CAP: usize = 100;

const MODERATION_PLAN_MEMBER_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(15);

pub(crate) const MODERATION_PLAN_BUDGET: std::time::Duration =
    std::time::Duration::from_secs(25);

pub(crate) const MODERATION_PLAN_CONCURRENCY: usize = 6;

pub(crate) async fn assess_within_budget<T, R, F, Fut, L>(
    items: Vec<T>,
    concurrency: usize,
    deadline: tokio::time::Instant,
    assess: F,
    late: L,
) -> Vec<R>
where
    T: Clone,
    F: Fn(T) -> Fut,
    Fut: std::future::Future<Output = R>,
    L: Fn(&T) -> R,
{
    use futures_util::StreamExt;
    let answers: Vec<(T, Option<R>)> = futures_util::stream::iter(items)
        .map(|item| {
            let work = assess(item.clone());
            async move { (item, tokio::time::timeout_at(deadline, work).await.ok()) }
        })
        .buffered(concurrency.max(1))
        .collect()
        .await;
    answers
        .into_iter()
        .map(|(item, answer)| answer.unwrap_or_else(|| late(&item)))
        .collect()
}

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
        let deadline = tokio::time::Instant::now() + MODERATION_PLAN_BUDGET;
        let assess = |room_id: String| {
            let client = client.clone();
            let uid = uid.clone();
            let own_id = own_id.clone();
            async move { moderation_plan_row(&client, room_id, &uid, &own_id, op).await }
        };
        let rows = assess_within_budget(
            room_ids,
            MODERATION_PLAN_CONCURRENCY,
            deadline,
            assess,
            |room_id: &String| {
                let room = RoomId::parse(room_id).ok().and_then(|id| client.get_room(&id));
                json!({
                    "room_id": room_id,
                    "name": room
                        .as_ref()
                        .and_then(|r| r.cached_display_name())
                        .map(|n| n.to_string())
                        .unwrap_or_default(),
                    "is_space": room.as_ref().map(|r| r.is_space()).unwrap_or(false),
                    "membership": "unknown",
                    "reason": "not_checked",
                })
            },
        )
        .await;
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

async fn moderation_plan_row(
    client: &matrix_sdk::Client,
    room_id: String,
    uid: &UserId,
    own_id: &UserId,
    op: u8,
) -> serde_json::Value {
    let room = RoomId::parse(&room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined);
    let Some(room) = room else {
        return json!({
            "room_id": room_id,
            "name": "",
            "is_space": false,
            "membership": "unknown",
            "reason": "not_joined",
        });
    };
    let name = room
        .cached_display_name()
        .map(|n| n.to_string())
        .unwrap_or_default();
    let own = room.get_member_no_sync(own_id).await.ok().flatten();
    let target = match room.get_member_no_sync(uid).await {
        Ok(Some(member)) => Ok(Some(member)),
        _ => match tokio::time::timeout(MODERATION_PLAN_MEMBER_TIMEOUT, room.get_member(uid))
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
    json!({
        "room_id": room_id,
        "name": name,
        "is_space": room.is_space(),
        "membership": membership,
        "own_level": own_level,
        "target_level": target_level,
        "reason": reason,
    })
}

const CLOSURE_PLAN_CAP: usize = 100;

pub(crate) const CLOSURE_KICK_CAP: usize = 1000;

const CLOSURE_STEP_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(60);

const CLOSURE_MEMBERS_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);

const CLOSURE_STAYING_NAMES: usize = 3;

fn closure_verdict(
    joined: bool,
    own_known: bool,
    members_known: bool,
    can_restrict: bool,
) -> &'static str {
    if !joined {
        return "not_joined";
    }
    if !own_known || !members_known {
        return "unknown";
    }
    if !can_restrict {
        return "no_permission";
    }
    ""
}

fn closure_removes(can_kick: bool, own_level: i64, member_level: i64) -> bool {
    can_kick && member_level < own_level
}

fn closure_outcome(
    join_rule_ok: bool,
    members_read: bool,
    other_failures: usize,
    not_attempted: usize,
) -> &'static str {
    if !join_rule_ok {
        "failed"
    } else if !members_read || other_failures > 0 || not_attempted > 0 {
        "partial"
    } else {
        "closed"
    }
}

async fn closure_members(
    room: &matrix_sdk::Room,
    timeout: std::time::Duration,
) -> Option<Vec<matrix_sdk::room::RoomMember>> {
    let wanted = RoomMemberships::ACTIVE | RoomMemberships::KNOCK;
    if room.are_members_synced() {
        return room.members_no_sync(wanted).await.ok();
    }
    tokio::time::timeout(timeout, room.members(wanted))
        .await
        .ok()
        .and_then(|r| r.ok())
}

async fn closure_parent_row(
    client: &matrix_sdk::Client,
    room_id: String,
    own_id: &UserId,
) -> serde_json::Value {
    let room = RoomId::parse(&room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined);
    let can_edit_children = match &room {
        Some(room) => room
            .get_member_no_sync(own_id)
            .await
            .ok()
            .flatten()
            .map(|m| m.can_send_state(StateEventType::SpaceChild))
            .unwrap_or(false),
        None => false,
    };
    json!({
        "room_id": room_id,
        "name": room
            .as_ref()
            .and_then(|r| r.cached_display_name())
            .map(|n| n.to_string())
            .unwrap_or_default(),
        "is_space": room.as_ref().map(|r| r.is_space()).unwrap_or(false),
        "reason": "parent",
        "can_edit_children": can_edit_children,
    })
}

async fn closure_plan_row(
    client: &matrix_sdk::Client,
    room_id: String,
    own_id: &UserId,
) -> serde_json::Value {
    let room = RoomId::parse(&room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| room.state() == RoomState::Joined);
    let Some(room) = room else {
        return json!({
            "room_id": room_id,
            "name": "",
            "is_space": false,
            "reason": closure_verdict(false, false, false, false),
        });
    };
    let name = room
        .cached_display_name()
        .map(|n| n.to_string())
        .unwrap_or_default();
    let own = room.get_member_no_sync(own_id).await.ok().flatten();
    let members = closure_members(&room, MODERATION_PLAN_MEMBER_TIMEOUT).await;
    let can_restrict = own
        .as_ref()
        .map(|m| m.can_send_state(StateEventType::RoomJoinRules))
        .unwrap_or(false);
    let can_kick = own.as_ref().map(|m| m.can_kick()).unwrap_or(false);
    let own_level = own.as_ref().map(|m| power_level_int(m.power_level())).unwrap_or(0);
    let mut removable = 0usize;
    let mut staying = 0usize;
    let mut staying_names: Vec<String> = Vec::new();
    for member in members.iter().flatten() {
        if member.user_id() == own_id {
            continue;
        }
        if closure_removes(can_kick, own_level, power_level_int(member.power_level())) {
            removable += 1;
        } else {
            staying += 1;
            if staying_names.len() < CLOSURE_STAYING_NAMES {
                staying_names.push(
                    member
                        .display_name()
                        .map(str::to_owned)
                        .unwrap_or_else(|| member.user_id().to_string()),
                );
            }
        }
    }
    let can_edit_children = own
        .as_ref()
        .map(|m| m.can_send_state(StateEventType::SpaceChild))
        .unwrap_or(false);
    let world_readable = matches!(
        room.history_visibility(),
        Some(matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility::WorldReadable)
    );
    json!({
        "room_id": room_id,
        "name": name,
        "is_space": room.is_space(),
        "reason": closure_verdict(true, own.is_some(), members.is_some(), can_restrict),
        "join_rule": join_rule_str(room.join_rule().as_ref()),
        "can_kick": can_kick,
        "can_edit_children": can_edit_children,
        "world_readable": world_readable,
        "removable": removable,
        "staying": staying,
        "staying_names": staying_names,
    })
}

pub(crate) fn closure_plan(
    bridge: &RustClient,
    room_ids_json: String,
    parent_ids_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
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
    let truncated = room_ids.len() > CLOSURE_PLAN_CAP;
    room_ids.truncate(CLOSURE_PLAN_CAP);
    let parents: Vec<String> = serde_json::from_str::<Vec<String>>(&parent_ids_json)
        .map_err(|_| "invalid parent list".to_owned())?
        .into_iter()
        .filter(|id| !room_ids.contains(id))
        .take(CLOSURE_PLAN_CAP)
        .collect();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let deadline = tokio::time::Instant::now() + MODERATION_PLAN_BUDGET;
        let mut parent_rows: Vec<serde_json::Value> = Vec::new();
        for parent_id in parents {
            parent_rows.push(closure_parent_row(&client, parent_id, &own_id).await);
        }
        let mut rows = assess_within_budget(
            room_ids,
            MODERATION_PLAN_CONCURRENCY,
            deadline,
            |room_id: String| {
                let client = client.clone();
                let own_id = own_id.clone();
                async move { closure_plan_row(&client, room_id, &own_id).await }
            },
            |room_id: &String| {
                let room = RoomId::parse(room_id).ok().and_then(|id| client.get_room(&id));
                json!({
                    "room_id": room_id,
                    "name": room
                        .as_ref()
                        .and_then(|r| r.cached_display_name())
                        .map(|n| n.to_string())
                        .unwrap_or_default(),
                    "is_space": room.as_ref().map(|r| r.is_space()).unwrap_or(false),
                    "reason": "not_checked",
                })
            },
        )
        .await;
        rows.extend(parent_rows);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_closure_plan",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "truncated": truncated,
            "rooms": rows,
        }));
    });
    Ok(())
}

pub(crate) fn close_room(
    bridge: &RustClient,
    room_id: String,
    reason: String,
    leave: bool,
    unlist_from_json: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let own_id = client
        .user_id()
        .map(|u| u.to_owned())
        .ok_or_else(|| "no active Matrix session".to_owned())?;
    let unlist_from: Vec<String> = serde_json::from_str(&unlist_from_json)
        .map_err(|_| "invalid space list".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        use matrix_sdk::ruma::room::JoinRule;
        let finish = |outcome: serde_json::Value| {
            if timelines.lifecycle_current(lifecycle) {
                let mut outcome = outcome;
                outcome["type"] = json!("room_closure_result");
                outcome["op_id"] = json!(op_id);
                outcome["lifecycle"] = json!(lifecycle);
                outcome["room_id"] = json!(room_id.clone());
                enqueue(&events, outcome);
            }
        };
        let progress = |done: usize, total: usize| {
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, json!({
                    "type": "room_closure_progress",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "room_id": room_id.clone(),
                    "done": done,
                    "total": total,
                }));
            }
        };
        let own = room.get_member_no_sync(&own_id).await.ok().flatten();
        let Some(own) = own else {
            finish(json!({ "outcome": "failed", "join_rule": "failed", "category": "unknown" }));
            return;
        };
        if !own.can_send_state(StateEventType::RoomJoinRules) {
            finish(json!({ "outcome": "failed", "join_rule": "failed", "category": "forbidden" }));
            return;
        }

        let join_rule = if matches!(room.join_rule(), Some(JoinRule::Invite)) {
            "already"
        } else {
            match tokio::time::timeout(
                CLOSURE_STEP_TIMEOUT,
                room.privacy_settings().update_join_rule(JoinRule::Invite),
            )
            .await
            {
                Ok(Ok(())) => "set",
                Ok(Err(err)) => {
                    finish(json!({
                        "outcome": "failed",
                        "join_rule": "failed",
                        "category": classify_room_error(&err.to_string()),
                    }));
                    return;
                }
                Err(_) => {
                    // Timed out: the change may still have landed.
                    finish(json!({
                        "outcome": "failed", "join_rule": "unknown", "category": "unknown",
                    }));
                    return;
                }
            }
        };

        progress(0, 0);

        let directory = match tokio::time::timeout(
            CLOSURE_STEP_TIMEOUT,
            room.privacy_settings().get_room_visibility(),
        )
        .await
        {
            Ok(Ok(Visibility::Public)) => match tokio::time::timeout(
                CLOSURE_STEP_TIMEOUT,
                room.privacy_settings().update_room_visibility(Visibility::Private),
            )
            .await
            {
                Ok(Ok(())) => "unlisted",
                _ => "failed",
            },
            Ok(Ok(_)) => "not_listed",
            _ => "unknown",
        };

        progress(0, 0);

        let mut unlisted = 0usize;
        let mut unlist_failed = 0usize;
        for space_id in &unlist_from {
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            match tokio::time::timeout(
                CLOSURE_STEP_TIMEOUT,
                remove_space_child(&client, space_id, &room_id),
            )
            .await
            {
                Ok(Ok(())) => unlisted += 1,
                _ => unlist_failed += 1,
            }
            progress(0, 0);
        }

        let can_kick = own.can_kick();
        let own_level = power_level_int(own.power_level());
        let members = closure_members(&room, CLOSURE_MEMBERS_TIMEOUT).await;
        let members_read = members.is_some();
        let mut targets: Vec<OwnedUserId> = Vec::new();
        let mut staying = 0usize;
        for member in members.iter().flatten() {
            if member.user_id() == &*own_id {
                continue;
            }
            if closure_removes(can_kick, own_level, power_level_int(member.power_level())) {
                targets.push(member.user_id().to_owned());
            } else {
                staying += 1;
            }
        }
        let not_attempted = targets.len().saturating_sub(CLOSURE_KICK_CAP);
        targets.truncate(CLOSURE_KICK_CAP);
        let total = targets.len();
        let reason_opt = (!reason.is_empty()).then_some(reason.as_str());
        let mut removed = 0usize;
        let mut remove_failed = 0usize;
        for (index, user) in targets.iter().enumerate() {
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            match tokio::time::timeout(CLOSURE_STEP_TIMEOUT, room.kick_user(user, reason_opt))
                .await
            {
                Ok(Ok(())) => removed += 1,
                _ => remove_failed += 1,
            }
            progress(index + 1, total);
        }

        let other_failures = remove_failed
            + unlist_failed
            + usize::from(directory == "failed" || directory == "unknown");
        let outcome = closure_outcome(true, members_read, other_failures, not_attempted);
        let mut left = false;
        let mut category = "";
        if leave && outcome == "closed" {
            match tokio::time::timeout(CLOSURE_STEP_TIMEOUT, room.leave()).await {
                Ok(Ok(())) => left = true,
                Ok(Err(err)) => category = classify_room_error(&err.to_string()),
                Err(_) => category = "network",
            }
        }
        finish(json!({
            "outcome": outcome,
            "join_rule": join_rule,
            "directory": directory,
            "unlisted": unlisted,
            "unlist_failed": unlist_failed,
            "removed": removed,
            "remove_failed": remove_failed,
            "not_attempted": not_attempted,
            "staying": staying,
            "members_read": members_read,
            "can_kick": can_kick,
            "left": left,
            "category": category,
        }));
        if left {
            crate::enqueue_rooms(&events, &client).await;
        }
    });
    Ok(())
}

pub(crate) fn join_rule_str(rule: Option<&matrix_sdk::ruma::room::JoinRule>) -> &'static str {
    use matrix_sdk::ruma::room::JoinRule;
    match rule {
        Some(JoinRule::Invite) => "invite",
        Some(JoinRule::Public) => "public",
        Some(JoinRule::Knock) => "knock",
        Some(JoinRule::Private) => "private",
        Some(JoinRule::Restricted(_)) => "restricted",
        Some(JoinRule::KnockRestricted(_)) => "knock_restricted",
        _ => "",
    }
}

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
    let target_level =
        Int::try_from(level).map_err(|_| "power level out of range".to_owned())?;

    let mut changes = RoomPowerLevelChanges::new();
    let mut event_type: Option<StateEventType> = None;
    let notification_level = key == "notifications.room";
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
        "m.room.history_visibility" => event_type = Some(StateEventType::RoomHistoryVisibility),
        "m.room.guest_access" => event_type = Some(StateEventType::RoomGuestAccess),
        "notifications.room" => {},
        _ => return Err("unsupported power level key".to_owned()),
    }

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let key_for_event = key.clone();
    bridge.spawn_room_action(async move {
        let result = async {
            if notification_level {
                let mut levels = room.power_levels().await
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                levels.notifications.room = target_level;
                let content = RoomPowerLevelsEventContent::try_from(levels)
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned())?;
                room.send_state_event(content).await.map(|_| ())
                    .map_err(|err| classify_room_error(&err.to_string()).to_owned())
            } else {
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
            let alt_aliases = room.alt_aliases();
            if let Some(alias) = parsed.as_deref() {
                let already_here = match client.resolve_room_alias(alias).await {
                    Ok(response) => response.room_id.as_str() == room_id_for_task,
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
    let content = crate::timeline::outgoing_content(&body, &spec, mention_user_ids);
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

pub(crate) fn leavable_room(
    client: &matrix_sdk::Client,
    room_id: &str,
) -> Result<matrix_sdk::Room, String> {
    RoomId::parse(room_id)
        .ok()
        .and_then(|id| client.get_room(&id))
        .filter(|room| {
            matches!(room.state(), RoomState::Joined | RoomState::Invited | RoomState::Knocked)
        })
        .ok_or_else(|| "unknown or already-left room".to_owned())
}

pub(crate) fn leave_room(
    bridge: &RustClient,
    room_id: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = leavable_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = crate::invites::leave_or_retire(&room, || {
            crate::invites::fetch_server_invites(&client)
        })
        .await;
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
                .map(|err| classify_room_error(&err))
                .unwrap_or(""),
        }));
        crate::enqueue_rooms(&events, &client).await;
    });
    Ok(())
}

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

pub(crate) struct PosterBytes {
    pub data: Vec<u8>,
    pub width: u64,
    pub height: u64,
}

const MAX_POSTER_BYTES: usize = 2 * 1024 * 1024;

impl PosterBytes {
    pub(crate) fn into_thumbnail(self) -> Option<Thumbnail> {
        if self.data.is_empty() || self.data.len() > MAX_POSTER_BYTES {
            return None;
        }
        if self.width == 0 || self.height == 0 {
            return None;
        }
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

pub(crate) fn send_voice_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    duration_ms: u64,
    waveform: Vec<u8>,
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        in_reply_to,
        op_id,
    )
}

pub(crate) fn send_thread_voice_path(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    path: String,
    mime: String,
    duration_ms: u64,
    waveform: Vec<u8>,
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        None,
        in_reply_to,
        op_id,
    )
}

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
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        in_reply_to,
        op_id,
    )
}

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
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        in_reply_to,
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
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        in_reply_to,
        op_id,
    )
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_attachment_bytes(
    bridge: &RustClient,
    room_id: String,
    bytes: Vec<u8>,
    filename: String,
    mime: String,
    width: u64,
    height: u64,
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("clipboard data is not a supported image".to_owned());
    }
    let size = bytes.len() as u64;
    let info = attachment_info(&mime, width, height, size, 0);
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::Data { bytes, filename },
        mime,
        None,
        info,
        None,
        in_reply_to,
        op_id,
    )
}

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
                "ok": result.is_ok(),
                "category": if result.is_ok() { "" } else { "rejected" },
            }),
        );
    });
    Ok(())
}

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
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
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
        in_reply_to,
        op_id,
    )
}

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
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
    op_id: u64,
) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    if mime.starts_with("image/") && sniff_image_mime(&bytes).is_none() {
        return Err("clipboard data is not a supported image".to_owned());
    }
    let size = bytes.len() as u64;
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
        in_reply_to,
        op_id,
    )
}

pub(crate) fn media_timeout_secs(class: u32) -> u64 {
    match class {
        2 => 270,
        1 => 90,
        _ => 40,
    }
}

pub(crate) fn media_size_cap(timeout_class: u32) -> u64 {
    match timeout_class {
        2 => 2 * 1024 * 1024 * 1024,
        _ => 512 * 1024 * 1024,
    }
}

pub(crate) const MOTION_PROBE_HEIGHT: u64 = 1;
pub(crate) const MOTION_PROBE_MAX_BYTES: u64 = 8 * 1024 * 1024;

pub(crate) fn mxc_fetch_cap(width: u64, height: u64) -> u64 {
    if width == 0 && height == MOTION_PROBE_HEIGHT {
        MOTION_PROBE_MAX_BYTES
    } else {
        media_size_cap(0)
    }
}

pub(crate) const MEDIA_STORE_MAX_FILE_BYTES: u64 = 100 * 1024 * 1024;

pub(crate) const MEDIA_STORE_ENCODED_MAX_BYTES: u64 =
    MEDIA_STORE_MAX_FILE_BYTES / 2 * 3 + 1024 * 1024;

pub(crate) const MEDIA_STORE_MAX_CACHE_BYTES: u64 = 1024 * 1024 * 1024;

pub(crate) fn media_retention_policy() -> matrix_sdk::media::MediaRetentionPolicy {
    matrix_sdk::media::MediaRetentionPolicy::new()
        .with_max_cache_size(Some(MEDIA_STORE_MAX_CACHE_BYTES))
        .with_max_file_size(Some(MEDIA_STORE_ENCODED_MAX_BYTES))
}

static KEEP_MEDIA: AtomicBool = AtomicBool::new(true);

pub(crate) fn set_keep_media(enabled: bool) {
    KEEP_MEDIA.store(enabled, Ordering::SeqCst);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct MediaPersistence {
    pub sdk_store: bool,
    pub kept_file: bool,
}

pub(crate) fn media_persistence(
    source_encrypted: bool,
    room_encrypted: bool,
    kind: u32,
    declared_size: Option<u64>,
    keep: bool,
    store: crate::mediastore::Active,
) -> MediaPersistence {
    if !keep || !store.persists() || (source_encrypted && !store.admits_encrypted()) {
        return MediaPersistence { sdk_store: false, kept_file: false };
    }
    let over_store_cap = declared_size.map_or(true, |s| s > MEDIA_STORE_MAX_FILE_BYTES);
    MediaPersistence {
        sdk_store: kind != 0 || declared_size.map_or(true, |s| s <= MEDIA_STORE_MAX_FILE_BYTES),
        kept_file: !source_encrypted && !room_encrypted && kind == 0 && over_store_cap,
    }
}

fn media_may_be_from_encrypted_room(
    client: &matrix_sdk::Client,
    timelines: &crate::timeline::TimelineRegistry,
    key: &str,
) -> bool {
    if timelines.media_from_encrypted_room(key) {
        return true;
    }
    let Some(room_id) = timelines.active_room_id() else {
        return true;
    };
    let Ok(room_id) = RoomId::parse(&room_id) else {
        return true;
    };
    client.get_room(&room_id).map_or(true, |room| {
        let state = room.encryption_state();
        state.is_encrypted() || state.is_unknown()
    })
}

pub(crate) async fn clear_kept_media(
    client: &matrix_sdk::Client,
    kept_dir: Option<std::path::PathBuf>,
) -> Option<(u64, u64)> {
    let files = match kept_dir {
        Some(dir) => tokio::task::spawn_blocking(move || crate::mediafiles::clear(&dir))
            .await
            .ok()
            .and_then(|result| result.ok()),
        None => Some((0, 0)),
    };
    let media = client.media();
    let empty = matrix_sdk::media::MediaRetentionPolicy::new()
        .with_max_cache_size(Some(0))
        .with_max_file_size(Some(0));
    let cleaned = media.set_media_retention_policy(empty).await.is_ok()
        && media.clean().await.is_ok();
    let restored = media
        .set_media_retention_policy(media_retention_policy())
        .await
        .is_ok();
    if cleaned && restored {
        files
    } else {
        None
    }
}

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
    if crate::imagesend::svg_thumbnail_unavailable(
        kind, has_embedded_thumbnail, mimetype.as_deref(),
    ) {
        emit_media_failed(
            &terminal, &results, op_id, lifecycle, &key, kind, "rejected",
        );
        return Ok(());
    }
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
    // `get_media_content` stores the DECRYPTED buffer when `use_cache` is set,
    // so encrypted-room media is cached only in the store encrypted with the
    // account's key (mediastore.rs), and never as a kept file
    // (media_persistence). Without that store it is downloaded again each
    // session; MediaBridge's RAM cache serves repeats within one.
    //
    // An unencrypted payload the SDK store refuses by size is kept as a file
    // instead (mediafiles.rs), or a video is downloaded again every session.
    let source_is_encrypted = matches!(&source, MediaSource::Encrypted(_));
    let persistence = media_persistence(
        source_is_encrypted,
        media_may_be_from_encrypted_room(&client, &bridge.timelines, &key),
        kind,
        declared_size,
        KEEP_MEDIA.load(Ordering::SeqCst),
        crate::mediastore::active(&bridge.store_path),
    );
    let use_cache = persistence.sdk_store;
    let kept_file = match &source {
        MediaSource::Plain(uri)
            if persistence.kept_file && !bridge.store_path.as_os_str().is_empty() =>
        {
            crate::cachelayout::kept_files_dir(&bridge.store_path)
                .map(|dir| (dir, uri.to_string()))
        }
        _ => None,
    };
    if kind == 2 && !has_embedded_thumbnail
        && matches!(&source, MediaSource::Encrypted(_))
    {
        emit_media_failed(
            &terminal, &results, op_id, lifecycle, &key, kind, "unavailable",
        );
        return Ok(());
    }
    let aborts = Arc::clone(&bridge.media_fetch_aborts);
    let health_identity = match &source {
        MediaSource::Plain(uri) => crate::mediahealth::identify(uri),
        MediaSource::Encrypted(file) => crate::mediahealth::identify(&file.url),
    };
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
        let kept = match kept_file.clone() {
            Some((dir, uri)) => tokio::task::spawn_blocking(move || {
                crate::mediafiles::read(&dir, &uri, cap)
            })
            .await
            .ok()
            .flatten(),
            None => None,
        };
        let from_kept_file = kept.is_some();
        let outcome = match kept {
            Some(bytes) => Ok(Ok(bytes)),
            None => {
                let limit = std::time::Duration::from_secs(media_timeout_secs(timeout_class));
                tokio::time::timeout(
                    limit,
                    crate::mediafetch::get_media_content_shared(
                        &client, &request, use_cache, limit,
                    ),
                )
                .await
            }
        };
        if let Ok(mut guard) = aborts.lock() {
            guard.remove(&op_id);
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Err(_elapsed) => {
                if !from_kept_file {
                    crate::mediahealth::record_timeout(&health_identity);
                }
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &key, kind,
                    "timeout",
                );
            }
            Ok(Ok(bytes)) => {
                let size = bytes.len() as u64;
                if size > cap {
                    emit_media_failed(
                        &terminal, &results, op_id, lifecycle, &key, kind,
                        "too_large",
                    );
                    return;
                }
                let bytes = match kept_file {
                    Some((dir, uri))
                        if !from_kept_file && size > MEDIA_STORE_MAX_FILE_BYTES =>
                    {
                        let written = tokio::task::spawn_blocking(move || {
                            if crate::mediafiles::write(&dir, &uri, &bytes).is_ok() {
                                let _ = crate::mediafiles::trim(
                                    &dir,
                                    crate::mediafiles::MAX_TOTAL_BYTES,
                                    crate::mediafiles::MAX_AGE,
                                    std::time::SystemTime::now(),
                                );
                            }
                            bytes
                        })
                        .await;
                        match written {
                            Ok(bytes) => bytes,
                            Err(_) => {
                                emit_media_failed(
                                    &terminal, &results, op_id, lifecycle, &key, kind,
                                    "internal",
                                );
                                return;
                            }
                        }
                    }
                    _ => bytes,
                };
                if !timelines.lifecycle_current(lifecycle) {
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
                    &crate::mediafetch::failure_category(&err),
                );
            }
        }
    });
    Ok(())
}

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
    let size_cap = mxc_fetch_cap(width, height);
    let use_cache = crate::mediastore::active(&bridge.store_path).persists();
    let health_identity = crate::mediahealth::identify(&uri);
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
        // Standard class: 40 s, below the C++ 45 s watchdog. Each request is a
        // single attempt of at most 15 s, and a thumbnail that fails on a
        // server fault falls back to ONE download of the file (a plain source
        // only; this function never carries an encrypted one). Without that a
        // homeserver answering 500 to every remote thumbnail was retried by the
        // SDK until this timeout, eight at a time (mediafetch.rs).
        let outcome = tokio::time::timeout(
            std::time::Duration::from_secs(media_timeout_secs(0)),
            crate::mediafetch::get_avatar_bounded(
                &client,
                &request,
                use_cache,
                crate::mediafetch::AVATAR_REQUEST_TIMEOUT,
            ),
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
                crate::mediahealth::record_timeout(&health_identity);
                emit_media_failed(
                    &terminal, &results, op_id, lifecycle, &mxc, 2, "timeout",
                );
            }
            Ok(Ok(bytes)) => {
                let size = bytes.len() as u64;
                if size > size_cap {
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
                    &crate::mediafetch::failure_category(&err),
                );
            }
        }
    });
    Ok(())
}

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

    #[test]
    fn media_timeout_classes_stay_below_watchdog() {
        assert_eq!(media_timeout_secs(0), 40);
        assert_eq!(media_timeout_secs(1), 90);
        assert_eq!(media_timeout_secs(2), 270);
        assert!(media_timeout_secs(0) < 45);
        assert!(media_timeout_secs(1) < 100);
        assert!(media_timeout_secs(2) < 300);
        assert_eq!(media_timeout_secs(99), 40);
    }

    #[test]
    fn a_motion_probe_has_its_own_size_class() {
        assert_eq!(mxc_fetch_cap(0, MOTION_PROBE_HEIGHT), 8 * 1024 * 1024);
        assert_eq!(mxc_fetch_cap(0, 0), media_size_cap(0));
        assert_eq!(mxc_fetch_cap(224, 224), media_size_cap(0));
        assert_eq!(mxc_fetch_cap(1, 1), media_size_cap(0));
    }

    use crate::mediastore::Active;

    const SECURE: Active = Active::Encrypted { admits_encrypted: true };
    const INSECURE: Active = Active::Encrypted { admits_encrypted: false };
    const NOTHING: MediaPersistence = MediaPersistence { sdk_store: false, kept_file: false };

    #[test]
    fn a_large_unencrypted_video_is_kept_as_a_file() {
        let big = Some(MEDIA_STORE_MAX_FILE_BYTES + 1);
        for store in [SECURE, INSECURE, Active::Legacy] {
            assert_eq!(
                media_persistence(false, false, 0, big, true, store),
                MediaPersistence { sdk_store: false, kept_file: true },
            );
            assert_eq!(
                media_persistence(false, false, 0, None, true, store),
                MediaPersistence { sdk_store: true, kept_file: true },
            );
            assert_eq!(
                media_persistence(false, false, 0, Some(MEDIA_STORE_MAX_FILE_BYTES), true, store),
                MediaPersistence { sdk_store: true, kept_file: false },
            );
            assert_eq!(
                media_persistence(false, false, 1, big, true, store),
                MediaPersistence { sdk_store: true, kept_file: false },
            );
        }
    }

    #[test]
    fn encrypted_media_is_kept_only_in_the_encrypted_store() {
        let big = Some(MEDIA_STORE_MAX_FILE_BYTES + 1);
        assert_eq!(
            media_persistence(true, true, 0, Some(10), true, SECURE),
            MediaPersistence { sdk_store: true, kept_file: false },
        );
        assert_eq!(
            media_persistence(true, true, 1, None, true, SECURE),
            MediaPersistence { sdk_store: true, kept_file: false },
        );
        assert_eq!(media_persistence(true, true, 0, big, true, SECURE), NOTHING);
        assert!(!media_persistence(true, true, 0, None, true, SECURE).kept_file);
    }

    #[test]
    fn encrypted_media_is_never_kept_anywhere_else() {
        for store in [INSECURE, Active::Legacy, Active::Memory] {
            for kind in [0u32, 1, 2] {
                for size in [None, Some(10), Some(MEDIA_STORE_MAX_FILE_BYTES + 1)] {
                    assert_eq!(
                        media_persistence(true, true, kind, size, true, store),
                        NOTHING,
                        "{store:?} kind {kind} size {size:?}",
                    );
                }
            }
        }
    }

    #[test]
    fn an_in_memory_store_keeps_nothing() {
        for encrypted in [false, true] {
            for kind in [0u32, 1, 2] {
                for size in [None, Some(10), Some(MEDIA_STORE_MAX_FILE_BYTES + 1)] {
                    assert_eq!(
                        media_persistence(encrypted, encrypted, kind, size, true, Active::Memory),
                        NOTHING,
                    );
                }
            }
        }
    }

    #[test]
    fn a_plain_file_in_an_encrypted_room_is_never_a_kept_file() {
        let big = Some(MEDIA_STORE_MAX_FILE_BYTES + 1);
        for store in [SECURE, INSECURE, Active::Legacy] {
            assert!(!media_persistence(false, true, 0, big, true, store).kept_file);
            assert!(!media_persistence(false, true, 0, None, true, store).kept_file);
            assert!(media_persistence(false, true, 0, Some(10), true, store).sdk_store);
        }
        assert!(media_persistence(false, false, 0, big, true, SECURE).kept_file);
    }

    #[test]
    fn keep_media_off_keeps_nothing() {
        for store in [SECURE, INSECURE, Active::Legacy] {
            for encrypted in [false, true] {
                for kind in [0u32, 1, 2] {
                    for size in [None, Some(10), Some(MEDIA_STORE_MAX_FILE_BYTES + 1)] {
                        assert_eq!(
                            media_persistence(encrypted, encrypted, kind, size, false, store),
                            NOTHING,
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn the_store_policy_keeps_its_file_cap() {
        let policy = media_retention_policy();
        assert_eq!(policy.max_file_size, Some(MEDIA_STORE_ENCODED_MAX_BYTES));
        assert!(MEDIA_STORE_ENCODED_MAX_BYTES > MEDIA_STORE_MAX_FILE_BYTES / 2 * 3);
        assert!(MEDIA_STORE_MAX_FILE_BYTES >= 100 * 1024 * 1024);
        assert_eq!(policy.max_cache_size, Some(MEDIA_STORE_MAX_CACHE_BYTES));
        assert_eq!(policy.computed_max_file_size(), Some(MEDIA_STORE_ENCODED_MAX_BYTES));
        let defaults = matrix_sdk::media::MediaRetentionPolicy::new();
        assert_eq!(policy.last_access_expiry, defaults.last_access_expiry);
    }

    #[test]
    fn media_size_caps_by_class() {
        assert_eq!(media_size_cap(0), 512 * 1024 * 1024);
        assert_eq!(media_size_cap(1), 512 * 1024 * 1024);
        assert_eq!(media_size_cap(2), 2 * 1024 * 1024 * 1024);
    }

    #[test]
    fn a_timed_attachment_carries_its_duration() {
        match attachment_info("audio/mpeg", 0, 0, 4096, 185_000) {
            Some(AttachmentInfo::Audio(info)) => {
                assert_eq!(
                    info.duration,
                    Some(std::time::Duration::from_millis(185_000)),
                    "an audio attachment lost the duration the sender decoded"
                );
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
        match attachment_info("image/png", 2, 2, 10, 999) {
            Some(AttachmentInfo::Image(_)) => {}
            other => panic!("expected image info, got {other:?}"),
        }
    }

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
        assert!(attachment_info("image/png", 0, 0, 5, 0).is_none());
        match attachment_info("image/gif", 100, 100, 5, 0) {
            Some(AttachmentInfo::Image(info)) => {
                assert_eq!(info.is_animated, Some(true));
            }
            other => panic!("expected image info, got {other:?}"),
        }
    }

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

    #[test]
    fn poster_is_validated_by_magic_not_by_claim() {
        let thumb = poster(jpeg_bytes(64), 320, 180)
            .into_thumbnail()
            .expect("a real JPEG is accepted");
        assert_eq!(thumb.content_type.to_string(), "image/jpeg");
        assert_eq!(thumb.width, UInt::new(320).unwrap());
        assert_eq!(thumb.height, UInt::new(180).unwrap());
        assert_eq!(thumb.size, UInt::new(64).unwrap());

        assert!(poster(b"<svg xmlns=\"http://www.w3.org/2000/svg\"/>".to_vec(),
                       320, 180)
            .into_thumbnail()
            .is_none());
        assert!(poster(b"not an image at all".to_vec(), 320, 180)
            .into_thumbnail()
            .is_none());
        assert!(poster(vec![0xFF, 0xD8], 320, 180).into_thumbnail().is_none());
    }

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

    #[test]
    fn an_invitation_marked_is_direct_is_a_direct_message() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;

        let dm = room_id!("!dm-invite:example.org");
        let plain = room_id!("!room-invite:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            let me = client.user_id().unwrap().to_string();
            let mut response = v5::Response::new("pos-1".to_owned());
            for (room, is_direct) in [(dm, Some(true)), (plain, None)] {
                let mut content = json!({ "membership": "invite" });
                if let Some(flag) = is_direct {
                    content["is_direct"] = json!(flag);
                }
                let mut entry = v5::response::Room::default();
                entry.invite_state = Some(vec![Raw::from_json_string(
                    json!({
                        "type": "m.room.member", "state_key": me,
                        "sender": "@inviter:example.org",
                        "content": content
                    })
                    .to_string(),
                )
                .unwrap()]);
                response.rooms.insert(room.to_owned(), entry);
            }
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });
        let dm_room = client.get_room(dm).unwrap();
        let plain_room = client.get_room(plain).unwrap();
        assert_eq!(dm_room.state(), RoomState::Invited);
        assert!(dm_room.direct_targets().is_empty(), "no m.direct before join");
        assert!(rt.block_on(payload_is_direct(&dm_room, false)));
        assert!(!rt.block_on(payload_is_direct(&plain_room, false)));
        assert!(rt.block_on(payload_is_direct(&plain_room, true)));
        drop(client);
        drop(server);
        drop(rt);
    }

    #[test]
    fn leave_room_rejects_a_pending_invitation() {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id};
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use matrix_sdk_base::RequestedRequiredStates;

        let room = room_id!("!invited:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            server.mock_room_leave().ok(room).expect(1).mount().await;
            server.mock_room_forget().ok().mount().await;
            let me = client.user_id().unwrap().to_string();
            let mut response = v5::Response::new("pos-1".to_owned());
            let mut entry = v5::response::Room::default();
            entry.invite_state = Some(vec![Raw::from_json_string(
                json!({
                    "type": "m.room.member", "state_key": me,
                    "sender": "@inviter:example.org",
                    "content": { "membership": "invite" }
                })
                .to_string(),
            )
            .unwrap()]);
            response.rooms.insert(room.to_owned(), entry);
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });
        assert_eq!(client.get_room(room).unwrap().state(), RoomState::Invited);

        let dir = std::env::temp_dir()
            .join(format!("lightning-leave-invite-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        leave_room(&bridge, room.to_string(), 41).expect("an invitation can be left");
        let started = std::time::Instant::now();
        let result = loop {
            let found = bridge.events.lock().unwrap().iter().find_map(|raw| {
                let value: serde_json::Value = serde_json::from_str(raw).ok()?;
                (value["type"] == "room_leave_result").then_some(value)
            });
            if let Some(value) = found {
                break value;
            }
            assert!(started.elapsed() < std::time::Duration::from_secs(20), "no answer");
            std::thread::sleep(std::time::Duration::from_millis(20));
        };
        assert_eq!(result["op_id"], json!(41), "{result}");
        assert_eq!(result["ok"], json!(true), "{result}");
        assert!(!matches!(
            client.get_room(room).map(|r| r.state()),
            Some(RoomState::Invited)
        ));
        drop(bridge);
        drop(server);
        drop(rt);
    }

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
        assert_eq!(only_title["preview_route"], "server");
        let only_desc = f(&json!({ "og:description": "Some page" }))
            .expect("a description alone is a usable preview");
        assert_eq!(only_desc["description"], "Some page");
        let with_image = f(&json!({
            "og:title": "T",
            "og:image": "mxc://example.org/abc",
            "matrix:image:size": 4096
        }))
        .expect("usable");
        assert_eq!(with_image["image_source"], "mxc://example.org/abc");
        assert_eq!(with_image["image_size"], 4096);
        assert_eq!(with_image["preview_kind"], "metadata");
    }

    #[test]
    fn a_server_answer_for_a_direct_image_is_media_not_a_fallback() {
        use super::server_preview_fields as f;
        let answer = f(&json!({
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/png",
            "og:image:width": 640,
            "og:image:height": 480,
            "matrix:image:size": 9000
        }))
        .expect("the server's image must be used");
        assert_eq!(answer["preview_kind"], "direct_media");
        assert_eq!(answer["preview_route"], "server");
        assert_eq!(answer["image_source"], "mxc://example.org/img");
        assert_eq!(answer["image_mime"], "image/png");
        assert_eq!(answer["image_width"], 640);
        let named = f(&json!({
            "og:description": "photo.jpg",
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/jpeg"
        }))
        .expect("usable");
        assert_eq!(named["preview_kind"], "direct_media");
        assert!(f(&json!({ "og:image": "mxc://example.org/s",
                            "og:image:type": "image/svg+xml" })).is_none());
        assert!(f(&json!({ "og:image": "https://example.org/a.png",
                            "og:image:type": "image/png" })).is_none());
        let spaced = f(&json!({
            "og:description": "my holiday photo.JPG",
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/jpeg"
        }))
        .expect("usable");
        assert_eq!(spaced["preview_kind"], "direct_media");
        let sentence = f(&json!({
            "og:description": "A few words about the page.",
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/jpeg"
        }))
        .expect("usable");
        assert_eq!(sentence["preview_kind"], "metadata");
        let page = f(&json!({
            "og:description": "Some words from the page body.",
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/png"
        }))
        .expect("usable");
        assert_eq!(page["preview_kind"], "metadata");
        let site = f(&json!({
            "og:site_name": "Example",
            "og:description": "x.png",
            "og:image": "mxc://example.org/img",
            "og:image:type": "image/png"
        }))
        .expect("usable");
        assert_eq!(site["preview_kind"], "metadata");
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

        assert!(!public_ip("::ffff:127.0.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:10.0.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:192.168.1.1".parse().unwrap()));
        assert!(!public_ip("::ffff:169.254.169.254".parse().unwrap()));
        assert!(!public_ip("::ffff:100.64.0.1".parse().unwrap()));
        assert!(!public_ip("::ffff:0.0.0.0".parse().unwrap()));
        assert!(!public_ip("64:ff9b::7f00:1".parse().unwrap()));
        assert!(!public_ip("::127.0.0.1".parse().unwrap()));
        assert!(public_ip("::ffff:93.184.216.34".parse().unwrap()));
        assert!(!public_ip("2002:7f00:1::1".parse().unwrap()));
        assert!(!public_ip("2002:c0a8:101::1".parse().unwrap()));
        assert!(!public_ip("2001:0:4136:e378:8000:63bf:3fff:fdd2".parse().unwrap()));
        assert!(!public_ip("fec0::1".parse().unwrap()));
        assert!(!public_ip("feff::1".parse().unwrap()));
        assert!(public_ip("2001:4860:4860::8888".parse().unwrap()));
        assert!(public_ip("2003:e0::1".parse().unwrap()));
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
        assert_eq!(classify_preview_payload("application/octet-stream", &gif),
                   Ok(Some("image/gif")));
        assert_eq!(classify_preview_payload("text/html", &gif), Ok(Some("image/gif")));
        assert_eq!(classify_preview_payload("text/html", b"<html><title>Giphy</title></html>"),
                   Ok(None));
        assert_eq!(classify_preview_payload("image/gif", b"<html>not gif</html>"),
                   Err("invalid_image"));
        assert_eq!(classify_preview_payload("image/svg+xml", b"<svg></svg>"),
                   Err("unsupported_mime"));
    }

    #[test]
    fn preview_webp_dimensions_cover_all_three_chunk_types() {
        let mut vp8x = b"RIFF\x00\x00\x00\x00WEBPVP8X".to_vec();
        vp8x.extend_from_slice(&0u32.to_le_bytes());
        vp8x.extend_from_slice(&[0u8; 4]);
        vp8x.extend_from_slice(&[99, 0, 0]);
        vp8x.extend_from_slice(&[49, 0, 0]);
        assert_eq!(image_dimensions("image/webp", &vp8x), Some((100, 50)));

        let bits: u32 = (99) | ((49) << 14);
        let mut vp8l = b"RIFF\x00\x00\x00\x00WEBPVP8L".to_vec();
        vp8l.extend_from_slice(&0u32.to_le_bytes());
        vp8l.push(0x2f);
        vp8l.extend_from_slice(&bits.to_le_bytes());
        assert_eq!(image_dimensions("image/webp", &vp8l), Some((100, 50)));

        let mut vp8 = b"RIFF\x00\x00\x00\x00WEBPVP8 ".to_vec();
        vp8.extend_from_slice(&0u32.to_le_bytes());
        vp8.extend_from_slice(&[0, 0, 0]);
        vp8.extend_from_slice(&[0x9d, 0x01, 0x2a]);
        vp8.extend_from_slice(&100u16.to_le_bytes());
        vp8.extend_from_slice(&50u16.to_le_bytes());
        assert_eq!(image_dimensions("image/webp", &vp8), Some((100, 50)));

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
        assert_eq!(MAX_INITIAL_FETCH_BYTES, MAX_IMAGE_BYTES);
        assert!(MAX_INITIAL_FETCH_BYTES >= MAX_HTML_BYTES);
    }

    #[test]
    fn preview_image_fields_rejects_unsupported_mime_like_svg() {
        assert!(image_fields("image/svg+xml".to_owned(), b"<svg></svg>".to_vec()).is_err());
    }

    fn orphan_probe() -> DmOrphanProbe {
        DmOrphanProbe {
            joined: true,
            joined_members: 1,
            invited_members: 0,
            has_name: false,
            is_space: false,
            known_before_request: false,
            created_by_us: true,
            marked_direct: false,
            encrypted: true,
            join_rule_invite: true,
            created_in_window: true,
            returned_by_a_create: false,
        }
    }

    #[test]
    fn a_failed_dm_leaves_an_orphan_that_is_recognised() {
        assert!(is_failed_dm_orphan(&orphan_probe()));
    }

    #[test]
    fn a_delivered_invite_is_a_working_dm_not_an_orphan() {
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { invited_members: 1, ..orphan_probe() }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { joined_members: 2, ..orphan_probe() }));
    }

    #[test]
    fn nothing_the_user_meant_to_keep_is_an_orphan() {
        let p = orphan_probe();
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { known_before_request: true, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { created_by_us: false, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { joined: false, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { has_name: true, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { is_space: true, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { marked_direct: true, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { invited_members: 2, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { encrypted: false, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { join_rule_invite: false, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { created_in_window: false, ..p }));
        assert!(!is_failed_dm_orphan(&DmOrphanProbe { returned_by_a_create: true, ..p }));
    }

    #[test]
    fn creation_time_is_inside_this_requests_window() {
        let (start, end) = (1_000_000, 1_050_000);
        assert!(created_in_window(Some(start), start, end));
        assert!(created_in_window(Some(end), start, end));
        assert!(created_in_window(Some(start - CREATE_CLOCK_SKEW_MS), start, end));
        assert!(created_in_window(Some(end + CREATE_CLOCK_SKEW_MS), start, end));
        assert!(!created_in_window(Some(start - CREATE_CLOCK_SKEW_MS - 1), start, end));
        assert!(!created_in_window(Some(end + CREATE_CLOCK_SKEW_MS + 1), start, end));
        assert!(!created_in_window(None, start, end));
        assert!(created_in_window(Some(0), 5, 10));
    }

    #[test]
    fn only_a_matrix_error_body_may_sweep() {
        assert!(!sweep_allowed(None, false, "network"));
        assert!(!sweep_allowed(None, true, "network"));
        assert!(sweep_allowed(Some(500), true, "network"));
        assert!(sweep_allowed(Some(400), true, "network"));
        assert!(!sweep_allowed(Some(500), false, "network"));
        for transient in [408, 499, 502, 503, 504] {
            assert!(!sweep_allowed(Some(transient), true, "network"), "{transient}");
        }
        for c in ["forbidden", "rate_limited", "invalid", "unrecognized", "not_found"] {
            assert!(refused_before_creation(c), "{c}");
            assert!(!sweep_allowed(Some(403), true, c), "{c}");
        }
    }

    #[test]
    fn the_registry_tracks_in_flight_creates_and_returned_rooms() {
        let reg = Arc::new(DmCreateRegistry::default());
        assert_eq!(reg.in_flight(), 0);
        let first = reg.begin();
        let second = reg.begin();
        assert_eq!(reg.in_flight(), 2);
        drop(first);
        assert_eq!(reg.in_flight(), 1);
        let room = RoomId::parse("!made:example.org").unwrap();
        assert!(!reg.returned_by_a_create(&room));
        reg.record_created(&room);
        assert!(reg.returned_by_a_create(&room));
        drop(second);
        assert_eq!(reg.in_flight(), 0);
    }

    #[test]
    fn an_unanswered_create_keeps_blocking_sweeps_for_ten_minutes() {
        let reg = Arc::new(DmCreateRegistry::default());
        let t0 = 5_000_000;
        assert!(!reg.busy(t0));
        reg.record_unanswered_end(t0);
        assert!(reg.busy(t0 + 1));
        assert!(reg.busy(t0 + UNANSWERED_BUSY_MS - 1));
        assert!(!reg.busy(t0 + UNANSWERED_BUSY_MS));
        let g = reg.begin();
        assert!(reg.busy(t0 + UNANSWERED_BUSY_MS * 2));
        drop(g);
        assert!(!reg.busy(t0 + UNANSWERED_BUSY_MS * 2));
        for i in 0..100 {
            reg.record_unanswered_end(i);
        }
        assert!(reg.state.lock().unwrap().unanswered_ends.len() <= 16);
    }

    #[test]
    fn a_matrix_answer_needs_a_standard_body_and_a_non_proxy_status() {
        assert!(matrix_answered(Some(500), true));
        assert!(matrix_answered(Some(403), true));
        assert!(!matrix_answered(None, false));
        assert!(!matrix_answered(Some(500), false));
        assert!(!matrix_answered(Some(502), true));
    }

    #[test]
    fn creates_are_sent_once_with_a_timeout_past_the_ui_guard() {
        let default = format!("{:?}", matrix_sdk::config::RequestConfig::default());
        assert!(!default.contains("retry_limit"), "{default}");
        let ours = format!("{:?}", create_request_config());
        assert!(ours.contains("retry_limit: 0"), "{ours}");
        assert!(CREATE_REQUEST_TIMEOUT > std::time::Duration::from_secs(60));
    }

    #[test]
    fn the_dm_request_is_what_create_dm_builds() {
        let uid = UserId::parse("@peer:example.org").unwrap();
        let req = dm_create_request(&uid, false);
        assert!(req.is_direct);
        assert_eq!(req.invite, vec![uid]);
        assert!(matches!(req.preset, Some(RoomPreset::TrustedPrivateChat)));
        assert_eq!(req.initial_state.len(), 1);
    }

    fn assert_stock_events_kept(events: &serde_json::Map<String, serde_json::Value>) {
        for ev in [crate::rtc::EV_MEMBER_LEGACY, crate::rtc::EV_MEMBER_STICKY] {
            assert_eq!(events.get(ev).and_then(|x| x.as_u64()), Some(0), "{ev}");
        }
        for (ev, level) in [
            ("m.room.power_levels", 100),
            ("m.room.history_visibility", 100),
            ("m.room.server_acl", 100),
            ("m.room.encryption", 100),
            ("m.room.tombstone", 100),
            ("m.room.name", 50),
            ("m.room.canonical_alias", 50),
            ("m.room.avatar", 50),
        ] {
            assert_eq!(events.get(ev).and_then(|x| x.as_u64()), Some(level), "{ev}");
        }
    }

    #[tokio::test]
    async fn a_slow_room_version_probe_is_cut_off_and_counts_as_no() {
        let slow = async {
            tokio::time::sleep(std::time::Duration::from_secs(30)).await;
            true
        };
        let started = std::time::Instant::now();
        assert!(!privileges_creators_within(slow, std::time::Duration::from_millis(50)).await);
        assert!(started.elapsed() < std::time::Duration::from_secs(5));
        let fast = async { true };
        assert!(privileges_creators_within(fast, std::time::Duration::from_secs(1)).await);
    }

    #[test]
    fn the_tombstone_level_follows_the_room_version_rules() {
        let tombstone = |creator_power: bool| {
            call_membership_power_levels(creator_power)["events"]["m.room.tombstone"]
                .as_u64()
        };
        assert_eq!(tombstone(false), Some(100));
        assert_eq!(tombstone(true), Some(150));
        let v11 = matrix_sdk::ruma::RoomVersionId::V11.rules().unwrap();
        let v12 = matrix_sdk::ruma::RoomVersionId::V12.rules().unwrap();
        assert!(!v11.authorization.explicitly_privilege_room_creators);
        assert!(v12.authorization.explicitly_privilege_room_creators);
    }

    #[test]
    fn a_created_dm_lets_the_invitee_publish_call_membership() {
        let uid = UserId::parse("@peer:example.org").unwrap();
        let raw = dm_create_request(&uid, false)
            .power_level_content_override
            .expect("a DM that can host a call needs the override");
        let v: serde_json::Value = raw.deserialize_as_unchecked().unwrap();
        let events = v.get("events").and_then(|e| e.as_object()).unwrap();
        assert_stock_events_kept(events);
        assert!(v.get("state_default").is_none());
    }

    #[test]
    fn classify_room_error_categories() {
        assert_eq!(classify_room_error("M_LIMIT_EXCEEDED: too many requests"), "rate_limited");
        assert_eq!(classify_room_error("M_FORBIDDEN: not allowed"), "forbidden");
        assert_eq!(classify_room_error("M_ROOM_IN_USE: alias taken"), "alias_taken");
        assert_eq!(classify_room_error("M_INVALID_PARAM: bad alias"), "invalid");
        assert_eq!(classify_room_error("M_NOT_FOUND"), "not_found");
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

        let mut jxl_box =
            b"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a\x00\x00\x00\x14ftypjxl ".to_vec();
        jxl_box.extend_from_slice(&[0; 4]);
        assert_eq!(sniff_image_mime(&jxl_box), Some("image/jxl"));

        let mut jxl_stream = vec![0xFF, 0x0A, 0x47, 0x06];
        jxl_stream.extend_from_slice(&[0; 8]);
        assert_eq!(sniff_image_mime(&jxl_stream), Some("image/jxl"));

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
        assert_stock_events_kept(events);
        assert!(v.get("state_default").is_none(),
                "state_default must be left at the server default");
    }

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
        assert_eq!(request.initial_state.len(), 1);
        assert_eq!(request.invite.len(), 1);
        assert!(!request.is_direct);
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
            encrypted: true,
            ..Default::default()
        };
        let request = build_create_room_request(&opts).unwrap();
        assert_eq!(request.name.as_deref(), Some("Team"));
        assert!(request.initial_state.is_empty());
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

    #[test]
    fn moderation_plan_offers_only_actions_that_can_succeed() {
        use matrix_sdk::ruma::events::room::member::MembershipState as M;
        let join = M::Join;
        let invite = M::Invite;
        let ban = M::Ban;
        let leave = M::Leave;

        assert_eq!(moderation_verdict(0, Some(&join), true, 50, 0), "");
        assert_eq!(moderation_verdict(0, Some(&invite), true, 50, 0), "");
        assert_eq!(moderation_verdict(0, Some(&leave), true, 50, 0), "not_a_member");
        assert_eq!(moderation_verdict(0, Some(&ban), true, 50, 0), "already_banned");
        assert_eq!(moderation_verdict(0, None, true, 50, 0), "not_a_member");

        assert_eq!(moderation_verdict(1, Some(&leave), true, 50, 0), "");
        assert_eq!(moderation_verdict(1, Some(&ban), true, 50, 0), "already_banned");

        assert_eq!(moderation_verdict(2, Some(&ban), true, 50, 0), "");
        assert_eq!(moderation_verdict(2, Some(&join), true, 50, 0), "not_banned");

        assert_eq!(moderation_verdict(0, Some(&join), false, 100, 0), "no_permission");

        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 50), "outranked");
        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 100), "outranked");
        assert_eq!(moderation_verdict(1, Some(&join), true, 50, 49), "");
        assert_eq!(moderation_verdict(0, Some(&join), true, 0, -1), "");

        assert_eq!(moderation_verdict(9, Some(&join), true, 100, 0), "invalid");
    }

    #[tokio::test]
    async fn a_plan_stops_waiting_at_its_deadline() {
        let start = tokio::time::Instant::now();
        let deadline = start + std::time::Duration::from_millis(200);
        let rows = assess_within_budget(
            vec![1u32, 2, 3, 4],
            2,
            deadline,
            |n: u32| async move {
                if n == 2 {
                    tokio::time::sleep(std::time::Duration::from_secs(30)).await;
                }
                format!("ok{n}")
            },
            |n: &u32| format!("late{n}"),
        )
        .await;
        assert_eq!(rows, vec!["ok1", "late2", "ok3", "ok4"]);
        assert!(start.elapsed() < std::time::Duration::from_secs(5));
    }

    #[tokio::test]
    async fn a_plan_bounds_how_many_rooms_it_reads_at_once() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let running = Arc::new(AtomicUsize::new(0));
        let peak = Arc::new(AtomicUsize::new(0));
        let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(10);
        let rows = assess_within_budget(
            (0..12u32).collect(),
            3,
            deadline,
            |n: u32| {
                let running = Arc::clone(&running);
                let peak = Arc::clone(&peak);
                async move {
                    let now = running.fetch_add(1, Ordering::SeqCst) + 1;
                    peak.fetch_max(now, Ordering::SeqCst);
                    tokio::time::sleep(std::time::Duration::from_millis(10)).await;
                    running.fetch_sub(1, Ordering::SeqCst);
                    n
                }
            },
            |_| u32::MAX,
        )
        .await;
        assert_eq!(rows, (0..12u32).collect::<Vec<_>>());
        assert!(peak.load(Ordering::SeqCst) <= 3);
        assert!(peak.load(Ordering::SeqCst) >= 2);
    }

    #[test]
    fn closing_needs_the_join_rule_and_a_known_roster() {
        assert_eq!(closure_verdict(true, true, true, true), "");
        assert_eq!(closure_verdict(false, true, true, true), "not_joined");
        assert_eq!(closure_verdict(true, false, true, true), "unknown");
        assert_eq!(closure_verdict(true, true, false, true), "unknown");
        assert_eq!(closure_verdict(true, true, true, false), "no_permission");
    }

    #[test]
    fn closing_removes_only_members_below_the_viewer() {
        assert!(closure_removes(true, 100, 50));
        assert!(closure_removes(true, 100, 0));
        assert!(closure_removes(true, 0, -1));
        assert!(!closure_removes(true, 100, 100));
        assert!(!closure_removes(true, 50, 100));
        assert!(!closure_removes(false, 100, 0));
        assert!(!closure_removes(true, 100, CREATOR_POWER_LEVEL));
        let max_finite: i64 = (1 << 53) - 1;
        assert!(closure_removes(true, CREATOR_POWER_LEVEL, max_finite));
        assert!(!closure_removes(true, max_finite, CREATOR_POWER_LEVEL));
        assert_eq!(CREATOR_POWER_LEVEL as f64 as i64, CREATOR_POWER_LEVEL);
    }

    #[test]
    fn a_close_is_complete_only_when_every_step_succeeded() {
        assert_eq!(closure_outcome(true, true, 0, 0), "closed");
        assert_eq!(closure_outcome(false, true, 0, 0), "failed");
        assert_eq!(closure_outcome(true, false, 0, 0), "partial");
        assert_eq!(closure_outcome(true, true, 1, 0), "partial");
        assert_eq!(closure_outcome(true, true, 0, 3), "partial");
        assert!(CLOSURE_KICK_CAP >= 100);
    }

}

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
    let mut content = crate::timeline::outgoing_content(&body, &spec, mention_user_ids);
    content.relates_to = match (root_id, reply_id) {
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

pub(crate) fn activity_seed_entry(
    value: &serde_json::Value,
    room_id: &str,
    timestamp_ms: u64,
    read: bool,
    own: &str,
) -> Option<serde_json::Value> {
    let event_id = value["event_id"].as_str().unwrap_or_default();
    let sender = value["sender"].as_str().unwrap_or_default();
    if event_id.is_empty() || sender.is_empty() || sender == own {
        return None;
    }
    let event_type = value["type"].as_str().unwrap_or_default();
    let encrypted = event_type == "m.room.encrypted";
    let content = &value["content"];
    let relates = &content["m.relates_to"];
    let shown = if relates["rel_type"] == "m.replace" && content["m.new_content"].is_object() {
        &content["m.new_content"]
    } else {
        content
    };
    let (body, msgtype): (String, &str) = match event_type {
        "m.room.message" => (
            crate::timeline::strip_reply_fallback(shown["body"].as_str().unwrap_or_default())
                .chars()
                .take(200)
                .collect(),
            shown["msgtype"].as_str().unwrap_or_default(),
        ),
        "m.sticker" => (String::new(), "m.sticker"),
        _ => (String::new(), ""),
    };
    let thread_root = if relates["rel_type"] == "m.thread" {
        relates["event_id"].as_str().unwrap_or_default()
    } else {
        ""
    };
    Some(json!({
        "event_id": event_id,
        "room_id": room_id,
        "sender": sender,
        "timestamp_ms": timestamp_ms,
        "read": read,
        "encrypted": encrypted,
        "body": body,
        "msgtype": msgtype,
        "thread_root_id": thread_root,
    }))
}

pub(crate) fn request_activity_seed(bridge: &RustClient, limit: u32) -> Result<(), String> {
    request_activity_seed_page(bridge, limit, None)
}

pub(crate) fn request_activity_seed_page(
    bridge: &RustClient,
    limit: u32,
    from: Option<String>,
) -> Result<(), String> {
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
        request.from = from.clone();
        let answer = client.send(request).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let response = match answer {
            Ok(response) => response,
            Err(err) => {
                enqueue(&events, json!({
                    "type": if from.is_some() { "activity_seed_page" } else { "activity_seed" },
                    "lifecycle": lifecycle, "ok": false, "entries": [],
                    "category": crate::oauth::http_failure_reason(&err),
                }));
                return;
            }
        };
        let next_token = response.next_token.clone().unwrap_or_default();
        let mut entries: Vec<serde_json::Value> = Vec::new();
        for notification in response.notifications {
            let value: serde_json::Value =
                notification.event.deserialize_as().unwrap_or(serde_json::Value::Null);
            if let Some(entry) = activity_seed_entry(
                &value,
                notification.room_id.as_str(),
                u64::from(notification.ts.get()),
                notification.read,
                &own,
            ) {
                entries.push(entry);
            }
        }
        enqueue(&events, json!({
            "type": if from.is_some() { "activity_seed_page" } else { "activity_seed" },
            "lifecycle": lifecycle, "ok": true, "entries": entries,
            "next_token": next_token,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod activity_seed_tests {
    use super::activity_seed_entry;
    use serde_json::json;

    fn seed(content: serde_json::Value, event_type: &str) -> serde_json::Value {
        activity_seed_entry(
            &json!({
                "event_id": "$e:x", "sender": "@bob:x", "type": event_type,
                "content": content,
            }),
            "!r:x",
            5,
            false,
            "@me:x",
        )
        .expect("a row")
    }

    #[test]
    fn a_seeded_reply_shows_the_reply_not_the_quote() {
        let row = seed(
            json!({
                "msgtype": "m.text",
                "body": "> <@alice:x> what Alice said\n> more\n\n@me look at this",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$q:x" } },
            }),
            "m.room.message",
        );
        assert_eq!(row["body"], "@me look at this");
        assert_eq!(row["msgtype"], "m.text");
    }

    #[test]
    fn a_seeded_row_carries_its_msgtype_and_an_edit_its_new_text() {
        let image = seed(json!({ "msgtype": "m.image", "body": "IMG_2041.jpg" }), "m.room.message");
        assert_eq!(image["msgtype"], "m.image");
        let edit = seed(
            json!({
                "msgtype": "m.text", "body": "* hi @me",
                "m.new_content": { "msgtype": "m.text", "body": "hi @me" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$o:x" },
            }),
            "m.room.message",
        );
        assert_eq!(edit["body"], "hi @me");
        let sticker = seed(json!({ "body": "a cat", "url": "mxc://x/y" }), "m.sticker");
        assert_eq!(sticker["msgtype"], "m.sticker");
        assert_eq!(sticker["body"], "");
        let encrypted = seed(json!({ "algorithm": "m.megolm.v1.aes-sha2" }), "m.room.encrypted");
        assert_eq!(encrypted["encrypted"], true);
        assert_eq!(encrypted["msgtype"], "");
    }

    #[test]
    fn a_plain_quote_and_the_thread_root_survive_and_our_own_rows_are_skipped() {
        let quote = seed(json!({ "msgtype": "m.text", "body": "> not a reply" }), "m.room.message");
        assert_eq!(quote["body"], "> not a reply");
        let threaded = seed(
            json!({
                "msgtype": "m.text", "body": "in thread",
                "m.relates_to": { "rel_type": "m.thread", "event_id": "$root:x" },
            }),
            "m.room.message",
        );
        assert_eq!(threaded["thread_root_id"], "$root:x");
        assert!(activity_seed_entry(
            &json!({ "event_id": "$e:x", "sender": "@me:x", "type": "m.room.message",
                     "content": { "body": "mine" } }),
            "!r:x", 5, false, "@me:x",
        )
        .is_none());
    }
}

#[cfg(test)]
mod member_freshness_tests {
    use super::*;
    use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id};
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk_base::RequestedRequiredStates;

    fn raw<T>(value: serde_json::Value) -> Raw<T> {
        Raw::from_json_string(value.to_string()).unwrap()
    }

    fn member(user: &str, membership: &str, name: &str, event_id: &str) -> serde_json::Value {
        json!({
            "type": "m.room.member", "state_key": user, "sender": user,
            "event_id": event_id, "origin_server_ts": 1_700_000_000_000u64,
            "content": { "membership": membership, "displayname": name },
        })
    }

    fn in_room(mut event: serde_json::Value, room: &RoomId) -> Raw<matrix_sdk::ruma::events::room::member::RoomMemberEvent> {
        event["room_id"] = json!(room.as_str());
        raw(event)
    }

    async fn sync(
        client: &matrix_sdk::Client,
        room: &RoomId,
        pos: &str,
        required_state: Vec<serde_json::Value>,
        timeline: Vec<serde_json::Value>,
    ) {
        let mut response = v5::Response::new(pos.to_owned());
        let mut entry = v5::response::Room::default();
        entry.required_state = required_state.into_iter().map(raw).collect();
        entry.timeline = timeline.into_iter().map(raw).collect();
        response.rooms.insert(room.to_owned(), entry);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
    }

    fn final_snapshot(bridge: &crate::RustClient, op_id: u64) -> serde_json::Value {
        let started = std::time::Instant::now();
        loop {
            let found = bridge.events.lock().unwrap().iter().find_map(|raw| {
                let value: serde_json::Value = serde_json::from_str(raw).ok()?;
                (value["type"] == "room_members"
                    && value["op_id"] == json!(op_id)
                    && value["partial"] != json!(true))
                    .then_some(value)
            });
            if let Some(value) = found {
                return value;
            }
            assert!(started.elapsed() < std::time::Duration::from_secs(20), "no answer");
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
    }

    fn row<'a>(snapshot: &'a serde_json::Value, user: &str) -> Option<&'a serde_json::Value> {
        snapshot["members"].as_array()?.iter().find(|m| m["user_id"] == user)
    }

    fn invite_outcomes(bridge: &crate::RustClient, op_id: u64) -> Vec<serde_json::Value> {
        let started = std::time::Instant::now();
        loop {
            let mut rows = Vec::new();
            let mut done = false;
            for raw in bridge.events.lock().unwrap().iter() {
                let Ok(value) = serde_json::from_str::<serde_json::Value>(raw) else { continue };
                if value["op_id"] != json!(op_id) {
                    continue;
                }
                match value["type"].as_str() {
                    Some("room_invite_result") => rows.push(value),
                    Some("room_invite_done") => done = true,
                    _ => {}
                }
            }
            if done {
                return rows;
            }
            assert!(started.elapsed() < std::time::Duration::from_secs(20), "no answer");
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
    }

    #[test]
    fn inviting_into_a_space_sends_the_invite() {
        let space = room_id!("!space:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            server.mock_invite_user_by_id().ok().mock_once().mount().await;
            let me = client.user_id().unwrap().to_string();
            let create = json!({
                "type": "m.room.create", "state_key": "", "sender": me,
                "event_id": "$c0", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "creator": me, "room_version": "10", "type": "m.space" },
            });
            sync(&client, space, "pos-1", vec![create, member(&me, "join", "Me", "$m0")], vec![])
                .await;
            (server, client)
        });
        let dir = std::env::temp_dir().join(format!("lightning-space-invite-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());
        assert!(client.get_room(space).unwrap().is_space());

        invite_users(&bridge, space.to_string(), json!(["@bob:example.org"]).to_string(), 7)
            .expect("a joined Space accepts an invite");
        let rows = invite_outcomes(&bridge, 7);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["ok"], json!(true), "{}", rows[0]);
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn the_canonical_space_parent_is_read_from_room_state() {
        let room = room_id!("!child:example.org");
        let canon = room_id!("!canon:example.org");
        let other = room_id!("!other:example.org");
        let removed = room_id!("!removed:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            let parent = |space: &RoomId, id: &str, content: serde_json::Value| json!({
                "type": "m.space.parent", "state_key": space.as_str(), "sender": me,
                "event_id": id, "origin_server_ts": 1_700_000_000_000u64, "content": content,
            });
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    parent(other, "$p0", json!({ "via": ["example.org"] })),
                    parent(removed, "$p1", json!({})),
                    parent(canon, "$p2", json!({ "via": ["example.org"], "canonical": true })),
                ],
                vec![],
            )
            .await;
            (server, client)
        });
        let joined_room = client.get_room(room).unwrap();
        let found = rt.block_on(canonical_space_parent_json(&client, &joined_room));
        assert_eq!(found["space_id"], json!(canon.as_str()), "{found}");
        assert_eq!(found["via"], json!(["example.org"]));
        assert_eq!(found["joined"], json!(false), "the Space is not one we are in");

        rt.block_on(sync(&client, canon, "pos-2", vec![], vec![]));
        let found = rt.block_on(canonical_space_parent_json(&client, &joined_room));
        assert_eq!(found["joined"], json!(true), "{found}");

        rt.block_on(sync(&client, other, "pos-3", vec![], vec![]));
        let none = rt.block_on(canonical_space_parent_json(&client, &client.get_room(other).unwrap()));
        assert!(none.is_null());
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_member_change_seen_only_in_the_timeline_reaches_the_roster() {
        let room = room_id!("!stale:example.org");
        let alice = "@alice:example.org";
        let bob = "@bob:example.org";
        let carol = "@carol:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client, me) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            server
                .mock_get_members()
                .ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", "Alice", "$a0"), room),
                    in_room(member(bob, "join", "Bob", "$b0"), room),
                ])
                .up_to_n_times(1)
                .mount()
                .await;
            server
                .mock_get_members()
                .ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", "Alicia", "$a1"), room),
                    in_room(member(bob, "ban", "Bob", "$b1"), room),
                    in_room(member(carol, "join", "Carol", "$c1"), room),
                ])
                .mount()
                .await;
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    member(&me, "join", "Me", "$m0"),
                    member(alice, "join", "Alice", "$a0"),
                    member(bob, "join", "Bob", "$b0"),
                ],
                vec![],
            )
            .await;
            (server, client, me)
        });
        let _ = me;

        let dir = std::env::temp_dir()
            .join(format!("lightning-member-freshness-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        room_members(&bridge, room.to_string(), 1).expect("joined room");
        let first = final_snapshot(&bridge, 1);
        assert_eq!(first["ok"], json!(true), "{first}");
        assert_eq!(first["joined_count"], json!(3), "{first}");

        rt.block_on(async {
            let ban = json!({
                "type": "m.room.member", "state_key": bob, "sender": alice,
                "event_id": "$b1", "origin_server_ts": 1_700_000_000_100u64,
                "content": { "membership": "ban", "displayname": "Bob" },
            });
            sync(
                &client,
                room,
                "pos-2",
                vec![],
                vec![
                    member(alice, "join", "Alicia", "$a1"),
                    ban,
                    member(carol, "join", "Carol", "$c1"),
                ],
            )
            .await;
            let joined = client.get_room(room).unwrap();
            let started = std::time::Instant::now();
            while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                != Some("$c1")
            {
                assert!(started.elapsed() < std::time::Duration::from_secs(10));
                tokio::time::sleep(std::time::Duration::from_millis(20)).await;
            }
        });

        room_members(&bridge, room.to_string(), 2).expect("joined room");
        let second = final_snapshot(&bridge, 2);
        assert_eq!(second["ok"], json!(true), "{second}");
        assert_eq!(second["joined_count"], json!(3), "me, Alicia, Carol: {second}");
        assert_eq!(row(&second, bob).map(|r| r["membership"].clone()), Some(json!("banned")));
        assert_eq!(row(&second, carol).map(|r| r["membership"].clone()), Some(json!("joined")));
        assert_eq!(row(&second, alice).map(|r| r["display_name"].clone()), Some(json!("Alicia")));

        room_members(&bridge, room.to_string(), 3).expect("joined room");
        let third = final_snapshot(&bridge, 3);
        assert_eq!(third["joined_count"], json!(3), "{third}");
        let member_requests = rt.block_on(async {
            server
                .server()
                .received_requests()
                .await
                .unwrap_or_default()
                .iter()
                .filter(|r| r.url.path().ends_with("/members"))
                .count()
        });
        assert_eq!(member_requests, 2, "one refetch per member change, none without");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn the_snapshot_carries_own_membership_and_the_sdk_send_and_redact_answers() {
        let room = room_id!("!levels:example.org");
        let invited = room_id!("!invited:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            let me = client.user_id().unwrap().to_string();
            server
                .mock_get_members()
                .ok(vec![in_room(member(&me, "join", "Me", "$m0"), room)])
                .mount()
                .await;
            let levels = json!({
                "type": "m.room.power_levels", "state_key": "", "sender": me,
                "event_id": "$pl", "origin_server_ts": 1_700_000_000_000u64,
                "content": {
                    "events_default": 50, "redact": 0, "users_default": 0,
                    "events": { "m.room.redaction": 0 },
                },
            });
            let create = json!({
                "type": "m.room.create", "state_key": "", "sender": me,
                "event_id": "$create", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "room_version": "11", "creator": me },
            });
            sync(
                &client,
                room,
                "pos-1",
                vec![create, member(&me, "join", "Me", "$m0"), levels],
                vec![],
            )
            .await;
            let mut response = v5::Response::new("pos-2".to_owned());
            let mut entry = v5::response::Room::default();
            entry.invite_state = Some(vec![raw(json!({
                "type": "m.room.member", "state_key": me,
                "sender": "@inviter:example.org",
                "content": { "membership": "invite" },
            }))]);
            response.rooms.insert(invited.to_owned(), entry);
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });

        let dir = std::env::temp_dir()
            .join(format!("lightning-member-permissions-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        room_members(&bridge, room.to_string(), 7).expect("joined room");
        let joined = final_snapshot(&bridge, 7);
        assert_eq!(joined["ok"], json!(true), "{joined}");
        assert_eq!(joined["own_membership"], json!("joined"), "{joined}");
        assert_eq!(joined["own_can_send_message"], json!(false), "{joined}");
        assert_eq!(joined["own_can_redact_other"], json!(true), "{joined}");

        room_members(&bridge, invited.to_string(), 8).expect("an invited room answers");
        let pending = final_snapshot(&bridge, 8);
        assert_eq!(pending["ok"], json!(false), "{pending}");
        assert_eq!(pending["category"], json!("not_joined"), "{pending}");
        assert_eq!(pending["own_membership"], json!("invited"), "{pending}");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    fn own_permissions_answer(bridge: &crate::RustClient, op_id: u64) -> serde_json::Value {
        let started = std::time::Instant::now();
        loop {
            let found = bridge.events.lock().unwrap().iter().find_map(|raw| {
                let value: serde_json::Value = serde_json::from_str(raw).ok()?;
                (value["type"] == "room_own_permissions" && value["op_id"] == json!(op_id))
                    .then_some(value)
            });
            if let Some(value) = found {
                return value;
            }
            assert!(started.elapsed() < std::time::Duration::from_secs(20), "no answer");
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
    }

    #[test]
    fn the_open_room_permissions_come_from_local_power_levels_and_membership() {
        let gated = room_id!("!gated:example.org");
        let lowered = room_id!("!lowered:example.org");
        let raised = room_id!("!raised:example.org");
        let encrypted = room_id!("!encrypted:example.org");
        let below = room_id!("!below:example.org");
        let at = room_id!("!at:example.org");
        let by_default = room_id!("!bydefault:example.org");
        let state_only = room_id!("!stateonly:example.org");
        let left = room_id!("!left:example.org");
        let invited = room_id!("!invited:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            let me = client.user_id().unwrap().to_string();
            let create = json!({
                "type": "m.room.create", "state_key": "", "sender": me,
                "event_id": "$create", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "room_version": "11", "creator": me },
            });
            let levels = |content: serde_json::Value| {
                json!({
                    "type": "m.room.power_levels", "state_key": "", "sender": me,
                    "event_id": "$pl", "origin_server_ts": 1_700_000_000_000u64,
                    "content": content,
                })
            };
            let joined = |content: serde_json::Value| {
                vec![create.clone(), member(&me, "join", "Me", "$m0"), levels(content)]
            };
            sync(&client, gated, "p1", joined(json!({
                "events_default": 50, "redact": 0, "users_default": 0,
                "events": { "m.room.redaction": 0 },
            })), vec![]).await;
            sync(&client, lowered, "p2", joined(json!({
                "events_default": 50, "redact": 50, "users_default": 0,
                "events": { "m.room.message": 0 },
            })), vec![]).await;
            sync(&client, raised, "p3", joined(json!({
                "events_default": 0, "redact": 50, "users_default": 0,
                "events": { "m.room.message": 100 },
            })), vec![]).await;
            let mut state = joined(json!({
                "events_default": 50, "redact": 50, "users_default": 0,
                "events": { "m.room.message": 0 },
            }));
            state.push(json!({
                "type": "m.room.encryption", "state_key": "", "sender": me,
                "event_id": "$enc", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "algorithm": "m.megolm.v1.aes-sha2" },
            }));
            sync(&client, encrypted, "p4", state, vec![]).await;
            sync(&client, below, "p5", joined(json!({
                "events_default": 50, "redact": 50, "users_default": 0,
                "users": { me.clone(): 49 },
            })), vec![]).await;
            sync(&client, at, "p6", joined(json!({
                "events_default": 50, "redact": 50, "users_default": 0,
                "users": { me.clone(): 50 },
            })), vec![]).await;
            sync(&client, by_default, "p6a", joined(json!({
                "events_default": 50, "redact": 50, "users_default": 50,
            })), vec![]).await;
            sync(&client, state_only, "p6b", joined(json!({
                "events_default": 0, "state_default": 100, "redact": 50,
                "users_default": 0,
            })), vec![]).await;
            sync(&client, left, "p7", vec![
                create.clone(),
                member(&me, "leave", "Me", "$m1"),
                levels(json!({ "events_default": 0, "redact": 0 })),
            ], vec![]).await;
            let mut response = v5::Response::new("p8".to_owned());
            let mut entry = v5::response::Room::default();
            entry.invite_state = Some(vec![raw(json!({
                "type": "m.room.member", "state_key": me,
                "sender": "@inviter:example.org",
                "content": { "membership": "invite" },
            }))]);
            response.rooms.insert(invited.to_owned(), entry);
            client
                .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
                .await
                .expect("sync processed");
            (server, client)
        });

        let dir = std::env::temp_dir()
            .join(format!("lightning-own-permissions-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        let requests = || {
            rt.block_on(async {
                server.server().received_requests().await.unwrap_or_default().len()
            })
        };
        let requests_before = requests();
        let ask = |room: &RoomId, op: u64| {
            room_own_permissions(&bridge, room.to_string(), op).expect("a known room");
            own_permissions_answer(&bridge, op)
        };
        let facts = |answer: &serde_json::Value| {
            (
                answer["own_membership"].clone(),
                answer["own_can_send_message"].clone(),
                answer["own_can_redact_other"].clone(),
            )
        };
        let joined = json!("joined");
        let cases = [
            (gated, 1, json!(false), json!(true)),
            (lowered, 2, json!(true), json!(false)),
            (raised, 3, json!(false), json!(false)),
            (encrypted, 4, json!(false), json!(false)),
            (below, 5, json!(false), json!(false)),
            (at, 6, json!(true), json!(true)),
            (by_default, 10, json!(true), json!(true)),
            (state_only, 11, json!(true), json!(false)),
        ];
        for (room, op, send, redact) in cases {
            let answer = ask(room, op);
            assert_eq!(answer["room_id"], json!(room.as_str()), "{answer}");
            assert_eq!(
                facts(&answer),
                (joined.clone(), send, redact),
                "{room}: {answer}"
            );
        }
        let gone = ask(left, 7);
        assert_eq!(facts(&gone), (json!("left"), json!(null), json!(null)), "{gone}");
        let pending = ask(invited, 8);
        assert_eq!(
            facts(&pending),
            (json!("invited"), json!(null), json!(null)),
            "{pending}"
        );
        assert!(room_own_permissions(&bridge, "!nowhere:example.org".to_owned(), 9).is_err());

        assert_eq!(requests(), requests_before, "the open-room facts made a request");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn the_open_room_answer_carries_the_moderation_facts() {
        let mixed = room_id!("!mixed:example.org");
        let none = room_id!("!none:example.org");
        let left = room_id!("!left2:example.org");
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            let me = client.user_id().unwrap().to_string();
            let create = json!({
                "type": "m.room.create", "state_key": "", "sender": me,
                "event_id": "$create", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "room_version": "11", "creator": me },
            });
            let levels = |content: serde_json::Value| {
                json!({
                    "type": "m.room.power_levels", "state_key": "", "sender": me,
                    "event_id": "$pl", "origin_server_ts": 1_700_000_000_000u64,
                    "content": content,
                })
            };
            sync(&client, mixed, "q1", vec![
                create.clone(),
                member(&me, "join", "Me", "$m0"),
                levels(json!({
                    "users": { me.clone(): 50 }, "users_default": 0,
                    "kick": 50, "ban": 100, "invite": 60,
                    "events": { "m.room.topic": 50, "m.room.name": 51 },
                })),
            ], vec![]).await;
            sync(&client, none, "q2", vec![
                create.clone(),
                member(&me, "join", "Me", "$m0"),
                levels(json!({
                    "users_default": 0, "kick": 50, "ban": 50, "invite": 50,
                    "events": { "m.room.topic": 50, "m.room.name": 50 },
                })),
            ], vec![]).await;
            sync(&client, left, "q3", vec![
                create.clone(),
                member(&me, "leave", "Me", "$m1"),
                levels(json!({ "users_default": 100, "kick": 0, "ban": 0 })),
            ], vec![]).await;
            (server, client)
        });
        let dir = std::env::temp_dir()
            .join(format!("lightning-own-moderation-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());
        let ask = |room: &RoomId, op: u64| {
            room_own_permissions(&bridge, room.to_string(), op).expect("a known room");
            own_permissions_answer(&bridge, op)
        };
        let keys = [
            "own_can_kick", "own_can_ban", "own_can_unban", "own_can_invite",
            "own_can_edit_topic", "own_can_edit_name",
        ];
        let read = |answer: &serde_json::Value| keys.map(|k| answer[k].clone());
        let answer = ask(mixed, 1);
        assert_eq!(
            read(&answer),
            [json!(true), json!(false), json!(false), json!(false), json!(true), json!(false)],
            "{answer}"
        );
        let answer = ask(none, 2);
        assert_eq!(read(&answer), std::array::from_fn::<_, 6, _>(|_| json!(false)), "{answer}");
        let answer = ask(left, 3);
        assert_eq!(read(&answer), std::array::from_fn::<_, 6, _>(|_| json!(null)), "{answer}");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_member_change_the_server_already_applied_costs_no_request() {
        let room = room_id!("!applied:example.org");
        let alice = "@alice:example.org";
        let bob = "@bob:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            server
                .mock_get_members()
                .ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", "Alice", "$a0"), room),
                    in_room(member(bob, "join", "Bob", "$b0"), room),
                ])
                .mount()
                .await;
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    member(&me, "join", "Me", "$m0"),
                    member(alice, "join", "Alice", "$a0"),
                    member(bob, "join", "Bob", "$b0"),
                ],
                vec![],
            )
            .await;
            (server, client)
        });

        let dir = std::env::temp_dir()
            .join(format!("lightning-member-applied-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        room_members(&bridge, room.to_string(), 1).expect("joined room");
        assert_eq!(final_snapshot(&bridge, 1)["ok"], json!(true));

        rt.block_on(async {
            let ban = json!({
                "type": "m.room.member", "state_key": bob, "sender": alice,
                "event_id": "$b1", "origin_server_ts": 1_700_000_000_100u64,
                "content": { "membership": "ban", "displayname": "Bob" },
            });
            let rename = member(alice, "join", "Alicia", "$a1");
            sync(
                &client,
                room,
                "pos-2",
                vec![rename.clone(), ban.clone()],
                vec![rename, ban],
            )
            .await;
            let joined = client.get_room(room).unwrap();
            let started = std::time::Instant::now();
            while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                != Some("$b1")
            {
                assert!(started.elapsed() < std::time::Duration::from_secs(10));
                tokio::time::sleep(std::time::Duration::from_millis(20)).await;
            }
        });

        room_members(&bridge, room.to_string(), 2).expect("joined room");
        let second = final_snapshot(&bridge, 2);
        assert_eq!(row(&second, bob).map(|r| r["membership"].clone()), Some(json!("banned")));
        assert_eq!(row(&second, alice).map(|r| r["display_name"].clone()), Some(json!("Alicia")));
        let member_requests = rt.block_on(async {
            server
                .server()
                .received_requests()
                .await
                .unwrap_or_default()
                .iter()
                .filter(|r| r.url.path().ends_with("/members"))
                .count()
        });
        assert_eq!(member_requests, 1, "the store agreed with the timeline: no refetch");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_failed_refetch_is_not_retried_on_every_read() {
        let room = room_id!("!failing:example.org");
        let alice = "@alice:example.org";
        let bob = "@bob:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            server
                .mock_get_members()
                .ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", "Alice", "$a0"), room),
                    in_room(member(bob, "join", "Bob", "$b0"), room),
                ])
                .up_to_n_times(1)
                .mount()
                .await;
            server.mock_get_members().error_unrecognized().mount().await;
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    member(&me, "join", "Me", "$m0"),
                    member(alice, "join", "Alice", "$a0"),
                    member(bob, "join", "Bob", "$b0"),
                ],
                vec![],
            )
            .await;
            (server, client)
        });
        let dir = std::env::temp_dir()
            .join(format!("lightning-member-failing-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        room_members(&bridge, room.to_string(), 1).expect("joined room");
        assert_eq!(final_snapshot(&bridge, 1)["ok"], json!(true));

        rt.block_on(async {
            let ban = json!({
                "type": "m.room.member", "state_key": bob, "sender": alice,
                "event_id": "$b1", "origin_server_ts": 1_700_000_000_100u64,
                "content": { "membership": "ban", "displayname": "Bob" },
            });
            sync(&client, room, "pos-2", vec![], vec![ban]).await;
            let joined = client.get_room(room).unwrap();
            let started = std::time::Instant::now();
            while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                != Some("$b1")
            {
                assert!(started.elapsed() < std::time::Duration::from_secs(10));
                tokio::time::sleep(std::time::Duration::from_millis(20)).await;
            }
        });

        room_members(&bridge, room.to_string(), 2).expect("joined room");
        let second = final_snapshot(&bridge, 2);
        assert_eq!(second["ok"], json!(true), "{second}");
        assert_eq!(row(&second, bob).map(|r| r["membership"].clone()), Some(json!("joined")));
        assert!(!client.get_room(room).unwrap().are_members_synced());

        std::thread::sleep(std::time::Duration::from_millis(150));
        for op in 3..6 {
            room_members(&bridge, room.to_string(), op).expect("joined room");
            let again = final_snapshot(&bridge, op);
            assert_eq!(again["ok"], json!(true), "{again}");
        }
        let member_requests = rt.block_on(async {
            server
                .server()
                .received_requests()
                .await
                .unwrap_or_default()
                .iter()
                .filter(|r| r.url.path().ends_with("/members"))
                .count()
        });
        assert_eq!(member_requests, 2, "the first fetch and the failed refetch, nothing after");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_read_inside_the_gap_answers_at_once_and_the_later_refetch_pokes() {
        let room = room_id!("!deferred:example.org");
        let alice = "@alice:example.org";
        let bob = "@bob:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            for (alice_name, times) in [("Alice", Some(1)), ("Alicia", Some(1)), ("Alix", None)] {
                let mock = server.mock_get_members().ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", alice_name, "$a"), room),
                    in_room(member(bob, "join", "Bob", "$b0"), room),
                ]);
                match times {
                    Some(n) => mock.up_to_n_times(n).mount().await,
                    None => mock.mount().await,
                }
            }
            let encryption = json!({
                "type": "m.room.encryption", "state_key": "", "sender": me,
                "event_id": "$enc", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "algorithm": "m.megolm.v1.aes-sha2" },
            });
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    encryption,
                    member(&me, "join", "Me", "$m0"),
                    member(alice, "join", "Alice", "$a0"),
                    member(bob, "join", "Bob", "$b0"),
                ],
                vec![],
            )
            .await;
            (server, client)
        });
        assert!(client.get_room(room).unwrap().encryption_state().is_encrypted());
        let dir = std::env::temp_dir()
            .join(format!("lightning-member-deferred-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        let rename = |name: &'static str, id: &'static str| {
            let client = client.clone();
            let pos = format!("pos-{id}");
            rt.block_on(async move {
                sync(&client, room, &pos, vec![], vec![member(alice, "join", name, id)]).await;
                let joined = client.get_room(room).unwrap();
                let started = std::time::Instant::now();
                while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                    != Some(id)
                {
                    assert!(started.elapsed() < std::time::Duration::from_secs(10));
                    tokio::time::sleep(std::time::Duration::from_millis(20)).await;
                }
            });
        };
        let name_of = |snapshot: &serde_json::Value| {
            row(snapshot, alice).map(|r| r["display_name"].clone())
        };

        room_members(&bridge, room.to_string(), 1).expect("joined room");
        assert_eq!(name_of(&final_snapshot(&bridge, 1)), Some(json!("Alice")));
        rename("Alicia", "$a1");
        room_members(&bridge, room.to_string(), 2).expect("joined room");
        assert_eq!(name_of(&final_snapshot(&bridge, 2)), Some(json!("Alicia")));

        rename("Alix", "$a2");
        let asked = std::time::Instant::now();
        room_members(&bridge, room.to_string(), 3).expect("joined room");
        let third = final_snapshot(&bridge, 3);
        assert!(asked.elapsed() < std::time::Duration::from_secs(2), "held for the gap");
        assert_eq!(name_of(&third), Some(json!("Alicia")), "{third}");

        let started = std::time::Instant::now();
        loop {
            let poked = bridge.events.lock().unwrap().iter().any(|raw| {
                serde_json::from_str::<serde_json::Value>(raw).is_ok_and(|v| {
                    v["type"] == "room_members_changed" && v["room_id"] == room.as_str()
                })
            });
            if poked {
                break;
            }
            assert!(started.elapsed() < std::time::Duration::from_secs(20), "no poke");
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        room_members(&bridge, room.to_string(), 4).expect("joined room");
        assert_eq!(name_of(&final_snapshot(&bridge, 4)), Some(json!("Alix")));
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_deferred_refetch_is_a_tracked_room_action_that_ends_with_the_session() {
        let room = room_id!("!deferred-shutdown:example.org");
        let alice = "@alice:example.org";
        let bob = "@bob:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            for (alice_name, times) in [("Alice", Some(1)), ("Alicia", Some(1)), ("Alix", None)] {
                let mock = server.mock_get_members().ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", alice_name, "$a"), room),
                    in_room(member(bob, "join", "Bob", "$b0"), room),
                ]);
                match times {
                    Some(n) => mock.up_to_n_times(n).mount().await,
                    None => mock.mount().await,
                }
            }
            let encryption = json!({
                "type": "m.room.encryption", "state_key": "", "sender": me,
                "event_id": "$enc", "origin_server_ts": 1_700_000_000_000u64,
                "content": { "algorithm": "m.megolm.v1.aes-sha2" },
            });
            sync(
                &client,
                room,
                "pos-1",
                vec![
                    encryption,
                    member(&me, "join", "Me", "$m0"),
                    member(alice, "join", "Alice", "$a0"),
                    member(bob, "join", "Bob", "$b0"),
                ],
                vec![],
            )
            .await;
            (server, client)
        });
        let dir = std::env::temp_dir()
            .join(format!("lightning-member-deferred-shutdown-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());

        let rename = |name: &'static str, id: &'static str| {
            let client = client.clone();
            let pos = format!("pos-{id}");
            rt.block_on(async move {
                sync(&client, room, &pos, vec![], vec![member(alice, "join", name, id)]).await;
                let joined = client.get_room(room).unwrap();
                let started = std::time::Instant::now();
                while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                    != Some(id)
                {
                    assert!(started.elapsed() < std::time::Duration::from_secs(10));
                    tokio::time::sleep(std::time::Duration::from_millis(20)).await;
                }
            });
        };
        let member_requests = || {
            rt.block_on(async {
                server
                    .server()
                    .received_requests()
                    .await
                    .unwrap_or_default()
                    .iter()
                    .filter(|r| r.url.path().ends_with("/members"))
                    .count()
            })
        };

        room_members(&bridge, room.to_string(), 1).expect("joined room");
        final_snapshot(&bridge, 1);
        rename("Alicia", "$a1");
        room_members(&bridge, room.to_string(), 2).expect("joined room");
        final_snapshot(&bridge, 2);
        rename("Alix", "$a2");
        room_members(&bridge, room.to_string(), 3).expect("joined room");
        final_snapshot(&bridge, 3);

        std::thread::sleep(std::time::Duration::from_millis(300));
        let running = bridge
            .room_action_tasks
            .lock()
            .unwrap()
            .iter()
            .filter(|task| !task.is_finished())
            .count();
        assert_eq!(running, 1, "the scheduled refetch is not a tracked room action");

        let (_, _, actions_missed, _, actions_ms, _, _) = bridge.shutdown_managed_tasks();
        assert_eq!(actions_missed, 0, "the waiting refetch had to be aborted");
        assert!(actions_ms < crate::SHUTDOWN_ACTION_JOIN_MS, "held the join for {actions_ms} ms");
        assert_eq!(member_requests(), 2, "a /members request after the session ended");
        let poked = bridge.events.lock().unwrap().iter().any(|raw| {
            serde_json::from_str::<serde_json::Value>(raw)
                .is_ok_and(|v| v["type"] == "room_members_changed")
        });
        assert!(!poked, "a poke from the ended session");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn a_refetch_decided_before_sign_out_sends_nothing_after_it() {
        let room = room_id!("!signout:example.org");
        let alice = "@alice:example.org";
        let rt = tokio::runtime::Runtime::new().expect("runtime");
        let (server, client) = rt.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            client.event_cache().subscribe().expect("event cache");
            let me = client.user_id().unwrap().to_string();
            server
                .mock_get_members()
                .ok(vec![
                    in_room(member(&me, "join", "Me", "$m0"), room),
                    in_room(member(alice, "join", "Alice", "$a0"), room),
                ])
                .mount()
                .await;
            sync(
                &client,
                room,
                "pos-1",
                vec![member(&me, "join", "Me", "$m0"), member(alice, "join", "Alice", "$a0")],
                vec![],
            )
            .await;
            (server, client)
        });
        let dir = std::env::temp_dir()
            .join(format!("lightning-member-signout-{}", std::process::id()));
        let bridge = crate::RustClient::new(dir).expect("bridge");
        *bridge.client.lock().unwrap() = Some(client.clone());
        room_members(&bridge, room.to_string(), 1).expect("joined room");
        final_snapshot(&bridge, 1);

        let joined = client.get_room(room).unwrap();
        let ctx = MemberRefreshCtx {
            timelines: Arc::clone(&bridge.timelines),
            lifecycle: bridge.timelines.lifecycle(),
            events: Arc::clone(&bridge.events),
        };
        let (step, scan, gap) = rt.block_on(async {
            sync(&client, room, "pos-2", vec![], vec![member(alice, "join", "Alicia", "$a1")])
                .await;
            let started = std::time::Instant::now();
            while scan_member_events(&joined, None).await.newest.as_deref().map(|e| e.as_str())
                != Some("$a1")
            {
                assert!(started.elapsed() < std::time::Duration::from_secs(10));
                tokio::time::sleep(std::time::Duration::from_millis(20)).await;
            }
            let (step, scan, gap, _) = decide_member_step(&joined, &ctx, false).await;
            (step, scan, gap)
        });
        assert_eq!(step, MemberStep::Refresh);

        bridge.timelines.shutdown(&bridge.runtime);
        let result =
            rt.block_on(forced_member_refresh(&joined, &ctx, &scan, gap, "read"));
        assert_eq!(result, (false, 0));
        let member_requests = rt.block_on(async {
            server
                .server()
                .received_requests()
                .await
                .unwrap_or_default()
                .iter()
                .filter(|r| r.url.path().ends_with("/members"))
                .count()
        });
        assert_eq!(member_requests, 1, "a /members request from the ended session");
        drop(bridge);
        drop(server);
        drop(rt);
    }

    #[test]
    fn the_store_agrees_only_with_what_the_roster_would_show() {
        let facts = |membership: &str, name: &str, avatar: &str| MemberFacts {
            membership: membership.to_owned(),
            display_name: name.to_owned(),
            avatar_url: avatar.to_owned(),
        };
        let bob = facts("join", "Bob", "mxc://x/b");
        assert!(store_agrees(Some(&bob), &bob));
        assert!(!store_agrees(Some(&bob), &facts("join", "Robert", "mxc://x/b")));
        assert!(!store_agrees(Some(&bob), &facts("join", "Bob", "mxc://x/new")));
        assert!(!store_agrees(Some(&bob), &facts("ban", "Bob", "mxc://x/b")));
        assert!(!store_agrees(Some(&bob), &facts("leave", "", "")));
        assert!(store_agrees(Some(&facts("ban", "Bob", "")), &facts("ban", "", "")));
        assert!(!store_agrees(None, &facts("join", "Carol", "")));
        assert!(!store_agrees(None, &facts("invite", "Carol", "")));
        assert!(!store_agrees(None, &facts("ban", "Carol", "")));
        assert!(store_agrees(None, &facts("leave", "", "")));
        assert!(store_agrees(None, &facts("knock", "", "")));
    }

    #[test]
    fn a_refetch_is_forced_only_when_stale_and_never_inside_the_gap_or_backoff() {
        let now = std::time::Instant::now();
        let gap = std::time::Duration::from_secs(5);
        let fresh = MemberRefreshState::default();
        assert_eq!(member_step(&fresh, now, true, false, gap), MemberStep::Skip);
        assert_eq!(member_step(&fresh, now, false, true, gap), MemberStep::SdkSync);
        assert_eq!(member_step(&fresh, now, true, true, gap), MemberStep::Refresh);
        let running = MemberRefreshState { in_flight: true, ..Default::default() };
        assert_eq!(member_step(&running, now, false, true, gap), MemberStep::ServeWhileRunning);
        let recent = MemberRefreshState { started: Some(now), ..Default::default() };
        assert_eq!(member_step(&recent, now, true, true, gap), MemberStep::Defer(now + gap));
        assert_eq!(member_step(&recent, now, true, false, gap), MemberStep::Skip);
        assert_eq!(member_step(&recent, now + gap, true, true, gap), MemberStep::Refresh);
        let failed = MemberRefreshState {
            started: Some(now),
            left_missing: true,
            failures: 1,
            retry_after: Some(now + std::time::Duration::from_secs(30)),
            ..Default::default()
        };
        assert_eq!(
            member_step(&failed, now + gap, false, false, gap),
            MemberStep::Defer(now + std::time::Duration::from_secs(30))
        );
        assert_eq!(
            member_step(&failed, now + std::time::Duration::from_secs(30), false, false, gap),
            MemberStep::Refresh
        );
        assert_eq!(member_step(&failed, now + gap, true, false, gap), MemberStep::Skip);
    }

    #[test]
    fn the_refetch_gap_grows_with_the_roster_and_is_capped() {
        use std::time::Duration;
        assert_eq!(member_refresh_gap(0, false), Duration::ZERO);
        assert_eq!(member_refresh_gap(3, false), Duration::from_millis(30));
        assert_eq!(member_refresh_gap(1_000, false), Duration::from_secs(10));
        assert_eq!(member_refresh_gap(6_000, false), MEMBER_REFRESH_GAP_MAX);
        assert_eq!(member_refresh_gap(u64::MAX, false), MEMBER_REFRESH_GAP_MAX);
        assert_eq!(member_refresh_gap(3, true), MEMBER_REFRESH_GAP_ENCRYPTED_MIN);
        assert_eq!(member_refresh_gap(500, true), Duration::from_secs(15));
        assert_eq!(member_refresh_gap(6_000, true), Duration::from_secs(180));
        assert_eq!(member_refresh_gap(u64::MAX, true), MEMBER_REFRESH_GAP_ENCRYPTED_MAX);
    }

    #[test]
    fn a_failed_refetch_backs_off_and_doubles() {
        use std::time::Duration;
        let gap = Duration::from_millis(30);
        assert_eq!(member_refresh_backoff(1, gap), Duration::from_secs(30));
        assert_eq!(member_refresh_backoff(2, gap), Duration::from_secs(60));
        assert_eq!(member_refresh_backoff(3, gap), Duration::from_secs(120));
        assert_eq!(member_refresh_backoff(50, gap), MEMBER_REFRESH_BACKOFF_MAX);
        assert_eq!(member_refresh_backoff(1, Duration::from_secs(45)), Duration::from_secs(45));
    }
}
