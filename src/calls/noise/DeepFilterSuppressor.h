#pragma once

#include "NoiseSuppressor.h"

#include <atomic>
#include <cstdint>
#include <memory>

struct MxDfHandle;

namespace calls::noise {

class DeepFilterSuppressor final : public NoiseSuppressor
{
public:
    explicit DeepFilterSuppressor(float attenLimitDb = kDefaultAttenLimitDb);
    ~DeepFilterSuppressor() override;
    DeepFilterSuppressor(const DeepFilterSuppressor &) = delete;
    DeepFilterSuppressor &operator=(const DeepFilterSuppressor &) = delete;

    Mode mode() const override { return Mode::DeepFilterNet; }
    int frameSize() const override { return m_frameSize; }
    bool ok() const override { return m_handle != nullptr; }
    void process(float *frame) noexcept override;
    int latencySamples() const override { return m_latency; }
    void reset() noexcept override;
    bool resetIsRealtimeSafe() const override { return true; }
    /// A panic inside the model poisoned the handle: every later frame passes
    /// through untouched.
    bool failed() const noexcept override
    {
        return m_poisoned.load(std::memory_order_relaxed);
    }

    std::uint64_t failedFrames() const { return m_failed.load(std::memory_order_relaxed); }
    std::uint64_t recoveredFrames() const { return m_recovered.load(std::memory_order_relaxed); }
    int lastStatus() const { return m_lastStatus.load(std::memory_order_relaxed); }

    static int liveInstances();

    static constexpr int kFrameSize = 480;
    static constexpr int kExpectedLatencySamples = 1440;
    static constexpr float kDefaultAttenLimitDb = 100.0f;

private:
    MxDfHandle *m_handle = nullptr;
    int m_frameSize = kFrameSize;
    int m_latency = 0;
    std::atomic<std::uint64_t> m_failed{0};
    std::atomic<std::uint64_t> m_recovered{0};
    std::atomic<int> m_lastStatus{0};
    std::atomic<bool> m_poisoned{false};
};

std::unique_ptr<NoiseSuppressor> createDeepFilterSuppressor();

bool deepFilterAvailable();

}
