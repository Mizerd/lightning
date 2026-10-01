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
        m_hold.release();
        reconcileRooms();
    });
    // Per-room update signals (roomUpdated per event, membersChanged per
    // roster) coalesce onto one zero-timer reconcile per event-loop turn.
    m_reconcileCoalesce.setSingleShot(true);
    m_reconcileCoalesce.setInterval(0);
    connect(&m_reconcileCoalesce, &QTimer::timeout,
            this, &RoomListModel::reconcileRooms);

    connect(&m_directAvatars, &DirectAvatarResolver::avatarResolved,
            this, &RoomListModel::onDirectAvatarResolved);

    // A held order is never allowed to go stale for ever.
    m_holdCap.setSingleShot(true);
    m_holdCap.setInterval(kHoldCapMs);
    connect(&m_holdCap, &QTimer::timeout, this, &RoomListModel::releaseOrder);

    // The favourites boundary derives from the current rows, and the model
    // mutates through many entry points; hooking its own change signals covers
    // all of them.
    const auto boundary = [this] {
        updateFavouritesBoundary();
        updateUnreadTotals();
    };
    connect(this, &QAbstractItemModel::modelReset, this, boundary);
    connect(this, &QAbstractItemModel::rowsInserted, this, boundary);
    connect(this, &QAbstractItemModel::rowsRemoved, this, boundary);
    connect(this, &QAbstractItemModel::rowsMoved, this, boundary);
    connect(this, &QAbstractItemModel::layoutChanged, this, boundary);
    // A favourite can change without the row moving (the tag lands before the
    // re-sort), so data changes count too.
    connect(this, &QAbstractItemModel::dataChanged, this, boundary);
}

void RoomListModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    // Another client's rooms are not these rooms: nothing to hold.
    m_hold.release();
    m_directAvatars.setClient(m_client);
    clearAdvertisedBridges();
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
    // The capability belongs to the backend, so it can only change when the
    // client is swapped.
    Q_EMIT roomFavouritesSupportedChanged();
    refresh();
}

void RoomListModel::clearProfileCaches()
{
    m_directAvatars.clear();
    // Bridge answers are keyed by room id and belong to the previous account.
    clearAdvertisedBridges();
    m_hold.release();
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
            // A different Space is a different list: order it fresh.
            m_hold.release();
            reconcileRooms();
        });
        connect(m_spaces, &SpaceManager::spacesChanged,
                this, &RoomListModel::refresh);
        // A Space roster changes which DMs the People scope admits. Only the
        // active Space affects the filter.
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
    // Both roles are synchronous and fetch nothing; the MSC2346 read that fills
    // the hash is user-driven (see setAdvertisedBridge).
    case NetworkRole:            return badgeFor(r).networkId;
    case NetworkLabelRole:       return badgeFor(r).label;
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
    // Search the client's full room set so lookups ignore the active Space
    // filter.
    if (!m_client)
        return {};
    for (const auto &r : m_client->rooms()) {
        if (r.id == roomId) {
            const BridgeBadge badge = badgeFor(r);
            return {
                { QStringLiteral("id"),        r.id },
                { QStringLiteral("name"),      r.name },
                { QStringLiteral("topic"),     r.topic },
                { QStringLiteral("avatarUrl"), effectiveAvatarUrl(r) },
                { QStringLiteral("encrypted"), r.encrypted },
                // Whether `encrypted` is a synced fact; the find bar's History
                // offer fails closed on false.
                { QStringLiteral("encryptionKnown"), r.encryptionKnown },
                { QStringLiteral("unreadCount"), r.unreadCount },
                { QStringLiteral("isSpace"),   r.isSpace },
                // The invite dialog's room header prefers the canonical alias.
                { QStringLiteral("canonicalAlias"), r.canonicalAlias },
                // The room header uses isDirect for circular avatars, and the
                // composer for the DM bubble layout.
                { QStringLiteral("isDirect"),  r.isDirect },
                // Every m.direct target; the legacy call lane needs to know
                // whether "direct" means exactly one other person.
                { QStringLiteral("directUserIds"), r.directUserIds },
                // One fallback-colour policy everywhere (see RoomInfo.h).
                { QStringLiteral("identityColorKey"), identityColorKey(r) },
                // The bridge badge the row shows, so the room info panel agrees
                // with it.
                { QStringLiteral("bridgeNetwork"), badge.networkId },
                { QStringLiteral("bridgeLabel"),   badge.label },
            };
        }
    }
    return {};
}

