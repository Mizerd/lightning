#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QQueue>
#include <QSet>
#include <QString>

#include <algorithm>
#include <limits>

namespace matrix::notification_producer {

class BoundedIdMap
{
public:
    explicit BoundedIdMap(int cap) : m_cap(std::max(1, cap)) {}

    void insert(const QString &id, bool flag)
    {
        if (id.isEmpty())
            return;
        auto it = m_flags.find(id);
        if (it != m_flags.end()) {
            *it = *it || flag;
            return;
        }
        while (m_order.size() >= m_cap) {
            m_flags.remove(m_order.dequeue());
        }
        m_order.enqueue(id);
        m_flags.insert(id, flag);
    }

    bool take(const QString &id, bool *flag = nullptr)
    {
        auto it = m_flags.find(id);
        if (it == m_flags.end())
            return false;
        if (flag)
            *flag = *it;
        m_flags.erase(it);
        m_order.removeOne(id);
        return true;
    }

    bool contains(const QString &id) const { return m_flags.contains(id); }
    int size() const { return static_cast<int>(m_flags.size()); }
    void clear()
    {
        m_flags.clear();
        m_order.clear();
    }

private:
    int m_cap;
    QHash<QString, bool> m_flags;
    QQueue<QString> m_order;
};

// Finding 121: under simplified sliding sync a room without an open timeline
// receives at most its newest event per response (the list's timeline_limit
// is 1), so a mention followed by any other message in the same response is
// never offered to the notification path. The room's highlight count still
// rises. This compares the rise against the highlighted events actually
// delivered for the room and reports the difference.
//
// Counts and events reach C++ in either order, so a rise is held for
// `windowMs` before it is judged, and events delivered within `lookbackMs`
// before it count as well. A rise the client cannot judge reports nothing:
// the room's first count (no baseline), a count seen before the initial sync
// finished, or a room where history (a backlog event) arrived in the window.
class HighlightGapTracker
{
public:
    static constexpr qint64 kWindowMs = 2000;
    static constexpr qint64 kLookbackMs = 15000;
    static constexpr int kMaxRooms = 20000;

    void creditHighlight(const QString &roomId, qint64 nowMs)
    {
        if (roomId.isEmpty())
            return;
        Room &room = m_rooms[roomId];
        room.credits.append(nowMs);
        prune(room, nowMs);
    }

    void noteBacklog(const QString &roomId, qint64 nowMs)
    {
        if (roomId.isEmpty())
            return;
        m_rooms[roomId].backlogAtMs = nowMs;
    }

    void observeCount(const QString &roomId, int count, qint64 nowMs, bool live)
    {
        if (roomId.isEmpty() || count < 0)
            return;
        if (m_rooms.size() >= kMaxRooms && !m_rooms.contains(roomId))
            m_rooms.clear();
        Room &room = m_rooms[roomId];
        if (!room.hasBaseline || !live) {
            room.hasBaseline = true;
            room.baseline = count;
            room.pendingRise = 0;
            room.dueMs = 0;
            return;
        }
        if (count < room.baseline) {
            room.baseline = count;
            room.pendingRise = 0;
            room.dueMs = 0;
            room.credits.clear();
            return;
        }
        if (count == room.baseline)
            return;
        room.pendingRise += count - room.baseline;
        room.baseline = count;
        if (room.dueMs == 0) {
            room.dueMs = nowMs + kWindowMs;
            room.riseAtMs = nowMs;
        }
    }

    QList<QPair<QString, int>> takeDue(qint64 nowMs)
    {
        QList<QPair<QString, int>> out;
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
            Room &room = it.value();
            if (room.dueMs == 0 || room.dueMs > nowMs)
                continue;
            prune(room, nowMs);
            const qint64 from = room.riseAtMs - kLookbackMs;
            int credited = 0;
            for (qint64 at : std::as_const(room.credits)) {
                if (at >= from)
                    ++credited;
            }
            const bool history = room.backlogAtMs != 0
                && room.backlogAtMs >= from;
            const int owed = room.pendingRise - credited;
            if (!history && owed > 0)
                out.append({ it.key(), owed });
            room.credits.clear();
            room.pendingRise = 0;
            room.dueMs = 0;
        }
        return out;
    }

    qint64 nextDueMs() const
    {
        qint64 next = std::numeric_limits<qint64>::max();
        for (const Room &room : m_rooms) {
            if (room.dueMs != 0)
                next = std::min(next, room.dueMs);
        }
        return next == std::numeric_limits<qint64>::max() ? 0 : next;
    }

    void forgetRoom(const QString &roomId) { m_rooms.remove(roomId); }
    void forgetRoomsNotIn(const QSet<QString> &present)
    {
        for (auto it = m_rooms.begin(); it != m_rooms.end();) {
            if (present.contains(it.key()))
                ++it;
            else
                it = m_rooms.erase(it);
        }
    }
    void reset() { m_rooms.clear(); }

private:
    struct Room {
        bool hasBaseline = false;
        int baseline = 0;
        int pendingRise = 0;
        qint64 dueMs = 0;
        qint64 riseAtMs = 0;
        qint64 backlogAtMs = 0;
        QList<qint64> credits;
    };

    static void prune(Room &room, qint64 nowMs)
    {
        const qint64 oldest = nowMs - kLookbackMs - kWindowMs;
        room.credits.removeIf([oldest](qint64 at) { return at < oldest; });
    }

    QHash<QString, Room> m_rooms;
};

}
