#include "models/SpaceChannelModel.h"

#include "models/ConversationOrder.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#include <algorithm>

namespace {
// Shared with SettingsManager::forgetDeviceGlobalAccountResidue, which clears
// the device-global copy when the last account goes.
constexpr auto kCollapsedKey = SettingsManager::kChannelCollapsedKey;

// Every Material Symbols glyph this model names. The bundled icon font is a
// subset, and names chosen in C++ have no QML literal for IconChromeTest to
// find, so `everyRuntimeChosenIconNameIsMapped` sweeps this `kIcon` block.
// Keep every name here and nowhere else.
constexpr auto kIconCreate = "add";
constexpr auto kIconJoinAddress = "link";
constexpr auto kIconExploreSpaces = "groups";
constexpr auto kIconMessageSearch = "search";
constexpr auto kIconLobby = "flag";
} // namespace

bool SpaceChannelModel::byRecency(const Row &a, const Row &b)
{
    return conversation::moreRecent(a.lastActivity, a.name, a.id,
                                    b.lastActivity, b.name, b.id);
}

bool SpaceChannelModel::byFavouriteThenRecency(const Row &a, const Row &b)
{
    if (a.favourite != b.favourite)
        return a.favourite;
    return byRecency(a, b);
}

bool SpaceChannelModel::Row::operator==(const Row &other) const
{
    return id == other.id && name == other.name && kind == other.kind
           && depth == other.depth && avatarUrl == other.avatarUrl
           && identityColorKey == other.identityColorKey
           && isDirect == other.isDirect && isInvite == other.isInvite
           && encrypted == other.encrypted && unread == other.unread
           && highlight == other.highlight && hasUnread == other.hasUnread
           && favourite == other.favourite
           && hiddenUnread == other.hiddenUnread
           && hiddenHighlight == other.hiddenHighlight
           && iconName == other.iconName
           // Part of identity so a row whose activity moved differs and the
           // list can reorder on a new message.
           && lastActivity == other.lastActivity;
}

SpaceChannelModel::SpaceChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // One rebuild per event-loop turn: every source is bursty and each rebuild
    // materialises the whole room list.
    m_rebuildCoalesce.setSingleShot(true);
    m_rebuildCoalesce.setInterval(0);
    connect(&m_rebuildCoalesce, &QTimer::timeout, this,
            &SpaceChannelModel::rebuild);

    // A late DM face must reach the row snapshot that data() reads, so rebuild
    // rather than emit a bare dataChanged. Ids and order are unchanged, so
    // applyRows emits dataChanged, never a reset. The resolver caches negatives
    // and announces only faces it learned, so this cannot loop; the coalescing
    // is a second guard.
    connect(&m_directAvatars, &DirectAvatarResolver::avatarResolved, this,
            &SpaceChannelModel::scheduleRebuild);
}

void SpaceChannelModel::scheduleRebuild()
{
    m_rebuildCoalesce.start();
}

void SpaceChannelModel::setSources(MatrixClient *client, SpaceManager *spaces,
                                   RailLayoutStore *layout)
{
    // A queued rebuild belongs to the sources that armed it; cancel it so it is
    // never delivered against the new wiring.
    m_rebuildCoalesce.stop();
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    if (m_spaces)
        disconnect(m_spaces, nullptr, this, nullptr);
    if (m_layout)
        disconnect(m_layout, nullptr, this, nullptr);
    m_client = client;
    m_directAvatars.setClient(client);
    m_spaces = spaces;
    m_layout = layout;
    // A destroyed source must leave a null, not a dangling pointer:
    // AppController destroys SpaceManager before this model, so a queued
    // rebuild could reach a dead object. No rebuild here; the application is
    // coming down.
    if (m_spaces) {
        connect(m_spaces, &QObject::destroyed, this, [this] {
            m_spaces = nullptr;
            m_rebuildCoalesce.stop();
        });
        // SpaceManager announces after rebuilding on roomsChanged and
        // roomUpdated, so this covers every room change with the hierarchy
        // already resolved.
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &SpaceChannelModel::scheduleRebuild);
        // A roster arriving is the only thing that can make a Space view's
        // People group appear or vanish.
        connect(m_spaces, &SpaceManager::spaceRosterChanged, this,
                &SpaceChannelModel::scheduleRebuild);
    }
    if (m_client) {
        connect(m_client, &QObject::destroyed, this, [this] {
            m_client = nullptr;
            m_rebuildCoalesce.stop();
        });
        // The collapse set is account-scoped: drop the in-memory copy on
        // sign-out or switch so one account's collapsed folders never apply to
        // another's rooms.
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            m_collapsed.clear();
            m_collapsedLoaded = false;
            rebuild();
        });
    }
    if (m_layout) {
        connect(m_layout, &QObject::destroyed, this, [this] {
            m_layout = nullptr;
            m_rebuildCoalesce.stop();
        });
        connect(m_layout, &RailLayoutStore::layoutChanged, this,
                &SpaceChannelModel::scheduleRebuild);
    }
    rebuild();
}

