#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class MatrixClient;
class RailLayoutStore;
class SettingsManager;
class SpaceManager;

class RailEntryModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
    Q_PROPERTY(QString draggingEntryId READ draggingEntryId NOTIFY dragChanged)
    Q_PROPERTY(QString dropTargetId READ dropTargetId NOTIFY dragChanged)
    Q_PROPERTY(bool grouping READ grouping NOTIFY dragChanged)
    Q_PROPERTY(bool peopleEntryVisible READ peopleEntryVisible
                   WRITE setPeopleEntryVisible NOTIFY peopleEntryVisibleChanged)
    Q_PROPERTY(bool orphansEntryVisible READ orphansEntryVisible
                   WRITE setOrphansEntryVisible
                   NOTIFY orphansEntryVisibleChanged)

public:
    enum Roles {
        EntryIdRole = Qt::UserRole + 1,
        KindRole,
        SpaceIdRole,
        NameRole,
        AvatarUrlRole,
        UnreadTotalRole,
        HighlightTotalRole,
        LevelRole,
        FolderIdRole,
        CollapsedRole,
        ChildCountRole,
        MemberPreviewRole,
        PseudoRole,
        HierarchyChildRole,
        ExpandableRole,
        ExpandedRole,
        DraggedRole,
        DropTargetRole,
        FolderLastRole,
        DraggableRole,
        BandPrevLevelRole,
        BandNextLevelRole,
        HasUnreadRole,
        MentionCountRole,
    };

    explicit RailEntryModel(QObject *parent = nullptr);

    void setSources(SpaceManager *spaces, RailLayoutStore *layout);
    void setRoomSources(MatrixClient *client, SettingsManager *settings);

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
    int legalGapForTest(int gap) const { return legalGap(gap); }
    bool grouping() const { return m_grouping; }
    bool peopleEntryVisible() const { return m_peopleEntryVisible; }
    bool orphansEntryVisible() const { return m_orphansEntryVisible; }
    void setOrphansEntryVisible(bool visible);
    void setPeopleEntryVisible(bool visible);

    /// Recompute the rows from the Space model and the stored arrangement.
    /// A refresh during a drag is deferred until the gesture ends.
    Q_INVOKABLE void refresh();

    Q_INVOKABLE bool beginDrag(const QString &entryId);
    Q_INVOKABLE void hoverGroup(int row);
    Q_INVOKABLE void hoverGap(int gap);
    Q_INVOKABLE void revealSpace(const QString &spaceId);
    Q_INVOKABLE void clearDropTarget();
    Q_INVOKABLE void endDrag(bool commit);

    Q_INVOKABLE int rowForEntry(const QString &entryId) const;
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    static constexpr int kMaxHierarchyDepth = 16;

Q_SIGNALS:
    void countChanged();
    void dragChanged();
    void revealRequested(const QString &spaceId);
    void peopleEntryVisibleChanged();
    void orphansEntryVisibleChanged();

private:
    void applyRows(QVector<QVariantMap> rows);
    static void stampGroupField(QVector<QVariantMap> &rows);
    static void stampFolderRuns(QVector<QVariantMap> &rows);
    void stampActivity(QVector<QVariantMap> &rows) const;
    void appendSubspaces(const QString &parentId,
                         const QString &owningFolderId,
                         const QHash<QString, QVariantMap> &byId,
                         QVector<QVariantMap> &rows, int depth);
    int blockLength(int row) const;
    void moveBlock(int from, int count, int to);
    QVector<QString> folderOwners(const QString &draggedId) const;
    void refreshFolderRuns();
    int legalGap(int gap) const;
    void commitGrouping(const QString &dragged, const QString &target);
    void commitReorder(const QString &dragged);
    void commitChildOrder(const QString &dragged);
    bool rowIsFolder(int row) const;
    bool railIsManual() const;

    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;
    QTimer m_modeRefresh;
    QVector<QVariantMap> m_rows;

    bool m_flat = false;
    bool m_peopleEntryVisible = false;
    bool m_orphansEntryVisible = true;
    bool m_dragging = false;
    bool m_grouping = false;
    QString m_dragEntryId;
    QString m_dropTargetId;
    bool m_refreshPending = false;
};
