#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
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
class PlayableFileWriter;
class VideoPosterExtractor;

// Managed media download pipeline for the Rust backend.
//
// QML asks for media by the timeline item's `mediaKey` (or an avatar's mxc
// URI). The bridge deduplicates requests, bounds concurrency, forwards to
// MatrixClient (the SDK decrypts encrypted attachments) and keeps the bytes in
// a bounded in-memory LRU shared with MediaImageProvider. Cleared on sign-out.
//
// What reaches disk:
//   * an explicit Save As destination and the user's starred-GIF store;
//   * playable and animated payloads, as 0600 files in a 0700 scratch
//     directory so the player can map them. Removed by clear() and on
//     destruction, but a crash leaves them behind;
//   * the SDK media store, for unencrypted rooms only. `media_fetch` in
//     rust/src/rooms.rs passes use_cache=false for encrypted sources because
//     that store has no cipher and would hold decrypted bytes.
class MediaBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)

public:
    explicit MediaBridge(QObject *parent = nullptr);
    // Releases the scratch directory's lock before the directory is removed;
    // otherwise Qt fails to remove its own lock file at exit.
    ~MediaBridge() override;

    void setClient(MatrixClient *client);
    bool supported() const;

    // Returns the image-provider URL when the payload is already cached,
    // otherwise dispatches a fetch and returns an empty string; QML retries
    // from the mediaCached(cacheKey) signal. kind: "thumb" or "full".
    Q_INVOKABLE QString mediaSource(const QString &mediaKey, const QString &kind);
    // Avatar thumbnails by mxc URI. Every avatar is fetched at one server-side
    // edge (kAvatarCanonicalEdge), so the cache key is size-independent and all
    // surfaces share one fetch, one entry and one failure mark. QML downscales.
    Q_INVOKABLE QString avatarSource(const QString &mxcUri, int size);
    // A wide image by bare mxc URI (profile banners). avatarSource() would ask
    // for a small square thumbnail. Returns "" on a miss and dispatches; re-ask
    // on mediaCached.
    Q_INVOKABLE QString wideImageSource(const QString &mxcUri);
    // NotificationManager's in-process read of an already-cached canonical
    // avatar. It never dispatches and never exposes bytes to QML/disk.
    QImage cachedAvatarImage(const QString &mxcUri) const;
    // Non-avatar mxc image (link-preview thumbnails): caller-chosen edge,
    // main cache class — never charged to the avatar budget.
    Q_INVOKABLE QString mxcImageSource(const QString &mxcUri, int edge);
    // Provider URL for an already-cached key ("" when evicted meanwhile).
    Q_INVOKABLE QString cachedSource(const QString &cacheKey) const;
    // Animatable media: SDK-fetched bytes written to the session scratch dir.
    // `animatedExtensionFor` decides from the magic; the declared mimetype is
    // never trusted.
    //
    // `speculative` is for stickers, whose mimetype is optional (MSC2545) and
    // which already draw the bytes as a still image: "not an animation" is then
    // silent instead of marking the key failed with "invalid_gif".
    Q_INVOKABLE QString animatedSource(const QString &mediaKey,
                                       bool speculative = false);
    // The same materialization for a bare mxc URI (sticker picker tiles, which
    // have no media key; mxcImageSource returns a single-frame server
    // thumbnail). Always speculative: a pack is member-writable room state with
    // an optional mimetype, so no caller can know the payload is an animation.
    // Bytes pass the same SVG/SVGZ, A/V-container and animation-magic checks as
    // everything else.
    Q_INVOKABLE QString mxcAnimatedSource(const QString &mxcUri);
    // Inline video/audio playback. Same contract as animatedSource (validated
    // by container magic, 0600 file with an unguessable name, separate LRU,
    // wiped on sign-out), but returns a file:// URL for the in-process
    // QMediaPlayer only; paths must never reach external applications.
    //
    // The file is written on a worker thread, so the first call for a payload
    // always returns ""; QML re-asks from playableMediaReady(cacheKey).
    Q_INVOKABLE QString playableSource(const QString &mediaKey);
    // Speculative playable prefetch for an on-screen video cover, so Play does
    // not wait for the download. Dispatches only when the declared size is at
    // or below the speculative cap (unknown sizes are never prefetched), at the
    // lowest priority. Dropped on room switch like GIF prefetches.
    Q_INVOKABLE void prefetchPlayable(const QString &mediaKey,
                                      double sizeBytes);
    // Poster for a video without a Matrix thumbnail. Returns the URL when a
    // poster is cached under "thumb:<mediaKey>"; otherwise extracts the first
    // frame (prefetching within the speculative cap) and returns "". The JPEG
    // lives in the in-RAM cache only; decrypted-media pixels never touch disk.
    Q_INVOKABLE QString videoPosterSource(const QString &mediaKey,
                                          double sizeBytes);
    // Embedded audio artwork from QMediaPlayer metadata, kept only in the
    // bounded in-memory cache. Returns a provider URL, or "" for an
    // absent/invalid/ oversized image.
    Q_INVOKABLE QString audioArtworkSource(const QString &mediaKey,
                                           const QVariant &artwork);
    // Cancels the playable fetch for a card that no longer wants it. Interest
    // is refcounted; at zero this frees the slot and aborts the backend
    // download. No failure mark is left. Only an animated consumer of the same
    // bytes keeps the fetch alive; a poster hook or prefetch does not veto a
    // user cancel.
    Q_INVOKABLE void cancelPlayable(const QString &mediaKey);
    // File suffix ("mp4", "webm", "ogg", ...) when the payload's magic matches
    // a supported A/V container, "" otherwise. Public for tests.
    static QString playableExtensionFor(const QByteArray &bytes,
                                        const QString &mimetype);
    Q_INVOKABLE QString previewAnimatedSource(const QString &dataSource,
                                              const QString &mimetype);
    // Validated client-preview bytes exposed only through the bounded
    // in-memory image provider. The remote URL is never an image source.
    Q_INVOKABLE QString previewImageSource(const QString &dataSource,
                                           const QString &mimetype);

    // A failed fetch marks its cache key so QML repolling cannot hammer the
    // backend; retry() clears it. Returns the coarse category or "".
    // Transient categories expire: the watchdog sweeps them and emits
    // mediaRetryable, at most one re-dispatch per interval per key. Validation
    // failures ("rejected", "invalid_gif") stay until retry().
    Q_INVOKABLE QString failureCategory(const QString &cacheKey) const;
    // Non-expiring failure lookup by mxc URI, so Avatar.qml can show initials
    // instead of a permanent skeleton.
    Q_INVOKABLE QString avatarFailureCategory(const QString &mxcUri) const;
    Q_INVOKABLE void retry(const QString &cacheKey);
    void setFailureRetryMsForTest(qint64 ms) { m_failureRetryMs = ms; }

    // In-flight watchdog. An op the backend never completes would pin its slot
    // forever, and kMaxConcurrent of them stall the whole pipeline. Reclaims
    // slots past their class timeout, marks a transient failure and pumps the
    // queue. Callable directly for tests.
    Q_INVOKABLE void checkInflightTimeouts();
    void setInflightTimeoutMsForTest(qint64 ms) { m_inflightTimeoutMs = ms; }

    // Sanitized queue-health snapshot: counts, ages and byte totals only, never
    // keys, URIs or bytes.
    Q_INVOKABLE QVariantMap healthSnapshot() const;
    int inflightCountForTest() const { return m_inflight.size(); }
    int queuedCountForTest() const { return m_queue.size(); }
    void setStarvationMsForTest(qint64 ms) { m_starvationMs = ms; }
    void setPlayableCapsForTest(int entries, qint64 bytes)
    {
        m_playableMaxEntries = entries;
        m_playableMaxBytes = bytes;
    }
    // Writes handed to the worker thread and not yet published: 0 means the
    // payload was refused before any file was created.
    int pendingPlayableWritesForTest() const
    {
        return m_playableWriting.size();
    }

    // A QMediaPlayer keeps its temp file open for the whole session, so cards
    // pin on start and unpin on reset. Eviction skips pinned files, exceeding
    // the cap if necessary; clear() drops all pins.
    Q_INVOKABLE void pinPlayable(const QString &mediaKey);
    Q_INVOKABLE void unpinPlayable(const QString &mediaKey);

    // Drops queued speculative work (GIF autoplay prefetch) on room switch.
    // In-flight ops are untouched and entries a playableSource() caller
    // coalesced onto are kept. Must be called after stopAll() and before the
    // new room's model attaches.
    Q_INVOKABLE void dropQueuedSpeculative();

    // Fetches the full payload and writes it atomically to the chosen
    // destination. Never opens the file. Result via saveFinished().
    Q_INVOKABLE void saveAs(const QString &mediaKey, const QUrl &destination);
    /// A safe default save-dialog file name from a sender-chosen attachment
    /// name, or empty to let the dialog decide.
    Q_INVOKABLE QString suggestedSaveName(const QString &rawName) const;

    /// True when the payload opens as markup (SVG) or gzip (SVGZ); image-class
    /// results are refused on this. Public so MediaImageProvider applies the
    /// same rule.
    static bool looksLikeMarkupOrCompressed(const QByteArray &bytes);

    // Fetches the full payload for starring a chat GIF and returns it via
    // mediaBytesForStar(). No GIF validation here; see
    // AppController::starChatGif. Uses the save timeout class.
    Q_INVOKABLE void fetchFullForStar(const QString &mediaKey);

    // SHA-256 hex of the full payload already in the display cache for
    // `mediaKey`, or "". Never dispatches and never exposes bytes; used by
    // GifStarredStore to tell whether a GIF is starred.
    QString cachedFullContentHash(const QString &mediaKey) const;

    Q_INVOKABLE void clear();

    // Shared with MediaImageProvider (QML render thread).
    QByteArray cachedBytes(const QString &cacheKey) const;
    // MediaImageProvider's image-thread read for embedded artwork.
    QImage cachedArtwork(const QString &cacheKey) const;

    // Avatar-class entries ("mxc:" keys) have their own byte budget so timeline
    // media cannot evict every avatar. Both budgets are hard bounds.
    void setCacheLimitBytes(qint64 bytes) { m_cacheLimit = bytes; }
    void setAvatarCacheLimitBytes(qint64 bytes) { m_avatarCacheLimit = bytes; }
    qint64 cacheBytesUsed() const;

