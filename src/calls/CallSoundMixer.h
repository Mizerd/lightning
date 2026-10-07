// Plays the call sounds through long-lived outputs, mixing them here: one
// output and one mixer per device the cues go to, normally two (the default
// device for chimes and the ringer, the call's speaker for call cues)
// (CallSoundPlayer on Linux with Qt 6.10 and later; see CallSoundPlayer.h).
//
// An output is opened when its device is first needed (the default one at
// preload) and replaced only when it fails or after the output list changed.
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
// Everything here runs on the thread it lives on (the GUI thread). The output
// is behind CueOutput so the state machine runs in tests without a device.
#pragma once

#include "calls/CallSoundPcm.h"

#include <QAudioFormat>
#include <QByteArray>
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
    };

    /// Outputs open at once at most; past it the one used longest ago with
    /// nothing playing is closed.
    static constexpr int kMaxOutputs = 3;

    CueEngine(QStringList sounds, Hooks hooks, QObject *parent = nullptr);
    ~CueEngine() override;

    /// Decodes every sound and opens the output on the default device.
    void preload();
    void play(const QString &sound, float gain, const QString &wanted);
    /// Starts `sound` looping (stopping any other loop); the same sound again
    /// only adjusts its gain; "" stops the loop.
    void loop(const QString &sound, float gain, const QString &wanted);
    /// After the output list changed: failed outputs are retried, outputs
    /// whose device is gone are closed, healthy ones are kept, and the loop
    /// moves to (or resumes on) the device it resolves to now. Returns how
    /// many sounds are playable.
    int reloadUnusable();
    void stopAll();

    // Test-only.
    void setIdleSuspendMsForTest(int ms) { m_idleSuspendMs = ms; }
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
    };

    Lane *laneFor(const QString &wanted);
    Lane *openLane(const QByteArray &id);
    void closeLane(const QByteArray &id);
    void onOutputFailed(CueOutput *output);
    void wake(Lane *lane);
    void onIdle(const QByteArray &id);
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
};
