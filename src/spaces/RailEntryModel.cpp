#include "spaces/RailEntryModel.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomUnreadRule.h"
#include "models/ConversationOrder.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSet>

#include <algorithm>

namespace {
const QString kKindFolder = QStringLiteral("folder");
const QString kKindSpace = QStringLiteral("space");
constexpr int kNotificationModeAllMessages = roomunread::kModeAllMessages;

bool isPseudoId(const QString &id)
{
    return id.isEmpty() || id.startsWith(QLatin1Char('@'));
}

struct RoomActivity {
    bool unread = false;
    int mentions = 0;
    bool direct = false;
};
}

RailEntryModel::RailEntryModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_modeRefresh.setSingleShot(true);
    m_modeRefresh.setInterval(0);
    connect(&m_modeRefresh, &QTimer::timeout, this, &RailEntryModel::refresh);
}

void RailEntryModel::setFlat(bool flat)
{
    if (m_flat == flat)
        return;
    m_flat = flat;
    refresh();
}

void RailEntryModel::setSources(SpaceManager *spaces, RailLayoutStore *layout)
{
    if (m_spaces) {
        disconnect(m_spaces, nullptr, this, nullptr);
    }
    if (m_layout) {
        disconnect(m_layout, nullptr, this, nullptr);
    }
    m_spaces = spaces;
    m_layout = layout;
    if (m_spaces) {
        connect(m_spaces, &SpaceManager::spacesChanged, this,
                &RailEntryModel::refresh);
    }
    if (m_layout) {
        connect(m_layout, &RailLayoutStore::layoutChanged, this,
                &RailEntryModel::refresh);
    }
    refresh();
}

void RailEntryModel::setRoomSources(MatrixClient *client,
                                    SettingsManager *settings)
{
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_client = client;
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::roomNotificationModeChanged,
                this, [this] { m_modeRefresh.start(); });
    }
    refresh();
}

int RailEntryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_rows.size());
}

QHash<int, QByteArray> RailEntryModel::roleNames() const
{
    return {
        { EntryIdRole, "entryId" },
        { KindRole, "kind" },
        { SpaceIdRole, "spaceId" },
        { NameRole, "name" },
        { AvatarUrlRole, "avatarUrl" },
        { UnreadTotalRole, "unreadTotal" },
        { HighlightTotalRole, "highlightTotal" },
        { LevelRole, "level" },
        { FolderIdRole, "folderId" },
        { CollapsedRole, "collapsed" },
        { ChildCountRole, "childCount" },
        { MemberPreviewRole, "memberPreview" },
        { PseudoRole, "pseudo" },
        { HierarchyChildRole, "hierarchyChild" },
        { ExpandableRole, "expandable" },
        { ExpandedRole, "expanded" },
        { DraggedRole, "dragged" },
        { DropTargetRole, "dropTarget" },
        { FolderLastRole, "folderLast" },
        { DraggableRole, "draggable" },
        { BandPrevLevelRole, "bandPrevLevel" },
        { BandNextLevelRole, "bandNextLevel" },
        { HasUnreadRole, "hasUnread" },
        { MentionCountRole, "mentionCount" },
    };
}

