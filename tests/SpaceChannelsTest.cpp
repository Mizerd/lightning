// 2026-08-23 navigation layouts: SpaceChannelModel's flattening of one
// Space's DIRECT hierarchy.
//
// What this suite is really defending — every item is a defect that would
// look like a working channel list:
//
//  * DIRECT, not transitive. SpaceManager::rebuild() deliberately flattens a
//    subspace's rooms into every ancestor's membership, so the pre-existing
//    childRoomsDetailed() returns the whole tree. Building on it showed every
//    room twice: once under the top-level Space and again under its own
//    category. The model must read the Space's OWN m.space.child order.
//  * HIERARCHY order, never activity order. A channel list whose rows move
//    when someone speaks is not a channel list; a member learns where things
//    are and they stay there. Unread changes a row's WEIGHT, not its place.
//  * A collapsed category must still report the activity it is hiding.
//    Collapsing to save space must not silently mute a group.
//  * Unjoined children are absent, not placeholder rows. Space Home is where
//    a join is offered.
//  * Collapse state is per SPACE, so collapsing "General" in one workspace
//    does not collapse a same-named category in another.
#include "models/SpaceChannelModel.h"

#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<RoomInfo> roomList;
    QList<TimelineEvent> timeline(const QString &) const override
    { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &,
                   const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    quint64 setSpaceChildSuggested(const QString &spaceId,
                                   const QString &roomId,
                                   bool suggested) override
    {
        lastSpaceId = spaceId;
        lastRoomId = roomId;
        lastSuggested = suggested;
        if (refuse)
            return 0;
        return ++opCounter;
    }
    void finishSuggested(quint64 opId, const QString &spaceId,
                         const QString &roomId, bool suggested, bool ok)
    {
        Q_EMIT spaceChildSuggestedFinished(opId, spaceId, roomId, suggested,
                                           ok);
    }

    QString lastSpaceId;
    QString lastRoomId;
    bool lastSuggested = false;
    bool refuse = false;
    quint64 opCounter = 0;
};
RoomInfo space(const QString &id, const QString &name,
               const QStringList &children,
               const QStringList &parents = {})
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = true;
    info.membership = RoomInfo::Joined;
    info.childRoomIds = children;
    info.parentSpaceIds = parents;
    return info;
}

RoomInfo channel(const QString &id, const QString &name,
                 const QStringList &parents, int unread = 0,
                 int highlight = 0)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = false;
    info.membership = RoomInfo::Joined;
    info.parentSpaceIds = parents;
    info.unreadCount = unread;
    info.highlightCount = highlight;
    info.hasUnreadMessages = unread > 0 || highlight > 0;
    return info;
}

QStringList namesOf(const SpaceChannelModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i, 0),
                              SpaceChannelModel::NameRole).toString());
    }
    return out;
}

QStringList kindsOf(const SpaceChannelModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i, 0),
                              SpaceChannelModel::KindRole).toString());
    }
    return out;
}

} // namespace

