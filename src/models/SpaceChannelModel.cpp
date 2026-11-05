#include "models/SpaceChannelModel.h"

#include "models/ConversationOrder.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomUnreadRule.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#include <algorithm>
#include <climits>
#include <utility>

namespace {
constexpr auto kCollapsedKey = SettingsManager::kChannelCollapsedKey;
constexpr auto kKeepImportantKey = "roomList/keepImportantInCollapsed";

// Every Material Symbols glyph this model names. The bundled icon font is a
// subset, and names chosen in C++ have no QML literal for IconChromeTest to
// find, so `everyRuntimeChosenIconNameIsMapped` sweeps this `kIcon` block.
// Keep every name here and nowhere else.
constexpr auto kIconCreate = "add";
constexpr auto kIconJoinAddress = "link";
constexpr auto kIconExploreSpaces = "explore";
constexpr auto kIconMessageSearch = "manage_search";
constexpr auto kIconLobby = "flag";
}

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
           && threadUnreadKind == other.threadUnreadKind
           && favourite == other.favourite
           && lowPriority == other.lowPriority
           && hiddenUnread == other.hiddenUnread
           && hiddenHighlight == other.hiddenHighlight
           && iconName == other.iconName
           && lastActivity == other.lastActivity;
}

SpaceChannelModel::SpaceChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_rebuildCoalesce.setSingleShot(true);
    m_rebuildCoalesce.setInterval(0);
    connect(&m_rebuildCoalesce, &QTimer::timeout, this,
            &SpaceChannelModel::rebuild);

    setDirectAvatarResolver(nullptr);

    m_holdCap.setSingleShot(true);
    m_holdCap.setInterval(kHoldCapMs);
    connect(&m_holdCap, &QTimer::timeout, this,
            &SpaceChannelModel::releaseOrder);
}

void SpaceChannelModel::setDirectAvatarResolver(DirectAvatarResolver *resolver)
{
    DirectAvatarResolver *next = resolver ? resolver : &m_ownDirectAvatars;
    if (m_directAvatarsConn && m_directAvatars == next)
        return;
    disconnect(m_directAvatarsConn);
    m_directAvatars = next;
    if (m_client)
        m_directAvatars->setClient(m_client);
    m_directAvatarsConn = connect(m_directAvatars,
        &DirectAvatarResolver::avatarResolved, this,
        &SpaceChannelModel::scheduleRebuild);
}

void SpaceChannelModel::setDeferReordering(bool defer)
{
    if (m_hold.enabled() == defer)
        return;
    m_hold.setEnabled(defer);
    Q_EMIT deferReorderingChanged();
    rebuild();
}

void SpaceChannelModel::setSortMode(int mode)
{
    const int valid = mode == -1 ? int(conversation::SortBySpaceOrder)
                                 : conversation::normalizedSortMode(mode);
    if (valid == m_sortMode)
        return;
    m_sortMode = valid;
    m_hold.release();
    rebuild();
    Q_EMIT sortModeChanged();
}

void SpaceChannelModel::releaseOrder()
{
    m_hold.release();
    rebuild();
}

void SpaceChannelModel::setHoldCapMs(int ms)
{
    m_holdCap.setInterval(ms);
}

void SpaceChannelModel::setOpenRoomId(const QString &roomId)
{
    if (roomId == m_hold.openRoomId())
        return;
    QDateTime liveNow;
    if (m_client && !roomId.isEmpty()) {
        for (const RoomInfo &info : m_client->rooms()) {
            if (info.id == roomId) {
                liveNow = info.lastActivity;
                break;
            }
        }
    }
    const bool hadHold = m_hold.enabled();
    m_hold.release();
    m_hold.setOpenRoom(roomId, liveNow);
    if (hadHold || keepImportantRoomsVisible())
        scheduleRebuild();
}

void SpaceChannelModel::setRoomHovered(const QString &roomId, bool hovered)
{
    if (!hovered && m_hoveredRoomId != roomId)
        return;
    const QString next = hovered ? roomId : QString();
    if (m_hoveredRoomId == next)
        return;
    m_hoveredRoomId = next;
    if (keepImportantRoomsVisible())
        scheduleRebuild();
}

bool SpaceChannelModel::keepImportantRoomsVisible() const
{
    return m_settings
           && m_settings->accountScopedValue(kKeepImportantKey,
                                              false).toBool();
}

void SpaceChannelModel::setKeepImportantRoomsVisible(bool enabled)
{
    if (!m_settings || keepImportantRoomsVisible() == enabled)
        return;
    m_settings->setAccountScopedValue(kKeepImportantKey, enabled);
    rebuild();
    Q_EMIT keepImportantRoomsVisibleChanged();
}

