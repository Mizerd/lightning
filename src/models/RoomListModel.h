#pragma once

#include "matrix/RoomInfo.h"
#include "models/ConversationOrder.h"
#include "models/DirectAvatarResolver.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>

#include <functional>
#include <QVariantMap>
#include <QTimer>

class MatrixClient;
class SpaceManager;

class RoomListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(quint64 filterGeneration READ filterGeneration NOTIFY filterGenerationChanged)
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode NOTIFY filterModeChanged)
    // False on a backend that cannot write room tags; the action is then not
    // offered, since a device-local favourite would disagree with other
    // clients.
    Q_PROPERTY(bool roomFavouritesSupported READ roomFavouritesSupported
                   NOTIFY roomFavouritesSupportedChanged)
    Q_PROPERTY(QString favouritesBoundaryRoomId READ favouritesBoundaryRoomId
                   NOTIFY favouritesBoundaryRoomIdChanged)
    Q_PROPERTY(int unreadRoomCount READ unreadRoomCount
                   NOTIFY unreadTotalsChanged)
    Q_PROPERTY(int highlightRoomCount READ highlightRoomCount
                   NOTIFY unreadTotalsChanged)
    Q_PROPERTY(bool deferReordering READ deferReordering
                   WRITE setDeferReordering NOTIFY deferReorderingChanged)
    Q_PROPERTY(bool orderHeld READ orderHeld NOTIFY orderHeldChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode
                   NOTIFY sortModeChanged)
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
        MemberCountRole,
        CategoryRole,
        HighlightCountRole,
        MarkedUnreadRole,
        HasUnreadRole,
        MembershipRole,
        IsDirectRole,
        IsFavouriteRole,
        IsLowPriorityRole,
        DirectUserIdRole,
        InviterRole,
        InviterUserIdRole,
        InvitePendingRole,
        InviteErrorRole,
        CanonicalAliasRole,
        IdentityColorKeyRole,
        SuccessorRoomIdRole,
        SupersededByAccessibleSuccessorRole,
        NetworkRole,
        NetworkLabelRole,
        ThreadUnreadKindRole,
    };

    explicit RoomListModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setDirectAvatarResolver(DirectAvatarResolver *resolver);

    void setSpaceManager(SpaceManager *spaces);
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);
    void notificationModeChanged();

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap findRoom(const QString &roomId) const;
    Q_INVOKABLE QStringList roomIdsBetween(int first, int last) const;

    void setAdvertisedBridge(const QString &roomId, const QString &networkId,
                             const QString &label);
    void clearAdvertisedBridges();
    Q_INVOKABLE QVariantList recentRooms(int max = 6) const;
    Q_INVOKABLE QVariantList spacesSummary(int max = 8) const;
    Q_INVOKABLE void acceptInvite(const QString &roomId);
    Q_INVOKABLE void rejectInvite(const QString &roomId);
    Q_INVOKABLE void markRoomRead(const QString &roomId);
    Q_INVOKABLE int markAllRoomsRead();
    QStringList roomsToMarkRead() const;
    Q_INVOKABLE int attentionRowFrom(int fromRow, int step,
                                     bool mentionsOnly = false) const;
    Q_INVOKABLE void markRoomUnread(const QString &roomId);
    Q_INVOKABLE bool isRoomFavourite(const QString &roomId) const;
    Q_INVOKABLE void setRoomFavourite(const QString &roomId, bool favourite);
    Q_INVOKABLE void setRoomLowPriority(const QString &roomId, bool lowPriority);
    Q_INVOKABLE static QString roomPermalink(const QString &roomId,
                                             const QString &canonicalAlias = QString());
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    quint64 filterGeneration() const { return m_filterGeneration; }
    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);
    void setPinnedRoomId(const QString &roomId);
    void setCallRoomId(const QString &roomId);
    QString callRoomId() const { return m_callRoomId; }

    bool deferReordering() const { return m_hold.enabled(); }
    void setDeferReordering(bool defer);
    bool orderHeld() const { return m_orderHeld; }
    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);
    Q_INVOKABLE void releaseOrder();
    static constexpr int kHoldCapMs = 0;
    void setHoldCapMs(int ms);

    void clearProfileCaches();

    bool roomFavouritesSupported() const;
    QString favouritesBoundaryRoomId() const { return m_favouritesBoundaryRoomId; }
    int unreadRoomCount() const { return m_unreadRoomCount; }
    int highlightRoomCount() const { return m_highlightRoomCount; }
    // The one classification: the section string and the sort read the same
    // function, so a category is never split across two runs.
    static int orderRankOf(const RoomInfo &room);
    int sortRankOf(const RoomInfo &room) const;
    bool isCallRoom(const QString &roomId) const;
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
    void onDirectAvatarResolved(const QString &userId);

private:
    QString effectiveAvatarUrl(const RoomInfo &room) const;
    bool passesScopeFilter(const RoomInfo &r) const;
    bool passesFilter(const RoomInfo &r) const;
    QList<RoomInfo> desiredRooms(const QSet<QString> &superseded);
    void setOrderHeld(bool held);
    void reconcileRooms();
    QSet<QString> computeSupersededRoomIds() const;
    void resolveMissingDirectAvatars();
    struct BridgeBadge {
        QString networkId;
        QString label;
    };
    BridgeBadge badgeFor(const RoomInfo &r) const;
    void updateFavouritesBoundary();

    QString m_favouritesBoundaryRoomId;
    int m_unreadRoomCount = 0;
    int m_highlightRoomCount = 0;
    void updateUnreadTotals();
    int notificationModeOf(const QString &roomId) const;
    int shownUnread(const RoomInfo &room) const;
    std::function<int(const QString &)> m_modeOf;
    MatrixClient *m_client = nullptr;
    QSet<QString> m_pendingMarkRead;
    SpaceManager *m_spaces = nullptr;
    QList<RoomInfo> m_rooms;
    QSet<QString> m_supersededRoomIds;
    QString m_searchQuery;
    QString m_pendingSearchQuery;
    int m_filterMode = 0;
    QString m_pinnedRoomId;
    QString m_callRoomId;
    quint64 m_filterGeneration = 1;
    QTimer m_searchDebounce;
    QTimer m_reconcileCoalesce;
    DirectAvatarResolver m_ownDirectAvatars;
    DirectAvatarResolver *m_directAvatars = &m_ownDirectAvatars;
    QMetaObject::Connection m_directAvatarsConn;
    conversation::RecencyHold m_hold;
    bool m_orderHeld = false;
    int m_sortMode = conversation::SortByActivity;
    bool m_sortPending = false;
    QTimer m_holdCap;
    QHash<QString, BridgeBadge> m_advertisedBridges;

Q_SIGNALS:
    void roomMarkedRead(const QString &roomId);
    void searchQueryChanged();
    void filterGenerationChanged();
    void filterModeChanged();
    void roomFavouritesSupportedChanged();
    void favouritesBoundaryRoomIdChanged();
    void unreadTotalsChanged();
    void deferReorderingChanged();
    void orderHeldChanged();
    void sortModeChanged();
    void callRoomIdChanged();
};