QVariant RailEntryModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const QVariantMap &row = m_rows.at(index.row());
    const QString entryId = row.value(QStringLiteral("entryId")).toString();
    switch (role) {
    case EntryIdRole:
        return entryId;
    case KindRole:
        return row.value(QStringLiteral("kind"));
    case SpaceIdRole:
        return row.value(QStringLiteral("spaceId"));
    case NameRole:
        return row.value(QStringLiteral("name"));
    case AvatarUrlRole:
        return row.value(QStringLiteral("avatarUrl"));
    case UnreadTotalRole:
        return row.value(QStringLiteral("unreadTotal"), 0);
    case HighlightTotalRole:
        return row.value(QStringLiteral("highlightTotal"), 0);
    case LevelRole:
        return row.value(QStringLiteral("level"), 0);
    case FolderIdRole:
        return row.value(QStringLiteral("folderId"), QString());
    case CollapsedRole:
        return row.value(QStringLiteral("collapsed"), false);
    case ChildCountRole:
        return row.value(QStringLiteral("childCount"), 0);
    case MemberPreviewRole:
        return row.value(QStringLiteral("memberPreview"), QVariantList{});
    case PseudoRole:
        return row.value(QStringLiteral("pseudo"), false);
    case HierarchyChildRole:
        return row.value(QStringLiteral("hierarchyChild"), false);
    case ExpandableRole:
        return row.value(QStringLiteral("expandable"), false);
    case ExpandedRole:
        return row.value(QStringLiteral("expanded"), false);
    case FolderLastRole:
        return row.value(QStringLiteral("folderLast"), false);
    case DraggableRole:
        return row.value(QStringLiteral("draggable"), false);
    case BandPrevLevelRole:
        return row.value(QStringLiteral("bandPrevLevel"), -1);
    case BandNextLevelRole:
        return row.value(QStringLiteral("bandNextLevel"), -1);
    case HasUnreadRole:
        return row.value(QStringLiteral("hasUnread"), false);
    case MentionCountRole:
        return row.value(QStringLiteral("mentionCount"), 0);
    case DraggedRole:
        return m_dragging && !entryId.isEmpty() && entryId == m_dragEntryId;
    case DropTargetRole:
        return m_grouping && !entryId.isEmpty() && entryId == m_dropTargetId;
    default:
        return {};
    }
}

void RailEntryModel::setPeopleEntryVisible(bool visible)
{
    if (m_peopleEntryVisible == visible)
        return;
    m_peopleEntryVisible = visible;
    Q_EMIT peopleEntryVisibleChanged();
    refresh();
}

void RailEntryModel::setOrphansEntryVisible(bool visible)
{
    if (m_orphansEntryVisible == visible)
        return;
    m_orphansEntryVisible = visible;
    Q_EMIT orphansEntryVisibleChanged();
    refresh();
}

bool RailEntryModel::railIsManual() const
{
    return !m_layout
           || m_layout->sortMode() == conversation::RailSortManual;
}

