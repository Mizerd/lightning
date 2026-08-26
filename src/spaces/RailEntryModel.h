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
// WHY THIS IS A MODEL AND NOT A JS ARRAY. The rail used to bind its ListView
// to a plain JavaScript array rebuilt on every change, which makes every
// change a model RESET: no move, no displaced transition, every delegate torn
// down and rebuilt. A reorder therefore could not animate, and during a drag
// the delegate holding the gesture was destroyed the moment anything refreshed
// it. Both of those are the reported "hard to tell exactly where you are
// moving them". A QAbstractListModel that emits a real `beginMoveRows` is what
// lets QML animate the neighbours out of the way while the pointer is still
// down.
//
// THREE SEPARATE PIECES OF STATE, deliberately not one:
//
//   * the DURABLE arrangement — RailLayoutStore, on disk;
//   * the TRANSIENT preview order — this model's rows while a drag is live,
//     never written anywhere;
//   * the drag's own facts — which entry is moving, and whether releasing
//     now would REORDER it or GROUP it into a folder.
//
// Nothing is saved until the gesture ends, so a drag is one settings write
// rather than one per pointer sample.
//
// HIERARCHY. Only ROOT Spaces sit at the top level; a subspace appears
// underneath its parent when that parent is expanded, exactly as Element
// Classic's Space panel does. A subspace row is presentation of MATRIX state,
// so it is not draggable and not a group target: its position is the
// hierarchy's, and a local folder must never look like it can change it.
class RailEntryModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
    Q_PROPERTY(QString draggingEntryId READ draggingEntryId NOTIFY dragChanged)
    /// The folder (or Space) a release would file the dragged Space into, or
    /// empty while the gesture is a plain reorder.
    Q_PROPERTY(QString dropTargetId READ dropTargetId NOTIFY dragChanged)
    /// True when releasing would GROUP rather than REORDER. The rail draws two
    /// visibly different things for the two, because "between" and "onto" are
    /// a few pixels apart and mean completely different outcomes.
    Q_PROPERTY(bool grouping READ grouping NOTIFY dragChanged)
    /// Whether the Direct Messages tab is offered, directly under Home.
    ///
    /// CHANNELS ONLY. Classic reaches DMs through its People filter chip and
    /// its one activity-ordered list, so a tab there would be a second route
    /// to the same rows and a change to a layout the maintainer asked to
    /// leave alone. The row is synthesised HERE rather than in SpaceManager
    /// because SpaceManager's model feeds other surfaces that must not grow
    /// a tab they have no view for.
    Q_PROPERTY(bool peopleEntryVisible READ peopleEntryVisible
                   WRITE setPeopleEntryVisible NOTIFY peopleEntryVisibleChanged)

