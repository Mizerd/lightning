#include "media/VoiceRecorder.h"

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

// Counts, states, and coarse errors only — never device names or paths.
Q_LOGGING_CATEGORY(lcVoice, "lightning.media.voice")

namespace {
// Preferred (Matrix-conventional), then fallback encoder profiles.
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
} // namespace

VoiceRecorder::VoiceRecorder(QObject *parent)
    : QObject(parent)
{
    m_maxDurationGuard.setSingleShot(true);
    m_maxDurationGuard.setInterval(static_cast<int>(kMaxDurationMs));
    connect(&m_maxDurationGuard, &QTimer::timeout, this, [this] {
        // Review H1: the cap must NEVER publish. stop() feeds the send
        // path, and a forgotten open mic auto-uploading fifteen minutes of
        // ambient audio would be an unintended publication the user never
        // authorized. Discard, and say why.
        if (recording()) {
            qCInfo(lcVoice, "recording hit the hard duration cap; discarding");
            cancel();
            Q_EMIT failed(tr("The recording reached the maximum length and "
                             "was discarded."));
        }
    });
    // Review L2: finalization (encoder flush + waveform decode) is bounded
    // too — a backend that never reports StoppedState (or a wedged decoder)
    // must not trap the composer in the recording pill forever.
    m_processingGuard.setSingleShot(true);
    m_processingGuard.setInterval(15000);
    connect(&m_processingGuard, &QTimer::timeout, this, [this] {
        if (m_state != State::Processing)
            return;
        qCWarning(lcVoice, "finalization timed out; discarding");
        m_cancelRequested = true;
        m_decoder.reset();
        discardActiveFile();
        m_state = State::Idle;
        Q_EMIT stateChanged();
        Q_EMIT failed(tr("The recording could not be finalized."));
    });
}

VoiceRecorder::~VoiceRecorder() = default; // temp dir wipes the recordings

bool VoiceRecorder::available()
{
    if (QMediaDevices::defaultAudioInput().isNull())
        return false;
    return resolveProfile() != nullptr;
}

bool VoiceRecorder::ensureCaptureChain()
{
    if (!m_dir) {
        // Same scratch root as every other decrypted-media path — inside the
        // portable folder when portable, the OS temp directory otherwise.
        m_dir = std::make_unique<QTemporaryDir>(
            lightning::portable::mediaScratchRoot()
            + QStringLiteral("/lightning-voice-XXXXXX"));
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
        connect(m_recorder.get(), &QMediaRecorder::recorderStateChanged,
                this, [this](QMediaRecorder::RecorderState state) {
                    if (state != QMediaRecorder::StoppedState)
                        return;
                    if (m_state != State::Processing)
                        return; // stop we did not initiate; cancel handled it
                    if (m_cancelRequested) {
                        discardActiveFile();
                        m_state = State::Idle;
                        Q_EMIT stateChanged();
                        return;
                    }
                    // The recorder resolves the REAL output location itself.
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
                    // Error strings can embed device/file detail; log the
                    // category only.
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

qint64 VoiceRecorder::durationMs() const
{
    if (m_state == State::Recording && m_recorder)
        return m_recorder->duration();
    return m_finalDurationMs;
}

bool VoiceRecorder::start()
{
    if (m_state != State::Idle)
        return false;
    const EncoderProfile *profile = resolveProfile();
    if (QMediaDevices::defaultAudioInput().isNull() || !profile) {
        Q_EMIT failed(tr("No microphone or audio encoder is available."));
        return false;
    }
    if (!ensureCaptureChain()) {
        Q_EMIT failed(tr("Recording storage could not be prepared."));
        return false;
    }
    // A new recording supersedes an unsent previous one; the dir is wiped
    // wholesale at destruction, so only the immediately previous file is
    // dropped eagerly here.
    discardActiveFile();

    QMediaFormat format(profile->container);
    format.setAudioCodec(profile->codec);
    m_recorder->setMediaFormat(format);
    // Voice, not music: mono at a speech bitrate. Without these the
    // recorder inherits the capture device's channel layout (a 4-channel
    // mic array produced QUAD Opus at a defaulted 192 kbps — 126 KB for
    // six seconds of speech). 32 kbps mono Opus at 48 kHz is the
    // conventional voice-message profile and ~6x smaller.
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
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError) {
        Q_EMIT failed(tr("Recording could not start."));
        return false;
    }
    m_state = State::Recording;
    m_paused = false;
    // A previous resume may have shortened the interval to the time that
    // was left; a new recording gets the full cap back.
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
    // The 15-minute cap measures RECORDED audio, so it is suspended too;
    // a paused recorder holds no microphone input.
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
    // Restart with the time that is LEFT, not a fresh 15 minutes: the cap
    // bounds recorded audio, and a pause/resume cycle must not extend it.
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

void VoiceRecorder::cancel()
{
    if (m_state == State::Idle)
        return;
    m_maxDurationGuard.stop();
    m_paused = false;
    m_processingGuard.stop();
    m_cancelRequested = true;
    if (m_decoder) {
        m_decoder->stop();
        m_decoder.reset();
    }
    if (m_recorder
        && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
        // The stopped handler sees m_cancelRequested and discards. Bounded
        // exactly like stop()-driven finalization (review recheck): a
        // backend that never reports StoppedState must not leave the
        // recorder wedged in Processing with a permanently dead mic.
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
    if (m_state != State::Recording)
        return;
    m_maxDurationGuard.stop();
    m_paused = false;
    m_state = State::Processing;
    m_processingGuard.start();
    Q_EMIT stateChanged();
    m_recorder->stop(); // finalization continues in recorderStateChanged
}

void VoiceRecorder::beginWaveformExtraction()
{
    m_decoder = std::make_unique<QAudioDecoder>();
    // A fixed mono float output keeps the peak math format-independent;
    // the FFmpeg decoder resamples internally.
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
        // Review M1: setAudioFormat() is a REQUEST. constData<T>() does no
        // type checking, so reading a non-Float delivery as float would be
        // an out-of-bounds read (Int16 buffers hold half the bytes). A
        // decoder that ignores the request costs only the waveform.
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
    connect(m_decoder.get(), &QAudioDecoder::finished, this, [this] {
        if (m_decodeFormatMismatch) {
            finishWithWaveform({});
            return;
        }
        if (m_chunkSamples > 0)
            m_chunkPeaks.append(m_chunkPeak);
        finishWithWaveform(
            bucketsFromPeaks(m_chunkPeaks, kMaxWaveformBuckets));
    });
    connect(m_decoder.get(),
            qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
            [this] {
        // The recording itself is fine; only the derived waveform is
        // unavailable. Send honestly without one.
        qCInfo(lcVoice, "waveform decode failed; sending without waveform");
        finishWithWaveform({});
    });
    m_decoder->start();
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
    const QString file = m_activeFile;
    const QString mime = m_activeMime;
    const qint64 duration = m_finalDurationMs;
    // Ownership transfers with ready() (review M2/L1): the consumer queues
    // the send — which reads the bytes immediately — and deletes the file
    // when its op resolves. Keeping it referenced here would let the NEXT
    // start() delete a file the send queue had just been handed.
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
    // Normalize against the loudest chunk so quiet recordings still render
    // a legible envelope; a fully silent recording stays flat at zero.
    float loudest = 0.0f;
    for (float peak : peaks)
        loudest = std::max(loudest, peak);
    for (int b = 0; b < buckets; ++b) {
        // Max-preserving downsample: a bucket covers [begin, end) source
        // chunks and takes their loudest value, so short spikes survive.
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
