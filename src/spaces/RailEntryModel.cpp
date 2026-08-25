#include "spaces/RailEntryModel.h"

#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

namespace {
const QString kKindFolder = QStringLiteral("folder");
const QString kKindSpace = QStringLiteral("space");

bool isPseudoId(const QString &id)
{
    return id.isEmpty() || id.startsWith(QLatin1Char('@'));
}
} // namespace

RailEntryModel::RailEntryModel(QObject *parent)
    : QAbstractListModel(parent)
{
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
    case DraggedRole:
        return m_dragging && !entryId.isEmpty() && entryId == m_dragEntryId;
    case DropTargetRole:
        return m_grouping && !entryId.isEmpty() && entryId == m_dropTargetId;
    default:
        return {};
    }
}

void RailEntryModel::refresh()
{
    if (m_dragging) {
        // The gesture owns the row order until it ends. Rebuilding here is
        // exactly what destroyed the delegate holding the drag.
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
        // Only ROOTS are arrangeable at the top level. A subspace is shown
        // under its parent, so handing it to arrange() would list it twice.
        if (entry.value(QStringLiteral("parentSpaceId")).toString().isEmpty())
            topLevelInput.append(entry);
    }

    const QVariantList arranged = m_layout->arrange(topLevelInput);
    QVector<QVariantMap> rows;
    rows.reserve(arranged.size() + 4);
    for (const QVariant &value : arranged) {
        QVariantMap entry = value.toMap();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString spaceId = entry.value(QStringLiteral("spaceId")).toString();
        const bool folder = kind == kKindFolder;
        const bool pseudo = !folder && isPseudoId(spaceId);
        if (kind.isEmpty()) {
            // A pseudo row comes back from arrange() exactly as it was given,
            // so it carries no kind of its own.
            entry.insert(QStringLiteral("kind"), kKindSpace);
            entry.insert(QStringLiteral("entryId"), spaceId);
            entry.insert(QStringLiteral("folderId"), QString());
        }
        entry.insert(QStringLiteral("pseudo"), pseudo);
        entry.insert(QStringLiteral("hierarchyChild"), false);
        // A folder is arrangeable; a pseudo row never is.
        entry.insert(QStringLiteral("draggable"), !pseudo);
        const int childSpaces =
            entry.value(QStringLiteral("childSpaceCount")).toInt();
        entry.insert(QStringLiteral("expandable"),
                     !folder && !pseudo && childSpaces > 0);
        entry.insert(QStringLiteral("expanded"),
                     !folder && !pseudo
                         && m_layout->spaceExpanded(spaceId));
        rows.append(entry);
        if (!folder && !pseudo) {
            appendSubspaces(spaceId,
                            entry.value(QStringLiteral("folderId")).toString(),
                            byId, rows, 1);
        }
    }
    applyRows(std::move(rows));
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
    for (const QString &childId : m_spaces->childSpaceIds(parentId)) {
        const auto it = byId.constFind(childId);
        if (it == byId.constEnd())
            continue;
        QVariantMap entry = *it;
        entry.insert(QStringLiteral("kind"), kKindSpace);
        entry.insert(QStringLiteral("entryId"), childId);
        // The subspaces of a FILED Space belong to the same folder block, or
        // dragging that folder would leave them behind: the block is found by
        // walking the folderId run after the header.
        entry.insert(QStringLiteral("folderId"), owningFolderId);
        entry.insert(QStringLiteral("folderLast"), false);
        entry.insert(QStringLiteral("pseudo"), false);
        entry.insert(QStringLiteral("hierarchyChild"), true);
        // Matrix owns this row's position. Offering to drag it would offer to
        // change a hierarchy this layer cannot change.
        entry.insert(QStringLiteral("draggable"), false);
        const int childSpaces =
            entry.value(QStringLiteral("childSpaceCount")).toInt();
        entry.insert(QStringLiteral("expandable"), childSpaces > 0);
        entry.insert(QStringLiteral("expanded"),
                     m_layout->spaceExpanded(childId));
        rows.append(entry);
        appendSubspaces(childId, owningFolderId, byId, rows, depth + 1);
    }
}