void SpaceChannelModel::setOrderHeld(bool held)
{
    if (held) {
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

void SpaceChannelModel::setCallRoomId(const QString &roomId)
{
    if (roomId == m_callRoomId)
        return;
    m_callRoomId = roomId;
    rebuild();
}

void SpaceChannelModel::sortGroup(QVector<Row> &rooms, bool favouritesFirst)
{
    const QString call = m_callRoomId;
    const auto isCall = [&call](const Row &r) {
        return !call.isEmpty() && r.id == call;
    };
    if (m_sortMode == conversation::SortByName) {
        const QCollator collator = conversation::makeNameCollator();
        std::sort(rooms.begin(), rooms.end(),
                  [favouritesFirst, &collator, &isCall](const Row &a, const Row &b) {
                      if (isCall(a) != isCall(b))
                          return isCall(a);
                      if (favouritesFirst && a.favourite != b.favourite)
                          return a.favourite;
                      if (a.lowPriority != b.lowPriority)
                          return !a.lowPriority;
                      return conversation::byName(collator, a.name, a.id,
                                                  b.name, b.id);
                  });
        return;
    }
    const bool unreadFirst = m_sortMode == conversation::SortByUnread;
    std::sort(rooms.begin(), rooms.end(),
              [favouritesFirst, unreadFirst, &isCall](const Row &a, const Row &b) {
                  if (isCall(a) != isCall(b))
                      return isCall(a);
                  if (favouritesFirst && a.favourite != b.favourite)
                      return a.favourite;
                  if (a.lowPriority != b.lowPriority)
                      return !a.lowPriority;
                  if (unreadFirst && a.hasUnread != b.hasUnread)
                      return a.hasUnread;
                  return byRecency(a, b);
              });
    if (m_sortPending)
        return;
    const auto byTarget = [favouritesFirst, unreadFirst, &isCall](const Row &a, const Row &b) {
        if (isCall(a) != isCall(b))
            return isCall(a);
        if (favouritesFirst && a.favourite != b.favourite)
            return a.favourite;
        if (a.lowPriority != b.lowPriority)
            return !a.lowPriority;
        if (unreadFirst && a.hasUnread != b.hasUnread)
            return a.hasUnread;
        return conversation::moreRecent(a.targetActivity, a.name, a.id,
                                        b.targetActivity, b.name, b.id);
    };
    if (!std::is_sorted(rooms.begin(), rooms.end(), byTarget))
        m_sortPending = true;
}

void SpaceChannelModel::sortSpaceChannels(QVector<Row> &rooms)
{
    if (m_sortMode != conversation::SortBySpaceOrder) {
        sortGroup(rooms, true);
        return;
    }
    std::stable_sort(rooms.begin(), rooms.end(),
                     [this](const Row &a, const Row &b) {
                         const bool aCall = !m_callRoomId.isEmpty()
                                            && a.id == m_callRoomId;
                         const bool bCall = !m_callRoomId.isEmpty()
                                            && b.id == m_callRoomId;
                         if (aCall != bCall)
                             return aCall;
                         if (a.favourite != b.favourite)
                             return a.favourite;
                         return !a.lowPriority && b.lowPriority;
                     });
}

void SpaceChannelModel::scheduleRebuild()
{
    m_rebuildCoalesce.start();
}

void SpaceChannelModel::setSources(MatrixClient *client, SpaceManager *spaces,
                                   RailLayoutStore *layout)
{
    m_rebuildCoalesce.stop();
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    if (m_spaces)
        disconnect(m_spaces, nullptr, this, nullptr);
    if (m_layout)
        disconnect(m_layout, nullptr, this, nullptr);
    m_client = client;
    m_hold.release();
    m_hoveredRoomId.clear();
    m_directAvatars->setClient(client);
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
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &SpaceChannelModel::scheduleRebuild);
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
            m_hoveredRoomId.clear();
            m_hold.release();
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
    if (m_settings) {
        connect(m_settings, &QObject::destroyed, this, [this] {
            m_settings = nullptr;
            m_collapsedLoaded = false;
            m_collapsed.clear();
        });
        connect(m_settings, &SettingsManager::roomNotificationModeChanged, this,
                [this](const QString &) { scheduleRebuild(); });
        connect(m_settings, &SettingsManager::sessionChanged, this, [this] {
            m_collapsedLoaded = false;
            m_collapsed.clear();
            m_hoveredRoomId.clear();
            rebuild();
            Q_EMIT keepImportantRoomsVisibleChanged();
        });
    }
    m_collapsedLoaded = false;
    m_collapsed.clear();
    rebuild();
}

void SpaceChannelModel::setFilterMode(int mode)
{
    const int clamped = (mode < 0 || mode > 4) ? 0 : mode;
    if (m_filterMode == clamped)
        return;
    m_filterMode = clamped;
    Q_EMIT filterModeChanged();
    m_hold.release();
    rebuild();
}

void SpaceChannelModel::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query)
        return;
    m_searchQuery = query;
    Q_EMIT searchQueryChanged();
    m_hold.release();
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
    m_hoveredRoomId.clear();
    m_peopleView = spaceId == peopleViewId();
    m_scopeSpaceId =
        spaceId.startsWith(QLatin1Char('!')) ? spaceId : QString();
    Q_EMIT scopeSpaceIdChanged();
    m_hold.release();
    rebuild();
}

