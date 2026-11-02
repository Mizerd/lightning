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

use std::sync::Arc;
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

use crate::mediahealth;

/// Attempts per HTTP request. matrix-sdk counts `retry_limit` as attempts
/// (`native.rs:73-79`: `with_max_times(limit - 1)`), so 1 is a single request
/// and no retry. A failed media fetch is retried later by MediaBridge's
/// transient failure mark; retrying here would hold one of its eight slots,
/// and a 429's `retry_after` can be minutes.
pub(crate) const MEDIA_ATTEMPTS: usize = 1;

pub(crate) const AVATAR_REQUEST_TIMEOUT: Duration = Duration::from_secs(15);

pub(crate) const AVATAR_FALLBACK_MAX_BYTES: u64 = 8 * 1024 * 1024;

pub(crate) fn bounded_request_config(base: RequestConfig, timeout: Duration) -> RequestConfig {
    base.retry_limit(MEDIA_ATTEMPTS).timeout(Some(timeout))
}

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

pub(crate) fn failure_status(err: &matrix_sdk::Error) -> Option<u16> {
    err.as_client_api_error().map(|e| e.status_code.as_u16())
}

fn failure_is_not_found(err: &matrix_sdk::Error) -> bool {
    matches!(err.client_api_error_kind(), Some(ErrorKind::NotFound))
}

pub(crate) fn classify_media_error(err: &matrix_sdk::Error) -> &'static str {
    if breaker_retry_after_ms(err).is_some() {
        return "server_unreachable";
    }
    match failure_status(err) {
        Some(code) if (500..600).contains(&code) => "server_error",
        Some(429) => "rate_limited",
        Some(403) => "forbidden",
        Some(404) if matches!(err.client_api_error_kind(), Some(ErrorKind::Unrecognized)) => {
            "unrecognized"
        }
        Some(404) => "not_found",
        _ => crate::rooms::classify_room_error(&err.to_string()),
    }
}

pub(crate) fn breaker_retry_after_ms(err: &matrix_sdk::Error) -> Option<u64> {
    match err {
        matrix_sdk::Error::UnknownError(inner) => inner
            .downcast_ref::<mediahealth::BreakerOpen>()
            .map(|open| open.retry_after_ms),
        _ => None,
    }
}

pub(crate) fn failure_category(err: &matrix_sdk::Error) -> String {
    match breaker_retry_after_ms(err) {
        Some(ms) => format!("server_unreachable:{ms}"),
        None => classify_media_error(err).to_owned(),
    }
}

pub(crate) async fn get_media_content_bounded(
    client: &Client,
    request: &MediaRequestParameters,
    use_cache: bool,
    timeout: Duration,
) -> matrix_sdk::Result<Vec<u8>> {
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

pub(crate) struct Coalescer<T: Clone> {
    inflight: std::sync::Mutex<
        std::collections::HashMap<String, futures_util::future::WeakShared<BoxFuture<T>>>,
    >,
}

type BoxFuture<T> = futures_util::future::BoxFuture<'static, T>;

impl<T: Clone> Coalescer<T> {
    pub(crate) fn new() -> Self {
        Self { inflight: std::sync::Mutex::new(std::collections::HashMap::new()) }
    }

    pub(crate) fn join(
        &self,
        key: String,
        start: impl FnOnce() -> BoxFuture<T>,
    ) -> futures_util::future::Shared<BoxFuture<T>> {
        use futures_util::FutureExt;
        let mut map = match self.inflight.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        if let Some(live) = map.get(&key).and_then(|weak| weak.upgrade()) {
            return live;
        }
        map.retain(|_, weak| weak.upgrade().is_some());
        let shared = start().shared();
        if let Some(weak) = shared.downgrade() {
            map.insert(key, weak);
        }
        shared
    }
}

type SharedBytes = Result<Arc<Vec<u8>>, Arc<matrix_sdk::Error>>;

fn media_coalescer() -> &'static Coalescer<SharedBytes> {
    static COALESCER: once_cell::sync::Lazy<Coalescer<SharedBytes>> =
        once_cell::sync::Lazy::new(Coalescer::new);
    &COALESCER
}

