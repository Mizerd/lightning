#include "RnnoiseSuppressor.h"

#include <atomic>
#include <cmath>
#include <cstring>

#ifdef HAVE_RNNOISE
extern "C" {
#include "rnnoise.h"
}
#endif

namespace calls::noise {

namespace {

std::atomic<int> g_liveInstances{0};

#ifdef HAVE_RNNOISE
// RNNoise works on 16-bit-range floats; Lightning's pipeline is +-1.0 float.
constexpr float kToRnnoise = 32768.0f;
constexpr float kFromRnnoise = 1.0f / 32768.0f;
constexpr float kMaxInput = 8.0f;
constexpr float kDenormalFloor = 1.0e-20f;
#endif

}

#ifdef HAVE_RNNOISE

RnnoiseSuppressor::RnnoiseSuppressor()
{
    std::memset(m_in, 0, sizeof(m_in));
    std::memset(m_out, 0, sizeof(m_out));
    if (rnnoise_get_frame_size() != kFrameSize)
        return;
    m_state = rnnoise_create(nullptr);
    if (m_state)
        g_liveInstances.fetch_add(1, std::memory_order_relaxed);
}

RnnoiseSuppressor::~RnnoiseSuppressor()
{
    if (m_state) {
        rnnoise_destroy(m_state);
        m_state = nullptr;
        g_liveInstances.fetch_sub(1, std::memory_order_relaxed);
    }
}

void RnnoiseSuppressor::process(float *frame) noexcept
{
    if (!m_state || !frame)
        return;

    for (int i = 0; i < kFrameSize; ++i) {
        float v = frame[i];
        if (!std::isfinite(v))
            v = 0.0f;
        else if (v > kMaxInput)
            v = kMaxInput;
        else if (v < -kMaxInput)
            v = -kMaxInput;
        else if (std::fabs(v) < kDenormalFloor)
            v = 0.0f;
        m_in[i] = v * kToRnnoise;
    }

    const float vad = rnnoise_process_frame(m_state, m_out, m_in);
    m_lastVad.store(std::isfinite(vad) ? vad : 0.0f, std::memory_order_relaxed);

    for (int i = 0; i < kFrameSize; ++i) {
        float v = m_out[i] * kFromRnnoise;
        if (!std::isfinite(v))
            v = 0.0f;
        else if (v > 1.0f)
            v = 1.0f;
        else if (v < -1.0f)
            v = -1.0f;
        frame[i] = v;
    }
}

void RnnoiseSuppressor::reset() noexcept
{
    if (!m_state)
        return;
    if (rnnoise_init(m_state, nullptr) != 0) {
        rnnoise_destroy(m_state);
        m_state = nullptr;
        g_liveInstances.fetch_sub(1, std::memory_order_relaxed);
    }
    m_lastVad.store(0.0f, std::memory_order_relaxed);
}

std::unique_ptr<NoiseSuppressor> createRnnoiseSuppressor()
{
    auto s = std::make_unique<RnnoiseSuppressor>();
    if (!s->ok())
        return nullptr;
    return s;
}

bool rnnoiseAvailable()
{
    return true;
}

#else

RnnoiseSuppressor::RnnoiseSuppressor() = default;
RnnoiseSuppressor::~RnnoiseSuppressor() = default;
void RnnoiseSuppressor::process(float *) noexcept {}
void RnnoiseSuppressor::reset() noexcept {}

std::unique_ptr<NoiseSuppressor> createRnnoiseSuppressor()
{
    return nullptr;
}

bool rnnoiseAvailable()
{
    return false;
}

#endif

int RnnoiseSuppressor::liveInstances()
{
    return g_liveInstances.load(std::memory_order_relaxed);
}

}
