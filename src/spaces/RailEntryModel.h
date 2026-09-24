#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class RailLayoutStore;
class SpaceManager;

// The rows the Spaces rail draws, and the live state of a drag over them.
//
// A real list model (not a JS array) so a reorder emits beginMoveRows and QML
// can animate neighbours while the pointer is down, without resetting and
// destroying the delegate that holds the gesture.
//
// Three separate pieces of state: the durable arrangement (RailLayoutStore),
// the transient preview order (these rows during a drag, never saved), and
// the drag itself (what is moving, and whether a release would reorder or
// group). Nothing is written until the gesture ends.
//
// Only root Spaces sit at the top level; subspaces appear under an expanded
// parent. A subspace row reflects Matrix hierarchy, so it is neither
// draggable nor a group target.
class RailEntryModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
    Q_PROPERTY(QString draggingEntryId READ draggingEntryId NOTIFY dragChanged)
    /// The folder (or Space) a release would file the dragged Space into, or
    /// empty while the gesture is a plain reorder.
    Q_PROPERTY(QString dropTargetId READ dropTargetId NOTIFY dragChanged)
    /// True when releasing would group rather than reorder.
    Q_PROPERTY(bool grouping READ grouping NOTIFY dragChanged)
    /// Whether the Direct Messages tab is offered under Home. Channels only:
    /// Classic reaches DMs through its People filter. Synthesised here rather
    /// than in SpaceManager, whose model feeds surfaces with no DM view.
    Q_PROPERTY(bool peopleEntryVisible READ peopleEntryVisible
                   WRITE setPeopleEntryVisible NOTIFY peopleEntryVisibleChanged)
    /// Whether the "Other rooms" tile is offered. Classic only: Channels'
    /// Home already excludes every room in a Space, so the tile would open
    /// the same page.
    Q_PROPERTY(bool orphansEntryVisible READ orphansEntryVisible
                   WRITE setOrphansEntryVisible
                   NOTIFY orphansEntryVisibleChanged)

