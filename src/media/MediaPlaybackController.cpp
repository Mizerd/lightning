#include "media/MediaPlaybackController.h"

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPlayback, "matrix.media.playback")

MediaPlaybackController::MediaPlaybackController(QObject *parent)
    : QObject(parent)
{
}

void MediaPlaybackController::acquire(const QString &ownerKey)
{
    if (ownerKey.isEmpty() || ownerKey == m_audibleOwner)
        return;
    m_audibleOwner = ownerKey;
    Q_EMIT audibleOwnerChanged();
}

void MediaPlaybackController::release(const QString &ownerKey)
{
    if (ownerKey.isEmpty() || ownerKey != m_audibleOwner)
        return;
    m_audibleOwner.clear();
    Q_EMIT audibleOwnerChanged();
}

bool MediaPlaybackController::owns(const QString &ownerKey) const
{
    return !ownerKey.isEmpty() && ownerKey == m_audibleOwner;
}

void MediaPlaybackController::requestTogglePlayPause()
{
    if (m_audibleOwner.isEmpty())
        return;
    Q_EMIT togglePlayPauseRequested(m_audibleOwner);
}

void MediaPlaybackController::stopAll()
{
    qCDebug(lcPlayback) << "stopAll; had owner:" << !m_audibleOwner.isEmpty();
    m_audibleOwner.clear();
    ++m_stopGeneration;
    Q_EMIT audibleOwnerChanged();
}

void MediaPlaybackController::setVideoPlaybackRate(qreal rate)
{
    if (!(rate > 0.0))
        rate = 1.0;
    rate = std::clamp(rate, 0.25, 4.0);
    if (qFuzzyCompare(rate, m_videoPlaybackRate))
        return;
    m_videoPlaybackRate = rate;
    Q_EMIT videoPlaybackRateChanged();
}

void MediaPlaybackController::storeResume(const QString &mediaKey,
                                          qint64 value)
{
    const auto it = m_resume.constFind(mediaKey);
    if (it != m_resume.cend() && it.value() == value)
        return;
    if (it == m_resume.cend()) {
        m_resumeOrder.append(mediaKey);
        while (m_resumeOrder.size() > kMaxResumeEntries)
            m_resume.remove(m_resumeOrder.takeFirst());
    }
    m_resume.insert(mediaKey, value);
    ++m_resumeRevision;
    Q_EMIT resumePositionsChanged();
}

void MediaPlaybackController::rememberVideoPosition(const QString &mediaKey,
                                                    qint64 positionMs,
                                                    qint64 durationMs)
{
    if (mediaKey.isEmpty())
        return;
    // "Near the end" needs a real position: on a clip shorter than the
    // slack, a stop at 0 would otherwise count as watched.
    if (durationMs > 0 && positionMs > 0
        && positionMs >= durationMs - kEndSlackMs) {
        markVideoFinished(mediaKey);
        return;
    }
    if (positionMs < kMinResumeMs) {
        forgetVideoPosition(mediaKey);
        return;
    }
    storeResume(mediaKey, positionMs);
}

void MediaPlaybackController::markVideoFinished(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    storeResume(mediaKey, kFinished);
}

qint64 MediaPlaybackController::videoResumePosition(
    const QString &mediaKey) const
{
    const qint64 value = m_resume.value(mediaKey, 0);
    return value > 0 ? value : 0;
}

bool MediaPlaybackController::videoFinished(const QString &mediaKey) const
{
    return !mediaKey.isEmpty() && m_resume.value(mediaKey, 0) == kFinished;
}

void MediaPlaybackController::forgetVideoPosition(const QString &mediaKey)
{
    if (!m_resume.remove(mediaKey))
        return;
    m_resumeOrder.removeOne(mediaKey);
    ++m_resumeRevision;
    Q_EMIT resumePositionsChanged();
}

void MediaPlaybackController::rememberVideoTracks(const QString &mediaKey,
                                                  int audio, int subtitle)
{
    if (mediaKey.isEmpty())
        return;
    if (!m_tracks.contains(mediaKey)) {
        m_tracksOrder.append(mediaKey);
        while (m_tracksOrder.size() > kMaxResumeEntries)
            m_tracks.remove(m_tracksOrder.takeFirst());
    }
    m_tracks.insert(mediaKey, {audio, subtitle});
}

QVariantList MediaPlaybackController::videoTracks(const QString &mediaKey) const
{
    const auto picked = m_tracks.value(mediaKey, {-1, -2});
    return {picked.first, picked.second};
}

void MediaPlaybackController::forgetResumePositions()
{
    m_tracks.clear();
    m_tracksOrder.clear();
    if (m_resume.isEmpty())
        return;
    qCDebug(lcPlayback) << "forgetting resume positions:" << m_resume.size();
    m_resume.clear();
    m_resumeOrder.clear();
    ++m_resumeRevision;
    Q_EMIT resumePositionsChanged();
}
