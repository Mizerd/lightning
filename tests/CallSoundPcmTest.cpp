// The Linux Qt 6.10+ call sounds (CallSoundPlayer): the bundled WAVs decode
// sample for sample and anything else is refused; the mixer sums, ends and
// loops voices exactly; and the engine keeps ONE output, opened at preload
// and replaced only on failure, a routing change, an output-list change, or
// when it stops taking audio, against a fake output. Opens no audio device.
#include "calls/CallSoundMixer.h"
#include "calls/CallSoundPcm.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QtEndian>
#include <QtTest/QtTest>

#include <memory>

Q_LOGGING_CATEGORY(lcCallSound, "lightning.calls.sound")

namespace {

QByteArray le16(quint16 v)
{
    QByteArray b(2, 0);
    qToLittleEndian<quint16>(v, b.data());
    return b;
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian<quint32>(v, b.data());
    return b;
}

/// A 48 kHz 16-bit WAV of `channels` holding `samples` (interleaved).
QByteArray wavOf(int channels, const QList<qint16> &samples)
{
    QByteArray data;
    for (qint16 s : samples)
        data += le16(quint16(s));
    const QByteArray fmt = le16(1) + le16(quint16(channels)) + le32(48000)
        + le32(48000 * channels * 2) + le16(quint16(channels * 2)) + le16(16);
    const QByteArray body = QByteArray("WAVE") + QByteArray("fmt ")
        + le32(quint32(fmt.size())) + fmt + QByteArray("data")
        + le32(quint32(data.size())) + data;
    return QByteArray("RIFF") + le32(quint32(body.size())) + body;
}

QByteArray floats(const QList<float> &values)
{
    QByteArray b;
    for (float v : values)
        b.append(reinterpret_cast<const char *>(&v), sizeof v);
    return b;
}

QList<float> asFloats(const QByteArray &bytes)
{
    QList<float> out;
    const float *f = reinterpret_cast<const float *>(bytes.constData());
    for (qsizetype i = 0; i < bytes.size() / qsizetype(sizeof(float)); ++i)
        out << f[i];
    return out;
}

class FakeOutput : public CueOutput
{
public:
    FakeOutput(QByteArray id, QAudioFormat format, QObject *parent)
        : CueOutput(parent), id(std::move(id)), format(std::move(format))
    {
    }

    void start(QIODevice *f) override
    {
        feed = f;
        connect(f, &QIODevice::readyRead, this, [this] { ++kicks; });
        if (failOnStart)
            Q_EMIT failed();
    }
    void suspend() override { isSuspended = true; }
    void resume() override { isSuspended = false; }
    bool suspended() const override { return isSuspended; }
    void stop() override { stopped = true; }

    /// What the real sink would pull now: up to `frames` frames.
    QList<float> pull(qint64 frames)
    {
        QByteArray b(frames * format.channelCount() * qsizetype(sizeof(float)), 0);
        const qint64 n = feed->read(b.data(), b.size());
        b.resize(qMax<qint64>(0, n));
        return asFloats(b);
    }
    void fail() { Q_EMIT failed(); }

    QByteArray id;
    QAudioFormat format;
    QIODevice *feed = nullptr;
    bool isSuspended = false;
    bool stopped = false;
    bool failOnStart = false;
    int kicks = 0;
};

/// A CueEngine over fake outputs and three synthetic stereo sounds:
/// "a" (2 frames), "b" (3 frames, for loops), "c" (1 frame).
struct Rig {
    QSet<QByteArray> outputs{QByteArrayLiteral("default")};
    QByteArray defaultOutput = QByteArrayLiteral("default");
    std::function<bool(const QByteArray &, const QAudioFormat &)> supports =
        [](const QByteArray &, const QAudioFormat &) { return true; };
    bool failNextStart = false;
    QList<QPointer<FakeOutput>> opened;
    QHash<QString, bool> ready;
    std::unique_ptr<CueEngine> engine;
    /// The server's socket, for a rig with the PulseAudio gate.
    bool reachable = true;
    int reachableAsked = 0;

    explicit Rig(bool pulseGate = false)
    {
        CueEngine::Hooks h;
        if (pulseGate) {
            h.serverReachable = [this] {
                ++reachableAsked;
                return reachable;
            };
        }
        h.resolveOutput = [this](const QString &wanted) -> std::optional<QByteArray> {
            if (!wanted.isEmpty() && outputs.contains(wanted.toUtf8()))
                return wanted.toUtf8();
            if (outputs.contains(defaultOutput))
                return defaultOutput;
            return std::nullopt;
        };
        h.supports = [this](const QByteArray &id, const QAudioFormat &f) {
            return supports(id, f);
        };
        h.open = [this](const QByteArray &id, const QAudioFormat &f,
                        QObject *parent) -> CueOutput * {
            if (!outputs.contains(id))
                return nullptr;
            auto *out = new FakeOutput(id, f, parent);
            out->failOnStart = std::exchange(failNextStart, false);
            opened << out;
            return out;
        };
        h.wav = [](const QString &sound) -> std::optional<QByteArray> {
            if (sound == QLatin1String("a"))
                return wavOf(2, {16384, -16384, 8192, -8192});
            if (sound == QLatin1String("b"))
                return wavOf(2, {1000, 1000, 2000, 2000, 3000, 3000});
            if (sound == QLatin1String("c"))
                return wavOf(2, {32767, 32767});
            return std::nullopt;
        };
        h.setReady = [this](const QString &sound, bool r) { ready[sound] = r; };
        engine = std::make_unique<CueEngine>(
            QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")},
            h);
    }

