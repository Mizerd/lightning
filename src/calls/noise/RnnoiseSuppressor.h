#pragma once

#include "NoiseSuppressor.h"

#include <atomic>
#include <memory>

struct DenoiseState; // RNNoise (third_party/rnnoise), opaque

namespace calls::noise {

/**
 * RNNoise (xiph/rnnoise v0.2, vendored in third_party/rnnoise) behind the
 * NoiseSuppressor contract.
 *
 * - mono float32, 48 kHz, frames of exactly frameSize() == 480 samples (10 ms)
 * - callers pass samples in [-1, 1]; RNNoise wants the 16-bit range, so the
 *   scaling by 32768 happens here, and the output is scaled back and clamped
 * - process() never allocates, locks or logs, and sanitises non-finite input
 * - construction allocates (control thread); reset() re-initialises the state
 *   IN PLACE (no allocation) and must not race process()
 * - the model is the one compiled into the binary; nothing is loaded at runtime
 */
class RnnoiseSuppressor final : public NoiseSuppressor
{
public:
    RnnoiseSuppressor();
    ~RnnoiseSuppressor() override;
    RnnoiseSuppressor(const RnnoiseSuppressor &) = delete;
    RnnoiseSuppressor &operator=(const RnnoiseSuppressor &) = delete;

    Mode mode() const override { return Mode::RNNoise; }
    int frameSize() const override { return kFrameSize; }
    bool ok() const override { return m_state != nullptr; }
    void process(float *frame) noexcept override;
    int latencySamples() const override { return kLatencySamples; }
    void reset() noexcept override;
    /// rnnoise_init() over the existing state: no allocation, no lock.
    bool resetIsRealtimeSafe() const override { return true; }

    /// Voice-activity probability RNNoise computed for the most recent frame
    /// (0..1). Safe to read from another thread (diagnostics only).
    float lastVoiceProbability() const { return m_lastVad.load(std::memory_order_relaxed); }

    /// Live instance count, for leak tests.
    static int liveInstances();

    /// Samples per frame RNNoise works in (10 ms at 48 kHz).
    static constexpr int kFrameSize = 480;
    /// Algorithmic delay RNNoise adds, in samples at 48 kHz (measured, see
    /// third_party/rnnoise/PROVENANCE.md and the unit test).
    static constexpr int kLatencySamples = 960; // 20 ms: one frame of overlap-add + one frame of look-ahead (denoise.c delayed_X)

private:
    DenoiseState *m_state = nullptr;
    alignas(16) float m_in[kFrameSize];
    alignas(16) float m_out[kFrameSize];
    std::atomic<float> m_lastVad{0.0f};
};

/// nullptr when RNNoise is not compiled in (LIGHTNING_ENABLE_RNNOISE=OFF) or
/// the state could not be created.
std::unique_ptr<NoiseSuppressor> createRnnoiseSuppressor();

/// True when this build carries RNNoise.
bool rnnoiseAvailable();

} // namespace calls::noise
