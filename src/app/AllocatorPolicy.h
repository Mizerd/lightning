#pragma once

#include <cstdlib> // defines __GLIBC__

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
inline bool applyAllocatorPolicy()
{
#if defined(__GLIBC__)
    constexpr int kMmapThresholdBytes = 128 * 1024;
    constexpr int kTrimThresholdBytes = 2 * 1024 * 1024;
    return mallopt(M_MMAP_THRESHOLD, kMmapThresholdBytes) == 1
        && mallopt(M_TRIM_THRESHOLD, kTrimThresholdBytes) == 1;
#else
    return false;
#endif
}

} // namespace lightning
