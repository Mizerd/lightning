// The Channels navigation layout: three views chosen by the rail — Home,
// Direct Messages, and one per joined Space (Sable's model).
//
//   Home                        Direct Messages          A Space
//   ----                        ---------------          -------
//   Create Room                 Create Chat              Lobby
//   Join with Address                                    Message Search
//   Explore Spaces              Invites   (DM invites)
//   Message Search              Chats     >              Rooms      >
//                                 person 1                 room 1
//   Invites  (room invites)       person 2                 room 2
//   Rooms    >                                           Subspace   >
//     room 1
//     room 2
//   Direct Messages >
//     person 1
//
// A Space shows its direct child rooms and its subspaces as sibling folders.
// It never shows a DM as a child (Matrix cannot express one), but it can show
// a People group: DMs with members of that Space, once its roster is known
// (see appendSpacePeople). Home lists joined DMs again after Rooms; the
// Direct Messages tab remains the complete list and holds DM invites.
//
// Deliberate consequences, each tested:
//
//  * A room that is a child of two Spaces appears under both Spaces' views.
//  * A room whose only parents are unjoined Spaces appears in Home's "Rooms",
//    so nothing joined is unreachable.
//  * Invites split the same way: DM invites in People, room invites at Home,
//    none in a Space view.
//  * Structure is fixed, order is by activity. Spaces follow the rail's
//    hand-arranged order, untouched here. Within a group, rows are newest
//    first through the comparator the Classic list uses
//    (models/ConversationOrder.h), so the two layouts agree.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "models/DirectAvatarResolver.h"

class MatrixClient;
class RailLayoutStore;
class SettingsManager;
class SpaceManager;

class SpaceChannelModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    /// Which groups to include: 0 All, 1 People, 2 Rooms, 3 Unreads. The same
    /// closed set the room list's filter chips write.
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode
                   NOTIFY filterModeChanged)
    /// The room-list search box. While non-empty every folder is opened and
    /// only matching rooms are listed; collapse state is untouched, so clearing
    /// the box restores it.
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery
                   NOTIFY searchQueryChanged)
    /// Whether the homeserver can answer a message search; the Message Search
    /// row is dropped when it cannot.
    Q_PROPERTY(bool messageSearchSupported READ messageSearchSupported
                   WRITE setMessageSearchSupported
                   NOTIFY messageSearchSupportedChanged)
    /// The rail's selection, verbatim, and it chooses the view: a room id ('!')
    /// is that Space, `peopleViewId()` is Direct Messages, and anything else
    /// (`""` for Home, `"@orphans"`) is Home. Written from
    /// `SpaceManager::activeSpaceId` so rail and column always agree.
    Q_PROPERTY(QString scopeSpaceId READ scopeSpaceId WRITE setScopeSpaceId
                   NOTIFY scopeSpaceIdChanged)
    /// Which view is produced: "home" | "people" | "space". A string, like
    /// KindRole, because an integer comparison in QML silently stops matching
    /// when a value is inserted.
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY scopeSpaceIdChanged)
    /// True when the account has nothing to list (no Spaces, no rooms).
    /// Distinct from "the filter matched nothing", which is not a fact about
    /// the account.
    Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
    /// Room rows that survived the current filter and search. Zero here with
    /// `empty` false means the filter matched nothing, so the column can say
    /// so.
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)

