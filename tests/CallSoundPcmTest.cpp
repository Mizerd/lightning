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

struct Rig {
    QSet<QByteArray> outputs{QByteArrayLiteral("default")};
    QByteArray defaultOutput = QByteArrayLiteral("default");
    std::function<bool(const QByteArray &, const QAudioFormat &)> supports =
        [](const QByteArray &, const QAudioFormat &) { return true; };
    bool failNextStart = false;
    QList<QPointer<FakeOutput>> opened;
    QHash<QString, bool> ready;
    std::unique_ptr<CueEngine> engine;
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

constexpr int kTestStallMs = 150;

}

class CallSoundPcmTest : public QObject
{
    Q_OBJECT

private slots:
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
        QVERIFY(!callsound::decodeWav(make(3, 2, 32, QByteArray(8, 0), {}), 0));
        QVERIFY(!callsound::decodeWav(make(1, 2, 8, QByteArray(2, 0), {}), 0));
        QVERIFY(!callsound::decodeWav(make(1, 3, 16, QByteArray(6, 0), {}), 0));
        QVERIFY(!callsound::decodeWav(make(1, 2, 16, QByteArray(), {}), 0));
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

    void theMixerSumsEndsAndGoesQuiet()
    {
        callsound::CueMixer mixer(2);
        QCOMPARE(mixer.start(floats({0.5f, 0.5f, 0.5f}), 1.0f, false), 0);
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
        QCOMPARE(out[0], 9.0f);

        mixer.start(floats({0.9f, 0.9f}), 1.0f, false);
        mixer.start(floats({0.9f, -0.9f}), 1.0f, false);
        mixer.mix(out, 1);
        QCOMPARE(out[0], 1.0f);
        QCOMPARE(out[1], 0.0f);
    }

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
        QCOMPARE(feed.read(buffer, 13), qint64(8));
        int kicks = 0;
        connect(&feed, &QIODevice::readyRead, this, [&kicks] { ++kicks; });
        feed.kick();
        QCOMPARE(kicks, 1);
    }

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
        QCOMPARE(feed.read(buffer.data(), 8), qint64(8));
        QCOMPARE(feed.silenceLeft(), qint64(0));
        mixer.rewindOneShots();
        QCOMPARE(feed.read(buffer.data(), 16), qint64(16));
        QCOMPARE(asFloats(buffer.left(16)), (QList<float>{0.5f, 0.5f, 0.25f, 0.25f}));
        QVERIFY(!mixer.isPlaying(id));
        QVERIFY(!feed.busy());
        QCOMPARE(feed.read(buffer.data(), 16), qint64(0));
        QCOMPARE(feed.bytesTaken(), quint64(40));

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
        QCOMPARE(got.at(6), got.at(0));
        rig.engine->loop(QString(), 0.0f, QString());
        QCOMPARE(rig.engine->voicesForTest(), 0);
    }

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
        QTRY_VERIFY(first.isNull());
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);

        QCOMPARE(rig.engine->reloadUnusable(), 3);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.readyCount(), 3);
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QCOMPARE(rig.current()->pull(1).size(), 2);
    }

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

    void aHealthyOutputIsKeptAcrossAnOutputChange()
    {
        Rig rig;
        rig.engine->preload();
        rig.outputs.insert(QByteArrayLiteral("headset"));
        rig.engine->reloadUnusable();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        rig.defaultOutput = QByteArrayLiteral("headset");
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->outputIdsForTest(), QList<QByteArray>{QByteArrayLiteral("headset")});
        QVERIFY(rig.opened.first()->stopped);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("headset")), 1);
    }

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
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("unplugged"));
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 2);
    }

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

        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), opened);
        QCOMPARE(rig.engine->outputForTest(QByteArrayLiteral("speaker")), speaker);
        QVERIFY(!speaker->stopped);
        QCOMPARE(speaker->kicks, speakerKicks);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("speaker")), 1);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 1);
        QCOMPARE(speaker->pull(1), (QList<float>{1000.0f / 32768, 1000.0f / 32768}));
        QCOMPARE(rig.current()->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
        rig.engine->loop(QStringLiteral("b"), 0.5f, QString());
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("speaker")), 1);
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("default")), 0);
        QCOMPARE(speaker->pull(1),
                 (QList<float>{2000.0f / 32768 * 0.5f, 2000.0f / 32768 * 0.5f}));
    }

    void theOutputsAreBounded()
    {
        Rig rig;
        for (const char *id : {"s1", "s2", "s3"})
            rig.outputs.insert(QByteArray(id));
        rig.engine->preload();
        rig.engine->loop(QStringLiteral("b"), 1.0f, QStringLiteral("s1"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("s2"));
        rig.engine->play(QStringLiteral("c"), 1.0f, QStringLiteral("s3"));
        QCOMPARE(int(rig.engine->outputIdsForTest().size()), CueEngine::kMaxOutputs);
        QVERIFY(rig.engine->outputForTest(QByteArrayLiteral("s1")));
        QVERIFY(!rig.engine->outputIdsForTest().contains(QByteArrayLiteral("default")));
        QCOMPARE(rig.engine->voicesForTest(QByteArrayLiteral("s1")), 1);
    }

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
        QCOMPARE(dead->pull(1).size(), 2);
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
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(rig.engine->outputsOpenedForTest(), 2);
        QCOMPARE(fresh->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

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

    void anOutputChangeFindsADeadOutputBeforeTheNextCue()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        QPointer<FakeOutput> dead = rig.current();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->voicesForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 2000);
        QVERIFY(dead.isNull() || dead->stopped);
        FakeOutput *fresh = rig.current();
        QVERIFY(fresh && fresh != dead);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QCOMPARE(fresh->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

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
        rig.engine->reloadUnusable();
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 1000);
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QCOMPARE(rig.current()->pull(2),
                 (QList<float>{k16384 * 0.5f, -k16384 * 0.5f, k16384 * 0.25f,
                               -k16384 * 0.25f}));

        Rig quiet;
        quiet.engine->setStallMsForTest(kTestStallMs, 60000, 500);
        quiet.engine->preload();
        quiet.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTest::qWait(250);
        QCOMPARE(quiet.engine->outputsReopenedForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(quiet.engine->outputsReopenedForTest(), 1, 2000);
        QCOMPARE(quiet.current()->pull(1), (QList<float>{32767.0f / 32768, 32767.0f / 32768}));
    }

    void whatAnOutputTookBetweenTwoChecksCounts()
    {
        Rig rig;
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 60000);
        rig.engine->preload();
        rig.engine->reloadUnusable();
        QCOMPARE(rig.current()->pull(CueEngine::kProbeMs * 48).size(),
                 CueEngine::kProbeMs * 48 * 2);
        QTest::qWait(3 * kTestStallMs);
        rig.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTest::qWait(3 * kTestStallMs);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);

        Rig again;
        again.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        again.engine->preload();
        again.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTRY_COMPARE_WITH_TIMEOUT(again.engine->outputsReopenedForTest(), 1, 2000);
        QVERIFY(again.current());
        again.current()->pull(CueEngine::kProbeMs * 48 + 1);
        QTest::qWait(3 * kTestStallMs);
        again.engine->play(QStringLiteral("c"), 1.0f, QString());
        QTRY_COMPARE_WITH_TIMEOUT(again.engine->outputsReopenedForTest(), 2, 2000);
        QVERIFY(!again.engine->failedForTest(QByteArrayLiteral("default")));
    }

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
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        rig.reachable = false;
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
        rig.engine->reloadUnusable();
        QTest::qWait(1200);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        QCOMPARE(rig.engine->outputsOpenedForTest(), 1);
        QCOMPARE(rig.engine->voicesForTest(), 1);
        QVERIFY(!rig.engine->failedForTest(QByteArrayLiteral("default")));
    }

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

    void theGateBacksOffWhileItSaysNo()
    {
        Rig rig(true);
        rig.engine->setStallMsForTest(kTestStallMs, 60000, 0);
        rig.engine->setServerLiveMsForTest(1500);
        rig.engine->setGatePollMsForTest(50, 400);
        rig.engine->preload();
        rig.reachable = false;
        rig.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTest::qWait(400);
        const int before = rig.reachableAsked;
        QVERIFY(before > 0);
        QTest::qWait(2000);
        const int whileNo = rig.reachableAsked - before;
        QVERIFY2(whileNo >= 3 && whileNo <= 10, qPrintable(QString::number(whileNo)));
        rig.reachable = true;
        QTest::qWait(700);
        rig.reachable = false;
        const int atNo = rig.reachableAsked;
        QTest::qWait(300);
        QVERIFY2(rig.reachableAsked - atNo >= 2,
                 qPrintable(QString::number(rig.reachableAsked - atNo)));
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
        const int beforeYes = rig.reachableAsked;
        rig.reachable = true;
        QTRY_COMPARE_WITH_TIMEOUT(rig.engine->outputsReopenedForTest(), 1, 4000);
        QVERIFY2(rig.reachableAsked - beforeYes >= 12,
                 qPrintable(QString::number(rig.reachableAsked - beforeYes)));
    }

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
        QTest::qWait(800);
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
        stale.engine->reloadUnusable();
        QVERIFY(!stale.engine->failedForTest(QByteArrayLiteral("default")));
        stale.engine->play(QStringLiteral("a"), 1.0f, QString());
        QTRY_VERIFY_WITH_TIMEOUT(stale.engine->failedForTest(QByteArrayLiteral("default")), 4000);
        while (sinceFirst.elapsed() < 2200)
            QTest::qWait(20);
        QVERIFY2(stale.engine->failedForTest(QByteArrayLiteral("default")),
                 "the first retry timer cleared the second failure");
    }

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

    void aNewOrSlowOutputIsNotCondemned()
    {
        Rig rig;
        rig.engine->preload();
        rig.outputs.insert(QByteArrayLiteral("headset"));
        rig.defaultOutput = QByteArrayLiteral("headset");
        rig.engine->reloadUnusable();
        QCOMPARE(rig.engine->outputIdsForTest(), QList<QByteArray>{QByteArrayLiteral("headset")});
        QCOMPARE(rig.current()->pull(1), QList<float>{});

        rig.engine->reloadUnusable();
        QTest::qWait(1500);
        QCOMPARE(rig.current()->pull(CueEngine::kProbeMs * 48).size(),
                 CueEngine::kProbeMs * 48 * 2);
        QTest::qWait(1500);
        QCOMPARE(rig.engine->outputsReopenedForTest(), 0);
    }

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

    void aHealthyOutputIsNeverReopened()
    {
        Rig rig;
        rig.engine->setStallMsForTest(300, 60000, 0);
        rig.engine->preload();
        QPointer<FakeOutput> out = rig.current();
        rig.engine->reloadUnusable();
        QCOMPARE(out->pull(CueEngine::kProbeMs * 48).size(), CueEngine::kProbeMs * 48 * 2);
        QCOMPARE(out->pull(1), QList<float>{});
        rig.engine->loop(QStringLiteral("b"), 1.0f, QString());
        const auto take = [&out] {
            if (out)
                out->pull(1);
        };
        for (int i = 0; i < 40; ++i) {
            take();
            QTest::qWait(25);
        }
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