void SpaceChannelModel::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    // Same rule as the sources above: no raw pointer may outlive its target. A
    // null means "nothing to persist to".
    if (m_settings) {
        connect(m_settings, &QObject::destroyed, this, [this] {
            m_settings = nullptr;
            m_collapsedLoaded = false;
            m_collapsed.clear();
        });
        // loggedOut fires from detachSession() before the active account moves,
        // and its rebuild re-caches the outgoing account's set. This fires
        // after the move.
        connect(m_settings, &SettingsManager::sessionChanged, this, [this] {
            m_collapsedLoaded = false;
            m_collapsed.clear();
            rebuild();
        });
    }
    m_collapsedLoaded = false;
    m_collapsed.clear();
    rebuild();
}

void SpaceChannelModel::setFilterMode(int mode)
{
    const int clamped = (mode < 0 || mode > 3) ? 0 : mode;
    if (m_filterMode == clamped)
        return;
    m_filterMode = clamped;
    Q_EMIT filterModeChanged();
    rebuild();
}

void SpaceChannelModel::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query)
        return;
    m_searchQuery = query;
    Q_EMIT searchQueryChanged();
    rebuild();
}

void SpaceChannelModel::setMessageSearchSupported(bool supported)
{
    if (m_messageSearchSupported == supported)
        return;
    m_messageSearchSupported = supported;
    Q_EMIT messageSearchSupportedChanged();
    rebuild();
}

QString SpaceChannelModel::peopleViewId()
{
    return SpaceManager::peopleId();
}

QStringList SpaceChannelModel::actionIds()
{
    return { createRoomActionId(), joinAddressActionId(),
             exploreSpacesActionId(), createChatActionId() };
}

QString SpaceChannelModel::viewKind() const
{
    if (m_peopleView)
        return QStringLiteral("people");
    return m_scopeSpaceId.isEmpty() ? QStringLiteral("home")
                                    : QStringLiteral("space");
}

void SpaceChannelModel::setScopeSpaceId(const QString &spaceId)
{
    if (m_selection == spaceId)
        return;
    m_selection = spaceId;
    // The selection is kept verbatim and the view derived from it: a room id
    // ('!') is a Space, peopleViewId() is the Direct Messages tab, and any
    // other pseudo id ("" for Home, "@orphans") is Home.
    m_peopleView = spaceId == peopleViewId();
    m_scopeSpaceId =
        spaceId.startsWith(QLatin1Char('!')) ? spaceId : QString();
    Q_EMIT scopeSpaceIdChanged();
    rebuild();
}

QStringList SpaceChannelModel::childSpacesOf(
    const QString &spaceId, const QHash<QString, RoomInfo> &byId) const
{
    // The rail's nesting is not this view's hierarchy. childSpaceIds keeps only
    // children whose primary parent is this Space (so the rail is a tree), but
    // a Space's column should list a shared subspace under every parent;
    // otherwise the parent's rail badge counts rooms its own view does not
    // list. Use the rail's answer (which also covers child-side
    // `parentSpaceIds` edges) plus this Space's own `m.space.child` state,
    // additively, in the Space's order.
    QStringList out;
    if (m_spaces)
        out = m_spaces->childSpaceIds(spaceId);
    const auto parent = byId.constFind(spaceId);
    if (parent == byId.constEnd())
        return out;
    for (const QString &childId : parent->childRoomIds) {
        if (childId == spaceId || out.contains(childId))
            continue;
        const auto child = byId.constFind(childId);
        if (child == byId.constEnd() || !child->isSpace
            || child->membership != RoomInfo::Joined) {
            continue;
        }
        out.append(childId);
    }
    return out;
}

