#include "models/SpaceChannelModel.h"

#include <QVariantMap>

#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"

SpaceChannelModel::SpaceChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void SpaceChannelModel::setSpaceManager(SpaceManager *spaces)
{
    if (m_spaces == spaces)
        return;
    if (m_spaces)
        disconnect(m_spaces, nullptr, this, nullptr);
    m_spaces = spaces;
    if (m_spaces) {
        // The hierarchy is authoritative and arrives from sync. Rebuilding on
        // its signal rather than polling is what keeps a channel added in
        // Element from needing a restart here.
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &SpaceChannelModel::rebuild);
    }
    rebuild();
}

void SpaceChannelModel::setRoomListModel(RoomListModel *rooms)
{
    if (m_rooms == rooms)
        return;
    if (m_rooms)
        disconnect(m_rooms, nullptr, this, nullptr);
    m_rooms = rooms;
    if (m_rooms) {
        // Favourites and DMs change on their own schedule (a tag write, a new
        // DM, an unread arriving), so this follows the room list rather than
        // only the hierarchy.
        connect(m_rooms, &QAbstractItemModel::modelReset, this,
                &SpaceChannelModel::rebuild);
        connect(m_rooms, &QAbstractItemModel::rowsInserted, this,
                &SpaceChannelModel::rebuild);
        connect(m_rooms, &QAbstractItemModel::rowsRemoved, this,
                &SpaceChannelModel::rebuild);
        connect(m_rooms, &QAbstractItemModel::dataChanged, this,
                &SpaceChannelModel::rebuild);
    }
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

void SpaceChannelModel::setSpaceId(const QString &spaceId)
{
    if (m_spaceId == spaceId)
        return;
    m_spaceId = spaceId;
    Q_EMIT spaceIdChanged();
    rebuild();
}

bool SpaceChannelModel::emptyHierarchy() const
{
    // Only the UNFILTERED emptiness counts. "This space has no channels yet"
    // is a fact about the Space; a filter that matched nothing is a fact
    // about the filter, and saying the first when the second is true sends
    // the user looking for a problem that is not there.
    return !m_spaceId.isEmpty() && m_rows.isEmpty() && m_filterMode == 0;
}

// Placed after emptyHierarchy so the filter note sits with it.
int SpaceChannelModel::appendRoomListGroup(const QString &label,
                                           bool wantDirect)
{
    if (!m_rooms)
        return 0;
    // The header goes in FIRST and is removed again if nothing followed it:
    // a "Favourites" label over an empty list is worse than no label.
    Row header;
    header.name = label;
    header.kind = SectionKind;
    m_rows.append(header);
    const int headerIndex = m_rows.size() - 1;

    int added = 0;
    const int total = m_rooms->rowCount();
    for (int i = 0; i < total; ++i) {
        const QModelIndex index = m_rooms->index(i, 0);
        // A Space is not a conversation, and an INVITE is not one either —
        // both have their own surfaces and neither belongs in a channel list.
        if (m_rooms->data(index, RoomListModel::IsSpaceRole).toBool())
            continue;
        const QString category =
            m_rooms->data(index, RoomListModel::CategoryRole).toString();
        if (category == QLatin1String("invite"))
            continue;

        const bool isDirect =
            m_rooms->data(index, RoomListModel::IsDirectRole).toBool();
        const bool favourite =
            m_rooms->data(index, RoomListModel::IsFavouriteRole).toBool();
        if (wantDirect ? !isDirect : !favourite)
            continue;
        // Favourites come first as their own group, so a favourited DM is
        // listed ONCE — there, not again under Direct messages.
        if (wantDirect && favourite)
            continue;

        const int unread =
            m_rooms->data(index, RoomListModel::UnreadCountRole).toInt();
        const int highlight =
            m_rooms->data(index, RoomListModel::HighlightCountRole).toInt();
        const bool hasUnread =
            m_rooms->data(index, RoomListModel::HasUnreadRole).toBool()
            || m_rooms->data(index, RoomListModel::MarkedUnreadRole).toBool();
        if (!filterAdmits(isDirect, hasUnread || unread > 0 || highlight > 0))
            continue;

        Row row;
        row.roomId = m_rooms->data(index, RoomListModel::RoomIdRole).toString();
        row.name = m_rooms->data(index, RoomListModel::NameRole).toString();
        row.kind = ChannelKind;
        row.depth = 1;
        row.avatarUrl =
            m_rooms->data(index, RoomListModel::AvatarUrlRole).toString();
        row.identityColorKey =
            m_rooms->data(index, RoomListModel::IdentityColorKeyRole).toString();
        row.isDirect = isDirect;
        row.encrypted =
            m_rooms->data(index, RoomListModel::EncryptedRole).toBool();
        row.unread = unread;
        row.highlight = highlight;
        row.hasUnread = hasUnread;
        m_rows.append(row);
        ++added;
    }
    if (added == 0)
        m_rows.remove(headerIndex);
    return added;
}

int SpaceChannelModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
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
        { EncryptedRole, "encrypted" },
        { UnreadCountRole, "unreadCount" },
        { HighlightCountRole, "highlightCount" },
        { HasUnreadRole, "hasUnread" },
        { CollapsedRole, "collapsed" },
        { HiddenUnreadRole, "hiddenUnread" },
        { HiddenHighlightRole, "hiddenHighlight" },
    };
}

