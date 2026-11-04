#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QList>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QTemporaryDir>
#include <atomic>
#include <functional>
#include <memory>

class MatrixClient;
class PlayableFileWriter;
class VideoPosterExtractor;

class MediaBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(QVariantList activeSaves READ activeSaves
               NOTIFY activeSavesChanged)
    Q_PROPERTY(QStringList savingKeys READ savingKeys
               NOTIFY activeSavesChanged)

public:
    explicit MediaBridge(QObject *parent = nullptr);
    ~MediaBridge() override;

    void setClient(MatrixClient *client);
    bool supported() const;

    using LinkMediaResolver = std::function<QVariantMap(const QString &)>;
    void setLinkMediaResolver(LinkMediaResolver resolver)
    {
        m_linkResolver = std::move(resolver);
    }
    static bool isLinkMediaKey(const QString &mediaKey)
    {
        return mediaKey.startsWith(QLatin1String("link:"));
    }

    Q_INVOKABLE QString mediaSource(const QString &mediaKey, const QString &kind);
    Q_INVOKABLE QString avatarSource(const QString &mxcUri, int size);
    Q_INVOKABLE QString wideImageSource(const QString &mxcUri);
    QImage cachedAvatarImage(const QString &mxcUri) const;
    Q_INVOKABLE QString mxcImageSource(const QString &mxcUri, int edge);
    Q_INVOKABLE QString cachedSource(const QString &cacheKey) const;
    QSize cachedImageSize(const QString &source) const;
    Q_INVOKABLE QString animatedSource(const QString &mediaKey,
                                       bool speculative = false);
    Q_INVOKABLE QString mxcAnimatedSource(const QString &mxcUri);
    Q_INVOKABLE QString avatarAnimationSource(const QString &mxcUri,
                                              bool explicitIntent);
    Q_INVOKABLE QString wideAnimationSource(const QString &mxcUri);
    Q_INVOKABLE QString backdropAnimationSource(const QString &mxcUri);
    Q_INVOKABLE QString stagedAnimationSource(const QString &stagedUrl);
    void setStagedBytesProvider(std::function<QByteArray(const QString &)> provider)
    {
        m_stagedBytes = std::move(provider);
    }
    Q_INVOKABLE bool claimMotionSlot(QObject *owner, bool intent = false);
    Q_INVOKABLE void releaseMotionSlot(QObject *owner);
    int motionSlotsInUseForTest() const
    {
        return static_cast<int>(m_motionOwners.size());
    }
    static constexpr int kMaxMotionSlots = 24;
    static constexpr int kMotionIntentReserve = 4;
    // fetchMxcThumbnail(mxc, 0, kMotionProbeHeight) asks Rust for the original
    // under the motion-probe size class (rust/src/rooms.rs mxc_fetch_cap).
    static constexpr int kMotionProbeHeight = 1;
    Q_INVOKABLE bool holdAnimation(QObject *owner, const QString &fileUrl);
    Q_INVOKABLE void releaseAnimation(QObject *owner);
    static constexpr int kAnimatedHardCapEntries = 128;
    static constexpr qint64 kAnimatedHardCapBytes = 256 * 1024 * 1024;
    int animationHoldsForTest() const
    {
        return static_cast<int>(m_animationHolds.size());
    }
    Q_INVOKABLE QString playableSource(const QString &mediaKey);
    Q_INVOKABLE void prefetchPlayable(const QString &mediaKey,
                                      double sizeBytes,
                                      double capBytes = 0);
    Q_INVOKABLE QString videoPosterSource(const QString &mediaKey,
                                          double sizeBytes,
                                          double capBytes = 0);
    Q_INVOKABLE QString audioArtworkSource(const QString &mediaKey,
                                           const QVariant &artwork);
    Q_INVOKABLE void cancelPlayable(const QString &mediaKey);
    Q_INVOKABLE void cancelPrefetch(const QString &mediaKey);
    Q_INVOKABLE bool hasPrefetch(const QString &mediaKey) const;
    static QString playableExtensionFor(const QByteArray &bytes,
                                        const QString &mimetype);
    Q_INVOKABLE QString previewAnimatedSource(const QString &dataSource,
                                              const QString &mimetype);
    int previewDecodeCount() const { return m_previewDecodes; }
    Q_INVOKABLE QString previewImageSource(const QString &dataSource,
                                           const QString &mimetype);

    Q_INVOKABLE QString failureCategory(const QString &cacheKey) const;
    Q_INVOKABLE QString avatarFailureCategory(const QString &mxcUri) const;
    Q_INVOKABLE void retry(const QString &cacheKey);
    Q_INVOKABLE int failureRetrySeconds(const QString &cacheKey) const;
    void setFailureRetryMsForTest(qint64 ms) { m_failureRetryMs = ms; }

    Q_INVOKABLE void checkInflightTimeouts();
    void setInflightTimeoutMsForTest(qint64 ms) { m_inflightTimeoutMs = ms; }

    Q_INVOKABLE QVariantMap healthSnapshot() const;
    int inflightCountForTest() const { return m_inflight.size(); }
    int queuedCountForTest() const { return m_queue.size(); }
    void setStarvationMsForTest(qint64 ms) { m_starvationMs = ms; }
    void setPlayableCapsForTest(int entries, qint64 bytes)
    {
        m_playableMaxEntries = entries;
        m_playableMaxBytes = bytes;
    }
    int pendingPlayableWritesForTest() const
    {
        return m_playableWriting.size();
    }

    Q_INVOKABLE void pinPlayable(const QString &mediaKey);
    Q_INVOKABLE void unpinPlayable(const QString &mediaKey);

    Q_INVOKABLE void dropQueuedSpeculative();

    Q_INVOKABLE void saveAs(const QString &mediaKey, const QUrl &destination);
    void saveInto(const QString &mediaKey, const QString &folder,
                  const QString &leaf);
    QVariantList activeSaves() const;
    QStringList savingKeys() const;
    Q_INVOKABLE void cancelSave(const QString &mediaKey);
    Q_INVOKABLE QString suggestedSaveName(const QString &rawName) const;

    /// True when the payload opens as markup (SVG) or gzip (SVGZ); image-class
    /// results are refused on this. Public so MediaImageProvider applies the
    /// same rule.
    static bool looksLikeMarkupOrCompressed(const QByteArray &bytes);

    Q_INVOKABLE void fetchFullForStar(const QString &mediaKey);

    QString cachedFullContentHash(const QString &mediaKey) const;

    Q_INVOKABLE void clear();
    void retryAvatar(const QString &mxcUri);

    QByteArray cachedBytes(const QString &cacheKey) const;
    // The image provider's read: ends the eviction shield cachedSource() put
    // on the key.
    QByteArray bytesForDecode(const QString &cacheKey);
    static QString wideImageCacheKey(const QString &mxcUri);
    QImage cachedArtwork(const QString &cacheKey) const;

    void setCacheLimitBytes(qint64 bytes) { m_cacheLimit = bytes; }
    void setAvatarCacheLimitBytes(qint64 bytes) { m_avatarCacheLimit = bytes; }
    void setDecodeGraceMsForTest(qint64 ms) { m_decodeGraceMs = ms; }
    void setDecodeShieldBudgetForTest(qint64 bytes) { m_decodeShieldBudget = bytes; }
    qint64 cacheBytesUsed() const;

    // Counters since the previous call, for the scroll-perf-window trace.
    Q_INVOKABLE QVariantMap takeTraceWindow();

