#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>

// The BACKGROUND timeline mirror's bound.
//
// Every live event of a room the user has not opened is kept in
// RustSdkMatrixClient's per-room mirror, and nothing but sign-out ever
// emptied it: a day in a busy account is tens of thousands of TimelineEvents,
// each carrying its body, its formatted body and its metadata, and each new
// event was compared against all of them by a linear scan before being
// appended.
//
// A ring is enough for the job that mirror actually does. It is the
// room-list preview source and the pre-snapshot content TimelineModel reloads
// when a room opens — and the open replaces it wholesale a moment later with
// the SDK timeline snapshot. Three backward-pagination batches (the Rust
// side's PAGINATION_BATCH is 20) is more than either use needs, and the cap
// also bounds the de-duplication scan to a constant.
//
// Header-only and pure so the bound is testable without a Rust handle.
namespace matrix::rust_timeline {

inline constexpr int kBackgroundMirrorCap = 60;

// Append `event`, keeping at most `cap` rows and dropping the OLDEST first,
// so the mirror always holds the NEWEST events — which is what a preview and
// a pre-snapshot render both want.
inline void appendBounded(QList<TimelineEvent> &mirror,
                          const TimelineEvent &event,
                          int cap = kBackgroundMirrorCap)
{
    if (cap <= 0)
        return;
    while (mirror.size() >= cap)
        mirror.removeFirst();
    mirror.append(event);
}

} // namespace matrix::rust_timeline
