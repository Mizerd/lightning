#include "spaces/SpaceManager.h"

#include "matrix/MatrixClient.h"

SpaceManager::SpaceManager(QObject *parent)
    : QAbstractListModel(parent)
{
    // See m_rebuildCoalesce's note: a batch of room updates must cost one
    // rebuild, not one per room.
    m_rebuildCoalesce.setSingleShot(true);
    m_rebuildCoalesce.setInterval(0);
    connect(&m_rebuildCoalesce, &QTimer::timeout, this, &SpaceManager::rebuild);
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
        // COALESCED, unlike roomsChanged below it. A per-room signal can
        // arrive in a batch; a structural one cannot.
        connect(m_client, &MatrixClient::roomUpdated, this,
                [this](const QString &) { m_rebuildCoalesce.start(); });
        connect(m_client, &MatrixClient::loggedOut,
                this, [this] {
            m_pendingChildAdds.clear(); // account isolation
            m_pendingChildRemovals.clear();
            m_pendingChildSuggests.clear();
            m_lobbyCollapsed.clear();
            dropSpaceRosters();
            rebuild();
        });
        connect(m_client, &MatrixClient::roomMembersReceived,
                this, &SpaceManager::onRoomMembersReceived);
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
    // Folded lobby sections name one account's Spaces.
    m_lobbyCollapsed.clear();
    // A roster is an ANSWER ABOUT ONE ACCOUNT. Carrying one across a client
    // swap would scope the next account's People list by the previous
    // account's Space membership.
    dropSpaceRosters();
    rebuild();
    // The selection survives a client swap (the rail restores it), so the
    // roster for wherever the user already is has to be asked for again.
    ensureSpaceRoster(m_activeSpaceId);
}

void SpaceManager::setActiveSpaceId(const QString &spaceId)
{
    if (m_activeSpaceId == spaceId)
        return;
    m_activeSpaceId = spaceId;
    // Selecting a Space is the one moment that justifies asking who is in it:
    // it is a user action, it happens once per Space per session, and it is
    // the same shape as AppController hydrating a room's roster on first
    // open. Nothing here waits for the answer.
    ensureSpaceRoster(m_activeSpaceId);
    Q_EMIT activeSpaceIdChanged();
}

bool SpaceManager::isRealSpaceId(const QString &spaceId)
{
    // "" is All rooms, "@…" are the pseudo rail selections. A real Matrix
    // room id starts with '!' and nothing else can.
    return spaceId.startsWith(QLatin1Char('!'));
}

void SpaceManager::dropSpaceRosters()
{
    const QStringList had = m_spaceMembers.keys();
    m_spaceMembers.clear();
    m_rosterRequested.clear();
    for (const QString &spaceId : had)
        Q_EMIT spaceRosterChanged(spaceId);
}

void SpaceManager::ensureSpaceRoster(const QString &spaceId)
{
    if (!m_client || !isRealSpaceId(spaceId))
        return;
    if (m_rosterRequested.contains(spaceId))
        return;
    const quint64 opId = m_client->requestRoomMembers(spaceId);
    // Record it ONLY when the dispatch actually went out. A synchronous
    // rejection — no SDK handle yet, a backend with no member support —
    // returns 0 and never answers, and marking it here would leave the Space
    // permanently "asked" and its People list permanently unscoped.
    if (opId == 0)
        return;
    m_rosterRequested.insert(spaceId);
}