QStringList SpaceChannelModel::listedSpaceIds(
    const QHash<QString, RoomInfo> &byId) const
{
    // Home and People list no Spaces; the rail already shows them.
    if (!m_spaces || m_scopeSpaceId.isEmpty())
        return {};
    const QVariantList spaceRows = m_spaces->allSpaces();
    QStringList ordered;
    if (m_layout) {
        ordered = m_layout->orderedSpaceIds(spaceRows);
    } else {
        for (const QVariant &value : spaceRows) {
            const QString id =
                value.toMap().value(QStringLiteral("spaceId")).toString();
            if (!id.isEmpty() && !id.startsWith(QLatin1Char('@')))
                ordered.append(id);
        }
    }
    if (!ordered.contains(m_scopeSpaceId)) {
        // The selected Space was left while open. It stays the head of this
        // list so the view renders its own emptiness rather than becoming
        // another Space.
        return { m_scopeSpaceId };
    }

    // The scoped Space, then its subspaces, recursively and deduped, flat like
    // every other folder here. The visited set is load-bearing: raw
    // m.space.child state is a graph and may contain cycles.
    QStringList out{ m_scopeSpaceId };
    QSet<QString> seen{ m_scopeSpaceId };
    for (int head = 0; head < out.size(); ++head) {
        for (const QString &childId : childSpacesOf(out.at(head), byId)) {
            if (seen.contains(childId))
                continue;
            seen.insert(childId);
            out.append(childId);
        }
    }
    // Back into rail order, with the selected Space always first. The rail
    // ranks only root Spaces; subspaces follow in activity order, so without
    // seeding `ranked` with the selection a subspace could render above its
    // parent.
    QStringList ranked{ m_scopeSpaceId };
    for (const QString &id : ordered) {
        if (seen.contains(id) && !ranked.contains(id))
            ranked.append(id);
    }
    for (const QString &id : out) {
        if (!ranked.contains(id))
            ranked.append(id);
    }
    return ranked;
}

bool SpaceChannelModel::empty() const
{
    return !m_accountHasContent;
}

bool SpaceChannelModel::filterAdmits(bool isDirect, bool unread) const
{
    switch (m_filterMode) {
    case 1:   // People
        return isDirect;
    case 2:   // Rooms
        return !isDirect;
    case 3:   // Unreads
        return unread;
    default:  // All
        return true;
    }
}

bool SpaceChannelModel::matchesQuery(const QString &name) const
{
    if (m_searchQuery.trimmed().isEmpty())
        return true;
    return name.contains(m_searchQuery.trimmed(), Qt::CaseInsensitive);
}

void SpaceChannelModel::loadCollapsed() const
{
    if (m_collapsedLoaded)
        return;
    m_collapsedLoaded = true;
    m_collapsed.clear();
    if (!m_settings)
        return;
    const QString json =
        m_settings->accountScopedValue(kCollapsedKey, QString()).toString();
    if (json.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return;
    for (const QJsonValue &value : doc.array()) {
        const QString id = value.toString();
        if (!id.isEmpty())
            m_collapsed.insert(id);
    }
}

void SpaceChannelModel::saveCollapsed()
{
    if (!m_settings)
        return;
    QStringList ids(m_collapsed.constBegin(), m_collapsed.constEnd());
    // Sorted so the stored value is stable across no-op toggles.
    std::sort(ids.begin(), ids.end());
    m_settings->setAccountScopedValue(
        kCollapsedKey,
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(ids))
                              .toJson(QJsonDocument::Compact)));
}

bool SpaceChannelModel::isCollapsed(const QString &headerId) const
{
    loadCollapsed();
    return m_collapsed.contains(headerId);
}

