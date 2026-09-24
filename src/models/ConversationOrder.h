#pragma once

// The recency comparator shared by conversation lists.
//
// Classic (RoomListModel) and Channels (SpaceChannelModel) must order rooms
// identically, or rooms swap places when the layout changes.
//
// What counts as activity is decided upstream: RoomInfo::lastActivity is
// written only through the monotonic raiseActivity(), whose writers exclude
// state changes, call rows and virtual events; on Rust the key is the SDK's
// LatestEventValue (message-like, including local echoes). A room that moves
// for something nobody said is a lastActivity writer bug, not this file's.
//
// Header-only so the many test targets linking the models need no new source.

#include <QDateTime>
#include <QString>

namespace conversation {

/// Strict-weak ordering: newest activity first. The tiebreak (name, then the
/// unique id) gives a total order, so rooms sharing a timestamp (none, or the
/// same millisecond after a backfill) do not reshuffle between syncs.
inline bool moreRecent(const QDateTime &aWhen, const QString &aName,
                       const QString &aId, const QDateTime &bWhen,
                       const QString &bName, const QString &bId)
{
    if (aWhen != bWhen) {
        // An invalid (never-active) timestamp sorts last rather than first,
        // which is what QDateTime's own comparison would do with it.
        if (!aWhen.isValid())
            return false;
        if (!bWhen.isValid())
            return true;
        return aWhen > bWhen;
    }
    const int byName = aName.compare(bName, Qt::CaseInsensitive);
    if (byName != 0)
        return byName < 0;
    return aId < bId;
}

} // namespace conversation
