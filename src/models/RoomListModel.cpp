#include "models/RoomListModel.h"

#include "models/ConversationOrder.h"

#include "matrix/BridgeNetwork.h"
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

    connect(&m_directAvatars, &DirectAvatarResolver::avatarResolved,
            this, &RoomListModel::onDirectAvatarResolved);

    // The favourites group boundary is derived from the CURRENT rows, and
    // this model mutates through eight different entry points (reset,
    // append, prepend, insert, replace, move, remove, truncate). Hooking the
    // model's own change signals covers all of them at once, and cannot be
    // forgotten by the ninth. Every path ends in one of these.
    const auto boundary = [this] {
        updateFavouritesBoundary();
        updateUnreadTotals();
    };
    connect(this, &QAbstractItemModel::modelReset, this, boundary);
    connect(this, &QAbstractItemModel::rowsInserted, this, boundary);
    connect(this, &QAbstractItemModel::rowsRemoved, this, boundary);
    connect(this, &QAbstractItemModel::rowsMoved, this, boundary);
    connect(this, &QAbstractItemModel::layoutChanged, this, boundary);
    // A favourite can be added or dropped without the row moving at all
    // (the tag write lands before the re-sort), so the data signal counts
    // too. The scan is a switch over a few hundred structs at most.
    connect(this, &QAbstractItemModel::dataChanged, this, boundary);
}

void RoomListModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_directAvatars.setClient(m_client);
    if (m_client) {
        connect(m_client, &MatrixClient::roomsChanged,
                this, &RoomListModel::refresh);
        connect(m_client, &MatrixClient::roomUpdated,
                this, &RoomListModel::refreshRoom);
        connect(m_client, &MatrixClient::membersChanged,
                this, &RoomListModel::refreshRoom);
        connect(m_client, &MatrixClient::loginSucceeded,
                this, [this](const QString &) { refresh(); });
        connect(m_client, &MatrixClient::loggedOut,
                this, &RoomListModel::clearProfileCaches);
    }
    // The capability is a property of the backend, so swapping the client
    // (backend selection, account switch) is the one moment it can change.
    Q_EMIT roomFavouritesSupportedChanged();
    refresh();
}

