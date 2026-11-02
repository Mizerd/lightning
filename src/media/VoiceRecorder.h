#pragma once

#include <QAudioDecoder>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>

class QMediaCaptureSession;
class QAudioInput;
class QMediaRecorder;
class QMediaDevices;
namespace lightning::media {
class QtAudioGate;
}

class VoiceRecorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool processing READ processing NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)

public:
    explicit VoiceRecorder(QObject *parent = nullptr);
    ~VoiceRecorder() override;

    Q_INVOKABLE bool available();
    virtual bool recording() const { return m_state == State::Recording; }
    virtual bool processing() const { return m_state == State::Processing; }
    virtual bool paused() const { return m_paused; }
    qint64 durationMs() const;

    Q_INVOKABLE virtual bool pause();
    Q_INVOKABLE virtual bool resume();
    Q_INVOKABLE bool ownsPath(const QString &path) const;

    Q_INVOKABLE virtual bool start();
    Q_INVOKABLE virtual void cancel();
    Q_INVOKABLE void stop();

    static QList<int> bucketsFromPeaks(const QList<float> &peaks,
                                       int maxBuckets);

    static constexpr qint64 kMaxDurationMs = 15 * 60 * 1000;
    static constexpr qint64 kBucketMs = 100;
    static constexpr int kMaxWaveformBuckets = 100;

    bool startPending() const { return m_startPending; }
    void setAudioGateForTest(lightning::media::QtAudioGate *gate);
    int recordStartsForTest() const { return m_recordStarts; }
    void setStartNowForTest(std::function<bool()> start)
    {
        m_startNowForTest = std::move(start);
    }
    struct InputInfo {
        QByteArray id;
        bool isDefault = false;
    };
    static bool inputStillPresent(const QByteArray &id, bool wasDefault,
                                  const QList<InputInfo> &inputs);
    void inputDevicesChanged(const QList<InputInfo> &inputs);
    QMediaDevices *watchInputDevicesForTest();
    void setRecordingForTest(const QByteArray &deviceId,
                             const QString &activeFile = QString(),
                             qint64 durationMs = 0);
    void finishForTest();
    bool endedByDisconnect() const { return m_endedByDisconnect; }
    int startNowCallsForTest() const { return m_startNowCalls; }

Q_SIGNALS:
    void stateChanged();
    void durationChanged();
    // The finalized recording: a 0600 file in the 0700 temp dir, its mimetype,
    // duration and waveform (possibly empty).
    void ready(const QString &filePath, const QString &mime,
               qint64 durationMs, const QList<int> &waveform);
    // Could not start or produced nothing usable. The message contains no
    // paths.
    void failed(const QString &message);

private:
    void watchInputDevices();
    enum class State { Idle, Recording, Processing };

    bool ensureCaptureChain();
    bool startNow();
    bool beginRecording();
    void dropPendingStart();
    void beginWaveformExtraction();
    void finishLater(quint64 tag, const QList<int> &waveform);
    void finishWithWaveform(const QList<int> &waveform);
    void discardActiveFile();

    State m_state = State::Idle;
    QPointer<lightning::media::QtAudioGate> m_audioGate;
    bool m_startPending = false;
    int m_recordStarts = 0;
    int m_startNowCalls = 0;
    std::function<bool()> m_startNowForTest;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QAudioInput> m_audioInput;
    std::unique_ptr<QMediaDevices> m_devices;
    QByteArray m_recordingDeviceId;
    bool m_recordingWasDefault = false;
    bool m_endedByDisconnect = false;
    std::unique_ptr<QMediaRecorder> m_recorder;
    std::unique_ptr<QAudioDecoder> m_decoder;
    QTimer m_maxDurationGuard;
    QTimer m_processingGuard;
    QString m_activeFile;
    QString m_activeMime;
    qint64 m_finalDurationMs = 0;
    bool m_cancelRequested = false;
    bool m_paused = false;
    bool m_decodeFormatMismatch = false;
    quint64 m_decodeTag = 0;
    int m_fileSerial = 0;
    QList<float> m_chunkPeaks;
    float m_chunkPeak = 0.0f;
    qint64 m_chunkSamples = 0;
    qint64 m_samplesPerChunk = 0;
};
