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
           && hiddenHighlight == other.hiddenHighlight;
}

SpaceChannelModel::SpaceChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // A late-arriving DM face has to reach the ROW, and the rows hold a
    // snapshot: data() reads Row::avatarUrl, so a bare dataChanged would
    // repaint the same initials. rebuild() re-reads and applyRows diffs — the
    // ids and order are identical, so it is a dataChanged over the existing
    // rows, never a reset, and nothing moves. resolveMissing() is guarded by
    // its own cache, so this cannot feed itself.
    connect(&m_directAvatars, &DirectAvatarResolver::avatarResolved, this,
            &SpaceChannelModel::rebuild);
}

void SpaceChannelModel::setSources(MatrixClient *client, SpaceManager *spaces,
                                   RailLayoutStore *layout)
{
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
    if (m_spaces) {
        // SpaceManager rebuilds on both roomsChanged and roomUpdated and
        // always announces afterwards, so this single connection covers every
        // room change AND guarantees the hierarchy is resolved first.
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &SpaceChannelModel::rebuild);
    }
    if (m_client) {
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
        connect(m_layout, &RailLayoutStore::layoutChanged, this,
                &SpaceChannelModel::rebuild);
    }
    rebuild();
}

void SpaceChannelModel::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    m_settings = settings;
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

void SpaceChannelModel::setScopeSpaceId(const QString &spaceId)
{
    // A pseudo id ("", "@orphans") is not a Space and scopes nothing — it is
    // how the rail says "Home", and Home is the whole account.
    const QString clean =
        spaceId.startsWith(QLatin1Char('!')) ? spaceId : QString();
    if (m_scopeSpaceId == clean)
        return;
    m_scopeSpaceId = clean;
    Q_EMIT scopeSpaceIdChanged();
    rebuild();
}

