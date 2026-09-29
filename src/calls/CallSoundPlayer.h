// Plays the call sounds with QSoundEffect.
//
// Where the sound effects live depends on Qt:
//
// * Qt before 6.10: QSoundEffect opens a QAudioSink on play(), on the
//   effect's own thread, and with a stalled sound server that open waits for
//   the server's 30 s timeout. On the GUI thread that froze the whole app in
//   the middle of joining a call (measured 2026-09-25: the "connected" cue
//   held it for 30.3 s, and the call's signalling with it). So the effects
//   live on a thread of their own. After such a stall Qt 6.8 never gives that
//   thread back (its sink keeps waiting on a stream that already failed), so
//   cues stay silent for the rest of the session while the UI and the call
//   carry on; see soundThreadStuck() for what that means at quit.
// * Qt 6.10 and later: QSoundEffect's engine (QRtAudioEngine) moves itself
//   to the application thread when created elsewhere, but its eventfd socket
//   notifier stays registered with the creating thread's event loop, and
//   tearing the engine down then crashes that loop. So the effects stay on
//   the GUI thread there. Mixing already runs on Qt's real-time audio thread.
//
// Every sound is preloaded so the ringer can be relied on the moment a call
// arrives; canPlay() reports what reached QSoundEffect::Ready. Only bundled
// sound names are accepted.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "calls/CallSoundController.h"

class QThread;
class CallSoundEffects;
struct CallSoundReadiness;

class CallSoundPlayer : public QObject, public CallSoundSink
{
    Q_OBJECT

public:
    /// `callSpeakerId` returns the call's preferred output device id, or ""
    /// for the system default. Always called on the GUI thread.
    explicit CallSoundPlayer(std::function<QString()> callSpeakerId,
                             QObject *parent = nullptr);
    ~CallSoundPlayer() override;

    void play(const QString &sound, qreal volume, bool inCall) override;
    void loop(const QString &sound, qreal volume, bool inCall) override;
    bool canPlay(const QString &sound) const override;

    /// Every sound this build bundles (data/sounds/<name>.wav).
    static const QStringList &knownSounds();
    /// Whether this build keeps the effects off the GUI thread; see above.
    static bool playsOffTheGuiThread();
    /// A player was destroyed with its sound thread still stuck on the sound
    /// server. The process then ends as soon as the application object is
    /// torn down, without Qt's own teardown: freeing the Pulse mainloop the
    /// thread still waits on aborts inside libpulse (measured: a core dump on
    /// quit after every stall).
    static bool soundThreadStuck();
    /// The status the process exits with when it has to end that way; main()
    /// records it after the event loop returns.
    static void setExitStatus(int status);

    /// Test-only: a player with no preload, its effects on their own thread
    /// or not.
    struct ForTest {};
    CallSoundPlayer(ForTest, bool offTheGuiThread);
    /// Test-only: run `job` where the effects live, as a cue would.
    void postForTest(std::function<void()> job);
    /// Test-only: mark `sound` loaded without a sound effect.
    void markReadyForTest(const QString &sound);

private:
    void startEffects(bool offTheGuiThread);
    /// Run `job` where the effects live, never waiting for it.
    void post(std::function<void(CallSoundEffects *)> job);

    std::function<QString()> m_callSpeakerId;
    std::shared_ptr<CallSoundReadiness> m_readiness;
    QThread *m_thread = nullptr;
    CallSoundEffects *m_effects = nullptr;
    /// The looping sound, mirrored here so play() can refuse a one-shot of
    /// it without asking the effects' thread.
    QString m_loopSound;
};
