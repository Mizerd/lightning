// DeepFilterSuppressor: the C++ side of the DeepFilterNet backend (#20).
// Exercises the real model through rust/src/denoise.rs when the build carries
// it (HAVE_DEEPFILTERNET), and the pass-through stub when it does not.

#include "DeepFilterSuppressor.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

using calls::noise::DeepFilterSuppressor;
using calls::noise::Mode;
using calls::noise::NoiseSuppressor;

namespace {

constexpr int kRate = 48000;
constexpr int kHop = 480;
constexpr float kPi = 3.14159265358979f;

// Deterministic "fan" noise: xorshift hiss + low rumble + 120 Hz hum.
std::vector<float> fanNoise(int samples, std::uint64_t seed)
{
    std::vector<float> out(static_cast<size_t>(samples));
    std::uint64_t x = seed;
    float lp = 0.0f;
    for (int i = 0; i < samples; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const float w = static_cast<float>(x >> 40) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
        lp = 0.9f * lp + 0.1f * w;
        const float t = static_cast<float>(i) / kRate;
        out[static_cast<size_t>(i)] = 0.03f * w + 0.1f * lp
            + 0.01f * std::sin(2.0f * kPi * 120.0f * t);
    }
    return out;
}

double energy(const float *x, size_t n)
{
    double e = 0.0;
    for (size_t i = 0; i < n; ++i)
        e += double(x[i]) * double(x[i]);
    return n ? e / double(n) : 0.0;
}

std::vector<float> runAll(NoiseSuppressor &s, const std::vector<float> &in)
{
    std::vector<float> out = in;
    for (size_t off = 0; off + kHop <= out.size(); off += kHop)
        s.process(out.data() + off);
    return out;
}

bool allFinite(const std::vector<float> &v)
{
    for (float f : v) {
        if (!std::isfinite(f))
            return false;
    }
    return true;
}

} // namespace

class DeepFilterSuppressorTest : public QObject
{
    Q_OBJECT

private slots:
    void availabilityMatchesTheBuild()
    {
#ifdef HAVE_DEEPFILTERNET
        QVERIFY(calls::noise::deepFilterAvailable());
        auto s = calls::noise::createDeepFilterSuppressor();
        QVERIFY(s);
        QVERIFY(s->ok());
        QCOMPARE(s->mode(), Mode::DeepFilterNet);
#else
        QVERIFY(!calls::noise::deepFilterAvailable());
        QVERIFY(!calls::noise::createDeepFilterSuppressor());
#endif
    }

    void stubIsPassThroughWhenNotBuilt()
    {
#ifdef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet is built in; the stub is not");
#else
        DeepFilterSuppressor s;
        QVERIFY(!s.ok());
        QCOMPARE(s.frameSize(), kHop);
        std::vector<float> frame(kHop, 0.25f);
        s.process(frame.data());
        for (float f : frame)
            QCOMPARE(f, 0.25f);
        s.reset();
#endif
    }

    void frameSizeAndLatency()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        DeepFilterSuppressor s;
        QVERIFY(s.ok());
        QCOMPARE(s.frameSize(), DeepFilterSuppressor::kFrameSize);
        QCOMPARE(s.frameSize(), kHop);
        // DeepFilterNet3: 480 samples of STFT overlap + 2 frames lookahead.
        QCOMPARE(s.latencySamples(), DeepFilterSuppressor::kExpectedLatencySamples);
#endif
    }

    void noiseIsActuallyRemoved()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        // Two seconds of steady fan noise: the model must drive the floor
        // far down once it has seen it. Pass-through would be 0 dB.
        DeepFilterSuppressor s;
        QVERIFY(s.ok());
        const auto in = fanNoise(2 * kRate, 42);
        const auto out = runAll(s, in);
        QVERIFY(allFinite(out));
        const size_t from = size_t(kRate) / 2; // after warm-up
        const size_t n = in.size() - from - size_t(s.latencySamples());
        const double reduction = 10.0
            * std::log10(energy(in.data() + from, n)
                         / std::max(energy(out.data() + from + s.latencySamples(), n), 1e-30));
        QVERIFY2(reduction > 20.0, qPrintable(QStringLiteral("noise reduced by %1 dB").arg(reduction)));
        QCOMPARE(s.failedFrames(), std::uint64_t(0));
        QCOMPARE(s.lastStatus(), 0);
#endif
    }

    void attenuationLimitBoundsTheReduction()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        DeepFilterSuppressor s(12.0f);
        QVERIFY(s.ok());
        const auto in = fanNoise(2 * kRate, 7);
        const auto out = runAll(s, in);
        const size_t from = size_t(kRate) / 2;
        const size_t n = in.size() - from - size_t(s.latencySamples());
        const double reduction = 10.0
            * std::log10(energy(in.data() + from, n)
                         / std::max(energy(out.data() + from + s.latencySamples(), n), 1e-30));
        QVERIFY2(reduction > 10.0 && reduction < 14.0,
                 qPrintable(QStringLiteral("12 dB limit gave %1 dB").arg(reduction)));