void RoomListModel::clearProfileCaches()
{
    m_directAvatars.clear();
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
        // A Space's roster arriving changes which DMs the People scope
        // admits. Only the ACTIVE Space can move a row — the filter reads no
        // other — so a roster for anywhere else costs nothing here.
        connect(m_spaces, &SpaceManager::spaceRosterChanged, this,
                [this](const QString &spaceId) {
            if (!m_spaces || spaceId != m_spaces->activeSpaceId())
                return;
            ++m_filterGeneration;
            Q_EMIT filterGenerationChanged();
            reconcileRooms();
        });
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
    case NameRole: {
        // Bridged DMs only: repair the ghost-localpart degradation and the
        // hero arithmetic (see BridgeNetwork::presentableDmName). A native
        // room's directUserId yields no network and passes through as-is.
        if (r.isDirect && !r.directUserId.isEmpty()) {
            const auto dm =
                matrix::bridge::presentableDmName(r.name, r.directUserId);
            if (!dm.name.isEmpty())
                return dm.name;
            if (!dm.networkLabel.isEmpty())
                //: A bridged chat partner with no usable name yet;
                //: %1 is the network, e.g. "WhatsApp contact".
                return tr("%1 contact").arg(dm.networkLabel);
        }
        return r.name;
    }
    case TopicRole:              return r.topic;
    case AvatarUrlRole:          return effectiveAvatarUrl(r);
    case LastMessagePreviewRole: return r.lastMessagePreview;
    case LastActivityRole:       return r.lastActivity;
    case UnreadCountRole:        return r.unreadCount;
    case EncryptedRole:          return r.encrypted;
    case IsSpaceRole:            return r.isSpace;
    case MemberCountRole:        return static_cast<int>(r.members.size());
    case CategoryRole:           return categoryOf(r);
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
    case IsFavouriteRole:        return r.isFavourite;
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
    case NetworkRole:
        return matrix::bridge::networkIdForRoom(r.directUserId,
                                                r.canonicalAlias);
    case NetworkLabelRole:
        return matrix::bridge::labelForNetworkId(
            matrix::bridge::networkIdForRoom(r.directUserId,
                                             r.canonicalAlias));
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
        { IsFavouriteRole,        "isFavourite" },
        { DirectUserIdRole,       "directUserId" },
        { InviterRole,            "inviter" },
        { InvitePendingRole,      "invitePending" },
        { InviteErrorRole,        "inviteError" },
        { CanonicalAliasRole,     "canonicalAlias" },
        { IdentityColorKeyRole,   "identityColorKey" },
        { SuccessorRoomIdRole,    "successorRoomId" },
        { SupersededByAccessibleSuccessorRole,
          "supersededByAccessibleSuccessor" },
        { NetworkRole,            "network" },
        { NetworkLabelRole,       "networkLabel" },
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
                         return conversation::moreRecent(
                             a.lastActivity, a.name, a.id,
                             b.lastActivity, b.name, b.id);
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
    // The derivation itself is DirectAvatarResolver's — the Channels column
    // needs exactly the same answer, and a DM avatar rule that exists twice is
    // a DM avatar rule that will disagree with itself.
    return m_directAvatars.avatarFor(room);
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
    // A DIRECT MESSAGE IS NEVER A SPACE'S CHILD, so it is scoped by the
    // Space's PEOPLE or not at all.
    //
    // Matrix has no notion of a DM belonging to a Space unless somebody adds
    // it as an m.space.child, which essentially nobody does. Scoping DMs by
    // the hierarchy therefore hid every one of them in every Space — reported
    // as "the people tab in spaces/rooms isnt populated" — and exempting only
    // the People CHIP fixed that list while breaking a bigger one, because
    // All then showed FEWER rooms than People did. Both of those failures
    // came from asking the wrong question of a DM. The right one, and
    // Element's, is whether the person you are talking to is IN this Space,
    // and SpaceManager::directScope is the single place it is answered.
    //
    // WHAT AN UNKNOWN ROSTER MEANS HERE. This is a REMOVAL filter over a list
    // that already shows the row, so unknown must FAIL OPEN: while the roster
    // is unfetched, in flight, truncated or failed, every DM stays exactly
    // where it was. A list that empties itself while it waits for an answer
    // is the original bug wearing a timer. Because the answer is applied in
    // the SCOPE predicate rather than in the People case, All stays exactly
    // People plus Rooms whichever way the roster lands.
    //
    // Applies to real Spaces only. "@orphans" and "@people" are views, not
    // containers, and they keep showing every DM.
    if (r.isDirect) {
        if (!SpaceManager::isRealSpaceId(active))
            return true;
        // directUserIds is the authoritative m.direct target list; the
        // singular field is the fallback for backends that only fill it.
        QStringList peers = r.directUserIds;
        if (peers.isEmpty() && !r.directUserId.isEmpty())
            peers.append(r.directUserId);
        return m_spaces->directScope(active, peers) != 0;
    }
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
        // ONE ACTIVITY FEED. Invitations stay a block at the top because
        // they need action; everything joined below them is ordered purely by
        // when somebody last spoke, with direct messages and rooms
        // interleaved.
        //
        // This used to sort by category first — invites, favourites, DMs,
        // rooms — so a room that had just received a message sat below every
        // person the user had ever spoken to, and reaching it meant scrolling
        // past the entire People section. Favourites made it worse: starring
        // a conversation froze it above live traffic forever.
        //
        // A favourite keeps its star and everything else it does; what it no
        // longer gets is RANK, because a list sorted by importance-someone-
        // declared-once is not a list of what is happening now.
        //
        // v0.7.x room upgrades: a room whose successor the user can reach
        // sorts BELOW every live room of the same rank. Deliberately a
        // demotion and not a filter — the old room stays present, openable
        // and readable, which is the whole point of banner-and-link over
        // auto-follow. It is applied before recency so a superseded room
        // cannot outrank a live one by having been busy.
        std::stable_sort(desired.begin(), desired.end(),
                         [&superseded](const RoomInfo &a, const RoomInfo &b) {
            const int aRank = orderRankOf(a);
            const int bRank = orderRankOf(b);
            if (aRank != bRank)
                return aRank < bRank;
            const bool aOld = superseded.contains(a.id);
            const bool bOld = superseded.contains(b.id);
            if (aOld != bOld)
                return bOld;
            return conversation::moreRecent(a.lastActivity, a.name, a.id,
                                            b.lastActivity, b.name, b.id);
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

void RoomListModel::onDirectAvatarResolved(const QString &userId)
{
    for (int row = 0; row < m_rooms.size(); ++row) {
        if (m_rooms.at(row).isDirect && m_rooms.at(row).directUserId == userId)
            Q_EMIT dataChanged(index(row), index(row), {AvatarUrlRole});
    }
}

void RoomListModel::resolveMissingDirectAvatars()
{
    m_directAvatars.resolveMissing(m_rooms);
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

// Where a row sits in the list's top-level order, and the ONLY classification
// in this file.
//
// There are exactly two ranks, and that is the point. Invitations need action
// and are held at the top; everything else is one activity feed in which a
// direct message and a room compete on nothing but recency. The category role
// below is derived from this same function, so the sort and the section
// headers cannot disagree — RoomsPanel opens one header per contiguous run of
// `category`, and an ordering that disagreed with the string would split a
// section in two and grow a second "PEOPLE" header further down the list.
int RoomListModel::orderRankOf(const RoomInfo &room)
{
    // Invitations stay first — they need action, and a room the user has not
    // joined cannot carry their tags anyway.
    if (room.membership == RoomInfo::Invited)
        return 0;
    return 1;
}

QString RoomListModel::categoryOf(const RoomInfo &room)
{
    // Two values, because there are two ranks. "conversation" covers DMs and
    // rooms alike: they are interleaved by recency, so any finer split would
    // repeat its header every time the two kinds alternate — which, in a list
    // ordered by when people spoke, is constantly.
    //
    // The label a user reads is chosen by the presenter from the active
    // filter (People / Rooms / Unread / everything), because that is what the
    // section actually contains; the model does not need to know.
    return orderRankOf(room) == 0 ? QStringLiteral("invite")
                                  : QStringLiteral("conversation");
}

void RoomListModel::updateUnreadTotals()
{
    // The WHOLE account, not the current view. A filter chip or a Space
    // selection changes what is listed; it does not change how many
    // conversations are waiting, and a title that dropped to zero because
    // the user picked "Rooms" would be lying about their DMs.
    //
    // hasUnreadMessages OR markedUnread OR a count, because on Matrix those
    // disagree constantly: a room can carry unread messages with
    // notification_count 0 (its push rules generate no notification), and
    // keying a badge on the count alone is how an unread direct message
    // ended up showing nothing at all.
    int unread = 0;
    int highlight = 0;
    if (m_client) {
        for (const RoomInfo &room : m_client->rooms()) {
            if (room.isSpace || room.membership != RoomInfo::Joined)
                continue;
            if (room.highlightCount > 0)
                ++highlight;
            if (room.hasUnreadMessages || room.markedUnread
                || room.unreadCount > 0 || room.highlightCount > 0) {
                ++unread;
            }
        }
    }
    if (unread == m_unreadRoomCount && highlight == m_highlightRoomCount)
        return;
    m_unreadRoomCount = unread;
    m_highlightRoomCount = highlight;
    Q_EMIT unreadTotalsChanged();
}

void RoomListModel::updateFavouritesBoundary()
{
    // RETIRED, deliberately, and kept as a no-op rather than removed so the
    // property stays bound and every consumer keeps working.
    //
    // The divider marked the end of the favourites GROUP, and there is no
    // such group any more: favourites are interleaved with everything else by
    // recency, so the rows it used to separate are no longer adjacent and a
    // line drawn anywhere in the feed would divide nothing. Favouriting still
    // works and still shows its star; it simply no longer buys rank.
    if (m_favouritesBoundaryRoomId.isEmpty())
        return;
    m_favouritesBoundaryRoomId.clear();
    Q_EMIT favouritesBoundaryRoomIdChanged();
}

bool RoomListModel::roomFavouritesSupported() const
{
    return m_client && m_client->supportsRoomFavourites();
}

bool RoomListModel::isRoomFavourite(const QString &roomId) const
{
    if (!m_client)
        return false;
    for (const auto &r : m_client->rooms()) {
        if (r.id == roomId)
            return r.isFavourite;
    }
    return false;
}

void RoomListModel::setRoomFavourite(const QString &roomId, bool favourite)
{
    // Deliberately no local write: the flag is account state, and the row
    // must keep showing what the ACCOUNT holds until the backend confirms
    // the tag changed. A refused write therefore leaves the row where it is
    // instead of parking it under a Favourites header it does not belong in.
    if (m_client) m_client->setRoomFavourite(roomId, favourite);
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
