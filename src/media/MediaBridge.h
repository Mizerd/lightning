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
    // Avatar thumbnails by plain mxc URI, server-side scaled.
    Q_INVOKABLE QString avatarSource(const QString &mxcUri, int size);
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
    Q_INVOKABLE QString failureCategory(const QString &cacheKey) const;
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

    // Explicit Save As: fetches the full payload (cache or network) and
    // writes it atomically to the user-chosen destination. Never executes
    // or opens the file. Result arrives via saveFinished().
    Q_INVOKABLE void saveAs(const QString &mediaKey, const QUrl &destination);

    Q_INVOKABLE void clear();

    // Shared with MediaImageProvider (called from the QML render thread).
    QByteArray cachedBytes(const QString &cacheKey) const;

    // Cache cap; exposed for tests.
    void setCacheLimitBytes(qint64 bytes) { m_cacheLimit = bytes; }
    qint64 cacheBytesUsed() const;

Q_SIGNALS:
    void supportedChanged();
    void mediaCached(const QString &cacheKey);
    void animatedMediaReady(const QString &cacheKey);
    // v0.7: a requested playable (video/audio) payload was validated and
    // materialized; QML re-calls playableSource(cacheKey) for the URL.
    void playableMediaReady(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    void saveFinished(bool ok, const QString &message);

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
        // v0.7: backend timeout class (0 standard / 1 playable / 2 save).
        // The Rust timeout for each class sits strictly below the matching
        // C++ watchdog deadline, so Rust normally emits the terminal event
        // and the watchdog stays last-resort.
        int timeoutClass = 0;
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
    void dispatch(const Pending &request);
    void pump();
    bool alreadyPending(const QString &cacheKey) const;
    static QString sanitizedFileName(const QString &name);
    void writeSaveFile(const QUrl &destination, const QByteArray &bytes);
    QString writeAnimatedFile(const QString &cacheKey, const QByteArray &bytes,
                              const QString &mimetype);
    QString writePlayableFile(const QString &cacheKey, const QByteArray &bytes,
                              const QString &mimetype);

    MatrixClient *m_client = nullptr;

    mutable QMutex m_cacheMutex;
    QHash<QString, QByteArray> m_cache;
    mutable QList<QString> m_lru; // front = most recent
    qint64 m_cacheLimit = 64 * 1024 * 1024;

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
    QSet<QString> m_playableWanted;
    QString m_playableNameSalt;
    // v0.7: raised from 4 — a cold room list fetches its visible avatars in
    // one or two bursts instead of a long 4-at-a-time trickle. Still a hard
    // bound; excess requests queue and pump as fetches complete.
    static constexpr int kMaxConcurrent = 8;
    static constexpr int kMaxFailureMarks = 512;
    static constexpr qint64 kAnimatedCacheBytes = 64 * 1024 * 1024;
    static constexpr int kAnimatedCacheEntries = 64;
    static constexpr qint64 kPlayableCacheBytes = 256 * 1024 * 1024;
    static constexpr int kPlayableCacheEntries = 16;
    // Full-size media above this skips the RAM LRU (it exists on disk for
    // the player; caching it in memory would evict every image at once).
    static constexpr qint64 kLargeCacheSkipBytes = 8 * 1024 * 1024;
};
