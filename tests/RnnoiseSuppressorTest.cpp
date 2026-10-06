// calls::noise::RnnoiseSuppressor against the vendored RNNoise (third_party/rnnoise).
//
// Everything here is deterministic: own seeded PRNG, synthetic voice (harmonic
// stack under formant-like resonances, gated into syllables), white and pink
// noise. The margins below come from a standalone run of the same signals
// (see third_party/rnnoise/PROVENANCE.md): measured 32-40 dB of suppression in
// the gaps with <= 0.8 dB loss in the voiced parts; asserted at 20 dB and 3 dB.
//
// These cannot pass on the tree before this change: the class does not exist.

#include "RnnoiseSuppressor.h"
#include "RnnoiseTestSignals.h"

#include <QtTest>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

using calls::noise::Mode;
using calls::noise::NoiseSuppressor;
using calls::noise::RnnoiseSuppressor;
using namespace rnnoise_test;

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#define RNNOISE_TEST_COUNT_ALLOCS 1
// Count heap allocations process-wide so the real-time claim ("process() never
// allocates") is asserted, not assumed.
static std::atomic<long> g_allocs{0};
void *operator new(std::size_t n)
{
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t n)
{
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

std::vector<float> runThrough(NoiseSuppressor &s, std::vector<float> x)
{
    const int fs = s.frameSize();
    for (size_t i = 0; i + size_t(fs) <= x.size(); i += size_t(fs))
        s.process(&x[i]);
    return x;
}

std::vector<float> mix(const std::vector<float> &a, const std::vector<float> &b)
{
    std::vector<float> m(a.size());
    for (size_t i = 0; i < a.size(); ++i)
        m[i] = a[i] + b[i];
    return m;
}

} // namespace

class RnnoiseSuppressorTest : public QObject
{
    Q_OBJECT

private slots:
    void availabilityMatchesTheBuild()
    {
        auto s = calls::noise::createRnnoiseSuppressor();
        if (calls::noise::rnnoiseAvailable()) {
            QVERIFY(s);
            QVERIFY(s->ok());
        } else {
            QVERIFY2(!s, "built without RNNoise: the factory must report unavailable, not hand out a pass-through");
        }
    }

