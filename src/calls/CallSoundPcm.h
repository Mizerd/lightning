// The call sounds as plain PCM, and the mixer that plays them through one
// long-lived output (CallSoundPlayer on Linux with Qt 6.10 and later; see
// CallSoundPlayer.h for why QSoundEffect is not used there).
//
// Only what scripts/generate-call-sounds.py writes is accepted: RIFF/WAVE,
// 16-bit integer PCM, one or two channels. It is mixed as 32-bit float, the
// format QSoundEffect's own engine used.
//
// Everything here runs on one thread (the GUI thread): the output pulls the
// mixed audio from MixerFeed there and only hands a ring buffer to the audio
// backend's real-time thread.
#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QHash>
#include <QIODevice>

#include <optional>

namespace callsound {

struct Pcm {
    QAudioFormat format;
    QByteArray data;
    qint64 frames() const;
    qint64 durationMs() const;
};

std::optional<Pcm> decodeWav(const QByteArray &wav, int tailMs = 0);

Pcm withChannels(const Pcm &pcm, int channels);

class CueMixer
{
public:
    explicit CueMixer(int channels = 2);

    int channels() const { return m_channels; }
    int start(const QByteArray &pcm, float gain, bool loop);
    void stop(int id);
    void setGain(int id, float gain);
    void stopOneShots();
    void rewindOneShots();
    void clear();
    bool isPlaying(int id) const { return m_voices.contains(id); }
    bool idle() const { return m_voices.isEmpty(); }
    int voiceCount() const { return int(m_voices.size()); }

    qint64 mix(float *out, qint64 frames);

private:
    struct Voice {
        QByteArray pcm;
        qint64 position = 0;
        float gain = 1.0f;
        bool loop = false;
    };

    int m_channels;
    int m_nextId = 1;
    QHash<int, Voice> m_voices;
};

class MixerFeed : public QIODevice
{
    Q_OBJECT

public:
    explicit MixerFeed(CueMixer *mixer, QObject *parent = nullptr);

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    bool atEnd() const override { return false; }
    void kick();

    void addSilence(qint64 frames);
    qint64 silenceLeft() const { return m_silenceFrames; }
    bool busy() const { return !m_mixer->idle() || m_silenceFrames > 0; }
    quint64 bytesTaken() const { return m_bytesTaken; }

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    CueMixer *m_mixer;
    qint64 m_silenceFrames = 0;
    quint64 m_bytesTaken = 0;
};

}
