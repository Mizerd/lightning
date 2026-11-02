#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <functional>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class SpaceManager : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString activeSpaceId READ activeSpaceId WRITE setActiveSpaceId NOTIFY activeSpaceIdChanged)
    Q_PROPERTY(bool activeSpaceCanManageChildren READ activeSpaceCanManageChildren
               NOTIFY spacePermissionsChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY spacesChanged)
    Q_PROPERTY(bool hasSpaces READ hasSpaces NOTIFY spacesChanged)
    Q_PROPERTY(int spaceCount READ spaceCount NOTIFY spacesChanged)
    Q_PROPERTY(bool directMessagesHaveOwnTile READ directMessagesHaveOwnTile
                   WRITE setDirectMessagesHaveOwnTile
                   NOTIFY directMessagesHaveOwnTileChanged)

public:
    enum Roles {
        SpaceIdRole = Qt::UserRole + 1,
        NameRole,
        TopicRole,
        AvatarUrlRole,
        ChildCountRole,
        UnreadTotalRole,
        HighlightTotalRole,
        LevelRole,
        ParentSpaceIdRole,
        ChildSpaceCountRole,
        // Direct joined rooms of this Space's own. ChildCountRole is
        // transitive and ChildSpaceCountRole counts subspaces only; the rail
        // expander needs both this and ChildSpaceCountRole.
        DirectChildRoomCountRole,
    };

    static QString allRoomsId()   { return QStringLiteral(""); }
    static QString orphansId()    { return QStringLiteral("@orphans"); }
    /// Direct Messages: a rail selection, not a container. roomsInSpace()
    /// answers with every DM, but a DM's Space membership is unchanged.
    /// Offered only in the Channels layout.
    static QString peopleId()     { return QStringLiteral("@people"); }

    explicit SpaceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    /// Drop every in-flight child add/remove/suggest/order operation of the
    /// previous account; pending order saves are reported as failed so an
    /// open editor stops waiting. For sign-in paths that see no loggedOut.
    void abandonPendingChildOps();

    void setDirectMessagesHaveOwnTile(bool own);
    bool directMessagesHaveOwnTile() const
    { return m_directMessagesHaveOwnTile; }

    int peopleUnreadTotal() const { return m_peopleUnreadTotal; }
    int peopleHighlightTotal() const { return m_peopleHighlightTotal; }

    QString activeSpaceId() const { return m_activeSpaceId; }
    void    setActiveSpaceId(const QString &spaceId);
    void    selectJoinedSpace(const QString &spaceId);
    Q_INVOKABLE void noteSettingsDialogOpened(const QString &spaceId)
    {
        Q_EMIT settingsDialogOpened(spaceId);
    }
    bool activeSpaceCanManageChildren() const
    {
        return isRealSpaceId(m_activeSpaceId)
            && m_manageChildren.value(m_activeSpaceId, false);
    }
    bool    hasSpaces() const { return !m_spaces.isEmpty(); }
    int     spaceCount() const { return m_spaces.size(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantList allSpaces() const;
    Q_INVOKABLE QStringList roomsInSpace(const QString &spaceId) const;
    Q_INVOKABLE bool includesRoom(const QString &spaceId, const QString &roomId) const;
    QHash<QString, int> childOrder(const QString &spaceId) const;
    void setPersonalRoomOrder(std::function<QStringList(const QString &, const QStringList &)> resolver)
    { m_personalRoomOrder = std::move(resolver); Q_EMIT spacesChanged(); }
    Q_INVOKABLE QString spaceName(const QString &spaceId) const;

    Q_INVOKABLE QVariantMap spaceInfo(const QString &spaceId) const;
    Q_INVOKABLE QVariantList childRoomsDetailed(const QString &spaceId) const;
    Q_INVOKABLE QVariantList childSpacesDetailed(const QString &spaceId) const;
    Q_INVOKABLE QStringList childSpaceIds(const QString &spaceId) const;
    Q_INVOKABLE QString parentSpaceIdOf(const QString &spaceId) const;
    Q_INVOKABLE QStringList ancestorSpaceIds(const QString &spaceId) const;
    Q_INVOKABLE QStringList moderationScopeRoomIds(
        const QString &spaceId) const;
    Q_INVOKABLE bool roomInAnySpace(const QString &roomId) const;
    Q_INVOKABLE QVariantList directChildRoomsDetailed(
        const QString &spaceId) const;
    QStringList directChildRoomIds(
        const QString &spaceId,
        const QHash<QString, RoomInfo> &byId) const;
    Q_INVOKABLE QVariantList addableRooms(const QString &spaceId,
                                          const QString &filter) const;

    Q_INVOKABLE QVariantList lobbySections(
        const QString &spaceId, const QVariantMap &hierarchyBySpace,
        const QString &filter) const;
    static QVariantList buildLobbySections(
        const QString &spaceId, const QHash<QString, RoomInfo> &byId,
        const QVariantMap &hierarchyBySpace, const QString &filter,
        const QSet<QString> &collapsedSections,
        const std::function<int(const QString &)> &modeOf = {});
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);
    void notificationModesChanged();
    Q_INVOKABLE QStringList lobbySubspaceIds(const QString &spaceId) const;
    Q_INVOKABLE void setLobbySectionCollapsed(const QString &spaceId,
                                              const QString &sectionId,
                                              bool collapsed);
    Q_INVOKABLE bool lobbySectionCollapsed(const QString &spaceId,
                                           const QString &sectionId) const;

    /// True once a complete roster for `spaceId` has arrived. A truncated or
    /// failed roster is never "known": filtering on a partial one would hide
    /// conversations at random.
    Q_INVOKABLE bool spaceRosterKnown(const QString &spaceId) const;
    Q_INVOKABLE bool spaceHasMember(const QString &spaceId,
                                    const QString &userId) const;
    int directScope(const QString &spaceId, const QStringList &peerIds) const;
    void ensureSpaceRoster(const QString &spaceId);
    static bool isRealSpaceId(const QString &spaceId);
    Q_INVOKABLE void addRoomToSpace(const QString &spaceId,
                                    const QString &roomId);
    Q_INVOKABLE void removeRoomFromSpace(const QString &spaceId,
                                         const QString &roomId);
    Q_INVOKABLE bool childOrderPending(const QString &spaceId) const
    { return m_pendingChildOrders.values().contains(spaceId); }
    Q_INVOKABLE QVariantList defaultOrderChildren(const QString &spaceId) const;
    Q_INVOKABLE void setDefaultChildOrder(const QString &spaceId, const QStringList &ids);
    Q_INVOKABLE void setSpaceChildSuggested(const QString &spaceId,
                                            const QString &roomId,
                                            bool suggested);

Q_SIGNALS:
    void activeSpaceIdChanged();
    void spacePermissionsChanged();
    void settingsDialogOpened(const QString &spaceId);
    void spacesChanged();
    void directMessagesHaveOwnTileChanged();
    void childAddFinished(const QString &spaceId, const QString &roomId,
                          bool ok);
    void childRemoveFinished(const QString &spaceId, const QString &roomId,
                             bool ok);
    void childOrderFinished(const QString &spaceId, bool ok);
    void childSuggestedFinished(const QString &spaceId, const QString &roomId,
                                bool suggested, bool ok);
    void spaceRosterChanged(const QString &spaceId);
    void lobbyCollapseChanged(const QString &spaceId);

private Q_SLOTS:
    void rebuild();
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);

