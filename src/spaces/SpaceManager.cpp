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
            m_pendingChildSuggests.clear();
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
        connect(m_client, &MatrixClient::spaceChildSuggestedFinished,
                this, [this](quint64 opId, const QString &spaceId,
                             const QString &roomId, bool suggested, bool ok) {
            const auto it = m_pendingChildSuggests.constFind(opId);
            if (it == m_pendingChildSuggests.constEnd())
                return;
            m_pendingChildSuggests.erase(it);
            Q_EMIT childSuggestedFinished(spaceId, roomId, suggested, ok);
        });
    }
    m_pendingChildAdds.clear();
    m_pendingChildRemovals.clear();
    m_pendingChildSuggests.clear();
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
        case ParentSpaceIdRole: return QString{};
        case ChildSpaceCountRole: return 0;
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
            case ParentSpaceIdRole: return QString{};
            case ChildSpaceCountRole: return 0;
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
    case ParentSpaceIdRole: return s.parentSpaceId;
    case ChildSpaceCountRole: return int(s.childSpaceIds.size());
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
        { ParentSpaceIdRole, "parentSpaceId" },
        { ChildSpaceCountRole, "childSpaceCount" },
    };
}

void SpaceManager::setActiveSpaceId(const QString &spaceId)
{
    if (m_activeSpaceId == spaceId)
        return;
    m_activeSpaceId = spaceId;
    Q_EMIT activeSpaceIdChanged();
}

QVariantList SpaceManager::allSpaces() const
{
    QVariantList out;
    const QHash<int, QByteArray> names = roleNames();
    for (int row = 0; row < rowCount(); ++row) {
        QVariantMap entry;
        const QModelIndex idx = index(row, 0);
        for (auto it = names.constBegin(); it != names.constEnd(); ++it)
            entry.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
        out.append(entry);
    }
    return out;
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

QStringList SpaceManager::directChildRoomIds(
    const QString &spaceId, const QHash<QString, RoomInfo> &byId) const
{
    QStringList out;
    if (spaceId.isEmpty())
        return out;
    const auto parent = byId.constFind(spaceId);
    if (parent == byId.constEnd() || !parent->isSpace)
        return out;
    // The Space's own state order. Children the account has not joined, and
    // child SPACES (which are categories, not channels), are simply absent —
    // never fabricated placeholder rows. Identical rules to
    // directChildRoomsDetailed; only the room lookup differs.
    QSet<QString> seen;
    for (const QString &childId : parent->childRoomIds) {
        if (seen.contains(childId))
            continue;
        seen.insert(childId);
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd() || it->isSpace
            || it->membership != RoomInfo::Joined) {
            continue;
        }
        out.append(it->id);
    }
    return out;
}

QVariantList SpaceManager::directChildRoomsDetailed(
    const QString &spaceId) const
{
    QVariantList out;
    if (!m_client || spaceId.isEmpty())
        return out;
    const auto rooms = m_client->rooms();
    QHash<QString, RoomInfo> byId;
    byId.reserve(rooms.size());
    for (const RoomInfo &room : rooms)
        byId.insert(room.id, room);

    const auto parent = byId.constFind(spaceId);
    if (parent == byId.constEnd() || !parent->isSpace)
        return out;

    // The Space's own state order. Children the account has not joined, and
    // child SPACES (which are categories, not channels), are simply absent —
    // never fabricated placeholder rows.
    QSet<QString> seen;
    for (const QString &childId : parent->childRoomIds) {
        if (seen.contains(childId))
            continue;
        seen.insert(childId);
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd() || it->isSpace
            || it->membership != RoomInfo::Joined) {
            continue;
        }
        out.append(QVariantMap{
            { QStringLiteral("roomId"),           it->id },
            { QStringLiteral("name"),             it->name },
            { QStringLiteral("avatarUrl"),        it->avatarUrl },
            { QStringLiteral("isDirect"),         it->isDirect },
            // KNOWN encryption only. The channel row draws a lock for this,
            // and a lock on a room whose state has not resolved would claim
            // encryption as a fact — the hash glyph is the honest fallback
            // for "not established yet".
            { QStringLiteral("encrypted"),
              it->encrypted && it->encryptionKnown },
            { QStringLiteral("identityColorKey"), identityColorKey(*it) },
            { QStringLiteral("hasUnread"),        it->hasUnreadMessages },
            { QStringLiteral("unreadCount"),      it->unreadCount },
            { QStringLiteral("highlightCount"),   it->highlightCount },
        });
    }
    return out;
}

