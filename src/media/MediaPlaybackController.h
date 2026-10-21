#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

// Shared inline-playback coordinator for video, audio and voice cards.
//
// Video players live in QML, one per card; voice and audio clips play in the
// one app-owned VoicePlaybackController. This decides which may be audible: a
// player acquire()s its stable owner key (event identity, never a visual
// index) when playback starts and pauses when audibleOwnerChanged names
// another. AppController calls stopAll() on room switch, account switch and
// sign-out, so no inline video survives the change; the voice player is
// stopped explicitly on account switch and sign-out and deliberately keeps
// playing through a room switch.
//
// A stopped video is not a forgotten one: the card records where it was
// (rememberVideoPosition) and the cover offers to resume from there, so a
// room switch costs the user nothing but the decoder. Positions are keyed by
// media key (the event id), held in RAM for the session only, never
// persisted and bounded; forgetResumePositions() drops them with the
// account. The video speed is session state here too, so a resumed or a new
// player starts at the speed the user last chose and every control bar shows
// the player's real rate.
class MediaPlaybackController : public QObject
{
    Q_OBJECT
    // Stable owner key of the card currently allowed to play audibly;
    // empty when nothing plays.
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
    // The speed the user last chose for video in this session (1.0 until
    // then). Never persisted.
    Q_PROPERTY(qreal videoPlaybackRate READ videoPlaybackRate
                   WRITE setVideoPlaybackRate
                   NOTIFY videoPlaybackRateChanged)

public:
    explicit MediaPlaybackController(QObject *parent = nullptr);

    // Below this a remembered position is not worth offering: starting over
    // is the same thing.
    static constexpr qint64 kMinResumeMs = 3000;
    // A stop this close to the end counts as watched to the end.
    static constexpr qint64 kEndSlackMs = 2000;
    // At most this many videos are remembered; the oldest entry goes first.
    static constexpr int kMaxResumeEntries = 256;

    QString audibleOwner() const { return m_audibleOwner; }
    int stopGeneration() const { return m_stopGeneration; }
    int resumeRevision() const { return m_resumeRevision; }
    qreal videoPlaybackRate() const { return m_videoPlaybackRate; }
    // Clamped to the range the control bars offer (0.25..4).
    void setVideoPlaybackRate(qreal rate);

    // Claim audibility for `ownerKey`. The previous owner (if different)
    // observes the change and pauses. Empty keys are ignored.
    Q_INVOKABLE void acquire(const QString &ownerKey);
    // Release audibility, but only if `ownerKey` still holds it — a card
    // that was superseded must not clear the new owner's claim.
    Q_INVOKABLE void release(const QString &ownerKey);
    // True when `ownerKey` currently holds audibility.
    Q_INVOKABLE bool owns(const QString &ownerKey) const;
    // Stop everything (room/account switch, sign-out, shutdown).
    Q_INVOKABLE void stopAll();
    // Asks the audible card to toggle play/pause (media key). Only a broadcast;
    // cards ignore it unless they own audibility. No-op when nothing plays.
    Q_INVOKABLE void requestTogglePlayPause();

    // Record where the video under `mediaKey` stopped. A position under
    // kMinResumeMs forgets it; one past 0 and within kEndSlackMs of a known
    // duration marks it watched (as markVideoFinished). Empty keys are
    // ignored.
    Q_INVOKABLE void rememberVideoPosition(const QString &mediaKey,
                                           qint64 positionMs,
                                           qint64 durationMs);
    // The video played to its end: its cover offers Replay, not Resume.
    Q_INVOKABLE void markVideoFinished(const QString &mediaKey);
    // Where to resume, or 0 when there is nothing to resume (never seen,
    // forgotten, or watched to the end).
    Q_INVOKABLE qint64 videoResumePosition(const QString &mediaKey) const;
    Q_INVOKABLE bool videoFinished(const QString &mediaKey) const;
    Q_INVOKABLE void forgetVideoPosition(const QString &mediaKey);
    // Account switch and sign-out: the next account starts with nothing.
    Q_INVOKABLE void forgetResumePositions();

Q_SIGNALS:
    void audibleOwnerChanged();
    void resumePositionsChanged();
    void videoPlaybackRateChanged();
    // Carries the owner key the request was made for, so a card that lost
    // audibility between the key press and the delivery cannot act on it.
    void togglePlayPauseRequested(const QString &ownerKey);

private:
    // -1 marks a video watched to the end.
    static constexpr qint64 kFinished = -1;
    void storeResume(const QString &mediaKey, qint64 value);

    QString m_audibleOwner;
    int m_stopGeneration = 0;
    int m_resumeRevision = 0;
    qreal m_videoPlaybackRate = 1.0;
    QHash<QString, qint64> m_resume;
    // Insertion order, oldest first, for the bound.
    QStringList m_resumeOrder;
};
