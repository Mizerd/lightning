#include "models/RoomListModel.h"

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
    const auto boundary = [this] { updateFavouritesBoundary(); };
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
    // A DIRECT MESSAGE IS NEVER SCOPED BY A SPACE — in any filter.
    //
    // A DM is not a room in a Space. Matrix has no notion of one belonging to
    // a Space unless somebody adds it as an m.space.child, which essentially
    // nobody does, so scoping DMs by the selected Space hid every one of them
    // in every Space. That was reported first as "the people tab in
    // spaces/rooms isnt populated".
    //
    // Exempting only the People chip fixed that list and broke a bigger one:
    // All then showed FEWER rooms than People did, which makes "All" a lie.
    // So the exemption belongs to the DM, not to the chip. Rooms still has no
    // DMs in it — passesFilter excludes them there by their own isDirect —
    // and All, People and Unreads all show them.
    //
    // Scoping DMs by the Space's MEMBERSHIP instead (Element's reading: DMs
    // with people who are in this Space) would need that Space's roster, and
    // this predicate runs on every row of every model update — a lazily
    // fetched roster would make the list's contents depend on load state and
    // flicker as it arrived.
    if (r.isDirect)
        return true;
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
        // Invitations are separate, then favourites, then Matrix m.direct
        // rooms, then normal joined rooms — groupIndexOf() owns that order
        // and the `category` role reads the same function. Member count is
        // deliberately irrelevant.
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
        std::stable_sort(desired.begin(), desired.end(),
                         [&superseded](const RoomInfo &a, const RoomInfo &b) {
            const int aGroup = groupIndexOf(a);
            const int bGroup = groupIndexOf(b);
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

// The section a row belongs to, and the ONLY classification in this file.
//
// RoomsPanel opens one header per contiguous run of the `category` role, so
// desiredRooms() has to sort by exactly this classification: an ordering
// that disagrees with the string splits a category into two runs and the
// list grows a second "PEOPLE" header further down. Both callers read this
// one function so they cannot drift apart.
int RoomListModel::groupIndexOf(const RoomInfo &room)
{
    // Invitations stay first — they need action, and a room the user has not
    // joined cannot carry their tags anyway.
    if (room.membership == RoomInfo::Invited)
        return 0;
    // A favourite outranks its own kind, as in Element classic: a
    // favourited DM appears under Favourites and NOT also under People.
    if (room.isFavourite)
        return 1;
    return room.isDirect ? 2 : 3;
}

QString RoomListModel::categoryOf(const RoomInfo &room)
{
    switch (groupIndexOf(room)) {
    case 0:  return QStringLiteral("invite");
    case 1:  return QStringLiteral("favourite");
    case 2:  return QStringLiteral("dm");
    default: return QStringLiteral("room");
    }
}

void RoomListModel::updateFavouritesBoundary()
{
    // groupIndexOf == 1 is the favourites group (0 invites, 2 DMs, 3 rooms),
    // and the list is already sorted by it, so the boundary is the last
    // favourite that still has a row after it. No trailing row means no
    // rule: a divider hanging off the bottom of the list divides nothing.
    QString boundary;
    for (int i = 0; i < m_rooms.size(); ++i) {
        if (groupIndexOf(m_rooms.at(i)) != 1)
            continue;
        boundary = (i + 1 < m_rooms.size()) ? m_rooms.at(i).id : QString();
    }
    if (boundary == m_favouritesBoundaryRoomId)
        return;
    m_favouritesBoundaryRoomId = boundary;
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
