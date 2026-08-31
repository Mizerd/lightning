#pragma once

// The one recency comparator for conversation lists.
//
// Classic (RoomListModel) and Channels (SpaceChannelModel) both order
// conversations by "when did somebody last actually say something here", and
// they must agree: two lists over the same rooms that disagree about which is
// newer is a bug the user sees as rooms swapping places when they switch
// layout. Both call this, so there is nothing to drift.
//
// WHAT COUNTS AS ACTIVITY IS DECIDED UPSTREAM, not here. RoomInfo::lastActivity
// is written only through raiseActivity(), which is monotonic, and its writers
// exclude state changes, call rows and virtual events; on the Rust side the
// sort key is the SDK's LatestEventValue, which is the room-list preview value
// and so is message-like by construction, including the local echo of a message
// the user has just sent. Reading a room, changing its topic or renaming it
// therefore does not move it. If a room jumps for something nobody said, the
// defect is in a writer of lastActivity, not in this file.
//
// Header-only on purpose: roughly twenty test targets link the models, and a
// new .cpp would mean editing every one of their source lists.

#include <QDateTime>
#include <QString>

namespace conversation {

/// Strict-weak ordering: newest conversation activity first.
///
/// The tiebreak is not decoration. Rooms with no activity yet share an invalid
/// timestamp, and a great many rooms can share a timestamp to the millisecond
/// after a backfill, so without a total order the list reshuffles itself
/// between syncs purely from the order the backend happened to hand rooms
/// over. Name first so a tie reads alphabetically to a human, then id, which
/// is unique.
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
