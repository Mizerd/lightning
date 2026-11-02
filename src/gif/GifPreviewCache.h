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
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit GifPreviewCache(QObject *parent = nullptr);
    ~GifPreviewCache() override;

    void setTransport(GifTransport *transport);

    Q_INVOKABLE QString source(const QString &url, bool still = false);
    Q_INVOKABLE void hold(QObject *owner, const QStringList &urls);
    Q_INVOKABLE void dropQueued();
    void setActive(bool active);
    bool isActive() const { return m_active; }
    void expireStaleFetches();
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
    static constexpr qint64 kMaxPreviewBytes = 8 * 1024 * 1024;
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
    QHash<QString, Entry> m_files;
    QList<QString> m_lru;
    qint64 m_totalBytes = 0;

    QQueue<QString> m_stillQueue;
    QQueue<QString> m_previewQueue;
    struct Fetch {
        QString url;
        qint64 startedMs = 0;
    };
    QHash<quint64, Fetch> m_inflight;
    QTimer m_watchdog;
    qint64 m_fetchTimeoutMs = kFetchTimeoutMs;
    bool m_active = false;

    QHash<QObject *, QStringList> m_holders;
    QHash<QString, int> m_holds;

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
    QTimer m_retryTimer;
};