void SpaceManager::onRoomMembersReceived(quint64 opId, const QString &roomId,
                                         const QVariantMap &snapshot)
{
    Q_UNUSED(opId);
    // KEYED ON THE ROOM, NOT ON THE OP, and deliberately.
    //
    // The op map every other pending-request table here uses answers "did I
    // ask this?", which is the right question when the ANSWER is scoped to
    // the asker — a send result, an edit outcome. A roster is not: "who is in
    // room X" has one answer whoever asked for it, and the member panel or a
    // mention completion fetching the same Space's roster is the same fact
    // arriving for free.
    //
    // It also closes an ordering hazard the op map would have. The op id only
    // exists AFTER requestRoomMembers returns, so a backend that emitted
    // synchronously would deliver into an empty table — the answer dropped,
    // the Space marked as asked, and its People list unscoped for the whole
    // session. Every backend here is asynchronous (MockMatrixClient says so
    // in a comment, for this exact reason), which makes that a property of
    // today's clients rather than of this code.
    //
    // What it does NOT accept is a room nobody asked about: `m_rosterRequested`
    // only ever holds Spaces this manager requested through a dispatch that
    // actually went out.
    if (!m_rosterRequested.contains(roomId))
        return;
    // The Rust bridge answers a member request TWICE under one op: a
    // cache-only `partial` snapshot first, then the synced roster. The
    // partial one is a subset by construction, so recording it would publish
    // an incomplete roster as a complete one — and every DM whose peer was
    // missing from it would disappear from the Space until the real answer
    // landed.
    if (snapshot.value(QStringLiteral("partial")).toBool())
        return;
    const QString spaceId = roomId;

    if (!snapshot.value(QStringLiteral("ok")).toBool()) {
        // Un-mark, so selecting the Space again retries rather than failing
        // closed for the whole session.
        m_rosterRequested.remove(spaceId);
        return;
    }
    if (snapshot.value(QStringLiteral("truncated")).toBool()) {
        // The bridge caps a roster at 500 active members and says so. A
        // capped list cannot answer "is this person in the Space" — the
        // people it dropped are indistinguishable from the people who are
        // not there — so the roster stays UNKNOWN and both layouts fall back
        // to their unscoped behaviour. Deliberately still marked as
        // requested: asking again would return the same cap.
        return;
    }
    QSet<QString> members;
    for (const QVariant &value :
         snapshot.value(QStringLiteral("members")).toList()) {
        const QVariantMap entry = value.toMap();
        const QString membership =
            entry.value(QStringLiteral("membership")).toString();
        // Joined and invited are "in the Space"; banned members are in the
        // snapshot too and are emphatically not.
        if (membership != QLatin1String("joined")
            && membership != QLatin1String("invited")) {
            continue;
        }
        const QString userId = entry.value(QStringLiteral("userId")).toString();
        if (!userId.isEmpty())
            members.insert(userId);
    }
    m_spaceMembers.insert(spaceId, members);
    Q_EMIT spaceRosterChanged(spaceId);
}

bool SpaceManager::spaceRosterKnown(const QString &spaceId) const
{
    return m_spaceMembers.contains(spaceId);
}

bool SpaceManager::spaceHasMember(const QString &spaceId,
                                  const QString &userId) const
{
    const auto it = m_spaceMembers.constFind(spaceId);
    return it != m_spaceMembers.constEnd() && it->contains(userId);
}

