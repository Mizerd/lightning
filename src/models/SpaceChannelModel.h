// The Channels navigation layout: three views chosen by the rail — Home,
// Direct Messages, and one per joined Space.
//
// The shape, and why it is this one
// ---------------------------------
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
// This is Sable's model, matched deliberately. The rail carries Home, a
// People tab and then every Space; picking one decides WHICH of the three
// views this model produces. A Space shows its own direct child rooms and its
// subspaces as sibling folders. It never shows a DM as a CHILD of the Space,
// because Matrix gives no way for a DM to be one and a DM under a Space
// folder would be a claim the state does not make.
//
// 2026-08-28: it does show a Space's PEOPLE — a group of the DMs you have
// with people who are in that Space, which is a claim about the Space's
// membership rather than about its children, and is what the Classic layout's
// People chip means once it is scoped. It appears only once that Space's
// roster is a complete, known fact, and every DM remains in the People tab
// whatever it does. See `appendSpacePeople`.
//
// This replaced a design in which every view was the same list narrowed by a
// scope: Home listed every joined Space's folder AND a Direct messages group
// AND a Rooms group, and a scoped Space kept the DM group because dropping it
// made the People filter chip produce nothing. Both problems are gone once
// DMs have a tab of their own — the DM group does not need to survive a
// scope, and Home does not need to repeat what the rail already shows.
//
// 2026-09-05: Home lists the joined DMs AGAIN, as a Direct Messages group of
// its own after Rooms, because the maintainer asked for them there ("listed
// under rooms but keep a separate people dms tab too"). That is a second
// listing of the same conversations, not a scope: the tab stays the complete
// list and keeps the DM invites, and a Space view still never carries one.
//
// Deliberate consequences, each one tested:
//
//  * A room that is a child of two Spaces appears under BOTH Spaces' views.
//    That is what "this Space contains it" means, and inventing a
//    first-parent-wins rule would make one of the two Spaces look incomplete.
//  * A room whose only Space parents are Spaces the account has not joined has
//    no Space view to appear in, so it is in Home's "Rooms". Nothing joined is
//    unreachable.
//  * INVITES are split the same way the rest is: a DM invite is in People, a
//    room invite is at Home. A Space view carries none, because an invite is
//    not yet a member of anything.
//  * STRUCTURE IS FIXED, ORDER IS BY ACTIVITY. Spaces follow the rail's own
//    arrangement (the order the user dragged them into, folders expanded in
//    place) and that is NOT touched here — the rail is hand-arranged and
//    reordering it would throw away the user's own work. Which Space owns
//    which rooms, and each subspace as its own folder, is equally fixed.
//    But WITHIN a group — Home's "Rooms", People's "Chats", a Space's rooms,
//    a Space's People — rows are ordered newest conversation first, through
//    the one comparator the Classic list also uses (models/ConversationOrder.h),
//    so the two layouts cannot disagree about which room is more recent.
//
//    This reverses the rule that stood here until 2026-08-31 ("nothing here
//    is activity-ordered: a channel list whose rows move when somebody speaks
//    is not a channel list"). Alphabetical is a stable order and a useless
//    one: a room somebody had just posted in sat wherever its name put it, so
//    the thing the user was looking for never moved to where they were
//    looking. Unread still changes a row's WEIGHT; now recency also changes
//    its position.
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
    /// closed set the room list's filter chips already write, so the chips
    /// mean the same thing in both layouts instead of going inert in one.
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode
                   NOTIFY filterModeChanged)
    /// The room-list search box. While it is non-empty every folder is opened
    /// and only matching rooms are listed — a room has to be findable whatever
    /// the user last collapsed. Collapse state is untouched, so clearing the
    /// box restores exactly what was collapsed before.
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery
                   NOTIFY searchQueryChanged)
    /// Whether the homeserver can answer a message search. The Message Search
    /// row is dropped when it cannot, rather than offering a dead entry.
    Q_PROPERTY(bool messageSearchSupported READ messageSearchSupported
                   WRITE setMessageSearchSupported
                   NOTIFY messageSearchSupportedChanged)
    /// The rail's selection, verbatim, and it chooses the VIEW: a room id
    /// ('!') is that Space, `peopleViewId()` is Direct Messages, and anything
    /// else (`""` for Home, `"@orphans"`) is Home.
    ///
    /// Written straight from `SpaceManager::activeSpaceId` so the rail and the
    /// column can never disagree about where the user is. It used to strip
    /// every pseudo id to "" and mean "scope", which is why the People tab
    /// needed a value of its own rather than another chip.
    Q_PROPERTY(QString scopeSpaceId READ scopeSpaceId WRITE setScopeSpaceId
                   NOTIFY scopeSpaceIdChanged)
    /// Which of the three views is being produced: "home" | "people" |
    /// "space". A string for the same reason KindRole is one — a bare integer
    /// comparison in QML silently stops matching when a value is inserted.
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY scopeSpaceIdChanged)
    /// True when the account genuinely has nothing to list — no Spaces and no
    /// rooms. Distinct from "the filter matched nothing", which is a fact
    /// about the filter and must not be reported as a fact about the account.
    Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
    /// How many ROOM rows survived the current filter and search. This is the
    /// other half of the distinction `empty` refuses to make: zero here with
    /// `empty` false means the chip or the search box matched nothing, and the
    /// column can say so instead of rendering its two navigation rows over
    /// blank space — which is what made a filter that produced no rooms look
    /// like a filter that did nothing.
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)