QVariant SpaceChannelModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case RoomIdRole:
        return row.roomId;
    case NameRole:
        return row.name;
    case KindRole:
        switch (row.kind) {
        case CategoryKind:
            return QStringLiteral("category");
        case SectionKind:
            return QStringLiteral("section");
        default:
            return QStringLiteral("channel");
        }
    case DepthRole:
        return row.depth;
    case AvatarUrlRole:
        return row.avatarUrl;
    case IdentityColorKeyRole:
        return row.identityColorKey;
    case IsDirectRole:
        return row.isDirect;
    case EncryptedRole:
        return row.encrypted;
    case UnreadCountRole:
        return row.unread;
    case HighlightCountRole:
        return row.highlight;
    case HasUnreadRole:
        return row.hasUnread;
    case CollapsedRole:
        return row.kind == CategoryKind && categoryCollapsed(row.roomId);
    case HiddenUnreadRole:
        return row.hiddenUnread;
    case HiddenHighlightRole:
        return row.hiddenHighlight;
    default:
        return {};
    }
}

void SpaceChannelModel::toggleCategory(const QString &categoryId)
{
    if (categoryId.isEmpty())
        return;
    QSet<QString> &collapsed = m_collapsed[m_spaceId];
    if (collapsed.contains(categoryId))
        collapsed.remove(categoryId);
    else
        collapsed.insert(categoryId);
    // A collapse changes which rows EXIST, not just how one looks, so this is
    // a rebuild rather than a dataChanged on the header.
    rebuild();
}

bool SpaceChannelModel::categoryCollapsed(const QString &categoryId) const
{
    const auto it = m_collapsed.constFind(m_spaceId);
    return it != m_collapsed.cend() && it->contains(categoryId);
}

int SpaceChannelModel::rowForRoom(const QString &roomId) const
{
    if (roomId.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        // Categories are Spaces and a Space is never the ACTIVE room in the
        // timeline sense, so a category whose id happens to be asked for is
        // deliberately not a match: highlighting it would mark the whole
        // group as "the room you are in".
        if (m_rows.at(i).kind == ChannelKind && m_rows.at(i).roomId == roomId)
            return i;
    }
    return -1;
}

void SpaceChannelModel::appendChannels(const QString &parentId, int depth,
                                       bool append, int *unread,
                                       int *highlight)
{
    if (!m_spaces)
        return;
    // DIRECT children. childRoomsDetailed is transitive (see SpaceManager),
    // which would show every room in the tree under the top-level Space and
    // then show them AGAIN under their own category.
    const QVariantList children =
        m_spaces->directChildRoomsDetailed(parentId);
    for (const QVariant &entry : children) {
        const QVariantMap child = entry.toMap();
        const int rowUnread =
            child.value(QStringLiteral("unreadCount")).toInt();
        const int rowHighlight =
            child.value(QStringLiteral("highlightCount")).toInt();
        if (unread)
            *unread += rowUnread;
        if (highlight)
            *highlight += rowHighlight;
        if (!append)
            continue;
        const bool rowHasUnread =
            child.value(QStringLiteral("hasUnread")).toBool()
            || rowUnread > 0 || rowHighlight > 0;
        if (!filterAdmits(child.value(QStringLiteral("isDirect")).toBool(),
                          rowHasUnread)) {
            continue;
        }

        Row row;
        row.roomId = child.value(QStringLiteral("roomId")).toString();
        row.name = child.value(QStringLiteral("name")).toString();
        row.kind = ChannelKind;
        row.depth = depth;
        row.avatarUrl = child.value(QStringLiteral("avatarUrl")).toString();
        row.identityColorKey =
            child.value(QStringLiteral("identityColorKey")).toString();
        row.isDirect = child.value(QStringLiteral("isDirect")).toBool();
        row.encrypted = child.value(QStringLiteral("encrypted")).toBool();
        row.unread = rowUnread;
        row.highlight = rowHighlight;
        row.hasUnread = child.value(QStringLiteral("hasUnread")).toBool();
        m_rows.append(row);
    }
}

void SpaceChannelModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    if (!m_spaces || m_spaceId.isEmpty()) {
        endResetModel();
        Q_EMIT countChanged();
        return;
    }

    // Groups a Space hierarchy cannot contain, ABOVE it. Favourites and DMs
    // belong to the account, not to any Space, and without them this layout
    // could not reach a direct message at all — the People filter had nothing
    // to show and Favourites never appeared.
    //
    // Favourites first, matching the Classic order (invites, favourites, DMs,
    // rooms) so switching layout does not rearrange what the user knows.
    if (m_filterMode != 2)   // "Rooms" is explicitly the hierarchy only
        appendRoomListGroup(tr("Favourites"), /*wantDirect=*/false);
    if (m_filterMode != 2)
        appendRoomListGroup(tr("Direct messages"), /*wantDirect=*/true);

    // The Space's OWN direct rooms first, uncategorised. Element and Sable
    // both put these above the categories, and putting them below would make
    // a Space with one uncategorised channel look empty until you scroll.
    //
    // Under the People filter the hierarchy is skipped entirely: a channel
    // list is not people, and leaving the categories standing over nothing
    // would say the filter had failed.
    const bool wantHierarchy = m_filterMode != 1;
    if (!wantHierarchy) {
        endResetModel();
        Q_EMIT countChanged();
        return;
    }
    // A label over the Space's channels, but only when something above it
    // needed separating — with no favourites and no DMs the hierarchy is the
    // whole list and a header would be noise.
    const bool needsChannelLabel = !m_rows.isEmpty();
    const int beforeHierarchy = m_rows.size();
    if (needsChannelLabel) {
        Row header;
        header.name = tr("Channels");
        header.kind = SectionKind;
        m_rows.append(header);
    }
    appendChannels(m_spaceId, /*depth=*/needsChannelLabel ? 1 : 0,
                   /*append=*/true, nullptr, nullptr);

    // Then each direct child Space as a category, with its own direct rooms
    // nested one level under it. Deeper nesting is deliberately not
    // flattened in: it is reachable by opening the subspace, which re-roots
    // this model.
    const QVariantList categories = m_spaces->childSpacesDetailed(m_spaceId);
    // Drop the "Channels" label if the Space turned out to have neither
    // uncategorised rooms nor categories under this filter.
    if (needsChannelLabel && m_rows.size() == beforeHierarchy + 1
        && categories.isEmpty()) {
        m_rows.remove(beforeHierarchy);
    }
    for (const QVariant &entry : categories) {
        const QVariantMap category = entry.toMap();
        const QString categoryId =
            category.value(QStringLiteral("roomId")).toString();
        if (categoryId.isEmpty())
            continue;

        Row header;
        header.roomId = categoryId;
        header.name = category.value(QStringLiteral("name")).toString();
        header.kind = CategoryKind;
        header.depth = 0;
        m_rows.append(header);
        const int headerIndex = m_rows.size() - 1;

        const bool collapsed = categoryCollapsed(categoryId);
        int hiddenUnread = 0;
        int hiddenHighlight = 0;
        appendChannels(categoryId, /*depth=*/1, /*append=*/!collapsed,
                       &hiddenUnread, &hiddenHighlight);
        if (collapsed) {
            // Only meaningful while collapsed. When the rows are visible they
            // carry their own badges, and a header total on top of them would
            // double-count what the user can already see.
            m_rows[headerIndex].hiddenUnread = hiddenUnread;
            m_rows[headerIndex].hiddenHighlight = hiddenHighlight;
        }
    }

    endResetModel();
    Q_EMIT countChanged();
}
