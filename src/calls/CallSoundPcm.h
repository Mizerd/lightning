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
    /// Interleaved float samples in `format`; never empty.
    QByteArray data;
    qint64 frames() const;
    qint64 durationMs() const;
};

/// `wav` as float PCM with `tailMs` of silence appended, or nothing when it is
/// not a 16-bit PCM WAVE file of one or two channels.
std::optional<Pcm> decodeWav(const QByteArray &wav, int tailMs = 0);

/// `pcm` with `channels` (1 or 2) channels: stereo is averaged to mono, mono
/// is copied to both sides. The same data when it already has them.
Pcm withChannels(const Pcm &pcm, int channels);

/// Sums the playing cues into one stream. Voices are float PCM with the
/// mixer's channel count; a one-shot ends with its data, a loop wraps.
class CueMixer
{
public:
    explicit CueMixer(int channels = 2);

    int channels() const { return m_channels; }
    /// Starts `pcm` (interleaved, channels() channels). Returns its id, or 0
    /// when `pcm` is empty or not whole frames.
    int start(const QByteArray &pcm, float gain, bool loop);
    void stop(int id);
    void setGain(int id, float gain);
    /// Ends every one-shot; loops go on.
    void stopOneShots();
    /// Every one-shot starts again from its first frame, and one queued more
    /// than once (the same PCM) is kept once, the newest; loops go on. For
    /// cues an output took and never played (see CueEngine's stall check).
    void rewindOneShots();
    void clear();
    bool isPlaying(int id) const { return m_voices.contains(id); }
    bool idle() const { return m_voices.isEmpty(); }
    int voiceCount() const { return int(m_voices.size()); }

    /// Mixes `frames` frames into `out` (interleaved, channels() channels)
    /// and returns `frames`, or returns 0 and writes nothing when no voice
    /// is playing. A one-shot that ends inside the block leaves silence
    /// after it; samples are clamped to [-1, 1].
    qint64 mix(float *out, qint64 frames);

private:
    struct Voice {
        QByteArray pcm;
        qint64 position = 0; // in samples
        float gain = 1.0f;
        bool loop = false;
    };

    int m_channels;
    int m_nextId = 1;
    QHash<int, Voice> m_voices;
};

/// What an output pulls from: the mixer's stream while any cue plays, and
/// nothing at all while none does, so a cue that starts later waits behind
/// no pre-mixed silence. kick() tells the output that there is data again.
/// One feed per output: an output's connections to its source die with it.
class MixerFeed : public QIODevice
{
    Q_OBJECT

public:
    explicit MixerFeed(CueMixer *mixer, QObject *parent = nullptr);

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    bool atEnd() const override { return false; }
    void kick();

    /// Offers `frames` of silence, mixed under any cue, even while no cue
    /// plays: something the output must take, so whether it still takes
    /// audio can be seen without making a sound.
    void addSilence(qint64 frames);
    qint64 silenceLeft() const { return m_silenceFrames; }
    /// Whether the output has anything to take now.
    bool busy() const { return !m_mixer->idle() || m_silenceFrames > 0; }
    /// Bytes the output has taken so far.
    quint64 bytesTaken() const { return m_bytesTaken; }

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    CueMixer *m_mixer;
    qint64 m_silenceFrames = 0;
    quint64 m_bytesTaken = 0;
};

} // namespace callsound