Q_SIGNALS:
    void supportedChanged();
    void mediaCached(const QString &cacheKey);
    void mediaRetryable(const QString &cacheKey);
    void animatedMediaReady(const QString &cacheKey);
    void motionSlotFreed();
    void playableMediaReady(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    void saveFinished(bool ok, const QString &message,
                      const QString &mediaKey);
    void saveCompleted(const QString &mediaKey, const QString &path);
    void saveCancelled(const QString &mediaKey);
    void activeSavesChanged();
    void mediaBytesForStar(const QString &mediaKey, bool ok,
                           const QByteArray &bytes, const QString &category);
    void videoDimensionsLearned(const QString &mediaKey, int width,
                                int height);
    void playableSizeLearned(const QString &mediaKey, qint64 bytes);

private Q_SLOTS:
    void onMediaReady(quint64 opId, const QString &mediaKey, int kind,
                      const QByteArray &bytes, const QString &mimetype,
                      const QString &filename);
    void onMediaFailed(quint64 opId, const QString &mediaKey, int kind,
                       const QString &category);
    void onLoggedOut();

private:
    void abortFetch(const QString &cacheKey);
    struct Pending {
        QString cacheKey;
        bool isMxc = false;
        QString mediaKey;
        int kind = 0;
        int size = 0;
        bool saveRequest = false;
        QUrl saveDestination;
        bool saveUniquify = false;
        bool starRequest = false;
        int timeoutClass = 0;
        int priority = 2;
        qint64 enqueuedAtMs = 0;
        qint64 dispatchedAtMs = 0;
    };

    void insertCache(const QString &cacheKey, const QByteArray &bytes);
    void trimClassLocked(bool avatarClass, const QString &keep);
    void touch(const QString &cacheKey) const;
    void markFailed(const Pending &request, const QString &category,
                    qint64 retryAfterMs = 0);
    qint64 markExpiryMs(const QString &category, qint64 retryAfterMs) const;
    bool failureBlocks(const QString &cacheKey);
    void sweepExpiredFailureMarks();
    static bool isPermanentCategory(const QString &category);
    // Every terminal outcome must clear the key's interest sets, or they grow
    // without bound and veto cancelPlayable.
    void dropInterestSets(const QString &cacheKey);
    static bool isAvatarClassKey(const QString &cacheKey);
    void dispatch(const Pending &request);
    quint64 dispatchLinkMedia(Pending &request);
    void pump();
    bool alreadyPending(const QString &cacheKey) const;
    void promoteQueuedRequest(const QString &cacheKey, int priority,
                              int timeoutClass);
    int heavyInflightCount() const;
    // A homeserver that cannot thumbnail may return the original media under
    // the parent's mimetype, so the bytes decide whether it may enter the image
    // path.
    static bool looksLikeAvContainer(const QByteArray &bytes);
public:
    static QString animatedExtensionFor(const QByteArray &bytes);
private:
    static QString sanitizedFileName(const QString &name);
    void writeSaveFile(const QUrl &destination, const QByteArray &bytes,
                       const QString &mediaKey, bool uniquify = false);
    void startSave(const QString &mediaKey, const QUrl &destination,
                   bool uniquify);
    QString writeAnimatedFile(const QString &cacheKey, const QByteArray &bytes);
    QString writeMotionFile(const QString &cacheKey, const QByteArray &bytes,
                            qint64 maxPixels);
    QString motionFileUrl(const QString &cacheKey, qint64 maxPixels);
    bool beginPlayableWrite(const QString &cacheKey, const QString &mediaKey,
                            const QByteArray &bytes, const QString &mimetype,
                            bool notifyFailure);
    void onPlayableWriteFinished(quint64 serial, const QString &cacheKey,
                                 const QString &path, quint64 generation,
                                 bool ok);
    void registerPlayableFile(const QString &cacheKey, const QString &path,
                              qint64 bytes);
    PlayableFileWriter *ensurePlayableWriter();
    VideoPosterExtractor *ensurePosterExtractor();
    void startPosterExtraction(const QString &mediaKey,
                               const QString &filePath);
    void warmMultimediaBackend();
    void onPosterReady(const QString &mediaKey, const QByteArray &jpeg);

    MatrixClient *m_client = nullptr;
    LinkMediaResolver m_linkResolver;
    static constexpr quint64 kLocalOpIdBase = quint64(1) << 62;
    quint64 m_nextLocalOpId = kLocalOpIdBase;

    mutable QMutex m_cacheMutex;
    QHash<QString, QByteArray> m_cache;
    mutable QList<QString> m_lru;
    mutable QList<QString> m_avatarLru;
    // Keys handed to QML whose bytes the image provider has not read yet.
    // Evicting one makes its Image fail and fetch again, and the provider
    // reads in hand-out order, which is LRU order: the next read is the next
    // victim.
    mutable QHash<QString, qint64> m_awaitingDecode;
    QElapsedTimer m_handoutClock;
    qint64 m_decodeGraceMs = 5000;
    qint64 m_decodeShieldBudget = 4 * 1024 * 1024;
    qint64 m_cacheLimit = 32 * 1024 * 1024;
    qint64 m_avatarCacheLimit = 8 * 1024 * 1024;
    qint64 m_cacheBytesMain = 0;
    qint64 m_cacheBytesAvatar = 0;
    QHash<QString, QImage> m_artworkCache;
    QList<QString> m_artworkLru;
    qint64 m_artworkBytes = 0;
    QHash<QString, quint32> m_revision;
    struct ContentHashEntry { QString hex; quint32 revision = 0; };
    mutable QHash<QString, ContentHashEntry> m_contentHashCache;

    QHash<quint64, Pending> m_inflight;
    QQueue<Pending> m_queue;
    struct FailureMark {
        QString category;
        qint64 markedAtMs = 0;
        qint64 retryAfterMs = 0;
    };
    QHash<QString, FailureMark> m_failed;
    QElapsedTimer m_failureClock;
    qint64 m_failureRetryMs = 60 * 1000;

    QTimer m_watchdog;
    QTimer m_burstSummary;
    qint64 m_burstCompleted = 0;
    qint64 m_burstFailed = 0;
    qint64 m_burstBytes = 0;
    qint64 m_burstPeakQueued = 0;
    void noteMediaActivity();
    qint64 m_inflightTimeoutMs = 45 * 1000;
    qint64 m_saveTimeoutMs = 5 * 60 * 1000;
    qint64 m_playableTimeoutMs = 100 * 1000;
    qint64 m_statCompleted = 0;
    qint64 m_statFailed = 0;
    qint64 m_statTimedOut = 0;
    qint64 m_statDroppedStale = 0;
    qint64 m_statCacheHit = 0;
    qint64 m_statCacheMiss = 0;
    qint64 m_statCancelled = 0;
    mutable qint64 m_statContentHashComputed = 0;
    QHash<QString, int> m_fetchCounts;
    QHash<QString, int> m_windowRefetches;
    qint64 m_windowFetches = 0;
    qint64 m_windowRefetchTotal = 0;
    qint64 m_windowEvictions = 0;
    std::atomic<qint64> m_windowDecodeMisses{0};
    void noteDispatched(const QString &cacheKey);

    std::function<QByteArray(const QString &)> m_stagedBytes;
    QHash<QString, QString> m_stagedKeys;
    std::unique_ptr<QTemporaryDir> m_animatedDir;
    QHash<QString, QString> m_animatedFiles;
    QHash<QPair<qsizetype, size_t>, QPair<QString, QString>> m_previewAnimMemo;
    static constexpr qsizetype kMaxAnimatedBytes = 20 * 1024 * 1024;
    int m_previewDecodes = 0;
    QHash<QString, qint64> m_animatedSizes;
    QList<QString> m_animatedLru;
    struct AnimationHold {
        QString cacheKey;
        QMetaObject::Connection destroyed;
    };
    QHash<QObject *, AnimationHold> m_animationHolds;
    bool isAnimationHeld(const QString &cacheKey) const;
    quint64 m_animatedWriteSerial = 0;
    QSet<QString> m_animatedWanted;
    QSet<QString> m_animatedDemanded;
    QHash<QString, qint64> m_motionVerdict;
    struct MotionHolder {
        QMetaObject::Connection destroyed;
        bool intent = false;
    };
    QHash<QObject *, MotionHolder> m_motionOwners;
    bool m_motionFreedPending = false;
    int passiveMotionSlots() const;
    static constexpr int kMaxMotionVerdicts = 4096;
    static constexpr qint64 kMotionMaxBytes = 8 * 1024 * 1024;
    QHash<QString, QString> m_playableFiles;
    QHash<QString, qint64> m_playableSizes;
    QList<QString> m_playableLru;
    QHash<QString, int> m_playableWanted;
    QSet<QString> m_prefetchWanted;
    QSet<QString> m_posterWanted;
    QSet<QString> m_posterExtracting;
    VideoPosterExtractor *m_posterExtractor = nullptr;
    bool m_multimediaWarmed = false;
    QHash<QString, int> m_pinnedPlayables;
    QString m_playableNameSalt;
    PlayableFileWriter *m_playableWriter = nullptr;
    struct ActiveSave {
        QString mediaKey;
        QString fileName;
    };
    QList<ActiveSave> m_activeSaves;
    void removeActiveSave(const QString &mediaKey);

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
    quint64 m_sessionGeneration = 1;
    int m_playableMaxEntries;
    qint64 m_playableMaxBytes;
    qint64 m_starvationMs = 15 * 1000;
    static constexpr int kMaxConcurrent = 8;
    static constexpr int kMaxHeavyConcurrent = 6;
    static constexpr int kMaxFailureMarks = 512;
    static constexpr int kAvatarCanonicalEdge = 224;
    static constexpr int kArtworkMaxEntries = 24;
    static constexpr qint64 kArtworkMaxBytes = 24 * 1024 * 1024;
    static constexpr int kArtworkMaxEdge = 4096;
    static constexpr qint64 kAnimatedCacheBytes = 64 * 1024 * 1024;
    static constexpr int kAnimatedCacheEntries = 64;
    static constexpr qint64 kPlayableCacheBytes = 256 * 1024 * 1024;
    static constexpr int kPlayableCacheEntries = 16;
    static constexpr qint64 kLargeCacheSkipBytes = 8 * 1024 * 1024;
    static constexpr qint64 kSpeculativePlayableMaxBytes = 32 * 1024 * 1024;
};
