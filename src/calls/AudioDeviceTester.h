// The Settings microphone and speaker test (Discord's "Let's Check", Teams'
// level bar under the microphone picker).
//
// Three things, one at a time:
//
//   * a microphone level check: the chosen microphone, opened exactly as a
//     call opens it (SfuMediaEngine::resolveMicrophoneCapture() and
//     microphoneFrontDescription(), the same processing and gain), into a
//     `level` element and nowhere else;
//   * the same with the user's voice played back to the chosen output
//     device, so they hear what others would hear;
//   * a short test tone on the chosen output device.
//
// Never during a call (refused, and a running test stops when one starts):
// the call owns the microphone, and two captures of one device are a
// different test. The in-call meter reads the call's own `level` instead
// (SfuCallController::microphoneLevel).
//
// A test always ends by itself: kMeterLimitMs / kLoopbackLimitMs, or the
// tone's own length. It also ends when Settings closes the section, when a
// call starts and when the application quits.
//
// Threading: the device lookup may block for up to the enumeration budget
// and a sound server that hangs can block a state change for 30 s, so a
// pipeline is built, started and stopped on a worker thread; GStreamer's
// messages reach the GUI thread through a relay that is cut before this
// object is destroyed, and a session generation discards anything late.
//
// Builds without the call media engine (HAVE_LIGHTNING_WEBRTC unset) compile
// this as a stub whose `built` is false, and the UI says why.
//
// Privacy: no device id, device name or audio is ever logged; levels are
// numbers.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "calls/AudioLevelMeter.h"
#include "calls/noise/NoiseSuppressor.h"

class CallDeviceController;
class SettingsManager;

