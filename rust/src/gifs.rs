//! GIF provider network access (GIPHY / KLIPY).
//!
//! A bounded, redirect-validated HTTPS GET for the C++ GIF controller. The
//! URL is built in C++ and carries the provider API key, so it is treated
//! as secret and never logged; only a coarse status/category and the bounded
//! JSON body cross back. No Matrix identifiers are sent.

use std::sync::Arc;

use serde_json::json;

use crate::RustClient;

// Bound provider JSON responses so a broken endpoint cannot stream
// unbounded bytes into memory.
const MAX_GIF_JSON_BYTES: usize = 2 * 1_048_576;

/// Coarse, safe category for a provider HTTP status. Never server text.
fn status_category(status: u16) -> &'static str {
    match status {
        200..=299 => "ok",
        429 => "rate_limited",
        400..=499 => "provider_error",
        _ => "provider_error",
    }
}

/// Fetch a provider trending/search response. Emits one `gif_response`:
///   { type:"gif_response", op_id, lifecycle, ok, status, category, body }
/// `body` is the bounded JSON on success (parsed in C++; QML never sees raw
/// JSON), empty otherwise. `category`: ok / rate_limited / provider_error /
/// timeout / network / too_large / blocked / invalid_url.
pub(crate) fn gif_get(
    bridge: &RustClient,
    url: String,
    op_id: u64,
) -> Result<(), String> {
    // Session-scoped, so logout or account switch cancels in-flight work.
    crate::rooms::require_client(bridge)?;
    let parsed = url::Url::parse(url.trim())
        .map_err(|_| "invalid provider URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return Err("unsupported or credentialed URL".to_owned());
    }

    let events = Arc::clone(&bridge.command_events);
    let parked = Arc::clone(&bridge.media_results);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = crate::rooms::safe_get_following_redirects(
            parsed, MAX_GIF_JSON_BYTES, "application/json").await;
        if !timelines.lifecycle_current(lifecycle) {
            return; // stale after logout / account switch
        }
        match result {
            Ok((response, _final_url, _redirects)) => {
                let status = response.status.as_u16();
                let category = status_category(status);
                // Only forwarded on 2xx; error bodies are never surfaced.
                let body = if category == "ok" {
                    String::from_utf8_lossy(&response.bytes).into_owned()
                } else {
                    String::new()
                };
                crate::enqueue_terminal(&events, &parked, json!({
                    "type": "gif_response",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": category == "ok",
                    "status": status,
                    "category": category,
                    "body": body,
                }));
            }
            Err(failure) => {
                // Map preview failure categories to the GIF set.
                let category = match failure.category {
                    "timeout" => "timeout",
                    "response_too_large" => "too_large",
                    "blocked_destination" | "invalid_url" => "blocked",
                    "dns_failure" | "request_failure" => "network",
                    "too_many_redirects" | "invalid_redirect" => "network",
                    _ => "network",
                };
                crate::enqueue_terminal(&events, &parked, json!({
                    "type": "gif_response",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "status": failure.status,
                    "category": category,
                    "body": "",
                }));
            }
        }
    });
    Ok(())
}

// Maximum download size for a sendable GIF.
const MAX_GIF_BYTES: usize = 25 * 1_048_576;
// Maximum canvas edge (matches C++ gif::kMaxGifDimension).
const MAX_GIF_EDGE: u16 = 4096;

/// True for known GIF-provider media CDNs. C++ validates the host too.
fn is_provider_media_host(host: &str) -> bool {
    let h = host.to_ascii_lowercase();
    h == "giphy.com" || h.ends_with(".giphy.com")
        || h == "klipy.com" || h.ends_with(".klipy.com")
}

/// Validate bytes as a real GIF and read its canvas size: (width, height),
/// or a coarse rejection. Rejects error pages, non-GIF magic and oversized
/// canvases.
fn validate_gif_bytes(bytes: &[u8]) -> Result<(u32, u32), &'static str> {
    if bytes.len() < 10 {
        return Err("invalid_media");
    }
    // Only "GIF87a"/"GIF89a" may be sent as image/gif.
    if &bytes[0..6] != b"GIF87a" && &bytes[0..6] != b"GIF89a" {
        return Err("not_a_gif");
    }
    // Logical screen descriptor: little-endian u16 width/height at [6..10].
    let width = u16::from_le_bytes([bytes[6], bytes[7]]);
    let height = u16::from_le_bytes([bytes[8], bytes[9]]);
    if width == 0 || height == 0 {
        return Err("invalid_media");
    }
    if width > MAX_GIF_EDGE || height > MAX_GIF_EDGE {
        return Err("too_large");
    }
    Ok((u32::from(width), u32::from(height)))
}

