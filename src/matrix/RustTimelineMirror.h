#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>

namespace matrix::rust_timeline {

inline constexpr int kBackgroundMirrorCap = 60;

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

inline qsizetype trimToBackgroundBound(QList<TimelineEvent> &mirror,
                                       int cap = kBackgroundMirrorCap)
{
    const qsizetype keep = cap > 0 ? cap : 0;
    const qsizetype excess = mirror.size() - keep;
    if (excess <= 0)
        return 0;
    mirror.remove(0, excess);
    return excess;
}

}
