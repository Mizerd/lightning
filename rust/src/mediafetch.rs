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

/// Downloads in flight, one shared future per (account, source, format).
///
/// A "thumb" request (kind 1) for an item that carries no sender thumbnail and
/// a "full" request (kind 0) for the same item are two ops with two MediaBridge
/// cache keys, and both resolve to `MediaFormat::File` of one source. Live
/// check 2026-10-01: a 426.8 KB image was downloaded twice, 230 ms apart.
/// Both ops now await the SAME future, so the homeserver is asked once and
/// each op still gets its own terminal event from its own caller.
///
/// The future is driven by whichever waiter polls it, so an op aborted while
/// another still waits does not strand that other op (`Shared`); with no
/// waiter left it is dropped, as an unshared fetch always was. Entries are
/// weak, so one that finished and has no holder is simply not found, and a
/// request arriving after the fetch is over starts a fresh one (and meets the
/// store's cache entry).
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

    /// The shared future for `key`: the one already in flight, or a new one
    /// built from `start`. `start` is not called when one is in flight.
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
        // Forget what finished, so the map holds only what is running.
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

/// `get_media_content_bounded`, with a second request for the same account,
/// source, format, cache use and timeout (`coalesce_key`) joining the download already running for the first
/// instead of starting its own. The bytes are returned owned: a caller that
/// was not the only waiter copies them once. The error is shared, so every
/// waiter classifies the one real failure.
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

/// Read `request` from the media store, without any request.
async fn cache_get(
    client: &Client,
    request: &MediaRequestParameters,
) -> matrix_sdk::Result<Option<Vec<u8>>> {
    Ok(client.media_store().lock().await?.get_media_content(request).await?)
}

/// Write `content` under `request` in the media store, as the SDK does after
/// a download. Nothing over the plaintext cap: the SDK's own limit is on the
/// encoded row (rooms::MEDIA_STORE_ENCODED_MAX_BYTES), so the cap the rest of
/// Lightning reasons in, and the kept-file path begins above, is held here.
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

/// Whether a downloaded payload of `len` bytes goes to the SDK media store.
fn fits_store(len: usize) -> bool {
    len as u64 <= crate::rooms::MEDIA_STORE_MAX_FILE_BYTES
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

    // The plaintext cap is Lightning's own gate, because the SDK limit is on
    // the encoded row: nothing over it is written, everything at it is.
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

    // The gate is applied by cache_put itself, not left to the SDK policy:
    // with a store that would take anything, a payload one byte over the cap
    // is still not written, and one under it is.
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
    // ---- one download for one source ------------------------------------

    fn file_request(id: &str) -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(format!("mxc://remote.example/{id}"))),
            format: MediaFormat::File,
        }
    }

    // The coalescing decision itself, without a network.
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
        // The joiner gets the first future's answer, not a second computation.
        assert_eq!(futures_util::FutureExt::now_or_never(second), Some(1));
        assert_eq!(futures_util::FutureExt::now_or_never(other), Some(3));
        // Nobody holds "a" now: a later request starts afresh.
        let _third = coalescer.join("a".into(), || start(4));
        assert_eq!(started.load(std::sync::atomic::Ordering::SeqCst), 3);
    }

    // The reported case: a thumb request and a full request for an item with no
    // sender thumbnail both resolve to the File of one source (media_fetch).
    // Two ops, one HTTP request. `expect(1)` fails the test on the old path,
    // which asked twice.
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
        // Each op still gets its own complete answer.
        assert_eq!(thumb.expect("thumb op"), b"binaryjpegfullimagedata");
        assert_eq!(full.expect("full op"), b"binaryjpegfullimagedata");
    }

    // Different sources are different downloads.
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

    // A failure is the one real failure, classified the same by every waiter.
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

    // Nothing is remembered once a fetch is over and unheld: a later request is
    // a new download (no store here), not a stale answer.
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

    // The review's findings, as key decisions. The SDK's own unique_key is the
    // bare url for both variants, which is the premise of the first two.
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
        // The same file is the same key, or nothing could coalesce.
        assert_eq!(
            coalesce_key("@a:x", &one, true, T),
            coalesce_key("@a:x", &encrypted_request("mxc://remote.example/Same", K1), true, T)
        );
        // The key is a digest, never the key material.
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
