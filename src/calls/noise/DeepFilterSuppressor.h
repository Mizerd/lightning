#pragma once

#include "NoiseSuppressor.h"

#include <atomic>
#include <cstdint>
#include <memory>

struct MxDfHandle; // rust/include/matrix_denoise.h, opaque

namespace calls::noise {

/**
 * DeepFilterNet (DeepFilterNet3, libDF vendored in third_party/deepfilternet)
 * behind the NoiseSuppressor contract, through the C ABI of
 * rust/src/denoise.rs (rust/include/matrix_denoise.h).
 *
 * - mono float32, 48 kHz, frames of exactly frameSize() == 480 samples (10 ms)
 * - adds latencySamples() == 1440 (30 ms): 480 samples of STFT overlap plus
 *   two frames of model look-ahead, on top of the caller's framing
 * - construction parses and optimises the embedded model (a few hundred ms):
 *   call it on a control thread, never the streaming or GUI thread
 * - process() takes no locks, does no I/O and does not log. It DOES allocate:
 *   the tract inference engine creates its intermediate tensors per frame
 *   (measured ~1.5k-2.5k allocations / ~0.7 MB per frame, all freed before it
 *   returns), which cannot be removed without changing tract. Measured cost
 *   on an i9-10900K: mean ~1 ms, p99 ~2 ms per 10 ms frame, one core.
 * - process() sanitises non-finite input (Rust side) and leaves the frame
 *   untouched on any error, so a failure degrades to pass-through
 * - reset() is REAL-TIME SAFE: it swaps in a fresh model state the Rust side
 *   keeps ready (no allocation, no deallocation, no lock; measured 0.4 us
 *   mean), so the element may call it on the streaming thread, e.g. on the
 *   valve's DISCONT after unmute, and no pre-reset audio (look-ahead,
 *   recurrent state, STFT overlap) is replayed. The next process() re-prepares
 *   the spare state (~+0.3 ms that frame). Must not race process()
 * - the model is compiled into the binary; nothing is loaded at runtime
 */
class DeepFilterSuppressor final : public NoiseSuppressor
{
public:
    /// `attenLimitDb`: how far noise may be attenuated, in dB; 100 or more
    /// means unlimited (upstream's default).
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

    /// Frames the Rust side refused or failed (left unprocessed), and frames
    /// it had to silence after a non-finite model output. Any thread;
    /// diagnostics only.
    std::uint64_t failedFrames() const { return m_failed.load(std::memory_order_relaxed); }
    std::uint64_t recoveredFrames() const { return m_recovered.load(std::memory_order_relaxed); }
    /// The most recent mx_df_process() code (MX_DF_* in matrix_denoise.h).
    int lastStatus() const { return m_lastStatus.load(std::memory_order_relaxed); }

    /// Live instance count, for leak tests.
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

/// nullptr when DeepFilterNet is not compiled in (no Rust backend, or
/// LIGHTNING_ENABLE_DEEPFILTERNET=OFF) or the model failed to initialise.
std::unique_ptr<NoiseSuppressor> createDeepFilterSuppressor();

/// True when this build carries DeepFilterNet.
bool deepFilterAvailable();

} // namespace calls::noise