#endif
    }

    void malformedInputCannotBreakIt()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        DeepFilterSuppressor s;
        QVERIFY(s.ok());
        s.process(nullptr); // ignored, no crash
        const std::array<float, 6> specials = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            1e30f,
            -1e30f,
            std::numeric_limits<float>::denorm_min(),
        };
        std::vector<float> frame(kHop);
        for (int i = 0; i < 60; ++i) {
            for (int j = 0; j < kHop; ++j)
                frame[size_t(j)] = specials[size_t(i + j) % specials.size()];
            s.process(frame.data());
            QVERIFY(allFinite(frame));
        }
        for (int i = 0; i < 60; ++i) { // full-scale square wave
            for (int j = 0; j < kHop; ++j)
                frame[size_t(j)] = ((i + j) % 2) ? 1.0f : -1.0f;
            s.process(frame.data());
            QVERIFY(allFinite(frame));
        }
        QCOMPARE(s.failedFrames(), std::uint64_t(0));
        // The running state is not poisoned: it still removes noise.
        const auto in = fanNoise(2 * kRate, 3);
        const auto out = runAll(s, in);
        QVERIFY(allFinite(out));
        const size_t from = size_t(kRate);
        const size_t n = in.size() - from - size_t(s.latencySamples());
        QVERIFY(energy(out.data() + from + s.latencySamples(), n) < 0.1 * energy(in.data() + from, n));
#endif
    }

    void resetMakesProcessingReproducible()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        DeepFilterSuppressor s;
        QVERIFY(s.ok());
        const auto in = fanNoise(kRate / 2, 11);
        const auto first = runAll(s, in);
        s.reset();
        const auto second = runAll(s, in);
        QVERIFY(first == second);
#endif
    }

    void resetDropsPreResetAudioLikeAFreshInstance()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        // The unmute case: loud audio, then the element resets on the valve's
        // DISCONT. Nothing of the earlier audio may come out afterwards, so
        // the output must equal an instance that never heard it. The reset
        // runs on the streaming thread, so it must declare itself RT-safe.
        DeepFilterSuppressor s;
        QVERIFY(s.ok());
        QVERIFY(s.resetIsRealtimeSafe());
        std::vector<float> tone(size_t(kRate) / 2);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.8f * std::sin(2.0f * kPi * 440.0f * float(i) / kRate);
        const auto after = fanNoise(kRate / 2, 31);
        DeepFilterSuppressor fresh;
        QVERIFY(fresh.ok());
        const auto expected = runAll(fresh, after);
        for (int round = 0; round < 2; ++round) {
            (void)runAll(s, tone);
            s.reset();
            QVERIFY(runAll(s, after) == expected);
        }
        QCOMPARE(s.failedFrames(), std::uint64_t(0));
#endif
    }

    void instancesAreIndependentAcrossThreads()
    {
#ifndef HAVE_DEEPFILTERNET
        QSKIP("DeepFilterNet not built in");
#else
        // Two calls' microphones, or a mic test beside a call: two handles
        // processed concurrently must give exactly what each gives alone.
        DeepFilterSuppressor ref;
        QVERIFY(ref.ok());
        const auto inA = fanNoise(kRate / 2, 21);
        const auto expectA = runAll(ref, inA);
        DeepFilterSuppressor a;
        DeepFilterSuppressor b;
        QVERIFY(a.ok() && b.ok());
        const auto inB = fanNoise(kRate / 2, 22);
        std::vector<float> outA;
        std::vector<float> outB;
        std::thread ta([&] { outA = runAll(a, inA); });
        std::thread tb([&] { outB = runAll(b, inB); });
        ta.join();
        tb.join();
        QVERIFY(outA == expectA);
        QVERIFY(allFinite(outB));
#endif
    }

    void createDestroyLoopReleasesInstances()
    {
        const int before = DeepFilterSuppressor::liveInstances();
        for (int i = 0; i < 5; ++i) {
            auto s = calls::noise::createDeepFilterSuppressor();
#ifdef HAVE_DEEPFILTERNET
            QVERIFY(s);
            std::vector<float> frame(kHop, 0.1f);
            s->process(frame.data());
            s->reset();
#else
            QVERIFY(!s);
#endif
        }
        QCOMPARE(DeepFilterSuppressor::liveInstances(), before);
    }
};

QTEST_GUILESS_MAIN(DeepFilterSuppressorTest)
#include "DeepFilterSuppressorTest.moc"
