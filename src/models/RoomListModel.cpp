#include "models/RoomListModel.h"

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

#include <algorithm>
#include <QSet>
#include <QUrl>

RoomListModel::RoomListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_searchDebounce.setSingleShot(true);
    m_searchDebounce.setInterval(200);
    connect(&m_searchDebounce, &QTimer::timeout, this, [this] {
        if (m_searchQuery == m_pendingSearchQuery) return;
        m_searchQuery = m_pendingSearchQuery;
        ++m_filterGeneration;
        Q_EMIT searchQueryChanged();
        Q_EMIT filterGenerationChanged();
        reconcileRooms();
    });
    // Per-room update signals (roomUpdated fires per incoming event,
    // membersChanged per roster arrival) coalesce onto one zero-timer
    // reconcile per event-loop turn — a sync burst used to run the full
    // sort-and-diff pass once per event.
    m_reconcileCoalesce.setSingleShot(true);
    m_reconcileCoalesce.setInterval(0);
    connect(&m_reconcileCoalesce, &QTimer::timeout,
            this, &RoomListModel::reconcileRooms);
}

void RoomListModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_profileAvatars.clear();
    m_profileOps.clear();
    m_profilePending.clear();
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomsChanged,
                this, &RoomListModel::refresh);
        connect(m_client, &MatrixClient::roomUpdated,
                this, &RoomListModel::refreshRoom);
        connect(m_client, &MatrixClient::membersChanged,
                this, &RoomListModel::refreshRoom);
        connect(m_client, &MatrixClient::loginSucceeded,
                this, [this](const QString &) { refresh(); });
        connect(m_client, &MatrixClient::userProfileFinished,
                this, &RoomListModel::onUserProfileFinished);
        connect(m_client, &MatrixClient::loggedOut,
                this, &RoomListModel::clearProfileCaches);
    }
    refresh();
}

void RoomListModel::clearProfileCaches()
{
    m_profileAvatars.clear();
    m_profileOps.clear();
    m_profilePending.clear();
    refresh();
}

void RoomListModel::setSpaceManager(SpaceManager *spaces)
{
    if (m_spaces == spaces)
        return;
    if (m_spaces)
        m_spaces->disconnect(this);
    m_spaces = spaces;
    if (m_spaces) {
        connect(m_spaces, &SpaceManager::activeSpaceIdChanged, this, [this] {
            ++m_filterGeneration;
            Q_EMIT filterGenerationChanged();
            reconcileRooms();
        });
        connect(m_spaces, &SpaceManager::spacesChanged,
                this, &RoomListModel::refresh);
    }
    refresh();
}

int RoomListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rooms.size());
}

QVariant RoomListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rooms.size())
        return {};
    const auto &r = m_rooms.at(index.row());
    switch (role) {
    case RoomIdRole:             return r.id;
    case NameRole:               return r.name;
    case TopicRole:              return r.topic;
    case AvatarUrlRole:          return effectiveAvatarUrl(r);
    case LastMessagePreviewRole: return r.lastMessagePreview;
    case LastActivityRole:       return r.lastActivity;
    case UnreadCountRole:        return r.unreadCount;
    case EncryptedRole:          return r.encrypted;
    case IsSpaceRole:            return r.isSpace;
    case MemberCountRole:        return static_cast<int>(r.members.size());
    case CategoryRole:           return r.membership == RoomInfo::Invited
                                     ? QStringLiteral("invite")
                                     : (r.isDirect ? QStringLiteral("dm")
                                                   : QStringLiteral("room"));
    case HighlightCountRole:     return r.highlightCount;
    case MarkedUnreadRole:       return r.markedUnread;
    case HasUnreadRole:          return r.hasUnreadMessages;
    case MembershipRole:
        switch (r.membership) {
        case RoomInfo::Invited: return QStringLiteral("invited");
        case RoomInfo::Knocked: return QStringLiteral("knocked");
        case RoomInfo::Left: return QStringLiteral("left");
        case RoomInfo::Joined: return QStringLiteral("joined");
        }
        return QStringLiteral("joined");
    case IsDirectRole:           return r.isDirect;
    case DirectUserIdRole:       return r.directUserId;
    case InviterRole:            return r.inviterDisplayName.isEmpty()
                                     ? r.inviterUserId : r.inviterDisplayName;
    case InvitePendingRole:      return r.invitePending;
    case InviteErrorRole:        return r.inviteError;
    case CanonicalAliasRole:     return r.canonicalAlias;
    case IdentityColorKeyRole:   return identityColorKey(r);
    case SuccessorRoomIdRole:    return r.successorRoomId;
    case SupersededByAccessibleSuccessorRole:
        return m_supersededRoomIds.contains(r.id);
    default:                     return {};
    }
}

