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

// v0.7 perf round: offline poster-frame extraction for videos that carry no
// Matrix thumbnail. Given a locally materialized (already decrypted,
// magic-validated) video file, decodes the first presentable frame with an
// offscreen QMediaPlayer + QVideoSink, downscales it, and returns it as
// encoded JPEG bytes. MediaBridge feeds those bytes into its ordinary
// in-memory image cache under the event's "thumb:" key, so the timeline
// cover renders a real poster through the existing image path — no live
// decoder is held per row, and nothing is written to disk.
//
// One extraction runs at a time (a queue bounds decoder pressure); each job
// is bounded by a hard timeout so a hostile or undecodable file can never
// wedge the pipeline. Failure is terminal per request — the caller decides
// whether to re-request. The class touches only the local file path it is
// given; it never sees URLs, keys, or network state.
//
// ALL decoding runs on a private worker thread (2026-08-17). Measured on the
// maintainer's clip (1254x1254, 59.94 fps, 3.0 s) with a heartbeat timer on
// the GUI thread: the FIRST QVideoSink constructed in a process costs 931 ms
// — lazy Qt Multimedia backend initialization, including a hardware-decoder
// probe that fails on machines without VAAPI — and ~QMediaPlayer another
// ~68 ms per job, with ~232 ms of frame conversion in between. On the GUI
// thread that was a ~1.2 s freeze the moment a video scrolled into view.
// Off it, the same extraction measured a worst GUI-thread stall of 1 ms.
// The backend initialization is process-global, so warming it here also
// makes the first inline playback (which constructs its sink on the GUI
// thread, and cannot avoid doing so) free: measured 0.0 ms afterwards.
//
// This object stays on its creator's thread — construct, destroy, call and
// connect to it exactly as before; the threading is internal.
class VideoPosterExtractor : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterExtractor(QObject *parent = nullptr);
    ~VideoPosterExtractor() override;

    /// Give up the worker thread WITHOUT waiting for it.
    ///
    /// The destructor's join is bounded by the longest call the worker can be
    /// inside, and that is ~931 ms the first time Qt Multimedia initialises
    /// its backend (measured; see above). MediaBridge destroys this object on
    /// every ACCOUNT SWITCH for session isolation, so that join sat on the
    /// GUI thread during exactly the operation that must not block.
    ///
    /// Isolation is unaffected: quit() still drains the worker's loop and
    /// runs its queued deleteLater, so the decoder is torn down on its own
    /// thread and none outlives the account whose file it read. What changes
    /// is only that the caller does not stand there for it — and a late
    /// result is already inert, because MediaBridge disconnects this object
    /// and clears its tracking map first.
    void retireWithoutWaiting();

    // Queue a poster grab for `filePath`, reported back with `tag` (the
    // caller's media key). Duplicate tags already queued or active are
    // dropped. Emits posterReady(tag, jpeg, ...) — jpeg is empty on failure.
    // Safe to call from this object's thread; the work is forwarded to the
    // worker thread and the reply arrives back on this thread.
    void requestPoster(const QString &tag, const QString &filePath);

    // Pay the one-time Qt Multimedia backend initialization on the worker
    // thread, ahead of need. It is process-global, so this also spares the
    // first INLINE playback — whose sink is built by QML on the GUI thread
    // and cannot be moved off it — the same ~931 ms freeze. Idempotent and
    // safe to call when already warm (measured 0.0 ms).
    void warmUp();

    // Longest edge of the emitted poster and its JPEG quality; fixed —
    // posters are timeline covers, not full-fidelity stills.
    static constexpr int kMaxEdge = 640;
    static constexpr int kJpegQuality = 85;

Q_SIGNALS:
    // `posterSize` is the emitted JPEG's own geometry and `sourceSize` the
    // decoded frame's, both as presented by Qt (so a rotated container is
    // already upright). `durationMs` is the clip length, 0 when the decoder
    // never reported one. The three trailing values exist for the SEND
    // path, which must declare honest thumbnail and video metadata on the
    // outgoing Matrix event; the receive path connects a two-argument slot
    // and ignores them. All are 0/invalid when `jpeg` is empty.
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    QThread *m_thread = nullptr;
    VideoPosterWorker *m_worker = nullptr;
};

// The decoding half, which lives on VideoPosterExtractor's worker thread.
// Declared here only because the project builds no per-translation-unit moc;
// nothing outside VideoPosterExtractor should construct or call it.
class VideoPosterWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterWorker(QObject *parent = nullptr);
    ~VideoPosterWorker() override;

public Q_SLOTS:
    // Both run on the worker thread (queued from VideoPosterExtractor).
    void enqueue(const QString &tag, const QString &filePath);
    void warmUp();

Q_SIGNALS:
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    void startNext();
    // Terminal per job; queued (never re-entered from a sink callback).
    void finishActive(const QByteArray &jpeg, const QSize &posterSize,
                      const QSize &sourceSize, qint64 durationMs);
    // Terminal via the fallback frame when no presentable frame arrived
    // (timeout, or a short clip that ended inside its black lead-in).
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
    // Black-lead-in skipping (fade-ins postered as solid black cards):
    // frames rejected as lead-in are counted and the LAST one kept as the
    // fallback poster.
    int m_skippedFrames = 0;
    QImage m_fallbackFrame;
    // Latest non-black frame seen before the target timestamp — the
    // poster candidate (a fade-in keeps improving until fully faded in).
    QImage m_bestFrame;
    // Latest duration the decoder reported for the active job, in ms; 0
    // while unknown. Read at completion, never from the sink callback.
    qint64 m_durationMs = 0;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_sink;
    // A CHILD, so moveToThread() carries it to the worker thread with the
    // rest of the object. A plain member would keep the creating thread's
    // affinity and Qt would refuse to start it ("Timers cannot be started
    // from another thread"), silently disarming the watchdog that stops a
    // hostile file wedging the single extraction slot.
    QTimer *m_timeout = nullptr;
    // The backend initializes once per PROCESS, so this stays true across
    // sign-out and account switches — a later warm-up genuinely has
    // nothing left to do.
    bool m_warmed = false;
};