private:
    /// One rebuild per event-loop turn. rebuild() is a full model reset plus
    /// an O(all rooms) walk, and batch emitters of roomUpdated would
    /// otherwise trigger one reset per room.
    QTimer m_rebuildCoalesce;

    void dropSpaceRosters();
    struct SpaceEntry {
        RoomInfo info;
        QStringList childRoomIds;
        QStringList childSpaceIds;
        int directChildRoomCount = 0;
        int unreadTotal = 0;
        int highlightTotal = 0;
        int level = 0;
        QString parentSpaceId;
    };

    void resolveHierarchy(const QHash<QString, RoomInfo> &byId);
    void recomputeOrphans();

    std::function<QStringList(const QString &, const QStringList &)> m_personalRoomOrder;
    QHash<quint64, QString> m_pendingChildOrders;
    MatrixClient *m_client = nullptr;

    QHash<QString, QSet<QString>> m_lobbyCollapsed;

    QList<SpaceEntry> m_spaces;
    QHash<QString, QSet<QString>> m_membership;
    QSet<QString> m_allRoomIds;
    QSet<QString> m_spaceChildRoomIds;
    QSet<QString> m_orphanRoomIds;
    std::function<int(const QString &)> m_modeOf;
    int m_homeUnreadTotal = 0;
    int m_homeHighlightTotal = 0;
    int m_peopleUnreadTotal = 0;
    int m_peopleHighlightTotal = 0;
    int m_unparentedUnreadTotal = 0;
    int m_unparentedHighlightTotal = 0;
    bool m_directMessagesHaveOwnTile = false;
    // Space rosters. `m_spaceMembers` holds only complete ones, so "known"
    // means "usable". `m_rosterRequested` is the once-per-Space-per-client
    // guard. Both are cleared with the client.
    QHash<QString, QSet<QString>> m_spaceMembers;
    QSet<QString> m_rosterRequested;
    QSet<QString> m_permissionTracked;
    QHash<QString, bool> m_manageChildren;
    QHash<QString, quint64> m_permissionRequests;
    QHash<quint64, QPair<QString, QString>> m_pendingChildAdds;
    QHash<quint64, QPair<QString, QString>> m_pendingChildRemovals;
    QHash<quint64, QPair<QString, QString>> m_pendingChildSuggests;

    QString m_activeSpaceId;
    QString m_pendingJoinedSpaceId;
};
