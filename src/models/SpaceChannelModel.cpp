#include "models/SpaceChannelModel.h"

#include <QVariantMap>

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
    return !m_spaceId.isEmpty() && m_rows.isEmpty();
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
        return row.kind == CategoryKind ? QStringLiteral("category")
                                        : QStringLiteral("channel");
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

    // The Space's OWN direct rooms first, uncategorised. Element and Sable
    // both put these above the categories, and putting them below would make
    // a Space with one uncategorised channel look empty until you scroll.
    appendChannels(m_spaceId, /*depth=*/0, /*append=*/true, nullptr, nullptr);

    // Then each direct child Space as a category, with its own direct rooms
    // nested one level under it. Deeper nesting is deliberately not
    // flattened in: it is reachable by opening the subspace, which re-roots
    // this model.
    const QVariantList categories = m_spaces->childSpacesDetailed(m_spaceId);
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
