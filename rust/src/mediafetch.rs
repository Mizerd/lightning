//! Bounded media downloads.
//!
//! `matrix_sdk::media::Media::get_media_content` starts from
//! `Client::request_config()` and overrides only the timeout (to
//! `Duration::MAX`, matrix-sdk 0.18.0 `media.rs:440-442`). The default
//! `RequestConfig` has `retry_limit: None` (`config/request.rs:78`, "By default
//! requests are retried indefinitely"), and `http_client/native.rs:67-76` then
//! retries a transient answer, which includes every HTTP 5xx and 429, with an
//! exponential backoff for up to 15 minutes. So a homeserver that answers a
//! media request with a prompt HTTP 500 is asked again and again, and the
//! caller's only exit is its own outer timeout.
//!
//! Reported by a Beeper user: every avatar on another homeserver was a
//! thumbnail request Beeper answered with 500 in ~150 ms, each looped until
//! `media_timeout_secs(0)` (40 s), and `MediaBridge::kMaxConcurrent` (8) of
//! them held every slot, so every avatar and image waited 40-45 s.
//!
//! The SDK function takes no `RequestConfig`, so this module is its body with
//! one change: every request is sent with an explicit, bounded config. The
//! cache read, the authenticated-media endpoint choice, the decrypt of an
//! encrypted source and the cache write are the SDK's, step for step.

use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    media::{MediaFormat, MediaRequestParameters},
    ruma::{
        api::{
            client::{authenticated_media, media},
            error::ErrorKind,
            Metadata,
        },
        events::room::MediaSource,
    },
    Client,
};
use matrix_sdk_base::media::store::IgnoreMediaRetentionPolicy;

/// Attempts per HTTP request. matrix-sdk counts `retry_limit` as attempts
/// (`native.rs:73-79`: `with_max_times(limit - 1)`), so 1 is a single request
/// and no retry. A failed media fetch is retried later by MediaBridge's
/// transient failure mark; retrying here would hold one of its eight slots,
/// and a 429's `retry_after` can be minutes.
pub(crate) const MEDIA_ATTEMPTS: usize = 1;

/// Per-request ceiling for an avatar or thumbnail, below the 40 s overall
/// bound even when the thumbnail request and the download fallback both use
/// all of it.
pub(crate) const AVATAR_REQUEST_TIMEOUT: Duration = Duration::from_secs(15);

/// Largest payload accepted from the avatar download fallback. An avatar that
/// needed its thumbnail fetched is at most what an avatar upload may be
/// (MediaBridge::kMotionMaxBytes, `MOTION_PROBE_MAX_BYTES`); the fallback
/// answers a thumbnail request and must not become a path for a 512 MiB file.
pub(crate) const AVATAR_FALLBACK_MAX_BYTES: u64 = 8 * 1024 * 1024;

/// The config every media request is sent with: the client's own, with the
/// retry limit and per-request timeout this module is for.
pub(crate) fn bounded_request_config(base: RequestConfig, timeout: Duration) -> RequestConfig {
    base.retry_limit(MEDIA_ATTEMPTS).timeout(Some(timeout))
}

/// Whether a failed thumbnail request is worth one download of the file. Only
/// a thumbnail (`is_thumbnail`) can fall back, and only on a server fault: a
/// 5xx, or a 404 whose errcode is `M_NOT_FOUND` (a remote server a proxy could
/// not thumbnail). A 404 `M_UNRECOGNIZED` is an endpoint the server lacks, a
/// 403 is a refusal, and a request that never got an answer (`status` None:
/// timeout, connection error) says nothing about the file; none of those
/// retries as a download.
pub(crate) fn avatar_fallback_eligible(
    is_thumbnail: bool,
    status: Option<u16>,
    not_found_errcode: bool,
) -> bool {
    if !is_thumbnail {
        return false;
    }
    match status {
        Some(code) if (500..600).contains(&code) => true,
        Some(404) => not_found_errcode,
        _ => false,
    }
}

/// The HTTP status of a failed request, when the server answered at all.
pub(crate) fn failure_status(err: &matrix_sdk::Error) -> Option<u16> {
    err.as_client_api_error().map(|e| e.status_code.as_u16())
}

fn failure_is_not_found(err: &matrix_sdk::Error) -> bool {
    matches!(err.client_api_error_kind(), Some(ErrorKind::NotFound))
}

/// The category C++ receives for a failed media fetch. Server faults get their
/// own, so a log (and MediaBridge's mark) can tell "the server answered 500"
/// from "the connection failed" and from a timeout; the rest keep the
/// `classify_room_error` words.
pub(crate) fn classify_media_error(err: &matrix_sdk::Error) -> &'static str {
    match failure_status(err) {
        Some(code) if (500..600).contains(&code) => "server_error",
        Some(429) => "rate_limited",
        Some(403) => "forbidden",
        // An endpoint the server lacks keeps the word classify_room_error gave it.
        Some(404) if matches!(err.client_api_error_kind(), Some(ErrorKind::Unrecognized)) => {
            "unrecognized"
        }
        Some(404) => "not_found",
        _ => crate::rooms::classify_room_error(&err.to_string()),
    }
}

