#include "media/VoiceRecorder.h"

#include "media/QtAudioGate.h"
#include "storage/PortableMode.h"

#include <QAudioBuffer>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioInput>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QUrl>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcVoice, "lightning.media.voice")

namespace {
struct EncoderProfile {
    QMediaFormat::FileFormat container;
    QMediaFormat::AudioCodec codec;
    const char *mime;
    const char *suffix;
};
constexpr EncoderProfile kProfiles[] = {
    { QMediaFormat::Ogg, QMediaFormat::AudioCodec::Opus, "audio/ogg", "ogg" },
    { QMediaFormat::Mpeg4Audio, QMediaFormat::AudioCodec::AAC, "audio/mp4",
      "m4a" },
};

const EncoderProfile *resolveProfile()
{
    for (const auto &profile : kProfiles) {
        QMediaFormat format(profile.container);
        format.setAudioCodec(profile.codec);
        if (format.isSupported(QMediaFormat::Encode))
            return &profile;
    }
    return nullptr;
}
}

VoiceRecorder::VoiceRecorder(QObject *parent)
    : QObject(parent)
{
    m_maxDurationGuard.setSingleShot(true);
    m_maxDurationGuard.setInterval(static_cast<int>(kMaxDurationMs));
    connect(&m_maxDurationGuard, &QTimer::timeout, this, [this] {
        if (recording()) {
            qCInfo(lcVoice, "recording hit the hard duration cap; discarding");
            cancel();
            Q_EMIT failed(tr("The recording reached the maximum length and "
                             "was discarded."));
        }
    });
    m_processingGuard.setSingleShot(true);
    m_processingGuard.setInterval(15000);
    connect(&m_processingGuard, &QTimer::timeout, this, [this] {
        if (m_state != State::Processing)
            return;
        qCWarning(lcVoice, "finalization timed out; discarding");
        m_cancelRequested = true;
        ++m_decodeTag;
        m_decoder.reset();
        discardActiveFile();
        m_state = State::Idle;
        Q_EMIT stateChanged();
        Q_EMIT failed(tr("The recording could not be finalized."));
    });
}

VoiceRecorder::~VoiceRecorder()
{
    if (m_dir && m_dir->isValid())
        lightning::portable::releaseScratchDir(m_dir->path());
}

bool VoiceRecorder::available()
{
    if (QMediaDevices::defaultAudioInput().isNull())
        return false;
    return resolveProfile() != nullptr;
}

bool VoiceRecorder::ensureCaptureChain()
{
    if (!m_dir) {
        m_dir = std::make_unique<QTemporaryDir>(
            lightning::portable::mediaScratchRoot()
            + QStringLiteral("/lightning-voice-XXXXXX"));
        if (m_dir->isValid())
            lightning::portable::holdScratchDirLive(m_dir->path());
        if (!m_dir->isValid()) {
            m_dir.reset();
            return false;
        }
    }
    if (!m_session) {
        m_session = std::make_unique<QMediaCaptureSession>();
        m_audioInput = std::make_unique<QAudioInput>();
        m_recorder = std::make_unique<QMediaRecorder>();
        m_session->setAudioInput(m_audioInput.get());
        m_session->setRecorder(m_recorder.get());
        connect(m_recorder.get(), &QMediaRecorder::durationChanged,
                this, &VoiceRecorder::durationChanged);
        // A microphone unplugged mid-recording produces silence for the rest
        // of the clip and nothing reports it (the recorder raises no error).
        // Watch the device list for the one this recording started on.
        watchInputDevices();
        connect(m_recorder.get(), &QMediaRecorder::recorderStateChanged,
                this, [this](QMediaRecorder::RecorderState state) {
                    if (state != QMediaRecorder::StoppedState)
                        return;
                    if (m_state != State::Processing)
                        return;
                    if (m_cancelRequested) {
                        discardActiveFile();
                        m_state = State::Idle;
                        Q_EMIT stateChanged();
                        return;
                    }
                    const QString actual =
                        m_recorder->actualLocation().toLocalFile();
                    if (!actual.isEmpty())
                        m_activeFile = actual;
                    QFile::setPermissions(m_activeFile,
                                          QFileDevice::ReadOwner
                                              | QFileDevice::WriteOwner);
                    m_finalDurationMs = m_recorder->duration();
                    if (m_finalDurationMs <= 0
                        || !QFile::exists(m_activeFile)) {
                        discardActiveFile();
                        m_state = State::Idle;
                        Q_EMIT stateChanged();
                        Q_EMIT failed(tr("Nothing was recorded."));
                        return;
                    }
                    beginWaveformExtraction();
                });
        connect(m_recorder.get(), &QMediaRecorder::errorOccurred, this,
                [this](QMediaRecorder::Error, const QString &) {
                    qCWarning(lcVoice, "recorder error during capture");
                    if (m_state == State::Idle)
                        return;
                    m_cancelRequested = true;
                    m_recorder->stop();
                    discardActiveFile();
                    m_state = State::Idle;
                    Q_EMIT stateChanged();
                    Q_EMIT failed(tr("Recording failed."));
                });
    }
    return true;
}