/// What two requests must share to be ONE download. Not `request.unique_key()`
/// alone: matrix-sdk-base's `Encrypted::unique_key()` is just the file's url,
/// the very string `Plain(uri)` gives, so that key would let
///   * a Plain op join an Encrypted fetch and receive DECRYPTED bytes (which
///     the caller may then keep as a plain file, CLAUDE.md §6);
///   * two Encrypted sources with one url and different key/iv/hashes (a
///     hostile sender can write that) share a future and its plaintext;
///   * ops with different `use_cache` or timeout have the cache write and the
///     per-request bound decided by whichever started first.
/// So the key carries the source variant, for an Encrypted source a SHA-256
/// digest of everything that decides its plaintext (url, key, iv, hashes,
/// version; the digest is never logged), the format, `use_cache` and the
/// timeout in milliseconds.
fn coalesce_key(
    account: &str,
    request: &MediaRequestParameters,
    use_cache: bool,
    timeout: Duration,
) -> String {
    use matrix_sdk_base::media::UniqueKey;
    use sha2::{Digest, Sha256};
    use std::sync::atomic::{AtomicU64, Ordering};
    static UNSHARED: AtomicU64 = AtomicU64::new(0);
    let source = match &request.source {
        MediaSource::Plain(uri) => format!("plain:{uri}"),
        MediaSource::Encrypted(file) => match serde_json::to_vec(file.as_ref()) {
            Ok(json) => {
                let digest = Sha256::digest(&json);
                let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
                format!("enc:{hex}")
            }
            // Cannot say what decides the plaintext: never share it.
            Err(_) => format!("enc-unshared:{}", UNSHARED.fetch_add(1, Ordering::Relaxed)),
        },
    };
    format!(
        "{account}\u{1f}{source}\u{1f}{}\u{1f}{use_cache}\u{1f}{}",
        request.format.unique_key(),
        timeout.as_millis()
    )
}

pub(crate) async fn get_media_content_shared(
    client: &Client,
    request: &MediaRequestParameters,
    use_cache: bool,
    timeout: Duration,
) -> Result<Vec<u8>, Arc<matrix_sdk::Error>> {
    let key = coalesce_key(client.user_id().map(|u| u.as_str()).unwrap_or(""), request, use_cache, timeout);
    let (client, request) = (client.clone(), request.clone());
    let shared = media_coalescer().join(key, move || {
        Box::pin(async move {
            get_media_content_bounded(&client, &request, use_cache, timeout)
                .await
                .map(Arc::new)
                .map_err(Arc::new)
        })
    });
    shared
        .await
        .map(|bytes| Arc::try_unwrap(bytes).unwrap_or_else(|shared| (*shared).clone()))
}

async fn cache_get(
    client: &Client,
    request: &MediaRequestParameters,
) -> matrix_sdk::Result<Option<Vec<u8>>> {
    Ok(client.media_store().lock().await?.get_media_content(request).await?)
}

async fn cache_put(
    client: &Client,
    request: &MediaRequestParameters,
    content: &[u8],
) -> matrix_sdk::Result<()> {
    if !fits_store(content.len()) {
        return Ok(());
    }
    client
        .media_store()
        .lock()
        .await?
        .add_media_content(request, content.to_vec(), IgnoreMediaRetentionPolicy::No)
        .await?;
    Ok(())
}

fn fits_store(len: usize) -> bool {
    len as u64 <= crate::rooms::MEDIA_STORE_MAX_FILE_BYTES
}

fn is_local_uri(source: &MediaSource) -> bool {
    const LOCAL_MXC_SERVER_NAME: &str = "send-queue.localhost";
    let uri = match source {
        MediaSource::Plain(uri) => uri,
        MediaSource::Encrypted(file) => &file.url,
    };
    uri.server_name().is_ok_and(|name| name == LOCAL_MXC_SERVER_NAME)
}