/// Download and validate a provider GIF, parking the bytes for
/// `mx_rust_media_take` (by op_id); they never enter the JSON queue. Emits
/// one `gif_download_result { op_id, ok, mime, width, height, size,
/// category }`; failure categories: blocked / not_a_gif / too_large /
/// invalid_media / timeout / network / provider_error.
pub(crate) fn gif_download(
    bridge: &RustClient,
    url: String,
    op_id: u64,
) -> Result<(), String> {
    crate::rooms::require_client(bridge)?;
    let parsed = url::Url::parse(url.trim())
        .map_err(|_| "invalid gif URL".to_owned())?;
    if parsed.scheme() != "https" || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return Err("unsupported or credentialed URL".to_owned());
    }
    // Provider media CDNs only.
    match parsed.host_str() {
        Some(host) if is_provider_media_host(host) => {}
        _ => return Err("gif host not allowed".to_owned()),
    }

    let events = Arc::clone(&bridge.command_events);
    let timelines = Arc::clone(&bridge.timelines);
    let results = Arc::clone(&bridge.media_results);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        // Accept image/gif only, so a `.gif` URL is not negotiated into webp/mp4.
        let fetched = crate::rooms::safe_get_following_redirects(
            parsed, MAX_GIF_BYTES, "image/gif").await;
        if !timelines.lifecycle_current(lifecycle) {
            return; // stale after logout / account switch
        }
        let emit_fail = |category: &str| {
            crate::enqueue_terminal(&events, &results, json!({
                "type": "gif_download_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": false,
                "category": category,
            }));
        };
        let (response, _final_url, _redirects) = match fetched {
            Ok(v) => v,
            Err(failure) => {
                let category = match failure.category {
                    "timeout" => "timeout",
                    "response_too_large" => "too_large",
                    "blocked_destination" | "invalid_url" => "blocked",
                    _ => "network",
                };
                emit_fail(category);
                return;
            }
        };
        if !response.status.is_success() {
            emit_fail("provider_error");
            return;
        }
        match validate_gif_bytes(&response.bytes) {
            Ok((width, height)) => {
                let size = response.bytes.len() as u64;
                if let Ok(mut guard) = results.lock() {
                    guard.insert(op_id, response.bytes);
                }
                crate::enqueue_terminal(&events, &results, json!({
                    "type": "gif_download_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "mime": "image/gif",
                    "width": width,
                    "height": height,
                    "size": size,
                }));
            }
            Err(category) => emit_fail(category),
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gif_header(w: u16, h: u16) -> Vec<u8> {
        let mut v = b"GIF89a".to_vec();
        v.extend_from_slice(&w.to_le_bytes());
        v.extend_from_slice(&h.to_le_bytes());
        v.extend_from_slice(&[0u8; 8]); // pad past the header
        v
    }

    #[test]
    fn accepts_real_gif_and_reads_size() {
        assert_eq!(validate_gif_bytes(&gif_header(200, 150)), Ok((200, 150)));
        let mut gif87 = b"GIF87a".to_vec();
        gif87.extend_from_slice(&[10, 0, 20, 0, 0, 0, 0, 0]);
        assert_eq!(validate_gif_bytes(&gif87), Ok((10, 20)));
    }

    #[test]
    fn rejects_non_gif_payloads() {
        assert_eq!(validate_gif_bytes(b"<!DOCTYPE html><html>"),
                   Err("not_a_gif"));
        assert_eq!(validate_gif_bytes(b"{\"error\":\"nope\"}"),
                   Err("not_a_gif"));
        // An MP4 ftyp box is not a GIF.
        assert_eq!(validate_gif_bytes(b"\x00\x00\x00\x18ftypmp42"),
                   Err("not_a_gif"));
        assert_eq!(validate_gif_bytes(b"GIF"), Err("invalid_media"));
    }

    #[test]
    fn rejects_zero_and_oversized_canvas() {
        assert_eq!(validate_gif_bytes(&gif_header(0, 100)), Err("invalid_media"));
        assert_eq!(validate_gif_bytes(&gif_header(5000, 100)), Err("too_large"));
    }

    #[test]
    fn host_policy_allows_only_providers() {
        assert!(is_provider_media_host("media0.giphy.com"));
        assert!(is_provider_media_host("giphy.com"));
        assert!(is_provider_media_host("static.klipy.com"));
        assert!(!is_provider_media_host("evil.com"));
        assert!(!is_provider_media_host("giphy.com.evil.net"));
    }
}