void SpaceChannelModel::toggleCollapsed(const QString &headerId)
{
    if (headerId.isEmpty())
        return;
    loadCollapsed();
    if (m_collapsed.contains(headerId))
        m_collapsed.remove(headerId);
    else
        m_collapsed.insert(headerId);
    saveCollapsed();
    // A collapse changes which rows exist, not just how one looks.
    rebuild();
}

int SpaceChannelModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_rows.size());
}

QHash<int, QByteArray> SpaceChannelModel::roleNames() const
{
    return {
        { RoomIdRole, "roomId" },
        { NameRole, "name" },
        { KindRole, "kind" },
        { DepthRole, "depth" },
        { AvatarUrlRole, "avatarUrl" },
        { IdentityColorKeyRole, "identityColorKey" },
        { IsDirectRole, "isDirect" },
        { IsInviteRole, "isInvite" },
        { EncryptedRole, "encrypted" },
        { UnreadCountRole, "unreadCount" },
        { HighlightCountRole, "highlightCount" },
        { HasUnreadRole, "hasUnread" },
        { CollapsedRole, "collapsed" },
        { HiddenUnreadRole, "hiddenUnread" },
        { HiddenHighlightRole, "hiddenHighlight" },
        { IsFavouriteRole, "isFavourite" },
        { IconNameRole, "iconName" },
    };
}

QVariant SpaceChannelModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case RoomIdRole:
        return row.id;
    case NameRole:
        return row.name;
    case KindRole:
        switch (row.kind) {
        case LobbyKind:
            return QStringLiteral("lobby");
        case SearchKind:
            return QStringLiteral("search");
        case GroupKind:
            return QStringLiteral("group");
        case SpaceKind:
            return QStringLiteral("space");
        case ActionKind:
            return QStringLiteral("action");
        default:
            return QStringLiteral("room");
        }
    case DepthRole:
        return row.depth;
    case AvatarUrlRole:
        return row.avatarUrl;
    case IdentityColorKeyRole:
        return row.identityColorKey;
    case IsDirectRole:
        return row.isDirect;
    case IsInviteRole:
        return row.isInvite;
    case EncryptedRole:
        return row.encrypted;
    case UnreadCountRole:
        return row.unread;
    case HighlightCountRole:
        return row.highlight;
    case HasUnreadRole:
        return row.hasUnread;
    case CollapsedRole:
        return (row.kind == SpaceKind || row.kind == GroupKind)
               && isCollapsed(row.id);
    case HiddenUnreadRole:
        return row.hiddenUnread;
    case HiddenHighlightRole:
        return row.hiddenHighlight;
    case IsFavouriteRole:
        return row.favourite;
    case IconNameRole:
        return row.iconName;
    default:
        return {};
    }
}

int SpaceChannelModel::rowForRoom(const QString &roomId) const
{
    if (roomId.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).kind == RoomKind && m_rows.at(i).id == roomId)
            return i;
    }
    return -1;
}

int SpaceChannelModel::appendGroup(QVector<Row> &rows, Row header,
                                   QVector<Row> rooms)
{
    // A search opens everything so any room is findable; the collapse set is
    // untouched and clearing the search restores it.
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    const bool collapsed = !searching && isCollapsed(header.id);

    QVector<Row> kept;
    int hiddenUnread = 0;
    int hiddenHighlight = 0;
    bool anyUnread = false;
    for (Row &room : rooms) {
        if (!matchesQuery(room.name))
            continue;
        // An invite passes every filter, as in Classic: it needs action
        // whatever view is chosen.
        if (!room.isInvite
            && !filterAdmits(room.isDirect, room.hasUnread || room.unread > 0
                                                || room.highlight > 0)) {
            continue;
        }
        hiddenUnread += room.unread;
        hiddenHighlight += room.highlight;
        anyUnread = anyUnread || room.hasUnread;
        room.depth = 1;
        kept.append(room);
    }
    if (kept.isEmpty())
        return 0;
    if (collapsed) {
        // The header shows hiddenUnread as a dot (only mentions are counted),
        // so an unread room without a count (marked unread, an invite) must
        // make it non-zero, or collapsing a group would hide that something is
        // waiting.
        header.hiddenUnread =
            hiddenUnread > 0 ? hiddenUnread : (anyUnread ? 1 : 0);
        header.hiddenHighlight = hiddenHighlight;
    }
    rows.append(header);
    if (!collapsed)
        rows.append(kept);
    return kept.size();
}