int SpaceManager::directScope(const QString &spaceId,
                              const QStringList &peerIds) const
{
    if (!isRealSpaceId(spaceId))
        return -1;
    const auto it = m_spaceMembers.constFind(spaceId);
    if (it == m_spaceMembers.constEnd())
        return -1;
    if (peerIds.isEmpty())
        return -1;   // no peer to judge: unknown, never "not in the Space"
    for (const QString &peer : peerIds) {
        if (!peer.isEmpty() && it->contains(peer))
            return 1;
    }
    // A group DM belongs to the Space if ANY of its people do. Requiring all
    // of them would drop a conversation from the Space over one outsider.
    return 0;
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
        // A TILE COUNTS WHAT ITS VIEW LISTS. In Channels, a Space's rooms
        // are on the Space's tile and Home does not list them — so counting
        // them here sends the user to Home looking for a message that can
        // never appear there. DMs are on their own tile AND, since
        // 2026-09-05, listed at Home again (a Direct Messages group after
        // Rooms, at the maintainer's request), so Home counts them too: a
        // room in two views is counted by both tiles, exactly as a room under
        // two Spaces already is. Classic has no People tab and its Home lists
        // the whole account, so there the whole-account total is the honest
        // one.
        case UnreadTotalRole:
            return m_directMessagesHaveOwnTile
                ? m_unparentedUnreadTotal + m_peopleUnreadTotal
                : m_homeUnreadTotal;
        case HighlightTotalRole:
            return m_directMessagesHaveOwnTile
                ? m_unparentedHighlightTotal + m_peopleHighlightTotal
                : m_homeHighlightTotal;
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
            // Was hardcoded 0, which made the one tile that does list these
            // rooms the one tile that could never say they had traffic.
            case UnreadTotalRole: return m_unparentedUnreadTotal;
            case HighlightTotalRole: return m_unparentedHighlightTotal;
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
    case DirectChildRoomCountRole: return s.directChildRoomCount;
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
        { DirectChildRoomCountRole, "directChildRoomCount" },
    };
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
    if (spaceId == peopleId()) {
        // Read from the CLIENT, not from a membership set: a DM is not a
        // Space's child and never will be, so there is nothing here to
        // accumulate. Anything that scopes by this id gets the DMs.
        QStringList out;
        if (!m_client)
            return out;
        const QList<RoomInfo> rooms = m_client->rooms();
        out.reserve(rooms.size());
        for (const RoomInfo &room : rooms) {
            if (!room.isSpace && room.isDirect)
                out.append(room.id);
        }
        return out;
    }
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
            // CARRIED BECAUSE A CALLER SORTS ON IT, AND SILENTLY GOT
            // `undefined` WHEN IT WAS NOT HERE.
            //
            // The rail's inline room reveal orders by activity and takes the
            // top few. It used to call childRoomsDetailed(), which is the
            // TRANSITIVE list and carries this key; moving it to the direct
            // list — which is the actual fix for showing a subspace's rooms
            // under its ancestors — would have handed the comparator
            // `undefined - undefined`, i.e. NaN, on every pair. The sort then
            // does nothing and "top rooms by activity" quietly becomes
            // whatever order the state happened to be in, with no error
            // anywhere. The two accessors are still not interchangeable in
            // the other direction either: `encrypted` above is here and not
            // there.
            { QStringLiteral("lastActivity"),     it->lastActivity },
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

QString SpaceManager::parentSpaceIdOf(const QString &spaceId) const
{
    for (const SpaceEntry &entry : m_spaces) {
        if (entry.info.id == spaceId)
            return entry.parentSpaceId;
    }
    return {};
}

