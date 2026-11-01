#pragma once

#include <cstdlib> // defines __GLIBC__
#include <mutex>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace lightning {

// Give large buffers back to the system when they are freed.
//
// glibc serves a block of 128 KiB or more with its own mmap, which munmap
// returns at free. But freeing one RAISES that threshold to the block's size
// (up to 32 MiB), so from the first decoded image or media payload on, those
// sizes come out of the heap, where one small live block above them pins the
// whole range. Measured on a 500-room account after opening 30 rooms: about
// 1015 MB resident with the adaptive threshold, 945 MB with it fixed, for
// eight times the page faults (1.5 s of system time over that walk).
//
// Setting either value turns the adaptation off, so this pins the threshold
// at its documented default, and keeps a little slack at the top of a heap so
// a burst does not trim and regrow on every free.
//
// The fixed threshold has one price: a block above it costs a mapping and its
// page faults on every malloc, and a call allocates one 1080p frame per
// received frame. So a live call lifts the thresholds above any frame and the
// last one ending puts them back. CallAllocatorHold is the only writer.
inline constexpr int kIdleMmapThresholdBytes = 128 * 1024;
inline constexpr int kCallMmapThresholdBytes = 32 * 1024 * 1024; // glibc's ceiling
inline constexpr int kIdleTrimThresholdBytes = 2 * 1024 * 1024;
// Above a frame too: a smaller one gives each freed frame back to the system
// and faults it in again, which costs as much as the mapping did.
inline constexpr int kCallTrimThresholdBytes = 64 * 1024 * 1024;

inline bool applyAllocatorPolicy()
{
#if defined(__GLIBC__)
    return mallopt(M_MMAP_THRESHOLD, kIdleMmapThresholdBytes) == 1
        && mallopt(M_TRIM_THRESHOLD, kIdleTrimThresholdBytes) == 1;
#else
    return false;
#endif
}

// Held by a call's media engine for as long as it carries a call. Engines
// overlap across a transfer, so the threshold follows the count.
class CallAllocatorHold
{
public:
    CallAllocatorHold() = default;
    CallAllocatorHold(const CallAllocatorHold &) = delete;
    CallAllocatorHold &operator=(const CallAllocatorHold &) = delete;
    ~CallAllocatorHold() { release(); }

    void engage()
    {
        if (m_held)
            return;
        m_held = true;
        adjust(+1);
    }
    void release()
    {
        if (!m_held)
            return;
        m_held = false;
        adjust(-1);
    }
    static int activeCalls()
    {
        const std::lock_guard<std::mutex> lock(mutex());
        return count();
    }

private:
    static std::mutex &mutex()
    {
        static std::mutex m;
        return m;
    }
    static int &count()
    {
        static int n = 0;
        return n;
    }
    static void adjust(int delta)
    {
        const std::lock_guard<std::mutex> lock(mutex());
        const int before = count();
        count() = before + delta;
#if defined(__GLIBC__)
        if (before == 0 && count() == 1) {
            mallopt(M_MMAP_THRESHOLD, kCallMmapThresholdBytes);
            mallopt(M_TRIM_THRESHOLD, kCallTrimThresholdBytes);
        } else if (before == 1 && count() == 0) {
            mallopt(M_MMAP_THRESHOLD, kIdleMmapThresholdBytes);
            mallopt(M_TRIM_THRESHOLD, kIdleTrimThresholdBytes);
            // The frames the call left in the heap go back now, not at some
            // later free.
            malloc_trim(0);
        }
#endif
    }

    bool m_held = false;
};

} // namespace lightning
