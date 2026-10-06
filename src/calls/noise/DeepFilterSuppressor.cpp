#include "DeepFilterSuppressor.h"

#ifdef HAVE_DEEPFILTERNET
#include "matrix_denoise.h"
#endif

#include <new>

namespace calls::noise {

namespace {
std::atomic<int> g_live{0};
} // namespace

DeepFilterSuppressor::DeepFilterSuppressor(float attenLimitDb)
{
    g_live.fetch_add(1, std::memory_order_relaxed);
#ifdef HAVE_DEEPFILTERNET
    m_handle = mx_df_create(attenLimitDb);
    if (m_handle) {
        m_frameSize = mx_df_frame_size(m_handle);
        m_latency = mx_df_latency_samples(m_handle);
        // The element frames for exactly 480; a model with another hop (a
        // future vendored model) must not be handed frames of the wrong size.
        if (m_frameSize != kFrameSize || m_latency < 0) {
            mx_df_destroy(m_handle);
            m_handle = nullptr;
            m_frameSize = kFrameSize;
            m_latency = 0;
        }
    }
#else
    (void)attenLimitDb;
#endif
}

DeepFilterSuppressor::~DeepFilterSuppressor()
{
#ifdef HAVE_DEEPFILTERNET
    mx_df_destroy(m_handle);
#endif
    m_handle = nullptr;
    g_live.fetch_sub(1, std::memory_order_relaxed);
}

void DeepFilterSuppressor::process(float *frame) noexcept
{
    if (!frame || !m_handle)
        return; // pass-through
#ifdef HAVE_DEEPFILTERNET
    const int rc = mx_df_process(m_handle, frame, static_cast<size_t>(m_frameSize));
    m_lastStatus.store(rc, std::memory_order_relaxed);
    if (rc == MX_DF_RECOVERED) {
        m_recovered.fetch_add(1, std::memory_order_relaxed);
    } else if (rc < 0) {
        // The Rust side left the frame untouched: this frame passes through.
        // MX_DF_ERR_PANIC poisons the handle; a reset() clears it.
        m_failed.fetch_add(1, std::memory_order_relaxed);
        if (rc == MX_DF_ERR_PANIC)
            m_poisoned.store(true, std::memory_order_relaxed);
    }
#endif
}

void DeepFilterSuppressor::reset() noexcept
{
    if (!m_handle)
        return;
#ifdef HAVE_DEEPFILTERNET
    if (mx_df_reset(m_handle) == MX_DF_OK)
        m_poisoned.store(false, std::memory_order_relaxed);
#endif
}

int DeepFilterSuppressor::liveInstances()
{
    return g_live.load(std::memory_order_relaxed);
}

std::unique_ptr<NoiseSuppressor> createDeepFilterSuppressor()
{
#ifdef HAVE_DEEPFILTERNET
    if (!mx_df_available())
        return nullptr;
    std::unique_ptr<DeepFilterSuppressor> s(new (std::nothrow) DeepFilterSuppressor());
    if (!s || !s->ok())
        return nullptr;
    return s;
#else
    return nullptr;
#endif
}

bool deepFilterAvailable()
{
#ifdef HAVE_DEEPFILTERNET
    return mx_df_available() != 0;
#else
    return false;
#endif
}

} // namespace calls::noise