void VoiceRecorder::watchInputDevices()
{
    if (m_devices)
        return;
    m_devices = std::make_unique<QMediaDevices>();
    connect(m_devices.get(), &QMediaDevices::audioInputsChanged, this, [this] {
        QList<InputInfo> infos;
        for (const QAudioDevice &device : QMediaDevices::audioInputs())
            infos.append({device.id(), device.isDefault()});
        inputDevicesChanged(infos);
    });
}

QMediaDevices *VoiceRecorder::watchInputDevicesForTest()
{
    watchInputDevices();
    return m_devices.get();
}

bool VoiceRecorder::inputStillPresent(const QByteArray &id, bool wasDefault,
                                      const QList<InputInfo> &inputs)
{
    for (const InputInfo &input : inputs) {
        if (input.id == id)
            return true;
        if (wasDefault && input.isDefault)
            return true;
    }
    return false;
}

void VoiceRecorder::inputDevicesChanged(const QList<InputInfo> &inputs)
{
    if (m_state != State::Recording || m_recordingDeviceId.isEmpty())
        return;
    if (inputStillPresent(m_recordingDeviceId, m_recordingWasDefault, inputs))
        return;
    qCWarning(lcVoice, "the capture device disappeared during recording");
    m_endedByDisconnect = true;
    stop();
}

void VoiceRecorder::setRecordingForTest(const QByteArray &deviceId,
                                        const QString &activeFile,
                                        qint64 durationMs)
{
    m_state = State::Recording;
    m_recordingDeviceId = deviceId;
    m_activeFile = activeFile;
    m_activeMime = QStringLiteral("audio/ogg");
    m_finalDurationMs = durationMs;
}

void VoiceRecorder::finishForTest()
{
    finishWithWaveform({});
}

qint64 VoiceRecorder::durationMs() const
{
    if (m_state == State::Recording && m_recorder)
        return m_recorder->duration();
    return m_finalDurationMs;
}

bool VoiceRecorder::start()
{
    if (m_state != State::Idle || m_startPending)
        return false;
    // record() opens the microphone stream, which aborts the process while
    // Qt's PulseAudio backend has no connection (QtAudioGate.h). Then the
    // whole start waits for it, bounded; true is returned because the start
    // is on its way, and failed() reports it if it cannot happen.
    using lightning::media::QtAudioGate;
    QtAudioGate *gate = m_audioGate ? m_audioGate.data() : QtAudioGate::current();
    if (gate && !gate->checkNow()) {
        m_startPending = true;
        gate->whenReady(
            this,
            [this] {
                if (!m_startPending)
                    return;
                m_startPending = false;
                startNow();
            },
            [this] {
                if (!m_startPending)
                    return;
                m_startPending = false;
                Q_EMIT failed(tr("The sound server is not answering. Try "
                                 "recording again in a moment."));
            });
        return true;
    }
    return startNow();
}

bool VoiceRecorder::startNow()
{
    ++m_startNowCalls;
    if (m_startNowForTest)
        return m_startNowForTest();
    const EncoderProfile *profile = resolveProfile();
    if (QMediaDevices::defaultAudioInput().isNull() || !profile) {
        Q_EMIT failed(tr("No microphone or audio encoder is available."));
        return false;
    }
    if (!ensureCaptureChain()) {
        Q_EMIT failed(tr("Recording storage could not be prepared."));
        return false;
    }
    discardActiveFile();

    QMediaFormat format(profile->container);
    format.setAudioCodec(profile->codec);
    m_recorder->setMediaFormat(format);
    m_recorder->setAudioChannelCount(1);
    m_recorder->setAudioSampleRate(48000);
    m_recorder->setAudioBitRate(32000);
    m_recorder->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
    m_activeMime = QString::fromLatin1(profile->mime);
    m_activeFile = m_dir->filePath(
        QStringLiteral("voice-%1.%2")
            .arg(++m_fileSerial)
            .arg(QString::fromLatin1(profile->suffix)));
    m_recorder->setOutputLocation(QUrl::fromLocalFile(m_activeFile));
    m_cancelRequested = false;
    m_finalDurationMs = 0;
    return beginRecording();
}

