#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.4.1: real space model. Rebuilt from the active MatrixClient every time
// `roomsChanged` fires. Exposes:
//   - a list model of Space rooms for QML (name, id, unread total, avatar);
//   - `activeSpaceId` — selection state driving the RoomListModel filter;
//   - `roomsInSpace(spaceId)` — hierarchy lookup;
//   - `includesRoom(spaceId, roomId)` — filter helper for RoomListModel.
//
// The pseudo-space id "" (empty string) means "All rooms" and matches every
// room. The pseudo-space id "@orphans" matches every room that is not a
// child of any Space.
//
// Space *editing* (creating a Space, adding a room to a Space, power-level
// checks) is out of scope for v0.4.1 and documented as v0.5 work.
class SpaceManager : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString activeSpaceId READ activeSpaceId WRITE setActiveSpaceId NOTIFY activeSpaceIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY spacesChanged)
    Q_PROPERTY(bool hasSpaces READ hasSpaces NOTIFY spacesChanged)
    // v0.6.5: the number of REAL joined Spaces (excludes the "All rooms" and
    // orphans pseudo-rows) — the account switcher's honest "N spaces" meta.
    Q_PROPERTY(int spaceCount READ spaceCount NOTIFY spacesChanged)

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
        // The one PRIMARY parent Space this one is displayed under, or empty
        // when it is a root. Matrix permits several parents and malformed
        // state permits cycles; see rebuild() for how one is chosen and why
        // it is stable.
        ParentSpaceIdRole,
        // How many joined child SPACES this one has — the rail's expander
        // gate. Zero means "nothing to expand", which is a different fact
        // from "no rooms".
        ChildSpaceCountRole,
    };

    // Well-known pseudo-space ids surfaced through the model as extra rows
    // so QML can render them consistently with real Spaces.
    static QString allRoomsId()   { return QStringLiteral(""); }
    static QString orphansId()    { return QStringLiteral("@orphans"); }
    /// Direct Messages. A rail SELECTION, not a container: no room is "in"
    /// it in the sense the other two are — `roomsInSpace` answers with every
    /// DM so a consumer that scopes by id still gets the honest set, but a
    /// DM's Space membership is unchanged and unchangeable (Matrix has no
    /// way to make a DM a Space's child).
    ///
    /// The row is only OFFERED in the Channels layout, where DMs have a view
    /// of their own; Classic keeps its People filter chip and never sees it.
    static QString peopleId()     { return QStringLiteral("@people"); }

    explicit SpaceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString activeSpaceId() const { return m_activeSpaceId; }
    void    setActiveSpaceId(const QString &spaceId);
    bool    hasSpaces() const { return !m_spaces.isEmpty(); }
    int     spaceCount() const { return m_spaces.size(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Hierarchy queries. Return the set of room ids that belong to
    // `spaceId`. `allRoomsId()` returns every room; `orphansId()` returns
    // rooms not referenced by any Space.
    // Every row as a map, in model order, for a presentation layer that has
    // to REORDER them (the rail's folders and drag ordering). A ListView
    // cannot reorder a QAbstractListModel from QML, and this model's order is
    // the hierarchy's, not the user's.
    Q_INVOKABLE QVariantList allSpaces() const;
    Q_INVOKABLE QStringList roomsInSpace(const QString &spaceId) const;
    Q_INVOKABLE bool includesRoom(const QString &spaceId, const QString &roomId) const;
    // v0.7 design shell: display name for the room-list workspace header.
    Q_INVOKABLE QString spaceName(const QString &spaceId) const;

    // v0.7 Space Home. Presentation snapshot of one Space (name, topic,
    // avatar, child/unread totals) and its children in authoritative
    // m.space.child order with unread/mention state. `addableRooms` lists
    // joined, non-Space rooms that are NOT yet children (name-filtered) for
    // the "Add existing room" picker.
    Q_INVOKABLE QVariantMap spaceInfo(const QString &spaceId) const;
    Q_INVOKABLE QVariantList childRoomsDetailed(const QString &spaceId) const;
    // 2026-08-18 tester report #2 ("Land of the Insane"): JOINED child
    // sub-spaces of a space, in room-list order (the Rust spaces event's
    // ordering — m.space.child order keys are NOT honored here). rebuild()
    // deliberately flattens subspace ROOMS into the ancestor's membership;
    // this surfaces the subspaces THEMSELVES so Space Home can nest them
    // (unjoined subspaces come from /hierarchy via RoomDiscoveryController
    // and are rendered as join offers).
    Q_INVOKABLE QVariantList childSpacesDetailed(const QString &spaceId) const;
    // The joined child SPACES of `spaceId`, in the Space's own
    // `m.space.child` order, restricted to the ones whose PRIMARY parent is
    // this Space. Restricting to the primary parent is what makes the rail's
    // tree a tree: a subspace with two joined parents is nested under
    // exactly one of them, deterministically, instead of appearing twice.
    Q_INVOKABLE QStringList childSpaceIds(const QString &spaceId) const;
    // Whether `roomId` is a DIRECT child of any joined Space. The Channels
    // layout's "Rooms" group is the complement of this: every joined room
    // that no Space folder will list. Direct rather than transitive because
    // a subspace is itself a Space folder in that layout, so its own
    // children are already reachable there.
    Q_INVOKABLE bool roomInAnySpace(const QString &roomId) const;
    // 2026-08-23 Channels navigation layout: DIRECT child rooms only, in
    // `m.space.child` state order.
    //
    // NOT the same thing as childRoomsDetailed, which is TRANSITIVE:
    // rebuild() deliberately flattens a subspace's rooms into every
    // ancestor's membership, so childRoomsDetailed on a Space with three
    // subspaces returns every room in the tree as one undifferentiated run.
    // That is right for "show me everything in this Space" and wrong for a
    // channel list, where the whole point is the structure the Space's admin
    // built. This reads the Space's OWN `childRoomIds` and keeps only joined
    // non-Space children.
    Q_INVOKABLE QVariantList directChildRoomsDetailed(
        const QString &spaceId) const;
    /// The same DIRECT children, as ids only, resolved against a room map the
    /// caller already has.
    ///
    /// directChildRoomsDetailed() materialises the ENTIRE room list and builds
    /// a fresh QHash on every call, so a caller that walks every Space paid
    /// (1 + numSpaces) full room-list materialisations per pass — which is a
    /// real cost on an account switch, where the model rebuilds repeatedly
    /// against a room list the caller has already built once. Same order, same
    /// skip rules (deduped, unknown / Space / non-Joined children dropped);
    /// only the lookup is borrowed.
    QStringList directChildRoomIds(
        const QString &spaceId,
        const QHash<QString, RoomInfo> &byId) const;
    Q_INVOKABLE QVariantList addableRooms(const QString &spaceId,
                                          const QString &filter) const;

    // ---- The Space's PEOPLE ------------------------------------------------
    //
    // "Who is in this Space" is one question with no cheap local answer. A
    // Space's rooms are known; their MEMBERS are not — RoomInfo::members is
    // only populated for rooms whose roster was actually fetched, so deriving
    // this from the child rooms would make the answer depend on which rooms
    // the user happened to open, and asking every child room for its roster
    // is one /members request per room (and CLAUDE.md records that a
    // membership read falls back to a full /state for an idle room).
    //
    // So the roster is the SPACE ROOM's own joined-and-invited membership,
    // which is Element's reading of the same question and costs ONE bounded
    // request per Space per session, fired when the Space is selected. The
    // honest limitation: somebody who is in a room inside the Space but has
    // not joined the Space room itself is not "in the Space" by this rule.

    /// True once a COMPLETE roster for `spaceId` has arrived. False while one
    /// is unfetched, in flight, failed, or truncated by the snapshot cap — a
    /// partial roster is not a smaller truth, it is a different one, and a
    /// caller that filtered on it would hide conversations at random.
    Q_INVOKABLE bool spaceRosterKnown(const QString &spaceId) const;
    /// Whether `userId` is a joined or invited member of the Space room.
    /// Always false for an unknown roster; ask spaceRosterKnown() first.
    Q_INVOKABLE bool spaceHasMember(const QString &spaceId,
                                    const QString &userId) const;
    /// The one place the People scope is decided, so the two layouts cannot
    /// drift: 1 = this DM belongs to the Space's people, 0 = it does not,
    /// -1 = UNKNOWN (no complete roster, or the DM names no peer at all).
    /// The two callers choose their own fail direction for -1, because they
    /// need opposite ones: Classic REMOVES DMs from a list that shows them,
    /// Channels ADDS them to a view that has none.
    int directScope(const QString &spaceId, const QStringList &peerIds) const;
    /// Requests the roster for `spaceId` once per Space per client. A no-op
    /// for a pseudo id, a non-Space, an already-known or in-flight roster, or
    /// a backend that cannot answer.
    void ensureSpaceRoster(const QString &spaceId);
    /// Whether `spaceId` is a real Space room id rather than a pseudo rail
    /// selection ("", "@orphans", "@people"). Every People-scope rule keys on
    /// this: a pseudo selection is a view of everything and scopes nothing.
    static bool isRealSpaceId(const QString &spaceId);
    // Sends the real m.space.child state event through the backend. The
    // authoritative hierarchy update arrives via sync (roomsChanged →
    // rebuild); childAddFinished only reports the send outcome.
    Q_INVOKABLE void addRoomToSpace(const QString &spaceId,
                                    const QString &roomId);
    // MSC1772 removal (empty-via m.space.child); the room itself is never
    // left or deleted. Permission failures surface as ok=false.
    Q_INVOKABLE void removeRoomFromSpace(const QString &spaceId,
                                         const QString &roomId);
    // 2026-08-19: toggles the `suggested` flag on an existing child (via
    // list and order preserved by the backend; a non-child is refused).
    Q_INVOKABLE void setSpaceChildSuggested(const QString &spaceId,
                                            const QString &roomId,
                                            bool suggested);

Q_SIGNALS:
    void activeSpaceIdChanged();
    void spacesChanged();
    // v0.7: outcome of one addRoomToSpace call (send result, not sync).
    void childAddFinished(const QString &spaceId, const QString &roomId,
                          bool ok);
    void childRemoveFinished(const QString &spaceId, const QString &roomId,
                             bool ok);
    void childSuggestedFinished(const QString &spaceId, const QString &roomId,
                                bool suggested, bool ok);
    /// A complete roster for `spaceId` arrived (or was dropped on an account
    /// change). Both room-list models re-filter on it; nothing else should
    /// need it.
    void spaceRosterChanged(const QString &spaceId);

private Q_SLOTS:
    void rebuild();
    /// One member snapshot. Ignores every op this manager did not issue, and
    /// records only a COMPLETE, successful, non-truncated roster.
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);

private:
    /// Forgets every roster and every in-flight request, and announces the
    /// ones that were known so a filter built on them re-opens.
    void dropSpaceRosters();
    struct SpaceEntry {
        RoomInfo info;              // The Space room itself.
        QStringList childRoomIds;   // TRANSITIVE member rooms, ordered.
        // Direct joined child SPACES whose primary parent is this one, in
        // m.space.child order.
        QStringList childSpaceIds;
        int unreadTotal = 0;        // Sum of children's unread counts.
        int highlightTotal = 0;
        // Real depth in the hierarchy: 0 for a root, +1 per level. Was
        // hardcoded to "0 or 1" — a two-level approximation that made a
        // three-deep tree render as a flat pair of indents.
        int level = 0;
        // The primary parent, empty for a root.
        QString parentSpaceId;
    };

    // Assigns level/parentSpaceId/childSpaceIds across `m_spaces`.
    // Cycle-safe and deterministic; see the implementation for the rules.
    void resolveHierarchy(const QHash<QString, RoomInfo> &byId);
    void recomputeOrphans();

    MatrixClient *m_client = nullptr;

    QList<SpaceEntry> m_spaces;             // Rows: [All rooms] [orphans?] [space1] [space2] ...
    QHash<QString, QSet<QString>> m_membership;    // spaceId → set(roomId)
    QSet<QString> m_allRoomIds;             // Every non-space room id.
    // Rooms that are a DIRECT child of at least one joined Space.
    QSet<QString> m_spaceChildRoomIds;
    QSet<QString> m_orphanRoomIds;          // Rooms not in any Space.
    int m_homeUnreadTotal = 0;
    int m_homeHighlightTotal = 0;
    // The Space rosters (see the People block above). `m_spaceMembers` holds
    // only COMPLETE ones — a truncated or failed snapshot is never recorded,
    // so "known" and "usable" are the same fact. `m_rosterRequested` is the
    // once-per-Space-per-client guard; both are cleared with the client.
    QHash<QString, QSet<QString>> m_spaceMembers;
    QSet<QString> m_rosterRequested;
    // v0.7: pending m.space.child sends, opId → (spaceId, roomId).
    QHash<quint64, QPair<QString, QString>> m_pendingChildAdds;
    QHash<quint64, QPair<QString, QString>> m_pendingChildRemovals;
    QHash<quint64, QPair<QString, QString>> m_pendingChildSuggests;

    QString m_activeSpaceId; // Empty means "All rooms".
};