Q_SIGNALS:
    void supportedChanged();
    void mediaCached(const QString &cacheKey);
    // An expired transient failure mark was swept; consumers may re-request.
    void mediaRetryable(const QString &cacheKey);
    void animatedMediaReady(const QString &cacheKey);
    void playableMediaReady(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    // mediaKey identifies which save finished.
    void saveFinished(bool ok, const QString &message,
                      const QString &mediaKey);
    // Result of fetchFullForStar(). On failure `bytes` is empty and `category`
    // is set.
    void mediaBytesForStar(const QString &mediaKey, bool ok,
                           const QByteArray &bytes, const QString &category);
    // The poster extractor saw the video's display-oriented size, so the card
    // can take the true shape for events that declare none. Dimensions only.
    void videoDimensionsLearned(const QString &mediaKey, int width,
                                int height);
    // A full A/V payload's real size, so later sessions can prefetch events
    // that declare no size. Size only.
    void playableSizeLearned(const QString &mediaKey, qint64 bytes);

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
        // Full payload with the save timeout class, relayed via
        // mediaBytesForStar().
        bool starRequest = false;
        // 0 standard / 1 playable / 2 save. Each Rust timeout sits below the
        // matching C++ watchdog deadline, so the watchdog stays last-resort.
        int timeoutClass = 0;
        // Lower dispatches first:
        //   0 explicit user intent (play, Save As, star)
        //   1 interactive chrome (avatars, thumbnails, mxc images)
        //   2 full static media
        //   3 speculative (GIF autoplay prefetch)
        int priority = 2;
        // Monotonic enqueue time for the starvation bound (see pump()).
        qint64 enqueuedAtMs = 0;
        // Dispatch time (m_failureClock ms) for log timing; 0 until dispatched.
        qint64 dispatchedAtMs = 0;
    };

    void insertCache(const QString &cacheKey, const QByteArray &bytes);
    void touch(const QString &cacheKey) const;
    void markFailed(const Pending &request, const QString &category);
    // True while the key's failure mark blocks dispatch; expires transient
    // marks.
    bool failureBlocks(const QString &cacheKey);
    // Removes expired transient marks and emits mediaRetryable for each.
    void sweepExpiredFailureMarks();
    // Validation failures are permanent; everything else is transient.
    static bool isPermanentCategory(const QString &category);
    // Every terminal outcome must clear the key's interest sets, or they grow
    // without bound and veto cancelPlayable.
    void dropInterestSets(const QString &cacheKey);
    static bool isAvatarClassKey(const QString &cacheKey);
    void dispatch(const Pending &request);
    void pump();
    bool alreadyPending(const QString &cacheKey) const;
    // Raises an already-queued entry to the caller's priority and timeout
    // class.
    void promoteQueuedRequest(const QString &cacheKey, int priority,
                              int timeoutClass);
    // Heavy = priority >= 2. Capped below kMaxConcurrent so interactive classes
    // always have headroom.
    int heavyInflightCount() const;
    // A homeserver that cannot thumbnail may return the original media under
    // the parent's mimetype, so the bytes decide whether it may enter the image
    // path.
    static bool looksLikeAvContainer(const QByteArray &bytes);
