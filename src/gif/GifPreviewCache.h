#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class GifTransport;
class QTemporaryDir;

// The picker's provider previews and stills (`app.gif.previews`). Qt picks an
// image decoder from the payload, so a provider URL handed to Image or
// AnimatedImage could be decoded as SVG wherever the qsvg plugin ships. Tiles
// get their source from here instead: the bytes come through
// GifTransport::download (the Rust safe-get: https, provider CDN hosts,
// DNS/IP, redirects, size, GIF magic) and are checked again with
// gif::validateGifBytes before they are written. A GIF must start with
// "GIF87a"/"GIF89a", so markup and gzip (SVG, SVGZ) can never pass.
//
// Validated copies live as files in a private temporary directory, because
// AnimatedImage only plays files or network URLs. Bounded by count and bytes,
// fetched a few at a time with stills first, only while a picker is open, and
// cleared when the session ends. A fetch with no answer frees its slot after
// kFetchTimeoutMs.
//
// Tiles can outlive a closed picker (the Saved and Recent lists are not
// cleared), and a finishing fetch re-evaluates their sources, so an inactive
// cache serves what it has and queues nothing.
class GifPreviewCache : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GifPreviewCache is exposed via app.gif.previews")
    // Bumped whenever a source appears or the cache is cleared; bindings that
    // call source() read it to re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit GifPreviewCache(QObject *parent = nullptr);
    ~GifPreviewCache() override;

    // Not owned. Reconnecting is allowed.
    void setTransport(GifTransport *transport);

    // file:// URL of the validated copy of `url`, or "" while it downloads
    // (revisionChanged follows) or when it was refused. Only https .gif URLs
    // on the provider CDNs are fetched. `still` jumps the preview queue.
    Q_INVOKABLE QString source(const QString &url, bool still = false);
    // The URLs `owner` (a grid tile) is showing. Held copies are never
    // evicted, and a fetch nobody holds any more is dropped if it has not
    // started. Replaces the owner's previous set; released when it is
    // destroyed.
    Q_INVOKABLE void hold(QObject *owner, const QStringList &urls);
    // Drops every fetch that has not started (picker closed).
    Q_INVOKABLE void dropQueued();
    // A picker opened (true) or closed (false). Inactive, source() fetches
    // nothing and queued fetches are dropped; opening re-evaluates sources.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // Frees the slots of fetches older than kFetchTimeoutMs. The backend
    // drops a result silently once its session is gone, so without this a
    // lost answer would hold its slot for the life of the process. Runs on a
    // timer while fetches are in flight.
    void expireStaleFetches();
    // Drops everything, including the files (session ended).
    Q_INVOKABLE void clear();

    int revision() const { return m_revision; }

    int inflightCountForTest() const { return m_inflight.size(); }
    int queuedCountForTest() const
    {
        return m_stillQueue.size() + m_previewQueue.size();
    }
    int cachedCountForTest() const { return m_files.size(); }
    void setLimitsForTest(int entries, qint64 bytes)
    {
        m_maxEntries = entries;
        m_maxBytes = bytes;
    }
    void setFetchTimeoutMsForTest(qint64 ms) { m_fetchTimeoutMs = ms; }
    void setRetryAfterMsForTest(qint64 ms) { m_retryAfterMs = ms; }

    static constexpr int kMaxConcurrent = 6;
    // A picker preview; the sendable original has its own, larger bound.
    static constexpr qint64 kMaxPreviewBytes = 8 * 1024 * 1024;
    // Above the Rust request timeout, so only a lost answer reaches it.
    static constexpr qint64 kFetchTimeoutMs = 2 * 60 * 1000;

Q_SIGNALS:
    void revisionChanged();

private:
    void onDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                            const QString &category);
    void pump();
    void bump();
    bool isQueuedOrInflight(const QString &url) const;
    void setHeld(QObject *owner, const QStringList &urls);
    bool failureBlocks(const QString &url);
    void markFailed(const QString &url, bool permanent);
    QString store(const QString &url, const QByteArray &bytes);
    void evict();
    static bool acceptableUrl(const QString &url);

    QPointer<GifTransport> m_transport;
    std::unique_ptr<QTemporaryDir> m_dir;

    struct Entry {
        QString path;
        qint64 bytes = 0;
    };
    QHash<QString, Entry> m_files; // url -> validated copy
    QList<QString> m_lru;          // front = most recent
    qint64 m_totalBytes = 0;

    QQueue<QString> m_stillQueue;
    QQueue<QString> m_previewQueue;
    struct Fetch {
        QString url;
        qint64 startedMs = 0;
    };
    QHash<quint64, Fetch> m_inflight; // op id -> fetch
    QTimer m_watchdog;
    qint64 m_fetchTimeoutMs = kFetchTimeoutMs;
    bool m_active = false;

    QHash<QObject *, QStringList> m_holders; // never dereferenced
    QHash<QString, int> m_holds;             // url -> holder count

    struct Failure {
        qint64 atMs = 0;
        bool permanent = false;
    };
    QHash<QString, Failure> m_failed;
    QElapsedTimer m_clock;

    int m_revision = 0;
    int m_maxEntries = 256;
    qint64 m_maxBytes = 64 * 1024 * 1024;
    static constexpr int kMaxFailureMarks = 512;
    qint64 m_retryAfterMs = 60 * 1000;
    // Re-evaluates sources once transient failures may be retried.
    QTimer m_retryTimer;
};