void RailEntryModel::refresh()
{
    if (m_dragging) {
        m_refreshPending = true;
        return;
    }
    if (!m_spaces || !m_layout) {
        applyRows({});
        return;
    }

    const QVariantList spaces = m_spaces->allSpaces();
    QVariantList topLevelInput;
    QHash<QString, QVariantMap> byId;
    for (const QVariant &value : spaces) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value(QStringLiteral("spaceId")).toString();
        if (isPseudoId(id)) {
            topLevelInput.append(entry);
            continue;
        }
        byId.insert(id, entry);
        if (entry.value(QStringLiteral("parentSpaceId")).toString().isEmpty())
            topLevelInput.append(entry);
    }

    const QVariantList arranged = m_layout->arrange(topLevelInput);
    QVector<QVariantMap> rows;
    rows.reserve(arranged.size() + 4);
    const bool peopleWanted = m_peopleEntryVisible;
    bool peopleInserted = false;
    auto insertPeople = [&rows, this] {
        QVariantMap people;
        people.insert(QStringLiteral("kind"), kKindSpace);
        people.insert(QStringLiteral("entryId"), SpaceManager::peopleId());
        people.insert(QStringLiteral("spaceId"), SpaceManager::peopleId());
        people.insert(QStringLiteral("name"), tr("Direct Messages"));
        people.insert(QStringLiteral("folderId"), QString());
        people.insert(QStringLiteral("pseudo"), true);
        people.insert(QStringLiteral("hierarchyChild"), false);
        people.insert(QStringLiteral("draggable"), false);
        people.insert(QStringLiteral("expandable"), false);
        people.insert(QStringLiteral("expanded"), false);
        const int unread = m_spaces ? m_spaces->peopleUnreadTotal() : 0;
        const int highlight = m_spaces ? m_spaces->peopleHighlightTotal() : 0;
        people.insert(QStringLiteral("unreadTotal"), unread);
        people.insert(QStringLiteral("highlightTotal"), highlight);
        rows.append(people);
    };
    for (const QVariant &value : arranged) {
        QVariantMap entry = value.toMap();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString spaceId = entry.value(QStringLiteral("spaceId")).toString();
        const bool folder = kind == kKindFolder;
        const bool pseudo = !folder && isPseudoId(spaceId);
        if (!m_orphansEntryVisible && pseudo
            && spaceId == SpaceManager::orphansId())
            continue;
        if (kind.isEmpty()) {
            entry.insert(QStringLiteral("kind"), kKindSpace);
            entry.insert(QStringLiteral("entryId"), spaceId);
            entry.insert(QStringLiteral("folderId"), QString());
        }
        entry.insert(QStringLiteral("pseudo"), pseudo);
        entry.insert(QStringLiteral("hierarchyChild"), false);
        // A folder is arrangeable; a pseudo row never is.
        // In A-Z / Recent activity the order is derived, so a drop could not
        // land where it was made; the rail is not draggable until Manual.
        entry.insert(QStringLiteral("draggable"), !pseudo && railIsManual());
        const int childSpaces =
            entry.value(QStringLiteral("childSpaceCount")).toInt();
        const int directRooms =
            entry.value(QStringLiteral("directChildRoomCount")).toInt();
        entry.insert(QStringLiteral("expandable"),
                     !m_flat && !folder && !pseudo
                         && (childSpaces > 0 || directRooms > 0));
        entry.insert(QStringLiteral("expanded"),
                     !m_flat && !folder && !pseudo
                         && m_layout->spaceExpanded(spaceId));
        rows.append(entry);
        if (peopleWanted && !peopleInserted && pseudo
            && spaceId == SpaceManager::allRoomsId()) {
            peopleInserted = true;
            insertPeople();
        }
        if (!m_flat && !folder && !pseudo) {
            appendSubspaces(spaceId,
                            entry.value(QStringLiteral("folderId")).toString(),
                            byId, rows, 1);
        }
    }
    if (peopleWanted && !peopleInserted)
        insertPeople();
    stampActivity(rows);
    applyRows(std::move(rows));
}

void RailEntryModel::stampActivity(QVector<QVariantMap> &rows) const
{
    QHash<QString, RoomActivity> rooms;
    if (m_client) {
        const QList<RoomInfo> all = m_client->rooms();
        rooms.reserve(all.size());
        for (const RoomInfo &r : all) {
            if (r.isSpace || r.membership != RoomInfo::Joined)
                continue;
            RoomActivity activity;
            activity.direct = r.isDirect;
            const bool unread = roomunread::rawUnread(r);
            const int mode = unread && m_settings
                            ? m_settings->roomUnreadPolicy(r.id)
                            : kNotificationModeAllMessages;
            activity.unread = unread && roomunread::countsAsUnread(r, mode);
            activity.mentions = roomunread::mentionCount(r, mode);
            if (r.isDirect
                && roomunread::baseMode(mode) == kNotificationModeAllMessages
                && !r.unreadThreadsOnly)
                activity.mentions = std::max(r.unreadCount, r.highlightCount);
            rooms.insert(r.id, activity);
        }
    }

    const auto inAnySpace = [this](const QString &roomId) {
        return m_spaces && m_spaces->roomInAnySpace(roomId);
    };
    const auto pseudoLists = [&](const QString &spaceId, const QString &roomId,
                                 const RoomActivity &activity) {
        if (spaceId == SpaceManager::allRoomsId()) {
            return !m_peopleEntryVisible || activity.direct
                   || !inAnySpace(roomId);
        }
        if (spaceId == SpaceManager::peopleId())
            return activity.direct;
        if (spaceId == SpaceManager::orphansId())
            return !activity.direct && !inAnySpace(roomId);
        return false;
    };

    for (QVariantMap &row : rows) {
        QSet<QString> listed;
        const QString spaceId = row.value(QStringLiteral("spaceId")).toString();
        if (row.value(QStringLiteral("kind")).toString() == kKindFolder) {
            if (m_spaces) {
                const QStringList members =
                    row.value(QStringLiteral("memberIds")).toStringList();
                for (const QString &member : members) {
                    for (const QString &roomId : m_spaces->roomsInSpace(member))
                        listed.insert(roomId);
                }
            }
        } else if (isPseudoId(spaceId)) {
            for (auto it = rooms.constBegin(); it != rooms.constEnd(); ++it) {
                if (pseudoLists(spaceId, it.key(), it.value()))
                    listed.insert(it.key());
            }
        } else if (m_spaces) {
            for (const QString &roomId : m_spaces->roomsInSpace(spaceId))
                listed.insert(roomId);
        }

        bool anyUnread = false;
        int mentions = 0;
        for (const QString &roomId : std::as_const(listed)) {
            const auto it = rooms.constFind(roomId);
            if (it == rooms.constEnd())
                continue;
            anyUnread = anyUnread || it->unread;
            mentions += it->mentions;
        }
        row.insert(QStringLiteral("hasUnread"), anyUnread);
        row.insert(QStringLiteral("mentionCount"), mentions);
    }
}