QHash<int, QByteArray> RoomListModel::roleNames() const
{
    return {
        { RoomIdRole,             "roomId" },
        { NameRole,               "name" },
        { TopicRole,              "topic" },
        { AvatarUrlRole,          "avatarUrl" },
        { LastMessagePreviewRole, "lastMessagePreview" },
        { LastActivityRole,       "lastActivity" },
        { UnreadCountRole,        "unreadCount" },
        { EncryptedRole,          "encrypted" },
        { IsSpaceRole,            "isSpace" },
        { MemberCountRole,        "memberCount" },
        { CategoryRole,           "category" },
        { HighlightCountRole,     "highlightCount" },
        { MarkedUnreadRole,       "markedUnread" },
        { HasUnreadRole,          "hasUnread" },
        { MembershipRole,         "membership" },
        { IsDirectRole,           "isDirect" },
        { DirectUserIdRole,       "directUserId" },
        { InviterRole,            "inviter" },
        { InvitePendingRole,      "invitePending" },
        { InviteErrorRole,        "inviteError" },
        { CanonicalAliasRole,     "canonicalAlias" },
        { IdentityColorKeyRole,   "identityColorKey" },
        { SuccessorRoomIdRole,    "successorRoomId" },
        { SupersededByAccessibleSuccessorRole,
          "supersededByAccessibleSuccessor" },
    };
}

QVariantMap RoomListModel::findRoom(const QString &roomId) const
{
    // Search the client's full room set so lookups do not depend on the
    // active Space filter.
    if (!m_client)
        return {};
    for (const auto &r : m_client->rooms()) {
        if (r.id == roomId) {
            return {
                { QStringLiteral("id"),        r.id },
                { QStringLiteral("name"),      r.name },
                { QStringLiteral("topic"),     r.topic },
                { QStringLiteral("avatarUrl"), effectiveAvatarUrl(r) },
                { QStringLiteral("encrypted"), r.encrypted },
                // Review H1: whether `encrypted` is a synced fact. The
                // find bar's History offer fails closed on false.
                { QStringLiteral("encryptionKnown"), r.encryptionKnown },
                { QStringLiteral("unreadCount"), r.unreadCount },
                { QStringLiteral("isSpace"),   r.isSpace },
                // v0.6.5: the invite dialog's room header prefers the
                // canonical alias over the raw id.
                { QStringLiteral("canonicalAlias"), r.canonicalAlias },
                // The room header binds currentRoom.isDirect for the
                // people-are-circles shape rule (and the composer for the
                // DM bubble layout); omitting it made every DM header
                // avatar render as a rounded square.
                { QStringLiteral("isDirect"),  r.isDirect },
                // One fallback-colour policy everywhere (see RoomInfo.h).
                { QStringLiteral("identityColorKey"), identityColorKey(r) },
            };
        }
    }
    return {};
}

QVariantList RoomListModel::recentRooms(int max) const
{
    // Immune to the mode filter (People/Rooms/Unreads): selecting a chip
    // in the room list must not reshape Home's "jump back in" strip, so
    // this iterates the authoritative client list with the scope filters
    // (space + search) only, re-sorted by activity.
    QList<RoomInfo> pool;
    if (m_client) {
        for (const auto &r : m_client->rooms()) {
            // Spaces render on the rail, not as conversations; invites and
            // left rooms are not somewhere to "jump back in".
            if (r.isSpace || r.membership != RoomInfo::Joined)
                continue;
            if (!passesScopeFilter(r))
                continue;
            pool.append(r);
        }
    }
    std::stable_sort(pool.begin(), pool.end(),
                     [](const RoomInfo &a, const RoomInfo &b) {
                         return a.lastActivity > b.lastActivity;
                     });
    QVariantList out;
    for (const auto &r : pool) {
        if (out.size() >= max)
            break;
        out.append(QVariantMap{
            { QStringLiteral("roomId"),      r.id },
            { QStringLiteral("name"),        r.name },
            { QStringLiteral("avatarUrl"),   effectiveAvatarUrl(r) },
            { QStringLiteral("isDirect"),    r.isDirect },
            { QStringLiteral("hasUnread"),   r.hasUnreadMessages },
            { QStringLiteral("unreadCount"), r.unreadCount },
            // v0.7 Home: activity recency and the mention badge.
            { QStringLiteral("lastActivity"), r.lastActivity },
            { QStringLiteral("highlightCount"), r.highlightCount },
            { QStringLiteral("identityColorKey"), identityColorKey(r) },
        });
    }
    return out;
}

