#include "calls/CallSoundPcm.h"

#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

namespace callsound {

namespace {
// The longest call sound is a few seconds; refuse anything absurd rather than
// allocate for it.
constexpr qint64 kMaxDataBytes = 64 * 1024 * 1024;

quint16 u16At(const QByteArray &bytes, qsizetype at)
{
    return qFromLittleEndian<quint16>(bytes.constData() + at);
}

quint32 u32At(const QByteArray &bytes, qsizetype at)
{
    return qFromLittleEndian<quint32>(bytes.constData() + at);
}
} // namespace

qint64 Pcm::durationMs() const
{
    const qint32 frameBytes = format.bytesPerFrame();
    if (frameBytes <= 0 || format.sampleRate() <= 0)
        return 0;
    return (qint64(data.size()) / frameBytes) * 1000 / format.sampleRate();
}

std::optional<Pcm> decodeWav(const QByteArray &wav, int tailMs)
{
    if (wav.size() < 12 || std::memcmp(wav.constData(), "RIFF", 4) != 0
        || std::memcmp(wav.constData() + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }
    int channels = 0;
    int sampleRate = 0;
    bool haveFormat = false;
    qsizetype dataAt = -1;
    qsizetype dataSize = 0;
    qsizetype at = 12;
    while (at + 8 <= wav.size()) {
        const QByteArray id = wav.mid(at, 4);
        const quint32 size = u32At(wav, at + 4);
        const qsizetype body = at + 8;
        if (qint64(size) > qint64(wav.size()) - body)
            return std::nullopt; // truncated
        if (id == "fmt ") {
            if (size < 16)
                return std::nullopt;
            const quint16 encoding = u16At(wav, body);
            channels = u16At(wav, body + 2);
            sampleRate = int(u32At(wav, body + 4));
            const quint16 blockAlign = u16At(wav, body + 12);
            const quint16 bits = u16At(wav, body + 14);
            if (encoding != 1 || bits != 16 || (channels != 1 && channels != 2)
                || sampleRate <= 0 || sampleRate > 384000
                || blockAlign != channels * 2) {
                return std::nullopt;
            }
            haveFormat = true;
        } else if (id == "data") {
            dataAt = body;
            dataSize = qsizetype(size);
        }
        // Chunks are padded to an even size.
        at = body + qsizetype(size) + (size & 1u);
    }
    if (!haveFormat || dataAt < 0 || dataSize <= 0 || dataSize > kMaxDataBytes)
        return std::nullopt;
    const qsizetype frameBytes = qsizetype(channels) * 2;
    const qsizetype frames = dataSize / frameBytes;
    if (frames <= 0)
        return std::nullopt;
    const qsizetype tailFrames =
        qsizetype(qMax(0, tailMs)) * sampleRate / 1000;

    Pcm pcm;
    pcm.format.setSampleFormat(QAudioFormat::Float);
    pcm.format.setChannelCount(channels);
    pcm.format.setChannelConfig(channels == 1 ? QAudioFormat::ChannelConfigMono
                                              : QAudioFormat::ChannelConfigStereo);
    pcm.format.setSampleRate(sampleRate);
    const qsizetype samples = frames * channels;
    const qsizetype totalSamples = samples + tailFrames * channels;
    pcm.data.resize(totalSamples * qsizetype(sizeof(float)));
    float *out = reinterpret_cast<float *>(pcm.data.data());
    const char *in = wav.constData() + dataAt;
    for (qsizetype i = 0; i < samples; ++i) {
        const qint16 s = qFromLittleEndian<qint16>(in + i * 2);
        out[i] = float(s) / 32768.0f;
    }
    for (qsizetype i = samples; i < totalSamples; ++i)
        out[i] = 0.0f;
    return pcm;
}

qint64 Pcm::frames() const
{
    const qint32 frameBytes = format.bytesPerFrame();
    return frameBytes > 0 ? qint64(data.size()) / frameBytes : 0;
}

Pcm withChannels(const Pcm &pcm, int channels)
{
    const int from = pcm.format.channelCount();
    if (from == channels || (channels != 1 && channels != 2)
        || (from != 1 && from != 2)) {
        return pcm;
    }
    const qint64 frames = pcm.frames();
    Pcm out;
    out.format = pcm.format;
    out.format.setChannelCount(channels);
    out.format.setChannelConfig(channels == 1 ? QAudioFormat::ChannelConfigMono
                                              : QAudioFormat::ChannelConfigStereo);
    out.data.resize(frames * channels * qsizetype(sizeof(float)));
    const float *in = reinterpret_cast<const float *>(pcm.data.constData());
    float *o = reinterpret_cast<float *>(out.data.data());
    for (qint64 f = 0; f < frames; ++f) {
        if (channels == 1) {
            o[f] = 0.5f * (in[2 * f] + in[2 * f + 1]);
        } else {
            o[2 * f] = in[f];
            o[2 * f + 1] = in[f];
        }
    }
    return out;
}

CueMixer::CueMixer(int channels) : m_channels(channels == 1 ? 1 : 2) {}

int CueMixer::start(const QByteArray &pcm, float gain, bool loop)
{
    const qsizetype frameBytes = qsizetype(sizeof(float)) * m_channels;
    if (pcm.isEmpty() || pcm.size() % frameBytes != 0)
        return 0;
    const int id = m_nextId++;
    if (m_nextId <= 0)
        m_nextId = 1;
    Voice voice;
    voice.pcm = pcm; // shared, not copied
    voice.gain = gain;
    voice.loop = loop;
    m_voices.insert(id, voice);
    return id;
}

void CueMixer::stop(int id)
{
    m_voices.remove(id);
}

void CueMixer::setGain(int id, float gain)
{
    const auto it = m_voices.find(id);
    if (it != m_voices.end())
        it->gain = gain;
}

void CueMixer::stopOneShots()
{
    for (auto it = m_voices.begin(); it != m_voices.end();)
        it = it->loop ? std::next(it) : m_voices.erase(it);
}

void CueMixer::clear()
{
    m_voices.clear();
}

qint64 CueMixer::mix(float *out, qint64 frames)
{
    if (m_voices.isEmpty() || frames <= 0)
        return 0;
    const qint64 samples = frames * m_channels;
    std::fill(out, out + samples, 0.0f);
    for (auto it = m_voices.begin(); it != m_voices.end();) {
        Voice &voice = it.value();
        const float *in = reinterpret_cast<const float *>(voice.pcm.constData());
        const qint64 length = voice.pcm.size() / qsizetype(sizeof(float));
        qint64 written = 0;
        bool finished = false;
        while (written < samples) {
            const qint64 chunk = qMin(samples - written, length - voice.position);
            for (qint64 i = 0; i < chunk; ++i)
                out[written + i] += in[voice.position + i] * voice.gain;
            written += chunk;
            voice.position += chunk;
            if (voice.position >= length) {
                if (!voice.loop) {
                    finished = true;
                    break;
                }
                voice.position = 0;
            }
        }
        if (finished)
            it = m_voices.erase(it);
        else
            ++it;
    }
    for (qint64 i = 0; i < samples; ++i)
        out[i] = qBound(-1.0f, out[i], 1.0f);
    return frames;
}

MixerFeed::MixerFeed(CueMixer *mixer, QObject *parent)
    : QIODevice(parent), m_mixer(mixer)
{
    // Unbuffered: QIODevice's own read-ahead would mix far ahead of the
    // output and delay every cue that starts after it.
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

qint64 MixerFeed::bytesAvailable() const
{
    if (m_mixer->idle())
        return 0;
    // Endless while anything plays, in whole frames.
    const qint64 frameBytes = qint64(sizeof(float)) * m_mixer->channels();
    return (std::numeric_limits<qint32>::max() / frameBytes) * frameBytes;
}

void MixerFeed::kick()
{
    Q_EMIT readyRead();
}

qint64 MixerFeed::readData(char *data, qint64 maxSize)
{
    const qint64 frameBytes = qint64(sizeof(float)) * m_mixer->channels();
    const qint64 frames = maxSize / frameBytes;
    return m_mixer->mix(reinterpret_cast<float *>(data), frames) * frameBytes;
}

} // namespace callsound
