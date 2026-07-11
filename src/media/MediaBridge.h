#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>

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

    // v0.5.11: failure state. A failed fetch marks its cache key so QML
    // repolling cannot hammer the backend; retry() clears the mark so the
    // next mediaSource/avatarSource call dispatches again. failureCategory
    // returns the coarse category ("network", "rejected", ...) or "".
    Q_INVOKABLE QString failureCategory(const QString &cacheKey) const;
    Q_INVOKABLE void retry(const QString &cacheKey);

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
    };

    void insertCache(const QString &cacheKey, const QByteArray &bytes);
    void touch(const QString &cacheKey) const;
    void markFailed(const Pending &request, const QString &category);
    void dispatch(const Pending &request);
    void pump();
    bool alreadyPending(const QString &cacheKey) const;
    static QString sanitizedFileName(const QString &name);
    void writeSaveFile(const QUrl &destination, const QByteArray &bytes);

    MatrixClient *m_client = nullptr;

    mutable QMutex m_cacheMutex;
    QHash<QString, QByteArray> m_cache;
    mutable QList<QString> m_lru; // front = most recent
    qint64 m_cacheLimit = 64 * 1024 * 1024;

    QHash<quint64, Pending> m_inflight;
    QQueue<Pending> m_queue;
    // v0.5.11: cache keys whose last fetch failed, with the coarse
    // category. Bounded; cleared on sign-out and per key via retry().
    QHash<QString, QString> m_failed;
    static constexpr int kMaxConcurrent = 4;
    static constexpr int kMaxFailureMarks = 512;
};
