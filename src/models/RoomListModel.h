#pragma once

#include "matrix/RoomInfo.h"
#include "models/DirectAvatarResolver.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVariantMap>
#include <QTimer>

class MatrixClient;
class SpaceManager;

class RoomListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(quint64 filterGeneration READ filterGeneration NOTIFY filterGenerationChanged)
    // Element-style list filter: 0 All, 1 People (DMs), 2 Rooms, 3 Unreads.
    // Invites always pass, and in Unreads mode the pinned (open) room stays
    // visible so reading it does not remove the selected row.
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode NOTIFY filterModeChanged)
    // False on a backend that cannot write room tags; the action is then not
    // offered, since a device-local favourite would disagree with other
    // clients.
    Q_PROPERTY(bool roomFavouritesSupported READ roomFavouritesSupported
                   NOTIFY roomFavouritesSupportedChanged)
    // Retired: always empty now that favourites have their own section header.
    // Kept so existing bindings keep working (see updateFavouritesBoundary()).
    Q_PROPERTY(QString favouritesBoundaryRoomId READ favouritesBoundaryRoomId
                   NOTIFY favouritesBoundaryRoomIdChanged)
    /// Joined conversations that are unread, and how many of those mention the
    /// user. Account-wide and immune to the chips and Space scope: this feeds
    /// the window title and tray. Counts rooms, not messages, since Matrix
    /// message counts are often 0 for genuinely unread rooms.
    Q_PROPERTY(int unreadRoomCount READ unreadRoomCount
                   NOTIFY unreadTotalsChanged)
    Q_PROPERTY(int highlightRoomCount READ highlightRoomCount
                   NOTIFY unreadTotalsChanged)
public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        TopicRole,
        AvatarUrlRole,
        LastMessagePreviewRole,
        LastActivityRole,
        UnreadCountRole,
        EncryptedRole,
        IsSpaceRole,
        MemberCountRole, // display/diagnostics only; never used for DM classification
        CategoryRole,    // "invite" | "favourite" | "conversation"; see orderRankOf()
        HighlightCountRole,
        MarkedUnreadRole,
        HasUnreadRole,
        MembershipRole,
        IsDirectRole,
        // Matrix `m.favourite` room tag: account state shared with other
        // clients.
        IsFavouriteRole,
        DirectUserIdRole,
        InviterRole,
        InvitePendingRole,
        InviteErrorRole,
        CanonicalAliasRole,
        // identityColorKey(RoomInfo): partner MXID for unambiguous 1:1 DMs,
        // room id otherwise.
        IdentityColorKeyRole,
        // The successor of a tombstoned room (empty otherwise), and whether the
        // user can actually reach it; only then is the row de-emphasized.
        SuccessorRoomIdRole,
        SupersededByAccessibleSuccessorRole,
        // Which bridged network this conversation belongs to. Empty for a
        // native Matrix room (no badge). Presentation only, never routing.
        NetworkRole,
        NetworkLabelRole,
    };

    explicit RoomListModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    // Optional Space filter: when bound, only rooms in
    // SpaceManager::activeSpaceId() are shown. Space rooms themselves are
    // always excluded (they belong to the rail).
    void setSpaceManager(SpaceManager *spaces);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Look up a room by id and return the small map of fields the UI needs, or
    // an empty map.
    Q_INVOKABLE QVariantMap findRoom(const QString &roomId) const;

    /// Record what a room's bridge advertises (MSC2346), as resolved by
    /// matrix::bridge::labelForAdvertisedBridge. data() must stay synchronous
    /// and never fetch, so the /state read is driven elsewhere (room open, room
    /// info panel) and lands here; data() falls back to the ghost-mxid/alias
    /// inference. An empty `label` removes the entry without erasing a
    /// still-correct DM inference.
    void setAdvertisedBridge(const QString &roomId, const QString &networkId,
                             const QString &label);
    /// Account-scoped: another account's rooms are not these rooms.
    void clearAdvertisedBridges();
    // The most recent joined conversations (Spaces excluded) for Home, as up to
    // `max` {roomId,name,avatarUrl,isDirect,hasUnread,unreadCount} maps in
    // activity order.
    Q_INVOKABLE QVariantList recentRooms(int max = 6) const;
    // Joined Spaces (presentation fields only) for Home's shortcut strip.
    Q_INVOKABLE QVariantList spacesSummary(int max = 8) const;
    Q_INVOKABLE void acceptInvite(const QString &roomId);
    Q_INVOKABLE void rejectInvite(const QString &roomId);
    Q_INVOKABLE void markRoomRead(const QString &roomId);
    /// Mark every unread joined room read. Returns how many were marked.
    ///
    /// The per-room receipt is what ActivityModel::markRoomReadUpTo listens
    /// for, so the bell clears too. Only unread rooms are touched; invites are
    /// skipped (a decision, not unread mail), and so are `markedUnread` rooms,
    /// which the user deliberately kept for later.
    Q_INVOKABLE int markAllRoomsRead();
    Q_INVOKABLE void markRoomUnread(const QString &roomId);
    // Favourites (Matrix `m.favourite` tag). The toggle is not applied locally;
    // see the .cpp. isRoomFavourite() reads the client's full room set, so it
    // works for rooms the Space filter hides.
    Q_INVOKABLE bool isRoomFavourite(const QString &roomId) const;
    Q_INVOKABLE void setRoomFavourite(const QString &roomId, bool favourite);
    // "Copy room link": prefers the canonical alias over the room id, with
    // TimelineModel::messagePermalink's percent-encoding (! $ : @ excluded).
    // Static for unit testing.
    Q_INVOKABLE static QString roomPermalink(const QString &roomId,
                                             const QString &canonicalAlias = QString());
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    quint64 filterGeneration() const { return m_filterGeneration; }
    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);
    // The open room, kept visible in Unreads mode. Set by AppController on room
    // switch.
    void setPinnedRoomId(const QString &roomId);

    // Account switch: drop DM profile lookups made under the previous account,
    // then rebuild.
    void clearProfileCaches();

    bool roomFavouritesSupported() const;
    QString favouritesBoundaryRoomId() const { return m_favouritesBoundaryRoomId; }
    int unreadRoomCount() const { return m_unreadRoomCount; }
    int highlightRoomCount() const { return m_highlightRoomCount; }
    // The one classification: the section string and the sort read the same
    // function, so a category is never split across two runs.
    static int orderRankOf(const RoomInfo &room);
    static QString categoryOf(const RoomInfo &room);

    void resetRooms(const QList<RoomInfo> &rooms);
    bool appendRooms(const QList<RoomInfo> &rooms);
    bool prependRooms(const QList<RoomInfo> &rooms);
    bool insertRoom(int index, const RoomInfo &room);
    bool replaceRoom(int index, const RoomInfo &room);
    bool removeRoom(int index);
    bool removeRange(int index, int count);
    bool truncate(int length);
    void clearRooms();