public:
    enum Kind {
        /// The home / all-conversations surface. Navigation, not a room: no
        /// Matrix identity and nothing persisted.
        LobbyKind = 0,
        /// Opens the global message search.
        SearchKind = 1,
        /// A collapsible group that is not a Space — "Invites", "Rooms".
        GroupKind = 2,
        /// A joined Space, drawn as a collapsible folder of its rooms.
        SpaceKind = 3,
        /// A room. Opens a timeline.
        RoomKind = 4,
        /// A one-shot command row (Create Room, Join with Address, Explore
        /// Spaces, Create Chat). It carries a synthetic '@' action id, never a
        /// room id; the host dispatches on it, and `actionIds()` plus its
        /// contract test keep every id handled.
        ActionKind = 5,
    };
    Q_ENUM(Kind)

    enum Roles {
        /// The room id for a room, the Space id for a Space folder, the
        /// synthetic group id for a group, empty for Lobby and Search. A
        /// synthetic id always starts with '@', which no room id can.
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        /// "lobby" | "search" | "group" | "space" | "room" | "action". A string
        /// rather than the enum, so the type need not be registered with QML
        /// and comparisons cannot silently shift when a value is inserted.
        KindRole,
        /// 0 for a header, 1 for a room inside one. Drives indentation only.
        DepthRole,
        AvatarUrlRole,
        IdentityColorKeyRole,
        IsDirectRole,
        IsInviteRole,
        EncryptedRole,
        UnreadCountRole,
        HighlightCountRole,
        HasUnreadRole,
        /// Headers only: whether this one is collapsed.
        CollapsedRole,
        /// Headers only: unread/highlight totals of the rooms hidden inside, so
        /// collapsing never hides activity. Zero when expanded, where the rows
        /// carry their own badges.
        HiddenUnreadRole,
        HiddenHighlightRole,
        IsFavouriteRole,
        /// The Material Symbols glyph a navigation or action row draws. Named
        /// by the model because the rows are a closed set it owns; the bundled
        /// font is a subset, so every name is pinned by `IconChromeTest`.
        IconNameRole,
    };

    explicit SpaceChannelModel(QObject *parent = nullptr);

    /// Everything this model reads. Rooms come from the client rather than
    /// RoomListModel, which is scoped to the active Space and filtered by the
    /// chips.
    void setSources(MatrixClient *client, SpaceManager *spaces,
                    RailLayoutStore *layout);
    /// Where the collapse state is kept. Local, never sent to the server.
    void setSettings(SettingsManager *settings);

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
    /// How many times rebuild() has run. Test seam for the coalescing; counting
    /// the client's rooms() calls would include SpaceManager's own rebuilds.
    int rebuildCountForTest() const { return m_rebuildCount; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Collapse or expand one Space folder or group. Persisted locally.
    Q_INVOKABLE void toggleCollapsed(const QString &headerId);
    Q_INVOKABLE bool isCollapsed(const QString &headerId) const;

    /// The row showing `roomId`, or -1. A Space folder does not match its own
    /// id, or the whole group would read as "the room you are in".
    Q_INVOKABLE int rowForRoom(const QString &roomId) const;

    /// Synthetic header ids: never sent anywhere; the '@' prefix keeps them
    /// from colliding with room ids.
    static QString invitesGroupId() { return QStringLiteral("@invites"); }
    /// The group inside the Direct Messages view. A DM can never appear under a
    /// Space heading.
    static QString directsGroupId() { return QStringLiteral("@directs"); }
    static QString roomsGroupId() { return QStringLiteral("@rooms"); }
    /// Home's own Direct Messages group, collapsed independently of
    /// `directsGroupId()`.
    static QString homeDirectsGroupId() { return QStringLiteral("@home-directs"); }
    /// The People group inside a Space view: DMs with that Space's members.
    /// Separate from `directsGroupId()` so the two collapse independently; one
    /// is a scope, the other the complete list.
    static QString spacePeopleGroupId()
    { return QStringLiteral("@space-people"); }

    /// The rail selection that means Direct Messages. Shared with SpaceManager,
    /// which owns the pseudo rail rows, so the tab and view agree.
    static QString peopleViewId();

    /// The action rows' synthetic ids. Every one must be handled by the host;
    /// `ChannelActionContractTest` asserts the presenter names all four.
    static QString createRoomActionId() { return QStringLiteral("@new-room"); }
    static QString joinAddressActionId() { return QStringLiteral("@join-address"); }
    static QString exploreSpacesActionId() { return QStringLiteral("@explore"); }
    static QString createChatActionId() { return QStringLiteral("@new-chat"); }
    /// All of them, in no particular order. Test seam and the closed set.
    static QStringList actionIds();

    QString viewKind() const;

Q_SIGNALS:
    void countChanged();
    void filterModeChanged();
    void searchQueryChanged();
    void messageSearchSupportedChanged();
    void scopeSpaceIdChanged();
    void matchCountChanged();

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
        bool favourite = false;
        int hiddenUnread = 0;
        int hiddenHighlight = 0;
        QString iconName;
        /// When somebody last spoke here. Held on the row because applyRows
        /// diffs rows by value; a sort key the row lacks could never make it
        /// move. Empty for headers, actions, Lobby and Search.
        QDateTime lastActivity;

        bool operator==(const Row &other) const;
        bool operator!=(const Row &other) const { return !(*this == other); }
    };

    /// Newest conversation first, via the same comparator as the Classic list
    /// (models/ConversationOrder.h) so the two layouts agree.
    static bool byRecency(const Row &a, const Row &b);
    /// Favourites first within a group, then recency.
    static bool byFavouriteThenRecency(const Row &a, const Row &b);

    /// Cancels any queued rebuild before running, so nothing armed under the
    /// previous state is still pending afterwards. Every source change ends
    /// here.
    void rebuild();
    /// Coalesces a burst of source signals into one rebuild per event-loop
    /// turn. Signal handlers use this; direct setters call rebuild()
    /// synchronously.
    void scheduleRebuild();
    void applyRows(QVector<Row> rows);
    void loadCollapsed() const;
    void saveCollapsed();
    bool filterAdmits(bool isDirect, bool unread) const;
    bool matchesQuery(const QString &name) const;
    /// Appends `rooms` under a header, dropping the header when nothing
    /// survived the filter. Returns how many rooms were appended.
    int appendGroup(QVector<Row> &rows, Row header, QVector<Row> rooms);
    /// The Spaces the column lists: the selected Space first, then its
    /// subspaces in rail order (recursive, deduped, cycle-safe). Empty in Home
    /// and People; the rail already lists every Space.
    QStringList listedSpaceIds(const QHash<QString, RoomInfo> &byId) const;
    /// Every joined child Space of `spaceId`: the rail's nesting plus the
    /// Space's own `m.space.child` state. See the implementation.
    QStringList childSpacesOf(const QString &spaceId,
                              const QHash<QString, RoomInfo> &byId) const;
    /// One command row.
    Row actionRow(const QString &id, const QString &name,
                  const QString &icon) const;
    /// The three view builders. Each appends to `rows` and returns how many
    /// room rows survived the filter and search.
    int buildHome(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildPeople(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildSpace(QVector<Row> &rows, const QHash<QString, RoomInfo> &byId);
    /// The Space view's People group. Adds nothing until the Space's roster is
    /// complete; see the implementation for why this fails closed.
    int appendSpacePeople(QVector<Row> &rows,
                          const QHash<QString, RoomInfo> &byId);
    /// Shared by all three: one room's Row.
    Row roomRow(const RoomInfo &info) const;

    MatrixClient *m_client = nullptr;
    /// A DM's face belongs to the other person; the resolver derives it, the
    /// same way the Classic list does.
    DirectAvatarResolver m_directAvatars;
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
    /// Whether anything at all exists to list, independent of the filter.
    bool m_accountHasContent = false;
    /// Room rows that survived the filter and the search.
    int m_matchCount = 0;
    int m_rebuildCount = 0;

    mutable QSet<QString> m_collapsed;
    mutable bool m_collapsedLoaded = false;
};
