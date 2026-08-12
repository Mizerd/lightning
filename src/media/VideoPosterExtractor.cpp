#include "media/VideoPosterExtractor.h"

#include <QBuffer>
#include <QImage>
#include <QLoggingCategory>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

Q_LOGGING_CATEGORY(lcPoster, "lightning.media.poster")

namespace {
// A frame that never arrives (broken container, missing codec, hostile
// file) must not wedge the single extraction slot. Local-file decode of the
// first frame is fast; only a genuinely undecodable input reaches this.
constexpr int kExtractTimeoutMs = 6000;
} // namespace

VideoPosterExtractor::VideoPosterExtractor(QObject *parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kExtractTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        qCDebug(lcPoster, "poster extraction timed out");
        finishActive({});
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
    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
            [this, jobTag](const QVideoFrame &frame) {
                if (!m_active || m_activeTag != jobTag || m_frameSeen
                    || !frame.isValid())
                    return;
                const QImage image = frame.toImage();
                if (image.isNull())
                    return; // wait for a decodable frame
                m_frameSeen = true;
                QImage scaled = image;
                if (scaled.width() > kMaxEdge || scaled.height() > kMaxEdge)
                    scaled = scaled.scaled(kMaxEdge, kMaxEdge,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                QByteArray jpeg;
                QBuffer buffer(&jpeg);
                buffer.open(QIODevice::WriteOnly);
                if (!scaled.save(&buffer, "JPG", kJpegQuality))
                    jpeg.clear();
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