bool VoiceRecorder::beginRecording()
{
    ++m_recordStarts;
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError) {
        Q_EMIT failed(tr("Recording could not start."));
        return false;
    }
    m_state = State::Recording;
    m_paused = false;
    m_endedByDisconnect = false;
    m_recordingDeviceId = m_audioInput ? m_audioInput->device().id()
                                       : QByteArray();
    m_recordingWasDefault = m_audioInput && m_audioInput->device().isDefault();
    m_maxDurationGuard.setInterval(static_cast<int>(kMaxDurationMs));
    m_maxDurationGuard.start();
    Q_EMIT stateChanged();
    Q_EMIT durationChanged();
    qCDebug(lcVoice, "recording started");
    return true;
}

bool VoiceRecorder::pause()
{
    if (m_state != State::Recording || m_paused || !m_recorder)
        return false;
    m_recorder->pause();
    if (m_recorder->error() != QMediaRecorder::NoError)
        return false;
    m_paused = true;
    m_maxDurationGuard.stop();
    Q_EMIT stateChanged();
    qCDebug(lcVoice, "recording paused");
    return true;
}

bool VoiceRecorder::resume()
{
    if (m_state != State::Recording || !m_paused || !m_recorder)
        return false;
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError)
        return false;
    m_paused = false;
    const qint64 remaining =
        std::max<qint64>(1000, kMaxDurationMs - m_recorder->duration());
    m_maxDurationGuard.start(static_cast<int>(remaining));
    Q_EMIT stateChanged();
    qCDebug(lcVoice, "recording resumed");
    return true;
}

bool VoiceRecorder::ownsPath(const QString &path) const
{
    if (path.isEmpty() || !m_dir || !m_dir->isValid())
        return false;
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    const QString dir = QFileInfo(m_dir->path()).canonicalFilePath();
    if (dir.isEmpty())
        return false;
    return info.canonicalPath() == dir;
}

void VoiceRecorder::dropPendingStart()
{
    m_startPending = false;
    using lightning::media::QtAudioGate;
    if (QtAudioGate *gate =
            m_audioGate ? m_audioGate.data() : QtAudioGate::current())
        gate->cancel(this);
}

void VoiceRecorder::setAudioGateForTest(lightning::media::QtAudioGate *gate)
{
    m_audioGate = gate;
}

void VoiceRecorder::cancel()
{
    if (m_startPending) {
        dropPendingStart();
        qCDebug(lcVoice, "pending recording start cancelled");
        return;
    }
    if (m_state == State::Idle)
        return;
    m_maxDurationGuard.stop();
    m_paused = false;
    m_processingGuard.stop();
    m_cancelRequested = true;
    if (m_decoder) {
        ++m_decodeTag;
        m_decoder->stop();
        m_decoder.reset();
    }
    if (m_recorder
        && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
        m_state = State::Processing;
        m_processingGuard.start();
        m_recorder->stop();
        return;
    }
    discardActiveFile();
    m_state = State::Idle;
    Q_EMIT stateChanged();
    qCDebug(lcVoice, "recording cancelled");
}

void VoiceRecorder::stop()
{
    if (m_startPending) {
        dropPendingStart();
        Q_EMIT failed(tr("The sound server is not answering. Try recording "
                         "again in a moment."));
        return;
    }
    if (m_state != State::Recording)
        return;
    m_maxDurationGuard.stop();
    m_paused = false;
    m_state = State::Processing;
    m_processingGuard.start();
    Q_EMIT stateChanged();
    if (m_recorder)
        m_recorder->stop();
}