public:
    // File suffix an animatable payload must use ("gif", "webp"), or "". Public
    // for tests. Decided from the bytes only: sticker mimetypes are optional
    // (MSC2545) and attacker-chosen, so the magic is the only authority.
    static QString animatedExtensionFor(const QByteArray &bytes);
private:
    static QString sanitizedFileName(const QString &name);
    void writeSaveFile(const QUrl &destination, const QByteArray &bytes,
                       const QString &mediaKey);
    QString writeAnimatedFile(const QString &cacheKey, const QByteArray &bytes);
    // Playable payloads are written on PlayableFileWriter's worker thread. The
    // size bound, container sniff and name derivation run here first so they
    // fail closed. Returns true when a write is under way (possibly coalesced),
    // false when the payload was refused and no file will appear.
    // `notifyFailure` marks a pressed-play consumer that is owed
    // mediaFetchFailed on failure.
    bool beginPlayableWrite(const QString &cacheKey, const QString &mediaKey,
                            const QByteArray &bytes, const QString &mimetype,
                            bool notifyFailure);
    void onPlayableWriteFinished(quint64 serial, const QString &cacheKey,
                                 const QString &path, quint64 generation,
                                 bool ok);
    // Registers a finished file and evicts to the caps, never a pinned file.
    void registerPlayableFile(const QString &cacheKey, const QString &path,
                              qint64 bytes);
    // Lazy: sessions that never play A/V never start the writer thread.
    PlayableFileWriter *ensurePlayableWriter();
    // Lazy so headless tests and sessions without A/V never touch Qt
    // Multimedia.
    VideoPosterExtractor *ensurePosterExtractor();
    void startPosterExtraction(const QString &mediaKey,
                               const QString &filePath);
    // Pays Qt Multimedia's one-time ~1 s initialization off the GUI thread.
    void warmMultimediaBackend();
    void onPosterReady(const QString &mediaKey, const QByteArray &jpeg);

    MatrixClient *m_client = nullptr;

    mutable QMutex m_cacheMutex;
    QHash<QString, QByteArray> m_cache;
    // Separate LRUs over one byte store so timeline media and avatars cannot
    // evict each other. front = most recent.
    mutable QList<QString> m_lru;
    mutable QList<QString> m_avatarLru;
    qint64 m_cacheLimit = 64 * 1024 * 1024;
    qint64 m_avatarCacheLimit = 8 * 1024 * 1024;
    // Per-class byte totals (m_cacheMutex), so size never needs a full scan.
    qint64 m_cacheBytesMain = 0;
    qint64 m_cacheBytesAvatar = 0;
    // Decoded cover art in its own bounded LRU, so it never needs a plaintext
    // file to reach QML.
    QHash<QString, QImage> m_artworkCache;
    QList<QString> m_artworkLru;
    qint64 m_artworkBytes = 0;
    // Per-key revision, bumped on each byte insert and appended to provider
    // URLs as "?r=<n>". A re-cached key yields a new source so an Image stuck
    // in Error reloads, while cache hits keep the pixmap-cache key stable.
    QHash<QString, quint32> m_revision;
    // Memoized SHA-256 per cache key: cachedFullContentHash() is queried from
    // several bindings per GIF row and hashing a 64 MiB payload takes ~430 ms.
    // Invalidated on revision change, eviction and clear(). Guarded by
    // m_cacheMutex.
    struct ContentHashEntry { QString hex; quint32 revision = 0; };
    mutable QHash<QString, ContentHashEntry> m_contentHashCache;

    QHash<quint64, Pending> m_inflight;
    QQueue<Pending> m_queue;
    // Failure marks with the time they were set. Bounded; cleared on sign-out
    // and per key via retry().
    struct FailureMark {
        QString category;
        qint64 markedAtMs = 0;
    };
    QHash<QString, FailureMark> m_failed;
    QElapsedTimer m_failureClock;
    qint64 m_failureRetryMs = 60 * 1000;

    QTimer m_watchdog;
    // Per-request lines go to lightning.media.trace; the default category gets
    // one summary per burst. Counts and bytes only, no keys, URIs or paths, so
    // it is safe to paste into a bug report.
    QTimer m_burstSummary;
    qint64 m_burstCompleted = 0;
    qint64 m_burstFailed = 0;
    qint64 m_burstBytes = 0;
    qint64 m_burstPeakQueued = 0;
    /// Restarts the quiet-period timer and tracks the peak queue depth.
    void noteMediaActivity();
    // Class timeouts; only a stuck op reaches them.
    qint64 m_inflightTimeoutMs = 45 * 1000;
    qint64 m_saveTimeoutMs = 5 * 60 * 1000; // user Save As of a large file
    // Above the 90 s Rust bound, below the save class.
    qint64 m_playableTimeoutMs = 100 * 1000;
    // Object thread only.
    qint64 m_statCompleted = 0;
    qint64 m_statFailed = 0;
    qint64 m_statTimedOut = 0;
    qint64 m_statDroppedStale = 0;
    qint64 m_statCacheHit = 0;
    qint64 m_statCacheMiss = 0;
    qint64 m_statCancelled = 0;
    // Counts SHA-256 runs on a memo miss, so tests can prove memoization
    // without timing. Written under m_cacheMutex.
    mutable qint64 m_statContentHashComputed = 0;

    std::unique_ptr<QTemporaryDir> m_animatedDir;
    QHash<QString, QString> m_animatedFiles;
    QHash<QString, qint64> m_animatedSizes;
    QList<QString> m_animatedLru;
    QSet<QString> m_animatedWanted;
    // Keys with at least one non-speculative asker; only these report
    // mediaFetchFailed("invalid_gif").
    QSet<QString> m_animatedDemanded;
    // Playable registry. Shares the scratch dir with the animated path but has
    // its own larger LRU. The per-session suffix keeps names unguessable.
    QHash<QString, QString> m_playableFiles;
    QHash<QString, qint64> m_playableSizes;
    QList<QString> m_playableLru;
    // Refcounted: the main timeline and thread panel can show the same event,
    // and one card's cancel must not strand the other. Failures drop the entry.
    QHash<QString, int> m_playableWanted;
    // Speculative interest ("full:" keys), separate so dropQueuedSpeculative
    // can tell a prefetch from a pressed-play consumer.
    QSet<QString> m_prefetchWanted;
    // "full:" keys that should trigger a poster, and media keys being
    // extracted.
    QSet<QString> m_posterWanted;
    QSet<QString> m_posterExtracting;
    VideoPosterExtractor *m_posterExtractor = nullptr;
    // Not reset by clear(): Qt Multimedia initializes once per process.
    bool m_multimediaWarmed = false;
    // Refcounted pins on files a live player holds open; never evicted while
    // positive.
    QHash<QString, int> m_pinnedPlayables;
    QString m_playableNameSalt;
    PlayableFileWriter *m_playableWriter = nullptr;
    struct PendingPlayableWrite {
        quint64 serial = 0;
        QString mediaKey;
        QString path;
        qint64 bytes = 0;
        quint64 generation = 0;
        bool notifyFailure = false;
    };
    // Writes handed to the worker and not yet published. This hash, not the
    // signal connection, is the session-isolation authority: a queued
    // completion can outlive a disconnect, and clear() empties it. The
    // generation below is a second guard.
    QHash<QString, PendingPlayableWrite> m_playableWriting;
    // Bumped by clear(); a completion with an older value publishes nothing.
    quint64 m_sessionGeneration = 1;
    // Members so tests can shrink them.
    int m_playableMaxEntries;
    qint64 m_playableMaxBytes;
    // Starvation bound: an entry older than this dispatches ahead of newer,
    // higher-priority ones.
    qint64 m_starvationMs = 15 * 1000;
    // Hard bound; excess requests queue.
    static constexpr int kMaxConcurrent = 8;
    // Cap on heavy slots (priority >= 2), so playback and chrome always find
    // headroom.
    static constexpr int kMaxHeavyConcurrent = 6;
    static constexpr int kMaxFailureMarks = 512;
    // One server-side edge for every avatar (largest consumer: 96 px at 2x
    // DPR).
    static constexpr int kAvatarCanonicalEdge = 224;
    static constexpr int kArtworkMaxEntries = 24;
    static constexpr qint64 kArtworkMaxBytes = 24 * 1024 * 1024;
    static constexpr int kArtworkMaxEdge = 4096;
    static constexpr qint64 kAnimatedCacheBytes = 64 * 1024 * 1024;
    static constexpr int kAnimatedCacheEntries = 64;
    static constexpr qint64 kPlayableCacheBytes = 256 * 1024 * 1024;
    static constexpr int kPlayableCacheEntries = 16;
    // Full-size media above this skips the RAM LRU (the player reads it from
    // disk).
    static constexpr qint64 kLargeCacheSkipBytes = 8 * 1024 * 1024;
    // Playables declaring more than this are fetched only on explicit Play.
    static constexpr qint64 kSpeculativePlayableMaxBytes = 32 * 1024 * 1024;
};
