// The Channels navigation layout: EVERY joined Space as a flat, collapsible
// folder of rooms, above a group for the rooms that belong to no Space.
//
// The shape, and why it is this one
// ---------------------------------
//
//   Lobby
//   Message Search
//
//   Invites            (only when there are any)
//   Rooms       >      every joined room no Space folder will list
//   Space A     v
//     room 1
//     room 2
//   Space B     >
//
// This replaces an earlier design that scoped the whole layout to the ACTIVE
// Space and rendered its child Spaces as nested categories. Two things were
// wrong with it and both were structural:
//
//  * It could not exist at Home. With no Space selected there was no
//    hierarchy, so the host silently fell back to Classic — the user chose a
//    navigation layout and got the other one, with nothing saying why.
//  * Nesting a Space tree inside a sidebar is unreadable by about three
//    levels, and it duplicated: a subspace's rooms appeared under the
//    subspace's category AND (transitively) under the top-level Space.
//
// So the visual structure is FLAT BY SPACE. A subspace is a joined Space like
// any other and gets its own folder at the same level; nothing is nested and
// nothing is listed twice under one heading. Matrix relationships still decide
// MEMBERSHIP — a folder lists the Space's DIRECT child rooms — they just do
// not decide indentation.
//
// Deliberate consequences, each one tested:
//
//  * A room that is a child of two Spaces appears under BOTH folders. That is
//    what "this Space contains it" means, and inventing a first-parent-wins
//    rule would make one of the two Spaces look incomplete.
//  * A room whose only Space parents are Spaces the account has not joined has
//    no folder to appear in, so it is in "Rooms". Nothing joined is
//    unreachable.
//  * ORDER IS STABLE. Spaces follow the rail's own arrangement (the order the
//    user dragged them into, folders expanded in place); rooms follow their
//    Space's `m.space.child` order, and "Rooms" is sorted by name. Nothing
//    here is activity-ordered: a channel list whose rows move when somebody
//    speaks is not a channel list. Unread changes a row's WEIGHT, never its
//    position.
//  * There is no Favourites group and no Direct messages group. Sable has
//    neither, DMs live in "Rooms" like every other unparented room, and the
//    People filter chip still reaches them.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
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
    /// Narrows the column to ONE Space: its own folder, then each of its
    /// subspaces as a folder of their own, and nothing else. Empty is the
    /// whole account, which is what Lobby returns to.
    ///
    /// This is the rail's selection, and it is what makes clicking a Space in
    /// Channels mean something. Without it the column showed every Space you
    /// are in whatever you clicked, so selecting one did nothing visible.
    /// It is NOT a return to the old active-Space design: there is still no
    /// fallback to Classic, Lobby is always one row away, and a scoped Space's
    /// subspaces stay FLAT folders rather than nesting.
    Q_PROPERTY(QString scopeSpaceId READ scopeSpaceId WRITE setScopeSpaceId
                   NOTIFY scopeSpaceIdChanged)
    /// True when the account genuinely has nothing to list — no Spaces and no
    /// rooms. Distinct from "the filter matched nothing", which is a fact
    /// about the filter and must not be reported as a fact about the account.
    Q_PROPERTY(bool empty READ empty NOTIFY countChanged)

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
    };
    Q_ENUM(Kind)

    enum Roles {
        /// The room id for a room, the Space id for a Space folder, the
        /// synthetic group id for a group, empty for Lobby and Search. A
        /// synthetic id always starts with '@', which no room id can.
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        /// "lobby" | "search" | "group" | "space" | "room". A STRING rather
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
    static QString roomsGroupId() { return QStringLiteral("@rooms"); }

Q_SIGNALS:
    void countChanged();
    void filterModeChanged();
    void searchQueryChanged();
    void messageSearchSupportedChanged();
    void scopeSpaceIdChanged();

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

        bool operator==(const Row &other) const;
        bool operator!=(const Row &other) const { return !(*this == other); }
    };

    void rebuild();
    void applyRows(QVector<Row> rows);
    void loadCollapsed() const;
    void saveCollapsed();
    bool filterAdmits(bool isDirect, bool unread) const;
    bool matchesQuery(const QString &name) const;
    /// Appends `rooms` under a header, dropping the header again when nothing
    /// survived the filter — a "Rooms" label over an empty list is worse than
    /// no label. Returns how many rooms were appended.
    int appendGroup(QVector<Row> &rows, Row header, QVector<Row> rooms);
    /// The Spaces the column lists, in rail order: every joined Space when
    /// unscoped, or the scoped Space followed by its subspaces (recursively,
    /// deduped, cycle-safe) when scoped.
    QStringList listedSpaceIds() const;

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
    QString m_scopeSpaceId;
    QVector<Row> m_rows;
    /// Whether anything at all exists to list, independent of the filter.
    bool m_accountHasContent = false;

    mutable QSet<QString> m_collapsed;
    mutable bool m_collapsedLoaded = false;
};