// Live provider checks, ignored by default (network and real keys needed):
//   export LIGHTNING_GIPHY_API_KEY=... LIGHTNING_KLIPY_API_KEY=...
//   cargo test --manifest-path rust/Cargo.toml -- --ignored --nocapture gif_live
// They use the same fetch path as gif_get; the key and URL are never
// printed.
#[cfg(test)]
mod live_tests {
    async fn fetch_json(url: url::Url) -> (u16, serde_json::Value) {
        let (resp, _u, _r) = crate::rooms::safe_get_following_redirects(
            url, 2 * 1_048_576, "application/json")
            .await
            .expect("safe fetch");
        let status = resp.status.as_u16();
        let value: serde_json::Value =
            serde_json::from_slice(&resp.bytes).unwrap_or(serde_json::Value::Null);
        (status, value)
    }

    #[tokio::test]
    #[ignore]
    async fn gif_live_giphy_trending_and_search() {
        let key = std::env::var("LIGHTNING_GIPHY_API_KEY").expect("GIPHY key");
        let trending = url::Url::parse(&format!(
            "https://api.giphy.com/v1/gifs/trending?api_key={key}&limit=5&rating=g"
        ))
        .unwrap();
        let (st, v) = fetch_json(trending).await;
        assert_eq!(st, 200, "giphy trending status");
        assert!(v.get("data").and_then(|d| d.as_array()).is_some(),
                "giphy trending has data[]");

        let search = url::Url::parse(&format!(
            "https://api.giphy.com/v1/gifs/search?api_key={key}&q=cat&limit=5&rating=g"
        ))
        .unwrap();
        let (st2, v2) = fetch_json(search).await;
        assert_eq!(st2, 200, "giphy search status");
        assert!(!v2["data"].as_array().unwrap().is_empty(),
                "giphy search returned results");
    }

    #[tokio::test]
    #[ignore]
    async fn gif_live_download_and_validate() {
        // Download a real GIPHY result's original .gif through gif_download's path
        // and validate it.
        let key = std::env::var("LIGHTNING_GIPHY_API_KEY").expect("GIPHY key");
        let trending = url::Url::parse(&format!(
            "https://api.giphy.com/v1/gifs/trending?api_key={key}&limit=1&rating=g"
        ))
        .unwrap();
        let (_st, v) = fetch_json(trending).await;
        let gif_url = v["data"][0]["images"]["original"]["url"]
            .as_str()
            .expect("original gif url")
            .to_owned();
        let parsed = url::Url::parse(&gif_url).unwrap();
        assert!(super::is_provider_media_host(parsed.host_str().unwrap()));
        let (resp, _u, _r) = crate::rooms::safe_get_following_redirects(
            parsed, super::MAX_GIF_BYTES, "image/gif")
            .await
            .expect("download");
        assert!(resp.status.is_success());
        let (w, h) = super::validate_gif_bytes(&resp.bytes).expect("valid gif");
        assert!(w > 0 && h > 0);
    }

    #[tokio::test]
    #[ignore]
    async fn gif_live_klipy_trending_and_search() {
        let key = std::env::var("LIGHTNING_KLIPY_API_KEY").expect("KLIPY key");
        let trending = url::Url::parse(&format!(
            "https://api.klipy.com/api/v1/{key}/gifs/trending?per_page=8&rating=g"
        ))
        .unwrap();
        let (st, v) = fetch_json(trending).await;
        assert_eq!(st, 200, "klipy trending status");
        assert_eq!(v.get("result").and_then(|r| r.as_bool()), Some(true),
                   "klipy result:true");
        assert!(v["data"]["data"].as_array().is_some(), "klipy data.data[]");

        let search = url::Url::parse(&format!(
            "https://api.klipy.com/api/v1/{key}/gifs/search?q=cat&per_page=8&rating=g"
        ))
        .unwrap();
        let (st2, _v2) = fetch_json(search).await;
        assert_eq!(st2, 200, "klipy search status");
    }
}
