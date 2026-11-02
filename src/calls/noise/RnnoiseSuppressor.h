#pragma once

#include "NoiseSuppressor.h"

#include <atomic>
#include <memory>

struct DenoiseState;

namespace calls::noise {

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

    float lastVoiceProbability() const { return m_lastVad.load(std::memory_order_relaxed); }

    static int liveInstances();

    static constexpr int kFrameSize = 480;
    static constexpr int kLatencySamples = 960;

private:
    DenoiseState *m_state = nullptr;
    alignas(16) float m_in[kFrameSize];
    alignas(16) float m_out[kFrameSize];
    std::atomic<float> m_lastVad{0.0f};
};

std::unique_ptr<NoiseSuppressor> createRnnoiseSuppressor();

bool rnnoiseAvailable();

}