void RailEntryModel::appendSubspaces(const QString &parentId,
                                     const QString &owningFolderId,
                                     const QHash<QString, QVariantMap> &byId,
                                     QVector<QVariantMap> &rows, int depth)
{
    if (!m_spaces || !m_layout || depth > kMaxHierarchyDepth)
        return;
    if (!m_layout->spaceExpanded(parentId))
        return;
    const QStringList children = m_layout->sortedForMode(
        m_layout->orderedChildren(parentId, m_spaces->childSpaceIds(parentId)),
        byId);
    for (const QString &childId : children) {
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd())
            continue;
        QVariantMap entry = *it;
        entry.insert(QStringLiteral("kind"), kKindSpace);
        entry.insert(QStringLiteral("entryId"), childId);
        entry.insert(QStringLiteral("folderId"), owningFolderId);
        entry.insert(QStringLiteral("folderLast"), false);
        entry.insert(QStringLiteral("pseudo"), false);
        entry.insert(QStringLiteral("hierarchyChild"), true);
        // Draggable among its own siblings only; legalGap() prevents a
        // reparent, which is Matrix state and may need power we lack.
        // Not in an automatic order: a drag would write an order nothing shows.
        entry.insert(QStringLiteral("draggable"), railIsManual());
        const int childSpaces =
            entry.value(QStringLiteral("childSpaceCount")).toInt();
        const int directRooms =
            entry.value(QStringLiteral("directChildRoomCount")).toInt();
        entry.insert(QStringLiteral("expandable"),
                     childSpaces > 0 || directRooms > 0);
        entry.insert(QStringLiteral("expanded"),
                     m_layout->spaceExpanded(childId));
        rows.append(entry);
        appendSubspaces(childId, owningFolderId, byId, rows, depth + 1);
    }
}

void RailEntryModel::stampGroupField(QVector<QVariantMap> &rows)
{
    for (int i = 0; i < rows.size(); ++i) {
        const int level = rows.at(i).value(QStringLiteral("level")).toInt();
        const int prevLevel =
            i > 0 ? rows.at(i - 1).value(QStringLiteral("level")).toInt() : -1;
        const int nextLevel =
            i + 1 < rows.size()
            ? rows.at(i + 1).value(QStringLiteral("level")).toInt() : -1;
        rows[i].insert(QStringLiteral("bandPrevLevel"), prevLevel);
        rows[i].insert(QStringLiteral("bandNextLevel"), nextLevel);
    }
}

