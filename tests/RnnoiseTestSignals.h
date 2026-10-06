#pragma once

// Deterministic synthetic signals for the RNNoise tests (and for the standalone
// sanity run recorded in third_party/rnnoise/PROVENANCE.md). Header-only, no Qt.
// Own PRNG on purpose: std::normal_distribution is not specified bit-for-bit
// across standard libraries and these numbers must be the same everywhere.

#include <cmath>
#include <cstdint>
#include <vector>

namespace rnnoise_test {

constexpr int kRate = 48000;
constexpr int kFrame = 480;
constexpr double kPi = 3.14159265358979323846;

class Rng
{
public:
    explicit Rng(uint64_t seed) : m_s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next()
    {
        // xorshift64*
        m_s ^= m_s >> 12;
        m_s ^= m_s << 25;
        m_s ^= m_s >> 27;
        return m_s * 0x2545F4914F6CDD1Dull;
    }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    double gauss()
    {
        // Box-Muller
        double u1 = uniform();
        if (u1 < 1e-300)
            u1 = 1e-300;
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    uint64_t m_s;
};

inline std::vector<float> whiteNoise(int n, uint64_t seed, double rms)
{
    Rng r(seed);
    std::vector<float> v(n);
    for (auto &x : v)
        x = float(r.gauss() * rms);
    return v;
}

// Paul Kellet's economy pink filter on white noise, scaled to the given RMS.
inline std::vector<float> pinkNoise(int n, uint64_t seed, double rms)
{
    Rng r(seed);
    double b0 = 0, b1 = 0, b2 = 0;
    std::vector<double> d(n);
    double acc = 0;
    for (int i = 0; i < n; ++i) {
        const double w = r.gauss();
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        d[i] = b0 + b1 + b2 + w * 0.1848;
        acc += d[i] * d[i];
    }
    const double scale = rms / std::sqrt(acc / n);
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = float(d[i] * scale);
    return v;
}

struct Voice
{
    std::vector<float> samples; // voiced signal, gaps are exact zeros
    std::vector<char> on;       // 1 where a syllable is fully on (ramps excluded)
    std::vector<char> gap;      // 1 where it is silent with margin (ramps excluded)
};

// Harmonic stack on a drifting f0 (~110-140 Hz) shaped by three formant-like
// resonances, gated into syllables with raised-cosine edges.
inline Voice syntheticVoice(int n, uint64_t seed, double peakRms)
{
    Rng r(seed);
    Voice v;
    v.samples.assign(n, 0.0f);
    v.on.assign(n, 0);
    v.gap.assign(n, 0);

    const double formant[3] = {700.0, 1220.0, 2600.0};
    const double bw[3] = {130.0, 90.0, 160.0};
    const double gain[3] = {1.0, 0.6, 0.25};

    const int onLen = int(0.32 * kRate);
    const int offLen = int(0.28 * kRate);
    const int ramp = int(0.02 * kRate);
    const int margin = int(0.06 * kRate);

    double phase = 0;
    double acc = 0;
    int cnt = 0;
    std::vector<double> raw(n, 0.0);
    std::vector<double> env(n, 0.0);
    const int period = onLen + offLen;
    for (int i = 0; i < n; ++i) {
        const int pos = i % period;
        double e = 0.0;
        if (pos < onLen) {
            e = 1.0;
            if (pos < ramp)
                e = 0.5 - 0.5 * std::cos(kPi * pos / ramp);
            else if (pos >= onLen - ramp)
                e = 0.5 - 0.5 * std::cos(kPi * (onLen - pos) / ramp);
        }
        env[i] = e;
        if (pos >= ramp && pos < onLen - ramp)
            v.on[i] = 1;
        if (pos >= onLen + margin && pos < period - margin / 2)
            v.gap[i] = 1;
        if (e > 0.0) {
            const double t = double(i) / kRate;
            const double f0 = 125.0 + 15.0 * std::sin(2 * kPi * 3.1 * t) + 6.0 * std::sin(2 * kPi * 0.7 * t);
            phase += 2 * kPi * f0 / kRate;
            if (phase > 2 * kPi)
                phase -= 2 * kPi;
            double s = 0;
            for (int h = 1; h * f0 < 5000.0; ++h) {
                const double f = h * f0;
                double a = 1.0 / h; // glottal tilt
                for (int k = 0; k < 3; ++k) {
                    const double dx = (f - formant[k]) / bw[k];
                    a += gain[k] * std::exp(-0.5 * dx * dx) * 1.0;
                }
                s += a * std::sin(h * phase);
            }
            raw[i] = s * e + 0.01 * r.gauss() * e; // tiny breathiness
            acc += raw[i] * raw[i];
            ++cnt;
        }
    }
    const double rmsOn = cnt ? std::sqrt(acc / cnt) : 1.0;
    const double scale = peakRms / rmsOn;
    for (int i = 0; i < n; ++i)
        v.samples[i] = float(raw[i] * scale);
    return v;
}

inline double energy(const std::vector<float> &x, const std::vector<char> &mask, int from, int to, int shift = 0)
{
    double acc = 0;
    long cnt = 0;
    for (int i = from; i < to; ++i) {
        const int j = i - shift; // mask index for output delayed by `shift`
        if (j < 0 || j >= int(mask.size()) || !mask[j])
            continue;
        acc += double(x[i]) * x[i];
        ++cnt;
    }
    return cnt ? acc / cnt : 0.0;
}

inline double db(double powerRatio)
{
    return 10.0 * std::log10(powerRatio > 1e-30 ? powerRatio : 1e-30);
}

} // namespace rnnoise_test
