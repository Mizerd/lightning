#pragma once

#include <QObject>
#include <QString>

// Shared inline-playback coordinator for video, audio and voice cards.
//
// Players live in QML, one per card. This decides which card may be audible:
// a card acquire()s its stable owner key (event identity, never a visual
// index) when playback starts and pauses when audibleOwnerChanged names
// another. AppController calls stopAll() on room switch, account switch and
// sign-out so no playback or decrypted media access survives the change.
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

public:
    explicit MediaPlaybackController(QObject *parent = nullptr);

    QString audibleOwner() const { return m_audibleOwner; }
    int stopGeneration() const { return m_stopGeneration; }

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

Q_SIGNALS:
    void audibleOwnerChanged();
    // Carries the owner key the request was made for, so a card that lost
    // audibility between the key press and the delivery cannot act on it.
    void togglePlayPauseRequested(const QString &ownerKey);

private:
    QString m_audibleOwner;
    int m_stopGeneration = 0;
};
