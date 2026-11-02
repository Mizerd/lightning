#include "media/VideoPosterExtractor.h"
#include "media/ShutdownThreadJoin.h"

#include <QBuffer>
#include <QImage>
#include <QLoggingCategory>
#include <QMediaPlayer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPoster, "lightning.media.poster")

namespace {
constexpr int kExtractTimeoutMs = 6000;

constexpr int kBlackMaxLuma = 24;
constexpr int kMaxSkippedFrames = 90;

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

QByteArray encodePoster(const QImage &image, int maxEdge, int quality,
                        QSize *encodedSize = nullptr)
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
    if (encodedSize)
        *encodedSize = jpeg.isEmpty() ? QSize() : scaled.size();
    return jpeg;
}
}

VideoPosterExtractor::VideoPosterExtractor(QObject *parent)
    : QObject(parent)
    , m_thread(new QThread)
    , m_worker(new VideoPosterWorker)
{
    m_thread->setObjectName(QStringLiteral("lightning-poster"));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &VideoPosterWorker::posterReady,
            this, &VideoPosterExtractor::posterReady);
    m_thread->start();
}

void VideoPosterExtractor::retireWithoutWaiting()
{
    if (!m_thread)
        return;
    // quit() still drains the loop and runs the queued deleteLater, so the
    // decoder is destroyed on its own thread. The thread then deletes itself.
    QThread *retiring = m_thread;
    m_thread = nullptr;
    connect(retiring, &QThread::finished, retiring, &QObject::deleteLater);
    retiring->quit();
}

VideoPosterExtractor::~VideoPosterExtractor()
{
    if (!m_thread)
        return;
    if (!lightning::media::quitAndJoinWorkerForShutdown(m_thread, 2000)) {
        qWarning("quit: poster worker exceeded 2000 ms; detaching thread");
        return;
    }
    delete m_thread;
}

void VideoPosterExtractor::requestPoster(const QString &tag,
                                         const QString &filePath)
{
    if (tag.isEmpty() || filePath.isEmpty())
        return;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, tag, filePath] {
        worker->enqueue(tag, filePath);
    });
}

void VideoPosterExtractor::warmUp()
{
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker] { worker->warmUp(); });
}

VideoPosterWorker::VideoPosterWorker(QObject *parent)
    : QObject(parent)
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kExtractTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        qCDebug(lcPoster, "poster extraction timed out");
        finishWithBestAvailable();
    });
}

VideoPosterWorker::~VideoPosterWorker()
{
    teardownPlayer();
}

void VideoPosterWorker::enqueue(const QString &tag, const QString &filePath)
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

void VideoPosterWorker::warmUp()
{
    if (m_warmed)
        return;
    m_warmed = true;
    QVideoSink probe;
    Q_UNUSED(probe);
}

void VideoPosterWorker::startNext()
{
    if (m_active || m_queue.isEmpty())
        return;
    m_warmed = true;
    const Job job = m_queue.takeFirst();
    m_active = true;
    m_activeTag = job.tag;
    m_frameSeen = false;

    m_sink = std::make_unique<QVideoSink>();
    m_player = std::make_unique<QMediaPlayer>();
    m_player->setVideoSink(m_sink.get());

    const QString jobTag = m_activeTag;
    m_skippedFrames = 0;
    m_fallbackFrame = QImage();
    m_bestFrame = QImage();
    m_durationMs = 0;
    connect(m_player.get(), &QMediaPlayer::durationChanged, this,
            [this, jobTag](qint64 durationMs) {
                if (m_active && m_activeTag == jobTag && durationMs > 0)
                    m_durationMs = durationMs;
            });
    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
            [this, jobTag](const QVideoFrame &frame) {
                if (!m_active || m_activeTag != jobTag || m_frameSeen
                    || !frame.isValid())
                    return;
                const QImage image = frame.toImage();
                if (image.isNull())
                    return;
                ++m_skippedFrames;
                if (frameLooksBlack(image)) {
                    if (m_bestFrame.isNull())
                        m_fallbackFrame = image;
                } else {
                    m_bestFrame = image;
                }
                const qint64 durationMs =
                    m_player ? m_player->duration() : 0;
                const qint64 targetUs = 1000
                    * std::min<qint64>(2000, durationMs > 0
                                                 ? durationMs * 2 / 5 : 800);
                const bool reached = frame.startTime() >= 0
                    ? frame.startTime() >= targetUs
                    : m_skippedFrames >= kMaxSkippedFrames;
                if (!reached || m_bestFrame.isNull())
                    return;
                m_frameSeen = true;
                QSize posterSize;
                const QByteArray jpeg = encodePoster(
                    m_bestFrame, VideoPosterExtractor::kMaxEdge,
                    VideoPosterExtractor::kJpegQuality, &posterSize);
                const QSize sourceSize = m_bestFrame.size();
                // Queued: never tear the player down from its own frame
                // callback.
                QMetaObject::invokeMethod(
                    this,
                    [this, jobTag, jpeg, posterSize, sourceSize] {
                        if (m_active && m_activeTag == jobTag)
                            finishActive(jpeg, posterSize, sourceSize,
                                         m_durationMs);
                    },
                    Qt::QueuedConnection);
            });
    connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this,
            [this, jobTag](QMediaPlayer::MediaStatus status) {
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
                qCDebug(lcPoster, "poster extraction failed (decoder error)");
                QMetaObject::invokeMethod(
                    this,
                    [this, jobTag] {
                        if (m_active && m_activeTag == jobTag)
                            finishActive({}, {}, {}, 0);
                    },
                    Qt::QueuedConnection);
            });

    m_timeout->start();
    m_player->setSource(QUrl::fromLocalFile(job.path));
    m_player->play();
}

void VideoPosterWorker::finishWithBestAvailable()
{
    const QImage &frame = !m_bestFrame.isNull() ? m_bestFrame : m_fallbackFrame;
    if (frame.isNull()) {
        finishActive({}, {}, {}, m_durationMs);
        return;
    }
    QSize posterSize;
    const QByteArray jpeg =
        encodePoster(frame, VideoPosterExtractor::kMaxEdge,
                     VideoPosterExtractor::kJpegQuality, &posterSize);
    finishActive(jpeg, posterSize, frame.size(), m_durationMs);
}

void VideoPosterWorker::finishActive(const QByteArray &jpeg,
                                     const QSize &posterSize,
                                     const QSize &sourceSize,
                                     qint64 durationMs)
{
    if (!m_active)
        return;
    m_timeout->stop();
    const QString tag = m_activeTag;
    m_active = false;
    m_activeTag.clear();
    m_durationMs = 0;
    teardownPlayer();
    if (jpeg.isEmpty())
        Q_EMIT posterReady(tag, {}, {}, {}, durationMs);
    else
        Q_EMIT posterReady(tag, jpeg, posterSize, sourceSize, durationMs);
    startNext();
}

void VideoPosterWorker::teardownPlayer()
{
    if (m_player) {
        m_player->stop();
        m_player->setVideoSink(nullptr);
    }
    m_player.reset();
    m_sink.reset();
}