public:
    enum Roles {
        EntryIdRole = Qt::UserRole + 1,
        /// "space" | "folder". A string, matching the convention the rest of
        /// the shell's models use: exposing an enum to QML means registering
        /// this type purely so a delegate can name a constant.
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
        /// A Space shown because its parent is expanded. Matrix's arrangement,
        /// not the user's.
        HierarchyChildRole,
        /// This Space has joined subspaces, so the expander means something.
        ExpandableRole,
        ExpandedRole,
        /// The row currently being dragged: the rail draws it as the gap the
        /// entry would land in.
        DraggedRole,
        /// This row is the folder/Space a release would group into.
        DropTargetRole,
        /// The last member row of an open folder — carries the container's
        /// rounded bottom.
        FolderLastRole,
        /// Whether the user may drag this row at all.
        DraggableRole,
    };

    explicit RailEntryModel(QObject *parent = nullptr);

    void setSources(SpaceManager *spaces, RailLayoutStore *layout);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool dragging() const { return m_dragging; }
    QString draggingEntryId() const { return m_dragEntryId; }
    QString dropTargetId() const { return m_dropTargetId; }
    bool grouping() const { return m_grouping; }
    bool peopleEntryVisible() const { return m_peopleEntryVisible; }
    void setPeopleEntryVisible(bool visible);

    /// Recompute the rows from the Space model and the stored arrangement.
    /// A refresh that arrives DURING a drag is remembered and applied when the
    /// gesture ends — rebuilding under the pointer is what destroyed the
    /// delegate holding the gesture.
    Q_INVOKABLE void refresh();

    /// Takes hold of `entryId`. False when it cannot be dragged (a pseudo row,
    /// a subspace) or is not currently shown.
    Q_INVOKABLE bool beginDrag(const QString &entryId);
    /// The pointer is ON the TILE of view row `row`: arm the GROUP gesture and
    /// MOVE NOTHING. An ineligible target (a pseudo row, a subspace, or a
    /// folder being dragged onto another folder) clears the target and returns
    /// — it never degrades into a reorder.
    ///
    /// TWO VERBS, NOT A FLAG. This used to be `updateDrag(row, onto)`, one
    /// entry point whose `onto == false` branch REORDERED INTO THE HOVERED
    /// ROW. Every caller that wanted to say "the pointer is aiming at this
    /// tile" had to spell it with the same call that moves the dragged block
    /// onto that tile's slot — so the tile stepped aside, the row under the
    /// pointer became the dragged block, and grouping was unreachable. No drop
    /// ever created a folder, through two rounds and fifteen passing model
    /// tests, because those tests called the grouping branch directly.
    ///
    /// The `onto` flag is gone rather than defaulted: leaving the
    /// reorder-into-the-hovered-row path reachable is precisely the defect.
    Q_INVOKABLE void hoverGroup(int row);
    /// The pointer is in the GAP before view row `gap` (gaps run 0..rowCount):
    /// clear any armed group target and move the dragged block so it starts
    /// there. A gap is never a group target and a tile is never a reorder
    /// target, so the tile the user is aiming at can never move out from under
    /// the pointer.
    ///
    /// `gap` is a GAP INDEX, not a row index. The destination handed to
    /// moveBlock() is derived from it, accounting for the block's own removal
    /// — the conversion the row-index version never had, and the reason a
    /// one-row hover used to park the block under the pointer and oscillate.
    Q_INVOKABLE void hoverGap(int gap);
    /// Clear any armed group target and leave the preview order exactly as it
    /// is. The view calls this for the one reading neither verb covers: the
    /// pointer sitting over the dragged block's OWN slot, where there is
    /// nothing to group with and nowhere new to go.
    Q_INVOKABLE void clearDropTarget();
    /// Ends the gesture. `commit` false abandons it and restores the stored
    /// arrangement.
    Q_INVOKABLE void endDrag(bool commit);

    Q_INVOKABLE int rowForEntry(const QString &entryId) const;
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    /// Recursion bound for the subspace walk. The hierarchy is already a tree
    /// (one primary parent each), so this is a backstop, not a policy.
    static constexpr int kMaxHierarchyDepth = 16;

Q_SIGNALS:
    void countChanged();
    void dragChanged();
    void peopleEntryVisibleChanged();

private:
    void applyRows(QVector<QVariantMap> rows);
    void appendSubspaces(const QString &parentId,
                         const QString &owningFolderId,
                         const QHash<QString, QVariantMap> &byId,
                         QVector<QVariantMap> &rows, int depth);
    /// The rows a folder header owns: [header, members…]. One row for
    /// anything else. Dragging a folder has to move its open members with it,
    /// or the header detaches from its own contents mid-gesture.
    int blockLength(int row) const;
    void moveBlock(int from, int count, int to);
    /// Which folder each row would belong to if the arrangement were committed
    /// right now, "" for the top level. ONE definition, shared by the preview
    /// and the commit: two copies of this rule is how a drop lands somewhere
    /// the preview never showed.
    QVector<QString> folderOwners(const QString &draggedId) const;
    /// Applies folderOwners() to the rows' own folderId/folderLast, so the
    /// container band drawn behind an open folder follows the drag.
    void refreshFolderRuns();
    /// Snaps a pointer GAP to a legal one for the entry being dragged: never
    /// above a pseudo row, never strictly inside a subspace run, and — for a
    /// folder — only at a top-level boundary. Gaps run 0..m_rows.size().
    int legalGap(int gap) const;
    void commitGrouping(const QString &dragged, const QString &target);
    void commitReorder(const QString &dragged);
    bool rowIsFolder(int row) const;

    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    QVector<QVariantMap> m_rows;

    bool m_peopleEntryVisible = false;
    bool m_dragging = false;
    bool m_grouping = false;
    QString m_dragEntryId;
    QString m_dropTargetId;
    bool m_refreshPending = false;
};