    FakeOutput *current() const
    {
        return static_cast<FakeOutput *>(engine->outputForTest());
    }
    int readyCount() const
    {
        int n = 0;
        for (bool r : ready)
            n += r ? 1 : 0;
        return n;
    }
};

constexpr float k16384 = 16384.0f / 32768.0f;

/// A stall bound for cases that inspect the reopened output: well over
/// QTRY's 50 ms polling step, so the reopened output (which the fake does not
/// drain) is looked at before it could stall in turn.
constexpr int kTestStallMs = 150;

} // namespace

class CallSoundPcmTest : public QObject
{
    Q_OBJECT

private slots:
    // The bundled cues decode to float PCM in the cue format, sample for
    // sample, with the silent tail appended.
    void aBundledCueDecodesSampleForSample()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/join.wav"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray wav = file.readAll();
        const std::optional<callsound::Pcm> pcm = callsound::decodeWav(wav, 50);
        QVERIFY(pcm.has_value());
        QCOMPARE(pcm->format.sampleFormat(), QAudioFormat::Float);
        QCOMPARE(pcm->format.channelCount(), 2);
        QCOMPARE(pcm->format.sampleRate(), 48000);
        const qsizetype frames = (wav.size() - 44) / 4;
        QCOMPARE(pcm->data.size(), (frames + 2400) * 2 * qsizetype(sizeof(float)));
        const float *samples = reinterpret_cast<const float *>(pcm->data.constData());
        for (qsizetype i : { qsizetype(0), frames / 2, frames * 2 - 1 }) {
            const qint16 raw = qFromLittleEndian<qint16>(wav.constData() + 44 + i * 2);
            QCOMPARE(samples[i], float(raw) / 32768.0f);
        }
        for (qsizetype i = frames * 2; i < (frames + 2400) * 2; ++i)
            QCOMPARE(samples[i], 0.0f);
        QCOMPARE(pcm->durationMs(), (frames + 2400) * 1000 / 48000);
        // Every bundled sound decodes.
        const QStringList bundled =
            QDir(QStringLiteral(SOUNDS_DIR)).entryList({QStringLiteral("*.wav")});
        QVERIFY(bundled.size() >= 17);
        for (const QString &name : bundled) {
            QFile each(QStringLiteral(SOUNDS_DIR "/") + name);
            QVERIFY(each.open(QIODevice::ReadOnly));
            QVERIFY2(callsound::decodeWav(each.readAll(), 60).has_value(),
                     qPrintable(name));
        }
    }

    // Anything but 16-bit PCM of one or two channels is refused, a truncated
    // file is refused, and chunks are walked by size (odd sizes padded), so a
    // LIST chunk before the format is skipped.
    void theDecoderRefusesWhatItWasNotMadeFor()
    {
        auto le16 = [](quint16 v) {
            QByteArray b(2, 0);
            qToLittleEndian<quint16>(v, b.data());
            return b;
        };
        auto le32 = [](quint32 v) {
            QByteArray b(4, 0);
            qToLittleEndian<quint32>(v, b.data());
            return b;
        };
        auto make = [&](quint16 encoding, quint16 channels, quint16 bits,
                        const QByteArray &data, const QByteArray &before) {
            QByteArray fmt = le16(encoding) + le16(channels) + le32(48000)
                + le32(48000 * channels * bits / 8)
                + le16(quint16(channels * bits / 8)) + le16(bits);
            QByteArray body = QByteArray("WAVE") + before + QByteArray("fmt ")
                + le32(quint32(fmt.size())) + fmt + QByteArray("data")
                + le32(quint32(data.size())) + data;
            return QByteArray("RIFF") + le32(quint32(body.size())) + body;
        };
        const QByteArray oneFrame = le16(0x4000) + le16(0xC000);
        const QByteArray list = QByteArray("LIST") + le32(3) + QByteArray("abc") + QByteArray(1, 0);

        const auto ok = callsound::decodeWav(make(1, 2, 16, oneFrame, list), 0);
        QVERIFY(ok.has_value());
        QCOMPARE(ok->data.size(), qsizetype(2 * sizeof(float)));
        const float *s = reinterpret_cast<const float *>(ok->data.constData());
        QCOMPARE(s[0], 0.5f);
        QCOMPARE(s[1], -0.5f);

        QVERIFY(!callsound::decodeWav(QByteArray("not a wave file at all"), 0));
        QVERIFY(!callsound::decodeWav(make(3, 2, 32, QByteArray(8, 0), {}), 0)); // float
        QVERIFY(!callsound::decodeWav(make(1, 2, 8, QByteArray(2, 0), {}), 0));  // 8-bit
        QVERIFY(!callsound::decodeWav(make(1, 3, 16, QByteArray(6, 0), {}), 0)); // 3 ch
        QVERIFY(!callsound::decodeWav(make(1, 2, 16, QByteArray(), {}), 0));     // empty
        QByteArray truncated = make(1, 2, 16, oneFrame + oneFrame, {});
        truncated.chop(3);
        QVERIFY(!callsound::decodeWav(truncated, 0));
    }

    void channelsAreConvertedNotReinterpreted()
    {
        const auto stereo = callsound::decodeWav(wavOf(2, {16384, 0, -16384, 16384}));
        QVERIFY(stereo);
        const callsound::Pcm mono = callsound::withChannels(*stereo, 1);
        QCOMPARE(mono.format.channelCount(), 1);
        QCOMPARE(asFloats(mono.data), (QList<float>{k16384 / 2, 0.0f}));
        const callsound::Pcm back = callsound::withChannels(mono, 2);
        QCOMPARE(asFloats(back.data),
                 (QList<float>{k16384 / 2, k16384 / 2, 0.0f, 0.0f}));
    }

    // The mixer sums voices with their gain, clamps, ends a one-shot with its
    // data (silence after it in that block), and then says it has nothing.
    void theMixerSumsEndsAndGoesQuiet()
    {
        callsound::CueMixer mixer(2);
        QCOMPARE(mixer.start(floats({0.5f, 0.5f, 0.5f}), 1.0f, false), 0); // half a frame
        const int a = mixer.start(floats({0.5f, -0.5f, 0.25f, 0.25f}), 1.0f, false);
        const int b = mixer.start(floats({0.75f, -0.75f}), 0.5f, false);
        QVERIFY(a > 0 && b > 0 && a != b);
        float out[8];
        std::fill(std::begin(out), std::end(out), 9.0f);
        QCOMPARE(mixer.mix(out, 4), qint64(4));
        QCOMPARE(QList<float>(std::begin(out), std::end(out)),
                 (QList<float>{0.875f, -0.875f, 0.25f, 0.25f, 0, 0, 0, 0}));
        QVERIFY(mixer.idle());
        std::fill(std::begin(out), std::end(out), 9.0f);
        QCOMPARE(mixer.mix(out, 4), qint64(0));
        QCOMPARE(out[0], 9.0f); // nothing written

        mixer.start(floats({0.9f, 0.9f}), 1.0f, false);
        mixer.start(floats({0.9f, -0.9f}), 1.0f, false);
        mixer.mix(out, 1);
        QCOMPARE(out[0], 1.0f);
        QCOMPARE(out[1], 0.0f);
    }

    // A loop wraps exactly whatever the block sizes, survives stopOneShots(),
    // and its gain follows setGain().
    void aLoopWrapsExactly()
    {
        callsound::CueMixer mixer(1);
        const int loop = mixer.start(floats({0.1f, 0.2f, 0.3f}), 1.0f, true);
        mixer.start(floats({0.5f}), 1.0f, false);
        QList<float> got;
        for (int block : {2, 1, 4, 3}) {
            float out[4];
            QCOMPARE(mixer.mix(out, block), qint64(block));
            got += QList<float>(out, out + block);
        }
        QCOMPARE(got, (QList<float>{0.6f, 0.2f, 0.3f, 0.1f, 0.2f, 0.3f, 0.1f,
                                    0.2f, 0.3f, 0.1f}));
        mixer.start(floats({0.5f}), 1.0f, false);
        mixer.stopOneShots();
        QCOMPARE(mixer.voiceCount(), 1);
        mixer.setGain(loop, 0.5f);
        float out[1];
        mixer.mix(out, 1);
        QCOMPARE(out[0], 0.1f);
        mixer.stop(loop);
        QVERIFY(mixer.idle());
    }

    // The feed offers nothing while nothing plays (so a later cue waits
    // behind no pre-mixed silence), whole frames otherwise, and kick() is the
    // output's readyRead.
    void theFeedOffersNothingWhileIdle()
    {
        callsound::CueMixer mixer(2);
        callsound::MixerFeed feed(&mixer);
        QCOMPARE(feed.bytesAvailable(), qint64(0));
        char buffer[64];
        QCOMPARE(feed.read(buffer, sizeof buffer), qint64(0));
        mixer.start(floats({0.5f, 0.5f}), 1.0f, false);
        QVERIFY(feed.bytesAvailable() > 0);
        QCOMPARE(feed.bytesAvailable() % 8, qint64(0));
        QCOMPARE(feed.read(buffer, 13), qint64(8)); // one whole frame of 13 bytes
        int kicks = 0;
        connect(&feed, &QIODevice::readyRead, this, [&kicks] { ++kicks; });
        feed.kick();
        QCOMPARE(kicks, 1);
    }

    // The silence an output is offered to prove it still takes audio: taken
    // while no cue plays, used up by cues that play over it, and counted with
    // them in what the output took. A rewound one-shot starts again.
    void theFeedOffersSilenceAndCountsWhatWasTaken()
    {
        callsound::CueMixer mixer(2);
        callsound::MixerFeed feed(&mixer);
        QVERIFY(!feed.busy());
        feed.addSilence(3);
        QVERIFY(feed.busy());
        QCOMPARE(feed.bytesAvailable(), qint64(24));
        QByteArray buffer(64, char(1));
        QCOMPARE(feed.read(buffer.data(), 16), qint64(16));
        QCOMPARE(asFloats(buffer.left(16)), (QList<float>{0, 0, 0, 0}));
        QCOMPARE(feed.silenceLeft(), qint64(1));
        QCOMPARE(feed.bytesTaken(), quint64(16));
        const int id = mixer.start(floats({0.5f, 0.5f, 0.25f, 0.25f}), 1.0f, false);
        QCOMPARE(feed.read(buffer.data(), 8), qint64(8)); // the cue, over the silence
        QCOMPARE(feed.silenceLeft(), qint64(0));
        mixer.rewindOneShots();
        QCOMPARE(feed.read(buffer.data(), 16), qint64(16));
        QCOMPARE(asFloats(buffer.left(16)), (QList<float>{0.5f, 0.5f, 0.25f, 0.25f}));
        QVERIFY(!mixer.isPlaying(id));
        QVERIFY(!feed.busy());
        QCOMPARE(feed.read(buffer.data(), 16), qint64(0));
        QCOMPARE(feed.bytesTaken(), quint64(40));

        // The same cue queued three times, nothing heard: once, the newest
        // (its gain); another sound and the loop are kept.
        const QByteArray cue = floats({0.5f, 0.5f});
        mixer.start(cue, 1.0f, false);
        mixer.start(cue, 1.0f, false);
        const int newest = mixer.start(cue, 0.5f, false);
        const int other = mixer.start(floats({0.25f, 0.25f}), 1.0f, false);
        const int loop = mixer.start(cue, 1.0f, true);
        mixer.rewindOneShots();
        QCOMPARE(mixer.voiceCount(), 3);
        QVERIFY(mixer.isPlaying(newest) && mixer.isPlaying(other) && mixer.isPlaying(loop));
    }

    // The stream-per-cue design failed review: a stream opened while the
    // output came and went was freed on PipeWire's thread. One output is
    // opened at preload; fifty cues open nothing more and each wakes it.
    void cuesOpenNoOutput()
    {
        Rig rig;
        rig.engine->preload();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(rig.readyCount(), 3);
        FakeOutput *out = rig.current();
        QVERIFY(out && out->feed);
        QCOMPARE(out->format.channelCount(), 2);
        QCOMPARE(out->format.sampleFormat(), QAudioFormat::Float);
        for (int i = 0; i < 50; ++i)
            rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(out->kicks, 50);
        QCOMPARE(rig.engine->voicesForTest(), 50);
        rig.engine->play(QStringLiteral("nonsense"), 1.0f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 50);
    }

    // End of data: the cue plays once, sample for sample at its gain, and the
    // output is then offered nothing.
    void aCuePlaysOnceAtItsGain()
    {
        Rig rig;
        rig.engine->preload();
        rig.engine->play(QStringLiteral("a"), 0.5f, QString());
        QCOMPARE(rig.current()->pull(4),
                 (QList<float>{k16384 * 0.5f, -k16384 * 0.5f, k16384 * 0.25f,
                               -k16384 * 0.25f, 0, 0, 0, 0}));
        QCOMPARE(rig.current()->pull(4), QList<float>{});
        QCOMPARE(rig.engine->voicesForTest(), 0);
    }

    // A loop goes on, is not doubled by a one-shot of itself, and a second
    // loop() of the same sound only changes its gain.
    void aLoopGoesOnAndIsNotDoubled()
    {
        Rig rig;
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        rig.engine->play(QStringLiteral("b"), 1.0f, QString());
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 1);
        const QList<float> got = rig.current()->pull(5);
        QCOMPARE(got.size(), 10);
        QCOMPARE(got.at(6), got.at(0)); // frame 3 is frame 0 again
        rig.engine->loop(QString(), 0.0f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 0);
    }

    // The output fails (device gone): nothing is playable, the failed output
    // is stopped from the event loop and not inside its own signal, cues are
    // ignored, and the next output-list change opens a new one on which the
    // loop goes on.
    void aFailedOutputIsReplacedOnTheNextOutputChange()
    {
        Rig rig;
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        QPointer<FakeOutput> first = rig.current();
        first->fail();
        QCOMPARE(rig.engine->outputForTest(), nullptr);
        QVERIFY2(!first->stopped, "stopped inside its own failed() signal");
        QCOMPARE(rig.readyCount(), 0);
        QTRY_VERIFY(first.isNull()); // stopped, then deleted, from the event loop
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);

        QCOMPARE(rig.engine->reloadUnusable(), 3);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.readyCount(), 3);
        QCOMPARE(rig.engine->voicesForTest(), 1); // the loop, again
        QCOMPARE(rig.current()->pull(1).size(), 2);
    }

    // An output that fails while it is being started leaves nothing behind
    // and nothing playable, until the outputs change.
    void anOutputThatFailsToStartIsNotKept()
    {
        Rig rig;
        rig.failNextStart = true;
        rig.engine->preload();
        QCOMPARE(rig.engine->outputForTest(), nullptr);
        QCOMPARE(rig.readyCount(), 0);
        QVERIFY(rig.engine->failedForTest(QByteArrayLiteral("default")));
        QTRY_VERIFY(rig.opened.first().isNull());
        rig.engine->reloadUnusable();
        QVERIFY(rig.engine->outputForTest());
        QCOMPARE(rig.readyCount(), 3);
    }

    // An output-list change that leaves the default where it was keeps the
    // output: opening streams while outputs come and go is the hazard.
    void aHealthyOutputIsKeptAcrossAnOutputChange()
    {
        Rig rig;
        rig.engine->preload();
        rig.outputs.insert(QByteArrayLiteral("headset"));
        rig.engine->reloadUnusable();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        // The default moved: the loop follows it, and the old default's
        // output, idle now, is closed.
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        rig.defaultOutput = QByteArrayLiteral("headset");
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->outputIdsForTest(), QList<QByteArray>{QByteArrayLiteral("headset")});
        QVERIFY(rig.opened.first()->stopped);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("headset")), 1);
    }

    // Found in review: one output for every device dragged the call's sounds to
    // whichever device the last cue wanted. A cue for the call's speaker gets
    // an output of its own; the default device's output is left alone.
    void aRoutedCueGetsItsOwnOutput()
    {
        Rig rig;
        rig.outputs.insert(QByteArrayLiteral("speaker"));
        rig.engine->preload();
        FakeOutput *def = rig.current();
        rig.engine->play(QStringLiteral("a"), 1.0f, QStringLiteral("speaker"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("speaker"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("speaker")), 2);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 1);
        QCOMPARE(rig.current(), def);
        QVERIFY(!def->stopped);
        // A speaker that is gone falls back to the default.
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("unplugged"));
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 2);
    }

    // The call-waiting loop plays on the call's speaker; a chime on the
    // default device neither moves it nor closes and reopens its output, and
    // the loop's own cues keep going to the speaker.
    void aLoopStaysOnTheCallDeviceWhileAChimePlaysOnTheDefault()
    {
        Rig rig;
        rig.outputs.insert(QByteArrayLiteral("speaker"));
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QStringLiteral("speaker"));
        auto *speaker = static_cast<FakeOutput *>(
            rig.engine->outputForTest(QByteArrayLiteral("speaker")));
        QVERIFY(speaker);
        const int opened = rig.engine->outputsOpenedForTest();
        const int speakerKicks = speaker->kicks;

        rig.engine->play(QStringLiteral("c"), 1.0f, QString()); // the chime
        QCOMPARE(rig.engine->outputsOpenedForTest(), opened);
        QCOMPARE(rig.engine->outputForTest(QByteArrayLiteral("speaker")), speaker);
        QVERIFY(!speaker->stopped);
        QCOMPARE(speaker->kicks, speakerKicks); // not even woken
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("speaker")), 1);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 1);
        // The loop is what the speaker plays; the chime is what the default
        // plays.
        QCOMPARE(speaker->pull(1), (QList<float>{1000.0f / 32768, 1000.0f / 32768}));
        QCOMPARE(rig.current()->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
        // Re-asserting the loop (as the controller does on every state
        // change) neither restarts nor moves it.
        rig.engine->loop(QStringLiteral("b"), 0.5f, QString());
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("speaker")), 1);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 0);
        QCOMPARE(speaker->pull(1),
                 (QList<float>{2000.0f / 32768 * 0.5f, 2000.0f / 32768 * 0.5f}));
    }

    // The outputs are bounded: past kMaxOutputs the one used longest ago with
    // nothing playing is closed, never the loop's.
    void theOutputsAreBounded()
    {
        Rig rig;
        for (const char *id : {"s1", "s2", "s3"})
            rig.outputs.insert(QByteArray(id));
        rig.engine->preload(); // default
        rig.engine->loop(QStringLiteral("b"), 1.0f, QStringLiteral("s1"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("s2"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("s3"));
        QCOMPARE(int(rig.engine->outputIdsForTest().size()), CueEngine::kMaxOutputs);
        QVERIFY(rig.engine->outputForTest(QByteArrayLiteral("s1"))); // the loop's
        QVERIFY(!rig.engine->outputIdsForTest().contains(QByteArrayLiteral("default")));
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("s1")), 1);
    }

    // The format the routed output accepts: stereo, else mono (the cues are
    // converted), else none, and then nothing is playable.
    void theFormatFallsBackToMonoOrToNothing()
    {
        Rig rig;
        rig.supports = [](const QByteArray &, const QAudioFormat &f) {
            return f.channelCount() == 1
                && f.channelConfig() == QAudioFormat::ChannelConfigMono;
        };
        rig.engine->preload();
        QCOMPARE(rig.current()->format.channelCount(), 1);
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QCOMPARE(rig.current()->pull(2), (QList<float>{0.0f, 0.0f}));

        Rig none;
        none.supports = [](const QByteArray &, const QAudioFormat &) { return false; };
        none.engine->preload();
        QCOMPARE(none.engine->outputsOpenedForTest(), 0);
        QCOMPARE(none.readyCount(), 0);
    }

    // With no output at all nothing opens and nothing is playable; a loop
    // asked for meanwhile starts when an output appears.
    void aLoopWaitsForAnOutput()
    {
        Rig rig;
        rig.outputs.clear();
        rig.engine->preload();
        QCOMPARE(rig.readyCount(), 0);
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 0);
        rig.outputs.insert(QByteArrayLiteral("default"));
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QCOMPARE(rig.readyCount(), 3);
    }

    // Idle: the stream is suspended (kept open) once nothing has played for
    // a while, and the next cue resumes and wakes it.
    void anIdleOutputIsSuspendedAndWokenByTheNextCue()
    {
        Rig rig;
        rig.engine->setIdleSuspendMsForTest(30);
        rig.engine->preload();
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QVERIFY(!rig.current()->suspended());
        rig.current()->pull(8);
        QTRY_VERIFY_WITH_TIMEOUT(rig.current()->suspended(), 2000);
        const int kicks = rig.current()->kicks;
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QVERIFY(!rig.current()->suspended());
        QCOMPARE(rig.current()->kicks, kicks + 1);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        // A loop keeps it running.
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        QTest::qWait(100);
        QVERIFY(!rig.current()->suspended());
    }

    // Found live 2026-10-16: after the sound server restarted, Qt's sink
    // stayed active with no error, took one ring buffer of the next cue and
    // never read again, so every cue queued behind it in silence. An output
    // that takes nothing while a cue waits is reopened on the same device,
    // and the cue plays there from its first frame.
    void aCueOnAnOutputThatStoppedTakingAudioPlaysOnAReopenedOne()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->preload();
        QPointer<FakeOutput> dead = rig.current();
        rig.engine->play(QStringLiteral("a"), 0.5f, QString());
        QCOMPARE(dead->pull(1).size(), 2); // its ring buffer, then nothing
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsOpenedForTest(), 2, 2000);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 1);
        QVERIFY(dead.isNull() || dead->stopped);
        QCOMPARE(rig.engine->outputIdsForTest(), QList<QByteArray>{QByteArrayLiteral("default")});
        FakeOutput *fresh = rig.current();
        QVERIFY(fresh && fresh != dead);
        QVERIFY(!fresh->suspended());
        QVERIFY(fresh->kicks > 0);
        QCOMPARE(fresh->pull(2),
                 (QList<float>{k16384 * 0.5f, -k16384 * 0.5f, k16384 * 0.25f,
                               -k16384 * 0.25f}));
        QCOMPARE(rig.engine->voicesForTest(), 0);
        QCOMPARE(rig.readyCount(), 3);
        // The next cue opens nothing more and plays at once.
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(fresh->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

    // The ringer on an output that stopped taking audio goes on on the
    // reopened one, as the same voice: re-asserting it adjusts its gain and
    // neither doubles nor restarts it.
    void aLoopIsCarriedToTheReopenedOutput()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        rig.current()->pull(1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 2000);
        rig.engine->loop(QStringLiteral("b"), 0.5f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QCOMPARE(rig.current()->pull(1),
                 (QList<float>{2000.0f / 32768 * 0.5f, 2000.0f / 32768 * 0.5f}));
    }

    // After an output change (the Pulse backend reports one when the server
    // comes back) the kept output is offered a short silence: a dead one is
    // found and reopened before the next cue, so that cue is not late. At
    // once, not after the settle time: Qt listed the change over a live
    // connection.
    void anOutputChangeFindsADeadOutputBeforeTheNextCue()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        QPointer<FakeOutput> dead = rig.current();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->voicesForTest(), 0); // the silence is no cue
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 2000);
        QVERIFY(dead.isNull() || dead->stopped);
        FakeOutput *fresh = rig.current();
        QVERIFY(fresh && fresh != dead);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(fresh->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

    // Found live 2026-10-16: a new stream while Qt's PulseAudio backend is
    // still reconnecting aborts the process (libpulse asserts). An output
    // found dead by a cue, as the server goes away, is reopened only after an
    // output change or the settle time, never at once; what was queued on it
    // meanwhile plays then, a cue queued twice once.
    void anOutputFoundDeadByACueWaitsForTheServerToComeBack()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        rig.engine->play(QStringLiteral("a"), 0.5f, QString());
        rig.engine->play(QStringLiteral("a"), 0.5f, QString());
        QTest::qWait(300);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        rig.engine->reloadUnusable(); // Qt listed the outputs again
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 1000);
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QCOMPARE(rig.current()->pull(2),
                 (QList<float>{k16384 * 0.5f, -k16384 * 0.5f, k16384 * 0.25f,
                               -k16384 * 0.25f}));

        // No output change at all (Qt's PipeWire backend reports none): the
        // settle time.
        Rig quiet;
        quiet.engine->setStallMsForTest(kTestStallMs, 60000, 500);
        quiet.engine->preload();
        quiet.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTest::qWait(250);
        QCOMPARE(quiet.engine->outputsReopenedForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(quiet.engine->outputsReopenedForTest(), 1, 2000);
        QCOMPARE(quiet.current()->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

    // Found in review: an output that took its whole silence between two
    // checks was never seen taking it, so it stayed "found by the output
    // change" (and a later stall, as the server goes away, was reopened at
    // once, into Qt's reconnect) and a reopened one stayed "unproven" (and
    // its next stall was given up instead of reopened).
    void whatAnOutputTookBetweenTwoChecksCounts()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.current()->pull(CueEngine::kProbeMs * 48).size(),
                 CueEngine::kProbeMs * 48 * 2); // all of it, at once
        QTest::qWait(3 * kTestStallMs);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTest::qWait(3 * kTestStallMs);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0); // waits for the settle

        Rig again;
        again.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        again.engine->preload();
        again.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTRY_COMPARE_WITH_TIMEOUT(again.engine->outputsReopenedForTest(), 1, 2000);
        // The reopened output works: it takes the cue and its silence at once.
        QVERIFY(again.current());
        again.current()->pull(CueEngine::kProbeMs * 48 + 1);
        QTest::qWait(3 * kTestStallMs);
        // The server goes away again.
        again.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTRY_COMPARE_WITH_TIMEOUT(again.engine->outputsReopenedForTest(), 2, 2000);
        QVERIFY(!again.engine->failedForTest(QByteArrayLiteral("default")));
    }

    // Review 2026-10-16 (H1): neither the settle time nor an output change
    // proves Qt has reconnected to PulseAudio, and a stream opened before it
    // has aborts the process. Where the gate applies a dead output is
    // reopened only once the server's socket has answered without a break
    // for the live window, counted again from any break.
    void aPulseOutputIsReopenedOnlyOnceTheServerAnswersLongEnough()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->setServerLiveMsForTest(600);
        rig.engine->setGatePollMsForTest(30, 60);
        rig.engine->preload();
        rig.reachable = false;
        rig.engine->play(QStringLiteral("a"), 0.5f, QString());
        QTest::qWait(1000);
        QVERIFY(rig.reachableAsked > 0);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        rig.reachable = true;
        QTest::qWait(300);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0); // not long enough
        rig.reachable = false; // a break
        QTest::qWait(150);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        rig.reachable = true;
        QElapsedTimer sinceBack;
        sinceBack.start();
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 3000);
        QVERIFY2(sinceBack.elapsed() >= 550, "reopened before the window after the break");
        QVERIFY(rig.current());
        QCOMPARE(rig.current()->pull(2),
                 (QList<float>{k16384 * 0.5f, -k16384 * 0.5f, k16384 * 0.25f,
                               -k16384 * 0.25f}));
    }

    // A server that never comes back, or comes back where this process cannot
    // reach it (a Flatpak's bind-mounted socket inode): never reopened, by a
    // cue or by an output change. Silent, and nothing opened.
    void aPulseServerThatNeverAnswersIsNeverReopened()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->setServerLiveMsForTest(300);
        rig.engine->setGatePollMsForTest(30, 60);
        rig.engine->preload();
        rig.reachable = false;
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTest::qWait(600);
        rig.engine->reloadUnusable(); // even "found by the output change"
        QTest::qWait(1200);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(rig.engine->voicesForTest(), 1); // still waiting
        QVERIFY(!rig.engine->failedForTest(QByteArrayLiteral("default")));
    }

    // H1 (b): the silence of an output change found it dead, but the change
    // was reported while the server was still there (a graceful restart
    // removes devices first): it waits for the server like any other.
    void aPulseOutputFoundByAnOutputChangeStillWaitsForTheServer()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->setServerLiveMsForTest(400);
        rig.engine->setGatePollMsForTest(30, 60);
        rig.engine->preload();
        rig.reachable = false;
        rig.engine->reloadUnusable();
        QTest::qWait(700);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        rig.reachable = true;
        QElapsedTimer sinceBack;
        sinceBack.start();
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 3000);
        QVERIFY(sinceBack.elapsed() >= 350);
    }

    // Follow-up to the review: a gate that keeps saying no is asked less and
    // less often (doubling to a ceiling), and every tick again once it says
    // yes, since the live window needs dense samples.
    void theGateBacksOffWhileItSaysNo()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0); // 30 ms ticks
        rig.engine->setServerLiveMsForTest(1500);
        rig.engine->setGatePollMsForTest(50, 400);
        rig.engine->preload();
        rig.reachable = false;
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTest::qWait(400); // found dead
        const int before = rig.reachableAsked;
        QVERIFY(before > 0);
        QTest::qWait(2000);
        const int whileNo = rig.reachableAsked - before;
        // Every tick would be ~66; 50, 100, 200, 400, 400, ... is ~6.
        QVERIFY2(whileNo >= 3 && whileNo <= 10, qPrintable(QString::number(whileNo)));
        // A yes too short for the window: from it, asked every tick again,
        // and a no after it starts the back-off from the bottom.
        rig.reachable = true;
        QTest::qWait(700); // the next ask is at most 400 ms away
        rig.reachable = false;
        const int atNo = rig.reachableAsked;
        QTest::qWait(300);
        QVERIFY2(rig.reachableAsked - atNo >= 2, // a tick, then +50, +100 (not +400)
                 qPrintable(QString::number(rig.reachableAsked - atNo)));
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        const int beforeYes = rig.reachableAsked;
        rig.reachable = true;
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 4000);
        // From the first yes to the reopen it was asked every tick.
        QVERIFY2(rig.reachableAsked - beforeYes >= 12,
                 qPrintable(QString::number(rig.reachableAsked - beforeYes)));
    }

    // The retry of a given-up output waits for the server too, and a retry
    // timer from before an output change does nothing (L3).
    void theRetryOfAGivenUpOutputWaitsForTheServerAndIsNotStale()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 300, 0);
        rig.engine->setServerLiveMsForTest(200);
        rig.engine->setGatePollMsForTest(30, 60);
        rig.engine->preload();
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTRY_VERIFY_WITH_TIMEOUT(rig.engine->failedForTest(QByteArrayLiteral("default")), 4000);
        rig.reachable = false;
        QTest::qWait(800); // the retry is due, the server is not there
        QVERIFY(rig.engine->failedForTest(QByteArrayLiteral("default")));
        QCOMPARE(rig.readyCount(), 0);
        rig.reachable = true;
        QTRY_VERIFY_WITH_TIMEOUT(!rig.engine->failedForTest(QByteArrayLiteral("default")), 2000);
        QCOMPARE(rig.readyCount(), 3);

        Rig stale;
        stale.engine->setStallMsForTest(kTestStallMs, 2000, 0);
        stale.engine->preload();
        stale.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTRY_VERIFY_WITH_TIMEOUT(stale.engine->failedForTest(QByteArrayLiteral("default")), 4000);
        QElapsedTimer sinceFirst;
        sinceFirst.start();
        stale.engine->reloadUnusable(); // clears it; its timer is now stale
        QVERIFY(!stale.engine->failedForTest(QByteArrayLiteral("default")));
        stale.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTRY_VERIFY_WITH_TIMEOUT(stale.engine->failedForTest(QByteArrayLiteral("default")), 4000);
        while (sinceFirst.elapsed() < 2200)
            QTest::qWait(20);
        QVERIFY2(stale.engine->failedForTest(QByteArrayLiteral("default")),
                 "the first retry timer cleared the second failure");
    }

    // L2: the ringer stopped while its dead output waits: nothing is left to
    // play, so nothing is reopened for it and the check stops; an output
    // change later finds the output dead and reopens it, without the loop.
    void theRingerStoppedWhileItsOutputWaits()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        QTest::qWait(500);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        rig.engine->loop(QString(), 0.0f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.engine->stallTimerActiveForTest(), 1000);
        QTest::qWait(300);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        rig.engine->reloadUnusable();
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 2000);
        QCOMPARE(rig.engine->voicesForTest(), 0);
    }

    // L2: the stall check runs only while something waits to be taken: it
    // stops once everything drained, and at stopAll().
    void theStallCheckStopsWhenNothingWaits()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->preload();
        QVERIFY(!rig.engine->stallTimerActiveForTest());
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QVERIFY(rig.engine->stallTimerActiveForTest());
        rig.current()->pull(8);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.engine->stallTimerActiveForTest(), 1000);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QVERIFY(rig.engine->stallTimerActiveForTest());
        rig.engine->stopAll();
        QVERIFY(!rig.engine->stallTimerActiveForTest());
    }

    // M1: an output the change itself opened has nothing to prove and is
    // offered no silence; and one slow to start (Bluetooth: 0.5-2 s) is not
    // condemned by the default bound.
    void aNewOrSlowOutputIsNotCondemned()
    {
        Rig rig;
        rig.engine->preload();
        rig.outputs.insert(QByteArrayLiteral("headset"));
        rig.defaultOutput = QByteArrayLiteral("headset");
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputIdsForTest(), QList<QByteArray>{QByteArrayLiteral("headset")});
        QCOMPARE(rig.current()->pull(1), QList<float>{}); // nothing offered

        rig.engine->reloadUnusable(); // now it was open before: offered
        QTest::qWait(1500); // starting up, takes nothing yet
        QCOMPARE(rig.current()->pull(CueEngine::kProbeMs * 48).size(),
                 CueEngine::kProbeMs * 48 * 2);
        QTest::qWait(1500);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
    }

    // L5: an output change withdraws "unproven": a reopened output that then
    // stalls after the change is reopened again, not given up.
    void anOutputChangeWithdrawsUnproven()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->preload();
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 2000);
        rig.engine->reloadUnusable();
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 2, 2000);
        QVERIFY(!rig.engine->failedForTest(QByteArrayLiteral("default")));
    }

    // A healthy output is never reopened: not while it takes a loop, not
    // after it took the silence an output change offers it, and not when
    // the GUI thread was held up (a late check judges nothing).
    void aHealthyOutputIsNeverReopened()
    {
        Rig rig;
        rig.engine->setStallMsForTest(300, 60000, 0);
        rig.engine->preload();
        QPointer<FakeOutput> out = rig.current();
        rig.engine->reloadUnusable();
        QCOMPARE(out->pull(CueEngine::kProbeMs * 48).size(), CueEngine::kProbeMs * 48 * 2);
        QCOMPARE(out->pull(1), QList<float>{}); // the silence is all taken
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        // What the real sink would do, as long as this one is still open.
        const auto take = [&out] {
            if (out)
                out->pull(1);
        };
        for (int i = 0; i < 40; ++i) {
            take();
            QTest::qWait(25);
        }
        // Held up past the check (nothing taken since it last looked), then
        // taking again as soon as it can.
        QTest::qWait(150);
        QThread::msleep(1200);
        QCoreApplication::processEvents();
        take();
        QTest::qWait(100);
        take();
        QTest::qWait(100);
        take();
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(rig.current(), out.data());
    }

    // An output that takes nothing even after it was reopened (the server is
    // still gone) is given up like a failed one, opens no stream per cue
    // meanwhile, and is tried again after the retry or an output change.
    void anOutputThatStaysDeadIsGivenUpAndRetried()
    {
        Rig rig;
        rig.engine->setStallMsForTest(60, 400, 0);
        rig.engine->preload();
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTRY_VERIFY_WITH_TIMEOUT(rig.engine->failedForTest(QByteArrayLiteral("default")), 3000);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 1);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->outputForTest(), nullptr);
        QCOMPARE(rig.readyCount(), 0);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(rig.readyCount(), 3, 3000);
        QVERIFY(!rig.engine->failedForTest(QByteArrayLiteral("default")));
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 3);
        QCOMPARE(rig.current()->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }
};

QTEST_GUILESS_MAIN(CallSoundPcmTest)
#include "CallSoundPcmTest.moc"
