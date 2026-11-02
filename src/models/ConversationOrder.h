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

inline bool moreRecent(const QDateTime &aWhen, const QString &aName,
                       const QString &aId, const QDateTime &bWhen,
                       const QString &bName, const QString &bId)
{
    if (aWhen != bWhen) {
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

enum SortMode {
    SortByActivity = 0,
    SortByName = 1,
    SortBySpaceOrder = 2,
    SortByUnread = 3,
};

inline int normalizedSortMode(int mode)
{
    return mode == SortByName || mode == SortBySpaceOrder
                   || mode == SortByUnread
               ? mode
               : SortByActivity;
}

enum RailSort {
    RailSortManual = 0,
    RailSortName = 1,
    RailSortActivity = 2,
};

inline int normalizedRailSort(int mode)
{
    return mode == RailSortName || mode == RailSortActivity ? mode
                                                            : RailSortManual;
}

inline QCollator makeNameCollator()
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

inline bool byName(const QCollator &collator, const QString &aName,
                   const QString &aId, const QString &bName,
                   const QString &bId)
{
    const int byCollation = collator.compare(aName, bName);
    if (byCollation != 0)
        return byCollation < 0;
    return aId < bId;
}

class RecencyHold
{
public:
    static constexpr int kOpenSkewSecs = 120;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on)
    {
        if (m_enabled == on)
            return;
        m_enabled = on;
        m_held.clear();
    }

    void release() { m_held.clear(); }

    void retainOnly(const QSet<QString> &liveIds)
    {
        for (auto it = m_held.begin(); it != m_held.end();) {
            if (liveIds.contains(it.key()))
                ++it;
            else
                it = m_held.erase(it);
        }
    }

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

    QDateTime target(const QString &id, const QDateTime &live) const
    {
        if (!m_openId.isEmpty() && id == m_openId && live.isValid()
            && (!m_openBaseline.isValid() || live > m_openBaseline)
            && live < m_openedAt.addSecs(-kOpenSkewSecs)) {
            return m_openBaseline;
        }
        return live;
    }

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

}