QVariantList RoomListModel::spacesSummary(int max) const
{
    // Home's Spaces strip: joined Spaces in list order, presentation
    // fields only. The rail remains the authoritative Space navigation;
    // this is a shortcut surface.
    QVariantList out;
    for (const auto &r : m_rooms) {
        if (out.size() >= max)
            break;
        if (!r.isSpace || r.membership != RoomInfo::Joined)
            continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"),    r.id },
            { QStringLiteral("name"),      r.name },
            { QStringLiteral("avatarUrl"), r.avatarUrl },
        });
    }
    return out;
}

QString RoomListModel::effectiveAvatarUrl(const RoomInfo &room) const
{
    // Matrix room state always wins. An explicit room NAME must never
    // disable this — only an explicit room AVATAR does. Otherwise derive a
    // member avatar for a room authoritatively classified by m.direct, and
    // only when it is unambiguously a strict 1:1 (never an arbitrary face
    // for a group DM or our own profile).
    if (!room.avatarUrl.isEmpty())
        return room.avatarUrl;
    if (!room.isDirect || room.directUserId.isEmpty())
        return {};

    // The Rust backend never populates the per-room member snapshot below
    // (it is fetched separately, on demand, only for the Room Information
    // "People" tab) — it instead reports the authoritative m.direct target
    // list directly, which is exactly the "unambiguous 1:1" signal this
    // needs and requires no member fetch at all. Backends that only ever
    // populate `members` (Mock/HTTP) derive the same signal from there.
    if (!room.directUserIds.isEmpty()) {
        if (room.directUserIds.size() > 1)
            return {};
    } else if (m_client && !room.members.isEmpty()) {
        const QString self = m_client->currentUserId();
        QString other;
        for (auto it = room.members.cbegin(); it != room.members.cend(); ++it) {
            const QString userId = it.key().isEmpty() ? it->userId : it.key();
            if (userId.isEmpty() || userId == self)
                continue;
            if (!other.isEmpty() && other != userId)
                return {};
            other = userId;
        }
        if (other != room.directUserId)
            return {};
    }

    const auto member = room.members.constFind(room.directUserId);
    if (member != room.members.cend() && !member->avatarMxcUrl.isEmpty())
        return member->avatarMxcUrl;
    return m_profileAvatars.value(room.directUserId);
}

// Scope filters only (space-room exclusion, search, Space membership) —
// shared by the list filter and mode-immune surfaces like Home's recent
// strip.
bool RoomListModel::passesScopeFilter(const RoomInfo &r) const
{
    // Space rooms themselves belong to the Space chip row, not the room list.
    if (r.isSpace && r.membership == RoomInfo::Joined) return false;
    if (!m_searchQuery.isEmpty()
        && !r.name.contains(m_searchQuery, Qt::CaseInsensitive)
        && !r.canonicalAlias.contains(m_searchQuery, Qt::CaseInsensitive))
        return false;
    if (!m_spaces) return true;
    const QString active = m_spaces->activeSpaceId();
    if (active.isEmpty()) return true; // "All rooms"
    return m_spaces->includesRoom(active, r.id);
}

bool RoomListModel::passesFilter(const RoomInfo &r) const
{
    if (!passesScopeFilter(r))
        return false;
    // Element-style mode filter. Invites always pass — they need action
    // regardless of the selected view — and in Unreads mode the pinned
    // (open) room stays visible so reading it doesn't remove the row the
    // selection sits on.
    if (m_filterMode != 0 && r.membership != RoomInfo::Invited) {
        switch (m_filterMode) {
        case 1: // People
            if (!r.isDirect) return false;
            break;
        case 2: // Rooms
            if (r.isDirect) return false;
            break;
        case 3: // Unreads
            if (!(r.hasUnreadMessages || r.markedUnread
                  || r.highlightCount > 0 || r.id == m_pinnedRoomId))
                return false;
            break;
        default:
            break;
        }
    }
    return true;
}

