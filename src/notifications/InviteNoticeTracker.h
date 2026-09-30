// Which room invites to announce, and when one has really gone.
//
// The room list can report one invite twice. A re-sort (the invite's name
// arriving, say) is a remove plus an insert, and a resync is a clear plus
// the list again: each step is its own roomsChanged, and the room is absent
// in between. Taken literally, that resolves the invite and announces it a
// second time, the first time under the SDK's "Empty Room". So an invite is
// resolved only after it has stayed off the list for a grace period, and a
// new one is announced after a short settle, under the name it has by then.
//
// Pure: the caller passes the time and reads the room when it announces.
#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class InviteNoticeTracker
{
public:
    /// How long a new invite waits before it is announced.
    static constexpr qint64 kSettleMs = 1000;
    /// How long an invite must stay off the list to count as resolved.
    static constexpr qint64 kGoneGraceMs = 3000;

    /// One room-list change: the ids listed as invites now. An id first seen
    /// once initial sync is done is due for announcement after kSettleMs;
    /// one seen before is recorded silently, so a restart never re-announces.
    void observe(const QSet<QString> &invites, bool initialSyncDone,
                 qint64 nowMs);
    /// Invites whose settle has passed and that are listed now. Each id is
    /// returned once per invite.
    QStringList takeDue(qint64 nowMs);
    /// Invites off the list for kGoneGraceMs: forgotten, so inviting again
    /// later is a new invite.
    QStringList takeResolved(qint64 nowMs);
    /// The room is listed as something else now (joined, left): the invite is
    /// settled at once, since a re-sort never changes a membership. True when
    /// it was a known invite.
    bool forget(const QString &id);
    /// Milliseconds until takeDue() or takeResolved() has something, or -1.
    qint64 nextDueInMs(qint64 nowMs) const;
    void clear();

private:
    // Listed now, or off the list for less than the grace.
    QSet<QString> m_known;
    // Known ids currently off the list, and since when.
    QHash<QString, qint64> m_goneSince;
    // Not yet announced, and from when they may be.
    QHash<QString, qint64> m_dueAt;
};
