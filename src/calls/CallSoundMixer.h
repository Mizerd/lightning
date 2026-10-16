// Plays the call sounds through long-lived outputs, mixing them here: one
// output and one mixer per device the cues go to, normally two (the default
// device for chimes and the ringer, the call's speaker for call cues)
// (CallSoundPlayer on Linux with Qt 6.10 and later; see CallSoundPlayer.h).
//
// An output is opened when its device is first needed (the default one at
// preload) and replaced only when it fails, after the output list changed, or
// when it stops taking audio (see "A sink that stops taking audio" below).
// A cue opens nothing: it starts a voice in its device's mixer and wakes that
// output. That is the point. Qt's PipeWire sink (6.11) frees a stream on its
// own PipeWire thread when the server tears the stream down after the
// application has dropped it (a device removed while a stream to it is being
// set up), and the eventfd notifiers that stream owns then spin the GUI
// thread's event loop for ever. A stream per cue made that a race every cue
// could lose (reproduced 4 of 4 with cues every 40 ms while the output came
// and went).
//
// A voice stays on the device it started on: a chime on the default device
// never moves the call's loop or its cues off the call's speaker.
//
// A sink that stops taking audio. Measured 2026-10-16 (Qt 6.11.2, private
// PipeWire 1.6.9 graph, both of Qt's PulseAudio and PipeWire backends): after
// the sound server restarts, a pull-mode QAudioSink opened before it stays
// ActiveState with NoError, takes one ring buffer (120 ms) of the next cue and
// never reads again, while a new sink on the same device id plays at once.
// Nothing fails, so the lane was kept, its cues were never drained, and every
// cue after them queued behind them in silence (the PipeWire backend does not
// even report an output change). Qt's PulseAudio sink drops its stream state
// callback once connected, so a stream that died with the server never says.
// So while an output has anything to take, what it took is watched: no audio
// taken for kStallMs while some was waiting means it is dead, and it is
// reopened on the same device with its cues carried over (a one-shot from its
// start, once however often it was queued: none of it was heard). One that
// also takes nothing is given up like a failed output until kStallRetryMs has
// passed (and the gate below allows it) or the outputs change. After an
// output change every output that was already open is offered a short silence
// (kProbeMs, longer than any ring buffer) so a dead one is found before the
// next cue rather than by it; a healthy one is never reopened. kStallMs is
// long enough for an output that is slow to start (a Bluetooth A2DP link
// takes 0.5-2 s), since every verdict, a cue's, the silence's and a reopened
// output's, uses it.
//
// NEVER WHILE QT HAS NO PULSEAUDIO CONNECTION, because a new stream then
// takes the whole process down: Qt 6.11.2's PulseAudio backend drops its
// connection when the server goes away and retries 3 s later, and
// QAudioSink::start() without one asserts in libpulse
// (pa_context_get_state(NULL), measured: SIGABRT). Worse, a retry whose
// connect fails outright (no socket yet) schedules no further retry, so after
// an outage longer than ~3 s without a socket holder Qt never reconnects and
// every new stream would abort for the rest of the process. Qt cannot be
// asked (its own state getter asserts the same way), its device list stays
// as it was, and an output change it reported says the connection WAS live,
// not that it still is. So where the backend may be PulseAudio
// (Hooks::serverReachable is set) a dead output, or the retry of a given-up
// one, waits until the hook has answered yes without a break for
// kServerLiveMs, counted from after the output was found dead and restarted
// by any late check: Qt's PulseAudio thread runs, which it does exactly while
// Qt holds a connection, AND then the server's socket accepts
// (PulseServerProbe.h, asked in that order so a backend that gave up never
// costs a connection to the server). While it says no it is asked less and
// less often (kGatePollMinMs doubling to kGatePollMaxMs). A server that never
// comes back, a Qt that gave up, or a socket the process cannot reach (a
// Flatpak sees the bind-mounted socket inode the server had when the app
// started) never pass: the cues stay silent and nothing aborts. Qt's PipeWire
// backend was measured to open during an outage without harm and keeps the
// timing rules alone: the silence an output change offered found it, or
// kReopenSettleMs after it stopped.
//
// Everything here runs on the thread it lives on (the GUI thread). The output
// is behind CueOutput so the state machine runs in tests without a device.
#pragma once

#include "calls/CallSoundPcm.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>

