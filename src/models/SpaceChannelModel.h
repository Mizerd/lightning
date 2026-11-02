#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "models/ConversationOrder.h"
#include "models/DirectAvatarResolver.h"

class MatrixClient;
class RailLayoutStore;
class SettingsManager;
class SpaceManager;

class SpaceChannelModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode
                   NOTIFY filterModeChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery
                   NOTIFY searchQueryChanged)
    Q_PROPERTY(bool messageSearchSupported READ messageSearchSupported
                   WRITE setMessageSearchSupported
                   NOTIFY messageSearchSupportedChanged)
    Q_PROPERTY(QString scopeSpaceId READ scopeSpaceId WRITE setScopeSpaceId
                   NOTIFY scopeSpaceIdChanged)
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY scopeSpaceIdChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)
    Q_PROPERTY(bool deferReordering READ deferReordering
                   WRITE setDeferReordering NOTIFY deferReorderingChanged)
    Q_PROPERTY(bool orderHeld READ orderHeld NOTIFY orderHeldChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode
                   NOTIFY sortModeChanged)
    Q_PROPERTY(bool keepImportantRoomsVisible READ keepImportantRoomsVisible
                   WRITE setKeepImportantRoomsVisible
                   NOTIFY keepImportantRoomsVisibleChanged)

public:
    enum Kind {
        LobbyKind = 0,
        SearchKind = 1,
        GroupKind = 2,
        SpaceKind = 3,
        RoomKind = 4,
        ActionKind = 5,
    };
    Q_ENUM(Kind)

    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        DepthRole,
        AvatarUrlRole,
        IdentityColorKeyRole,
        IsDirectRole,
        IsInviteRole,
        EncryptedRole,
        UnreadCountRole,
        HighlightCountRole,
        HasUnreadRole,
        ThreadUnreadKindRole,
        CollapsedRole,
        HiddenUnreadRole,
        HiddenHighlightRole,
        IsFavouriteRole,
        IsLowPriorityRole,
        IconNameRole,
    };

    explicit SpaceChannelModel(QObject *parent = nullptr);

    void setSources(MatrixClient *client, SpaceManager *spaces,
                    RailLayoutStore *layout);
    void setSettings(SettingsManager *settings);
    void setDirectAvatarResolver(DirectAvatarResolver *resolver);

    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    bool messageSearchSupported() const { return m_messageSearchSupported; }
    void setMessageSearchSupported(bool supported);
    QString scopeSpaceId() const { return m_scopeSpaceId; }
    void setScopeSpaceId(const QString &spaceId);
    bool empty() const;
    int matchCount() const { return m_matchCount; }
    bool deferReordering() const { return m_hold.enabled(); }
    void setDeferReordering(bool defer);
    bool orderHeld() const { return m_orderHeld; }
    void setCallRoomId(const QString &roomId);
    QString callRoomId() const { return m_callRoomId; }
    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);
    bool keepImportantRoomsVisible() const;
    void setKeepImportantRoomsVisible(bool enabled);
    Q_INVOKABLE void releaseOrder();
    void setOpenRoomId(const QString &roomId);
    Q_INVOKABLE void setRoomHovered(const QString &roomId, bool hovered);
    static constexpr int kHoldCapMs = 0;
    void setHoldCapMs(int ms);
    int rebuildCountForTest() const { return m_rebuildCount; }

    int rowCount(const QModelIndex &parent = {}) const override;
    Q_INVOKABLE QStringList roomIdsBetween(int first, int last) const;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void toggleCollapsed(const QString &headerId);
    Q_INVOKABLE bool isCollapsed(const QString &headerId) const;

    Q_INVOKABLE int rowForRoom(const QString &roomId) const;
    Q_INVOKABLE int attentionRowFrom(int fromRow, int step,
                                     bool mentionsOnly = false) const;

    static QString invitesGroupId() { return QStringLiteral("@invites"); }
    static QString directsGroupId() { return QStringLiteral("@directs"); }
    static QString roomsGroupId() { return QStringLiteral("@rooms"); }
    static QString homeDirectsGroupId() { return QStringLiteral("@home-directs"); }
    static QString spacePeopleGroupId()
    { return QStringLiteral("@space-people"); }

    static QString peopleViewId();

    static QString createRoomActionId() { return QStringLiteral("@new-room"); }
    static QString joinAddressActionId() { return QStringLiteral("@join-address"); }
    static QString exploreSpacesActionId() { return QStringLiteral("@explore"); }
    static QString createChatActionId() { return QStringLiteral("@new-chat"); }
    static QStringList actionIds();

    QString viewKind() const;