QStringList SpaceChannelModel::childSpacesOf(
    const QString &spaceId, const QHash<QString, RoomInfo> &byId) const
{
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

bool SpaceChannelModel::filterAdmits(bool isDirect, bool unread, bool favourite) const
{
    switch (m_filterMode) {
    case 1:
        return isDirect;
    case 2:
        return !isDirect;
    case 3:
        return unread;
    case 4:
        return favourite;
    default:
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
    rebuild();
}

QStringList SpaceChannelModel::roomIdsBetween(int first, int last) const
{
    QStringList ids;
    if (first < 0 || last < 0) return ids;
    for (int row = qMin(first, last); row <= qMin(qMax(first, last), rowCount() - 1); ++row) {
        if (data(index(row), KindRole).toString() != QLatin1String("room")) continue;
        const auto id = data(index(row), RoomIdRole).toString();
        if (!id.isEmpty()) ids.append(id);
    }
    return ids;
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
        { ThreadUnreadKindRole, "threadUnreadKind" },
        { CollapsedRole, "collapsed" },
        { HiddenUnreadRole, "hiddenUnread" },
        { HiddenHighlightRole, "hiddenHighlight" },
        { IsFavouriteRole, "isFavourite" },
        { IsLowPriorityRole, "isLowPriority" },
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
    case ThreadUnreadKindRole:
        return row.threadUnreadKind;
    case CollapsedRole:
        return (row.kind == SpaceKind || row.kind == GroupKind)
               && isCollapsed(row.id);
    case HiddenUnreadRole:
        return row.hiddenUnread;
    case HiddenHighlightRole:
        return row.hiddenHighlight;
    case IsFavouriteRole:
        return row.favourite;
    case IsLowPriorityRole:
        return row.lowPriority;
    case IconNameRole:
        return row.iconName;
    default:
        return {};
    }
}

int SpaceChannelModel::attentionRowFrom(int fromRow, int step,
                                        bool mentionsOnly) const
{
    if (step == 0)
        return -1;
    step = step > 0 ? 1 : -1;
    const int rows = int(m_rows.size());
    for (int i = fromRow; i >= 0 && i < rows; i += step) {
        const Row &row = m_rows.at(i);
        if (row.kind == RoomKind) {
            if (row.isInvite)
                continue;
            if (mentionsOnly ? row.highlight > 0
                             : (row.unread > 0 || row.highlight > 0))
                return i;
        } else if ((row.kind == SpaceKind || row.kind == GroupKind)
                   && isCollapsed(row.id)) {
            if (mentionsOnly ? row.hiddenHighlight > 0
                             : (row.hiddenUnread > 0 || row.hiddenHighlight > 0))
                return i;
        }
    }
    return -1;
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
    const bool searching = !m_searchQuery.trimmed().isEmpty();
    const bool collapsed = !searching && isCollapsed(header.id);
    const bool keepImportant = collapsed && keepImportantRoomsVisible();

    QVector<Row> kept;
    int hiddenUnread = 0;
    int hiddenHighlight = 0;
    bool anyHiddenUnread = false;
    QVector<Row> visible;
    QSet<QString> visibleRoomIds;
    if (keepImportant) {
        for (const Row &row : std::as_const(rows)) {
            if (row.kind == RoomKind)
                visibleRoomIds.insert(row.id);
        }
    }
    for (Row &room : rooms) {
        if (!matchesQuery(room.name))
            continue;
        if (!room.isInvite
            && !filterAdmits(room.isDirect, room.hasUnread || room.unread > 0
                                                || room.highlight > 0, room.favourite)) {
            continue;
        }
        room.depth = 1;
        kept.append(room);
        const bool retained = keepImportant
            && (room.id == m_hold.openRoomId() || room.id == m_callRoomId
                || room.id == m_hoveredRoomId || room.hasUnread
                || room.unread > 0 || room.highlight > 0);
        if (retained) {
            if (!visibleRoomIds.contains(room.id)) {
                visible.append(room);
                visibleRoomIds.insert(room.id);
            }
        } else if (collapsed) {
            hiddenUnread += room.unread;
            hiddenHighlight += room.highlight;
            anyHiddenUnread = anyHiddenUnread || room.hasUnread;
        }
    }
    if (kept.isEmpty())
        return 0;
    if (collapsed) {
        header.hiddenUnread =
            hiddenUnread > 0 ? hiddenUnread : (anyHiddenUnread ? 1 : 0);
        header.hiddenHighlight = hiddenHighlight;
    }
    rows.append(header);
    rows.append(collapsed ? visible : kept);
    return kept.size();
}

SpaceChannelModel::Row SpaceChannelModel::roomRow(const RoomInfo &info) const
{
    Row row;
    row.id = info.id;
    row.name = info.name;
    row.kind = RoomKind;
    row.avatarUrl = m_directAvatars->avatarFor(info);
    row.identityColorKey = identityColorKey(info);
    row.isDirect = info.isDirect;
    row.isInvite = info.membership == RoomInfo::Invited;
    row.encrypted = info.encrypted && info.encryptionKnown;
    const int mode = (m_settings && !row.isInvite)
        ? m_settings->roomUnreadPolicy(info.id)
        : roomunread::kModeAllMessages;
    const bool muted = roomunread::baseMode(mode) == roomunread::kModeMute;
    row.unread = roomunread::visibleUnreadCount(info, mode);
    row.highlight = roomunread::mentionCount(info, mode);
    row.hasUnread = row.isInvite
                    || roomunread::countsAsUnread(info, mode);
    row.threadUnreadKind =
        row.isInvite ? 0 : roomunread::threadUnreadKind(info, mode);
    row.favourite = info.isFavourite;
    row.lowPriority = info.lowPriority;
    row.lastActivity = m_hold.keyFor(info.id, info.lastActivity);
    row.targetActivity = m_hold.target(info.id, info.lastActivity);
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
        if (info.isDirect) {
            directs.append(roomRow(info));
            continue;
        }
        if (m_spaces && m_spaces->roomInAnySpace(info.id))
            continue;
        unparented.append(roomRow(info));
    }
    sortGroup(invites, false);
    sortGroup(unparented, true);
    sortGroup(directs, true);

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
    sortGroup(invites, false);
    sortGroup(chats, true);

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

        QVector<Row> children;
        if (m_spaces) {
            for (const QString &childId :
                 m_spaces->directChildRoomIds(spaceId, byId)) {
                const auto childInfo = byId.constFind(childId);
                if (childInfo == byId.constEnd())
                    continue;
                if (childInfo->isDirect)
                    continue;
                children.append(roomRow(*childInfo));
            }
        }
        if (m_sortMode == conversation::SortBySpaceOrder && m_spaces) {
            const auto rank = m_spaces->childOrder(spaceId);
            std::stable_sort(children.begin(), children.end(), [&rank](const Row &a, const Row &b) {
                return rank.value(a.id, INT_MAX) < rank.value(b.id, INT_MAX);
            });
        }
        sortSpaceChannels(children);
        shown += appendGroup(rows, header, children);
    }
    shown += appendSpacePeople(rows, byId);
    return shown;
}

int SpaceChannelModel::appendSpacePeople(QVector<Row> &rows,
                                         const QHash<QString, RoomInfo> &byId)
{
    if (!m_spaces)
        return 0;
    if (m_filterMode != 1)
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
    sortGroup(people, true);
    Row header;
    header.id = spacePeopleGroupId();
    header.kind = GroupKind;
    header.name = tr("People");
    return appendGroup(rows, header, people);
}

void SpaceChannelModel::rebuild()
{
    m_rebuildCoalesce.stop();
    ++m_rebuildCount;
    QVector<Row> rows;
    m_accountHasContent = false;
    m_sortPending = false;
    if (!m_client || !m_spaces) {
        setOrderHeld(false);
        applyRows(std::move(rows));
        return;
    }

    const QList<RoomInfo> allRooms = m_client->rooms();
    {
        QSet<QString> everyId;
        everyId.reserve(allRooms.size());
        for (const RoomInfo &info : allRooms)
            everyId.insert(info.id);
        m_hold.retainOnly(everyId);
    }
    m_directAvatars->resolveMissing(allRooms);

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

    if (m_matchCount != roomsShown) {
        m_matchCount = roomsShown;
        Q_EMIT matchCountChanged();
    }

    setOrderHeld(m_sortPending);
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