/// `Media::get_media_content` with every request bounded by `timeout` and
/// `MEDIA_ATTEMPTS`. Same cache semantics: a hit is returned without a
/// request, a miss is written back when `use_cache`.
pub(crate) async fn get_media_content_bounded(
    client: &Client,
    request: &MediaRequestParameters,
    use_cache: bool,
    timeout: Duration,
) -> matrix_sdk::Result<Vec<u8>> {
    // A send-queue local URI never touches the network; the SDK answers it from
    // the store, so its own function is exactly right.
    if is_local_uri(&request.source) {
        return client.media().get_media_content(request, use_cache).await;
    }

    if use_cache {
        if let Some(content) = cache_get(client, request).await? {
            return Ok(content);
        }
    }

    let content = fetch_uncached(client, request, timeout).await?;

    if use_cache {
        cache_put(client, request, &content).await?;
    }
    Ok(content)
}

/// Read `request` from the media store, without any request.
async fn cache_get(
    client: &Client,
    request: &MediaRequestParameters,
) -> matrix_sdk::Result<Option<Vec<u8>>> {
    Ok(client.media_store().lock().await?.get_media_content(request).await?)
}

/// Write `content` under `request` in the media store, as the SDK does after
/// a download.
async fn cache_put(
    client: &Client,
    request: &MediaRequestParameters,
    content: &[u8],
) -> matrix_sdk::Result<()> {
    client
        .media_store()
        .lock()
        .await?
        .add_media_content(request, content.to_vec(), IgnoreMediaRetentionPolicy::No)
        .await?;
    Ok(())
}

fn is_local_uri(source: &MediaSource) -> bool {
    // matrix-sdk media.rs:61, `LOCAL_MXC_SERVER_NAME`; private there.
    const LOCAL_MXC_SERVER_NAME: &str = "send-queue.localhost";
    let uri = match source {
        MediaSource::Plain(uri) => uri,
        MediaSource::Encrypted(file) => &file.url,
    };
    uri.server_name().is_ok_and(|name| name == LOCAL_MXC_SERVER_NAME)
}

/// The network half of `get_media_content` (matrix-sdk 0.18.0
/// `media.rs:437-520`), decrypting an encrypted source.
async fn fetch_uncached(
    client: &Client,
    request: &MediaRequestParameters,
    timeout: Duration,
) -> matrix_sdk::Result<Vec<u8>> {
    let config = bounded_request_config(client.request_config(), timeout);

    // Use the authenticated endpoints when the server supports it. NOT bounded
    // by `config`: `supported_versions()` is cached after the first call, but
    // that first call (or a refresh) uses the client's own request config and
    // is limited only by the caller's outer timeout.
    let supported_versions = client.supported_versions().await?;
    let use_auth = authenticated_media::get_content::v1::Request::PATH_BUILDER
        .is_supported(&supported_versions);

    let content: Vec<u8> = match &request.source {
        MediaSource::Encrypted(file) => {
            let content = if use_auth {
                let request = authenticated_media::get_content::v1::Request::from_uri(&file.url)?;
                client.send(request).with_request_config(config).await?.file
            } else {
                #[allow(deprecated)]
                let request = media::get_content::v3::Request::from_url(&file.url)?;
                client.send(request).with_request_config(config).await?.file
            };

            let content_len = content.len();
            let mut cursor = std::io::Cursor::new(content);
            let mut reader = matrix_sdk_base::crypto::AttachmentDecryptor::new(
                &mut cursor,
                file.as_ref().clone().into(),
            )?;
            let mut decrypted = Vec::with_capacity(content_len);
            std::io::Read::read_to_end(&mut reader, &mut decrypted)?;
            decrypted
        }

        MediaSource::Plain(uri) => {
            if let MediaFormat::Thumbnail(settings) = &request.format {
                if use_auth {
                    let mut request =
                        authenticated_media::get_content_thumbnail::v1::Request::from_uri(
                            uri,
                            settings.width,
                            settings.height,
                        )?;
                    request.method = Some(settings.method.clone());
                    request.animated = Some(settings.animated);
                    client.send(request).with_request_config(config).await?.file
                } else {
                    #[allow(deprecated)]
                    let request = {
                        let mut request = media::get_content_thumbnail::v3::Request::from_url(
                            uri,
                            settings.width,
                            settings.height,
                        )?;
                        request.method = Some(settings.method.clone());
                        request.animated = Some(settings.animated);
                        request
                    };
                    client.send(request).with_request_config(config).await?.file
                }
            } else if use_auth {
                let request = authenticated_media::get_content::v1::Request::from_uri(uri)?;
                client.send(request).with_request_config(config).await?.file
            } else {
                #[allow(deprecated)]
                let request = media::get_content::v3::Request::from_url(uri)?;
                client.send(request).with_request_config(config).await?.file
            }
        }
    };
    Ok(content)
}

