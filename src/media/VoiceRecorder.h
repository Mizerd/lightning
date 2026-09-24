#pragma once

#include <QAudioDecoder>
#include <QList>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class QMediaCaptureSession;
class QAudioInput;
class QMediaRecorder;

// Microphone capture for MSC3245 voice messages. Records into a
// session-scoped 0700 temp dir (OGG/Opus when available, the Matrix
// convention, else AAC-in-MP4), then decodes the recording once to derive the
// real amplitude waveform (0..=100 buckets). Nothing is fabricated: no
// decodable audio means an empty waveform.
//
// Created lazily on the first mic press. File ownership transfers with
// ready(): the SDK reads the bytes at queueing time and the consumer deletes
// the file when the send resolves. Cancel deletes immediately; the temp dir
// cleanup at destruction is only a backstop.
class VoiceRecorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool processing READ processing NOTIFY stateChanged)
    // A paused recording still holds the capture chain and the file, so
    // `recording` stays true and this is a separate flag.
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)

public:
    explicit VoiceRecorder(QObject *parent = nullptr);
    ~VoiceRecorder() override;

    // An input device exists and an encoder resolved. Not a property: nothing
    // signals device hotplug. A failed start() reports through failed().
    Q_INVOKABLE bool available();
    // Virtual only so a test double can stand in for the capture chain
    // (AppController::setVoiceRecorderForTest) to test the ownership rules.
    virtual bool recording() const { return m_state == State::Recording; }
    virtual bool processing() const { return m_state == State::Processing; }
    virtual bool paused() const { return m_paused; }
    qint64 durationMs() const;

    // No-ops returning false unless in the matching state. Elapsed time freezes
    // while paused.
    Q_INVOKABLE virtual bool pause();
    Q_INVOKABLE virtual bool resume();
    // True when `path` lives in this recorder's temp dir; gates UI-initiated
    // deletion of a previewed file.
    Q_INVOKABLE bool ownsPath(const QString &path) const;

    // Returns false without failed() when not Idle (never supersedes a running
    // or finalizing recording), and false with failed() when no device or
    // encoder is available.
    Q_INVOKABLE virtual bool start();
    // Discard the active recording and its file.
    Q_INVOKABLE virtual void cancel();
    // Finalize: stops capture, derives the waveform from the recorded
    // file, then emits ready(). No-op unless recording.
    Q_INVOKABLE void stop();

    // Downsamples chunk peaks (0..=1) into at most maxBuckets values 0..=100,
    // max-preserving.
    static QList<int> bucketsFromPeaks(const QList<float> &peaks,
                                       int maxBuckets);

    // Hard cap: a forgotten live mic must not record indefinitely.
    static constexpr qint64 kMaxDurationMs = 15 * 60 * 1000;
    // One bucket per this many ms, bounded to kMaxWaveformBuckets (MSC3245
    // suggests 30-120; the bridge caps at 1024).
    static constexpr qint64 kBucketMs = 100;
    static constexpr int kMaxWaveformBuckets = 100;

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
    enum class State { Idle, Recording, Processing };

    bool ensureCaptureChain();
    void beginWaveformExtraction();
    /// Finishes the waveform decode from the event loop: finishWithWaveform()
    /// destroys the decoder, and deleting a QObject during its own emission is
    /// a use-after-free. `tag` drops hops from abandoned decodes and makes two
    /// signals from one decode deliver a single ready().
    void finishLater(quint64 tag, const QList<int> &waveform);
    void finishWithWaveform(const QList<int> &waveform);
    void discardActiveFile();

    State m_state = State::Idle;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QAudioInput> m_audioInput;
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
    /// Identifies the decode in flight; bumped on start, completion and
    /// abandonment.
    quint64 m_decodeTag = 0;
    int m_fileSerial = 0;
    // Per-chunk absolute peaks accumulated during waveform decode.
    QList<float> m_chunkPeaks;
    float m_chunkPeak = 0.0f;
    qint64 m_chunkSamples = 0;
    qint64 m_samplesPerChunk = 0;
};
