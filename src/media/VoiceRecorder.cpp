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
        // The cap must never publish: stop() feeds the send path, and a
        // forgotten open mic must not upload fifteen minutes of audio. Discard
        // instead.
        if (recording()) {
            qCInfo(lcVoice, "recording hit the hard duration cap; discarding");
            cancel();
            Q_EMIT failed(tr("The recording reached the maximum length and "
                             "was discarded."));
        }
    });
    // Finalization is bounded too, so a backend that never reports StoppedState
    // cannot trap the composer in the recording pill.
    m_processingGuard.setSingleShot(true);
    m_processingGuard.setInterval(15000);
    connect(&m_processingGuard, &QTimer::timeout, this, [this] {
        if (m_state != State::Processing)
            return;
        qCWarning(lcVoice, "finalization timed out; discarding");
        m_cancelRequested = true;
        // Abandon any decode in flight so a queued finishLater() hop cannot
        // land.
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
    // Release the lock before the QTemporaryDir removes the recordings.
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
        // Same scratch root as other decrypted media (inside the portable
        // folder when portable).
        m_dir = std::make_unique<QTemporaryDir>(
            lightning::portable::mediaScratchRoot()
            + QStringLiteral("/lightning-voice-XXXXXX"));
        // Marked live so another instance's startup sweep leaves it alone;
        // released in the destructor.
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
                    // The recorder resolves the real output location.
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
                    // Error strings can contain device or file details; log the
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
    // A new recording supersedes an unsent one; the directory is wiped at
    // destruction, so only the previous file is dropped here.
    discardActiveFile();

    QMediaFormat format(profile->container);
    format.setAudioCodec(profile->codec);
    m_recorder->setMediaFormat(format);
    // Voice profile: 32 kbps mono Opus at 48 kHz. Otherwise the recorder
    // inherits the device's channel layout and bitrate (a 4-channel array gave
    // 192 kbps quad Opus).
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
    // A new recording gets the full cap back.
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
    // The cap measures recorded audio, so it pauses too.
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
    // Resume with the remaining time; pausing must not extend the cap.
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
        // Bump first so a hop already queued from this decoder is stale.
        ++m_decodeTag;
        m_decoder->stop();
        m_decoder.reset();
    }
    if (m_recorder
        && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
        // The stopped handler sees m_cancelRequested and discards. Bounded like
        // stop(), so a backend that never stops cannot leave the mic dead.
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
    // Identifies this decode across finishLater()'s queued hop; bumped by
    // anything that abandons a decode.
    const quint64 tag = ++m_decodeTag;
    m_decoder = std::make_unique<QAudioDecoder>();
    // Fixed mono float output keeps the peak math format-independent.
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
        // Only the waveform is unavailable; send without it.
        qCInfo(lcVoice, "waveform decode failed; sending without waveform");
        finishLater(tag, {});
    });
    m_decoder->start();
}

void VoiceRecorder::finishLater(quint64 tag, const QList<int> &waveform)
{
    // Never destroy the decoder inside its own signal emission
    // (finishWithWaveform resets it, and QObject touches the sender after the
    // slot returns). Hop back to the event loop and check the tag, which also
    // keeps completion once-only when both `error` and `finished` fire.
    QMetaObject::invokeMethod(
        this,
        [this, tag, waveform] {
            if (m_decodeTag != tag)
                return;
            ++m_decodeTag; // this decode is over; a sibling hop is stale
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
    const QString file = m_activeFile;
    const QString mime = m_activeMime;
    const qint64 duration = m_finalDurationMs;
    // Ownership moves with ready(): the send queue reads the file and deletes
    // it when done, so the next start() must not delete it.
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
    // Normalize to the loudest chunk so quiet recordings still show an
    // envelope.
    float loudest = 0.0f;
    for (float peak : peaks)
        loudest = std::max(loudest, peak);
    for (int b = 0; b < buckets; ++b) {
        // Max-preserving downsample, so short spikes survive.
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
