#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <memory>

class QMediaPlayer;
class QThread;
class QTimer;
class QVideoSink;
class VideoPosterWorker;

// Poster-frame extraction for videos without a Matrix thumbnail. Decodes the
// first presentable frame of a local, already-validated file with an
// offscreen QMediaPlayer and QVideoSink, downscales it and returns JPEG bytes,
// which MediaBridge keeps in its in-memory cache. Nothing is written to disk
// and no decoder is held per row.
//
// One extraction at a time, each bounded by a hard timeout so a hostile file
// cannot wedge the pipeline. Failure is terminal per request. Only the given
// local path is ever touched.
//
// Decoding runs on a private worker thread: the first QVideoSink in a process
// costs ~1 s (Qt Multimedia backend init plus a hardware-decoder probe), which
// froze the GUI when a video scrolled into view. The init is process-global,
// so warming it here also makes the first inline playback free.
//
// The object itself stays on its creator's thread.
class VideoPosterExtractor : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterExtractor(QObject *parent = nullptr);
    ~VideoPosterExtractor() override;

    /// Gives up the worker thread without joining it. The join can take ~1 s
    /// during backend init, and MediaBridge drops this object on every account
    /// switch on the GUI thread. quit() still tears the decoder down on its own
    /// thread, and late results are inert because MediaBridge disconnects
    /// first.
    void retireWithoutWaiting();

    // Queues a poster grab for `filePath`, reported with `tag` (the media key).
    // Duplicate tags are dropped. Emits posterReady(tag, jpeg, ...); jpeg is
    // empty on failure. The reply arrives on this object's thread.
    void requestPoster(const QString &tag, const QString &filePath);

    // Pays the one-time backend initialization on the worker thread, sparing
    // the first inline playback (whose sink QML builds on the GUI thread).
    // Idempotent.
    void warmUp();

    // Posters are timeline covers, not full-fidelity stills.
    static constexpr int kMaxEdge = 640;
    static constexpr int kJpegQuality = 85;

Q_SIGNALS:
    // `posterSize` is the JPEG's geometry and `sourceSize` the decoded frame's,
    // both upright. `durationMs` is the clip length, 0 if never reported. The
    // trailing values serve the send path, which declares thumbnail and video
    // metadata; the receive path ignores them. The sizes are invalid when
    // `jpeg` is empty, but `durationMs` is still valid (audio files have a
    // length and no frame).
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    QThread *m_thread = nullptr;
    VideoPosterWorker *m_worker = nullptr;
};

// The decoding half, living on the worker thread. Declared here only because
// the build has no per-translation-unit moc; internal to VideoPosterExtractor.
class VideoPosterWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterWorker(QObject *parent = nullptr);
    ~VideoPosterWorker() override;

public Q_SLOTS:
    // Both run on the worker thread.
    void enqueue(const QString &tag, const QString &filePath);
    void warmUp();

Q_SIGNALS:
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    void startNext();
    // Terminal per job; queued, never re-entered from a sink callback.
    void finishActive(const QByteArray &jpeg, const QSize &posterSize,
                      const QSize &sourceSize, qint64 durationMs);
    // Falls back to the best frame seen when none was presentable (timeout, or
    // a short clip ending inside its black lead-in).
    void finishWithBestAvailable();
    void teardownPlayer();

    struct Job {
        QString tag;
        QString path;
    };
    QList<Job> m_queue;
    bool m_active = false;
    QString m_activeTag;
    bool m_frameSeen = false;
    // Black lead-in skipping: rejected frames are counted and the last one kept
    // as a fallback poster.
    int m_skippedFrames = 0;
    QImage m_fallbackFrame;
    // Latest non-black frame before the target timestamp.
    QImage m_bestFrame;
    // Duration reported for the active job (ms), 0 while unknown.
    qint64 m_durationMs = 0;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_sink;
    // A child, so moveToThread() carries it along; a plain member keeps the old
    // affinity and cannot be started, silently disabling the timeout.
    QTimer *m_timeout = nullptr;
    // Process-global backend init, so this survives sign-out and switches.
    bool m_warmed = false;
};
