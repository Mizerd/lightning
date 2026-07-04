#include "spaces/SpaceManager.h"

#include "matrix/MatrixClient.h"

SpaceManager::SpaceManager(QObject *parent)
    : QAbstractListModel(parent)
{
}

void SpaceManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomsChanged,
                this, &SpaceManager::rebuild);
        connect(m_client, &MatrixClient::roomUpdated,
                this, &SpaceManager::rebuild);
        connect(m_client, &MatrixClient::loggedOut,
                this, &SpaceManager::rebuild);
    }
    rebuild();
}

int SpaceManager::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    // Rows: [All rooms] + [orphans if any] + real spaces.
    int extra = 1; // All rooms is always present.
    if (!m_orphanRoomIds.isEmpty() && !m_spaces.isEmpty()) {
        extra += 1;
    }
    return extra + m_spaces.size();
}

QVariant SpaceManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return {};
    const int row = index.row();

    // Virtual row 0 — "All rooms".
    if (row == 0) {
        switch (role) {
        case SpaceIdRole:      return allRoomsId();
        case NameRole:         return tr("All rooms");
        case TopicRole:        return QString{};
        case AvatarUrlRole:    return QString{};
        case ChildCountRole:   return m_allRoomIds.size();
        case UnreadTotalRole:  return 0;
        }
        return {};
    }

    // Optional orphans row when there is at least one Space and rooms exist
    // outside all Spaces.
    int cursor = 1;
    const bool hasOrphansRow = !m_orphanRoomIds.isEmpty() && !m_spaces.isEmpty();
    if (hasOrphansRow) {
        if (row == cursor) {
            switch (role) {
            case SpaceIdRole:     return orphansId();
            case NameRole:        return tr("Other rooms");
            case TopicRole:       return tr("Rooms not in any Space");
            case AvatarUrlRole:   return QString{};
            case ChildCountRole:  return m_orphanRoomIds.size();
            case UnreadTotalRole: return 0;
            }
            return {};
        }
        cursor += 1;
    }

    const int spaceIndex = row - cursor;
    if (spaceIndex < 0 || spaceIndex >= m_spaces.size())
        return {};
    const auto &s = m_spaces.at(spaceIndex);
    switch (role) {
    case SpaceIdRole:     return s.info.id;
    case NameRole:        return s.info.name;
    case TopicRole:       return s.info.topic;
    case AvatarUrlRole:   return s.info.avatarUrl;
    case ChildCountRole:  return s.childRoomIds.size();
    case UnreadTotalRole: return s.unreadTotal;
    }
    return {};
}

QHash<int, QByteArray> SpaceManager::roleNames() const
{
    return {
        { SpaceIdRole,     "spaceId" },
        { NameRole,        "name" },
        { TopicRole,       "topic" },
        { AvatarUrlRole,   "avatarUrl" },
        { ChildCountRole,  "childCount" },
        { UnreadTotalRole, "unreadTotal" },
    };
}

void SpaceManager::setActiveSpaceId(const QString &spaceId)
{
    if (m_activeSpaceId == spaceId)
        return;
    m_activeSpaceId = spaceId;
    Q_EMIT activeSpaceIdChanged();
}

QStringList SpaceManager::roomsInSpace(const QString &spaceId) const
{
    if (spaceId == allRoomsId())
        return QStringList(m_allRoomIds.constBegin(), m_allRoomIds.constEnd());
    if (spaceId == orphansId())
        return QStringList(m_orphanRoomIds.constBegin(), m_orphanRoomIds.constEnd());
    const auto it = m_membership.constFind(spaceId);
    if (it == m_membership.constEnd())
        return {};
    return QStringList(it->constBegin(), it->constEnd());
}

bool SpaceManager::includesRoom(const QString &spaceId, const QString &roomId) const
{
    if (spaceId == allRoomsId())
        return m_allRoomIds.contains(roomId);
    if (spaceId == orphansId())
        return m_orphanRoomIds.contains(roomId);
    const auto it = m_membership.constFind(spaceId);
    if (it == m_membership.constEnd())
        return false;
    return it->contains(roomId);
}

void SpaceManager::rebuild()
{
    beginResetModel();
    m_spaces.clear();
    m_membership.clear();
    m_allRoomIds.clear();
    m_orphanRoomIds.clear();

    if (!m_client) {
        endResetModel();
        Q_EMIT spacesChanged();
        return;
    }

    const auto rooms = m_client->rooms();
    QHash<QString, RoomInfo> byId;
    byId.reserve(rooms.size());
    for (const auto &r : rooms) {
        byId.insert(r.id, r);
        if (!r.isSpace)
            m_allRoomIds.insert(r.id);
    }

    for (const auto &r : rooms) {
        if (!r.isSpace) continue;
        SpaceEntry e;
        e.info = r;
        for (const auto &child : r.childRoomIds) {
            // Only surface children that the client actually has as a
            // joined non-space room — pending invites / space-of-spaces are
            // deferred to a later pass.
            const auto it = byId.constFind(child);
            if (it == byId.constEnd()) continue;
            if (it->isSpace) continue;
            e.childRoomIds.append(child);
            e.unreadTotal += it->unreadCount;
            m_membership[r.id].insert(child);
        }
        m_spaces.append(std::move(e));
    }

    recomputeOrphans();

    endResetModel();
    Q_EMIT spacesChanged();
}

void SpaceManager::recomputeOrphans()
{
    m_orphanRoomIds.clear();
    QSet<QString> covered;
    for (const auto &s : m_spaces) {
        for (const auto &r : s.childRoomIds)
            covered.insert(r);
    }
    for (const auto &r : m_allRoomIds) {
        if (!covered.contains(r))
            m_orphanRoomIds.insert(r);
    }
}