class AudioDeviceTester : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AudioDeviceTester is exposed via app.audioTester")

    /// Compiled with the call media engine. False means this build cannot
    /// test devices at all, whatever the runtime has.
    Q_PROPERTY(bool built READ built CONSTANT)
    /// Built AND the media runtime was found at startup.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    /// "idle" | "meter" | "loopback" | "tone"
    Q_PROPERTY(QString mode READ modeName NOTIFY modeChanged)
    /// The microphone is open (meter or loopback).
    Q_PROPERTY(bool microphoneTestRunning READ microphoneTestRunning
                   NOTIFY modeChanged)
    Q_PROPERTY(bool toneRunning READ toneRunning NOTIFY modeChanged)
    /// 0..1, the scale of AudioLevelMeter.h; 0 while idle.
    Q_PROPERTY(double level READ level NOTIFY levelChanged)
    /// Seconds until the running microphone test stops by itself; 0 idle.
    Q_PROPERTY(int secondsLeft READ secondsLeft NOTIFY secondsLeftChanged)
    /// A call is starting, ringing or live: every test is refused.
    Q_PROPERTY(bool blockedByCall READ blockedByCall NOTIFY blockedByCallChanged)
    /// Why the last attempt failed, as a category the UI words:
    /// "" | "in_call" | "unavailable" | "no_meter" | "microphone" | "output"
    /// | "pipeline".
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    enum class Mode { Idle, Meter, Loopback, Tone };
    Q_ENUM(Mode)

    /// How long a microphone test may run before it stops by itself.
    static constexpr int kMeterLimitMs = 120000;
    static constexpr int kLoopbackLimitMs = 30000;
    /// The test tone: 100 ms buffers of a quiet sine.
    static constexpr int kToneBuffers = 12;

    explicit AudioDeviceTester(QObject *parent = nullptr);
    ~AudioDeviceTester() override;

    /// Where the chosen devices and the microphone gain come from.
    void setDeviceController(CallDeviceController *devices);
    void setSettings(SettingsManager *settings);
    /// Set once the media runtime has been probed (AppController); never in
    /// tests that must not initialise GStreamer.
    void setRuntimeAvailable(bool available);
    /// A call is starting, ringing or live. Stops any running test.
    void setCallActive(bool active);

    bool built() const;
    bool available() const { return built() && m_runtimeAvailable; }
    Mode mode() const { return m_mode; }
    QString modeName() const;
    bool microphoneTestRunning() const
    {
        return m_mode == Mode::Meter || m_mode == Mode::Loopback;
    }
    bool toneRunning() const { return m_mode == Mode::Tone; }
    double level() const { return m_level; }
    int secondsLeft() const { return m_secondsLeft; }
    bool blockedByCall() const { return m_callActive; }
    QString error() const { return m_error; }

    /// Opens the chosen microphone; with `playBack`, plays it to the chosen
    /// output too. Replaces whatever test is running. False when refused (see
    /// `error`); a device that fails to open is reported later through
    /// `error` and a return to idle.
    Q_INVOKABLE bool startMicrophoneTest(bool playBack);
    /// A short tone on the chosen output device.
    Q_INVOKABLE bool playTestSound();
    /// Ends whatever is running. Idempotent.
    Q_INVOKABLE void stop();

    // ── Tests ──
    /// Synthetic microphone (`audiotestsrc`) and silent outputs (`fakesink`),
    /// so CI parses and runs the real descriptions without a sound card.
    void setTestSourceMode(bool on) { m_testSources = on; }
    /// Pretend the microphone has this many channels (multi-input path).
    void setDeviceChannelsForTest(int channels) { m_testChannels = channels; }
    void setLimitMsForTest(int ms) { m_limitOverrideMs = ms; }
    /// The description the last start actually handed GStreamer.
    QString lastDescriptionForTest() const { return m_lastDescription; }
    /// A pipeline is running for the current session.
    bool pipelineRunningForTest() const;
    /// A level reading for the current session, as the bus delivers it.
    void noteLevelForTest(double peakDb, qint64 nowMs)
    {
        onLevel(m_generation, peakDb, nowMs);
    }
    /// What composeDescription() builds, for a description-only check.
    /// `noiseMode` is the microphone noise suppression the test runs; the
    /// real test takes it from the setting, as the call does.
    static QString describeForTest(
        Mode mode, const QString &micSource, int channels, const QString &sink,
        int gainPercent,
        calls::noise::Mode noiseMode = calls::noise::kDefaultMode);

Q_SIGNALS:
    void availableChanged();
    void modeChanged();
    void levelChanged();
    void secondsLeftChanged();
    void blockedByCallChanged();
    void errorChanged();

private:
    struct Session;
    struct Relay;

    bool start(Mode mode);
    void setMode(Mode mode);
    void setError(const QString &category);
    void setLevel(double level);
    /// Ends the current session without touching mode or error.
    void endSession();
    int limitMs(Mode mode) const;
    void applyGain();
    /// The noise suppression setting changed: applied to the running test
    /// live, through the same switch the call uses.
    void applyNoiseMode();

    // From the worker and the bus, on the GUI thread, for `generation`.
    void onDescription(quint64 generation, const QString &description);
    void onStarted(quint64 generation);
    void onFailed(quint64 generation, const QString &category);
    void onFinished(quint64 generation);
    void onLevel(quint64 generation, double peakDb, qint64 nowMs);

    QPointer<CallDeviceController> m_devices;
    QPointer<SettingsManager> m_settings;
    bool m_runtimeAvailable = false;
    bool m_callActive = false;
    bool m_testSources = false;
    int m_testChannels = 0;
    int m_limitOverrideMs = 0;
    Mode m_mode = Mode::Idle;
    double m_level = 0.0;
    int m_secondsLeft = 0;
    QString m_error;
    QString m_lastDescription;
    quint64 m_generation = 0;
    lightning::calls::MeterThrottle m_throttle;
    std::shared_ptr<Session> m_session;
    std::shared_ptr<Relay> m_relay;
    QTimer m_limitTimer;
    QTimer m_countdown;
};