SpaceChannelModel::Row SpaceChannelModel::roomRow(const RoomInfo &info) const
{
    Row row;
    row.id = info.id;
    row.name = info.name;
    row.kind = RoomKind;
    row.avatarUrl = m_directAvatars.avatarFor(info);
    row.identityColorKey = identityColorKey(info);
    row.isDirect = info.isDirect;
    row.isInvite = info.membership == RoomInfo::Invited;
    // The lock glyph is a claim: drawn only for encryption the client knows
    // about.
    row.encrypted = info.encrypted && info.encryptionKnown;
    row.unread = info.unreadCount;
    row.highlight = info.highlightCount;
    // An invite always reads as unread: it is waiting on the user and has no
    // unread counters of its own.
    row.hasUnread = row.isInvite || info.hasUnreadMessages || info.markedUnread
                    || info.unreadCount > 0 || info.highlightCount > 0;
    row.favourite = info.isFavourite;
    row.lastActivity = info.lastActivity;
    return row;
}

SpaceChannelModel::Row SpaceChannelModel::actionRow(const QString &id,
                                                    const QString &name,
                                                    const QString &icon) const
{
    Row row;
    row.id = id;
    row.name = name;
    row.kind = ActionKind;
    row.iconName = icon;
    return row;
}

int SpaceChannelModel::buildHome(QVector<Row> &rows,
                                 const QList<RoomInfo> &allRooms)
{
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    if (!searching) {
        // Sable's Home menu, in Sable's order. Commands, not content, so a
        // search hides them all.
        rows.append(actionRow(createRoomActionId(), tr("Create Room"),
                              QLatin1String(kIconCreate)));
        rows.append(actionRow(joinAddressActionId(),
                              tr("Join with Address"),
                              QLatin1String(kIconJoinAddress)));
        rows.append(actionRow(exploreSpacesActionId(), tr("Explore Spaces"),
                              QLatin1String(kIconExploreSpaces)));
        if (m_messageSearchSupported) {
            Row search;
            search.kind = SearchKind;
            search.name = tr("Message Search");
            search.iconName = QLatin1String(kIconMessageSearch);
            rows.append(search);
        }
    }

    // Room invites at Home, DM invites in People, matching where the room will
    // be once accepted. Every invite is in exactly one view.
    QVector<Row> invites;
    QVector<Row> unparented;
    QVector<Row> directs;
    for (const RoomInfo &info : allRooms) {
        if (info.isSpace)
            continue;
        if (info.membership == RoomInfo::Invited) {
            if (!info.isDirect)
                invites.append(roomRow(info));
            continue;
        }
        if (info.membership != RoomInfo::Joined)
            continue;
        // Joined DMs are also listed here, as their own group after Rooms. DM
        // invites stay in the People tab's Invites group.
        if (info.isDirect) {
            directs.append(roomRow(info));
            continue;
        }
        // Every joined room that no Space's own view lists.
        if (m_spaces && m_spaces->roomInAnySpace(info.id))
            continue;
        unparented.append(roomRow(info));
    }
    // Newest first, using the same comparator as the Classic list so the two
    // layouts agree on recency.
    std::sort(invites.begin(), invites.end(), byRecency);
    std::sort(unparented.begin(), unparented.end(), byFavouriteThenRecency);
    std::sort(directs.begin(), directs.end(), byFavouriteThenRecency);

    int shown = 0;
    if (!invites.isEmpty()) {
        Row header;
        header.id = invitesGroupId();
        header.kind = GroupKind;
        header.name = tr("Invites");
        shown += appendGroup(rows, header, invites);
    }
    if (!unparented.isEmpty()) {
        Row header;
        header.id = roomsGroupId();
        header.kind = GroupKind;
        header.name = tr("Rooms");
        shown += appendGroup(rows, header, unparented);
    }
    if (!directs.isEmpty()) {
        Row header;
        header.id = homeDirectsGroupId();
        header.kind = GroupKind;
        header.name = tr("Direct Messages");
        shown += appendGroup(rows, header, directs);
    }
    return shown;
}

