#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QTemporaryDir>
#include <memory>

class MatrixClient;
class VideoPosterExtractor;

// v0.5.9: managed download half of the media pipeline for the Rust backend.
//
// QML asks for media by the timeline item's `mediaKey` (or an avatar's mxc
// URI); the bridge deduplicates requests, bounds concurrency, forwards to
// MatrixClient::fetchMedia / fetchMxcThumbnail (the SDK decrypts encrypted
// attachments internally), and keeps the resulting bytes in a bounded
// in-memory LRU cache shared with MediaImageProvider. Nothing is ever
// written to CacheStore or any other disk location except an explicit,
// user-chosen Save As destination. The cache is cleared on sign-out.
class MediaBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)

public:
    explicit MediaBridge(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    bool supported() const;

    // Returns the image-provider URL when the payload is already cached,
    // otherwise dispatches a fetch and returns an empty string; QML retries
    // from the mediaCached(cacheKey) signal. kind: "thumb" or "full".
    Q_INVOKABLE QString mediaSource(const QString &mediaKey, const QString &kind);
    // Avatar thumbnails by plain mxc URI. v0.7.1: every avatar identity is
    // fetched at ONE canonical server-side edge (kAvatarCanonicalEdge)
    // regardless of the requested render size, so the cache key —
    // "mxc:<edge>:<uri>" — is size-independent: the room list, room header,
    // timeline rows, popovers and rail all share a single fetch, a single
    // cache entry, and a single failure mark per identity. QML scales the
    // decoded bitmap down at render time.
    Q_INVOKABLE QString avatarSource(const QString &mxcUri, int size);
    // Non-avatar mxc image (link-preview thumbnails): caller-chosen edge,
    // main cache class — never charged to the avatar budget.
    Q_INVOKABLE QString mxcImageSource(const QString &mxcUri, int edge);
    // Provider URL for an already-cached key ("" when evicted meanwhile).
    Q_INVOKABLE QString cachedSource(const QString &cacheKey) const;
    // Confirmed GIFs use original SDK-fetched/decrypted bytes written
    // atomically beneath a short-lived account/session cache directory.
    Q_INVOKABLE QString animatedSource(const QString &mediaKey);
    // v0.7: inline video/audio playback. Same secure materialization
    // contract as animatedSource — SDK-fetched/decrypted bytes, validated
    // by container magic, written 0600 inside the session's 0700 temp dir
    // under an unguessable name, bounded by a separate LRU, wiped on
    // sign-out/account switch — but returns a file:// URL suitable for the
    // in-process QMediaPlayer ONLY. Paths must never reach external
    // applications. QML retries from playableMediaReady(cacheKey).
    Q_INVOKABLE QString playableSource(const QString &mediaKey);
    // v0.7 perf round: bounded speculative playable prefetch. Called for an
    // on-screen video cover so the payload is (usually) already
    // materialized when the user presses Play — the 5-8s press-to-playback
    // wait was pure download time. Only dispatches when the event's Matrix
    // metadata declares a size at or below the speculative cap (fail-safe:
    // unknown or large sizes are never prefetched), at the lowest priority
    // class, so it can never crowd out visible chrome or explicit intent.
    // Queued prefetches are dropped on room switch exactly like GIF
    // autoplay prefetches. sizeBytes comes from QML as a double.
    Q_INVOKABLE void prefetchPlayable(const QString &mediaKey,
                                      double sizeBytes);
    // v0.7 perf round: poster for a video WITHOUT a Matrix thumbnail.
    // Returns the provider URL when a poster is already cached under the
    // event's "thumb:" key; otherwise arranges one — extracting the first
    // frame of the already-materialized playable file, or (bounded by the
    // speculative cap) prefetching the payload first — and returns "".
    // QML retries from mediaCached("thumb:<mediaKey>"). The poster is
    // encoded JPEG in the ordinary in-RAM image cache: decrypted-media
    // derived pixels never touch disk.
    Q_INVOKABLE QString videoPosterSource(const QString &mediaKey,
                                          double sizeBytes);
    // v0.7 perf round: cancel the playable fetch for a card that no longer
    // wants it (closed mid-download, delegate reused, room left). Playable
    // interest is refcounted (two cards can share one fetch); when the
    // count reaches zero this frees the concurrency slot immediately and
    // aborts the backend download task, so an abandoned multi-hundred-MB
    // transfer stops consuming bandwidth and store access. No failure mark
    // is left — a fresh Play re-dispatches cleanly. ONLY an animated/GIF
    // consumer of the same bytes keeps the fetch alive; a pending poster
    // hook or speculative prefetch deliberately does NOT veto a user
    // cancel (review H1 — the poster is a derivative that can be
    // re-extracted whenever the file is next materialized).
    Q_INVOKABLE void cancelPlayable(const QString &mediaKey);
    // Container sniffing for the playable path: returns the file suffix
    // ("mp4", "webm", "ogg", …) when the payload's magic matches a
    // supported audio/video container, "" otherwise. Static + public for
    // the validation tests.
    static QString playableExtensionFor(const QByteArray &bytes,
                                        const QString &mimetype);
    Q_INVOKABLE QString previewAnimatedSource(const QString &dataSource,
                                              const QString &mimetype);
    // Validated client-preview bytes exposed only through the bounded
    // in-memory image provider. The remote URL is never an image source.
    Q_INVOKABLE QString previewImageSource(const QString &dataSource,
                                           const QString &mimetype);

    // v0.5.11: failure state. A failed fetch marks its cache key so QML
    // repolling cannot hammer the backend; retry() clears the mark so the
    // next mediaSource/avatarSource call dispatches again. failureCategory
    // returns the coarse category ("network", "rejected", ...) or "".
    //
    // v0.7: transient categories (network and similar) expire after a
    // bounded interval, so an avatar that failed once — e.g. during a flaky
    // startup — recovers on its own with at most one re-dispatch per
    // interval. Validation failures ("rejected", "invalid_gif") stay
    // permanent until an explicit retry().
    //
    // v0.7.1: expiry is ACTIVE, not merely passive: the watchdog tick sweeps
    // expired transient marks and emits mediaRetryable(cacheKey), so an
    // Avatar instantiated while its key was failure-marked recovers without
    // any user interaction. Anti-hammering is preserved — a mark re-arms its
    // retry window on every failed attempt, bounding retries to one
    // dispatch per interval per key.
    Q_INVOKABLE QString failureCategory(const QString &cacheKey) const;
    // Synchronous, non-expiring failure lookup by plain mxc URI (the
    // canonical avatar cache key is derived internally). Lets Avatar.qml
    // render honest initials instead of an eternal skeleton when
    // avatarSource() returns "" because the key is failure-marked.
    Q_INVOKABLE QString avatarFailureCategory(const QString &mxcUri) const;
    Q_INVOKABLE void retry(const QString &cacheKey);
    void setFailureRetryMsForTest(qint64 ms) { m_failureRetryMs = ms; }

    // v0.7.1: in-flight watchdog. A dispatched op that the backend never
    // completes (a dropped mediaReady/mediaFailed callback, a hung transport,
    // a request the Rust side silently discards) would otherwise pin its
    // concurrency slot forever. Once kMaxConcurrent such orphans accumulate,
    // pump() can never dispatch again and the whole media/avatar pipeline
    // stalls — the "images and avatars stop loading after a few minutes"
    // failure. The watchdog reclaims a slot whose op has exceeded its class
    // timeout, marks a transient failure (so QML shows a fallback now and
    // re-dispatches once the interval elapses), and pumps the queue. Runs on
    // the object's own event loop; also callable directly for deterministic
    // tests.
    Q_INVOKABLE void checkInflightTimeouts();
    void setInflightTimeoutMsForTest(qint64 ms) { m_inflightTimeoutMs = ms; }

    // Bounded, sanitized queue-health snapshot for diagnostics and the soak
    // test. Contains only counts, ages, and byte totals — never keys, URIs,
    // or bytes.
    Q_INVOKABLE QVariantMap healthSnapshot() const;
    // Slot-health accessors for tests: the soak proves in-flight returns to
    // zero and the queue fully drains under saturation.
    int inflightCountForTest() const { return m_inflight.size(); }
    int queuedCountForTest() const { return m_queue.size(); }
    void setStarvationMsForTest(qint64 ms) { m_starvationMs = ms; }
    void setPlayableCapsForTest(int entries, qint64 bytes)
    {
        m_playableMaxEntries = entries;
        m_playableMaxBytes = bytes;
    }

    // v0.7 media round: playable-file pinning. A QMediaPlayer holds its
    // materialized temp file open for the whole playback session; the LRU
    // evicting that file under it deletes what the decoder is reading
    // (survivable on Linux through the open fd, a hard failure on any
    // re-open or seek-after-source-reset). Cards pin on start and unpin on
    // reset/destruction; eviction skips pinned victims, temporarily
    // exceeding the cap when everything is pinned (bounded by the number of
    // live players). clear() drops all pins with the files.
    Q_INVOKABLE void pinPlayable(const QString &mediaKey);
    Q_INVOKABLE void unpinPlayable(const QString &mediaKey);

    // v0.7 media round: drops QUEUED speculative work (full-GIF autoplay
    // prefetch) that became irrelevant — called on room switch, where the
    // requesting delegates are destroyed. In-flight ops are untouched (the
    // backend has no cancellation; the watchdog and stale-drop already
    // bound them), and a revisit re-requests naturally. Entries a
    // playableSource() caller coalesced onto are kept (review L3). NOTE the
    // call-site ordering dependency: AppController calls this AFTER
    // stopAll() and BEFORE the new room's model attaches, all synchronously
    // — no consumer of a dropped entry survives to observe the silence.
    Q_INVOKABLE void dropQueuedSpeculative();

    // Explicit Save As: fetches the full payload (cache or network) and
    // writes it atomically to the user-chosen destination. Never executes
    // or opens the file. Result arrives via saveFinished().
    Q_INVOKABLE void saveAs(const QString &mediaKey, const QUrl &destination);

    // v0.6.6: "star a chat GIF" fetch trigger. Fetches the full payload
    // (cache or network, decrypted by the SDK exactly like every other
    // attachment) and hands the raw bytes back via mediaBytesForStar() —
    // this class stays media-generic and does no GIF-specific validation or
    // disk writing itself; see AppController::starChatGif for the caller
    // that relays the result into GifStarredStore. Mirrors saveAs()'s
    // dispatch/timeout class exactly (an explicit user export, same bound).
    Q_INVOKABLE void fetchFullForStar(const QString &mediaKey);

    // v0.6.6 fix: durable "is this GIF's content already starred" support
    // for GifStarredStore (see AppController::isChatGifStarred/
    // unstarChatGif, the only callers). Returns the SHA-256 hex digest of
    // whatever FULL payload is already sitting in the ordinary in-RAM
    // display cache for `mediaKey` — the exact bytes animatedSource()/
    // mediaSource() already fetched to show the row — or "" when nothing is
    // cached yet. Never dispatches a fetch and never returns raw bytes:
    // only a content hash crosses this boundary, so this stays safe to call
    // from the GIF-star path without handing decrypted media bytes to
    // another module.
    QString cachedFullContentHash(const QString &mediaKey) const;

    Q_INVOKABLE void clear();

    // Shared with MediaImageProvider (called from the QML render thread).
    QByteArray cachedBytes(const QString &cacheKey) const;

    // Cache caps; exposed for tests. Avatar-class entries ("mxc:" keys)
    // have their own reserved byte budget so churning timeline media can
    // never evict every avatar over a long session; both budgets are hard
    // bounds, so total memory stays bounded.
    void setCacheLimitBytes(qint64 bytes) { m_cacheLimit = bytes; }
    void setAvatarCacheLimitBytes(qint64 bytes) { m_avatarCacheLimit = bytes; }
    qint64 cacheBytesUsed() const;

Q_SIGNALS:
    void supportedChanged();
    void mediaCached(const QString &cacheKey);
    // v0.7.1: an expired TRANSIENT failure mark was swept by the watchdog;
    // consumers holding a fallback for this key may re-request it now
    // (bounded: one sweep emission per failure cycle, and a re-failed
    // attempt re-arms its window before the next emission).
    void mediaRetryable(const QString &cacheKey);
    void animatedMediaReady(const QString &cacheKey);
    // v0.7: a requested playable (video/audio) payload was validated and
    // materialized; QML re-calls playableSource(cacheKey) for the URL.
    void playableMediaReady(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    // mediaKey identifies WHICH save finished, so per-card save
    // feedback can never show another download's outcome.
    void saveFinished(bool ok, const QString &message,
                      const QString &mediaKey);
    // Result of fetchFullForStar(). `bytes` is empty and `category` is
    // non-empty on failure; category is "" on success. Never GIF-validated
    // here — that is GifStarredStore's job (see AppController::starChatGif).
    void mediaBytesForStar(const QString &mediaKey, bool ok,
                           const QByteArray &bytes, const QString &category);
    // v0.7: the poster extractor saw the video's real (display-oriented)
    // frame — its dimensions let the timeline card take the true shape on
    // every later render for events whose metadata declares none.
    // AppController persists them via SettingsManager. Dimensions only;
    // no pixels cross this signal.
    void videoDimensionsLearned(const QString &mediaKey, int width,
                                int height);

private Q_SLOTS:
    void onMediaReady(quint64 opId, const QString &mediaKey, int kind,
                      const QByteArray &bytes, const QString &mimetype,
                      const QString &filename);
    void onMediaFailed(quint64 opId, const QString &mediaKey, int kind,
                       const QString &category);
    void onLoggedOut();

private:
    struct Pending {
        QString cacheKey;
        bool isMxc = false;
        QString mediaKey; // or mxc uri
        int kind = 0;     // 0 full, 1 thumb (media), 2 mxc thumb
        int size = 0;     // mxc thumbnail edge
        bool saveRequest = false;
        QUrl saveDestination;
        // v0.6.6: same fetch shape as saveRequest (full payload, save-class
        // timeout) but the result is relayed raw via mediaBytesForStar()
        // instead of written to a user-chosen file.
        bool starRequest = false;
        // v0.7: backend timeout class (0 standard / 1 playable / 2 save).
        // The Rust timeout for each class sits strictly below the matching
        // C++ watchdog deadline, so Rust normally emits the terminal event
        // and the watchdog stays last-resort.
        int timeoutClass = 0;
        // v0.7 media round: request priority. Lower dispatches first.
        //   0 explicit user intent (press-play playable, Save As, star)
        //   1 interactive chrome (avatars, timeline thumbnails, mxc images)
        //   2 full static media (viewer, images without thumbnails)
        //   3 speculative (full-GIF prefetch for autoplay)
        // The old single FIFO let eight multi-megabyte GIF prefetches pin
        // every slot while the pressed-play FLAC and the room's avatars
        // waited behind them.
        int priority = 2;
        // Monotonic enqueue time for the starvation bound (see pump()).
        qint64 enqueuedAtMs = 0;
        // Monotonic dispatch time (m_failureClock ms) for resolution timing
        // in the logs; 0 until dispatched.
        qint64 dispatchedAtMs = 0;
    };

    void insertCache(const QString &cacheKey, const QByteArray &bytes);
    void touch(const QString &cacheKey) const;
    void markFailed(const Pending &request, const QString &category);
    // True while the key's failure mark still blocks a new dispatch;
    // expires transient marks as a side effect.
    bool failureBlocks(const QString &cacheKey);
    // Watchdog-driven active expiry: removes expired transient marks and
    // emits mediaRetryable for each, so QML recovers without repolling.
    void sweepExpiredFailureMarks();
    // Validation failures reported by the backend never fix themselves;
    // everything else (network, timeout, unavailable, …) is transient.
    static bool isPermanentCategory(const QString &category);
    // Review M3: every terminal outcome (failure, watchdog timeout,
    // dispatch failure) must void the interest sets for its key — leaked
    // entries both grew unboundedly and vetoed cancelPlayable forever.
    void dropInterestSets(const QString &cacheKey);
    static bool isAvatarClassKey(const QString &cacheKey);
    void dispatch(const Pending &request);
    void pump();
    bool alreadyPending(const QString &cacheKey) const;
    // review L4: when a caller coalesces onto an already-QUEUED entry,
    // raise that entry to the caller's class (lower priority value, wider
    // timeout class) in place.
    void promoteQueuedRequest(const QString &cacheKey, int priority,
                              int timeoutClass);
    // Heavy = priority >= 2 (full static media and speculative prefetch).
    // Bounded below kMaxConcurrent so interactive classes always have
    // reserved headroom.
    int heavyInflightCount() const;
    // Payload sniff for thumbnail-class results: a homeserver that cannot
    // thumbnail may return the ORIGINAL media, and the Rust bridge labels
    // thumbnail results with the parent's mimetype anyway — so the bytes,
    // not the label, decide whether the payload may enter the image path.
    static bool looksLikeAvContainer(const QByteArray &bytes);
    static QString sanitizedFileName(const QString &name);
    void writeSaveFile(const QUrl &destination, const QByteArray &bytes,
                       const QString &mediaKey);
    QString writeAnimatedFile(const QString &cacheKey, const QByteArray &bytes,
                              const QString &mimetype);
    QString writePlayableFile(const QString &cacheKey, const QByteArray &bytes,
                              const QString &mimetype);
    // Lazy poster machinery: constructed on the first poster request so
    // headless tests (and sessions that never show a thumbnail-less video)
    // never touch Qt Multimedia.
    void startPosterExtraction(const QString &mediaKey,
                               const QString &filePath);
    void onPosterReady(const QString &mediaKey, const QByteArray &jpeg);

    MatrixClient *m_client = nullptr;

    mutable QMutex m_cacheMutex;
    QHash<QString, QByteArray> m_cache;
    // Two LRU lists over the one byte store: avatar-class entries ("mxc:"
    // keys) are evicted only against their own reserved budget, so timeline
    // media churn cannot push avatars out over a long session (and vice
    // versa). front = most recent.
    mutable QList<QString> m_lru;
    mutable QList<QString> m_avatarLru;
    qint64 m_cacheLimit = 64 * 1024 * 1024;
    qint64 m_avatarCacheLimit = 8 * 1024 * 1024;
    // Running per-class byte totals (guarded by m_cacheMutex), maintained
    // by insertCache/clear so no path ever needs to iterate the whole
    // cache to know its size.
    qint64 m_cacheBytesMain = 0;
    qint64 m_cacheBytesAvatar = 0;
    // v0.7.1: per-key content revision, bumped ONLY on an actual byte
    // insert (insertCache) and appended to provider URLs as "?r=<n>".
    // A re-cached key therefore always yields a NEW source string, so a QML
    // Image stuck in Error (e.g. the cache-hit-then-evicted race) reloads;
    // cache hits keep an identical string so pixmap-cache dedup survives.
    // Guarded by m_cacheMutex; survives eviction, cleared with the cache.
    QHash<QString, quint32> m_revision;
    // v0.6.6 perf fix (review H1b): cachedFullContentHash() is queried from
    // up to eight different QML triggers per eligible GIF row (see the call
    // site's own comment), and a full SHA-256 over the payload is NOT cheap
    // — 146-149 MB/s measured on this Qt/OpenSSL build (~33ms for a 5MiB
    // GIF, ~430ms for the 64MiB cap) run on the UI thread inside a property
    // binding. Memoized per cache key so a given payload is ever hashed at
    // most once: invalidated whenever the key's m_revision changes (an
    // actual byte re-insert — see insertCache), when the key is evicted (the
    // LRU loop in insertCache), and on clear(). Guarded by m_cacheMutex,
    // exactly like m_cache/m_revision.
    struct ContentHashEntry { QString hex; quint32 revision = 0; };
    mutable QHash<QString, ContentHashEntry> m_contentHashCache;

    QHash<quint64, Pending> m_inflight;
    QQueue<Pending> m_queue;
    // v0.5.11: cache keys whose last fetch failed, with the coarse
    // category. Bounded; cleared on sign-out and per key via retry().
    // v0.7: transient marks also carry the monotonic time they were set so
    // they expire (see failureBlocks()).
    struct FailureMark {
        QString category;
        qint64 markedAtMs = 0;
    };
    QHash<QString, FailureMark> m_failed;
    QElapsedTimer m_failureClock;
    qint64 m_failureRetryMs = 60 * 1000;

    // v0.7.1: watchdog + sanitized diagnostics.
    QTimer m_watchdog;
    // Class timeouts. Thumbnails/avatars/full images are small; a genuine
    // fetch resolves in well under this. Only a truly stuck op reaches it.
    qint64 m_inflightTimeoutMs = 45 * 1000;
    qint64 m_saveTimeoutMs = 5 * 60 * 1000; // user Save As of a large file
    // Playable materialization: whole video/audio payloads are larger than
    // thumbnails but still interactive; above the 90s Rust bound, below
    // the save class.
    qint64 m_playableTimeoutMs = 100 * 1000;
    // All incremented on the object thread only (mediaSource/avatarSource,
    // onMediaReady/onMediaFailed, checkInflightTimeouts).
    qint64 m_statCompleted = 0;
    qint64 m_statFailed = 0;
    qint64 m_statTimedOut = 0;
    qint64 m_statDroppedStale = 0;
    qint64 m_statCacheHit = 0;
    qint64 m_statCacheMiss = 0;
    qint64 m_statCancelled = 0;
    // review H1b/M2: bumped when cachedFullContentHash() runs a SHA-256 on
    // a memo miss AND the payload survived the hash (the digest is only
    // counted once it is actually installed) — a timing-independent,
    // deterministic way for tests (and future diagnostics) to prove the
    // memoization eliminates repeat hashing, rather than asserting on
    // wall-clock cost. Written under m_cacheMutex like the memo table
    // itself; healthSnapshot() reads it unlocked, as it does the
    // neighbouring counters.
    mutable qint64 m_statContentHashComputed = 0;

    std::unique_ptr<QTemporaryDir> m_animatedDir;
    QHash<QString, QString> m_animatedFiles;
    QHash<QString, qint64> m_animatedSizes;
    QList<QString> m_animatedLru;
    QSet<QString> m_animatedWanted;
    // v0.7: playable (video/audio) materialization registry. Shares the
    // session temp dir with the animated path but has its own, larger LRU
    // budget so one video cannot evict every GIF (or vice versa). The
    // per-session random suffix keeps names unguessable even though the
    // directory itself is 0700.
    QHash<QString, QString> m_playableFiles;
    QHash<QString, qint64> m_playableSizes;
    QList<QString> m_playableLru;
    // REFCOUNTED playable interest (review M1): the same media event can be
    // rendered by two cards at once (main timeline + thread panel), and one
    // card's cancel must not strand the other mid-fetch. Only when the
    // count reaches zero may cancelPlayable abort the shared op. Failure
    // paths drop the whole entry — a retry re-expresses interest.
    QHash<QString, int> m_playableWanted;
    // v0.7 perf round: speculative playable interest ("full:" keys). Kept
    // separate from m_playableWanted so dropQueuedSpeculative can still
    // distinguish a real pressed-play consumer (kept) from a prefetch
    // (dropped on room switch).
    QSet<QString> m_prefetchWanted;
    // "full:" keys whose materialization should trigger a poster grab, and
    // media keys with an extraction currently queued/active.
    QSet<QString> m_posterWanted;
    QSet<QString> m_posterExtracting;
    VideoPosterExtractor *m_posterExtractor = nullptr;
    // Cache keys whose materialized file a live player currently holds
    // open, REFCOUNTED (review L1): the same media event can be rendered by
    // two cards at once (main timeline + thread panel), and one card's
    // reset must not unpin the file the other still holds. Never chosen as
    // an eviction victim while the count is positive.
    QHash<QString, int> m_pinnedPlayables;
    QString m_playableNameSalt;
    // Playable LRU caps as members so tests can shrink them; initialized
    // from the class constants below.
    int m_playableMaxEntries;
    qint64 m_playableMaxBytes;
    // Bounded-starvation guard for the priority queue: an entry older than
    // this dispatches ahead of higher-priority newcomers.
    qint64 m_starvationMs = 15 * 1000;
    // v0.7: raised from 4 — a cold room list fetches its visible avatars in
    // one or two bursts instead of a long 4-at-a-time trickle. Still a hard
    // bound; excess requests queue and pump as fetches complete.
    static constexpr int kMaxConcurrent = 8;
    // Heavy work (priority >= 2: full static media, speculative prefetch)
    // may hold at most this many slots, so explicit playback and visible
    // chrome always find headroom immediately — eight multi-megabyte GIF
    // prefetches can no longer starve the room's avatars or a pressed-play
    // track.
    static constexpr int kMaxHeavyConcurrent = 6;
    static constexpr int kMaxFailureMarks = 512;
    // One server-side thumbnail edge for every avatar surface (largest
    // consumer is the 96px popover at 2x DPR = 192; 224 covers it with
    // headroom). All render sizes downscale from this single decode.
    static constexpr int kAvatarCanonicalEdge = 224;
    static constexpr qint64 kAnimatedCacheBytes = 64 * 1024 * 1024;
    static constexpr int kAnimatedCacheEntries = 64;
    static constexpr qint64 kPlayableCacheBytes = 256 * 1024 * 1024;
    static constexpr int kPlayableCacheEntries = 16;
    // Full-size media above this skips the RAM LRU (it exists on disk for
    // the player; caching it in memory would evict every image at once).
    static constexpr qint64 kLargeCacheSkipBytes = 8 * 1024 * 1024;
    // Speculative playable prefetch cap: a video/audio payload whose Matrix
    // metadata declares more than this is only fetched on explicit Play.
    static constexpr qint64 kSpeculativePlayableMaxBytes = 32 * 1024 * 1024;
};