QStringList SpaceManager::ancestorSpaceIds(const QString &spaceId) const
{
    QStringList out;
    QString cursor = parentSpaceIdOf(spaceId);
    // BOUNDED. `recomputeHierarchy()` assigns each Space one primary parent
    // and never revisits an assigned id, so a cycle cannot survive that walk
    // — but this reads the RESULT of it, and a bound here costs nothing next
    // to a hang if that invariant ever weakens.
    while (!cursor.isEmpty() && out.size() < 64 && !out.contains(cursor)) {
        out.append(cursor);
        cursor = parentSpaceIdOf(cursor);
    }
    return out;
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
    m_peopleUnreadTotal = 0;
    m_peopleHighlightTotal = 0;
    m_unparentedUnreadTotal = 0;
    m_unparentedHighlightTotal = 0;

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
            // Home's total is the WHOLE account, which is what the Classic
            // layout's Home genuinely lists.
            m_homeUnreadTotal += r.unreadCount;
            m_homeHighlightTotal += r.highlightCount;
            if (r.isDirect) {
                m_peopleUnreadTotal += r.unreadCount;
                m_peopleHighlightTotal += r.highlightCount;
            }
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
            // Counted HERE rather than derived later, because this loop has
            // already applied every filter the count has to respect: direct,
            // joined, and not itself a Space.
            e.directChildRoomCount += 1;
        }
        m_spaces.append(std::move(e));
    }

    // WHAT THE CHANNELS HOME AND "OTHER ROOMS" VIEWS ACTUALLY LIST: joined,
    // not a Space, not a DM, and not a direct child of any Space. This has to
    // run after the loop above, because m_spaceChildRoomIds is only complete
    // once every Space has contributed its children.
    //
    // It deliberately mirrors SpaceChannelModel::buildHome's own predicate
    // rather than reusing m_orphanRoomIds, which is computed from the
    // TRANSITIVE child sets and includes DMs — a badge derived from a
    // different set than the view uses is the defect this whole block exists
    // to close.
    for (const QString &roomId : m_allRoomIds) {
        const auto it = byId.constFind(roomId);
        if (it == byId.constEnd() || it->isDirect)
            continue;
        if (m_spaceChildRoomIds.contains(roomId))
            continue;
        m_unparentedUnreadTotal += it->unreadCount;
        m_unparentedHighlightTotal += it->highlightCount;
    }

    resolveHierarchy(byId);

    // ONLY A REAL SPACE ID IS CHECKED AGAINST MEMBERSHIP. The rail's selection
    // also carries TAB SENTINELS -- "@people" for Direct Messages, "@orphans"
    // for the rooms in no Space -- and those are not rooms, so they are never
    // in m_membership. Clearing on that basis dropped the scope back to Home
    // every time anything rebuilt the space list, which opening a DM does: the
    // user clicked a person in Direct Messages and was thrown to Home.
    //
    // A Matrix room id always starts with '!', so that is the whole test; an
    // empty id is Home and was already exempt.
    //
    // AND MEMBERSHIP IS THE WRONG THING TO ASK. `m_membership[<space>]` is
    // created inside the descendant walk above, at the moment a joined child
    // ROOM is found -- so a Space the user is genuinely in, but whose rooms
    // they have not joined, has no key at all and this guard threw them back
    // to Home on the next rebuild. Two ordinary cases hit it: a Space just
    // created, and a public Space joined from Explore before joining any of
    // its rooms. RoomsPanel binds the column's scopeSpaceId to this, so the
    // Space view closes under the reader at precisely the moment they want
    // it open to reach Lobby.
    //
    // The question this guard is actually asking is "is the selected Space
    // still one of the Spaces I am in", and `m_spaces` is the answer: every
    // joined Space gets an entry above, rooms or no rooms.
    const auto selectionIsStillAJoinedSpace = [this] {
        for (const SpaceEntry &entry : m_spaces) {
            if (entry.info.id == m_activeSpaceId)
                return true;
        }
        return false;
    };
    if (m_activeSpaceId.startsWith(QLatin1Char('!'))
        && !selectionIsStillAJoinedSpace()) {
        m_activeSpaceId.clear();
        Q_EMIT activeSpaceIdChanged();
    }

    recomputeOrphans();

    endResetModel();
    Q_EMIT spacesChanged();
}