int SpaceChannelModel::buildPeople(QVector<Row> &rows,
                                   const QList<RoomInfo> &allRooms)
{
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    if (!searching) {
        rows.append(actionRow(createChatActionId(), tr("Create Chat"),
                              QLatin1String(kIconCreate)));
    }

    QVector<Row> invites;
    QVector<Row> chats;
    for (const RoomInfo &info : allRooms) {
        if (info.isSpace || !info.isDirect)
            continue;
        if (info.membership == RoomInfo::Invited) {
            invites.append(roomRow(info));
            continue;
        }
        if (info.membership != RoomInfo::Joined)
            continue;
        chats.append(roomRow(info));
    }
    // Chats newest first, like every other conversation list here.
    std::sort(invites.begin(), invites.end(), byRecency);
    std::sort(chats.begin(), chats.end(), byFavouriteThenRecency);

    int shown = 0;
    if (!invites.isEmpty()) {
        Row header;
        header.id = invitesGroupId();
        header.kind = GroupKind;
        header.name = tr("Invites");
        shown += appendGroup(rows, header, invites);
    }
    if (!chats.isEmpty()) {
        Row header;
        header.id = directsGroupId();
        header.kind = GroupKind;
        header.name = tr("Chats");
        shown += appendGroup(rows, header, chats);
    }
    return shown;
}

int SpaceChannelModel::buildSpace(QVector<Row> &rows,
                                  const QHash<QString, RoomInfo> &byId)
{
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    if (!searching) {
        // Lobby heads this view: the Space's own overview (rooms, subspaces,
        // People, settings).
        Row lobby;
        lobby.kind = LobbyKind;
        lobby.name = tr("Lobby");
        lobby.iconName = QLatin1String(kIconLobby);
        rows.append(lobby);
        if (m_messageSearchSupported) {
            Row search;
            search.kind = SearchKind;
            search.name = tr("Message Search");
            search.iconName = QLatin1String(kIconMessageSearch);
            rows.append(search);
        }
    }

    int shown = 0;
    // The selected Space, then each subspace as a folder at the same level.
    // Nothing is nested, so a subspace's rooms are never listed twice.
    for (const QString &spaceId : listedSpaceIds(byId)) {
        const auto info = byId.constFind(spaceId);
        if (info == byId.constEnd())
            continue;
        Row header;
        header.id = spaceId;
        header.kind = SpaceKind;
        header.name = info->name;
        header.avatarUrl = info->avatarUrl;
        header.identityColorKey = identityColorKey(*info);

        // Direct children only, resolved through the room map this rebuild
        // already built; directChildRoomsDetailed would materialise the whole
        // room list per Space.
        QVector<Row> children;
        if (m_spaces) {
            for (const QString &childId :
                 m_spaces->directChildRoomIds(spaceId, byId)) {
                const auto childInfo = byId.constFind(childId);
                if (childInfo == byId.constEnd())
                    continue;
                // A DM is never a Space's child; Matrix cannot express one, and
                // DMs belong in the People tab.
                if (childInfo->isDirect)
                    continue;
                children.append(roomRow(*childInfo));
            }
        }
        // Newest first within the group; the group structure itself is
        // unchanged.
        std::sort(children.begin(), children.end(), byFavouriteThenRecency);
        shown += appendGroup(rows, header, children);
    }
    shown += appendSpacePeople(rows, byId);
    return shown;
}

