#include "media/VideoPosterExtractor.h"

#include <QBuffer>
#include <QImage>
#include <QLoggingCategory>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPoster, "lightning.media.poster")

namespace {
// A frame that never arrives (broken container, missing codec, hostile
// file) must not wedge the single extraction slot. Local-file decode of the
// first frame is fast; only a genuinely undecodable input reaches this.
constexpr int kExtractTimeoutMs = 6000;

// A poster grabbed from a fade-in's lead-in is a solid black card
// (maintainer screenshot: a logo animation rendered an all-black poster).
// Frames whose brightest downsampled pixel stays below this are treated
// as lead-in and skipped — bounded by kMaxSkippedFrames, and the LAST
// skipped frame is kept as the fallback so an all-black video still gets
// its honest (black) poster rather than a failure.
constexpr int kBlackMaxLuma = 24;
constexpr int kMaxSkippedFrames = 90; // ~1.5s at 60fps

bool frameLooksBlack(const QImage &image)
{
    const QImage tiny = image.scaled(8, 8, Qt::IgnoreAspectRatio,
                                     Qt::FastTransformation);
    int maxLuma = 0;
    for (int y = 0; y < tiny.height(); ++y) {
        for (int x = 0; x < tiny.width(); ++x) {
            maxLuma = std::max(maxLuma, qGray(tiny.pixel(x, y)));
        }
    }
    return maxLuma < kBlackMaxLuma;
}

QByteArray encodePoster(const QImage &image, int maxEdge, int quality)
{
    QImage scaled = image;
    if (scaled.width() > maxEdge || scaled.height() > maxEdge)
        scaled = scaled.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!scaled.save(&buffer, "JPG", quality))
        jpeg.clear();
    return jpeg;
}
} // namespace

VideoPosterExtractor::VideoPosterExtractor(QObject *parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kExtractTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        qCDebug(lcPoster, "poster extraction timed out");
        finishWithBestAvailable();
    });
}

VideoPosterExtractor::~VideoPosterExtractor()
{
    teardownPlayer();
}

void VideoPosterExtractor::requestPoster(const QString &tag,
                                         const QString &filePath)
{
    if (tag.isEmpty() || filePath.isEmpty())
        return;
    if (m_active && m_activeTag == tag)
        return;
    for (const Job &job : std::as_const(m_queue)) {
        if (job.tag == tag)
            return;
    }
    m_queue.append({tag, filePath});
    if (!m_active)
        startNext();
}

void VideoPosterExtractor::startNext()
{
    if (m_active || m_queue.isEmpty())
        return;
    const Job job = m_queue.takeFirst();
    m_active = true;
    m_activeTag = job.tag;
    m_frameSeen = false;

    m_sink = std::make_unique<QVideoSink>();
    m_player = std::make_unique<QMediaPlayer>();
    m_player->setVideoSink(m_sink.get());
    // No AudioOutput is attached: decode is silent by construction and the
    // audio backend is never spun up for a poster grab.

    // Queued completions carry the job's tag (review M2): a decoder can
    // emit more than one terminal signal for one job, and an untagged
    // second callback would terminate the NEXT job with an empty poster.
    const QString jobTag = m_activeTag;
    m_skippedFrames = 0;
    m_fallbackFrame = QImage();
    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
            [this, jobTag](const QVideoFrame &frame) {
                if (!m_active || m_activeTag != jobTag || m_frameSeen
                    || !frame.isValid())
                    return;
                const QImage image = frame.toImage();
                if (image.isNull())
                    return; // wait for a decodable frame
                // Skip a fade-in's black lead-in (bounded): keep the last
                // rejected frame so an all-black video still posters.
                if (frameLooksBlack(image)
                    && m_skippedFrames < kMaxSkippedFrames) {
                    ++m_skippedFrames;
                    m_fallbackFrame = image;
                    return;
                }
                m_frameSeen = true;
                const QByteArray jpeg =
                    encodePoster(image, kMaxEdge, kJpegQuality);
                // Queued: never tear the player down from inside its own
                // frame callback.
                QMetaObject::invokeMethod(
                    this,
                    [this, jobTag, jpeg] {
                        if (m_active && m_activeTag == jobTag)
                            finishActive(jpeg);
                    },
                    Qt::QueuedConnection);
            });
    connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this,
            [this, jobTag](QMediaPlayer::MediaStatus status) {
                // A short clip can end before any non-black frame arrived;
                // deliver the honest fallback instead of idling to timeout.
                if (status != QMediaPlayer::EndOfMedia)
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this, jobTag] {
                        if (m_active && m_activeTag == jobTag && !m_frameSeen)
                            finishWithBestAvailable();
                    },
                    Qt::QueuedConnection);
            });
    connect(m_player.get(), &QMediaPlayer::errorOccurred, this,
            [this, jobTag](QMediaPlayer::Error, const QString &) {
                // Error text can embed a file path; log nothing of it.
                qCDebug(lcPoster, "poster extraction failed (decoder error)");
                QMetaObject::invokeMethod(
                    this,
                    [this, jobTag] {
                        if (m_active && m_activeTag == jobTag)
                            finishActive({});
                    },
                    Qt::QueuedConnection);
            });

    m_timeout.start();
    m_player->setSource(QUrl::fromLocalFile(job.path));
    m_player->play();
}

void VideoPosterExtractor::finishWithBestAvailable()
{
    // The last skipped lead-in frame is better than no poster: an
    // all-black video honestly gets a black poster.
    if (!m_fallbackFrame.isNull()) {
        finishActive(encodePoster(m_fallbackFrame, kMaxEdge, kJpegQuality));
        return;
    }
    finishActive({});
}

void VideoPosterExtractor::finishActive(const QByteArray &jpeg)
{
    if (!m_active)
        return;
    m_timeout.stop();
    const QString tag = m_activeTag;
    m_active = false;
    m_activeTag.clear();
    teardownPlayer();
    Q_EMIT posterReady(tag, jpeg);
    startNext();
}

void VideoPosterExtractor::teardownPlayer()
{
    if (m_player) {
        m_player->stop();
        m_player->setVideoSink(nullptr);
    }
    m_player.reset();
    m_sink.reset();
}
