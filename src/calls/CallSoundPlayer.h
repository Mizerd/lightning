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

class QMediaDevices;
class QThread;
class QTimer;
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

    /// GUI thread: the list of audio outputs changed. When an output went
    /// away (measured on Windows 2026-10-07: every cue went to Error on
    /// AUDCLNT_E_DEVICE_INVALIDATED) nothing moved a cue out of Error, so
    /// they stayed unusable for the rest of the session although the device
    /// came back. Debounced, because devices flap; then, where the effects
    /// live, each unusable sound is loaded again (and a loop that was lost
    /// resumes) if an output exists. One reload per burst of changes. The
    /// real constructor connects this to QMediaDevices; tests call it
    /// directly.
    void audioOutputsChanged();
    /// Test-only: the quiet period a burst must end with, and the most a
    /// burst may hold the reload off.
    void setOutputChangeDebounceForTest(int quietMs, int maxWaitMs)
    {
        m_outputChangeQuietMs = quietMs;
        m_outputChangeMaxWaitMs = maxWaitMs;
    }
    /// Test-only: reloads handed to the effects so far. A ForTest player
    /// counts them and posts nothing, so no sound effect is ever created.
    int soundReloadsForTest() const { return m_soundReloads; }

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

    /// The debounced half of audioOutputsChanged().
    void reloadAfterOutputChange();
    /// Only in the real player: Qt's device notifications.
    QMediaDevices *m_devices = nullptr;
    /// Trailing-edge debounce for output-list changes; single shot.
    QTimer *m_outputChangeTimer = nullptr;
    /// When the current burst began; -1 when none is open.
    qint64 m_outputChangeBurstStartMs = -1;
    int m_outputChangeQuietMs = 1000;
    int m_outputChangeMaxWaitMs = 5000;
    int m_soundReloads = 0;
    /// A ForTest player: never creates a sound effect (see the header).
    bool m_forTest = false;
};
