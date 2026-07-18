#include "gif/GifRecentModel.h"

GifRecentModel::GifRecentModel(QSettings *settings, QObject *parent)
    : GifStoredModel(settings, QStringLiteral("gif/recent"), kMaxRecent, parent)
{
}

void GifRecentModel::setRecordingEnabled(bool on)
{
    if (m_recording == on)
        return;
    m_recording = on;
    Q_EMIT recordingEnabledChanged();
}

void GifRecentModel::recordSent(const gif::GifResult &r)
{
    if (!m_recording)
        return; // history disabled — do not record; favorites untouched
    if (r.provider.isEmpty() || r.id.isEmpty())
        return;
    insertFront(r); // newest first, dedup-to-front, bounded
}