QStringList SpaceChannelModel::listedSpaceIds() const
{
    if (!m_spaces)
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
    if (m_scopeSpaceId.isEmpty())
        return ordered;
    if (!ordered.contains(m_scopeSpaceId)) {
        // Scoped to a Space the account no longer has. Falling back to the
        // whole list is the honest answer: an empty column would look like the
        // account had nothing in it.
        return ordered;
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

void SpaceChannelModel::rebuild()
{
    QVector<Row> rows;
    m_accountHasContent = false;
    if (!m_client || !m_spaces) {
        applyRows(std::move(rows));
        return;
    }

    Row lobby;
    lobby.kind = LobbyKind;
    lobby.name = tr("Lobby");
    Row search;
    search.kind = SearchKind;
    search.name = tr("Message Search");

    const bool searching = !m_searchQuery.trimmed().isEmpty();
    if (!searching) {
        // Both are navigation, not content: a search over rooms should not
        // leave two entries standing that match nothing.
        rows.append(lobby);
        if (m_messageSearchSupported)
            rows.append(search);
    }

    const QList<RoomInfo> allRooms = m_client->rooms();
    // Ask once per unresolved DM peer. Idempotent and bounded: a peer already
    // cached or already in flight is skipped, so running this on every rebuild
    // costs nothing after the first pass.
    m_directAvatars.resolveMissing(allRooms);
    QHash<QString, RoomInfo> byId;
    byId.reserve(allRooms.size());
    for (const RoomInfo &room : allRooms)
        byId.insert(room.id, room);

    auto roomRow = [this](const RoomInfo &info) {
        Row row;
        row.id = info.id;
        row.name = info.name;
        row.kind = RoomKind;
        row.avatarUrl = m_directAvatars.avatarFor(info);
        row.identityColorKey = identityColorKey(info);
        row.isDirect = info.isDirect;
        row.isInvite = info.membership == RoomInfo::Invited;
        // The lock glyph is a CLAIM. It is drawn only for encryption the
        // client knows about; "not established yet" gets the plain hash.
        row.encrypted = info.encrypted && info.encryptionKnown;
        row.unread = info.unreadCount;
        row.highlight = info.highlightCount;
        // An INVITE always reads as unread. It is an action waiting on the
        // user, and an invite has no unread counters of its own, so keying off
        // them alone would draw the loudest thing in the column as a quiet
        // read row.
        row.hasUnread = row.isInvite || info.hasUnreadMessages
                        || info.markedUnread || info.unreadCount > 0
                        || info.highlightCount > 0;
        row.favourite = info.isFavourite;
        return row;
    };

    // Invites first. They are not in Sable's own column, and they are here
    // anyway: this layout is the whole navigation column, so leaving invites
    // out would make an invite unreachable for anyone who chose it.
    //
    // Both account-wide groups are dropped while the column is SCOPED to one
    // Space: "every joined room no Space folder lists" is a statement about
    // the whole account, and repeating it under a Space the user just selected
    // is the "clicking a space basically does nothing" complaint.
    // A scope that no longer RESOLVES is not a scope: the Space was left while
    // it was selected, listedSpaceIds() has already fallen back to everything,
    // and dropping the account-wide groups on top of that would leave a column
    // narrowed to nothing in particular.
    const QStringList listed = listedSpaceIds();
    const bool scoped = !m_scopeSpaceId.isEmpty()
                        && listed.contains(m_scopeSpaceId);
    QVector<Row> invites;
    for (const RoomInfo &info : allRooms) {
        if (scoped || info.isSpace || info.membership != RoomInfo::Invited)
            continue;
        invites.append(roomRow(info));
    }

    // Every joined room no Space folder will list. DMs land here, which is
    // where they belong in this model: they have no Space parent, and giving
    // them a group of their own would be a second answer to the same question.
    QVector<Row> unparented;
    for (const RoomInfo &info : allRooms) {
        if (scoped || info.isSpace || info.membership != RoomInfo::Joined)
            continue;
        if (m_spaces->roomInAnySpace(info.id))
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

    // Scoped, the account-wide groups are deliberately absent, so their
    // emptiness says nothing about the account: only the Space folders below
    // can answer, and a scoped Space is itself content.
    m_accountHasContent = scoped || !invites.isEmpty() || !unparented.isEmpty();

    if (!invites.isEmpty()) {
        Row header;
        header.id = invitesGroupId();
        header.kind = GroupKind;
        header.name = tr("Invites");
        appendGroup(rows, header, invites);
    }
    if (!unparented.isEmpty()) {
        Row header;
        header.id = roomsGroupId();
        header.kind = GroupKind;
        header.name = tr("Rooms");
        appendGroup(rows, header, unparented);
    }

    // Spaces in RAIL order — the order the user dragged them into, with each
    // local folder's members inline where the folder sits. Using the rail's
    // arrangement is what keeps the two layouts from disagreeing about where a
    // Space is, and it is stable across syncs by construction. Scoped, this is
    // the selected Space and its subspaces instead.
    for (const QString &spaceId : listed) {
        const auto info = byId.constFind(spaceId);
        if (info == byId.constEnd())
            continue;
        m_accountHasContent = true;
        Row header;
        header.id = spaceId;
        header.kind = SpaceKind;
        header.name = info->name;
        header.avatarUrl = info->avatarUrl;
        header.identityColorKey = identityColorKey(*info);

        // DIRECT children only. A subspace is its own folder further down the
        // column, so listing its rooms here as well would show them twice
        // under two headings that both claim to contain them.
        QVector<Row> children;
        for (const QVariant &value :
             m_spaces->directChildRoomsDetailed(spaceId)) {
            const QVariantMap child = value.toMap();
            const auto childInfo =
                byId.constFind(child.value(QStringLiteral("roomId")).toString());
            if (childInfo == byId.constEnd())
                continue;
            children.append(roomRow(*childInfo));
        }
        appendGroup(rows, header, children);
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
