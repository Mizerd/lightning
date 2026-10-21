#include "RoomHistory.h"

namespace lightning {

void RoomHistory::visit(const QString &roomId)
{
    if (roomId.isEmpty()) {
        if (!m_entries.isEmpty())
            m_detached = true;
        trim();
        return;
    }
    // Coming back to the room the cursor is on (after the lobby, or the same
    // room re-selected) is not a new navigation: the forward list stays.
    if (m_cursor >= 0 && m_cursor < m_entries.size()
        && m_entries.at(m_cursor) == roomId) {
        m_detached = false;
        return;
    }
    // A new navigation drops everything ahead of the cursor.
    while (m_entries.size() > m_cursor + 1)
        m_entries.removeLast();
    m_entries.append(roomId);
    m_cursor = m_entries.size() - 1;
    m_detached = false;
    trim();
}

void RoomHistory::prune(const Openable &openable)
{
    if (!openable)
        return;
    QStringList kept;
    int keptUpToCursor = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString &room = m_entries.at(i);
        // The open room is never pruned; a detached cursor entry may be.
        const bool protectedEntry = !m_detached && i == m_cursor;
        if (!protectedEntry && !openable(room))
            continue;
        // Removing a room can leave the same one twice in a row.
        if (!kept.isEmpty() && kept.last() == room)
            continue;
        kept.append(room);
        if (i <= m_cursor)
            ++keptUpToCursor;
    }
    m_entries = kept;
    m_cursor = keptUpToCursor - 1;
    if (m_entries.isEmpty())
        m_detached = false;
    // A cursor of -1 is only valid while detached.
    if (m_cursor < 0 && !m_entries.isEmpty())
        m_detached = true;
    trim();
}

void RoomHistory::trim()
{
    // Back steps available: the entries before the cursor, plus the cursor's
    // own while detached.
    while (!m_entries.isEmpty()
           && m_cursor + (m_detached ? 1 : 0) > kMaxBackEntries) {
        m_entries.removeFirst();
        --m_cursor;
    }
}

QString RoomHistory::back(const Openable &openable)
{
    prune(openable);
    const int target = backTarget();
    if (target < 0 || target >= m_entries.size())
        return {};
    m_cursor = target;
    m_detached = false;
    return m_entries.at(target);
}

QString RoomHistory::forward(const Openable &openable)
{
    prune(openable);
    const int target = forwardTarget();
    if (target < 0 || target >= m_entries.size())
        return {};
    m_cursor = target;
    m_detached = false;
    return m_entries.at(target);
}

bool RoomHistory::canGoBack(const Openable &openable) const
{
    RoomHistory copy(*this);
    copy.prune(openable);
    const int target = copy.backTarget();
    return target >= 0 && target < copy.m_entries.size();
}

bool RoomHistory::canGoForward(const Openable &openable) const
{
    RoomHistory copy(*this);
    copy.prune(openable);
    const int target = copy.forwardTarget();
    return target >= 0 && target < copy.m_entries.size();
}

void RoomHistory::clear()
{
    m_entries.clear();
    m_cursor = -1;
    m_detached = false;
}

} // namespace lightning
