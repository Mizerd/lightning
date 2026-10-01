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

#include <QCollator>
#include <QDateTime>
#include <QHash>
#include <QSet>
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

/// How a conversation list is ordered inside each of its groups. The groups
/// themselves (invitations, favourites, People, Rooms, ...) never change with
/// it. Stored as an int (SettingsManager::roomListSort), so a value this build
/// does not know must land on the default: normalizedSortMode(), never a clamp.
enum SortMode {
    SortByActivity = 0,
    SortByName = 1,
};

/// An unknown value (hand-edited, or written by a newer build with a third
/// mode) is Activity, not the nearest mode: modes have no magnitude.
inline int normalizedSortMode(int mode)
{
    return mode == SortByName ? SortByName : SortByActivity;
}

/// The collator A-Z compares with: the user's locale, case-insensitive. Built
/// once per sorting pass by the caller (it is not free with ICU), and only in
/// A-Z mode.
inline QCollator makeNameCollator()
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

/// Strict-weak ordering: display name, locale-aware and case-insensitive. The
/// unique id breaks a tie (two rooms with one name, or names a collator
/// considers equal), so the order is total and does not shuffle between syncs.
/// Activity plays no part, which is why a held order is moot in this mode.
inline bool byName(const QCollator &collator, const QString &aName,
                   const QString &aId, const QString &bName,
                   const QString &bId)
{
    const int byCollation = collator.compare(aName, bName);
    if (byCollation != 0)
        return byCollation < 0;
    return aId < bId;
}

/// The sort key a list actually orders by, which is not always the live one.
///
/// Two separate reasons to keep a room where it is, both decided here so the
/// Classic list and the Channels column cannot disagree:
///
///  1. HOLD (a setting, off until a model is told otherwise). While on, a room
///     keeps the key it was last ordered by, so a new message moves nothing
///     under the pointer. Rooms that have no key yet (new ones) enter by their
///     live key; an entry that was never active upgrades to its first real
///     stamp, because it had no position worth keeping. release() drops every
///     key, and the next ordering is the live one. Only the ORDER is held:
///     callers keep reading live unread counts, previews and names.
///
///  2. THE OPEN ROOM. The SDK can resend a stale stamp for a room until it is
///     opened (see harvest_room_activity in rust/src/lib.rs), so opening a room
///     could send it to the top for a message that was already old. A stamp
///     that raises the open room's key but predates the moment it was opened
///     (less a clock-skew allowance) is ignored for ordering until the room is
///     no longer the open one. A stamp from a message sent after the open
///     passes untouched. Applies with the hold off too.
///
/// Header-only and Qt Core only, like the comparator above.
class RecencyHold
{
public:
    /// How far behind the open moment a stamp may be and still count as a
    /// message sent since (the server's clock is not ours).
    static constexpr int kOpenSkewSecs = 120;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on)
    {
        if (m_enabled == on)
            return;
        m_enabled = on;
        m_held.clear();
    }

    /// Drop every held key; the next ordering is the live one.
    void release() { m_held.clear(); }

    /// Forget keys of rooms the client no longer lists, so a room that leaves
    /// and returns is ordered afresh.
    void retainOnly(const QSet<QString> &liveIds)
    {
        for (auto it = m_held.begin(); it != m_held.end();) {
            if (liveIds.contains(it.key()))
                ++it;
            else
                it = m_held.erase(it);
        }
    }

    /// `liveNow`: the room's live stamp at the moment it is opened. An empty
    /// id clears the guard.
    void setOpenRoom(const QString &roomId, const QDateTime &liveNow,
                     const QDateTime &now = QDateTime::currentDateTimeUtc())
    {
        if (roomId == m_openId)
            return;
        m_openId = roomId;
        m_openBaseline = roomId.isEmpty() ? QDateTime() : liveNow;
        m_openedAt = now;
    }
    QString openRoomId() const { return m_openId; }

    /// Where the room WOULD sort if nothing were held: the live stamp, less
    /// the open-room guard. Pure.
    QDateTime target(const QString &id, const QDateTime &live) const
    {
        if (!m_openId.isEmpty() && id == m_openId && live.isValid()
            && (!m_openBaseline.isValid() || live > m_openBaseline)
            && live < m_openedAt.addSecs(-kOpenSkewSecs)) {
            return m_openBaseline;
        }
        return live;
    }

    /// The stamp the room is ordered by now. Records it when held.
    QDateTime keyFor(const QString &id, const QDateTime &live)
    {
        const QDateTime wanted = target(id, live);
        if (!m_enabled)
            return wanted;
        const auto it = m_held.find(id);
        if (it == m_held.end()) {
            m_held.insert(id, wanted);
            return wanted;
        }
        if (!it.value().isValid() && wanted.isValid())
            it.value() = wanted;
        return it.value();
    }

private:
    bool m_enabled = false;
    QHash<QString, QDateTime> m_held;
    QString m_openId;
    QDateTime m_openBaseline;
    QDateTime m_openedAt;
};

} // namespace conversation