void RoomListModel::setFilterMode(int mode)
{
    // Same convention as SettingsManager: an unknown value falls back to
    // All rather than snapping to the nearest edge.
    const int clamped = (mode < 0 || mode > 3) ? 0 : mode;
    if (clamped == m_filterMode)
        return;
    m_filterMode = clamped;
    // Same three-step sequence as the search and Space filter changes.
    ++m_filterGeneration;
    Q_EMIT filterGenerationChanged();
    reconcileRooms();
    Q_EMIT filterModeChanged();
}

void RoomListModel::setPinnedRoomId(const QString &roomId)
{
    if (roomId == m_pinnedRoomId)
        return;
    m_pinnedRoomId = roomId;
    // Only the Unreads view depends on the pin; skip the reconcile
    // otherwise (room switches are frequent).
    if (m_filterMode == 3) {
        ++m_filterGeneration;
        Q_EMIT filterGenerationChanged();
        reconcileRooms();
    }
}

void RoomListModel::setSearchQuery(const QString &query)
{
    if (query == m_pendingSearchQuery) return;
    m_pendingSearchQuery = query;
    m_searchDebounce.start();
}

QSet<QString> RoomListModel::computeSupersededRoomIds() const
{
    QSet<QString> superseded;
    if (!m_client)
        return superseded;
    const QList<RoomInfo> rooms = m_client->rooms();
    QHash<QString, const RoomInfo *> byId;
    byId.reserve(rooms.size());
    for (const RoomInfo &room : rooms)
        byId.insert(room.id, &room);
    for (const RoomInfo &room : rooms) {
        if (room.successorRoomId.isEmpty())
            continue;
        const RoomInfo *successor = byId.value(room.successorRoomId, nullptr);
        // Never heard of the successor: the user may well be able to join
        // it, but we have no evidence they can, and the maintainer's rule
        // is to de-emphasize only once it is ACTUALLY accessible. Leave the
        // row alone.
        if (!successor)
            continue;
        if (successor->membership != RoomInfo::Joined
            && successor->membership != RoomInfo::Invited) {
            continue;
        }
        // Defensive: the successor must point back at this room. A
        // tombstone naming a room that considers some OTHER room its
        // predecessor is not an established upgrade chain, and quietly
        // demoting the old room on its say-so would let a bad tombstone
        // bury a live room.
        if (successor->predecessorRoomId != room.id)
            continue;
        superseded.insert(room.id);
    }
    return superseded;
}

QList<RoomInfo> RoomListModel::desiredRooms(const QSet<QString> &superseded) const
{
    QList<RoomInfo> desired;
    if (m_client) {
        for (const auto &r : m_client->rooms()) {
            if (passesFilter(r))
                desired.append(r);
        }
        // Invitations are separate, followed by Matrix m.direct rooms and
        // normal joined rooms. Member count is deliberately irrelevant.
        // v0.7.x room upgrades: a room whose successor the user can reach
        // sorts BELOW every live room. Deliberately a demotion and not a
        // filter — the old room stays present, openable and readable, which
        // is the whole point of banner-and-link over auto-follow.
        // A superseded room sinks WITHIN its own group rather than below
        // every other room. RoomsPanel sections the list by the `category`
        // role (invite/dm/room), which until now corresponded 1:1 with these
        // groups, so each category was one contiguous run. A fourth top-level
        // group would be the first to mix categories, producing a second
        // "PEOPLE"/"ROOMS" header at the bottom of the list — and it would
        // demote a superseded INVITE out of the top block, breaking the
        // "invitations are separate" rule stated above.
        const auto groupOf = [](const RoomInfo &room) {
            return room.membership == RoomInfo::Invited ? 0 : (room.isDirect ? 1 : 2);
        };
        std::stable_sort(desired.begin(), desired.end(),
                         [&groupOf, &superseded](const RoomInfo &a, const RoomInfo &b) {
            const int aGroup = groupOf(a);
            const int bGroup = groupOf(b);
            if (aGroup != bGroup)
                return aGroup < bGroup;
            const bool aOld = superseded.contains(a.id);
            const bool bOld = superseded.contains(b.id);
            if (aOld != bOld)
                return bOld;
            return a.lastActivity > b.lastActivity;
        });
    }
    return desired;
}

void RoomListModel::refresh()
{
    // Structural change (roomsChanged/login): reconcile now and drop any
    // pending coalesced pass it supersedes.
    m_reconcileCoalesce.stop();
    reconcileRooms();
}

void RoomListModel::refreshRoom(const QString &roomId)
{
    Q_UNUSED(roomId);
    m_reconcileCoalesce.start();
}