    void contract()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        auto s = calls::noise::createRnnoiseSuppressor();
        QVERIFY(s);
        QCOMPARE(s->mode(), Mode::RNNoise);
        QCOMPARE(s->frameSize(), 480);
        QCOMPARE(s->latencySamples(), 960);
        QVERIFY(s->ok());
    }

    void suppressesWhiteAndPinkNoiseInTheGaps_data()
    {
        QTest::addColumn<int>("kind"); // 0 white, 1 pink
        QTest::addColumn<double>("noiseRms");
        QTest::newRow("white -30 dBFS") << 0 << 0.03;
        QTest::newRow("white -40 dBFS") << 0 << 0.01;
        QTest::newRow("pink -30 dBFS") << 1 << 0.03;
        QTest::newRow("pink -40 dBFS") << 1 << 0.01;
    }
    void suppressesWhiteAndPinkNoiseInTheGaps()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        QFETCH(int, kind);
        QFETCH(double, noiseRms);

        const int n = 10 * kRate;
        const int warm = 2 * kRate; // let the network settle
        const Voice v = syntheticVoice(n, 7, 0.10); // -20 dBFS syllables
        const auto noise = kind == 0 ? whiteNoise(n, 11, noiseRms) : pinkNoise(n, 13, noiseRms);
        const auto noisy = mix(v.samples, noise);

        RnnoiseSuppressor s;
        QVERIFY(s.ok());
        const auto out = runThrough(s, noisy);

        const double gapIn = energy(noisy, v.gap, warm, n);
        const double gapOut = energy(out, v.gap, warm, n);
        const double voicedIn = energy(noisy, v.on, warm, n);
        const double voicedOut = energy(out, v.on, warm, n);
        QVERIFY(gapIn > 0 && voicedIn > 0);

        const double suppressionDb = db(gapIn / (gapOut + 1e-30));
        const double voicedDeltaDb = db(voicedOut / voicedIn);
        qInfo().noquote() << QStringLiteral("gap suppression %1 dB, voiced delta %2 dB, vad %3")
                                 .arg(suppressionDb, 0, 'f', 1)
                                 .arg(voicedDeltaDb, 0, 'f', 1)
                                 .arg(double(s.lastVoiceProbability()), 0, 'f', 2);
        QVERIFY2(suppressionDb >= 20.0, qPrintable(QStringLiteral("only %1 dB").arg(suppressionDb)));
        // The speech-like segments survive: not gated away, not boosted.
        QVERIFY2(voicedDeltaDb > -3.0 && voicedDeltaDb < 1.0,
                 qPrintable(QStringLiteral("voiced energy moved by %1 dB").arg(voicedDeltaDb)));
    }

    void notAPassThrough()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int n = 3 * kRate;
        const auto noisy = mix(syntheticVoice(n, 3, 0.10).samples, whiteNoise(n, 5, 0.03));
        RnnoiseSuppressor s;
        const auto out = runThrough(s, noisy);
        QVERIFY(std::memcmp(out.data(), noisy.data(), noisy.size() * sizeof(float)) != 0);
        double diff = 0, ref = 0;
        for (int i = kRate; i < n; ++i) {
            diff += double(out[i] - noisy[i]) * (out[i] - noisy[i]);
            ref += double(noisy[i]) * noisy[i];
        }
        QVERIFY2(diff > 0.05 * ref, "output is within -13 dB of the input everywhere: that is not noise suppression");
    }

    void cleanSpeechSurvivesAlmostUntouched()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int n = 8 * kRate;
        const int warm = 2 * kRate;
        const Voice v = syntheticVoice(n, 9, 0.10);
        auto tiny = whiteNoise(n, 5, 0.0005); // -66 dBFS, essentially clean
        RnnoiseSuppressor s;
        const auto out = runThrough(s, mix(v.samples, tiny));
        const double delta = db(energy(out, v.on, warm, n) / energy(v.samples, v.on, warm, n));
        QVERIFY2(std::fabs(delta) < 3.0, qPrintable(QStringLiteral("voiced energy moved by %1 dB").arg(delta)));
    }

    void measuredLatencyMatchesTheDeclaredOne()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int n = 6 * kRate;
        const int warm = 2 * kRate;
        auto clean = mix(syntheticVoice(n, 7, 0.10).samples, whiteNoise(n, 5, 0.0005));
        RnnoiseSuppressor s;
        const auto out = runThrough(s, clean);
        double best = -1;
        int bestLag = -1;
        for (int lag = 0; lag <= 1500; ++lag) {
            double acc = 0;
            for (int i = warm; i < n - 1600; i += 2)
                acc += double(out[i + lag]) * clean[i];
            if (acc > best) {
                best = acc;
                bestLag = lag;
            }
        }
        QVERIFY2(std::abs(bestLag - s.latencySamples()) <= 8,
                 qPrintable(QStringLiteral("measured %1 samples, declared %2").arg(bestLag).arg(s.latencySamples())));
    }

    void hostileInputNeverCrashesOrPropagates()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        RnnoiseSuppressor s;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const float denorm = std::numeric_limits<float>::denorm_min();
        const float big = std::numeric_limits<float>::max();
        const struct { const char *name; float v; } cases[] = {
            {"NaN", nan}, {"+Inf", inf}, {"-Inf", -inf}, {"denormal", denorm},
            {"FLT_MAX", big}, {"-FLT_MAX", -big}, {"full scale +", 1.0f}, {"full scale -", -1.0f},
            {"4x full scale", 4.0f}, {"zero", 0.0f}};
        for (const auto &c : cases) {
            // Several frames each: a poisoned recurrent state would surface later.
            for (int rep = 0; rep < 6; ++rep) {
                std::vector<float> f(480, c.v);
                s.process(f.data());
                for (float x : f)
                    QVERIFY2(std::isfinite(x) && std::fabs(x) <= 1.0f, c.name);
            }
        }
        // Mixed garbage, then ordinary audio: the engine must recover.
        std::vector<float> g(480);
        for (int i = 0; i < 480; ++i)
            g[i] = (i % 3 == 0) ? nan : (i % 3 == 1 ? inf : -big);
        s.process(g.data());
        const auto n = whiteNoise(2 * kRate, 21, 0.02);
        const auto out = runThrough(s, n);
        for (float x : out)
            QVERIFY(std::isfinite(x));
        QVERIFY(s.ok());
    }

    void nullFrameIsIgnored()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        RnnoiseSuppressor s;
        s.process(nullptr);
        QVERIFY(s.ok());
    }

    void resetRestoresAFreshState()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int n = 2 * kRate;
        const auto in = mix(syntheticVoice(n, 7, 0.10).samples, whiteNoise(n, 11, 0.03));
        RnnoiseSuppressor fresh;
        const auto reference = runThrough(fresh, in);

        RnnoiseSuppressor used;
        runThrough(used, whiteNoise(n, 99, 0.2)); // dirty the state
        used.reset();
        QVERIFY(used.ok());
        const auto again = runThrough(used, in);
        QVERIFY2(std::memcmp(reference.data(), again.data(), reference.size() * sizeof(float)) == 0,
                 "reset() did not return the engine to its initial state");
    }

    void deterministic()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int n = 2 * kRate;
        const auto in = mix(syntheticVoice(n, 7, 0.10).samples, pinkNoise(n, 13, 0.02));
        RnnoiseSuppressor a, b;
        const auto oa = runThrough(a, in);
        const auto ob = runThrough(b, in);
        QVERIFY(std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) == 0);
    }

    void instancesAreIndependent()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        // Interleaving two streams frame by frame must equal running each alone.
        const int n = kRate;
        const auto x = mix(syntheticVoice(n, 7, 0.10).samples, whiteNoise(n, 11, 0.03));
        const auto y = whiteNoise(n, 77, 0.05);
        RnnoiseSuppressor alone;
        const auto ref = runThrough(alone, x);
        RnnoiseSuppressor a, b;
        auto xa = x, yb = y;
        for (int i = 0; i + kFrame <= n; i += kFrame) {
            a.process(&xa[i]);
            b.process(&yb[i]);
        }
        QVERIFY(std::memcmp(ref.data(), xa.data(), ref.size() * sizeof(float)) == 0);
    }

    void processNeverAllocates()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
