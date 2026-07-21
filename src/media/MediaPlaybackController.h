#pragma once

#include <QObject>
#include <QString>

// v0.7: shared inline-playback coordinator (video/audio/voice cards).
//
// The actual QMediaPlayer instances live in QML, one per card, behind
// Loaders that only activate on user intent. This object is the single
// authority for WHICH card may be audible: cards acquire() their stable
// owner key (event identity, never a visual index) when playback starts,
// observe audibleOwnerChanged, and pause themselves the moment another
// card takes over — one audible item at a time, no delegate-reuse
// cross-talk. AppController owns one instance per application session and
// calls stopAll() on room switches, account switches, and sign-out so no
// playback (or decrypted media access) survives a context change.
class MediaPlaybackController : public QObject
{
    Q_OBJECT
    // Stable owner key of the card currently allowed to play audibly;
    // empty when nothing plays.
    Q_PROPERTY(QString audibleOwner READ audibleOwner
                   NOTIFY audibleOwnerChanged)
    // Monotonic count of stopAll() calls. Cards also bind to this so a
    // stop that lands while audibleOwner is already empty (e.g. a room
    // switch racing a just-released card) still forces local pause state.
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

Q_SIGNALS:
    void audibleOwnerChanged();

private:
    QString m_audibleOwner;
    int m_stopGeneration = 0;
};