void RoomListModel::onUserProfileFinished(quint64 opId, bool ok,
                                          const QString &userId,
                                          const QString &displayName,
                                          const QString &avatarUrl,
                                          const QString &category)
{
    Q_UNUSED(displayName);
    Q_UNUSED(category);
    // Always release the pending marker for the op that completed, keyed by
    // BOTH what we requested and what the SDK reports. The previous early
    // return whenever the requested and returned ids differed (SDK id
    // normalization) left the target stuck in m_profilePending forever, so
    // resolveMissingDirectAvatars never re-fetched it and the DM avatar was
    // wedged on initials.
    const QString requestedUser = m_profileOps.take(opId);
    if (!requestedUser.isEmpty())
        m_profilePending.remove(requestedUser);
    if (!userId.isEmpty())
        m_profilePending.remove(userId);

    // Cache the resolved avatar under the SDK's authoritative user id, and
    // accept results even for ops we did not start (every consumer shares the
    // client's userProfileFinished signal). This is what lets a self-DM row
    // — whose direct target is our OWN user id — adopt the signed-in
    // account's own avatar (fetched for the account switcher) instead of
    // resolving to an "M" initial forever, since the room list never sees the
    // per-event room-member avatar the timeline uses.
    if (ok && !userId.isEmpty() && !avatarUrl.isEmpty())
        m_profileAvatars.insert(userId, avatarUrl);
    if (userId.isEmpty())
        return;
    for (int row = 0; row < m_rooms.size(); ++row) {
        if (m_rooms.at(row).isDirect && m_rooms.at(row).directUserId == userId)
            Q_EMIT dataChanged(index(row), index(row), {AvatarUrlRole});
    }
}

void RoomListModel::resolveMissingDirectAvatars()
{
    if (!m_client)
        return;
    for (const RoomInfo &room : std::as_const(m_rooms)) {
        const QString userId = room.directUserId;
        if (!room.isDirect || !room.avatarUrl.isEmpty() || userId.isEmpty()
            || room.directUserIds.size() > 1 // ambiguous group-DM mapping
            || !effectiveAvatarUrl(room).isEmpty()
            || m_profileAvatars.contains(userId)
            || m_profilePending.contains(userId))
            continue;
        const quint64 opId = m_client->fetchUserProfile(userId);
        if (opId != 0) {
            m_profilePending.insert(userId);
            m_profileOps.insert(opId, userId);
        }
    }
}

void RoomListModel::reconcileRooms()
{
    // Recomputed BEFORE the rows are written so replaceRoom's dataChanged
    // carries the new value; data() reads this cache rather than deriving
    // it per row, which would be quadratic in the room count.
    const QSet<QString> previousSuperseded = m_supersededRoomIds;
    m_supersededRoomIds = computeSupersededRoomIds();
    // Passed in rather than recomputed: MatrixClient::rooms() materialises a
    // fresh QList on every call, and roomsChanged fires on every unread or
    // ordering change.
    const auto desired = desiredRooms(m_supersededRoomIds);
    QSet<QString> wanted;
    for (const auto &room : desired) wanted.insert(room.id);

    for (int i = m_rooms.size() - 1; i >= 0; --i) {
        if (!wanted.contains(m_rooms.at(i).id)) removeRoom(i);
    }

    for (int target = 0; target < desired.size(); ++target) {
        int current = -1;
        for (int i = target; i < m_rooms.size(); ++i) {
            if (m_rooms.at(i).id == desired.at(target).id) { current = i; break; }
        }
        if (current < 0) {
            insertRoom(target, desired.at(target));
        } else if (current != target) {
            beginMoveRows({}, current, current, {}, current < target ? target + 1 : target);
            m_rooms.move(current, target);
            endMoveRows();
        }
        replaceRoom(target, desired.at(target));
    }
    truncate(desired.size());

    // A row's superseded state can flip without that room's own RoomInfo
    // changing at all — it flips when the SUCCESSOR is joined, or when the
    // successor's predecessor link finally arrives. replaceRoom compares
    // RoomInfo and would skip the dataChanged for exactly those rows, so
    // the chip would not appear until something unrelated about the old
    // room changed. Notify the difference explicitly.
    if (previousSuperseded != m_supersededRoomIds) {
        for (int i = 0; i < m_rooms.size(); ++i) {
            const QString &id = m_rooms.at(i).id;
            if (previousSuperseded.contains(id) == m_supersededRoomIds.contains(id))
                continue;
            const QModelIndex idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx, { SupersededByAccessibleSuccessorRole });
        }
    }

    resolveMissingDirectAvatars();
}

