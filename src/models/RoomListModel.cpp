#include "models/RoomListModel.h"

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

#include <algorithm>
#include <QSet>

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
                this, [this] {
                    m_profileAvatars.clear();
                    m_profileOps.clear();
                    m_profilePending.clear();
                    refresh();
                });
    }
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
                { QStringLiteral("unreadCount"), r.unreadCount },
                { QStringLiteral("isSpace"),   r.isSpace },
            };
        }
    }
    return {};
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

bool RoomListModel::passesFilter(const RoomInfo &r) const
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

void RoomListModel::setSearchQuery(const QString &query)
{
    if (query == m_pendingSearchQuery) return;
    m_pendingSearchQuery = query;
    m_searchDebounce.start();
}

QList<RoomInfo> RoomListModel::desiredRooms() const
{
    QList<RoomInfo> desired;
    if (m_client) {
        for (const auto &r : m_client->rooms()) {
            if (passesFilter(r))
                desired.append(r);
        }
        // Invitations are separate, followed by Matrix m.direct rooms and
        // normal joined rooms. Member count is deliberately irrelevant.
        std::stable_sort(desired.begin(), desired.end(), [](const RoomInfo &a, const RoomInfo &b) {
            const int aGroup = a.membership == RoomInfo::Invited ? 0 : (a.isDirect ? 1 : 2);
            const int bGroup = b.membership == RoomInfo::Invited ? 0 : (b.isDirect ? 1 : 2);
            if (aGroup != bGroup)
                return aGroup < bGroup;
            return a.lastActivity > b.lastActivity;
        });
    }
    return desired;
}

void RoomListModel::refresh()
{
    reconcileRooms();
}

void RoomListModel::refreshRoom(const QString &roomId)
{
    Q_UNUSED(roomId);
    reconcileRooms();
}

void RoomListModel::onUserProfileFinished(quint64 opId, bool ok,
                                          const QString &userId,
                                          const QString &displayName,
                                          const QString &avatarUrl,
                                          const QString &category)
{
    Q_UNUSED(displayName);
    Q_UNUSED(category);
    const QString requestedUser = m_profileOps.take(opId);
    if (requestedUser.isEmpty() || requestedUser != userId)
        return;
    m_profilePending.remove(userId);
    if (ok && !avatarUrl.isEmpty())
        m_profileAvatars.insert(userId, avatarUrl);
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
    const auto desired = desiredRooms();
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
    if (!m_client) return;
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