void RailEntryModel::applyRows(QVector<QVariantMap> rows)
{
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
            // Same rows, possibly different unread counts or names. A reset
            // here would tear down and rebuild every delegate — and its avatar
            // fetch — on every arriving message.
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
        // A folder owns its open members — and their own expanded subspaces,
        // which is why appendSubspaces gives them the same folderId. Moving
        // the header alone would detach it from its contents mid-gesture.
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
    // A Space carries its EXPANDED SUBSPACES with it. Those rows are Matrix's
    // arrangement under this Space; leaving them behind would strand them
    // under whatever the drag happened to move into their place.
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
    // Which folder each row would belong to IF the arrangement were committed
    // right now. ONE definition, used by both the preview (so the container
    // band follows the drag) and the commit (so what the user sees is what is
    // written). Two copies of this rule is how a drop lands somewhere the
    // preview never showed.
    //
    // The rule: a row belongs to the folder run it follows. Land after a
    // folder's header or after one of its members and you are inside; land
    // anywhere else and you are at the top level. Dropping just past a
    // folder's last member therefore APPENDS to it — the rule has to choose,
    // and appending is the choice that makes the end of a folder reachable.
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
            // A subspace inherits its ancestor's run: it is not placed by the
            // user and cannot change the grouping on its own.
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
    // Keeps the PREVIEW honest: a Space dragged out of a folder must stop
    // drawing the folder's container band immediately, and the row that is now
    // last in a run must take the rounded bottom. Without this the band tells
    // the user the drop will do something it will not.
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
    // A gap is the slot BEFORE row `gap`, so gap == rowCount is the end of the
    // rail and is always legal. Every rule below reads the row the gap sits
    // in front of.
    int g = qBound(0, gap, int(m_rows.size()));
    // A pseudo row keeps its place at the top of the rail, so nothing may be
    // dropped above one.
    int firstMovable = 0;
    while (firstMovable < m_rows.size()
           && m_rows.at(firstMovable).value(QStringLiteral("pseudo")).toBool()) {
        ++firstMovable;
    }
    if (g < firstMovable)
        return firstMovable;
    // A hierarchy child's slot belongs to Matrix; landing inside a subspace
    // run would put a user-arranged entry between a parent and its children.
    // Snap back to the gap in front of the run's owner.
    while (g > firstMovable && g < m_rows.size()
           && m_rows.at(g).value(QStringLiteral("hierarchyChild")).toBool()) {
        --g;
    }
    if (!rowIsFolder(dragRow))
        return g;
    // Dragging a FOLDER: its destination is a top-level boundary, never inside
    // another folder's member run.
    while (g > firstMovable && g < m_rows.size()
           && !m_rows.at(g).value(QStringLiteral("folderId")).toString()
                   .isEmpty()
           && !rowIsFolder(g)) {
        --g;
    }
    return g;
}

void RailEntryModel::moveBlock(int from, int count, int to)
{
    if (count <= 0 || from < 0 || from + count > m_rows.size())
        return;
    // `to` is the FINAL index the block starts at, so it cannot run past the
    // end once the block itself is out of the way.
    to = qBound(0, to, m_rows.size() - count);
    if (to == from)
        return;
    // Qt wants the destination in the ORIGINAL numbering: the row the block is
    // inserted before, which for a downward move is past the block itself.
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
        // Not the block in the user's own hand: a folder header's open members
        // travel with it, so the whole run is excluded, not just the header.
        (hovered < dragRow || hovered >= dragRow + length)
        && !target.value(QStringLiteral("pseudo")).toBool()
        && !target.value(QStringLiteral("hierarchyChild")).toBool()
        // A folder cannot go inside a folder: folders do not nest, and
        // pretending otherwise would create an arrangement the store cannot
        // represent.
        && !rowIsFolder(dragRow);
    if (!eligible) {
        // AND RETURN. The previous single-verb version fell through to the
        // reorder below when the target was ineligible, so aiming at something
        // that cannot be grouped with silently moved the dragged block instead
        // — a different outcome from the one the pointer asked for.
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

void RailEntryModel::hoverGap(int gap)
{
    if (!m_dragging)
        return;
    const int dragRow = rowForEntry(m_dragEntryId);
    if (dragRow < 0)
        return;
    // A gap is never a group: the two readings are exclusive, so arriving in
    // one has to disarm the other.
    clearDropTarget();

    const int length = blockLength(dragRow);
    const int g = legalGap(gap);
    if (g < 0)
        return;
    // The block's own slot and BOTH gaps adjacent to it are no-ops — the block
    // is already there. Without this, a gap strictly inside a multi-row block
    // would compute a destination above the block and move it.
    if (g >= dragRow && g <= dragRow + length)
        return;
    // THE CONVERSION THE ROW-INDEX VERSION NEVER HAD. moveBlock's `to` is the
    // FINAL index the block starts at, i.e. after its own rows have been taken
    // out, so a gap BELOW the block shifts up by the block's length. The old
    // code passed the hovered ROW index straight through, which is why hovering
    // the next row down always landed the block ON that row and under the
    // pointer, and why the next pointer sample read the dragged block and
    // oscillated.
    //
    // With this, a move only ever fires from a gap and the block lands adjacent
    // to that gap — so re-reading the same pointer position yields the same gap
    // and the guard above makes it a no-op. That is what makes the gesture
    // stable.
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
    // ANNOUNCE the cleared per-row flags before anything else. `refresh()`
    // below is allowed to find the rows identical and emit nothing at all —
    // which is right for the row data and catastrophic for these two roles:
    // the released tile kept rendering as "being dragged" (dimmed, with the
    // insertion line still under it) until some unrelated room update
    // happened to refresh the model. Reported as "their icons get darkened
    // after moved and let go and only clear up after entering a room".
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
    commitReorder(dragged);
}

void RailEntryModel::commitGrouping(const QString &dragged,
                                    const QString &target)
{
    // Dropping ONTO a folder files the Space there. Dropping onto another
    // Space makes a folder out of the pair, WHERE THE TARGET WAS — the folder
    // takes over the position the user was pointing at, which is what makes
    // the gesture feel like the two tiles merged rather than like one of them
    // was moved somewhere.
    //
    // Dropping onto a Space that is already filed joins THAT folder instead of
    // creating a nested one: folders do not nest, and a nested arrangement is
    // not something the store can represent, so pretending otherwise would
    // silently lose the grouping.
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
            continue;   // a filed member is not a top-level entry
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
    // The whole arrangement the rail is SHOWING, committed in one write, using
    // the SAME placement rule the preview drew (see folderOwners) — so what
    // the user saw is what is stored.
    // The dragged id is passed rather than read off the member, so this does
    // not depend on when endDrag happened to clear it.
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
            if (!members.contains(entryId))
                members.insert(entryId, {});
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