async fn fetch_uncached(
    client: &Client,
    request: &MediaRequestParameters,
    timeout: Duration,
) -> matrix_sdk::Result<Vec<u8>> {
    fetch_guarded(mediahealth::global(), request, || fetch_network(client, request, timeout)).await
}

async fn fetch_guarded<F, Fut>(
    health: &mediahealth::MediaHealth,
    request: &MediaRequestParameters,
    send: F,
) -> matrix_sdk::Result<Vec<u8>>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = matrix_sdk::Result<Vec<u8>>>,
{
    let uri = match &request.source {
        MediaSource::Plain(uri) => uri,
        MediaSource::Encrypted(file) => &file.url,
    };
    let identity = mediahealth::identify(uri);
    let (origin, file) = match &identity {
        Some((origin, file)) => (Some(origin.as_str()), file.as_str()),
        None => return send().await,
    };
    if let Err(wait) = health.check(origin, file, std::time::Instant::now()) {
        let retry_after_ms = u64::try_from(wait.as_millis()).unwrap_or(u64::MAX).max(1);
        return Err(matrix_sdk::Error::UnknownError(Box::new(
            mediahealth::BreakerOpen { retry_after_ms },
        )));
    }
    let outcome = send().await;
    match &outcome {
        Ok(_) => health.record_success(origin, file),
        Err(err) if speaks_for_server_health(err) => {
            health.record_failure(origin, file, std::time::Instant::now())
        }
        Err(_) => {}
    }
    outcome
}

pub(crate) fn speaks_for_server_health(err: &matrix_sdk::Error) -> bool {
    match failure_status(err) {
        Some(code) => (500..600).contains(&code),
        None => matches!(err, matrix_sdk::Error::Http(_)),
    }
}