/// One output stream the cues are mixed into: a QAudioSink in the app, a fake
/// in tests.
class CueOutput : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Starts pulling from `feed` (which it does not own). May emit failed()
    /// before it returns.
    virtual void start(QIODevice *feed) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual bool suspended() const = 0;
    /// Stops at once. Never called from inside one of its own signals.
    virtual void stop() = 0;

Q_SIGNALS:
    /// The device could not be opened or went away. At most once.
    void failed();
};

class CueEngine : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        /// The output a cue for `wanted` (an output id, "" for the default)
        /// plays on, or nothing when there is none.
        std::function<std::optional<QByteArray>(const QString &wanted)>
            resolveOutput;
        /// Whether output `id` accepts `format`.
        std::function<bool(const QByteArray &id, const QAudioFormat &format)>
            supports;
        /// A new output on `id` for `format`, not started; null if `id` is
        /// gone.
        std::function<CueOutput *(const QByteArray &id,
                                  const QAudioFormat &format, QObject *parent)>
            open;
        /// The bundled WAV of `sound`, or nothing.
        std::function<std::optional<QByteArray>(const QString &sound)> wav;
        /// Whether `sound` can be relied on now.
        std::function<void(const QString &sound, bool ready)> setReady;
        /// Whether a new stream may be opened now as far as the sound server
        /// goes (the app: PulseAudio's socket accepts and Qt is connected to
        /// it); set where a new stream must wait for that (Qt's PulseAudio
        /// backend, see the top of this file), unset where it need not.
        std::function<bool()> serverReachable;
    };

    /// Outputs open at once at most; past it the one used longest ago with
    /// nothing playing is closed.
    static constexpr int kMaxOutputs = 3;
    /// How long an output may take no audio while some waits for it before
    /// it counts as dead. A healthy one takes some every ring buffer
    /// (120 ms, AudioSinkOutput) unless the GUI thread is held up, and a
    /// check that comes late (the thread was held up) judges nothing.
    static constexpr int kStallMs = 2500;
    /// The silence each kept output is offered after an output change.
    static constexpr int kProbeMs = 500;
    /// How long an output that took nothing even after it was reopened is
    /// left alone, unless the outputs change first.
    static constexpr int kStallRetryMs = 30000;
    /// How long after a dead output stopped taking audio it may be reopened
    /// without an output change found it. Not a safety bound on its own
    /// (see kServerLiveMs): it keeps a backend that needs no gate from
    /// reopening while the server is still going away.
    static constexpr int kReopenSettleMs = 5000;
    /// How long Hooks::serverReachable must have answered yes, without a
    /// break, before a stream may be opened where the gate applies: longer
    /// than one Qt reconnect interval (3 s), so an attempt Qt still had
    /// pending has run (and finished: it runs on this thread) inside it.
    static constexpr int kServerLiveMs = 4000;
    /// How often a gate that says no is asked again: from the first value,
    /// doubling to the second; back to the first on a yes.
    static constexpr int kGatePollMinMs = 250;
    static constexpr int kGatePollMaxMs = 5000;

    CueEngine(QStringList sounds, Hooks hooks, QObject *parent = nullptr);
    ~CueEngine() override;

    /// Decodes every sound and opens the output on the default device.
    void preload();
    void play(const QString &sound, float gain, const QString &wanted);
    /// Starts `sound` looping (stopping any other loop); the same sound again
    /// only adjusts its gain; "" stops the loop.
    void loop(const QString &sound, float gain, const QString &wanted);
    /// After the output list changed: failed outputs are retried, outputs
    /// whose device is gone are closed, the others are kept and offered a
    /// silence that shows whether they still take audio (see the top of this
    /// file), and the loop moves to (or resumes on) the device it resolves to
    /// now. Returns how many sounds are playable.
    int reloadUnusable();
    void stopAll();

    // Test-only.
    void setIdleSuspendMsForTest(int ms) { m_idleSuspendMs = ms; }
    void setStallMsForTest(int stallMs, int retryMs, int settleMs)
    {
        m_stallMs = stallMs;
        m_stallRetryMs = retryMs;
        m_reopenSettleMs = settleMs;
    }
    void setServerLiveMsForTest(int ms) { m_serverLiveMs = ms; }
    void setGatePollMsForTest(int minMs, int maxMs)
    {
        m_gatePollMinMs = m_gatePollMs = minMs;
        m_gatePollMaxMs = maxMs;
    }
    /// Outputs reopened because they stopped taking audio.
    int outputsReopenedForTest() const { return m_outputsReopened; }
    bool stallTimerActiveForTest() const { return m_stallTimer.isActive(); }
    int outputsOpenedForTest() const { return m_outputsOpened; }
    /// The output on `id`, or on the default device for "".
    CueOutput *outputForTest(const QByteArray &id = {}) const;
    QList<QByteArray> outputIdsForTest() const;
    /// Voices playing on `id` (on every device for "").
    int voicesForTest(const QByteArray &id = {}) const;
    bool failedForTest(const QByteArray &id) const { return m_failed.contains(id); }

