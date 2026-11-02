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
    visitPlace({ Place::Room, roomId, {}, {} });
}

void RoomHistory::visitThread(const QString &roomId, const QString &rootEventId)
{
    if (!roomId.isEmpty() && !rootEventId.isEmpty())
        visitPlace({ Place::Thread, roomId, rootEventId, {} });
}

void RoomHistory::visitSearchResult(const QString &roomId, const QString &eventId)
{
    if (!roomId.isEmpty() && !eventId.isEmpty())
        visitPlace({ Place::SearchResult, roomId, {}, eventId });
}

void RoomHistory::visitPlace(const Place &place)
{
    if (m_cursor >= 0 && m_cursor < m_entries.size()
        && m_entries.at(m_cursor) == place) {
        m_detached = false;
        return;
    }
    while (m_entries.size() > m_cursor + 1)
        m_entries.removeLast();
    m_entries.append(place);
    m_cursor = m_entries.size() - 1;
    m_detached = false;
    trim();
}

void RoomHistory::prune(const Openable &openable)
{
    if (!openable)
        return;
    QList<Place> kept;
    int keptUpToCursor = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Place &place = m_entries.at(i);
        const bool protectedEntry = !m_detached && i == m_cursor;
        if (!protectedEntry && !openable(place.roomId))
            continue;
        if (!kept.isEmpty() && kept.last() == place)
            continue;
        kept.append(place);
        if (i <= m_cursor)
            ++keptUpToCursor;
    }
    m_entries = kept;
    m_cursor = keptUpToCursor - 1;
    if (m_entries.isEmpty())
        m_detached = false;
    if (m_cursor < 0 && !m_entries.isEmpty())
        m_detached = true;
    trim();
}

void RoomHistory::trim()
{
    while (!m_entries.isEmpty()
           && m_cursor + (m_detached ? 1 : 0) > kMaxBackEntries) {
        m_entries.removeFirst();
        --m_cursor;
    }
}

QString RoomHistory::back(const Openable &openable)
{
    return backPlace(openable).roomId;
}

RoomHistory::Place RoomHistory::backPlace(const Openable &openable)
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
    return forwardPlace(openable).roomId;
}

RoomHistory::Place RoomHistory::forwardPlace(const Openable &openable)
{
    prune(openable);
    const int target = forwardTarget();
    if (target < 0 || target >= m_entries.size())
        return {};
    m_cursor = target;
    m_detached = false;
    return m_entries.at(target);
}

QStringList RoomHistory::entries() const
{
    QStringList rooms;
    for (const Place &place : m_entries)
        rooms.append(place.roomId);
    return rooms;
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

}
