#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>

// The background timeline mirror's bound.
//
// RustSdkMatrixClient keeps every live event of an unopened room in a
// per-room mirror, which otherwise grows without limit and is de-duplicated
// by a linear scan. That mirror only feeds the room-list preview and the
// pre-snapshot render when a room opens (replaced by the SDK snapshot a moment
// later), so a ring of three pagination batches (PAGINATION_BATCH is 20)
// suffices and bounds the scan.
//
// Header-only and pure so the bound is testable without a Rust handle.
namespace matrix::rust_timeline {

inline constexpr int kBackgroundMirrorCap = 60;

// Append `event`, keeping at most `cap` rows and dropping the oldest, so the
// mirror holds the newest events.
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

// Put an opened room's mirror back under the background bound, dropping the
// oldest rows. Returns how many were dropped.
//
// Opening a room replaces the ring with the SDK snapshot and grows it with
// every diff (the viewport fill alone reaches hundreds of rows), and neither
// closing nor switching rooms shrank it. A busy room re-bounds on its next
// live event via appendBounded(); a room that goes quiet would hold its full
// opened size for the session. Trimming (not discarding) restores exactly
// what an unopened room holds and keeps the instant render on re-open.
inline qsizetype trimToBackgroundBound(QList<TimelineEvent> &mirror,
                                       int cap = kBackgroundMirrorCap)
{
    const qsizetype keep = cap > 0 ? cap : 0;
    const qsizetype excess = mirror.size() - keep;
    if (excess <= 0)
        return 0;
    // The newest `keep` rows survive, as in appendBounded().
    mirror.remove(0, excess);
    return excess;
}

} // namespace matrix::rust_timeline
