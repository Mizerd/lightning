//! A link's own media: a URL that is itself an image or a video.
//!
//! The preview (`rooms::preview`) decides from the first response's head
//! whether a link is a video, or an image too large to inline, and describes
//! it without reading the rest. The bytes are fetched here, only when the
//! reader asks (Play, or opening the viewer), under the same checks as every
//! preview fetch (`rooms::safe_open`: https only, no credentials, public DNS
//! answers pinned, no proxy, every redirect re-validated), with a size cap,
//! and validated by magic before they are parked. Results are the ordinary
//! `media_ready` / `media_failed` events, so MediaBridge treats the payload
//! like any other media. Never logs the URL.
use std::sync::Arc;

use serde_json::json;

use crate::rooms::{
    classify_preview_payload, image_dimensions, read_rest, require_client,
    response_mime, safe_open_following_redirects, MAX_IMAGE_BYTES,
};
use crate::RustClient;

/// What the viewer may fetch for a direct image link.
pub(crate) const LINK_IMAGE_MAX_BYTES: usize = 25 * 1024 * 1024;
/// Decoded as RGBA that is 200 MB; Qt's reader refuses much beyond it.
pub(crate) const LINK_IMAGE_MAX_PIXELS: u64 = 50_000_000;
/// What Play may fetch for a direct video link. Held in memory until it is
/// written to the player's scratch file, like any other played media.
pub(crate) const LINK_VIDEO_MAX_BYTES: usize = 100 * 1024 * 1024;

pub(crate) const EXPECT_IMAGE: u32 = 0;
pub(crate) const EXPECT_VIDEO: u32 = 1;

const IMAGE_ACCEPT: &str = "image/jpeg,image/png,image/webp,image/gif";
const VIDEO_ACCEPT: &str = "video/mp4,video/webm,video/quicktime,video/*;q=0.8";

/// The container a video payload starts with, as a MIME type, or None. Only
/// what the player's file writer also accepts (MediaBridge::
/// playableExtensionFor): ISO BMFF with a non-image brand, and EBML.
pub(crate) fn sniff_video_container(bytes: &[u8]) -> Option<&'static str> {
    if bytes.len() >= 12 && &bytes[4..8] == b"ftyp" {
        // HEIC and AVIF stills share the container, and so does audio.
        let brand = &bytes[8..12];
        if matches!(brand, b"avif" | b"avis" | b"heic" | b"heix" | b"mif1" | b"msf1"
                           | b"M4A " | b"M4B " | b"M4P ") {
            return None;
        }
        return Some(if brand == b"qt  " { "video/quicktime" } else { "video/mp4" });
    }
    if bytes.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
        // The EBML header names its DocType within the first few dozen bytes.
        let window = &bytes[..bytes.len().min(64)];
        let webm = window.windows(4).any(|w| w == b"webm");
        return Some(if webm { "video/webm" } else { "video/x-matroska" });
    }
    None
}

/// A video, by the same rule images follow: a declared video must carry
/// video magic, and a generic label is promoted only when the bytes prove
/// it. Anything else (text/html, audio/*, a declared image) is not a video.
pub(crate) fn classify_video(declared: &str, head: &[u8]) -> Option<&'static str> {
    let generic = matches!(declared, "" | "application/octet-stream" | "binary/octet-stream");
    if !generic && !declared.starts_with("video/") {
        return None;
    }
    sniff_video_container(head)
}