public:
    enum Kind {
        /// The home / all-conversations surface. Navigation, not a room: it
        /// has no Matrix identity and nothing is persisted for it.
        LobbyKind = 0,
        /// Opens the global message search.
        SearchKind = 1,
        /// A collapsible group that is not a Space — "Invites", "Rooms".
        GroupKind = 2,
        /// A joined Space, drawn as a collapsible folder of its rooms.
        SpaceKind = 3,
        /// A room. Opens a timeline.
        RoomKind = 4,
        /// A one-shot command row — Create Room, Join with Address, Explore
        /// Spaces, Create Chat. It carries a synthetic '@' id naming the
        /// action and NEVER a room id: the host dispatches on that id, so a
        /// row this model can produce and the host cannot name is a compile
        /// -time-invisible dead row, which is what `actionIds()` and its
        /// contract test exist to prevent.
        ActionKind = 5,
    };
    Q_ENUM(Kind)

    enum Roles {
        /// The room id for a room, the Space id for a Space folder, the
        /// synthetic group id for a group, empty for Lobby and Search. A
        /// synthetic id always starts with '@', which no room id can.
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        /// "lobby" | "search" | "group" | "space" | "room" | "action". A STRING rather
        /// than the enum, because exposing the enum to QML would mean
        /// registering this type with the QML engine purely so a delegate can
        /// name a constant — and a bare integer comparison in QML silently
        /// stops matching when a value is inserted.
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
        /// Headers only: unread/highlight totals of the rooms HIDDEN inside,
        /// so collapsing never hides the fact that something happened. Zero
        /// when expanded — the rows carry their own badges then, and a total
        /// on top of them would double-count what is already visible.
        HiddenUnreadRole,
        HiddenHighlightRole,
        IsFavouriteRole,
        /// The Material Symbols glyph a navigation or action row draws.
        /// Named by the MODEL rather than mapped in the delegate because the
        /// rows are a closed set the model owns, and a chooser in QML that
        /// misses one renders tofu — the bundled icon font is a SUBSET, so
        /// every name here is pinned by `IconChromeTest`.
        IconNameRole,
    };

    explicit SpaceChannelModel(QObject *parent = nullptr);

    /// Everything this model reads. The rooms come from the CLIENT rather than
    /// from RoomListModel on purpose: that model is scoped to the active Space
    /// and filtered by the chips, which is right for Classic and would make
    /// this layout's global "Rooms" group disappear the moment a Space was
    /// selected.
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
    /// How many times rebuild() has run. A test seam, and the only honest way
    /// to measure the coalescing: counting the CLIENT's rooms() calls also
    /// counts SpaceManager's own rebuilds, which are not this model's cost.
    int rebuildCountForTest() const { return m_rebuildCount; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Collapse or expand one Space folder or group. Persisted locally, as
    /// Discord and Sable both persist theirs: a collapse is how the user wants
    /// the column to look, and forgetting it on every restart makes the
    /// setting pointless.
    Q_INVOKABLE void toggleCollapsed(const QString &headerId);
    Q_INVOKABLE bool isCollapsed(const QString &headerId) const;

    /// The model row showing `roomId`, or -1. A Space folder is deliberately
    /// NOT a match for its own id: highlighting it would mark the whole group
    /// as "the room you are in".
    Q_INVOKABLE int rowForRoom(const QString &roomId) const;

    /// Synthetic header ids. Not room ids and never sent anywhere; the '@'
    /// prefix is what keeps them from colliding with one.
    static QString invitesGroupId() { return QStringLiteral("@invites"); }
    /// Direct messages have a VIEW of their own now, reached from the rail,
    /// and this is the group inside it. Matrix has no way for a DM to be a
    /// Space's child, so a DM can never appear under a Space heading.
    static QString directsGroupId() { return QStringLiteral("@directs"); }
    static QString roomsGroupId() { return QStringLiteral("@rooms"); }
    /// Home's own Direct Messages group (2026-09-05): the joined DMs listed
    /// again under Rooms. Collapsed independently of `directsGroupId()`,
    /// which is the tab's group.
    static QString homeDirectsGroupId() { return QStringLiteral("@home-directs"); }
    /// The People group inside a SPACE view: the DMs you have with people who
    /// are in that Space. A different group from `directsGroupId()`, and it
    /// has to be — the two are collapsed independently, and one of them is a
    /// scope while the other is the complete list.
    static QString spacePeopleGroupId()
    { return QStringLiteral("@space-people"); }

    /// The rail selection that means Direct Messages. Shared with
    /// SpaceManager, which owns the pseudo rail rows — one definition, or the
    /// tab and the view disagree about which string selects which.
    static QString peopleViewId();

    /// The action rows' synthetic ids. Every one must be handled by the host;
    /// `ChannelActionContractTest` asserts the presenter names all four.
    static QString createRoomActionId() { return QStringLiteral("@new-room"); }
    static QString joinAddressActionId() { return QStringLiteral("@join-address"); }
    static QString exploreSpacesActionId() { return QStringLiteral("@explore"); }
    static QString createChatActionId() { return QStringLiteral("@new-chat"); }
    /// All of them, in no particular order. A test seam and the closed set.
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
        /// When somebody last spoke here. Carried on the row so a room can
        /// MOVE when it receives a message: applyRows diffs rows by value, so
        /// a sort key the row does not hold is a sort key the diff cannot see
        /// change, and the row would sit still while the list claimed to be
        /// activity-ordered. Empty for headers, actions, Lobby and Search.
        QDateTime lastActivity;

        bool operator==(const Row &other) const;
        bool operator!=(const Row &other) const { return !(*this == other); }
    };

    /// Newest conversation first, over this model's rows.
    ///
    /// Delegates to the SAME comparator the Classic list uses
    /// (models/ConversationOrder.h), deliberately: two lists over the same
    /// rooms that disagree about which is newer is a bug the user sees as
    /// rooms swapping places when they switch layout.
    static bool byRecency(const Row &a, const Row &b);

    /// Cancels any queued rebuild before running, so that after this returns
    /// nothing armed under the previous state is still on its way. Every
    /// source change ends here, which is what makes that invariant reach them.
    void rebuild();
    /// Coalesces a burst of source signals into ONE rebuild per event-loop
    /// turn. Every caller that fires from a signal uses this; the direct
    /// setters keep calling rebuild() so a property write is still
    /// synchronous for its caller.
    void scheduleRebuild();
    void applyRows(QVector<Row> rows);
    void loadCollapsed() const;
    void saveCollapsed();
    bool filterAdmits(bool isDirect, bool unread) const;
    bool matchesQuery(const QString &name) const;
    /// Appends `rooms` under a header, dropping the header again when nothing
    /// survived the filter — a "Rooms" label over an empty list is worse than
    /// no label. Returns how many rooms were appended.
    int appendGroup(QVector<Row> &rows, Row header, QVector<Row> rooms);
    /// The Spaces the column lists, in rail order: the selected Space
    /// followed by its subspaces (recursively, deduped, cycle-safe). EMPTY in
    /// the Home and People views — the rail already lists every Space, and
    /// repeating them under Home is what made picking one look like it did
    /// nothing.
    QStringList listedSpaceIds() const;
    /// One command row.
    Row actionRow(const QString &id, const QString &name,
                  const QString &icon) const;
    /// The three view builders. Each one appends to `rows` and returns how
    /// many ROOM rows survived the filter and the search.
    int buildHome(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildPeople(QVector<Row> &rows, const QList<RoomInfo> &allRooms);
    int buildSpace(QVector<Row> &rows, const QHash<QString, RoomInfo> &byId);
    /// The Space view's People group — DMs with members of the selected
    /// Space. Adds nothing at all until that Space's roster is a complete,
    /// known fact; see the implementation for why this fails the opposite way
    /// from the Classic list's scope.
    int appendSpacePeople(QVector<Row> &rows,
                          const QHash<QString, RoomInfo> &byId);
    /// Shared by all three: one room's Row.
    Row roomRow(const RoomInfo &info) const;

    MatrixClient *m_client = nullptr;
    /// A DM usually carries no room avatar of its own; the face belongs to the
    /// other person. Deriving that is the resolver's job, and it is the SAME
    /// derivation the Classic list uses — the Channels column showed initials
    /// for every DM while Home showed the real pictures because this model
    /// read RoomInfo::avatarUrl raw.
    DirectAvatarResolver m_directAvatars;
    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    SettingsManager *m_settings = nullptr;

    int m_filterMode = 0;
    QString m_searchQuery;
    bool m_messageSearchSupported = false;
    /// The rail selection verbatim. `m_scopeSpaceId` is the Space id it names,
    /// or empty; the two are separate because "@people" is a real selection
    /// that is not a Space, and collapsing it to "" is exactly what made a
    /// People tab impossible to express.
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