QVariantList RoomListModel::recentRooms(int max) const
{
    // Immune to the mode filter so a chip never reshapes Home's "jump back in"
    // strip: the client's list with scope filters (space + search) only,
    // re-sorted by activity.
    QList<RoomInfo> pool;
    if (m_client) {
        for (const auto &r : m_client->rooms()) {
            // Spaces belong to the rail; invites and left rooms are not
            // somewhere to "jump back in".
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
            // Home: activity recency and the mention badge.
            { QStringLiteral("lastActivity"), r.lastActivity },
            { QStringLiteral("highlightCount"), r.highlightCount },
            { QStringLiteral("identityColorKey"), identityColorKey(r) },
        });
    }
    return out;
}

QVariantList RoomListModel::spacesSummary(int max) const
{
    // Home's Spaces strip: joined Spaces in list order, presentation fields
    // only; the rail remains the authoritative navigation. Iterates the client,
    // not `m_rooms`, because passesScopeFilter() drops joined Spaces from
    // `m_rooms`.
    QVariantList out;
    if (!m_client)
        return out;
    for (const auto &r : m_client->rooms()) {
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
    // Derived by DirectAvatarResolver, shared with the Channels column so the
    // two agree.
    return m_directAvatars.avatarFor(room);
}

// Scope filters only (space-room exclusion, search, Space membership),
// shared by the list filter and mode-immune surfaces like Home's recent
// strip.
bool RoomListModel::passesScopeFilter(const RoomInfo &r) const
{
    // Space rooms themselves belong to the rail, not the room list.
    if (r.isSpace && r.membership == RoomInfo::Joined) return false;
    if (!m_searchQuery.isEmpty()
        && !r.name.contains(m_searchQuery, Qt::CaseInsensitive)
        && !r.canonicalAlias.contains(m_searchQuery, Qt::CaseInsensitive))
        return false;
    if (!m_spaces) return true;
    const QString active = m_spaces->activeSpaceId();
    if (active.isEmpty()) return true; // "All rooms"
    // A DM is never a Space's child, so it is scoped by the Space's people:
    // whether the other person is in this Space (SpaceManager::directScope), as
    // Element does.
    //
    // This is a removal filter over rows already shown, so an unknown roster
    // (unfetched, in flight, truncated, failed) fails open and every DM stays.
    // Applying it in the scope predicate keeps All equal to People plus Rooms.
    // Real Spaces only; "@orphans" and "@people" show every DM.
    if (r.isDirect) {
        if (!SpaceManager::isRealSpaceId(active))
            return true;
        // directUserIds is the authoritative m.direct list; the singular field
        // is the fallback for backends that only fill it.
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
    // Element-style mode filter. Invites always pass (they need action), and in
    // Unreads mode the pinned (open) room stays so reading it does not remove
    // the selected row.
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
    // As in SettingsManager: an unknown value falls back to All rather than the
    // nearest edge.
    const int clamped = (mode < 0 || mode > 3) ? 0 : mode;
    if (clamped == m_filterMode)
        return;
    m_filterMode = clamped;
    // Same sequence as the search and Space filter changes.
    ++m_filterGeneration;
    Q_EMIT filterGenerationChanged();
    // A different tab is a different list: order it fresh.
    m_hold.release();
    reconcileRooms();
    Q_EMIT filterModeChanged();
}

void RoomListModel::setPinnedRoomId(const QString &roomId)
{
    if (roomId == m_pinnedRoomId)
        return;
    m_pinnedRoomId = roomId;
    // Opening a room must not move it for a stamp that predates the open (see
    // RecencyHold). The baseline is what the room sorts by right now.
    QDateTime liveNow;
    if (m_client && !roomId.isEmpty()) {
        for (const RoomInfo &r : m_client->rooms()) {
            if (r.id == roomId) {
                liveNow = r.lastActivity;
                break;
            }
        }
    }
    // Moving to another room is the user looking away from the list, which is
    // when a held order is applied (Element does the same).
    const bool hadHold = m_hold.enabled();
    m_hold.release();
    m_hold.setOpenRoom(roomId, liveNow);
    if (m_filterMode == 3) {
        ++m_filterGeneration;
        Q_EMIT filterGenerationChanged();
    }
    if (hadHold || m_filterMode == 3)
        reconcileRooms();
}

void RoomListModel::setDeferReordering(bool defer)
{
    if (m_hold.enabled() == defer)
        return;
    m_hold.setEnabled(defer);
    Q_EMIT deferReorderingChanged();
    // On: take the snapshot of the order on screen. Off: back to live now.
    reconcileRooms();
}

void RoomListModel::setSortMode(int mode)
{
    const int valid = conversation::normalizedSortMode(mode);
    if (valid == m_sortMode)
        return;
    m_sortMode = valid;
    // A held order belongs to the old mode; drop it so the new one shows now.
    m_hold.release();
    reconcileRooms();
    Q_EMIT sortModeChanged();
}

void RoomListModel::releaseOrder()
{
    m_hold.release();
    reconcileRooms();
}

void RoomListModel::setHoldCapMs(int ms)
{
    m_holdCap.setInterval(ms);
}

void RoomListModel::setOrderHeld(bool held)
{
    if (held) {
        // Armed once per stale spell, not restarted by every message.
        if (m_holdCap.interval() > 0 && !m_holdCap.isActive())
            m_holdCap.start();
    } else {
        m_holdCap.stop();
    }
    if (m_orderHeld == held)
        return;
    m_orderHeld = held;
    Q_EMIT orderHeldChanged();
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
        // Unknown successor: no evidence the user can reach it, so do not
        // de-emphasize the row.
        if (!successor)
            continue;
        if (successor->membership != RoomInfo::Joined
            && successor->membership != RoomInfo::Invited) {
            continue;
        }
        // The successor must point back at this room; otherwise a bad tombstone
        // could bury a live room.
        if (successor->predecessorRoomId != room.id)
            continue;
        superseded.insert(room.id);
    }
    return superseded;
}

QList<RoomInfo> RoomListModel::desiredRooms(const QSet<QString> &superseded)
{
    QList<RoomInfo> desired;
    m_sortPending = false;
    if (m_client) {
        const QList<RoomInfo> all = m_client->rooms();
        QSet<QString> everyId;
        everyId.reserve(all.size());
        for (const auto &r : all) {
            everyId.insert(r.id);
            if (passesFilter(r))
                desired.append(r);
        }
        m_hold.retainOnly(everyId);
        if (m_sortMode == conversation::SortByName) {
            // Same ranks, same demotion of a superseded room; only the order
            // inside a rank changes. Nothing here depends on activity, so
            // there is nothing to hold and no pending re-sort.
            const QCollator collator = conversation::makeNameCollator();
            std::stable_sort(
                desired.begin(), desired.end(),
                [&superseded, &collator](const RoomInfo &a, const RoomInfo &b) {
                    const int aRank = orderRankOf(a);
                    const int bRank = orderRankOf(b);
                    if (aRank != bRank)
                        return aRank < bRank;
                    const bool aOld = superseded.contains(a.id);
                    const bool bOld = superseded.contains(b.id);
                    if (aOld != bOld)
                        return bOld;
                    return conversation::byName(collator, a.name, a.id,
                                                b.name, b.id);
                });
            return desired;
        }
        // The stamp each room is ordered by (held, when holding) and the one
        // it would have if nothing were held. Only the order is held: the
        // rows written below carry the live RoomInfo.
        QHash<QString, QDateTime> key;
        QHash<QString, QDateTime> target;
        key.reserve(desired.size());
        target.reserve(desired.size());
        for (const auto &r : desired) {
            key.insert(r.id, m_hold.keyFor(r.id, r.lastActivity));
            target.insert(r.id, m_hold.target(r.id, r.lastActivity));
        }
        // Invitations first, then favourites under their own header, then one
        // activity feed of DMs and rooms interleaved by recency. Within each
        // rank the order is recency.
        //
        // A room whose successor the user can reach sorts below every live room
        // of its rank: a demotion, not a filter, so the old room stays
        // openable. Applied before recency so a superseded room cannot outrank
        // a live one.
        const auto order = [&superseded](const QHash<QString, QDateTime> &stamps) {
            return [&superseded, p = &stamps](const RoomInfo &a, const RoomInfo &b) {
                const int aRank = orderRankOf(a);
                const int bRank = orderRankOf(b);
                if (aRank != bRank)
                    return aRank < bRank;
                const bool aOld = superseded.contains(a.id);
                const bool bOld = superseded.contains(b.id);
                if (aOld != bOld)
                    return bOld;
                return conversation::moreRecent(
                    p->value(a.id), a.name, a.id,
                    p->value(b.id), b.name, b.id);
            };
        };
        std::stable_sort(desired.begin(), desired.end(), order(key));
        // Held differs from live exactly when sorting by the live stamps would
        // move something.
        m_sortPending =
            !std::is_sorted(desired.begin(), desired.end(), order(target));
    }
    return desired;
}

void RoomListModel::refresh()
{
    // Structural change (roomsChanged/login): reconcile now and drop the
    // pending coalesced pass.
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

RoomListModel::BridgeBadge RoomListModel::badgeFor(const RoomInfo &r) const
{
    // What the bridge advertises wins over inference, which only works for DMs
    // and portal aliases.
    const auto it = m_advertisedBridges.constFind(r.id);
    if (it != m_advertisedBridges.constEnd() && !it->label.isEmpty())
        return *it;
    // No usable advertisement: fall back to the inference.
    const QString inferred =
        matrix::bridge::networkIdForRoom(r.directUserId, r.canonicalAlias);
    return { inferred, matrix::bridge::labelForNetworkId(inferred) };
}

void RoomListModel::setAdvertisedBridge(const QString &roomId,
                                        const QString &networkId,
                                        const QString &label)
{
    if (roomId.isEmpty())
        return;
    const auto existing = m_advertisedBridges.constFind(roomId);
    const bool had = existing != m_advertisedBridges.constEnd();
    if (label.isEmpty()) {
        // "Advertises no bridge" must not erase a still-correct DM inference,
        // so it is stored as no entry. Belt-and-braces with badgeFor()'s own
        // empty-label check: the cache and the reader are guarded separately.
        if (!had)
            return;
        m_advertisedBridges.remove(roomId);
    } else {
        if (had && existing->networkId == networkId && existing->label == label)
            return;
        m_advertisedBridges.insert(roomId, { networkId, label });
    }
    for (int row = 0; row < m_rooms.size(); ++row) {
        if (m_rooms.at(row).id != roomId)
            continue;
        Q_EMIT dataChanged(index(row), index(row),
                           { NetworkRole, NetworkLabelRole });
        break;
    }
}

void RoomListModel::clearAdvertisedBridges()
{
    if (m_advertisedBridges.isEmpty())
        return;
    m_advertisedBridges.clear();
    if (!m_rooms.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(m_rooms.size() - 1),
                           { NetworkRole, NetworkLabelRole });
    }
}

void RoomListModel::reconcileRooms()
{
    // Recomputed before the rows are written so replaceRoom's dataChanged
    // carries the new value; data() reads this cache to stay linear.
    const QSet<QString> previousSuperseded = m_supersededRoomIds;
    m_supersededRoomIds = computeSupersededRoomIds();
    // Passed in rather than recomputed: rooms() materialises a fresh list per
    // call.
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

    // Superseded state can flip without the room's own RoomInfo changing (the
    // successor is joined or its predecessor link arrives), which replaceRoom
    // would not notice. Notify the difference explicitly.
    if (previousSuperseded != m_supersededRoomIds) {
        for (int i = 0; i < m_rooms.size(); ++i) {
            const QString &id = m_rooms.at(i).id;
            if (previousSuperseded.contains(id) == m_supersededRoomIds.contains(id))
                continue;
            const QModelIndex idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx, { SupersededByAccessibleSuccessorRole });
        }
    }

    setOrderHeld(m_sortPending);
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
    // Unchanged rows emit nothing: this runs for every row on every room
    // update.
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

int RoomListModel::markAllRoomsRead()
{
    if (!m_client)
        return 0;
    // Collect first, then mark: markRoomRead() may make the backend republish
    // the room list we are iterating.
    QStringList targets;
    for (const RoomInfo &room : m_client->rooms()) {
        if (room.membership != RoomInfo::Joined || room.isSpace
            || room.markedUnread) {
            continue;
        }
        if (room.unreadCount <= 0 && room.highlightCount <= 0
            && !room.hasUnreadMessages) {
            continue;
        }
        targets.append(room.id);
    }
    for (const QString &roomId : targets)
        markRoomRead(roomId);
    return targets.size();
}

void RoomListModel::markRoomRead(const QString &roomId)
{
    if (!m_client || roomId.isEmpty()) return;
    // Prefer the backend that can resolve the latest event without a loaded
    // timeline; the fallback only works for the open room on the Rust backend.
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

// Where a row sits in the list's top-level order; the only classification in
// this file. The category role derives from it, so the sort and the section
// headers cannot disagree (RoomsPanel opens one header per contiguous run of
// `category`).
int RoomListModel::orderRankOf(const RoomInfo &room)
{
    // Invitations first: they need action, and cannot carry tags yet.
    if (room.membership == RoomInfo::Invited)
        return 0;
    // Favourites next, under their own header (Element's shape).
    if (room.isFavourite)
        return 1;
    return 2;
}

QString RoomListModel::categoryOf(const RoomInfo &room)
{
    // Three ranks, three values. "conversation" covers DMs and rooms alike
    // since they interleave by recency. The presenter chooses the visible label
    // from the active filter.
    switch (orderRankOf(room)) {
    case 0: return QStringLiteral("invite");
    case 1: return QStringLiteral("favourite");
    default: return QStringLiteral("conversation");
    }
}

void RoomListModel::updateUnreadTotals()
{
    // The whole account, not the current view: a filter does not change how
    // many conversations are waiting. Counts hasUnreadMessages, markedUnread or
    // a count, since notification_count can be 0 for a genuinely unread room.
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
    // Retired: favourites have their own section header now, so no divider is
    // drawn. Kept as a no-op so the bound property keeps working.
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
    // No local write: the flag is account state and the row shows what the
    // account holds until the backend confirms, so a refused write leaves it
    // put.
    if (m_client) m_client->setRoomFavourite(roomId, favourite);
}

QString RoomListModel::roomPermalink(const QString &roomId,
                                     const QString &canonicalAlias)
{
    // Prefer the canonical alias, with TimelineModel::messagePermalink's
    // percent-encoding (! $ : @ excluded) so room and message links match.
    const QString target = !canonicalAlias.isEmpty() ? canonicalAlias : roomId;
    if (target.isEmpty())
        return {};
    return QStringLiteral("https://matrix.to/#/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(
            target, QByteArrayLiteral("!$:@"))));
}
