#pragma once

#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace matrix::rust_timeline {

inline constexpr int kBackgroundMirrorCap = 60;
inline constexpr qsizetype kLargeMemberList = 200;
inline constexpr int kLargeMemberListsKept = 3;

// Recency-ordered (newest first) bookkeeping for per-room member lists. Small
// rooms are never evicted; of the large ones only the newest few stay, plus any
// protected (open) room. Returns the rooms whose lists should be dropped.
inline QStringList memberListVictims(QStringList &recent, const QString &touched,
                                     const QHash<QString, qsizetype> &counts,
                                     const QSet<QString> &protectedRooms,
                                     int keep = kLargeMemberListsKept,
                                     qsizetype threshold = kLargeMemberList)
{
    recent.removeAll(touched);
    recent.prepend(touched);
    QStringList victims;
    int kept = 0;
    for (auto it = recent.begin(); it != recent.end();) {
        const qsizetype n = counts.value(*it, 0);
        if (n < threshold) {
            ++it;
            continue;
        }
        if (protectedRooms.contains(*it) || kept < keep) {
            ++kept;
            ++it;
            continue;
        }
        victims.append(*it);
        it = recent.erase(it);
    }
    return victims;
}

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