void RoomListModel::resetRooms(const QList<RoomInfo> &rooms)
{
    beginResetModel(); m_rooms = rooms; endResetModel();
}

bool RoomListModel::appendRooms(const QList<RoomInfo> &rooms)
{
    if (rooms.isEmpty()) return true;
    const int first = m_rooms.size();
    beginInsertRows({}, first, first + rooms.size() - 1);
    m_rooms.append(rooms); endInsertRows(); return true;
}

bool RoomListModel::prependRooms(const QList<RoomInfo> &rooms)
{
    if (rooms.isEmpty()) return true;
    beginInsertRows({}, 0, rooms.size() - 1);
    for (int i = rooms.size() - 1; i >= 0; --i) m_rooms.prepend(rooms.at(i));
    endInsertRows(); return true;
}

bool RoomListModel::insertRoom(int row, const RoomInfo &room)
{
    if (row < 0 || row > m_rooms.size() || room.id.isEmpty()) return false;
    for (const auto &existing : m_rooms) if (existing.id == room.id) return false;
    beginInsertRows({}, row, row); m_rooms.insert(row, room); endInsertRows(); return true;
}

bool RoomListModel::replaceRoom(int row, const RoomInfo &room)
{
    if (row < 0 || row >= m_rooms.size() || room.id.isEmpty()) return false;
    if (m_rooms.at(row).id != room.id) return false;
    // Unchanged rows emit nothing: reconcileRooms() calls this for EVERY
    // row on EVERY room update, and an unconditional roleless dataChanged
    // made each incoming message re-evaluate every delegate binding of
    // every visible room (including avatar sources).
    if (m_rooms.at(row) == room) return true;
    m_rooms[row] = room; Q_EMIT dataChanged(index(row), index(row)); return true;
}

bool RoomListModel::removeRoom(int row)
{
    return removeRange(row, 1);
}

bool RoomListModel::removeRange(int row, int count)
{
    if (row < 0 || count < 0 || row + count > m_rooms.size()) return false;
    if (count == 0) return true;
    beginRemoveRows({}, row, row + count - 1);
    m_rooms.remove(row, count); endRemoveRows(); return true;
}

bool RoomListModel::truncate(int length)
{
    if (length < 0 || length > m_rooms.size()) return false;
    return removeRange(length, m_rooms.size() - length);
}

void RoomListModel::clearRooms()
{
    removeRange(0, m_rooms.size());
}

void RoomListModel::acceptInvite(const QString &roomId)
{
    if (m_client) m_client->acceptInvite(roomId);
}

void RoomListModel::rejectInvite(const QString &roomId)
{
    if (m_client) m_client->rejectInvite(roomId);
}

void RoomListModel::markRoomRead(const QString &roomId)
{
    if (!m_client || roomId.isEmpty()) return;
    // Prefer the backend that can resolve the room's latest event WITHOUT a
    // loaded timeline. The fallback below reads m_client->timeline(roomId),
    // which on the Rust backend only ever holds the OPEN room — so marking
    // any other room read silently did nothing at all.
    if (m_client->supportsMarkRoomRead()) {
        m_client->markRoomRead(roomId);
        return;
    }
    for (const auto &room : m_client->rooms()) {
        if (room.id != roomId) continue;
        const auto events = m_client->timeline(roomId);
        for (auto it = events.crbegin(); it != events.crend(); ++it) {
            if (!it->eventId.isEmpty() && !it->eventId.startsWith(QLatin1String("local:"))) {
                m_client->sendReadReceipt(roomId, it->eventId);
                break;
            }
        }
        return;
    }
}


void RoomListModel::markRoomUnread(const QString &roomId)
{
    if (m_client) m_client->setRoomMarkedUnread(roomId, true);
}

QString RoomListModel::roomPermalink(const QString &roomId,
                                     const QString &canonicalAlias)
{
    // Pure formatting: prefer the canonical alias over the bare room id, and
    // reuse TimelineModel::messagePermalink's exact percent-encoding
    // convention (! $ : @ excluded) so message and room links look
    // consistent throughout the app. No server behavior.
    const QString target = !canonicalAlias.isEmpty() ? canonicalAlias : roomId;
    if (target.isEmpty())
        return {};
    return QStringLiteral("https://matrix.to/#/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(
            target, QByteArrayLiteral("!$:@"))));
}
