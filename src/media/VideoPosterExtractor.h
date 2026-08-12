#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

class QMediaPlayer;
class QVideoSink;

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
class VideoPosterExtractor : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterExtractor(QObject *parent = nullptr);
    ~VideoPosterExtractor() override;

    // Queue a poster grab for `filePath`, reported back with `tag` (the
    // caller's media key). Duplicate tags already queued or active are
    // dropped. Emits posterReady(tag, jpeg) — jpeg is empty on failure.
    void requestPoster(const QString &tag, const QString &filePath);

    // Longest edge of the emitted poster and its JPEG quality; fixed —
    // posters are timeline covers, not full-fidelity stills.
    static constexpr int kMaxEdge = 640;
    static constexpr int kJpegQuality = 85;

Q_SIGNALS:
    void posterReady(const QString &tag, const QByteArray &jpeg);

private:
    void startNext();
    // Terminal per job; queued (never re-entered from a sink callback).
    void finishActive(const QByteArray &jpeg);
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
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_sink;
    QTimer m_timeout;
};