void RailEntryModel::stampFolderRuns(QVector<QVariantMap> &rows)
{
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).value(QStringLiteral("kind")).toString() == kKindFolder)
            continue;
        const QString owner =
            rows.at(i).value(QStringLiteral("folderId")).toString();
        bool isLast = false;
        if (!owner.isEmpty()) {
            isLast = true;
            for (int j = i + 1; j < rows.size(); ++j) {
                if (rows.at(j).value(QStringLiteral("kind")).toString()
                    == kKindFolder)
                    break;
                if (rows.at(j).value(QStringLiteral("folderId")).toString()
                    == owner) {
                    isLast = false;
                }
                break;
            }
        }
        rows[i].insert(QStringLiteral("folderLast"), isLast);
    }
}

void RailEntryModel::applyRows(QVector<QVariantMap> rows)
{
    stampGroupField(rows);
    stampFolderRuns(rows);
    if (rows.size() == m_rows.size()) {
        bool sameIds = true;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).value(QStringLiteral("entryId"))
                != m_rows.at(i).value(QStringLiteral("entryId"))) {
                sameIds = false;
                break;
            }
        }
        if (sameIds) {
            // Same rows, possibly changed counts or names. A reset would
            // rebuild every delegate (and its avatar fetch) per message.
            if (rows == m_rows)
                return;
            m_rows = std::move(rows);
            Q_EMIT dataChanged(index(0, 0), index(m_rows.size() - 1, 0));
            return;
        }
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countChanged();
}

int RailEntryModel::rowForEntry(const QString &entryId) const
{
    if (entryId.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).value(QStringLiteral("entryId")).toString() == entryId)
            return i;
    }
    return -1;
}

QVariantMap RailEntryModel::entryAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

bool RailEntryModel::rowIsFolder(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return false;
    return m_rows.at(row).value(QStringLiteral("kind")).toString()
           == kKindFolder;
}

int RailEntryModel::blockLength(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return 0;
    if (rowIsFolder(row)) {
        const QString folderId =
            m_rows.at(row).value(QStringLiteral("entryId")).toString();
        int length = 1;
        while (row + length < m_rows.size()
               && m_rows.at(row + length)
                          .value(QStringLiteral("folderId")).toString()
                      == folderId) {
            ++length;
        }
        return length;
    }
    const int level = m_rows.at(row).value(QStringLiteral("level")).toInt();
    int length = 1;
    while (row + length < m_rows.size()
           && m_rows.at(row + length)
                      .value(QStringLiteral("hierarchyChild")).toBool()
           && m_rows.at(row + length).value(QStringLiteral("level")).toInt()
                  > level) {
        ++length;
    }
    return length;
}

QVector<QString> RailEntryModel::folderOwners(const QString &draggedId) const
{
    QVector<QString> owners(m_rows.size());
    QString run;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QVariantMap &row = m_rows.at(i);
        if (row.value(QStringLiteral("pseudo")).toBool()) {
            run.clear();
            continue;
        }
        if (rowIsFolder(i)) {
            run = row.value(QStringLiteral("entryId")).toString();
            owners[i] = run;
            continue;
        }
        if (row.value(QStringLiteral("hierarchyChild")).toBool()) {
            owners[i] = i > 0 ? owners.at(i - 1) : QString();
            continue;
        }
        const QString owning = row.value(QStringLiteral("folderId")).toString();
        const QString entryId = row.value(QStringLiteral("entryId")).toString();
        if (!run.isEmpty() && (owning == run || entryId == draggedId)) {
            owners[i] = run;
            continue;
        }
        run.clear();
    }
    return owners;
}

