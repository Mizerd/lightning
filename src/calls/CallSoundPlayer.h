// The one piece of the call-sound feature that makes a noise: Qt
// Multimedia's QSoundEffect, on a thread of its own.
//
// WHY A THREAD. The first Qt Multimedia object in a process pays the
// backend's lazy initialisation — measured at ~931 ms for the first
// QVideoSink here (§16 / the poster-extractor round) — and an incoming call
// is exactly the moment the GUI must not stall. Every QSoundEffect is
// created, loaded and played on the worker; the GUI thread only posts
// requests and reads an atomic "loaded" set.
//
// WHY PRELOAD. A ring must be audible the instant it is announced, and
// whether it CAN be heard decides whether AppController keeps the desktop's
// themed call sound as a fallback (CallSoundController::ringerAvailable).
// So every sound is loaded when the player starts, and `canPlay` answers
// from what actually reached QSoundEffect::Ready.
//
// WHERE IT PLAYS. In-call sounds follow the call's chosen speaker
// (SettingsManager::preferredSpeakerId, which stores QAudioDevice::id()
// verbatim); the ringer plays on the system default output. Both are
// re-resolved on every play, so a dock or headset plugged in mid-session is
// followed rather than remembered.
//
// ONLY KNOWN NAMES. A name that is not one of the bundled sounds is ignored,
// so nothing reaching `play` can become a resource path of its choosing.
#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>

#include <functional>

#include "calls/CallSoundController.h"

class QSoundEffect;
class CallSoundWorker;

class CallSoundPlayer : public CallSoundSink
{
public:
    /// `callSpeakerId` answers the call's preferred output device id ("" =
    /// system default). It is read on the GUI thread at each request.
    explicit CallSoundPlayer(std::function<QString()> callSpeakerId);
    ~CallSoundPlayer() override;

    void play(const QString &sound, qreal volume, bool inCall) override;
    void loop(const QString &sound, qreal volume, bool inCall) override;
    bool canPlay(const QString &sound) const override;

    /// Every sound this build bundles (data/sounds/<name>.wav).
    static const QStringList &knownSounds();

private:
    friend class CallSoundWorker;
    void markLoaded(const QString &sound, bool loaded);

    std::function<QString()> m_callSpeakerId;
    QThread m_thread;
    CallSoundWorker *m_worker = nullptr; // lives on m_thread
    mutable QMutex m_loadedLock;
    QSet<QString> m_loaded;
};

/// Implementation detail of CallSoundPlayer; declared here only so moc sees
/// it. Every member runs on the player's thread.
class CallSoundWorker : public QObject
{
    Q_OBJECT

public:
    explicit CallSoundWorker(CallSoundPlayer *owner);

    Q_INVOKABLE void preload();
    Q_INVOKABLE void play(const QString &sound, qreal volume,
                          const QString &deviceId);
    Q_INVOKABLE void loop(const QString &sound, qreal volume,
                          const QString &deviceId);
    Q_INVOKABLE void shutdown();

private:
    QSoundEffect *effect(const QString &sound);
    void route(QSoundEffect *effect, const QString &deviceId);

    CallSoundPlayer *m_owner;
    QHash<QString, QSoundEffect *> m_effects;
    QString m_loopSound;
};
