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

    Q_PROPERTY(bool built READ built CONSTANT)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString mode READ modeName NOTIFY modeChanged)
    Q_PROPERTY(bool microphoneTestRunning READ microphoneTestRunning
                   NOTIFY modeChanged)
    Q_PROPERTY(bool toneRunning READ toneRunning NOTIFY modeChanged)
    Q_PROPERTY(double level READ level NOTIFY levelChanged)
    Q_PROPERTY(int secondsLeft READ secondsLeft NOTIFY secondsLeftChanged)
    Q_PROPERTY(bool blockedByCall READ blockedByCall NOTIFY blockedByCallChanged)
    /// Why the last attempt failed, as a category the UI words:
    /// "" | "in_call" | "unavailable" | "no_meter" | "microphone" | "output"
    /// | "pipeline".
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    enum class Mode { Idle, Meter, Loopback, Tone };
    Q_ENUM(Mode)

    static constexpr int kMeterLimitMs = 120000;
    static constexpr int kLoopbackLimitMs = 30000;
    static constexpr int kToneBuffers = 12;

    explicit AudioDeviceTester(QObject *parent = nullptr);
    ~AudioDeviceTester() override;

    void setDeviceController(CallDeviceController *devices);
    void setSettings(SettingsManager *settings);
    void setRuntimeAvailable(bool available);
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

    Q_INVOKABLE bool startMicrophoneTest(bool playBack);
    Q_INVOKABLE bool playTestSound();
    Q_INVOKABLE void stop();

    void setTestSourceMode(bool on) { m_testSources = on; }
    void setDeviceChannelsForTest(int channels) { m_testChannels = channels; }
    void setMicrophoneSourceForTest(const QString &element)
    {
        m_testMicSource = element;
    }
    int startsReportedForTest() const { return m_startsReported; }
    void setLimitMsForTest(int ms) { m_limitOverrideMs = ms; }
    QString lastDescriptionForTest() const { return m_lastDescription; }
    bool pipelineRunningForTest() const;
    void noteLevelForTest(double peakDb, qint64 nowMs)
    {
        onLevel(m_generation, peakDb, nowMs);
    }
    static QString describeForTest(
        Mode mode, const QString &micSource, int channels, const QString &sink,
        int gainPercent,
        calls::noise::Mode noiseMode = calls::noise::kDefaultMode,
        bool autoGain = true);

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
    void endSession();
    int limitMs(Mode mode) const;
    void applyGain();
    void applyNoiseMode();

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
    QString m_testMicSource;
    int m_startsReported = 0;
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