/// What the head of a preview's first response says to do.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum HeadVerdict {
    /// A video: describe it, read no more.
    Video(&'static str),
    /// An image whose declared length is over the inline cap: describe it.
    LargeImage(&'static str),
    /// Read the rest (bounded) and classify as before. `image` is the raster
    /// type the head already proves, if any.
    ReadRest { image: Option<&'static str> },
}

pub(crate) fn judge_head(declared: &str, head: &[u8], declared_len: Option<u64>) -> HeadVerdict {
    if let Some(mime) = classify_video(declared, head) {
        return HeadVerdict::Video(mime);
    }
    let image = classify_preview_payload(declared, head).ok().flatten();
    if let (Some(mime), Some(len)) = (image, declared_len) {
        if len > MAX_IMAGE_BYTES as u64 {
            return HeadVerdict::LargeImage(mime);
        }
    }
    HeadVerdict::ReadRest { image }
}

/// Preview fields for a direct video link. `size` is 0 when unknown.
pub(crate) fn direct_video_fields(mime: &str, declared_len: Option<u64>) -> serde_json::Value {
    let size = declared_len.unwrap_or(0);
    let too_large = size > LINK_VIDEO_MAX_BYTES as u64;
    json!({
        "preview_kind": "direct_video",
        "title": "", "description": "", "site_name": "",
        "image_source": "", "image_mime": "", "image_width": 0,
        "image_height": 0, "image_size": 0,
        "video_mime": mime,
        "video_size": size,
        "media_too_large": too_large,
    })
}

/// Preview fields for a direct image too large to inline: no bytes, so the
/// card offers the viewer instead. Dimensions when the head carries them.
pub(crate) fn large_image_fields(mime: &str, head: &[u8], size: u64) -> serde_json::Value {
    let (width, height) = image_dimensions(mime, head).unwrap_or((0, 0));
    let pixels = u64::from(width) * u64::from(height);
    let too_large = size > LINK_IMAGE_MAX_BYTES as u64 || pixels > LINK_IMAGE_MAX_PIXELS;
    json!({
        "preview_kind": "direct_media",
        "title": "", "description": "", "site_name": "",
        "image_source": "", "image_mime": mime,
        "image_width": width, "image_height": height,
        "image_size": size,
        "media_too_large": too_large,
    })
}

/// The type of a fetched payload, or the failure category. "rejected" is
/// permanent in MediaBridge; "too_large" is shown to the reader as such.
pub(crate) fn validate_link_media(expect: u32, declared: &str, bytes: &[u8])
    -> Result<&'static str, &'static str> {
    if expect == EXPECT_VIDEO {
        return classify_video(declared, bytes).ok_or("rejected");
    }
    // A declared image must match its magic; SVG and HTML never pass.
    let mime = match classify_preview_payload(declared, bytes) {
        Ok(Some(mime)) => mime,
        _ => return Err("rejected"),
    };
    let (width, height) = image_dimensions(mime, bytes).ok_or("rejected")?;
    if width == 0 || height == 0 {
        return Err("rejected");
    }
    if u64::from(width) * u64::from(height) > LINK_IMAGE_MAX_PIXELS {
        return Err("too_large");
    }
    Ok(mime)
}

/// MediaBridge's key for a link's media: "link:" and 40 lowercase hex.
pub(crate) fn valid_link_key(key: &str) -> bool {
    key.strip_prefix("link:").is_some_and(|hex| {
        hex.len() == 40 && hex.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    })
}

fn fetch_category(category: &'static str) -> &'static str {
    match category {
        "response_too_large" => "too_large",
        "blocked_destination" | "invalid_url" => "blocked",
        other => other,
    }
}

async fn fetch_link_media(url: url::Url, expect: u32, timeout: std::time::Duration)
    -> Result<(Vec<u8>, &'static str), &'static str> {
    let (cap, accept) = if expect == EXPECT_VIDEO {
        (LINK_VIDEO_MAX_BYTES, VIDEO_ACCEPT)
    } else {
        (LINK_IMAGE_MAX_BYTES, IMAGE_ACCEPT)
    };
    let (mut response, _final_url, _redirects) =
        safe_open_following_redirects(url, accept, timeout)
            .await
            .map_err(|failure| fetch_category(failure.category))?;
    let status = response.status();
    if !status.is_success() {
        return Err(if status.is_server_error() || status.as_u16() == 429 {
            "http_transient"
        } else {
            "http_terminal"
        });
    }
    // Refused before a byte of the body is read.
    let declared_len = response.content_length();
    if declared_len.is_some_and(|len| len > cap as u64) {
        return Err("too_large");
    }
    let declared = response_mime(&response);
    // Sized once from the declared length (never beyond the cap), so a large
    // video is not copied through every doubling on its way in.
    let mut bytes = Vec::with_capacity(declared_len.map_or(0, |len| len as usize).min(cap));
    read_rest(&mut response, &mut bytes, cap).await.map_err(fetch_category)?;
    drop(response);
    let mime = validate_link_media(expect, &declared, &bytes)?;
    Ok((bytes, mime))
}

/// Fetch a direct media link for MediaBridge key `key`. `expect` is
/// EXPECT_IMAGE or EXPECT_VIDEO; `timeout_class` is MediaBridge's (0 viewer,
/// 1 playable, 2 save), bounding the whole request below that class's
/// watchdog. Cancellable through `mx_rust_media_cancel(op_id)`.
pub(crate) fn link_media_fetch(
    bridge: &RustClient,
    url: String,
    key: String,
    expect: u32,
    timeout_class: u32,
    op_id: u64,
) -> Result<(), String> {
    // A signed-in session only, like every other media fetch.
    let _ = require_client(bridge)?;
    if !valid_link_key(&key) {
        return Err("invalid link media key".to_owned());
    }
    if expect != EXPECT_IMAGE && expect != EXPECT_VIDEO {
        return Err("invalid link media kind".to_owned());
    }
    let parsed = url::Url::parse(url.trim()).map_err(|_| "invalid URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("unsupported or credentialed URL".to_owned());
    }
    let terminal = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let aborts = Arc::clone(&bridge.media_fetch_aborts);
    let lifecycle = timelines.lifecycle();
    let timeout = std::time::Duration::from_secs(
        crate::rooms::media_timeout_secs(timeout_class.min(2)));
    bridge.spawn_media_fetch(op_id, async move {
        // One bound for the whole exchange, redirects included: `timeout`
        // applies per hop inside, and five hops of it would outlive the C++
        // watchdog, which then reclaims the slot before this answers.
        let outcome = tokio::time::timeout(timeout, fetch_link_media(parsed, expect, timeout))
            .await
            .unwrap_or(Err("timeout"));
        if let Ok(mut guard) = aborts.lock() {
            guard.remove(&op_id);
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match outcome {
            Ok((bytes, mime)) => {
                let size = bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, bytes);
                }
                crate::enqueue_terminal(&terminal, &results, json!({
                    "type": "media_ready",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": key,
                    "kind": 0u32,
                    "size": size,
                    "mimetype": mime,
                    "filename": "",
                }));
            }
            Err(category) => {
                crate::enqueue_terminal(&terminal, &results, json!({
                    "type": "media_failed",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "key": key,
                    "kind": 0u32,
                    "category": category,
                }));
            }
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mp4_head(brand: &[u8; 4]) -> Vec<u8> {
        let mut b = vec![0, 0, 0, 0x20];
        b.extend_from_slice(b"ftyp");
        b.extend_from_slice(brand);
        b.extend_from_slice(&[0; 20]);
        b
    }

    fn webm_head() -> Vec<u8> {
        let mut b = vec![0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01];
        b.extend_from_slice(&[0x42, 0x82, 0x84]);
        b.extend_from_slice(b"webm");
        b.extend_from_slice(&[0; 16]);
        b
    }

    fn png(width: u32, height: u32) -> Vec<u8> {
        let mut b = vec![0; 24];
        b[..8].copy_from_slice(b"\x89PNG\r\n\x1a\n");
        b[16..20].copy_from_slice(&width.to_be_bytes());
        b[20..24].copy_from_slice(&height.to_be_bytes());
        b
    }

    #[test]
    fn video_containers_are_recognised_by_magic() {
        assert_eq!(sniff_video_container(&mp4_head(b"isom")), Some("video/mp4"));
        assert_eq!(sniff_video_container(&mp4_head(b"qt  ")), Some("video/quicktime"));
        assert_eq!(sniff_video_container(&webm_head()), Some("video/webm"));
        let mut mkv = webm_head();
        let at = mkv.windows(4).position(|w| w == b"webm").unwrap();
        mkv[at..at + 4].copy_from_slice(b"mkvx");
        assert_eq!(sniff_video_container(&mkv), Some("video/x-matroska"));
        // Stills in the same container are not video.
        assert_eq!(sniff_video_container(&mp4_head(b"avif")), None);
        assert_eq!(sniff_video_container(&mp4_head(b"heic")), None);
        assert_eq!(sniff_video_container(&mp4_head(b"M4A ")), None);
        assert_eq!(sniff_video_container(b"<html><body>"), None);
        assert_eq!(sniff_video_container(&png(1, 1)), None);
        assert_eq!(sniff_video_container(&[]), None);
    }

    // The same rule as images: a declared type must be matched by the bytes,
    // and only a generic label may be promoted.
    #[test]
    fn a_video_needs_a_video_or_generic_label_and_video_bytes() {
        let mp4 = mp4_head(b"isom");
        assert_eq!(classify_video("video/mp4", &mp4), Some("video/mp4"));
        assert_eq!(classify_video("application/octet-stream", &mp4), Some("video/mp4"));
        assert_eq!(classify_video("", &mp4), Some("video/mp4"));
        assert_eq!(classify_video("text/html", &mp4), None);
        assert_eq!(classify_video("audio/mp4", &mp4), None);
        assert_eq!(classify_video("image/png", &mp4), None);
        assert_eq!(classify_video("video/mp4", b"<html>not a video</html>"), None);
    }

    #[test]
    fn the_head_decides_what_is_read() {
        let mp4 = mp4_head(b"isom");
        assert_eq!(judge_head("video/mp4", &mp4, Some(500 * 1024 * 1024)),
                   HeadVerdict::Video("video/mp4"));
        // A video with no declared length is still only described.
        assert_eq!(judge_head("video/mp4", &mp4, None), HeadVerdict::Video("video/mp4"));
        let big = MAX_IMAGE_BYTES as u64 + 1;
        assert_eq!(judge_head("image/png", &png(10, 10), Some(big)),
                   HeadVerdict::LargeImage("image/png"));
        // At the cap it is read and inlined as before.
        assert_eq!(judge_head("image/png", &png(10, 10), Some(MAX_IMAGE_BYTES as u64)),
                   HeadVerdict::ReadRest { image: Some("image/png") });
        assert_eq!(judge_head("image/png", &png(10, 10), None),
                   HeadVerdict::ReadRest { image: Some("image/png") });
        // A page is read as a page.
        assert_eq!(judge_head("text/html", b"<html><title>x</title>", Some(big)),
                   HeadVerdict::ReadRest { image: None });
        // A lying image label is left to the old classification (invalid_image).
        assert_eq!(judge_head("image/png", b"<svg/>", Some(big)),
                   HeadVerdict::ReadRest { image: None });
    }

    #[test]
    fn description_fields_carry_size_and_the_cap_verdict() {
        let v = direct_video_fields("video/webm", Some(1234));
        assert_eq!(v["preview_kind"], "direct_video");
        assert_eq!(v["video_mime"], "video/webm");
        assert_eq!(v["video_size"], 1234);
        assert_eq!(v["media_too_large"], false);
        assert_eq!(v["image_source"], "");
        let unknown = direct_video_fields("video/mp4", None);
        assert_eq!(unknown["video_size"], 0);
        assert_eq!(unknown["media_too_large"], false);
        let over = direct_video_fields("video/mp4", Some(LINK_VIDEO_MAX_BYTES as u64 + 1));
        assert_eq!(over["media_too_large"], true);

        let img = large_image_fields("image/png", &png(4000, 3000), 9 * 1024 * 1024);
        assert_eq!(img["preview_kind"], "direct_media");
        assert_eq!(img["image_source"], "", "a large image carries no bytes");
        assert_eq!(img["image_width"], 4000);
        assert_eq!(img["image_size"], 9 * 1024 * 1024);
        assert_eq!(img["media_too_large"], false);
        assert_eq!(large_image_fields("image/png", &png(10, 10),
                                      LINK_IMAGE_MAX_BYTES as u64 + 1)["media_too_large"],
                   true);
        assert_eq!(large_image_fields("image/png", &png(10_000, 10_000), 1)["media_too_large"],
                   true);
    }

    #[test]
    fn fetched_bytes_are_validated_by_magic() {
        let mp4 = mp4_head(b"isom");
        assert_eq!(validate_link_media(EXPECT_VIDEO, "video/mp4", &mp4), Ok("video/mp4"));
        assert_eq!(validate_link_media(EXPECT_VIDEO, "video/mp4", b"<html></html>"),
                   Err("rejected"));
        // A video URL that now serves an image is not played, and vice versa.
        assert_eq!(validate_link_media(EXPECT_VIDEO, "image/png", &png(2, 2)), Err("rejected"));
        assert_eq!(validate_link_media(EXPECT_IMAGE, "video/mp4", &mp4), Err("rejected"));
        assert_eq!(validate_link_media(EXPECT_IMAGE, "image/png", &png(2, 2)), Ok("image/png"));
        // SVG, HTML, and an image with no readable size never pass.
        assert_eq!(validate_link_media(EXPECT_IMAGE, "image/svg+xml", b"<svg></svg>"),
                   Err("rejected"));
        assert_eq!(validate_link_media(EXPECT_IMAGE, "text/html", b"<html></html>"),
                   Err("rejected"));
        assert_eq!(validate_link_media(EXPECT_IMAGE, "image/png", &png(0, 5)), Err("rejected"));
        assert_eq!(validate_link_media(EXPECT_IMAGE, "image/png", &png(10_000, 10_000)),
                   Err("too_large"));
    }

    #[test]
    fn only_bridge_shaped_keys_are_accepted() {
        let good = format!("link:{}", "0123456789abcdef0123456789abcdef01234567");
        assert!(valid_link_key(&good));
        assert!(!valid_link_key("link:"));
        assert!(!valid_link_key("full:link:0123"));
        assert!(!valid_link_key(&good.to_uppercase()));
        assert!(!valid_link_key(&format!("{good}0")));
        assert!(!valid_link_key("$event:example.org"));
        assert!(!valid_link_key("link:https://example.org/a.png"));
    }

    #[test]
    fn fetch_failures_use_the_bridge_categories() {
        assert_eq!(fetch_category("response_too_large"), "too_large");
        assert_eq!(fetch_category("blocked_destination"), "blocked");
        assert_eq!(fetch_category("invalid_url"), "blocked");
        assert_eq!(fetch_category("timeout"), "timeout");
    }

    // The caps the C++ side quotes to the reader and the player's own bound.
    #[test]
    fn caps_stay_inside_the_bridge_bounds() {
        assert!(LINK_IMAGE_MAX_BYTES > MAX_IMAGE_BYTES);
        // MediaBridge::kPlayableCacheBytes (256 MiB) must hold one.
        assert!(LINK_VIDEO_MAX_BYTES <= 256 * 1024 * 1024);
    }
}
