// Plays the call sounds with QSoundEffect, on the GUI thread.
//
// Not on a worker thread: Qt 6.11's QSoundEffect engine (QRtAudioEngine)
// moves itself to the application thread when created elsewhere, but its
// eventfd socket notifier stays registered with the creating thread's event
// loop. Tearing the engine down (stopping the ring loop) then leaves that
// loop polling a closed fd through a dangling notifier and the process
// crashes. Mixing already runs on Qt's own real-time audio thread.
//
// Every sound is preloaded so the ringer can be relied on the moment a call
// arrives; canPlay() reports what reached QSoundEffect::Ready. Only bundled
// sound names are accepted.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include "calls/CallSoundController.h"

class QSoundEffect;

class CallSoundPlayer : public QObject, public CallSoundSink
{
    Q_OBJECT

public:
    /// `callSpeakerId` returns the call's preferred output device id, or ""
    /// for the system default.
    explicit CallSoundPlayer(std::function<QString()> callSpeakerId,
                             QObject *parent = nullptr);
    ~CallSoundPlayer() override;

    void play(const QString &sound, qreal volume, bool inCall) override;
    void loop(const QString &sound, qreal volume, bool inCall) override;
    bool canPlay(const QString &sound) const override;

    /// Every sound this build bundles (data/sounds/<name>.wav).
    static const QStringList &knownSounds();

private:
    QSoundEffect *effect(const QString &sound);
    void route(QSoundEffect *effect, bool inCall);

    std::function<QString()> m_callSpeakerId;
    QHash<QString, QSoundEffect *> m_effects;
    QString m_loopSound;
};
