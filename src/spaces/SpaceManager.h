#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
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

// The Space model, rebuilt from the active MatrixClient on room changes.
// Exposes a list model of Spaces for QML, the `activeSpaceId` selection that
// drives the room-list filter, and hierarchy lookups.
//
// Pseudo-space ids: "" is "All rooms" (matches every room); "@orphans"
// matches every room that is not a child of any Space.
class SpaceManager : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString activeSpaceId READ activeSpaceId WRITE setActiveSpaceId NOTIFY activeSpaceIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY spacesChanged)
    Q_PROPERTY(bool hasSpaces READ hasSpaces NOTIFY spacesChanged)
    // Real joined Spaces only, excluding the pseudo rows.
    Q_PROPERTY(int spaceCount READ spaceCount NOTIFY spacesChanged)
    // Bound from SpacesRail.qml to whether the Direct Messages tile exists.
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
        // The primary parent this Space is displayed under, empty for a
        // root. See resolveHierarchy() for how one is chosen.
        ParentSpaceIdRole,
        // Joined child Spaces. Zero means "no subspaces", not "no rooms".
        ChildSpaceCountRole,
        // Direct joined rooms of this Space's own. ChildCountRole is
        // transitive and ChildSpaceCountRole counts subspaces only; the rail
        // expander needs both this and ChildSpaceCountRole.
        DirectChildRoomCountRole,
    };

    // Pseudo-space ids surfaced as extra rows.
    static QString allRoomsId()   { return QStringLiteral(""); }
    static QString orphansId()    { return QStringLiteral("@orphans"); }
    /// Direct Messages: a rail selection, not a container. roomsInSpace()
    /// answers with every DM, but a DM's Space membership is unchanged.
    /// Offered only in the Channels layout.
    static QString peopleId()     { return QStringLiteral("@people"); }

    explicit SpaceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    /// Whether DMs have their own rail tile (Channels) or not (Classic).
    /// A tile's badge must count what its view lists: Classic's Home lists
    /// DMs and counts them; Channels' Home does not.
    void setDirectMessagesHaveOwnTile(bool own);
    bool directMessagesHaveOwnTile() const
    { return m_directMessagesHaveOwnTile; }

    /// Unread/highlight over every joined DM, for the tile RailEntryModel
    /// synthesises.
    int peopleUnreadTotal() const { return m_peopleUnreadTotal; }
    int peopleHighlightTotal() const { return m_peopleHighlightTotal; }

    QString activeSpaceId() const { return m_activeSpaceId; }
    void    setActiveSpaceId(const QString &spaceId);
    bool    hasSpaces() const { return !m_spaces.isEmpty(); }
    int     spaceCount() const { return m_spaces.size(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Every row as a map, in model order, for a layer that reorders them
    // (the rail's folders and drag). QML cannot reorder a list model.
    Q_INVOKABLE QVariantList allSpaces() const;
    // Room ids in `spaceId`: every room for allRoomsId(), rooms in no Space
    // for orphansId().
    Q_INVOKABLE QStringList roomsInSpace(const QString &spaceId) const;
    Q_INVOKABLE bool includesRoom(const QString &spaceId, const QString &roomId) const;
    // Display name for the room-list workspace header.
    Q_INVOKABLE QString spaceName(const QString &spaceId) const;

    // Space Home: a presentation snapshot of one Space, and its children in
    // m.space.child order with unread/mention state. `addableRooms` lists
    // joined non-Space rooms not yet children, for the "Add existing room"
    // picker.
    Q_INVOKABLE QVariantMap spaceInfo(const QString &spaceId) const;
    Q_INVOKABLE QVariantList childRoomsDetailed(const QString &spaceId) const;
    // Joined child subspaces of a Space, in m.space.child order. Unjoined
    // subspaces come from /hierarchy via RoomDiscoveryController.
    Q_INVOKABLE QVariantList childSpacesDetailed(const QString &spaceId) const;
    // Joined child Spaces in m.space.child order, restricted to those whose
    // primary parent is this Space, so a two-parent subspace appears once.
    Q_INVOKABLE QStringList childSpaceIds(const QString &spaceId) const;
    /// The primary parent of `spaceId` (as assigned in resolveHierarchy()),
    /// or "" for a root.
    Q_INVOKABLE QString parentSpaceIdOf(const QString &spaceId) const;
    /// Every ancestor of `spaceId`, nearest first. Bounded by the hierarchy
    /// depth limit, so a parent cycle cannot spin.
    Q_INVOKABLE QStringList ancestorSpaceIds(const QString &spaceId) const;
    // Whether `roomId` is a direct child of any joined Space. The Channels
    // "Rooms" group is the complement. Direct, since subspaces are folders
    // of their own in that layout.
    Q_INVOKABLE bool roomInAnySpace(const QString &roomId) const;
    // Direct joined non-Space child rooms, in m.space.child order. Unlike
    // childRoomsDetailed(), which is transitive, this keeps the structure the
    // Space's admin built, for a channel list.
    Q_INVOKABLE QVariantList directChildRoomsDetailed(
        const QString &spaceId) const;
    /// The same direct children as ids, resolved against a room map the
    /// caller already has, to avoid rebuilding the whole room list per Space.
    /// Same order and skip rules as directChildRoomsDetailed().
    QStringList directChildRoomIds(
        const QString &spaceId,
        const QHash<QString, RoomInfo> &byId) const;
    Q_INVOKABLE QVariantList addableRooms(const QString &spaceId,
                                          const QString &filter) const;

    // ---- Space Home lobby ------------------------------------------------
    //
    // The Space's own direct rooms first, then one section per direct child
    // Space listing that subspace's direct children. One level of sections:
    // a grandchild Space is a row in its parent's section, and its rooms are
    // not flattened in.
    //
    // Each section is { sectionId, isRoot, roomId, name, avatarUrl,
    // identityColorKey, topic, suggested, suggestedKnown, selectable,
    // roomCount, spaceCount, unreadTotal, highlightTotal, hasUnread,
    // matchCount, collapsed, rows }. A row's `hasUnread` is
    // hasUnreadMessages OR unreadCount > 0, and a section's is any row's.
    // Each row is { roomId, parentId, name, avatarUrl, identityColorKey,
    // topic, isSpace, joined, isDirect, suggested, suggestedKnown, members,
    // childCount, childrenCount, hasUnread, unreadCount, highlightCount,
    // membership, joinRule, via, selectable }.
    //
    // Order within a section is the parent's m.space.child order, then any
    // /hierarchy row not yet known, in the SDK's order. `selectable` is true
    // only for the Home Space's direct children, the only rows its Remove /
    // Mark as suggested can act on.
    //
    // `hierarchyBySpace` maps a space id to its /hierarchy rows (topics,
    // member counts, suggested flags, unjoined children). The filter matches
    // names and topics case-insensitively; a subspace that matches keeps all
    // its rows; an emptied section is dropped; a search overrides collapse.
    Q_INVOKABLE QVariantList lobbySections(
        const QString &spaceId, const QVariantMap &hierarchyBySpace,
        const QString &filter) const;
    /// The pure half of lobbySections(), for tests.
    static QVariantList buildLobbySections(
        const QString &spaceId, const QHash<QString, RoomInfo> &byId,
        const QVariantMap &hierarchyBySpace, const QString &filter,
        const QSet<QString> &collapsedSections);
    /// The joined direct child Spaces a lobby draws a section for, so the
    /// view can query /hierarchy for each. Includes every joined child Space,
    /// not only those whose primary parent this is.
    Q_INVOKABLE QStringList lobbySubspaceIds(const QString &spaceId) const;
    /// Folded lobby sections, per Space. Session state only, so no Matrix
    /// room ids are written to settings. Cleared on sign-out and client swap.
    Q_INVOKABLE void setLobbySectionCollapsed(const QString &spaceId,
                                              const QString &sectionId,
                                              bool collapsed);
    Q_INVOKABLE bool lobbySectionCollapsed(const QString &spaceId,
                                           const QString &sectionId) const;

    // ---- Space people ----------------------------------------------------
    //
    // The roster is the Space room's own joined and invited membership: one
    // bounded request per Space per session, fired on selection. Deriving it
    // from child rooms would depend on which rosters happened to be fetched
    // and cost a request per room. Someone in a child room who has not
    // joined the Space room is not counted.

    /// True once a complete roster for `spaceId` has arrived. A truncated or
    /// failed roster is never "known": filtering on a partial one would hide
    /// conversations at random.
    Q_INVOKABLE bool spaceRosterKnown(const QString &spaceId) const;
    /// Whether `userId` is a joined or invited member of the Space room.
    /// Always false for an unknown roster; ask spaceRosterKnown() first.
    Q_INVOKABLE bool spaceHasMember(const QString &spaceId,
                                    const QString &userId) const;
    /// The single People-scope decision for both layouts: 1 = the DM belongs
    /// to the Space's people, 0 = it does not, -1 = unknown. Callers pick
    /// their own direction for -1 (Classic removes, Channels adds).
    int directScope(const QString &spaceId, const QStringList &peerIds) const;
    /// Requests the roster for `spaceId` once per Space per client. A no-op
    /// for a pseudo id, a non-Space, an already-known or in-flight roster, or
    /// a backend that cannot answer.
    void ensureSpaceRoster(const QString &spaceId);
    /// Whether `spaceId` is a real Space room id rather than a pseudo
    /// selection. A pseudo selection scopes nothing.
    static bool isRealSpaceId(const QString &spaceId);
    // Sends a real m.space.child state event. The hierarchy update arrives
    // via sync; childAddFinished reports only the send outcome.
    Q_INVOKABLE void addRoomToSpace(const QString &spaceId,
                                    const QString &roomId);
    // MSC1772 removal (empty-via m.space.child); the room itself is never
    // left or deleted. Permission failures surface as ok=false.
    Q_INVOKABLE void removeRoomFromSpace(const QString &spaceId,
                                         const QString &roomId);
    // Toggles `suggested` on an existing child (via list and order kept; a
    // non-child is refused).
    Q_INVOKABLE void setSpaceChildSuggested(const QString &spaceId,
                                            const QString &roomId,
                                            bool suggested);

Q_SIGNALS:
    void activeSpaceIdChanged();
    void spacesChanged();
    void directMessagesHaveOwnTileChanged();
    // Outcome of one addRoomToSpace call (send result, not sync).
    void childAddFinished(const QString &spaceId, const QString &roomId,
                          bool ok);
    void childRemoveFinished(const QString &spaceId, const QString &roomId,
                             bool ok);
    void childSuggestedFinished(const QString &spaceId, const QString &roomId,
                                bool suggested, bool ok);
    /// A complete roster for `spaceId` arrived, or was dropped on an account
    /// change. Both room-list models re-filter on it.
    void spaceRosterChanged(const QString &spaceId);
    /// A lobby section of `spaceId` was folded or unfolded.
    void lobbyCollapseChanged(const QString &spaceId);

private Q_SLOTS:
    void rebuild();
    /// One member snapshot. Ignores ops this manager did not issue; records
    /// only a complete, successful, non-truncated roster.
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);

private:
    /// One rebuild per event-loop turn. rebuild() is a full model reset plus
    /// an O(all rooms) walk, and batch emitters of roomUpdated would
    /// otherwise trigger one reset per room.
    QTimer m_rebuildCoalesce;

    /// Forgets every roster and in-flight request, announcing the known ones
    /// so filters built on them re-open.
    void dropSpaceRosters();
    struct SpaceEntry {
        RoomInfo info;              // The Space room itself.
        QStringList childRoomIds;   // Transitive member rooms, ordered.
        // Direct joined child Spaces whose primary parent is this one, in
        // m.space.child order.
        QStringList childSpaceIds;
        // Direct joined non-Space children: whether expanding reveals
        // anything, which neither count above answers.
        int directChildRoomCount = 0;
        int unreadTotal = 0;        // Sum of children's unread counts.
        int highlightTotal = 0;
        // Depth in the hierarchy: 0 for a root, +1 per level.
        int level = 0;
        // The primary parent, empty for a root.
        QString parentSpaceId;
    };

    // Assigns level/parentSpaceId/childSpaceIds across `m_spaces`.
    // Cycle-safe and deterministic; see the implementation for the rules.
    void resolveHierarchy(const QHash<QString, RoomInfo> &byId);
    void recomputeOrphans();

    MatrixClient *m_client = nullptr;

    // Folded lobby sections: Home space id -> section ids. Session only.
    QHash<QString, QSet<QString>> m_lobbyCollapsed;

    QList<SpaceEntry> m_spaces;             // Rows: [All rooms] [orphans?] [space1] [space2] ...
    QHash<QString, QSet<QString>> m_membership;    // spaceId → set(roomId)
    QSet<QString> m_allRoomIds;             // Every non-space room id.
    // Rooms that are a direct child of at least one joined Space.
    QSet<QString> m_spaceChildRoomIds;
    QSet<QString> m_orphanRoomIds;          // Rooms not in any Space.
    int m_homeUnreadTotal = 0;
    int m_homeHighlightTotal = 0;
    // Joined direct messages.
    int m_peopleUnreadTotal = 0;
    int m_peopleHighlightTotal = 0;
    // Joined non-Space, non-DM rooms no Space lists: what Channels' Home and
    // "Other rooms" render.
    int m_unparentedUnreadTotal = 0;
    int m_unparentedHighlightTotal = 0;
    bool m_directMessagesHaveOwnTile = false;
    // Space rosters. `m_spaceMembers` holds only complete ones, so "known"
    // means "usable". `m_rosterRequested` is the once-per-Space-per-client
    // guard. Both are cleared with the client.
    QHash<QString, QSet<QString>> m_spaceMembers;
    QSet<QString> m_rosterRequested;
    // Pending m.space.child sends, opId -> (spaceId, roomId).
    QHash<quint64, QPair<QString, QString>> m_pendingChildAdds;
    QHash<quint64, QPair<QString, QString>> m_pendingChildRemovals;
    QHash<quint64, QPair<QString, QString>> m_pendingChildSuggests;

    QString m_activeSpaceId; // Empty means "All rooms".
};
