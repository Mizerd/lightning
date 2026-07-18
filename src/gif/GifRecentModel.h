#pragma once

#include "gif/GifStoredModel.h"

// v0.6.1: locally-persisted "recently used" GIFs. An entry is added only after
// a GIF is successfully handed to the Matrix send pipeline (never on preview or
// a failed/cancelled send). Newest first, deduped by (provider,id), bounded.
// Recording can be turned off; favorites are unaffected either way.
class GifRecentModel : public GifStoredModel
{
    Q_OBJECT
    Q_PROPERTY(bool recordingEnabled READ recordingEnabled
               WRITE setRecordingEnabled NOTIFY recordingEnabledChanged)

public:
    explicit GifRecentModel(QSettings *settings, QObject *parent = nullptr);

    bool recordingEnabled() const { return m_recording; }
    void setRecordingEnabled(bool on);

    // Record a successful send handoff. No-op when recording is disabled.
    void recordSent(const gif::GifResult &r);
    Q_INVOKABLE void recordSentMap(const QVariantMap &result)
    { recordSent(fromVariantMap(result)); }

Q_SIGNALS:
    void recordingEnabledChanged();

private:
    static constexpr int kMaxRecent = 60;
    bool m_recording = true;
};