void RailEntryModel::refreshFolderRuns()
{
    const QVector<QString> owners = folderOwners(m_dragEntryId);
    int first = -1;
    int last = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (rowIsFolder(i))
            continue;
        const QString owner = owners.at(i);
        const bool isLast =
            !owner.isEmpty()
            && (i + 1 >= m_rows.size() || owners.at(i + 1) != owner);
        const QString currentOwner =
            m_rows.at(i).value(QStringLiteral("folderId")).toString();
        const bool currentLast =
            m_rows.at(i).value(QStringLiteral("folderLast")).toBool();
        if (currentOwner == owner && currentLast == isLast)
            continue;
        m_rows[i].insert(QStringLiteral("folderId"), owner);
        m_rows[i].insert(QStringLiteral("folderLast"), isLast);
        if (first < 0)
            first = i;
        last = i;
    }
    if (first >= 0) {
        Q_EMIT dataChanged(index(first, 0), index(last, 0),
                           { FolderIdRole, FolderLastRole });
    }
}

bool RailEntryModel::beginDrag(const QString &entryId)
{
    const int row = rowForEntry(entryId);
    if (row < 0)
        return false;
    if (!m_rows.at(row).value(QStringLiteral("draggable")).toBool())
        return false;
    m_dragging = true;
    m_dragEntryId = entryId;
    m_dropTargetId.clear();
    m_grouping = false;
    m_refreshPending = false;
    Q_EMIT dataChanged(index(row, 0), index(row, 0), { DraggedRole });
    Q_EMIT dragChanged();
    return true;
}

int RailEntryModel::legalGap(int gap) const
{
    const int dragRow = rowForEntry(m_dragEntryId);
    if (dragRow < 0)
        return -1;
    int g = qBound(0, gap, int(m_rows.size()));
    int firstMovable = 0;
    while (firstMovable < m_rows.size()
           && m_rows.at(firstMovable).value(QStringLiteral("pseudo")).toBool()) {
        ++firstMovable;
    }
    if (m_rows.at(dragRow).value(QStringLiteral("hierarchyChild")).toBool()) {
        const int level = m_rows.at(dragRow).value(QStringLiteral("level"))
                              .toInt();
        int parentRow = dragRow - 1;
        while (parentRow >= 0
               && m_rows.at(parentRow).value(QStringLiteral("level")).toInt()
                      >= level) {
            --parentRow;
        }
        const int runStart = parentRow + 1;
        int runEnd = runStart;
        while (runEnd < m_rows.size()
               && m_rows.at(runEnd).value(QStringLiteral("hierarchyChild"))
                          .toBool()
               && m_rows.at(runEnd).value(QStringLiteral("level")).toInt()
                      >= level) {
            ++runEnd;
        }
        g = qBound(runStart, g, runEnd);
        while (g > runStart && g < runEnd
               && m_rows.at(g).value(QStringLiteral("level")).toInt() != level) {
            --g;
        }
        return g;
    }
    if (g < firstMovable)
        return firstMovable;

    const auto nearerBoundary = [this](int g, int lo, auto inRun) -> int {
        const int hi = int(m_rows.size());
        if (g <= lo || g >= hi || !inRun(g))
            return g;
        int up = g;
        while (up > lo && inRun(up))
            --up;
        int down = g;
        while (down < hi && inRun(down))
            ++down;
        return (g - up) <= (down - g) ? up : down;
    };

    g = nearerBoundary(g, firstMovable, [this](int row) {
        return m_rows.at(row).value(QStringLiteral("hierarchyChild")).toBool();
    });
    if (!rowIsFolder(dragRow))
        return g;
    g = nearerBoundary(g, firstMovable, [this](int row) {
        return !m_rows.at(row).value(QStringLiteral("folderId")).toString()
                    .isEmpty()
               && !rowIsFolder(row);
    });
    return g;
}

void RailEntryModel::moveBlock(int from, int count, int to)
{
    if (count <= 0 || from < 0 || from + count > m_rows.size())
        return;
    to = qBound(0, to, m_rows.size() - count);
    if (to == from)
        return;
    const int destination = to > from ? to + count : to;
    if (!beginMoveRows(QModelIndex(), from, from + count - 1, QModelIndex(),
                       destination)) {
        return;
    }
    QVector<QVariantMap> block;
    block.reserve(count);
    for (int i = 0; i < count; ++i)
        block.append(m_rows.at(from + i));
    m_rows.remove(from, count);
    for (int i = 0; i < count; ++i)
        m_rows.insert(to + i, block.at(i));
    endMoveRows();
}