#ifndef RNNOISE_TEST_COUNT_ALLOCS
        QSKIP("allocation counting is disabled under sanitizers");
#else
        RnnoiseSuppressor s;
        auto in = whiteNoise(kRate, 3, 0.02);
        const long before = g_allocs.load();
        for (int rep = 0; rep < 2; ++rep)
            for (int i = 0; i + kFrame <= int(in.size()); i += kFrame)
                s.process(&in[i]);
        s.reset();
        const long after = g_allocs.load();
        QCOMPARE(after - before, 0L);
#endif
    }

    void createDestroyDoesNotLeak()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int base = RnnoiseSuppressor::liveInstances();
        for (int i = 0; i < 1000; ++i) {
            auto s = calls::noise::createRnnoiseSuppressor();
            QVERIFY(s && s->ok());
            std::vector<float> f(480, 0.01f);
            s->process(f.data());
            if (i % 7 == 0)
                s->reset();
            if (i == 500)
                QCOMPARE(RnnoiseSuppressor::liveInstances(), base + 1);
        }
        QCOMPARE(RnnoiseSuppressor::liveInstances(), base);
    }

    void manyLiveInstancesThenAllFreed()
    {
        if (!calls::noise::rnnoiseAvailable())
            QSKIP("built without RNNoise");
        const int base = RnnoiseSuppressor::liveInstances();
        std::vector<std::unique_ptr<NoiseSuppressor>> v;
        for (int i = 0; i < 64; ++i)
            v.push_back(calls::noise::createRnnoiseSuppressor());
        QCOMPARE(RnnoiseSuppressor::liveInstances(), base + 64);
        v.clear();
        QCOMPARE(RnnoiseSuppressor::liveInstances(), base);
    }
};

QTEST_APPLESS_MAIN(RnnoiseSuppressorTest)
#include "RnnoiseSuppressorTest.moc"