Q_SIGNALS:
    void countChanged();
    void filterModeChanged();
    void searchQueryChanged();
    void messageSearchSupportedChanged();
    void scopeSpaceIdChanged();
    void matchCountChanged();
    void deferReorderingChanged();
    void orderHeldChanged();
    void sortModeChanged();
    void keepImportantRoomsVisibleChanged();

private:
    struct Row {
        QString id;
        QString name;
        Kind kind = RoomKind;
        int depth = 0;
        QString avatarUrl;
        QString identityColorKey;
        bool isDirect = false;
        bool isInvite = false;
        bool encrypted = false;
        int unread = 0;
        int highlight = 0;
        bool hasUnread = false;
        int threadUnreadKind = 0;
        bool favourite = false;
        bool lowPriority = false;
        int hiddenUnread = 0;
        int hiddenHighlight = 0;
        QString iconName;
        /// When somebody last spoke here. Held on the row because applyRows
        /// diffs rows by value; a sort key the row lacks could never make it
        /// move. Empty for headers, actions, Lobby and Search.
        QDateTime lastActivity;
        /// Where this row would sort with nothing held. Not part of identity:
        /// it only decides whether a held order differs from the live one.
        QDateTime targetActivity;

        bool operator==(const Row &other) const;
        bool operator!=(const Row &other) const { return !(*this == other); }
    };

    static bool byRecency(const Row &a, const Row &b);
    static bool byFavouriteThenRecency(const Row &a, const Row &b);
    void sortGroup(QVector<Row> &rooms, bool favouritesFirst);
    void sortSpaceChannels(QVector<Row> &rooms);
    void setOrderHeld(bool held);

    void rebuild();
    /// Coalesces a burst of source signals into one rebuild per event-loop
    /// turn. Signal handlers use this; direct setters call rebuild()
    /// synchronously.
    void scheduleRebuild();
    void applyRows(QVector<Row> rows);
    void loadCollapsed() const;
    void saveCollapsed();
    bool filterAdmits(bool isDirect, bool unread, bool favourite) const;
    bool matchesQuery(const QString &name) const;
    int appendGroup(QVector<Row> &rows, Row header, QVector<Row> rooms);
    QStringList listedSpaceIds(const QHash<QString, RoomInfo> &byId) const;
    QStringList childSpacesOf(const QString &spaceId,
                              const QHash<QString, RoomInfo> &byId) const;
    Row actionRow(const QString &id, const QString &name,
                  const QString &icon) const;
    int buildHome(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildPeople(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildSpace(QVector<Row> &rows, const QHash<QString, RoomInfo> &byId);
    int appendSpacePeople(QVector<Row> &rows,
                          const QHash<QString, RoomInfo> &byId);
    Row roomRow(const RoomInfo &info) const;

    MatrixClient *m_client = nullptr;
    DirectAvatarResolver m_ownDirectAvatars;
    DirectAvatarResolver *m_directAvatars = &m_ownDirectAvatars;
    QMetaObject::Connection m_directAvatarsConn;
    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    SettingsManager *m_settings = nullptr;

    int m_filterMode = 0;
    QString m_searchQuery;
    bool m_messageSearchSupported = false;
    /// The rail selection verbatim. `m_scopeSpaceId` is the Space it names, or
    /// empty; "@people" is a real selection that is not a Space.
    QString m_selection;
    QString m_scopeSpaceId;
    bool m_peopleView = false;
    QVector<Row> m_rows;
    QTimer m_rebuildCoalesce;
    /// The stamps rooms are ordered by. Mutable: roomRow() is const and
    /// records what it hands out.
    mutable conversation::RecencyHold m_hold;
    bool m_orderHeld = false;
    QString m_callRoomId;
    QString m_hoveredRoomId;
    int m_sortMode = conversation::SortByActivity;
    bool m_sortPending = false;
    QTimer m_holdCap;
    bool m_accountHasContent = false;
    int m_matchCount = 0;
    int m_rebuildCount = 0;

    mutable QSet<QString> m_collapsed;
    mutable bool m_collapsedLoaded = false;
};
