#include "media/MediaPlaybackController.h"

#include <QLoggingCategory>

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

void MediaPlaybackController::stopAll()
{
    // Counts only — never event ids in logs.
    qCDebug(lcPlayback) << "stopAll; had owner:" << !m_audibleOwner.isEmpty();
    m_audibleOwner.clear();
    ++m_stopGeneration;
    Q_EMIT audibleOwnerChanged();
}
