#include "models/SpaceChannelModel.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#include <algorithm>

namespace {
constexpr auto kCollapsedKey = "shell/channelCollapsed";

// EVERY Material Symbols glyph this model names, in one block.
//
// Named constants rather than literals at the call sites, and that is a test
// seam: the bundled icon font is a SUBSET, an unmapped name renders as tofu,
// and IconChromeTest's ordinary sweep only sees a literal sitting beside an
// `Icon { name: }` in QML. A glyph chosen in C++ and bound through the model
// has no such literal, so `everyRuntimeChosenIconNameIsMapped` sweeps this
// block by its `kIcon` prefix — which only works if the names are here and
// nowhere else.
constexpr auto kIconCreate = "add";
constexpr auto kIconJoinAddress = "link";
constexpr auto kIconExploreSpaces = "groups";
constexpr auto kIconMessageSearch = "search";
constexpr auto kIconLobby = "flag";
} // namespace

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
           && iconName == other.iconName;
}

SpaceChannelModel::SpaceChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // One rebuild per event-loop turn, not one per incoming signal.
    //
    // Every source this model follows is bursty: a sync delivers many room
    // updates at once, a fresh account resolves many DM faces at once, and
    // each rebuild materialises the whole room list. Coalescing is the same
    // idiom RoomListModel already uses for its reconcile, and it is what
    // keeps an account switch's backlog from costing one full rebuild per
    // event.
    m_rebuildCoalesce.setSingleShot(true);
    m_rebuildCoalesce.setInterval(0);
    connect(&m_rebuildCoalesce, &QTimer::timeout, this,
            &SpaceChannelModel::rebuild);

    // A late-arriving DM face has to reach the ROW, and the rows hold a
    // snapshot: data() reads Row::avatarUrl, so a bare dataChanged would
    // repaint the same initials. rebuild() re-reads and applyRows diffs — the
    // ids and order are identical, so it is a dataChanged over the existing
    // rows, never a reset, and nothing moves.
    //
    // This comment used to claim "resolveMissing() is guarded by its own
    // cache, so this cannot feed itself." That was FALSE for the two
    // commonest answers — a peer with no avatar set, and a profile that 404s
    // — because the resolver cached neither yet announced both, so the
    // rebuild asked again and the pair ran forever. The resolver now caches
    // the negative and announces only a face it actually learned; the
    // coalescing above is the second guard.
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
    // A QUEUED REBUILD BELONGS TO THE SOURCES THAT ARMED IT.
    //
    // scheduleRebuild() defers to the next event-loop turn, so a burst that
    // arrived just before this call is still sitting in the queue — and it
    // would be delivered against whatever is wired up below instead. rebuild()
    // at the end of this function supersedes it anyway; cancelling it HERE is
    // what makes that true even if this function ever grows an early return.
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
    // A SOURCE THAT IS DESTROYED MUST LEAVE A NULL, NOT A DANGLING POINTER.
    //
    // rebuild() guards on `!m_client || !m_spaces`, which defends nothing once
    // the object behind the pointer is gone: the pointer is still non-null and
    // the deref is undefined. That is not hypothetical ordering pedantry —
    // AppController declares `m_spaces` AFTER `m_spaceChannels`, so on teardown
    // the SpaceManager this model reads is destroyed FIRST, and any event-loop
    // turn between the two (a deleteLater drain, a nested exec) delivers a
    // queued rebuild into a dead SpaceManager. Today nothing spins a loop in
    // that window, which makes this safe BY ACCIDENT.
    //
    // Deliberately no rebuild() from these handlers: a destroyed source means
    // the application is coming down, and a model reset emitted into views
    // that may themselves be half-destroyed buys nothing. Nulling the pointer
    // and cancelling the queued work is the whole job.
    if (m_spaces) {
        connect(m_spaces, &QObject::destroyed, this, [this] {
            m_spaces = nullptr;
            m_rebuildCoalesce.stop();
        });
        // SpaceManager rebuilds on both roomsChanged and roomUpdated and
        // always announces afterwards, so this single connection covers every
        // room change AND guarantees the hierarchy is resolved first.
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &SpaceChannelModel::scheduleRebuild);
        // A roster arriving is the only thing that can make the Space view's
        // People group appear (or, on an account change, vanish). It is not a
        // room change, so nothing else here would notice it.
        connect(m_spaces, &SpaceManager::spaceRosterChanged, this,
                &SpaceChannelModel::scheduleRebuild);
    }
    if (m_client) {
        connect(m_client, &QObject::destroyed, this, [this] {
            m_client = nullptr;
            m_rebuildCoalesce.stop();
        });
        // The collapse set is ACCOUNT-SCOPED storage, so a sign-out or an
        // account switch must drop the in-memory copy and read whoever is
        // next — keeping it would apply one account's collapsed folders to
        // another account's rooms. detachSession() (the switch) emits this
        // too, which makes the invalidation idempotent rather than duplicated.
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
    // Same rule as the sources above: no raw pointer held here may outlive
    // what it points at. This one happens to be safe on the current teardown
    // order (AppController destroys its SettingsManager last), which is not a
    // reason to be the one member that depends on a declaration order nobody
    // is asked to preserve. A null reads as "nothing to persist to", which
    // loadCollapsed()/saveCollapsed() already handle.
    if (m_settings) {
        connect(m_settings, &QObject::destroyed, this, [this] {
            m_settings = nullptr;
            m_collapsedLoaded = false;
            m_collapsed.clear();
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
    // THE SELECTION IS KEPT VERBATIM AND THE VIEW IS DERIVED FROM IT.
    //
    // A room id ('!') is a Space; peopleViewId() is the Direct Messages tab;
    // every other pseudo id ("" for Home, "@orphans") is Home. This used to
    // collapse every non-'!' value to "" and call the result a "scope", which
    // meant the rail had exactly one way to say anything that was not a
    // Space — so a People tab could not be expressed at all and DMs had to
    // ride along inside every other view to stay reachable.
    m_peopleView = spaceId == peopleViewId();
    m_scopeSpaceId =
        spaceId.startsWith(QLatin1Char('!')) ? spaceId : QString();
    Q_EMIT scopeSpaceIdChanged();
    rebuild();
}

QStringList SpaceChannelModel::listedSpaceIds() const
{
    // Home and People list NO Spaces. The rail already shows every one of
    // them, and repeating the whole set under Home is what made picking one
    // in the rail look like it had done nothing.
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
        // Selected a Space the account no longer has — left while it was
        // open. It is still the selection, so still the head of this list:
        // the view then renders its own emptiness, which is the truth, rather
        // than silently becoming a different Space's view.
        return { m_scopeSpaceId };
    }

    // The scoped Space, then its subspaces — recursively, deduped, and with a
    // visited set so a cyclic hierarchy cannot loop. FLAT, like every other
    // folder here: a subspace is a folder at the same level, not a level.
    QStringList out{ m_scopeSpaceId };
    QSet<QString> seen{ m_scopeSpaceId };
    for (int head = 0; head < out.size(); ++head) {
        for (const QString &childId : m_spaces->childSpaceIds(out.at(head))) {
            if (seen.contains(childId))
                continue;
            seen.insert(childId);
            out.append(childId);
        }
    }
    // Back into rail order, so a scoped view and the whole list agree about
    // where a Space sits relative to its siblings.
    QStringList ranked;
    for (const QString &id : ordered) {
        if (seen.contains(id))
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
        m_settings->appearanceValue(kCollapsedKey, QString()).toString();
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
    // Sorted so the stored value is stable and a no-op toggle pair does not
    // rewrite the file with a different ordering every time.
    std::sort(ids.begin(), ids.end());
    m_settings->setAppearanceValue(
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
    // A collapse changes which rows EXIST, not just how one looks.
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
    // A search opens everything: a room has to be findable whatever the user
    // last collapsed, and the collapse SET is untouched so clearing the box
    // restores exactly what was collapsed before.
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    const bool collapsed = !searching && isCollapsed(header.id);

    QVector<Row> kept;
    int hiddenUnread = 0;
    int hiddenHighlight = 0;
    bool anyUnread = false;
    for (Row &room : rooms) {
        if (!matchesQuery(room.name))
            continue;
        // An INVITE passes every filter, exactly as it does in Classic: it
        // needs action regardless of which view the user chose, and a People
        // or Rooms chip is not a request to hide one.
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
        // The header renders hiddenUnread as a DOT, never as a number (only a
        // mention count is shown), so a room that is unread without a count —
        // a marked-unread room, an invite — has to make it non-zero. Otherwise
        // collapsing a group would hide the fact that something is waiting,
        // which is the one thing collapsing must not do.
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
    // The lock glyph is a CLAIM. It is drawn only for encryption the client
    // knows about; "not established yet" gets the plain hash.
    row.encrypted = info.encrypted && info.encryptionKnown;
    row.unread = info.unreadCount;
    row.highlight = info.highlightCount;
    // An INVITE always reads as unread. It is an action waiting on the user,
    // and an invite has no unread counters of its own, so keying off them
    // alone would draw the loudest thing in the column as a quiet read row.
    row.hasUnread = row.isInvite || info.hasUnreadMessages || info.markedUnread
                    || info.unreadCount > 0 || info.highlightCount > 0;
    row.favourite = info.isFavourite;
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
        // Sable's Home menu, in Sable's order. These are commands, not
        // content, so a search over rooms leaves none of them standing —
        // they match nothing and would sit above an empty result claiming to
        // be part of it.
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

    // ROOM invites at Home, DM invites in People — the same split the joined
    // rooms get, so an invite is found where its room would be found once
    // accepted. Nothing is dropped: every invite is in exactly one view.
    QVector<Row> invites;
    QVector<Row> unparented;
    for (const RoomInfo &info : allRooms) {
        if (info.isSpace)
            continue;
        if (info.membership == RoomInfo::Invited) {
            if (!info.isDirect)
                invites.append(roomRow(info));
            continue;
        }
        if (info.membership != RoomInfo::Joined || info.isDirect)
            continue;
        // Every joined room no Space's own view will list.
        if (m_spaces && m_spaces->roomInAnySpace(info.id))
            continue;
        unparented.append(roomRow(info));
    }
    auto byName = [](const Row &a, const Row &b) {
        const int cmp = a.name.compare(b.name, Qt::CaseInsensitive);
        // Name, then id: two rooms may legitimately share a name, and a tie
        // broken by nothing is a list that reorders itself between syncs.
        return cmp != 0 ? cmp < 0 : a.id < b.id;
    };
    std::sort(invites.begin(), invites.end(), byName);
    std::sort(unparented.begin(), unparented.end(), byName);

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
    auto byName = [](const Row &a, const Row &b) {
        const int cmp = a.name.compare(b.name, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a.id < b.id;
    };
    std::sort(invites.begin(), invites.end(), byName);
    std::sort(chats.begin(), chats.end(), byName);

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
        // Lobby is the HEAD of this view: the Space's own overview — its
        // rooms and subspaces, its People, its settings. It is not a "leave
        // this Space" control, which is what it was when it cleared the
        // selection instead.
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
    // The selected Space, then each of its subspaces as a folder at the SAME
    // level. Nothing is nested: a subspace's rooms appearing both under the
    // subspace and (transitively) under its parent is what the flat shape
    // exists to prevent.
    for (const QString &spaceId : listedSpaceIds()) {
        const auto info = byId.constFind(spaceId);
        if (info == byId.constEnd())
            continue;
        Row header;
        header.id = spaceId;
        header.kind = SpaceKind;
        header.name = info->name;
        header.avatarUrl = info->avatarUrl;
        header.identityColorKey = identityColorKey(*info);

        // DIRECT children only, resolved through the room map this rebuild
        // ALREADY built rather than through directChildRoomsDetailed — that
        // materialises the whole room list and a fresh hash of its own on
        // every call, so walking every Space cost (1 + numSpaces) full
        // materialisations per rebuild.
        QVector<Row> children;
        if (m_spaces) {
            for (const QString &childId :
                 m_spaces->directChildRoomIds(spaceId, byId)) {
                const auto childInfo = byId.constFind(childId);
                if (childInfo == byId.constEnd())
                    continue;
                // A DM is never a Space's child. Matrix has no way to make
                // one, so a DM reached here would be a claim the state does
                // not make — and the People tab is where it belongs.
                if (childInfo->isDirect)
                    continue;
                children.append(roomRow(*childInfo));
            }
        }
        shown += appendGroup(rows, header, children);
    }
    shown += appendSpacePeople(rows, byId);
    return shown;
}

int SpaceChannelModel::appendSpacePeople(QVector<Row> &rows,
                                         const QHash<QString, RoomInfo> &byId)
{
    // THE SPACE'S PEOPLE — the DMs you have with people who are in this
    // Space. Not the Space's children: a DM cannot be one, and this group
    // makes no such claim. It answers the question the People chip answers in
    // Classic, in the layout that has no chips.
    //
    // THIS ONE FAILS CLOSED, and that is the opposite of the Classic rule on
    // purpose. Classic REMOVES DMs from a list that already shows them, so an
    // unknown roster must leave them alone; here the group ADDS them to a
    // view that has none, so an unknown roster must add nothing. Fail the
    // other way and every Space would list every DM until its roster landed.
    // `SpaceManager::directScope` returns 1 only for a complete roster, so
    // this needs no separate load-state branch — but the reason it does not
    // is exactly that, and not an oversight.
    //
    // Every DM stays reachable in the Direct Messages tab whatever this does,
    // which is what makes a scope safe to apply at all.
    if (!m_spaces)
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
        // Joined only. A DM INVITE lives in the People tab's Invites group —
        // it is an action on an account, not a member of a Space, and the
        // invite-passes-every-filter rule inside appendGroup would drag one
        // in here regardless of whose DM it is.
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
    std::sort(people.begin(), people.end(), [](const Row &a, const Row &b) {
        const int cmp = a.name.compare(b.name, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a.id < b.id;
    });
    Row header;
    header.id = spacePeopleGroupId();
    header.kind = GroupKind;
    header.name = tr("People");
    return appendGroup(rows, header, people);
}

void SpaceChannelModel::rebuild()
{
    // INVARIANT: once a rebuild has run, none is queued.
    //
    // Two things depend on it. A direct setter (a filter chip, a search
    // keystroke, the collapse toggle) rebuilds synchronously for its caller,
    // and a coalesced rebuild armed a moment earlier would then run the whole
    // pass a second time for no new information. More importantly, every
    // source change ends in a direct rebuild() — so cancelling here is what
    // guarantees that work armed under the OLD sources is never delivered
    // under the new ones, rather than relying on rebuild() happening to
    // survive being called in that state. It does survive it today; that is a
    // property of the current guards, not a contract anybody wrote down.
    //
    // Calling stop() on the single-shot timer whose timeout brought us here is
    // a no-op: it is already inactive by the time the slot runs.
    m_rebuildCoalesce.stop();
    ++m_rebuildCount;
    QVector<Row> rows;
    m_accountHasContent = false;
    if (!m_client || !m_spaces) {
        applyRows(std::move(rows));
        return;
    }

    const QList<RoomInfo> allRooms = m_client->rooms();
    // Ask once per unresolved DM peer. Idempotent and bounded: a peer already
    // cached or already in flight is skipped, so running this on every rebuild
    // costs nothing after the first pass.
    m_directAvatars.resolveMissing(allRooms);

    // "Does this account have anything at all" is a question about the
    // ACCOUNT, so it is answered from the whole room list — never from what
    // the current view happens to contain. A Space with no rooms in it yet is
    // an empty VIEW on an account that is not empty, and saying "you have no
    // conversations" there sends the user looking for a problem that is not
    // there.
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

    // A filter that matched nothing is a fact about the FILTER, never about
    // the account (`empty` must keep answering the second question only). The
    // column needs both to tell "you have no conversations" from "this view
    // has nothing in it".
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
            // The rows did not move; at most their unread state changed. A
            // reset here would tear down and rebuild every delegate on every
            // arriving message, which for a sidebar this long is visible.
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