private Q_SLOTS:
    void refresh();
    void refreshRoom(const QString &roomId);
    /// One DM peer's profile arrived. Repaints only the rows that wear that
    /// face; a late profile must never reorder or rebuild the list.
    void onDirectAvatarResolved(const QString &userId);

private:
    QString effectiveAvatarUrl(const RoomInfo &room) const;
    bool passesScopeFilter(const RoomInfo &r) const;
    bool passesFilter(const RoomInfo &r) const;
    QList<RoomInfo> desiredRooms(const QSet<QString> &superseded) const;
    void reconcileRooms();
    // Rooms replaced by a successor the user can reach: a successor exists, we
    // hold a Joined or Invited record for it, and its predecessor points back
    // here. An unverifiable chain leaves the row alone.
    QSet<QString> computeSupersededRoomIds() const;
    void resolveMissingDirectAvatars();
    // The badge a row shows: the advertised answer, else the inference. One
    // place, so the list and findRoom() agree.
    struct BridgeBadge {
        QString networkId;
        QString label;
    };
    BridgeBadge badgeFor(const RoomInfo &r) const;
    // Retired; see favouritesBoundaryRoomId.
    void updateFavouritesBoundary();

    QString m_favouritesBoundaryRoomId;
    int m_unreadRoomCount = 0;
    int m_highlightRoomCount = 0;
    void updateUnreadTotals();
    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    QList<RoomInfo> m_rooms; // Filtered subset actually shown.
    // Rooms whose successor the user can reach, recomputed once per reconcile
    // from the client's full room set (the successor may be filtered out).
    QSet<QString> m_supersededRoomIds;
    QString m_searchQuery;
    QString m_pendingSearchQuery;
    int m_filterMode = 0;
    QString m_pinnedRoomId;
    quint64 m_filterGeneration = 1;
    QTimer m_searchDebounce;
    // Coalesces per-event refreshRoom() calls into one reconcile per turn.
    QTimer m_reconcileCoalesce;
    DirectAvatarResolver m_directAvatars;
    // roomId -> what its bridge advertises (MSC2346), filled by
    // setAdvertisedBridge; never fetched here.
    QHash<QString, BridgeBadge> m_advertisedBridges;

Q_SIGNALS:
    void searchQueryChanged();
    void filterGenerationChanged();
    void filterModeChanged();
    void roomFavouritesSupportedChanged();
    void favouritesBoundaryRoomIdChanged();
    void unreadTotalsChanged();
};
