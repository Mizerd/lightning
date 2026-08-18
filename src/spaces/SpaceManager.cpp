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
                this, [this] {
            m_pendingChildAdds.clear(); // account isolation
            m_pendingChildRemovals.clear();
            rebuild();
        });
        connect(m_client, &MatrixClient::spaceChildFinished,
                this, [this](quint64 opId, const QString &spaceId,
                             const QString &roomId, bool ok) {
            // Only ops this manager issued; ConversationController's
            // create-with-placement path reports through its own signal.
            const auto it = m_pendingChildAdds.constFind(opId);
            if (it == m_pendingChildAdds.constEnd())
                return;
            m_pendingChildAdds.erase(it);
            Q_EMIT childAddFinished(spaceId, roomId, ok);
        });
        connect(m_client, &MatrixClient::spaceChildRemoveFinished,
                this, [this](quint64 opId, const QString &spaceId,
                             const QString &roomId, bool ok) {
            const auto it = m_pendingChildRemovals.constFind(opId);
            if (it == m_pendingChildRemovals.constEnd())
                return;
            m_pendingChildRemovals.erase(it);
            Q_EMIT childRemoveFinished(spaceId, roomId, ok);
        });
    }
    m_pendingChildAdds.clear();
    m_pendingChildRemovals.clear();
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
        case UnreadTotalRole:  return m_homeUnreadTotal;
        case HighlightTotalRole: return m_homeHighlightTotal;
        case LevelRole: return 0;
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
            case HighlightTotalRole: return 0;
            case LevelRole: return 0;
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
    case HighlightTotalRole: return s.highlightTotal;
    case LevelRole:       return s.level;
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
        { HighlightTotalRole, "highlightTotal" },
        { LevelRole,       "level" },
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

QString SpaceManager::spaceName(const QString &spaceId) const
{
    if (spaceId == allRoomsId() || spaceId == orphansId())
        return {};
    for (const SpaceEntry &entry : m_spaces) {
        if (entry.info.id == spaceId)
            return entry.info.name;
    }
    return {};
}

QVariantMap SpaceManager::spaceInfo(const QString &spaceId) const
{
    for (const SpaceEntry &entry : m_spaces) {
        if (entry.info.id != spaceId || spaceId.isEmpty())
            continue;
        return QVariantMap{
            { QStringLiteral("roomId"),        entry.info.id },
            { QStringLiteral("name"),          entry.info.name },
            { QStringLiteral("topic"),         entry.info.topic },
            { QStringLiteral("avatarUrl"),     entry.info.avatarUrl },
            { QStringLiteral("childCount"),    entry.childRoomIds.size() },
            { QStringLiteral("unreadTotal"),   entry.unreadTotal },
            { QStringLiteral("highlightTotal"), entry.highlightTotal },
        };
    }
    return {};
}

QVariantList SpaceManager::childRoomsDetailed(const QString &spaceId) const
{
    QVariantList out;
    if (!m_client)
        return out;
    const SpaceEntry *space = nullptr;
    for (const SpaceEntry &entry : m_spaces) {
        if (!spaceId.isEmpty() && entry.info.id == spaceId) {
            space = &entry;
            break;
        }
    }
    if (!space)
        return out;

    QHash<QString, RoomInfo> byId;
    const auto rooms = m_client->rooms();
    for (const RoomInfo &room : rooms)
        byId.insert(room.id, room);

    // Authoritative m.space.child order; children the account has not
    // joined (or whose rooms are unknown locally) are simply absent —
    // never fabricated placeholder rows.
    for (const QString &childId : space->childRoomIds) {
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd() || it->membership != RoomInfo::Joined)
            continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"),        it->id },
            { QStringLiteral("name"),          it->name },
            { QStringLiteral("avatarUrl"),     it->avatarUrl },
            { QStringLiteral("isDirect"),      it->isDirect },
            // Shared fallback-colour policy (see RoomInfo.h).
            { QStringLiteral("identityColorKey"), identityColorKey(*it) },
            { QStringLiteral("hasUnread"),     it->hasUnreadMessages },
            { QStringLiteral("unreadCount"),   it->unreadCount },
            { QStringLiteral("highlightCount"), it->highlightCount },
            { QStringLiteral("lastActivity"),  it->lastActivity },
        });
    }
    return out;
}