void VoiceRecorder::beginWaveformExtraction()
{
    const quint64 tag = ++m_decodeTag;
    m_decoder = std::make_unique<QAudioDecoder>();
    QAudioFormat format;
    format.setChannelCount(1);
    format.setSampleRate(16000);
    format.setSampleFormat(QAudioFormat::Float);
    m_decoder->setAudioFormat(format);
    m_decoder->setSource(QUrl::fromLocalFile(m_activeFile));
    m_chunkPeaks.clear();
    m_chunkPeak = 0.0f;
    m_chunkSamples = 0;
    m_samplesPerChunk = 16000 * kBucketMs / 1000;

    m_decodeFormatMismatch = false;
    connect(m_decoder.get(), &QAudioDecoder::bufferReady, this, [this] {
        const QAudioBuffer buffer = m_decoder->read();
        // setAudioFormat() is only a request, and constData<T>() does no type
        // check: reading non-float data as float would read out of bounds.
        if (!buffer.isValid()
            || buffer.format().sampleFormat() != QAudioFormat::Float
            || buffer.format().channelCount() != 1) {
            m_decodeFormatMismatch = true;
            return;
        }
        if (m_decodeFormatMismatch)
            return;
        const auto *samples = buffer.constData<float>();
        if (!samples)
            return;
        for (qsizetype i = 0; i < buffer.sampleCount(); ++i) {
            m_chunkPeak = std::max(m_chunkPeak, std::abs(samples[i]));
            if (++m_chunkSamples >= m_samplesPerChunk) {
                m_chunkPeaks.append(m_chunkPeak);
                m_chunkPeak = 0.0f;
                m_chunkSamples = 0;
            }
        }
    });
    connect(m_decoder.get(), &QAudioDecoder::finished, this, [this, tag] {
        if (m_decodeFormatMismatch) {
            finishLater(tag, {});
            return;
        }
        if (m_chunkSamples > 0)
            m_chunkPeaks.append(m_chunkPeak);
        finishLater(tag,
                    bucketsFromPeaks(m_chunkPeaks, kMaxWaveformBuckets));
    });
    connect(m_decoder.get(),
            qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
            [this, tag] {
        qCInfo(lcVoice, "waveform decode failed; sending without waveform");
        finishLater(tag, {});
    });
    m_decoder->start();
}

void VoiceRecorder::finishLater(quint64 tag, const QList<int> &waveform)
{
    QMetaObject::invokeMethod(
        this,
        [this, tag, waveform] {
            if (m_decodeTag != tag)
                return;
            ++m_decodeTag;
            finishWithWaveform(waveform);
        },
        Qt::QueuedConnection);
}

void VoiceRecorder::finishWithWaveform(const QList<int> &waveform)
{
    m_decoder.reset();
    m_processingGuard.stop();
    m_paused = false;
    if (m_cancelRequested) {
        discardActiveFile();
        m_state = State::Idle;
        Q_EMIT stateChanged();
        return;
    }
    if (m_endedByDisconnect) {
        discardActiveFile();
        m_state = State::Idle;
        Q_EMIT stateChanged();
        Q_EMIT failed(tr("The microphone was disconnected. The recording was "
                         "discarded."));
        return;
    }
    const QString file = m_activeFile;
    const QString mime = m_activeMime;
    const qint64 duration = m_finalDurationMs;
    m_activeFile.clear();
    m_state = State::Idle;
    Q_EMIT stateChanged();
    qCDebug(lcVoice, "recording ready durationMs=%lld waveformBuckets=%lld",
            static_cast<long long>(duration),
            static_cast<long long>(waveform.size()));
    Q_EMIT ready(file, mime, duration, waveform);
}

void VoiceRecorder::discardActiveFile()
{
    if (!m_activeFile.isEmpty())
        QFile::remove(m_activeFile);
    m_activeFile.clear();
    m_finalDurationMs = 0;
}

QList<int> VoiceRecorder::bucketsFromPeaks(const QList<float> &peaks,
                                           int maxBuckets)
{
    if (peaks.isEmpty() || maxBuckets <= 0)
        return {};
    const int buckets =
        std::min<int>(maxBuckets, static_cast<int>(peaks.size()));
    QList<int> out;
    out.reserve(buckets);
    float loudest = 0.0f;
    for (float peak : peaks)
        loudest = std::max(loudest, peak);
    for (int b = 0; b < buckets; ++b) {
        const qsizetype begin = static_cast<qsizetype>(b) * peaks.size()
            / buckets;
        const qsizetype end = static_cast<qsizetype>(b + 1) * peaks.size()
            / buckets;
        float peak = 0.0f;
        for (qsizetype i = begin; i < std::max(end, begin + 1); ++i)
            peak = std::max(peak, peaks.at(std::min(i, peaks.size() - 1)));
        const float normalized = loudest > 0.0f ? peak / loudest : 0.0f;
        out.append(std::clamp(
            static_cast<int>(std::lround(normalized * 100.0f)), 0, 100));
    }
    return out;
}