private:
    struct Lane {
        QByteArray id;
        QPointer<CueOutput> output;
        callsound::MixerFeed *feed = nullptr; // a child of output
        std::unique_ptr<callsound::CueMixer> mixer;
        std::unique_ptr<QTimer> idleTimer;
        quint64 lastUsed = 0;
        /// The stall check: what the output had taken when last looked at,
        /// and when (-1: not watched, nothing to take).
        quint64 takenAtCheck = 0;
        qint64 checkedAtMs = -1;
        /// Reopened after a stall and not yet seen taking anything.
        bool unproven = false;
        /// Found dead, waiting to be reopened (said once).
        bool dead = false;
        /// Offered the silence of the last output change and has taken
        /// nothing since: a stream that died before that change, which Qt
        /// could only report over a live connection.
        bool probed = false;
    };

    Lane *laneFor(const QString &wanted);
    /// Opens the output on `id`, mixing `carried` (a stalled output's mixer,
    /// whose voices go on here) if the new output has its channel count.
    Lane *openLane(const QByteArray &id,
                   std::unique_ptr<callsound::CueMixer> carried = {});
    void closeLane(const QByteArray &id);
    void onOutputFailed(CueOutput *output);
    void wake(Lane *lane);
    void onIdle(const QByteArray &id);
    /// Starts watching what `lane` takes, if it has anything to take.
    void watch(Lane *lane);
    void onStallTick();
    void reopenStalled(const QByteArray &id);
    /// Whether a stream may be opened now as far as the server goes (see the
    /// top of this file); polls it while something waits.
    bool serverSettled(qint64 now, bool waiting, bool late);
    void giveUpStalled(const QByteArray &id);
    /// The loop goes to (or resumes on) the device it resolves to now.
    void resumeLoop();
    std::optional<callsound::Pcm> decoded(const QString &sound);
    std::optional<callsound::Pcm> pcm(const QString &sound, int channels);
    void startLoop(Lane *lane);
    void stopLoopVoice();
    int refreshReadiness();

    QStringList m_sounds;
    Hooks m_hooks;
    std::map<QByteArray, std::unique_ptr<Lane>> m_lanes;
    QHash<QString, callsound::Pcm> m_decoded; // as bundled
    QHash<int, QHash<QString, callsound::Pcm>> m_converted; // by channels
    QSet<QString> m_undecodable;
    /// Devices whose output failed or accepts no cue format: nothing plays
    /// on them until the output list changes.
    QSet<QByteArray> m_failed;
    int m_outputsOpened = 0;
    quint64 m_useCounter = 0;
    QString m_loopSound;
    float m_loopGain = 1.0f;
    QString m_loopDevice;
    QByteArray m_loopLane;
    int m_loopVoice = 0;
    int m_idleSuspendMs = 5000;
    /// The stall check; runs only while some output has anything to take.
    QTimer m_stallTimer{this};
    QElapsedTimer m_clock;
    qint64 m_lastStallTickMs = -1;
    int m_stallMs = kStallMs;
    int m_stallRetryMs = kStallRetryMs;
    int m_reopenSettleMs = kReopenSettleMs;
    /// Devices in m_failed because their output took nothing even after it
    /// was reopened: retried after m_stallRetryMs.
    QSet<QByteArray> m_stallFailed;
    /// Their retry is due (it waits for the gate); and which reload the
    /// pending retry timer belongs to.
    bool m_retryDue = false;
    quint64 m_retryGeneration = 0;
    /// Since when the server's socket has accepted without a break while
    /// something waited to be opened (-1: not now, or not asked).
    qint64 m_serverLiveSinceMs = -1;
    int m_serverLiveMs = kServerLiveMs;
    /// The gate's back-off while it says no (see kGatePollMinMs).
    int m_gatePollMinMs = kGatePollMinMs;
    int m_gatePollMaxMs = kGatePollMaxMs;
    int m_gatePollMs = kGatePollMinMs;
    qint64 m_nextGatePollMs = -1;
    int m_outputsReopened = 0;
};
