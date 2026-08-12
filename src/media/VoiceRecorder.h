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

// v0.7 voice round: microphone capture for MSC3245 voice messages. Records
// through Qt Multimedia into a session-scoped 0700 temp dir (OGG/Opus when
// the backend encodes it — the Matrix convention — falling back to
// AAC-in-MP4), then decodes its own recording once with QAudioDecoder to
// derive the real amplitude waveform (0..=100 buckets, the scale the
// receive path already renders). Nothing here is fabricated: no decodable
// audio means an empty waveform and the receiver's plain progress track.
//
// The object is constructed lazily by AppController on the first mic
// press, so the audio backend is never spun up for a session that never
// records. File ownership transfers with ready(): the SDK reads the bytes
// into the send queue at queueing time (it never re-reads the path), so
// the consumer deletes the file as soon as its send op resolves; cancel
// deletes immediately, and the temp dir sweep at destruction is only the
// last-resort backstop.
class VoiceRecorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool processing READ processing NOTIFY stateChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)

public:
    explicit VoiceRecorder(QObject *parent = nullptr);
    ~VoiceRecorder() override;

    // True when an input device exists and a supported encoder resolved.
    // A plain invokable, deliberately not a NOTIFYing property: nothing
    // drives device-hotplug change signals, and a binding that can never
    // update is a trap (review L3). A failed start() reports through
    // failed() regardless.
    Q_INVOKABLE bool available();
    bool recording() const { return m_state == State::Recording; }
    bool processing() const { return m_state == State::Processing; }
    qint64 durationMs() const;

    // Begin a new recording (any previous unsent recording's file is
    // dropped). Returns false — with failed() emitted — when no device or
    // encoder is available.
    Q_INVOKABLE bool start();
    // Discard the active recording and its file.
    Q_INVOKABLE void cancel();
    // Finalize: stops capture, derives the waveform from the recorded
    // file, then emits ready(). No-op unless recording.
    Q_INVOKABLE void stop();

    // Downsample a chunk-peak series (0..=1 floats) into at most
    // maxBuckets 0..=100 integers, max-preserving per bucket. Pure and
    // static for the unit test.
    static QList<int> bucketsFromPeaks(const QList<float> &peaks,
                                       int maxBuckets);

    // Hard cap: a forgotten live mic must not record indefinitely.
    static constexpr qint64 kMaxDurationMs = 15 * 60 * 1000;
    // One waveform bucket per this many ms, bounded to kMaxWaveformBuckets
    // (MSC3245 recommends 30-120 values; the bridge caps at 1024).
    static constexpr qint64 kBucketMs = 100;
    static constexpr int kMaxWaveformBuckets = 100;

Q_SIGNALS:
    void stateChanged();
    void durationChanged();
    // The finalized recording: a local file (0600, inside the recorder's
    // 0700 temp dir), its real mimetype, real duration, and the derived
    // waveform (possibly empty). The caller sends it via
    // MessageComposer.sendVoiceMessage.
    void ready(const QString &filePath, const QString &mime,
               qint64 durationMs, const QList<int> &waveform);
    // A recording could not start or produced nothing usable. The message
    // is presentation-safe (no paths).
    void failed(const QString &message);

private:
    enum class State { Idle, Recording, Processing };

    bool ensureCaptureChain();
    void beginWaveformExtraction();
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
    bool m_decodeFormatMismatch = false;
    int m_fileSerial = 0;
    // Per-chunk absolute peaks accumulated during waveform decode.
    QList<float> m_chunkPeaks;
    float m_chunkPeak = 0.0f;
    qint64 m_chunkSamples = 0;
    qint64 m_samplesPerChunk = 0;
};