void RailEntryModel::hoverGroup(int row)
{
    if (!m_dragging)
        return;
    const int dragRow = rowForEntry(m_dragEntryId);
    if (dragRow < 0)
        return;
    if (m_rows.isEmpty())
        return;
    const int hovered = qBound(0, row, m_rows.size() - 1);
    const int length = blockLength(dragRow);
    const QVariantMap &target = m_rows.at(hovered);
    const bool eligible =
        (hovered < dragRow || hovered >= dragRow + length)
        && !target.value(QStringLiteral("pseudo")).toBool()
        && !target.value(QStringLiteral("hierarchyChild")).toBool()
        && !rowIsFolder(dragRow)
        && !m_rows.at(dragRow).value(QStringLiteral("hierarchyChild")).toBool();
    if (!eligible) {
        clearDropTarget();
        return;
    }
    const QString targetId = target.value(QStringLiteral("entryId")).toString();
    if (targetId == m_dropTargetId && m_grouping)
        return;
    const int previous = rowForEntry(m_dropTargetId);
    m_grouping = true;
    m_dropTargetId = targetId;
    if (previous >= 0) {
        Q_EMIT dataChanged(index(previous, 0), index(previous, 0),
                           { DropTargetRole });
    }
    Q_EMIT dataChanged(index(hovered, 0), index(hovered, 0),
                       { DropTargetRole });
    Q_EMIT dragChanged();
}

void RailEntryModel::revealSpace(const QString &spaceId)
{
    if (spaceId.isEmpty() || !m_spaces || !m_layout)
        return;
    const QStringList ancestors = m_spaces->ancestorSpaceIds(spaceId);
    for (int i = ancestors.size() - 1; i >= 0; --i) {
        if (!m_layout->spaceExpanded(ancestors.at(i)))
            m_layout->setSpaceExpanded(ancestors.at(i), true);
    }
    if (!m_layout->spaceExpanded(spaceId))
        m_layout->setSpaceExpanded(spaceId, true);
    refresh();
    Q_EMIT revealRequested(spaceId);
}

void RailEntryModel::hoverGap(int gap)
{
    if (!m_dragging)
        return;
    const int dragRow = rowForEntry(m_dragEntryId);
    if (dragRow < 0)
        return;
    clearDropTarget();

    const int length = blockLength(dragRow);
    const int g = legalGap(gap);
    if (g < 0)
        return;
    if (g >= dragRow && g <= dragRow + length)
        return;
    const int to = (g > dragRow) ? g - length : g;
    moveBlock(dragRow, length, to);
    refreshFolderRuns();
}

void RailEntryModel::clearDropTarget()
{
    if (!m_dragging || !m_grouping)
        return;
    const int previous = rowForEntry(m_dropTargetId);
    m_grouping = false;
    m_dropTargetId.clear();
    if (previous >= 0) {
        Q_EMIT dataChanged(index(previous, 0), index(previous, 0),
                           { DropTargetRole });
    }
    Q_EMIT dragChanged();
}

void RailEntryModel::endDrag(bool commit)
{
    if (!m_dragging)
        return;
    const bool grouped = m_grouping;
    const QString dragged = m_dragEntryId;
    const QString target = m_dropTargetId;
    const int finalRow = rowForEntry(dragged);

    m_dragging = false;
    m_grouping = false;
    m_dragEntryId.clear();
    m_dropTargetId.clear();
    Q_EMIT dragChanged();
    if (!m_rows.isEmpty()) {
        Q_EMIT dataChanged(index(0, 0), index(m_rows.size() - 1, 0),
                           { DraggedRole, DropTargetRole });
    }

    if (!commit || finalRow < 0 || !m_layout) {
        refresh();
        return;
    }
    if (grouped && !target.isEmpty()) {
        commitGrouping(dragged, target);
        refresh();
        return;
    }
    if (finalRow >= 0 && finalRow < m_rows.size()
        && m_rows.at(finalRow).value(QStringLiteral("hierarchyChild"))
               .toBool()) {
        commitChildOrder(dragged);
        refresh();
        return;
    }
    commitReorder(dragged);
}