async fn fetch_network(
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
        Err(_) => return Err(err),
    };
    if bytes.len() as u64 > AVATAR_FALLBACK_MAX_BYTES {
        return Err(err);
    }
    if use_cache {
        let _ = cache_put(client, &file_request, &bytes).await;
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_store_gate_is_the_plaintext_cap() {
        let cap = crate::rooms::MEDIA_STORE_MAX_FILE_BYTES as usize;
        assert!(fits_store(cap));
        assert!(fits_store(0));
        assert!(!fits_store(cap + 1));
    }

    use matrix_sdk::{
        media::MediaThumbnailSettings,
        ruma::{
            api::client::media::get_content_thumbnail::v3::Method, mxc_uri, uint, OwnedMxcUri,
        },
        test_utils::mocks::MatrixMockServer,
    };

    #[test]
    fn media_requests_are_a_single_attempt_with_a_timeout() {
        let config = bounded_request_config(RequestConfig::new(), Duration::from_secs(7));
        let dbg = format!("{config:?}");
        assert!(dbg.contains("retry_limit: 1"), "{dbg}");
        assert!(dbg.contains("7s"), "{dbg}");
        assert!(!format!("{:?}", RequestConfig::new()).contains("retry_limit"));
    }

    #[test]
    fn only_a_thumbnail_server_fault_falls_back_to_the_file() {
        assert!(avatar_fallback_eligible(true, Some(500), false));
        assert!(avatar_fallback_eligible(true, Some(502), false));
        assert!(avatar_fallback_eligible(true, Some(599), false));
        assert!(avatar_fallback_eligible(true, Some(404), true));
        assert!(!avatar_fallback_eligible(true, Some(404), false));
        assert!(!avatar_fallback_eligible(true, Some(403), false));
        assert!(!avatar_fallback_eligible(true, Some(429), false));
        assert!(!avatar_fallback_eligible(true, Some(400), false));
        assert!(!avatar_fallback_eligible(true, None, false));
        assert!(!avatar_fallback_eligible(true, Some(600), false));
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

    #[tokio::test]
    async fn cache_put_writes_nothing_over_the_plaintext_cap() {
        let (_server, client) = authed_server().await;
        client
            .media()
            .set_media_retention_policy(matrix_sdk::media::MediaRetentionPolicy::empty())
            .await
            .unwrap();
        let file = |id: &str| MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(format!("mxc://remote.example/{id}"))),
            format: MediaFormat::File,
        };
        let cap = crate::rooms::MEDIA_STORE_MAX_FILE_BYTES as usize;
        cache_put(&client, &file("Over"), &vec![7u8; cap + 1]).await.unwrap();
        assert!(cache_get(&client, &file("Over")).await.unwrap().is_none(), "over the cap was kept");
        cache_put(&client, &file("Under"), b"small").await.unwrap();
        assert_eq!(cache_get(&client, &file("Under")).await.unwrap().as_deref(), Some(&b"small"[..]));
    }

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
            .expect(1)
            .named("file")
            .mount()
            .await;

        let fallback =
            get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
                .await
                .unwrap();
        assert_eq!(fallback, b"binaryjpegfullimagedata");

        let again = get_avatar_bounded(&client, &thumbnail_request(), true, AVATAR_REQUEST_TIMEOUT)
            .await
            .unwrap();
        assert_eq!(again, fallback);

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

    async fn guarded(
        health: &mediahealth::MediaHealth,
        client: &Client,
        id: &str,
    ) -> matrix_sdk::Result<Vec<u8>> {
        let request = file_request(id);
        tokio::time::timeout(
            BOUND,
            fetch_guarded(health, &request, || {
                fetch_network(client, &request, Duration::from_secs(40))
            }),
        )
        .await
        .expect("bounded")
    }

    #[tokio::test]
    async fn a_dead_origin_opens_after_three_files_and_the_fourth_never_reaches_it() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .error500()
            .expect(3)
            .named("dead origin")
            .mount()
            .await;
        let health = mediahealth::MediaHealth::default();
        for id in ["A", "B", "C"] {
            let err = guarded(&health, &client, id).await.expect_err("500");
            assert_eq!(classify_media_error(&err), "server_error");
        }
        for id in ["D", "E", "A"] {
            let started = std::time::Instant::now();
            let err = guarded(&health, &client, id).await.expect_err("refused");
            assert!(started.elapsed() < Duration::from_millis(500), "fast, not queued");
            assert_eq!(classify_media_error(&err), "server_unreachable");
            let category = failure_category(&err);
            let ms: u64 = category
                .strip_prefix("server_unreachable:")
                .expect("carries the wait")
                .parse()
                .expect("milliseconds");
            assert!(ms > 0 && ms <= 60_000, "first backoff is one minute, got {ms}");
        }
    }

    #[tokio::test]
    async fn not_found_never_opens_the_breaker() {
        let (server, client) = authed_server().await;
        // Unmocked endpoints answer 404 (the mock server's default).
        let health = mediahealth::MediaHealth::default();
        for id in ["A", "B", "C", "D", "E", "F"] {
            let err = guarded(&health, &client, id).await.expect_err("404");
            assert_ne!(classify_media_error(&err), "server_unreachable", "{id} was refused");
        }
        let reached = server
            .server()
            .received_requests()
            .await
            .expect("recording")
            .iter()
            .filter(|r| r.url.path().contains("/media/download/"))
            .count();
        assert_eq!(reached, 6, "every 404 reached the server; none was refused");
    }

    #[tokio::test]
    async fn a_success_after_a_failure_resets_the_per_file_count() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .ok_bytes(b"bytes".to_vec())
            .expect(1)
            .mount()
            .await;
        let health = mediahealth::MediaHealth::default();
        let uri = "mxc://remote.example/A";
        let now = std::time::Instant::now();
        health.record_failure(Some("remote.example"), uri, now);
        health.record_failure(Some("remote.example"), uri, now);
        assert!(guarded(&health, &client, "A").await.is_ok());
        health.record_failure(Some("remote.example"), uri, now);
        assert!(health.check(Some("remote.example"), uri, now).is_ok(), "count restarted");
    }

    fn file_request(id: &str) -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(format!("mxc://remote.example/{id}"))),
            format: MediaFormat::File,
        }
    }

    #[test]
    fn the_coalescer_starts_one_future_per_key_while_one_is_held() {
        use futures_util::FutureExt;
        let coalescer: Coalescer<u32> = Coalescer::new();
        let started = std::sync::atomic::AtomicUsize::new(0);
        let start = |value: u32| {
            started.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            async move { value }.boxed()
        };
        let first = coalescer.join("a".into(), || start(1));
        let second = coalescer.join("a".into(), || start(2));
        let other = coalescer.join("b".into(), || start(3));
        assert_eq!(started.load(std::sync::atomic::Ordering::SeqCst), 2, "a joined a, b started");
        assert_eq!(futures_util::FutureExt::now_or_never(first), Some(1));
        assert_eq!(futures_util::FutureExt::now_or_never(second), Some(1));
        assert_eq!(futures_util::FutureExt::now_or_never(other), Some(3));
        let _third = coalescer.join("a".into(), || start(4));
        assert_eq!(started.load(std::sync::atomic::Ordering::SeqCst), 3);
    }

    #[tokio::test]
    async fn a_thumb_and_a_full_request_for_one_source_download_once() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(1)
            .named("file")
            .mount()
            .await;
        let (thumb_req, full_req) = (file_request("Same"), file_request("Same"));
        let thumb_op = get_media_content_shared(&client, &thumb_req, false, BOUND);
        let full_op = get_media_content_shared(&client, &full_req, false, BOUND);
        let (thumb, full) = tokio::time::timeout(BOUND, async { tokio::join!(thumb_op, full_op) })
            .await
            .expect("bounded");
        assert_eq!(thumb.expect("thumb op"), b"binaryjpegfullimagedata");
        assert_eq!(full.expect("full op"), b"binaryjpegfullimagedata");
    }

    #[tokio::test]
    async fn two_sources_are_not_coalesced() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(2)
            .named("files")
            .mount()
            .await;
        let (one, two) = (file_request("One"), file_request("Two"));
        let (a, b) = tokio::time::timeout(BOUND, async {
            tokio::join!(
                get_media_content_shared(&client, &one, false, BOUND),
                get_media_content_shared(&client, &two, false, BOUND),
            )
        })
        .await
        .expect("bounded");
        assert!(a.is_ok() && b.is_ok());
    }

    #[tokio::test]
    async fn a_shared_failure_reaches_every_waiter_classified() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .error500()
            .expect(1)
            .named("file")
            .mount()
            .await;
        let broken = file_request("Broken");
        let (a, b) = tokio::time::timeout(BOUND, async {
            tokio::join!(
                get_media_content_shared(&client, &broken, false, BOUND),
                get_media_content_shared(&client, &broken, false, BOUND),
            )
        })
        .await
        .expect("bounded");
        assert_eq!(classify_media_error(&a.expect_err("a")), "server_error");
        assert_eq!(classify_media_error(&b.expect_err("b")), "server_error");
    }

    #[tokio::test]
    async fn a_finished_download_is_not_reused_by_a_later_request() {
        let (server, client) = authed_server().await;
        server
            .mock_authed_media_download()
            .ok_image()
            .expect(2)
            .named("file")
            .mount()
            .await;
        for _ in 0..2 {
            get_media_content_shared(&client, &file_request("Later"), false, BOUND)
                .await
                .expect("answers");
        }
    }

    fn encrypted_request(url: &str, key_k: &str) -> MediaRequestParameters {
        let file: matrix_sdk::ruma::events::room::EncryptedFile = serde_json::from_value(
            serde_json::json!({
                "url": url,
                "key": {
                    "kty": "oct", "key_ops": ["encrypt", "decrypt"],
                    "alg": "A256CTR", "k": key_k, "ext": true
                },
                "iv": "AAAAAAAAAAAAAAAAAAAAAA",
                "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
                "v": "v2"
            }),
        )
        .expect("a valid encrypted file");
        MediaRequestParameters {
            source: MediaSource::Encrypted(Box::new(file)),
            format: MediaFormat::File,
        }
    }

    const K1: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const K2: &str = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    const T: Duration = Duration::from_secs(40);

    #[test]
    fn plain_and_encrypted_on_one_url_do_not_share_a_key() {
        use matrix_sdk_base::media::UniqueKey;
        let plain = file_request("Same");
        let enc = encrypted_request("mxc://remote.example/Same", K1);
        assert_eq!(plain.source.unique_key(), enc.source.unique_key(), "premise: the SDK key collides");
        assert_ne!(coalesce_key("@a:x", &plain, true, T), coalesce_key("@a:x", &enc, true, T));
    }

    #[test]
    fn two_encrypted_sources_with_different_keys_do_not_share_a_key() {
        let one = encrypted_request("mxc://remote.example/Same", K1);
        let two = encrypted_request("mxc://remote.example/Same", K2);
        assert_ne!(coalesce_key("@a:x", &one, true, T), coalesce_key("@a:x", &two, true, T));
        assert_eq!(
            coalesce_key("@a:x", &one, true, T),
            coalesce_key("@a:x", &encrypted_request("mxc://remote.example/Same", K1), true, T)
        );
        assert!(!coalesce_key("@a:x", &one, true, T).contains(K1));
    }

    #[test]
    fn cache_use_timeout_account_and_format_each_split_the_key() {
        let req = file_request("Same");
        let base = coalesce_key("@a:x", &req, true, T);
        assert_ne!(base, coalesce_key("@a:x", &req, false, T));
        assert_ne!(base, coalesce_key("@a:x", &req, true, Duration::from_secs(20)));
        assert_ne!(base, coalesce_key("@b:x", &req, true, T));
        assert_ne!(base, coalesce_key("@a:x", &thumbnail_request(), true, T));
        assert_eq!(base, coalesce_key("@a:x", &file_request("Same"), true, T));
    }

    // Through the real entry point: a Plain and an Encrypted request on one url
    // are two downloads (the encrypted one then fails to decrypt the mock's
    // bytes, which is irrelevant here; the plain one must not receive it), and
    // two cache uses of one source are two as well.
    #[tokio::test]
    async fn plain_and_encrypted_on_one_url_download_twice() {
        let (server, client) = authed_server().await;
        server.mock_authed_media_download().ok_image().expect(2).named("file").mount().await;
        let plain = file_request("PlainEnc");
        let enc = encrypted_request("mxc://remote.example/PlainEnc", K1);
        let (p, e) = tokio::time::timeout(BOUND, async {
            tokio::join!(
                get_media_content_shared(&client, &plain, false, BOUND),
                get_media_content_shared(&client, &enc, false, BOUND),
            )
        })
        .await
        .expect("bounded");
        assert_eq!(p.expect("plain op"), b"binaryjpegfullimagedata", "the plain op got its own bytes");
        assert!(e.is_err(), "the mock's bytes are not a valid ciphertext for the encrypted op");
    }

    #[tokio::test]
    async fn the_same_source_with_different_cache_use_downloads_twice() {
        let (server, client) = authed_server().await;
        server.mock_authed_media_download().ok_image().expect(2).named("file").mount().await;
        let req = file_request("CacheUse");
        let (a, b) = tokio::time::timeout(BOUND, async {
            tokio::join!(
                get_media_content_shared(&client, &req, true, BOUND),
                get_media_content_shared(&client, &req, false, BOUND),
            )
        })
        .await
        .expect("bounded");
        assert!(a.is_ok() && b.is_ok());
    }
}
