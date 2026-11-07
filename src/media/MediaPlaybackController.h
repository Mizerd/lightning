#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class MediaPlaybackController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString audibleOwner READ audibleOwner
                   NOTIFY audibleOwnerChanged)
    // Count of stopAll() calls, so a stop that lands while audibleOwner is
    // already empty still pauses cards.
    Q_PROPERTY(int stopGeneration READ stopGeneration
                   NOTIFY audibleOwnerChanged)
    // Bumped whenever a remembered video position changes, so a QML binding
    // that asks videoResumePosition()/videoFinished() through a function call
    // can depend on it and re-evaluate (a call alone binds to nothing).
    Q_PROPERTY(int resumeRevision READ resumeRevision
                   NOTIFY resumePositionsChanged)
    Q_PROPERTY(qreal videoPlaybackRate READ videoPlaybackRate
                   WRITE setVideoPlaybackRate
                   NOTIFY videoPlaybackRateChanged)

public:
    explicit MediaPlaybackController(QObject *parent = nullptr);

    static constexpr qint64 kMinResumeMs = 3000;
    static constexpr qint64 kEndSlackMs = 2000;
    static constexpr int kMaxResumeEntries = 256;

    QString audibleOwner() const { return m_audibleOwner; }
    int stopGeneration() const { return m_stopGeneration; }
    int resumeRevision() const { return m_resumeRevision; }
    qreal videoPlaybackRate() const { return m_videoPlaybackRate; }
    void setVideoPlaybackRate(qreal rate);

    Q_INVOKABLE void acquire(const QString &ownerKey);
    Q_INVOKABLE void release(const QString &ownerKey);
    Q_INVOKABLE bool owns(const QString &ownerKey) const;
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void requestTogglePlayPause();

    Q_INVOKABLE void rememberVideoPosition(const QString &mediaKey,
                                           qint64 positionMs,
                                           qint64 durationMs);
    Q_INVOKABLE void markVideoFinished(const QString &mediaKey);
    Q_INVOKABLE qint64 videoResumePosition(const QString &mediaKey) const;
    Q_INVOKABLE bool videoFinished(const QString &mediaKey) const;
    Q_INVOKABLE void forgetVideoPosition(const QString &mediaKey);
    Q_INVOKABLE void forgetResumePositions();

    // A video's picked audio and subtitle track; -1 / -2 mean "not picked".
    Q_INVOKABLE void rememberVideoTracks(const QString &mediaKey, int audio,
                                         int subtitle);
    Q_INVOKABLE QVariantList videoTracks(const QString &mediaKey) const;

Q_SIGNALS:
    void audibleOwnerChanged();
    void resumePositionsChanged();
    void videoPlaybackRateChanged();
    void togglePlayPauseRequested(const QString &ownerKey);

private:
    static constexpr qint64 kFinished = -1;
    void storeResume(const QString &mediaKey, qint64 value);

    QString m_audibleOwner;
    int m_stopGeneration = 0;
    int m_resumeRevision = 0;
    qreal m_videoPlaybackRate = 1.0;
    QHash<QString, qint64> m_resume;
    QStringList m_resumeOrder;
    QHash<QString, QPair<int, int>> m_tracks;
    QStringList m_tracksOrder;
};
