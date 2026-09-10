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

// Put an OPENED room's mirror back under the background bound, dropping the
// OLDEST rows. Returns how many were dropped, so a caller can log a real
// number rather than an intention.
//
// The other half of the bound above, and it was missing until 2026-09-10.
// appendBounded() holds the ring for a room the user has never opened;
// opening one REPLACES that ring with the SDK timeline snapshot and then
// grows it with every diff — the viewport fill alone reaches 600-900 rows —
// and no transition reduced it again. Closing the room did not, switching
// rooms did not (the generation tracker forgets the old room without
// touching anything keyed by it), and a sign-out's wholesale clear was the
// only thing that ever freed any of it.
//
// PRECISELY WHICH ROOMS LEAK, corrected in review: appendBounded() collapses
// a backgrounded room's mirror to the cap on its very next LIVE EVENT, so a
// busy room re-bounds itself within seconds of being left. What leaks is the
// room that goes QUIET after being opened — which on a real account is most
// of them — and it holds its full opened size for the rest of the session,
// while matrix-sdk's own shrink_to_last_chunk has already released Rust's
// copy. The fix is the same; the defect is narrower than "every room".
//
// Trimming rather than discarding is the point: `cap` rows is exactly what
// the room would hold had it never been opened, so this restores the
// documented invariant instead of inventing a second one, and it keeps the
// instant pre-snapshot render on re-open.
inline qsizetype trimToBackgroundBound(QList<TimelineEvent> &mirror,
                                       int cap = kBackgroundMirrorCap)
{
    const qsizetype keep = cap > 0 ? cap : 0;
    const qsizetype excess = mirror.size() - keep;
    if (excess <= 0)
        return 0;
    // The NEWEST `keep` rows survive — the same end appendBounded() keeps.
    mirror.remove(0, excess);
    return excess;
}

} // namespace matrix::rust_timeline