/// Fetch a plain-source avatar (or other mxc thumbnail) and, when its
/// thumbnail request fails on a server fault, try ONE download of the file
/// before giving up. Never used for an encrypted source: a server cannot
/// thumbnail ciphertext, and an encrypted payload must not take a path whose
/// bytes a caller treats as an avatar. `Ok` bytes are not size-checked here
/// beyond the fallback's own cap; the caller applies its class cap.
pub(crate) async fn get_avatar_bounded(
    client: &Client,
    request: &MediaRequestParameters,
    use_cache: bool,
    timeout: Duration,
) -> matrix_sdk::Result<Vec<u8>> {
    let first = get_media_content_bounded(client, request, use_cache, timeout).await;
    let Err(err) = first else { return first };

    let is_thumbnail = matches!(request.format, MediaFormat::Thumbnail(_));
    let plain = matches!(request.source, MediaSource::Plain(_));
    if !plain
        || !avatar_fallback_eligible(is_thumbnail, failure_status(&err), failure_is_not_found(&err))
    {
        return Err(err);
    }

    // The file, under the FILE's own key and never the thumbnail's: a
    // thumbnail key holding full-size bytes would keep answering the thumbnail
    // request from cache after the server recovers. The cap is checked before
    // anything is written, so an oversize payload is not stored.
    let file_request = MediaRequestParameters {
        source: request.source.clone(),
        format: MediaFormat::File,
    };
    if use_cache {
        if let Ok(Some(hit)) = cache_get(client, &file_request).await {
            if hit.len() as u64 <= AVATAR_FALLBACK_MAX_BYTES {
                return Ok(hit);
            }
            return Err(err);
        }
    }
    let bytes = match fetch_uncached(client, &file_request, timeout).await {
        Ok(bytes) => bytes,
        // The thumbnail's error is the more informative of the two, and it
        // is what the user's server reported first; the download adds none.
        Err(_) => return Err(err),
    };
    if bytes.len() as u64 > AVATAR_FALLBACK_MAX_BYTES {
        return Err(err);
    }
    if use_cache {
        // A failed write only costs the next session another attempt.
        let _ = cache_put(client, &file_request, &bytes).await;
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    use matrix_sdk::{
        media::MediaThumbnailSettings,
        ruma::{
            api::client::media::get_content_thumbnail::v3::Method, mxc_uri, uint, OwnedMxcUri,
        },
        test_utils::mocks::MatrixMockServer,
    };

    // ---- decision functions -------------------------------------------

    #[test]
    fn media_requests_are_a_single_attempt_with_a_timeout() {
        let config = bounded_request_config(RequestConfig::new(), Duration::from_secs(7));
        let dbg = format!("{config:?}");
        assert!(dbg.contains("retry_limit: 1"), "{dbg}");
        assert!(dbg.contains("7s"), "{dbg}");
        // The default this replaces has no retry limit at all.
        assert!(!format!("{:?}", RequestConfig::new()).contains("retry_limit"));
    }

    #[test]
    fn only_a_thumbnail_server_fault_falls_back_to_the_file() {
        // Server faults and a not-found thumbnail.
        assert!(avatar_fallback_eligible(true, Some(500), false));
        assert!(avatar_fallback_eligible(true, Some(502), false));
        assert!(avatar_fallback_eligible(true, Some(599), false));
        assert!(avatar_fallback_eligible(true, Some(404), true));
        // Not these.
        assert!(!avatar_fallback_eligible(true, Some(404), false)); // M_UNRECOGNIZED
        assert!(!avatar_fallback_eligible(true, Some(403), false));
        assert!(!avatar_fallback_eligible(true, Some(429), false));
        assert!(!avatar_fallback_eligible(true, Some(400), false));
        assert!(!avatar_fallback_eligible(true, None, false)); // no answer at all
        assert!(!avatar_fallback_eligible(true, Some(600), false));
        // A request that is already for the file has nothing to fall back to.
        assert!(!avatar_fallback_eligible(false, Some(500), false));
    }

    // ---- against a mock homeserver -------------------------------------

    const MXC: &str = "mxc://remote.example/AvatarId";

    fn thumbnail_request() -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(MXC)),
            format: MediaFormat::Thumbnail(MediaThumbnailSettings::with_method(
                Method::Scale,
                uint!(96),
                uint!(96),
            )),
        }
    }

    async fn authed_server() -> (MatrixMockServer, Client) {
        let server = MatrixMockServer::new().await;
        // The mock builder disables retries; Lightning's build_client sets no
        // request config, so put back the SDK default it actually runs with
        // (retry forever), or these tests could not see the defect.
        let client = server
            .client_builder()
            .no_server_versions()
            .on_builder(|builder| builder.request_config(RequestConfig::new()))
            .build()
            .await;
        server
            .mock_versions()
            .with_versions(vec!["v1.7", "v1.8", "v1.9", "v1.10"])
            .with_feature("org.matrix.msc3916.stable", true)
            .ok()
            .mount()
            .await;
        (server, client)
    }

    /// Hard ceiling on a test that must not wait the SDK's backoff out.
    const BOUND: Duration = Duration::from_secs(5);

    #[tokio::test]
    async fn a_500_thumbnail_is_asked_once_and_the_file_downloaded_once() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .error500()
            .expect(1)
            .named("thumbnail")
            .mount()
            .await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(1)
            .named("file")
            .mount()
            .await;

        let got = tokio::time::timeout(
            BOUND,
            get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT),
        )
        .await
        .expect("a 500 must not be retried until the outer timeout")
        .expect("the file download answers");
        assert_eq!(got, b"binaryjpegfullimagedata");
    }

    // The fallback's bytes belong to the FILE's cache key. Under the
    // thumbnail's key they would be served to every later thumbnail request,
    // full size, after the server had recovered.
    #[tokio::test]
    async fn a_fallback_does_not_poison_the_thumbnail_cache() {
        let (server, client) = authed_server().await;
        let broken = server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .error500()
            .named("thumbnail-broken")
            .mount_as_scoped()
            .await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(1) // the second fallback below is a cache hit
            .named("file")
            .mount()
            .await;

        let fallback =
            get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
                .await
                .unwrap();
        assert_eq!(fallback, b"binaryjpegfullimagedata");

        // Still broken: the thumbnail is asked again, the file comes from cache.
        let again = get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
            .await
            .unwrap();
        assert_eq!(again, fallback);

        // The server recovers: the thumbnail request is answered by the server,
        // not from a cached full-size file.
        drop(broken);
        server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .ok()
            .expect(1)
            .named("thumbnail-recovered")
            .mount()
            .await;
        let recovered =
            get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
                .await
                .unwrap();
        assert_eq!(recovered, b"binaryjpegthumbnaildata");
    }

    #[tokio::test]
    async fn a_500_thumbnail_and_a_500_file_fail_after_one_attempt_each() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .error500()
            .expect(1)
            .named("thumbnail")
            .mount()
            .await;
        server
            .mock_authed_media_download()
            .error500()
            .expect(1)
            .named("file")
            .mount()
            .await;

        let started = std::time::Instant::now();
        let err = tokio::time::timeout(
            BOUND,
            get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT),
        )
        .await
        .expect("a 500 must reach the caller in seconds, not at the outer timeout")
        .expect_err("both requests were refused");
        assert!(started.elapsed() < BOUND);
        assert_eq!(failure_status(&err), Some(500));
        assert_eq!(classify_media_error(&err), "server_error");
    }

    #[tokio::test]
    async fn a_200_thumbnail_is_cached_and_never_falls_back() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .ok()
            .expect(1)
            .named("thumbnail")
            .mount()
            .await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(0)
            .named("file")
            .mount()
            .await;

        let first = get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
            .await
            .unwrap();
        assert_eq!(first, b"binaryjpegthumbnaildata");
        let second = get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
            .await
            .unwrap();
        assert_eq!(second, first);
    }

    #[tokio::test]
    async fn an_unrecognized_thumbnail_endpoint_does_not_fall_back() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_thumbnail(Method::Scale, 96, 96, false)
            .error_unrecognized()
            .expect(1)
            .named("thumbnail")
            .mount()
            .await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(0)
            .named("file")
            .mount()
            .await;

        let err = get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
            .await
            .expect_err("M_UNRECOGNIZED is not a thumbnail fault");
        assert_eq!(failure_status(&err), Some(404));
        assert_eq!(classify_media_error(&err), "unrecognized");
    }

    #[tokio::test]
    async fn a_500_on_the_file_itself_is_one_request() {
        // media_fetch's shape: a File request, no fallback, one attempt.
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .error500()
            .expect(1)
            .named("file")
            .mount()
            .await;
        let request = MediaRequestParameters {
            source: MediaSource::Plain(mxc_uri!("mxc://remote.example/File").to_owned()),
            format: MediaFormat::File,
        };
        let err = tokio::time::timeout(
            BOUND,
            get_media_content_bounded(&client, &request, true, Duration::from_secs(40)),
        )
        .await
        .expect("bounded, not 40 s")
        .expect_err("refused");
        assert_eq!(classify_media_error(&err), "server_error");
    }
}
