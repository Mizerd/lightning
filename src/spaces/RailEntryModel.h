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

    /// Recompute the rows from the Space model and the stored arrangement.
    /// A refresh that arrives DURING a drag is remembered and applied when the
    /// gesture ends — rebuilding under the pointer is what destroyed the
    /// delegate holding the gesture.
    Q_INVOKABLE void refresh();

    /// Takes hold of `entryId`. False when it cannot be dragged (a pseudo row,
    /// a subspace) or is not currently shown.
    Q_INVOKABLE bool beginDrag(const QString &entryId);
    /// The pointer moved over view row `row`. `onto` asks for the GROUP
    /// gesture — the caller decides that from the pointer's position within
    /// the row plus a dwell, so that merely passing through a tile on the way
    /// somewhere else cannot create a folder.
    Q_INVOKABLE void updateDrag(int row, bool onto);
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
    /// Snaps a pointer row to a legal destination for the entry being
    /// dragged: a folder may only land at a top-level boundary.
    int legalDestination(int row) const;
    void commitGrouping(const QString &dragged, const QString &target);
    void commitReorder(const QString &dragged);
    bool rowIsFolder(int row) const;

    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    QVector<QVariantMap> m_rows;

    bool m_dragging = false;
    bool m_grouping = false;
    QString m_dragEntryId;
    QString m_dropTargetId;
    bool m_refreshPending = false;
};