void RailEntryModel::commitChildOrder(const QString &dragged)
{
    const int row = rowForEntry(dragged);
    if (row < 0 || !m_layout)
        return;
    const int level = m_rows.at(row).value(QStringLiteral("level")).toInt();
    if (level < 1)
        return;
    int parentRow = row - 1;
    while (parentRow >= 0
           && m_rows.at(parentRow).value(QStringLiteral("level")).toInt()
                  >= level) {
        --parentRow;
    }
    if (parentRow < 0)
        return;
    const QString parentId =
        m_rows.at(parentRow).value(QStringLiteral("spaceId")).toString();
    if (parentId.isEmpty())
        return;
    QStringList children;
    for (int i = parentRow + 1; i < m_rows.size(); ++i) {
        const QVariantMap &r = m_rows.at(i);
        if (!r.value(QStringLiteral("hierarchyChild")).toBool())
            break;
        const int l = r.value(QStringLiteral("level")).toInt();
        if (l < level)
            break;
        if (l != level)
            continue;
        const QString id = r.value(QStringLiteral("spaceId")).toString();
        if (!id.isEmpty() && !children.contains(id))
            children.append(id);
    }
    m_layout->setChildOrder(parentId, children);
}

void RailEntryModel::commitGrouping(const QString &dragged,
                                    const QString &target)
{
    bool targetIsFolder = false;
    for (const QVariant &value : m_layout->folders()) {
        if (value.toMap().value(QStringLiteral("id")).toString() == target) {
            targetIsFolder = true;
            break;
        }
    }
    if (targetIsFolder) {
        m_layout->moveSpaceToFolder(dragged, target, -1);
        return;
    }
    const QString existing = m_layout->folderOf(target);
    if (!existing.isEmpty()) {
        const QStringList members = m_layout->folderMembers(existing);
        m_layout->moveSpaceToFolder(dragged, existing,
                                    members.indexOf(target) + 1);
        return;
    }
    QStringList top;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QVariantMap &row = m_rows.at(i);
        if (row.value(QStringLiteral("pseudo")).toBool()
            || row.value(QStringLiteral("hierarchyChild")).toBool()) {
            continue;
        }
        if (!rowIsFolder(i)
            && !row.value(QStringLiteral("folderId")).toString().isEmpty()) {
            continue;
        }
        top.append(row.value(QStringLiteral("entryId")).toString());
    }
    m_layout->createFolderWithSpaces({ target, dragged }, top.indexOf(target),
                                     QString());
}

void RailEntryModel::commitReorder(const QString &dragged)
{
    if (!m_layout) {
        refresh();
        return;
    }
    const QVector<QString> owners = folderOwners(dragged);
    QStringList topLevel;
    QHash<QString, QStringList> members;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QVariantMap &row = m_rows.at(i);
        if (row.value(QStringLiteral("pseudo")).toBool()
            || row.value(QStringLiteral("hierarchyChild")).toBool()) {
            continue;
        }
        const QString entryId = row.value(QStringLiteral("entryId")).toString();
        if (entryId.isEmpty())
            continue;
        if (rowIsFolder(i)) {
            topLevel.append(entryId);
            continue;
        }
        const QString owner = owners.at(i);
        if (owner.isEmpty())
            topLevel.append(entryId);
        else
            members[owner].append(entryId);
    }
    QVariantMap folderMembers;
    for (auto it = members.constBegin(); it != members.constEnd(); ++it)
        folderMembers.insert(it.key(), it.value());
    m_layout->applyArrangement(topLevel, folderMembers);
    refresh();
}