class SpaceChannelsTest : public QObject
{
    Q_OBJECT

private:
    /// A Space with two uncategorised channels and one subspace holding two
    /// more. The subspace's rooms are ALSO reachable transitively from the
    /// parent, which is exactly the trap.
    static QList<RoomInfo> workspace()
    {
        return {
            space(QStringLiteral("!work:x"), QStringLiteral("Work"),
                  { QStringLiteral("!general:x"), QStringLiteral("!random:x"),
                    QStringLiteral("!eng:x") }),
            channel(QStringLiteral("!general:x"), QStringLiteral("general"),
                    { QStringLiteral("!work:x") }),
            channel(QStringLiteral("!random:x"), QStringLiteral("random"),
                    { QStringLiteral("!work:x") }),
            space(QStringLiteral("!eng:x"), QStringLiteral("Engineering"),
                  { QStringLiteral("!backend:x"), QStringLiteral("!frontend:x") },
                  { QStringLiteral("!work:x") }),
            channel(QStringLiteral("!backend:x"), QStringLiteral("backend"),
                    { QStringLiteral("!eng:x") }, /*unread=*/4),
            channel(QStringLiteral("!frontend:x"), QStringLiteral("frontend"),
                    { QStringLiteral("!eng:x") }, /*unread=*/0,
                    /*highlight=*/2),
        };
    }

private slots:
    void withNoSpaceTheModelIsEmpty()
    {
        // Home. The model shows NOTHING rather than falling back to every
        // room: the host falls back to Classic there, and a silent fallback
        // here would make the Channels layout look like it works at Home.
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);

        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        QCOMPARE(model.rowCount(), 0);
        // ...and that is NOT "this space has no channels".
        QVERIFY(!model.emptyHierarchy());
    }

    void directChildrenOnlyNeverTheTransitiveTree()
    {
        // THE defect this model exists to avoid. SpaceManager's own
        // membership is transitive, so a naive implementation lists backend
        // and frontend at the top level AND under Engineering.
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);

        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(namesOf(model),
                 QStringList({ QStringLiteral("general"),
                               QStringLiteral("random"),
                               QStringLiteral("Engineering"),
                               QStringLiteral("backend"),
                               QStringLiteral("frontend") }));
        // Each subspace room appears exactly ONCE.
        QCOMPARE(namesOf(model).count(QStringLiteral("backend")), 1);
        QCOMPARE(namesOf(model).count(QStringLiteral("frontend")), 1);
    }

    void categoriesAreHeadersAndChannelsAreRows()
    {
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(kindsOf(model),
                 QStringList({ QStringLiteral("channel"),
                               QStringLiteral("channel"),
                               QStringLiteral("category"),
                               QStringLiteral("channel"),
                               QStringLiteral("channel") }));
        // Indentation: a channel inside a category sits one level in, the
        // category itself does not.
        QCOMPARE(model.data(model.index(0, 0),
                            SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(model.data(model.index(2, 0),
                            SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(model.data(model.index(3, 0),
                            SpaceChannelModel::DepthRole).toInt(), 1);
    }

    void uncategorisedChannelsComeBeforeTheCategories()
    {
        // Element and Sable both do this, and the reason is concrete: with
        // categories first, a Space whose only uncategorised channel is at
        // the bottom looks empty until you scroll.
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        const QStringList kinds = kindsOf(model);
        const int firstCategory = kinds.indexOf(QStringLiteral("category"));
        QVERIFY(firstCategory > 0);
        for (int i = 0; i < firstCategory; ++i)
            QCOMPARE(kinds.at(i), QStringLiteral("channel"));
    }

    void orderIsTheHierarchysNotActivitys()
    {
        // `random` has no unread and `general` none either; give `random` a
        // pile of activity and it must NOT move. This is the property that
        // makes a channel list learnable.
        FakeClient client;
        auto rooms = workspace();
        for (RoomInfo &room : rooms) {
            if (room.id == QStringLiteral("!random:x")) {
                room.unreadCount = 99;
                room.hasUnreadMessages = true;
                room.lastActivity = QDateTime::currentDateTime();
            }
        }
        client.roomList = rooms;
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(namesOf(model).first(), QStringLiteral("general"));
        QCOMPARE(namesOf(model).at(1), QStringLiteral("random"));
        // The activity IS reported, just not by moving the row.
        QCOMPARE(model.data(model.index(1, 0),
                            SpaceChannelModel::UnreadCountRole).toInt(), 99);
    }

    void collapsingACategoryHidesItsChannelsButNotItsActivity()
    {
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));
        QCOMPARE(model.rowCount(), 5);

        model.toggleCategory(QStringLiteral("!eng:x"));
        QCOMPARE(namesOf(model),
                 QStringList({ QStringLiteral("general"),
                               QStringLiteral("random"),
                               QStringLiteral("Engineering") }));
        QVERIFY(model.categoryCollapsed(QStringLiteral("!eng:x")));

        // The header now carries what it is hiding. Collapsing to save space
        // must not silently mute the group.
        const QModelIndex header = model.index(2, 0);
        QCOMPARE(model.data(header, SpaceChannelModel::CollapsedRole).toBool(),
                 true);
        QCOMPARE(model.data(header,
                            SpaceChannelModel::HiddenUnreadRole).toInt(), 4);
        QCOMPARE(model.data(header,
                            SpaceChannelModel::HiddenHighlightRole).toInt(), 2);

        model.toggleCategory(QStringLiteral("!eng:x"));
        QCOMPARE(model.rowCount(), 5);
        // Expanded, the rows speak for themselves — a header total on top of
        // visible badges would double-count what the user can already see.
        QCOMPARE(model.data(model.index(2, 0),
                            SpaceChannelModel::HiddenUnreadRole).toInt(), 0);
    }

    void collapseStateIsScopedToItsSpace()
    {
        FakeClient client;
        auto rooms = workspace();
        // A second workspace with a subspace of the SAME id shape but a
        // different id, so a per-name or global collapse set would leak.
        rooms.append(space(QStringLiteral("!home:x"), QStringLiteral("Home"),
                           { QStringLiteral("!eng2:x") }));
        rooms.append(space(QStringLiteral("!eng2:x"),
                           QStringLiteral("Engineering"),
                           { QStringLiteral("!ops:x") },
                           { QStringLiteral("!home:x") }));
        rooms.append(channel(QStringLiteral("!ops:x"), QStringLiteral("ops"),
                             { QStringLiteral("!eng2:x") }));
        client.roomList = rooms;
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);

        model.setSpaceId(QStringLiteral("!work:x"));
        model.toggleCategory(QStringLiteral("!eng:x"));
        QVERIFY(model.categoryCollapsed(QStringLiteral("!eng:x")));

        model.setSpaceId(QStringLiteral("!home:x"));
        // Its own category is untouched, and coming back preserves the first
        // Space's choice.
        QVERIFY(!model.categoryCollapsed(QStringLiteral("!eng2:x")));
        QCOMPARE(model.rowCount(), 2); // Engineering + ops
        model.setSpaceId(QStringLiteral("!work:x"));
        QVERIFY(model.categoryCollapsed(QStringLiteral("!eng:x")));
    }

    void unjoinedAndSpaceChildrenAreNotChannels()
    {
        FakeClient client;
        auto rooms = workspace();
        // An invited child and a child the account does not know about at
        // all. Both are absent — Space Home is where a join is offered, and
        // a placeholder row here would be a channel that cannot be opened.
        rooms[0].childRoomIds.append(QStringLiteral("!invited:x"));
        rooms[0].childRoomIds.append(QStringLiteral("!unknown:x"));
        RoomInfo invited =
            channel(QStringLiteral("!invited:x"), QStringLiteral("invited"),
                    { QStringLiteral("!work:x") });
        invited.membership = RoomInfo::Invited;
        rooms.append(invited);
        client.roomList = rooms;

        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QVERIFY(!namesOf(model).contains(QStringLiteral("invited")));
        QCOMPARE(model.rowCount(), 5);
    }

    void anEmptySpaceReportsAnEmptyHierarchy()
    {
        // Distinct from "no Space selected": the empty states differ, and
        // conflating them tells the user the wrong thing about what to do.
        FakeClient client;
        client.roomList = { space(QStringLiteral("!bare:x"),
                                  QStringLiteral("Bare"), {}) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!bare:x"));

        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.emptyHierarchy());
    }

    void rowForRoomFindsChannelsAndNeverCategories()
    {
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(model.rowForRoom(QStringLiteral("!backend:x")), 3);
        // A category is a Space and never the active room in the timeline
        // sense; highlighting it would mark a whole group as "where you are".
        QCOMPARE(model.rowForRoom(QStringLiteral("!eng:x")), -1);
        QCOMPARE(model.rowForRoom(QStringLiteral("!nope:x")), -1);
        QCOMPARE(model.rowForRoom(QString()), -1);
    }

    void switchingSpaceRebuildsAndAnnounces()
    {
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);

        QSignalSpy counts(&model, &SpaceChannelModel::countChanged);
        QSignalSpy ids(&model, &SpaceChannelModel::spaceIdChanged);
        model.setSpaceId(QStringLiteral("!work:x"));
        QCOMPARE(ids.count(), 1);
        QVERIFY(counts.count() >= 1);
        QCOMPARE(model.rowCount(), 5);

        // Re-setting the SAME id must not churn: this is bound to the rail's
        // active Space, which re-announces on every account event.
        const int before = ids.count();
        model.setSpaceId(QStringLiteral("!work:x"));
        QCOMPARE(ids.count(), before);
    }

    void aSubspaceCanBeOpenedAsItsOwnRoot()
    {
        // Depth stops at one, so a deeper subspace is reached by OPENING the
        // category — which re-roots this model at it.
        FakeClient client;
        auto rooms = workspace();
        rooms[3].childRoomIds.append(QStringLiteral("!deep:x"));
        rooms.append(space(QStringLiteral("!deep:x"), QStringLiteral("Deep"),
                           { QStringLiteral("!nested:x") },
                           { QStringLiteral("!eng:x") }));
        rooms.append(channel(QStringLiteral("!nested:x"),
                             QStringLiteral("nested"),
                             { QStringLiteral("!deep:x") }));
        client.roomList = rooms;
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);

        model.setSpaceId(QStringLiteral("!work:x"));
        // `nested` is two levels down, so it is NOT flattened in.
        QVERIFY(!namesOf(model).contains(QStringLiteral("nested")));

        model.setSpaceId(QStringLiteral("!eng:x"));
        QCOMPARE(namesOf(model),
                 QStringList({ QStringLiteral("backend"),
                               QStringLiteral("frontend"),
                               QStringLiteral("Deep"),
                               QStringLiteral("nested") }));
    }

    void favouritesAndDirectMessagesAppearAboveTheHierarchy()
    {
        // Reported: "in channels mode all list doesn't show people and in
        // people list doesn't show people, also make sure favourites work in
        // channels mode too."
        //
        // A Space hierarchy cannot contain a DM or a favourite — those belong
        // to the ACCOUNT — so the Channels layout could not reach a direct
        // message at all and the People chip had nothing to show.
        FakeClient client;
        auto rooms = workspace();
        RoomInfo dm = channel(QStringLiteral("!dm:x"), QStringLiteral("alice"),
                              {});
        dm.isDirect = true;
        rooms.append(dm);
        RoomInfo fav = channel(QStringLiteral("!fav:x"),
                               QStringLiteral("favourite room"), {});
        fav.isFavourite = true;
        rooms.append(fav);
        client.roomList = rooms;

        SpaceManager spaces;
        spaces.setClient(&client);
        RoomListModel list;
        list.setClient(&client);

        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setRoomListModel(&list);
        model.setSpaceId(QStringLiteral("!work:x"));

        const QStringList names = namesOf(model);
        QVERIFY2(names.contains(QStringLiteral("alice")),
                 qPrintable(QStringLiteral("no DM in: ")
                                + names.join(QLatin1Char(','))));
        QVERIFY(names.contains(QStringLiteral("favourite room")));
        // ...and the Space's own channels are still there.
        QVERIFY(names.contains(QStringLiteral("general")));
        QVERIFY(names.contains(QStringLiteral("Engineering")));

        // Group labels, and the account groups come FIRST — the Classic order
        // is favourites then DMs then rooms, and switching layout must not
        // rearrange what the user already knows.
        const QStringList kinds = kindsOf(model);
        QVERIFY(kinds.contains(QStringLiteral("section")));
        QCOMPARE(names.indexOf(QStringLiteral("favourite room")) <
                     names.indexOf(QStringLiteral("alice")), true);
        QCOMPARE(names.indexOf(QStringLiteral("alice")) <
                     names.indexOf(QStringLiteral("general")), true);
    }

    void aFavouritedDmIsListedOnceNotTwice()
    {
        FakeClient client;
        auto rooms = workspace();
        RoomInfo dm = channel(QStringLiteral("!dm:x"), QStringLiteral("alice"),
                              {});
        dm.isDirect = true;
        dm.isFavourite = true;
        rooms.append(dm);
        client.roomList = rooms;

        SpaceManager spaces;
        spaces.setClient(&client);
        RoomListModel list;
        list.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setRoomListModel(&list);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(namesOf(model).count(QStringLiteral("alice")), 1);
    }

    void theFilterChipsSelectWhichGroupsAppear()
    {
        FakeClient client;
        auto rooms = workspace();
        RoomInfo dm = channel(QStringLiteral("!dm:x"), QStringLiteral("alice"),
                              {});
        dm.isDirect = true;
        rooms.append(dm);
        client.roomList = rooms;

        SpaceManager spaces;
        spaces.setClient(&client);
        RoomListModel list;
        list.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setRoomListModel(&list);
        model.setSpaceId(QStringLiteral("!work:x"));

        // People: DMs only. A channel list is not people, so the hierarchy is
        // skipped entirely rather than left standing over nothing.
        model.setFilterMode(1);
        QVERIFY(namesOf(model).contains(QStringLiteral("alice")));
        QVERIFY(!namesOf(model).contains(QStringLiteral("general")));
        QVERIFY(!namesOf(model).contains(QStringLiteral("Engineering")));

        // Rooms: the hierarchy only.
        model.setFilterMode(2);
        QVERIFY(!namesOf(model).contains(QStringLiteral("alice")));
        QVERIFY(namesOf(model).contains(QStringLiteral("general")));

        // Unreads: only rows with something unread. `backend` has 4 unread
        // and `frontend` 2 mentions in the fixture; `general` has none.
        model.setFilterMode(3);
        const QStringList unread = namesOf(model);
        QVERIFY(unread.contains(QStringLiteral("backend")));
        QVERIFY(unread.contains(QStringLiteral("frontend")));
        QVERIFY(!unread.contains(QStringLiteral("general")));

        // All: everything back.
        model.setFilterMode(0);
        QVERIFY(namesOf(model).contains(QStringLiteral("alice")));
        QVERIFY(namesOf(model).contains(QStringLiteral("general")));

        // Out of range clamps to All rather than emptying the list.
        model.setFilterMode(99);
        QCOMPARE(model.filterMode(), 0);
    }

    void anEmptyGroupDropsItsOwnHeader()
    {
        // A "Favourites" label over nothing is worse than no label.
        FakeClient client;
        client.roomList = workspace();   // no DMs, no favourites
        SpaceManager spaces;
        spaces.setClient(&client);
        RoomListModel list;
        list.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setRoomListModel(&list);
        model.setSpaceId(QStringLiteral("!work:x"));

        QVERIFY(!namesOf(model).contains(QStringLiteral("Favourites")));
        QVERIFY(!namesOf(model).contains(QStringLiteral("Direct messages")));
        // With nothing above it, the hierarchy needs no "Channels" label
        // either — it is the whole list.
        QVERIFY(!namesOf(model).contains(QStringLiteral("Channels")));
        QVERIFY(!kindsOf(model).contains(QStringLiteral("section")));
    }

    void aFilterThatMatchesNothingIsNotAnEmptySpace()
    {
        // "This space has no channels yet" is a fact about the SPACE. A
        // filter that matched nothing is a fact about the filter, and saying
        // the first sends the user looking for a problem that is not there.
        FakeClient client;
        client.roomList = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        RoomListModel list;
        list.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setRoomListModel(&list);
        model.setSpaceId(QStringLiteral("!work:x"));

        model.setFilterMode(1);   // People, and there are none
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.emptyHierarchy());
    }

    void encryptionIsOnlyReportedWhenItIsKnown()
    {
        // The channel row draws a LOCK for this. A lock on a room whose
        // state has not resolved would claim encryption as a fact.
        FakeClient client;
        auto rooms = workspace();
        for (RoomInfo &room : rooms) {
            if (room.id == QStringLiteral("!general:x")) {
                room.encrypted = true;
                room.encryptionKnown = true;
            } else if (room.id == QStringLiteral("!random:x")) {
                room.encrypted = true;
                room.encryptionKnown = false; // not established yet
            }
        }
        client.roomList = rooms;
        SpaceManager spaces;
        spaces.setClient(&client);
        SpaceChannelModel model;
        model.setSpaceManager(&spaces);
        model.setSpaceId(QStringLiteral("!work:x"));

        QCOMPARE(model.data(model.index(0, 0),
                            SpaceChannelModel::EncryptedRole).toBool(), true);
        QCOMPARE(model.data(model.index(1, 0),
                            SpaceChannelModel::EncryptedRole).toBool(), false);
    }
};

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