int SpaceChannelModel::appendSpacePeople(QVector<Row> &rows,
                                         const QHash<QString, RoomInfo> &byId)
{
    // The Space's People: DMs with people who are in this Space. Unlike
    // Classic, this fails closed: Classic removes DMs from a list that already
    // has them, while this adds them to a view that has none, so an unknown
    // roster must add nothing. directScope returns 1 only for a complete
    // roster. Every DM stays reachable in the Direct Messages tab.
    if (!m_spaces)
        return 0;
    // Shown only under the People filter, not permanently. filterMode is the
    // same closed set the Classic chips write (0 All, 1 People, 2 Rooms, 3
    // Unreads).
    if (m_filterMode != 1)   // People
        return 0;
    const QString spaceId = m_scopeSpaceId;
    if (!SpaceManager::isRealSpaceId(spaceId)
        || !m_spaces->spaceRosterKnown(spaceId)) {
        return 0;
    }
    QVector<Row> people;
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        const RoomInfo &info = *it;
        if (info.isSpace || !info.isDirect)
            continue;
        // Joined only: a DM invite lives in the People tab's Invites group, and
        // the invite rule in appendGroup would otherwise drag every one in
        // here.
        if (info.membership != RoomInfo::Joined)
            continue;
        QStringList peers = info.directUserIds;
        if (peers.isEmpty() && !info.directUserId.isEmpty())
            peers.append(info.directUserId);
        if (m_spaces->directScope(spaceId, peers) != 1)
            continue;
        people.append(roomRow(info));
    }
    if (people.isEmpty())
        return 0;
    // A Space's People are conversations too, so they follow the same order.
    std::sort(people.begin(), people.end(), byFavouriteThenRecency);
    Row header;
    header.id = spacePeopleGroupId();
    header.kind = GroupKind;
    header.name = tr("People");
    return appendGroup(rows, header, people);
}

void SpaceChannelModel::rebuild()
{
    // Invariant: once a rebuild has run, none is queued. Direct setters rebuild
    // synchronously, and every source change ends in rebuild(), so cancelling
    // here also guarantees work armed under old sources is never delivered
    // under new ones. (stop() on the timer that brought us here is a no-op.)
    m_rebuildCoalesce.stop();
    ++m_rebuildCount;
    QVector<Row> rows;
    m_accountHasContent = false;
    if (!m_client || !m_spaces) {
        applyRows(std::move(rows));
        return;
    }

    const QList<RoomInfo> allRooms = m_client->rooms();
    // Ask once per unresolved DM peer. Cached and in-flight peers are skipped,
    // so this is free after the first pass.
    m_directAvatars.resolveMissing(allRooms);

    // Whether the account has anything at all is answered from the whole room
    // list, never from the current view: an empty Space is not an empty
    // account.
    for (const RoomInfo &info : allRooms) {
        if (info.membership == RoomInfo::Joined
            || info.membership == RoomInfo::Invited) {
            m_accountHasContent = true;
            break;
        }
    }

    int roomsShown = 0;
    if (m_peopleView) {
        roomsShown = buildPeople(rows, allRooms);
    } else if (m_scopeSpaceId.isEmpty()) {
        roomsShown = buildHome(rows, allRooms);
    } else {
        QHash<QString, RoomInfo> byId;
        byId.reserve(allRooms.size());
        for (const RoomInfo &room : allRooms)
            byId.insert(room.id, room);
        roomsShown = buildSpace(rows, byId);
    }

    // A filter that matched nothing is a fact about the filter; `empty` answers
    // only whether the account has conversations.
    if (m_matchCount != roomsShown) {
        m_matchCount = roomsShown;
        Q_EMIT matchCountChanged();
    }

    applyRows(std::move(rows));
}

void SpaceChannelModel::applyRows(QVector<Row> rows)
{
    if (rows.size() == m_rows.size()) {
        bool sameIds = true;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).id != m_rows.at(i).id
                || rows.at(i).kind != m_rows.at(i).kind) {
                sameIds = false;
                break;
            }
        }
        if (sameIds) {
            // Rows did not move; at most unread state changed. A reset would
            // rebuild every delegate on every message.
            if (rows == m_rows) {
                Q_EMIT countChanged();
                return;
            }
            m_rows = std::move(rows);
            Q_EMIT dataChanged(index(0, 0), index(m_rows.size() - 1, 0));
            Q_EMIT countChanged();
            return;
        }
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countChanged();
}