void SpaceManager::setDirectMessagesHaveOwnTile(bool own)
{
    if (m_directMessagesHaveOwnTile == own)
        return;
    m_directMessagesHaveOwnTile = own;
    // Only Home's two totals change meaning, and only if there is a row to
    // change: no aggregate is recomputed, so a full rebuild would be wasted
    // work on a setting the user toggles by switching layout.
    if (rowCount() > 0) {
        const QModelIndex home = index(0, 0);
        Q_EMIT dataChanged(home, home,
                           { UnreadTotalRole, HighlightTotalRole });
    }
    Q_EMIT spacesChanged();
    Q_EMIT directMessagesHaveOwnTileChanged();
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
    // The pre-check asks whether the room is a DIRECT child — the only thing
    // an m.space.child event in THIS Space can undo. It used to ask
    // includesRoom(), which is TRANSITIVE and covers joined non-Space rooms
    // only, so it was wrong both ways: a room of a SUBSPACE passed, and an
    // empty-via m.space.child was sent into a Space it was never a child of
    // (reported "removed", changed nothing); a child SPACE or an UNJOINED
    // child failed, and was reported "removed" without any request at all.
    // The Space Home lobby lets a manager select both of those.
    bool direct = false;
    for (const SpaceEntry &entry : m_spaces) {
        if (entry.info.id == spaceId) {
            direct = entry.info.childRoomIds.contains(roomId);
            break;
        }
    }
    if (!direct) {
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

// ---- The Space Home lobby --------------------------------------------------

namespace {

QHash<QString, QVariantMap> hierarchyIndex(const QVariantList &rows)
{
    QHash<QString, QVariantMap> out;
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QString id = row.value(QStringLiteral("roomId")).toString();
        if (!id.isEmpty() && !out.contains(id))
            out.insert(id, row);
    }
    return out;
}

// A parent's children in the order the lobby shows them: its own
// m.space.child order first, then any /hierarchy row that order does not
// know yet (a child whose state event has not synced, or a backend that
// reports the edge only through /hierarchy), in the SDK's own order.
QStringList lobbyChildOrder(const QString &parentId,
                            const QHash<QString, RoomInfo> &byId,
                            const QVariantList &hierarchyRows)
{
    QStringList out;
    QSet<QString> seen;
    const auto parent = byId.constFind(parentId);
    if (parent != byId.constEnd()) {
        for (const QString &id : parent->childRoomIds) {
            if (id.isEmpty() || id == parentId || seen.contains(id))
                continue;
            seen.insert(id);
            out.append(id);
        }
    }
    for (const QVariant &value : hierarchyRows) {
        const QString id =
            value.toMap().value(QStringLiteral("roomId")).toString();
        if (id.isEmpty() || id == parentId || seen.contains(id))
            continue;
        seen.insert(id);
        out.append(id);
    }
    return out;
}

bool isJoinedSpace(const QHash<QString, RoomInfo> &byId, const QString &id)
{
    const auto it = byId.constFind(id);
    return it != byId.constEnd() && it->membership == RoomInfo::Joined
           && it->isSpace;
}

// Joined direct non-Space children of a Space: the "N rooms" a nested Space
// row claims. Direct, like everything else in the lobby.
int joinedDirectRoomCount(const QString &spaceId,
                          const QHash<QString, RoomInfo> &byId)
{
    const auto parent = byId.constFind(spaceId);
    if (parent == byId.constEnd())
        return 0;
    int count = 0;
    QSet<QString> seen;
    for (const QString &id : parent->childRoomIds) {
        if (seen.contains(id))
            continue;
        seen.insert(id);
        const auto it = byId.constFind(id);
        if (it != byId.constEnd() && !it->isSpace
            && it->membership == RoomInfo::Joined)
            ++count;
    }
    return count;
}

bool lobbyMatches(const QString &needle, const QString &name,
                  const QString &topic)
{
    return needle.isEmpty() || name.contains(needle, Qt::CaseInsensitive)
           || topic.contains(needle, Qt::CaseInsensitive);
}

// One row, or an invalid map when the child cannot be shown honestly: an id
// neither sync nor /hierarchy knows anything about is never drawn as a
// placeholder.
QVariantMap lobbyRow(const QString &id, const QString &parentId,
                     const QString &homeId,
                     const QHash<QString, RoomInfo> &byId,
                     const QHash<QString, QVariantMap> &meta)
{
    // Selectable only where the Home's own m.space.child names it: a row
    // only /hierarchy knows has no event here that Remove could change.
    const auto home = byId.constFind(homeId);
    const bool selectable = parentId == homeId && home != byId.constEnd()
                            && home->childRoomIds.contains(id);
    const QVariantMap h = meta.value(id);
    const bool known = meta.contains(id);
    const bool suggestedKnown = known && h.contains(QStringLiteral("suggested"));
    const auto it = byId.constFind(id);
    const auto parent = byId.constFind(parentId);
    const bool declared = parent != byId.constEnd()
                          && parent->childRoomIds.contains(id);
    // A JOINED room the parent's synced state does not list is not its
    // child, whatever a cached /hierarchy answer says: that cache is how a
    // just-removed child came back (seen live 2026-09-24).
    if (!declared && parent != byId.constEnd() && it != byId.constEnd()
        && it->membership == RoomInfo::Joined)
        return {};
    if (it != byId.constEnd() && it->membership == RoomInfo::Joined) {
        // Sync is authoritative for a joined room; /hierarchy only fills in
        // what sync does not carry (member count, suggested) or has not
        // carried yet (a topic).
        const QString topic = it->topic.isEmpty()
            ? h.value(QStringLiteral("topic")).toString() : it->topic;
        return QVariantMap{
            { QStringLiteral("roomId"), id },
            { QStringLiteral("parentId"), parentId },
            { QStringLiteral("name"), it->name.isEmpty()
                  ? h.value(QStringLiteral("name")).toString() : it->name },
            { QStringLiteral("avatarUrl"), it->avatarUrl },
            { QStringLiteral("identityColorKey"), identityColorKey(*it) },
            { QStringLiteral("topic"), topic },
            { QStringLiteral("isSpace"), it->isSpace },
            { QStringLiteral("joined"), true },
            { QStringLiteral("isDirect"), it->isDirect },
            { QStringLiteral("suggested"),
              h.value(QStringLiteral("suggested")).toBool() },
            { QStringLiteral("suggestedKnown"), suggestedKnown },
            { QStringLiteral("members"),
              h.value(QStringLiteral("members")).toLongLong() },
            { QStringLiteral("childCount"),
              it->isSpace ? joinedDirectRoomCount(id, byId) : 0 },
            { QStringLiteral("childrenCount"),
              h.value(QStringLiteral("childrenCount")).toLongLong() },
            // ONE unread rule for the row badge and the section total.
            { QStringLiteral("hasUnread"),
              it->hasUnreadMessages || it->unreadCount > 0 },
            { QStringLiteral("unreadCount"), it->unreadCount },
            { QStringLiteral("highlightCount"), it->highlightCount },
            { QStringLiteral("membership"), QStringLiteral("joined") },
            { QStringLiteral("joinRule"), QString() },
            { QStringLiteral("via"), QStringList() },
            { QStringLiteral("selectable"), selectable },
        };
    }
    if (!known)
        return {};
    // /hierarchy says joined but sync has not delivered the room yet:
    // opening it would fail and joining it again is wrong, so it waits for
    // sync (the flat list's rule, kept).
    if (h.value(QStringLiteral("membership")).toString()
        == QLatin1String("joined"))
        return {};
    return QVariantMap{
        { QStringLiteral("roomId"), id },
        { QStringLiteral("parentId"), parentId },
        { QStringLiteral("name"), h.value(QStringLiteral("name")).toString() },
        { QStringLiteral("avatarUrl"),
          h.value(QStringLiteral("avatarUrl")).toString() },
        { QStringLiteral("identityColorKey"), QString() },
        { QStringLiteral("topic"), h.value(QStringLiteral("topic")).toString() },
        { QStringLiteral("isSpace"), h.value(QStringLiteral("isSpace")).toBool() },
        { QStringLiteral("joined"), false },
        { QStringLiteral("isDirect"), false },
        { QStringLiteral("suggested"),
          h.value(QStringLiteral("suggested")).toBool() },
        { QStringLiteral("suggestedKnown"), suggestedKnown },
        { QStringLiteral("members"),
          h.value(QStringLiteral("members")).toLongLong() },
        { QStringLiteral("childCount"), 0 },
        { QStringLiteral("childrenCount"),
          h.value(QStringLiteral("childrenCount")).toLongLong() },
        { QStringLiteral("hasUnread"), false },
        { QStringLiteral("unreadCount"), 0 },
        { QStringLiteral("highlightCount"), 0 },
        { QStringLiteral("membership"),
          h.value(QStringLiteral("membership")).toString() },
        { QStringLiteral("joinRule"),
          h.value(QStringLiteral("joinRule")).toString() },
        { QStringLiteral("via"), h.value(QStringLiteral("via")).toStringList() },
        { QStringLiteral("selectable"), selectable },
    };
}

} // namespace

QVariantList SpaceManager::buildLobbySections(
    const QString &spaceId, const QHash<QString, RoomInfo> &byId,
    const QVariantMap &hierarchyBySpace, const QString &filter,
    const QSet<QString> &collapsedSections)
{
    QVariantList out;
    if (spaceId.isEmpty())
        return out;
    const QString needle = filter.trimmed();
    const bool searching = !needle.isEmpty();

    const QVariantList homeRows =
        hierarchyBySpace.value(spaceId).toList();
    const QHash<QString, QVariantMap> homeMeta = hierarchyIndex(homeRows);
    const QStringList homeOrder = lobbyChildOrder(spaceId, byId, homeRows);

    // Builds one section: `sectionSpaceId`'s children, `header` being the
    // section's own identity fields.
    const auto section = [&](const QString &sectionSpaceId, bool isRoot,
                             QVariantMap header, const QStringList &order,
                             const QHash<QString, QVariantMap> &meta)
        -> QVariantMap {
        QVariantList all;
        int roomCount = 0;
        int spaceCount = 0;
        int unreadTotal = 0;
        int highlightTotal = 0;
        bool anyUnread = false;
        for (const QString &id : order) {
            if (id == spaceId || id == sectionSpaceId)
                continue; // a malformed or cyclic hierarchy
            // On the ROOT, a joined child Space is a section of its own and
            // never also a row.
            if (isRoot && isJoinedSpace(byId, id))
                continue;
            const QVariantMap row =
                lobbyRow(id, sectionSpaceId, spaceId, byId, meta);
            if (row.isEmpty())
                continue;
            if (row.value(QStringLiteral("isSpace")).toBool())
                ++spaceCount;
            else
                ++roomCount;
            unreadTotal += row.value(QStringLiteral("unreadCount")).toInt();
            anyUnread = anyUnread
                        || row.value(QStringLiteral("hasUnread")).toBool();
            highlightTotal +=
                row.value(QStringLiteral("highlightCount")).toInt();
            all.append(row);
        }
        const bool headerMatches = !isRoot && searching
            && lobbyMatches(needle,
                            header.value(QStringLiteral("name")).toString(),
                            header.value(QStringLiteral("topic")).toString());
        QVariantList shown;
        for (const QVariant &value : all) {
            const QVariantMap row = value.toMap();
            if (!searching || headerMatches
                || lobbyMatches(needle,
                                row.value(QStringLiteral("name")).toString(),
                                row.value(QStringLiteral("topic")).toString()))
                shown.append(row);
        }
        const bool collapsed =
            !searching && collapsedSections.contains(sectionSpaceId);
        header.insert(QStringLiteral("sectionId"), sectionSpaceId);
        header.insert(QStringLiteral("isRoot"), isRoot);
        header.insert(QStringLiteral("roomCount"), roomCount);
        header.insert(QStringLiteral("spaceCount"), spaceCount);
        header.insert(QStringLiteral("unreadTotal"), unreadTotal);
        header.insert(QStringLiteral("highlightTotal"), highlightTotal);
        header.insert(QStringLiteral("hasUnread"), anyUnread);
        header.insert(QStringLiteral("matchCount"), shown.size());
        header.insert(QStringLiteral("collapsed"), collapsed);
        header.insert(QStringLiteral("rows"),
                      collapsed ? QVariantList() : shown);
        // Whether the section is drawn at all.
        const bool visible = searching
            ? (headerMatches || !shown.isEmpty())
            : (!isRoot || !all.isEmpty());
        header.insert(QStringLiteral("__visible"), visible);
        return header;
    };

    // The root: the Space's own direct rooms (and its UNJOINED child Spaces,
    // which cannot be opened as a section until they are joined).
    {
        const QVariantMap root = section(
            spaceId, true,
            QVariantMap{
                { QStringLiteral("roomId"), spaceId },
                { QStringLiteral("name"), QString() },
                { QStringLiteral("avatarUrl"), QString() },
                { QStringLiteral("identityColorKey"), QString() },
                { QStringLiteral("topic"), QString() },
                { QStringLiteral("suggested"), false },
                { QStringLiteral("suggestedKnown"), false },
                { QStringLiteral("selectable"), false },
            },
            homeOrder, homeMeta);
        if (root.value(QStringLiteral("__visible")).toBool()) {
            QVariantMap copy = root;
            copy.remove(QStringLiteral("__visible"));
            out.append(copy);
        }
    }

    // One section per JOINED direct child Space, in the Home's order.
    const auto homeIt = byId.constFind(spaceId);
    const QStringList declared =
        homeIt != byId.constEnd() ? homeIt->childRoomIds : QStringList();
    for (const QString &childId : homeOrder) {
        if (childId == spaceId || !isJoinedSpace(byId, childId))
            continue;
        // Only a subspace the Home's synced state still lists: a cached
        // /hierarchy answer would keep a just-removed one as a section, with
        // Remove offered on something that is no longer a child.
        if (!declared.contains(childId))
            continue;
        const RoomInfo &info = *byId.constFind(childId);
        const QVariantMap h = homeMeta.value(childId);
        const QVariantList rows = hierarchyBySpace.value(childId).toList();
        const QVariantMap sec = section(
            childId, false,
            QVariantMap{
                { QStringLiteral("roomId"), childId },
                { QStringLiteral("name"), info.name.isEmpty()
                      ? h.value(QStringLiteral("name")).toString()
                      : info.name },
                { QStringLiteral("avatarUrl"), info.avatarUrl },
                { QStringLiteral("identityColorKey"), identityColorKey(info) },
                { QStringLiteral("topic"), info.topic.isEmpty()
                      ? h.value(QStringLiteral("topic")).toString()
                      : info.topic },
                { QStringLiteral("suggested"),
                  h.value(QStringLiteral("suggested")).toBool() },
                { QStringLiteral("suggestedKnown"),
                  h.contains(QStringLiteral("suggested")) },
                // The subspace IS a direct child of the Home Space, so the
                // Home's Remove / Mark as suggested apply to it.
                { QStringLiteral("selectable"), true },
            },
            lobbyChildOrder(childId, byId, rows), hierarchyIndex(rows));
        if (sec.value(QStringLiteral("__visible")).toBool()) {
            QVariantMap copy = sec;
            copy.remove(QStringLiteral("__visible"));
            out.append(copy);
        }
    }
    return out;
}

QVariantList SpaceManager::lobbySections(const QString &spaceId,
                                         const QVariantMap &hierarchyBySpace,
                                         const QString &filter) const
{
    if (!m_client || spaceId.isEmpty())
        return {};
    QHash<QString, RoomInfo> byId;
    const auto rooms = m_client->rooms();
    byId.reserve(rooms.size());
    for (const RoomInfo &room : rooms)
        byId.insert(room.id, room);
    return buildLobbySections(spaceId, byId, hierarchyBySpace, filter,
                              m_lobbyCollapsed.value(spaceId));
}

QStringList SpaceManager::lobbySubspaceIds(const QString &spaceId) const
{
    QStringList out;
    if (!m_client || spaceId.isEmpty())
        return out;
    QHash<QString, RoomInfo> byId;
    const auto rooms = m_client->rooms();
    byId.reserve(rooms.size());
    for (const RoomInfo &room : rooms)
        byId.insert(room.id, room);
    for (const QString &id : lobbyChildOrder(spaceId, byId, {})) {
        if (isJoinedSpace(byId, id))
            out.append(id);
    }
    return out;
}

void SpaceManager::setLobbySectionCollapsed(const QString &spaceId,
                                            const QString &sectionId,
                                            bool collapsed)
{
    if (spaceId.isEmpty() || sectionId.isEmpty())
        return;
    if (lobbySectionCollapsed(spaceId, sectionId) == collapsed)
        return;
    QSet<QString> &set = m_lobbyCollapsed[spaceId];
    if (collapsed)
        set.insert(sectionId);
    else
        set.remove(sectionId);
    if (set.isEmpty())
        m_lobbyCollapsed.remove(spaceId);
    Q_EMIT lobbyCollapseChanged(spaceId);
}

bool SpaceManager::lobbySectionCollapsed(const QString &spaceId,
                                         const QString &sectionId) const
{
    return m_lobbyCollapsed.value(spaceId).contains(sectionId);
}