public:
    enum Roles {
        EntryIdRole = Qt::UserRole + 1,
        /// "space" | "folder".
        KindRole,
        SpaceIdRole,
        NameRole,
        AvatarUrlRole,
        UnreadTotalRole,
        HighlightTotalRole,
        /// Depth in the Matrix hierarchy — 0 for a root, +1 per subspace
        /// level. Drives indentation only.
        LevelRole,
        /// The folder this row is filed in, empty at the top level. On a
        /// folder's own header row it is the folder's own id.
        FolderIdRole,
        CollapsedRole,
        ChildCountRole,
        /// Folder rows: up to four { spaceId, name, avatarUrl } for the
        /// composite tile.
        MemberPreviewRole,
        /// True for "All rooms" / "Other rooms": a view of everything, never
        /// something with a position among the Spaces.
        PseudoRole,
        /// A Space shown because its parent is expanded (Matrix hierarchy,
        /// not the user's arrangement).
        HierarchyChildRole,
        /// This Space has joined subspaces, so the expander means something.
        ExpandableRole,
        ExpandedRole,
        /// The row currently being dragged.
        DraggedRole,
        /// This row is the folder/Space a release would group into.
        DropTargetRole,
        /// The last member row of an open folder; carries the container's
        /// rounded bottom.
        FolderLastRole,
        /// Whether the user may drag this row at all.
        DraggableRole,
        /// Group field: derived from the rows' level sequence rather than the
        /// Space graph, so the regions follow the live drag preview.
        ///
        /// Depth of the row above, or -1 at the top. The region at depth d
        /// starts here when this is below d.
        BandPrevLevelRole,
        /// Depth of the row below, or -1 at the end. The region at depth d
        /// ends here when this is below d. Neighbour depths rather than
        /// booleans because a row sits on one region per ancestor.
        BandNextLevelRole,
    };

    explicit RailEntryModel(QObject *parent = nullptr);

    void setSources(SpaceManager *spaces, RailLayoutStore *layout);

    /// Flat (Classic) rail: top-level entries only, nothing expandable.
    /// A model flag, not a paint flag, so hidden rows never exist in the list
    /// that drag arithmetic, group bands and drop targets index into.
    /// Folders are unaffected: they group top-level Spaces, not hierarchy.
    void setFlat(bool flat);
    bool flat() const { return m_flat; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool dragging() const { return m_dragging; }
    QString draggingEntryId() const { return m_dragEntryId; }
    QString dropTargetId() const { return m_dropTargetId; }
    /// Test-only reader for legalGap(): a subspace may reorder but never
    /// reparent.
    int legalGapForTest(int gap) const { return legalGap(gap); }
    bool grouping() const { return m_grouping; }
    bool peopleEntryVisible() const { return m_peopleEntryVisible; }
    bool orphansEntryVisible() const { return m_orphansEntryVisible; }
    void setOrphansEntryVisible(bool visible);
    void setPeopleEntryVisible(bool visible);

    /// Recompute the rows from the Space model and the stored arrangement.
    /// A refresh during a drag is deferred until the gesture ends.
    Q_INVOKABLE void refresh();

    /// Takes hold of `entryId`. False when it cannot be dragged (a pseudo row,
    /// a subspace) or is not currently shown.
    Q_INVOKABLE bool beginDrag(const QString &entryId);
    /// The pointer is on the tile of view row `row`: arm the group gesture
    /// and move nothing. An ineligible target clears the target; it never
    /// degrades into a reorder. Kept separate from hoverGap() so aiming at a
    /// tile can never move it out from under the pointer.
    Q_INVOKABLE void hoverGroup(int row);
    /// The pointer is in the gap before view row `gap` (0..rowCount): clear
    /// any group target and move the dragged block to start there. `gap` is a
    /// gap index; the moveBlock() destination accounts for the block's own
    /// removal.
    Q_INVOKABLE void hoverGap(int gap);
    /// Expands every ancestor of `spaceId` so its row exists, then asks the
    /// rail to scroll it into view. A deep Space has no row until its whole
    /// chain is open.
    Q_INVOKABLE void revealSpace(const QString &spaceId);
    /// Clear any group target and leave the preview order as is: the pointer
    /// is over the dragged block's own slot.
    Q_INVOKABLE void clearDropTarget();
    /// Ends the gesture. `commit` false abandons it and restores the stored
    /// arrangement.
    Q_INVOKABLE void endDrag(bool commit);

    Q_INVOKABLE int rowForEntry(const QString &entryId) const;
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    /// Recursion backstop for the subspace walk.
    static constexpr int kMaxHierarchyDepth = 16;

Q_SIGNALS:
    void countChanged();
    void dragChanged();
    /// Emitted by revealSpace() after ancestors are expanded and rows rebuilt.
    void revealRequested(const QString &spaceId);
    void peopleEntryVisibleChanged();
    void orphansEntryVisibleChanged();

private:
    void applyRows(QVector<QVariantMap> rows);
    /// Stamps the group field onto every row. Called from applyRows(), which
    /// every row set passes through, including the drag preview.
    static void stampGroupField(QVector<QVariantMap> &rows);
    // `folderLast` over the folder's whole run, nested rows included.
    static void stampFolderRuns(QVector<QVariantMap> &rows);
    void appendSubspaces(const QString &parentId,
                         const QString &owningFolderId,
                         const QHash<QString, QVariantMap> &byId,
                         QVector<QVariantMap> &rows, int depth);
    /// The rows a folder header owns: [header, members…], one row otherwise.
    /// A dragged folder moves its open members with it.
    int blockLength(int row) const;
    void moveBlock(int from, int count, int to);
    /// Which folder each row would belong to if committed now, "" for the
    /// top level. Shared by preview and commit so a drop lands where the
    /// preview showed.
    QVector<QString> folderOwners(const QString &draggedId) const;
    /// Applies folderOwners() to the rows so the folder band follows the drag.
    void refreshFolderRuns();
    /// Snaps a pointer gap (0..m_rows.size()) to a legal one for the dragged
    /// entry: never above a pseudo row; a top-level entry never inside a
    /// subspace run; a folder only at a top-level boundary; a subspace only
    /// among its own parent's children, so a reorder never reparents.
    int legalGap(int gap) const;
    void commitGrouping(const QString &dragged, const QString &target);
    void commitReorder(const QString &dragged);
    /// Writes one parent's subspace order, read off the drag preview.
    void commitChildOrder(const QString &dragged);
    bool rowIsFolder(int row) const;

    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    QVector<QVariantMap> m_rows;

    bool m_flat = false;
    bool m_peopleEntryVisible = false;
    // Default true: Classic is the default layout and the tile belongs there.
    bool m_orphansEntryVisible = true;
    bool m_dragging = false;
    bool m_grouping = false;
    QString m_dragEntryId;
    QString m_dropTargetId;
    bool m_refreshPending = false;
};