QVariantList SpaceManager::childSpacesDetailed(const QString &spaceId) const
{
    QVariantList out;
    if (!m_client || spaceId.isEmpty())
        return out;
    // The resolved hierarchy's own answer, in m.space.child order. It used to
    // scan every room for `parentSpaceIds.contains(spaceId)`, which meant
    // room-list order (not the admin's), and nothing at all on a backend that
    // reports the edge only from the parent side.
    QHash<QString, RoomInfo> byId;
    for (const RoomInfo &room : m_client->rooms())
        byId.insert(room.id, room);
    for (const QString &childId : childSpaceIds(spaceId)) {
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd())
            continue;
        int unread = 0;
        int highlight = 0;
        for (const SpaceEntry &entry : m_spaces) {
            if (entry.info.id != childId)
                continue;
            unread = entry.unreadTotal;
            highlight = entry.highlightTotal;
            break;
        }
        out.append(QVariantMap{
            { QStringLiteral("roomId"), it->id },
            { QStringLiteral("name"), it->name },
            { QStringLiteral("avatarUrl"), it->avatarUrl },
            { QStringLiteral("identityColorKey"), identityColorKey(*it) },
            { QStringLiteral("childCount"),
              int(m_membership.value(childId).size()) },
            { QStringLiteral("unreadTotal"), unread },
            { QStringLiteral("highlightTotal"), highlight },
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

// Assigns every joined Space a real depth and one primary parent.
//
// Three things Matrix permits that a tree does not, and what each one gets:
//
//  * SEVERAL PARENTS. A subspace may be a child of two Spaces. It is nested
//    under exactly ONE of them for display — whichever the breadth-first walk
//    below reaches first — so it appears once, in a place that does not move
//    between syncs. The other parent still contains its rooms transitively;
//    only the nesting is exclusive.
//  * CYCLES. A -> B -> A is legal state and a naive walk never terminates.
//    Every Space is assigned at most once, so a cycle simply stops; anything
//    the walk never reaches (a cycle with no entry point) is treated as a
//    ROOT rather than dropped, because a Space the user has joined must stay
//    reachable in the rail whatever its state says.
//  * PARENT LINKS THE ACCOUNT CANNOT SEE. `parentSpaceIds` may name a Space
//    that is not joined, and on some backends it is not populated at all.
//    Parents are therefore the UNION of the child's own parent list
//    (restricted to joined Spaces) and the inverse of every joined Space's
//    own m.space.child list — so the hierarchy resolves identically whether
//    the backend reports edges from above, below, or both.
//
// Determinism comes from the iteration order: the model's own Space order
// (the backend's) for the roots, and `m.space.child` order within each Space.
void SpaceManager::resolveHierarchy(const QHash<QString, RoomInfo> &byId)
{
    QSet<QString> joinedSpaceIds;
    for (const SpaceEntry &entry : m_spaces)
        joinedSpaceIds.insert(entry.info.id);

    // Direct joined child spaces, in each Space's own m.space.child order.
    QHash<QString, QStringList> childSpacesOf;
    QHash<QString, QSet<QString>> parentsOf;
    for (const SpaceEntry &entry : m_spaces) {
        QStringList children;
        for (const QString &childId : entry.info.childRoomIds) {
            if (!joinedSpaceIds.contains(childId) || childId == entry.info.id)
                continue;
            if (children.contains(childId))
                continue;
            children.append(childId);
            parentsOf[childId].insert(entry.info.id);
        }
        childSpacesOf.insert(entry.info.id, children);
    }
    for (const SpaceEntry &entry : m_spaces) {
        const auto it = byId.constFind(entry.info.id);
        if (it == byId.constEnd())
            continue;
        for (const QString &parentId : it->parentSpaceIds) {
            if (parentId == entry.info.id || !joinedSpaceIds.contains(parentId))
                continue;
            parentsOf[entry.info.id].insert(parentId);
            // Keep the edge symmetric: a parent that only ever announced
            // itself from below still has to be able to nest this Space, or
            // the child would be a root under a parent that lists it.
            QStringList &children = childSpacesOf[parentId];
            if (!children.contains(entry.info.id))
                children.append(entry.info.id);
        }
    }

    // Breadth-first from the roots. Assign-once is what makes this both
    // cycle-safe and stable under several parents.
    QHash<QString, int> levelOf;
    QHash<QString, QString> primaryParentOf;
    QStringList queue;
    for (const SpaceEntry &entry : m_spaces) {
        if (parentsOf.value(entry.info.id).isEmpty()) {
            levelOf.insert(entry.info.id, 0);
            primaryParentOf.insert(entry.info.id, QString());
            queue.append(entry.info.id);
        }
    }
    for (int head = 0; head < queue.size(); ++head) {
        const QString parentId = queue.at(head);
        const int childLevel = levelOf.value(parentId) + 1;
        for (const QString &childId : childSpacesOf.value(parentId)) {
            if (levelOf.contains(childId))
                continue;
            levelOf.insert(childId, childLevel);
            primaryParentOf.insert(childId, parentId);
            queue.append(childId);
        }
    }

    for (SpaceEntry &entry : m_spaces) {
        const QString id = entry.info.id;
        // A Space the walk never reached is inside a parent cycle. It becomes
        // a root: visible, expandable, and never recursed into twice.
        entry.level = levelOf.value(id, 0);
        entry.parentSpaceId = primaryParentOf.value(id, QString());
        entry.childSpaceIds.clear();
        for (const QString &childId : childSpacesOf.value(id)) {
            if (primaryParentOf.value(childId) == id)
                entry.childSpaceIds.append(childId);
        }
    }
}

QStringList SpaceManager::childSpaceIds(const QString &spaceId) const
{
    for (const SpaceEntry &entry : m_spaces) {
        if (entry.info.id == spaceId)
            return entry.childSpaceIds;
    }
    return {};
}

bool SpaceManager::roomInAnySpace(const QString &roomId) const
{
    return m_spaceChildRoomIds.contains(roomId);
}

void SpaceManager::rebuild()
{
    beginResetModel();
    m_spaces.clear();
    m_membership.clear();
    m_allRoomIds.clear();
    m_orphanRoomIds.clear();
    m_spaceChildRoomIds.clear();
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
        // childRoomIds is the DIRECT child list; the membership this manager
        // publishes is deliberately TRANSITIVE (a subspace's rooms belong to
        // every ancestor for "show me everything in this Space"), so walk it.
        // The visited set plus the depth cap keep a malformed cyclic
        // hierarchy from duplicating rows or recursing forever.
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
        // Direct child rooms: the complement of this set is the Channels
        // layout's "Rooms" group, so it has to be DIRECT — a room whose only
        // parent is a subspace is listed by that subspace's own folder.
        for (const QString &childId : r.childRoomIds) {
            const auto it = byId.constFind(childId);
            if (it == byId.constEnd() || it->isSpace
                || it->membership != RoomInfo::Joined) {
                continue;
            }
            m_spaceChildRoomIds.insert(childId);
        }
        m_spaces.append(std::move(e));
    }

    resolveHierarchy(byId);

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

void SpaceManager::setSpaceChildSuggested(const QString &spaceId,
                                          const QString &roomId,
                                          bool suggested)
{
    if (!m_client || spaceId.isEmpty() || roomId.isEmpty())
        return;
    // No membership pre-check here: the backend reads the CURRENT
    // m.space.child and refuses a non-child itself — the local graph only
    // tracks joined children, and the suggested flag is equally valid on
    // an unjoined child the /hierarchy lists.
    const quint64 opId =
        m_client->setSpaceChildSuggested(spaceId, roomId, suggested);
    if (opId == 0) {
        Q_EMIT childSuggestedFinished(spaceId, roomId, suggested, false);
        return;
    }
    m_pendingChildSuggests.insert(opId, { spaceId, roomId });
}