QVariantList SpaceManager::childSpacesDetailed(const QString &spaceId) const
{
    QVariantList out;
    if (!m_client || spaceId.isEmpty())
        return out;
    const auto rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (!room.isSpace || room.membership != RoomInfo::Joined)
            continue;
        if (!room.parentSpaceIds.contains(spaceId))
            continue;
        const int childRooms = m_membership.value(room.id).size();
        out.append(QVariantMap{
            { QStringLiteral("roomId"), room.id },
            { QStringLiteral("name"), room.name },
            { QStringLiteral("avatarUrl"), room.avatarUrl },
            { QStringLiteral("identityColorKey"), identityColorKey(room) },
            { QStringLiteral("childCount"), childRooms },
        });
    }
    return out;
}

QVariantList SpaceManager::addableRooms(const QString &spaceId,
                                        const QString &filter) const
{
    QVariantList out;
    if (!m_client || spaceId.isEmpty())
        return out;
    const auto member = m_membership.constFind(spaceId);
    const QString needle = filter.trimmed();
    const auto rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (room.isSpace || room.membership != RoomInfo::Joined)
            continue;
        const bool alreadyChild =
            member != m_membership.constEnd() && member->contains(room.id);
        if (!needle.isEmpty()
            && !room.name.contains(needle, Qt::CaseInsensitive))
            continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"),       room.id },
            { QStringLiteral("name"),         room.name },
            { QStringLiteral("avatarUrl"),    room.avatarUrl },
            { QStringLiteral("isDirect"),     room.isDirect },
            { QStringLiteral("alreadyChild"), alreadyChild },
        });
        if (out.size() >= 50)
            break;
    }
    return out;
}

void SpaceManager::addRoomToSpace(const QString &spaceId,
                                  const QString &roomId)
{
    if (!m_client || spaceId.isEmpty() || roomId.isEmpty())
        return;
    // Duplicate protection: adding an existing child is a no-op success.
    if (includesRoom(spaceId, roomId)) {
        Q_EMIT childAddFinished(spaceId, roomId, true);
        return;
    }
    const quint64 opId = m_client->addRoomToSpace(spaceId, roomId);
    if (opId == 0) {
        Q_EMIT childAddFinished(spaceId, roomId, false);
        return;
    }
    m_pendingChildAdds.insert(opId, { spaceId, roomId });
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
    m_homeUnreadTotal = 0;
    m_homeHighlightTotal = 0;

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
        if (!r.isSpace && r.membership == RoomInfo::Joined) {
            m_allRoomIds.insert(r.id);
            m_homeUnreadTotal += r.unreadCount;
            m_homeHighlightTotal += r.highlightCount;
        }
    }

    // Resolve descendants iteratively with a visited set. Matrix permits
    // multiple parents and malformed state can contain cycles; neither may
    // duplicate rows or recurse forever. The depth cap is a final bound for
    // adversarial graphs, not a lifecycle timing workaround.
    for (const auto &r : rooms) {
        if (!r.isSpace || r.membership != RoomInfo::Joined) continue;
        SpaceEntry e;
        e.info = r;
        e.level = r.parentSpaceIds.isEmpty() ? 0 : 1;
        QList<QPair<QString, int>> pending;
        for (const auto &child : r.childRoomIds) pending.append({child, 1});
        QSet<QString> visited{r.id};
        while (!pending.isEmpty()) {
            const auto [childId, depth] = pending.takeFirst();
            if (depth > 64 || visited.contains(childId)) continue;
            visited.insert(childId);
            const auto it = byId.constFind(childId);
            if (it == byId.constEnd()) continue; // inaccessible/unjoined child
            if (it->isSpace) {
                for (const auto &nested : it->childRoomIds)
                    pending.append({nested, depth + 1});
                continue;
            }
            if (it->membership != RoomInfo::Joined) continue;
            e.childRoomIds.append(childId);
            e.unreadTotal += it->unreadCount;
            e.highlightTotal += it->highlightCount;
            m_membership[r.id].insert(childId);
        }
        m_spaces.append(std::move(e));
    }

    if (!m_activeSpaceId.isEmpty() && !m_membership.contains(m_activeSpaceId)) {
        m_activeSpaceId.clear();
        Q_EMIT activeSpaceIdChanged();
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

void SpaceManager::removeRoomFromSpace(const QString &spaceId,
                                       const QString &roomId)
{
    if (!m_client || spaceId.isEmpty() || roomId.isEmpty())
        return;
    if (!includesRoom(spaceId, roomId)) {
        Q_EMIT childRemoveFinished(spaceId, roomId, true); // already gone
        return;
    }
    const quint64 opId = m_client->removeRoomFromSpace(spaceId, roomId);
    if (opId == 0) {
        Q_EMIT childRemoveFinished(spaceId, roomId, false);
        return;
    }
    m_pendingChildRemovals.insert(opId, { spaceId, roomId });
}
