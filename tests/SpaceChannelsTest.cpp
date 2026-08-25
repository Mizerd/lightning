// The Channels navigation layout: SpaceChannelModel's flat, GLOBAL list of
// Space folders.
//
// What this suite defends, and why each item is a defect that would look like
// a working channel list:
//
//  * IT WORKS AT HOME. The previous design scoped everything to the active
//    Space and produced nothing without one, so the host silently fell back to
//    Classic — a user who chose a navigation layout got the other one. There is
//    no `spaceId` on this model any more, and there is nothing for one to mean.
//  * FLAT BY SPACE. A subspace is a joined Space like any other and gets its
//    own folder; it is never a nested category under its parent, and its rooms
//    are never listed under the parent as well. The parent's folder holds its
//    DIRECT children and nothing else.
//  * NOTHING JOINED IS UNREACHABLE. A room with no joined Space parent is in
//    "Rooms" — DMs included. A room in two Spaces is in both folders, because
//    that is what "this Space contains it" means.
//  * ORDER IS STABLE. Spaces follow the rail's arrangement, rooms follow their
//    Space's `m.space.child` order, and "Rooms" is sorted by name. A channel
//    list that reorders itself when somebody speaks is not a channel list.
//  * A COLLAPSED FOLDER STILL REPORTS WHAT IT HIDES, and collapse survives a
//    rebuild because it is stored, not held in a delegate.
//  * A SEARCH OPENS EVERYTHING and puts it back. Filtering must not mutate
//    what the user collapsed.
#include "models/SpaceChannelModel.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
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

    void announce() { Q_EMIT roomsChanged(); }
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
    // DIRECT children, in the Space's own m.space.child order.
    info.childRoomIds = children;
    info.parentSpaceIds = parents;
    return info;
}

RoomInfo room(const QString &id, const QString &name, int unread = 0,
              int highlight = 0)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = false;
    info.membership = RoomInfo::Joined;
    info.unreadCount = unread;
    info.highlightCount = highlight;
    info.hasUnreadMessages = unread > 0 || highlight > 0;
    return info;
}

RoomInfo dm(const QString &id, const QString &name)
{
    RoomInfo info = room(id, name);
    info.isDirect = true;
    return info;
}

RoomInfo invite(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Invited;
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

int rowOfName(const SpaceChannelModel &model, const QString &name)
{
    return namesOf(model).indexOf(name);
}

} // namespace

class SpaceChannelsTest : public QObject
{
    Q_OBJECT

private:
    /// Two Spaces, one of which is a SUBSPACE of the other, plus a room in no
    /// Space at all and a DM. The subspace is the trap: its rooms must appear
    /// under it and nowhere else, and it must not be nested.
    static QList<RoomInfo> workspace()
    {
        return {
            space(QStringLiteral("!work:x"), QStringLiteral("Work"),
                  { QStringLiteral("!general:x"), QStringLiteral("!random:x"),
                    QStringLiteral("!eng:x") }),
            room(QStringLiteral("!general:x"), QStringLiteral("general")),
            room(QStringLiteral("!random:x"), QStringLiteral("random")),
            space(QStringLiteral("!eng:x"), QStringLiteral("Engineering"),
                  { QStringLiteral("!backend:x"), QStringLiteral("!frontend:x") },
                  { QStringLiteral("!work:x") }),
            room(QStringLiteral("!backend:x"), QStringLiteral("backend"),
                 /*unread=*/4),
            room(QStringLiteral("!frontend:x"), QStringLiteral("frontend"),
                 /*unread=*/0, /*highlight=*/2),
            room(QStringLiteral("!lounge:x"), QStringLiteral("lounge")),
            dm(QStringLiteral("!dm:x"), QStringLiteral("Ada")),
        };
    }

    struct Fixture {
        FakeClient client;
        SpaceManager spaces;
        SettingsManager settings;
        RailLayoutStore layout{ &settings };
        SpaceChannelModel model;

        void build(const QList<RoomInfo> &rooms)
        {
            client.roomList = rooms;
            spaces.setClient(&client);
            model.setSettings(&settings);
            model.setSources(&client, &spaces, &layout);
        }
    };

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("space-channels-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // The whole reason this layout was rebuilt. There is no active Space here
    // and never was one; the column is fully populated anyway.
    void theLayoutIsGlobalAndNeedsNoActiveSpace()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

        const QStringList names = namesOf(f.model);
        QCOMPARE(names.mid(0, 2),
                 QStringList({ QStringLiteral("Lobby"),
                               QStringLiteral("Message Search") }));
        QVERIFY2(names.contains(QStringLiteral("Work")),
                 "a joined Space is missing with no Space selected");
        QVERIFY2(names.contains(QStringLiteral("Engineering")),
                 "a joined SUBSPACE is missing from the flat list");
        QVERIFY(!f.model.empty());
        // And the model has no notion of a selected Space to fall back on.
        QCOMPARE(f.model.property("spaceId").isValid(), false);
    }

    void everyJoinedSpaceIsAFlatFolderAndSubspacesAreNotNested()
    {
        Fixture f;
        f.build(workspace());

        const QStringList names = namesOf(f.model);
        const QStringList kinds = kindsOf(f.model);
        const int work = names.indexOf(QStringLiteral("Work"));
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        QVERIFY(work >= 0);
        QVERIFY(eng >= 0);
        QCOMPARE(kinds.at(work), QStringLiteral("space"));
        QCOMPARE(kinds.at(eng), QStringLiteral("space"));
        // Both folders sit at depth 0: the subspace is a sibling in the
        // column, not a level inside its parent.
        QCOMPARE(f.model.data(f.model.index(work, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);

        // Work holds only its DIRECT rooms. backend/frontend belong to
        // Engineering and appear exactly once, under it.
        QCOMPARE(names.mid(work + 1, 2),
                 QStringList({ QStringLiteral("general"),
                               QStringLiteral("random") }));
        QCOMPARE(names.count(QStringLiteral("backend")), 1);
        QCOMPARE(names.count(QStringLiteral("frontend")), 1);
        QVERIFY(eng > work);
        QCOMPARE(names.mid(eng + 1, 2),
                 QStringList({ QStringLiteral("backend"),
                               QStringLiteral("frontend") }));
        // Rooms inside a folder are indented one step.
        QCOMPARE(f.model.data(f.model.index(eng + 1, 0),
                              SpaceChannelModel::DepthRole).toInt(), 1);
    }

    void roomsWithNoJoinedSpaceParentAreReachableInTheRoomsGroup()
    {
        Fixture f;
        f.build(workspace());

        const QStringList names = namesOf(f.model);
        const int rooms = names.indexOf(QStringLiteral("Rooms"));
        QVERIFY2(rooms >= 0, "there is no Rooms group");
        QCOMPARE(kindsOf(f.model).at(rooms), QStringLiteral("group"));
        // A DM has no Space parent, so it is here — not in a group of its own.
        // Sorted by name, so the list does not reshuffle between syncs.
        QCOMPARE(names.mid(rooms + 1, 2),
                 QStringList({ QStringLiteral("Ada"),
                               QStringLiteral("lounge") }));
        QVERIFY2(!names.contains(QStringLiteral("Direct messages")),
                 "the DM group came back: a DM belongs in Rooms with every "
                 "other unparented room");
        QVERIFY2(!names.contains(QStringLiteral("Favourites")),
                 "the Favourites group came back");
    }

    void aRoomInTwoSpacesAppearsUnderBoth()
    {
        // Matrix permits it, and both Spaces genuinely contain it. Inventing a
        // first-parent-wins rule would make one of them look incomplete.
        Fixture f;
        f.build({
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!shared:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!shared:x") }),
            room(QStringLiteral("!shared:x"), QStringLiteral("shared")),
        });
        const QStringList names = namesOf(f.model);
        QCOMPARE(names.count(QStringLiteral("shared")), 2);
        // ...and it is NOT also in Rooms, because a Space does list it.
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
    }

    void aRoomWhoseOnlySpaceParentIsUnjoinedStaysReachable()
    {
        Fixture f;
        f.build({
            // The parent Space is not joined, so it is not in the room list at
            // all — only the child's own parent pointer names it.
            [] {
                RoomInfo info = room(QStringLiteral("!orphan:x"),
                                     QStringLiteral("orphan"));
                info.parentSpaceIds = { QStringLiteral("!unknown:x") };
                return info;
            }(),
        });
        const QStringList names = namesOf(f.model);
        QVERIFY2(names.contains(QStringLiteral("orphan")),
                 "a room whose only Space parent is unjoined has no folder to "
                 "appear in and vanished from the column");
        QVERIFY(names.contains(QStringLiteral("Rooms")));
    }

    void invitesAreOfferedRatherThanLostToThisLayout()
    {
        Fixture f;
        f.build({
            room(QStringLiteral("!lounge:x"), QStringLiteral("lounge")),
            invite(QStringLiteral("!invited:x"), QStringLiteral("Newcomers")),
        });
        const QStringList names = namesOf(f.model);
        const int invites = names.indexOf(QStringLiteral("Invites"));
        QVERIFY2(invites >= 0,
                 "an invite is unreachable in this layout: Classic is not a "
                 "fallback any more, so this column is the whole navigation");
        QCOMPARE(names.at(invites + 1), QStringLiteral("Newcomers"));
        const int row = rowOfName(f.model, QStringLiteral("Newcomers"));
        QVERIFY(f.model.data(f.model.index(row, 0),
                             SpaceChannelModel::IsInviteRole).toBool());
        // An invite is an action waiting on the user, so it always reads as
        // unread whatever its counters say — it has none of its own.
        QVERIFY2(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::HasUnreadRole).toBool(),
                 "an invite reads as a quiet read row");
        // Invites come before the ordinary rooms.
        QVERIFY(invites < names.indexOf(QStringLiteral("Rooms")));

        // ...and it survives every filter chip, exactly as in Classic: it
        // needs action regardless of the view, and pressing People or Rooms is
        // not a request to hide one.
        for (int mode = 0; mode <= 3; ++mode) {
            f.model.setFilterMode(mode);
            QVERIFY2(namesOf(f.model).contains(QStringLiteral("Newcomers")),
                     qPrintable(QStringLiteral("filter %1 hid an invite")
                                    .arg(mode)));
        }
        f.model.setFilterMode(0);

        // A collapsed Invites group still says something is waiting, even
        // though an invite carries no unread count to sum.
        f.model.toggleCollapsed(SpaceChannelModel::invitesGroupId());
        const int header = rowOfName(f.model, QStringLiteral("Invites"));
        QVERIFY(header >= 0);
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Newcomers")));
        QVERIFY2(f.model.data(f.model.index(header, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt() > 0,
                 "collapsing the Invites group hid the fact that an invite is "
                 "waiting");
    }

    void spaceOrderFollowsTheRailArrangement()
    {
        Fixture f;
        f.client.roomList = {
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!ra:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!rb:x") }),
            space(QStringLiteral("!c:x"), QStringLiteral("Gamma"),
                  { QStringLiteral("!rc:x") }),
            room(QStringLiteral("!ra:x"), QStringLiteral("ra")),
            room(QStringLiteral("!rb:x"), QStringLiteral("rb")),
            room(QStringLiteral("!rc:x"), QStringLiteral("rc")),
        };
        f.spaces.setClient(&f.client);
        f.model.setSettings(&f.settings);
        // The user's rail order, with one Space inside a folder.
        const QString folder = f.layout.createFolder(QStringLiteral("Work"));
        f.layout.setSpaceFolder(QStringLiteral("!c:x"), folder);
        f.layout.setTopLevelOrder({ QStringLiteral("!b:x"), folder,
                                    QStringLiteral("!a:x") });
        f.model.setSources(&f.client, &f.spaces, &f.layout);

        QStringList spaceNames;
        for (const QString &name : namesOf(f.model)) {
            if (name == QLatin1String("Alpha") || name == QLatin1String("Beta")
                || name == QLatin1String("Gamma")) {
                spaceNames.append(name);
            }
        }
        QCOMPARE(spaceNames, QStringList({ QStringLiteral("Beta"),
                                           QStringLiteral("Gamma"),
                                           QStringLiteral("Alpha") }));
    }

    // Clicking a Space in the rail NARROWS the column to it. Without this it
    // showed every Space whatever you clicked, so picking one did nothing
    // visible — reported as "clicking a space basically does nothing".
    void selectingASpaceNarrowsTheColumnToItAndItsSubspaces()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));

        f.model.setScopeSpaceId(QStringLiteral("!work:x"));
        const QStringList names = namesOf(f.model);
        // The Space and its subspace, and nothing about the rest of the
        // account: the two account-wide groups are statements about the whole
        // account and repeating them under a Space is the complaint.
        QVERIFY(names.contains(QStringLiteral("Work")));
        QVERIFY2(names.contains(QStringLiteral("Engineering")),
                 "a subspace of the selected Space is missing");
        QVERIFY2(!names.contains(QStringLiteral("Rooms")),
                 "the account-wide Rooms group survived the scope");
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM that belongs to no Space survived the scope");
        QVERIFY(names.contains(QStringLiteral("general")));
        QVERIFY(names.contains(QStringLiteral("backend")));
        // A subspace is still a FLAT folder, not a level: this is a narrower
        // view of the same layout, not the old nested one.
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        // Lobby is still one row away, which is what makes the scope escapable.
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
        // ...and an empty account is still not claimed.
        QVERIFY(!f.model.empty());

        // Lobby clears it.
        f.model.setScopeSpaceId(QString());
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));
    }

    void aPseudoRailRowScopesNothing()
    {
        // "" is Home and "@orphans" is "Other rooms". Neither is a Space, and
        // scoping to one would empty the column.
        Fixture f;
        f.build(workspace());
        const int all = f.model.rowCount();
        f.model.setScopeSpaceId(QStringLiteral("@orphans"));
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.rowCount(), all);
        f.model.setScopeSpaceId(QString());
        QCOMPARE(f.model.rowCount(), all);
    }

    void aScopeOnASpaceTheAccountNoLongerHasFallsBackToEverything()
    {
        // Left the Space while it was selected. An empty column would look
        // like the account had nothing in it.
        Fixture f;
        f.build(workspace());
        f.model.setScopeSpaceId(QStringLiteral("!gone:x"));
        const QStringList names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Work")));
        QVERIFY(names.contains(QStringLiteral("Rooms")));
    }

    void aCollapsedFolderHidesItsRoomsButNotItsActivity()
    {
        Fixture f;
        f.build(workspace());

        QVERIFY(namesOf(f.model).contains(QStringLiteral("backend")));
        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        QVERIFY(f.model.isCollapsed(QStringLiteral("!eng:x")));

        const QStringList names = namesOf(f.model);
        QVERIFY2(!names.contains(QStringLiteral("backend")),
                 "collapsing did not hide the rooms");
        QVERIFY(names.contains(QStringLiteral("Engineering")));
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        // 4 unread in backend, 2 mentions in frontend.
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 4);
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::HiddenHighlightRole).toInt(),
                 2);
        QVERIFY(f.model.data(f.model.index(eng, 0),
                             SpaceChannelModel::CollapsedRole).toBool());

        // Expanded again, the header reports nothing: the rows carry their own
        // badges and a total on top would double-count what is visible.
        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        const int engOpen = rowOfName(f.model, QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(engOpen, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 0);
    }

    void collapseStateSurvivesARebuildAndAReload()
    {
        // The old implementation kept collapse in memory keyed by the active
        // Space. With no active Space and rows rebuilt on every arriving
        // message, in-memory-only state is state the user loses constantly.
        Fixture f;
        f.build(workspace());
        f.model.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("general")));

        // A room update rebuilds the rows.
        f.client.roomList[1].unreadCount = 3;
        f.client.announce();
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("general")),
                 "the collapse was lost on a rebuild");

        // And a fresh model over the same settings still knows.
        SpaceChannelModel reopened;
        reopened.setSettings(&f.settings);
        reopened.setSources(&f.client, &f.spaces, &f.layout);
        QVERIFY(reopened.isCollapsed(QStringLiteral("!work:x")));
        QVERIFY(!namesOf(reopened).contains(QStringLiteral("general")));
    }

    void aSearchOpensEveryFolderAndPutsThemBack()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("backend")));

        f.model.setSearchQuery(QStringLiteral("back"));
        const QStringList found = namesOf(f.model);
        QVERIFY2(found.contains(QStringLiteral("backend")),
                 "a room inside a collapsed folder is not findable");
        QVERIFY2(found.contains(QStringLiteral("Engineering")),
                 "the match lost the folder that gives it context");
        QVERIFY2(!found.contains(QStringLiteral("general")),
                 "a non-matching room survived the search");
        // Navigation rows step aside while searching: they match nothing and
        // would be two dead entries above the results.
        QVERIFY(!found.contains(QStringLiteral("Lobby")));
        QVERIFY(!found.contains(QStringLiteral("Message Search")));
        // A folder with no match at all is dropped entirely.
        QVERIFY(!found.contains(QStringLiteral("Rooms")));

        // Clearing restores exactly what was collapsed before: filtering must
        // not mutate the collapse state.
        f.model.setSearchQuery(QString());
        QVERIFY(f.model.isCollapsed(QStringLiteral("!eng:x")));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("backend")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));
    }

    void theFilterChipsSelectRoomsWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        f.build(workspace());

        f.model.setFilterMode(1);   // People
        const QStringList people = namesOf(f.model);
        QVERIFY(people.contains(QStringLiteral("Ada")));
        QVERIFY(!people.contains(QStringLiteral("general")));
        // A filter that matched little is NOT an empty account, and the empty
        // state must not claim it is.
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        f.model.setFilterMode(2);   // Rooms
        const QStringList rooms = namesOf(f.model);
        QVERIFY(!rooms.contains(QStringLiteral("Ada")));
        QVERIFY(rooms.contains(QStringLiteral("general")));

        f.model.setFilterMode(3);   // Unreads
        const QStringList unread = namesOf(f.model);
        QVERIFY(unread.contains(QStringLiteral("backend")));
        QVERIFY(!unread.contains(QStringLiteral("general")));
    }

    void theMessageSearchRowIsAbsentWhenTheServerCannotSearch()
    {
        Fixture f;
        f.build(workspace());
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("Message Search")),
                 "a dead search row is offered on a server that cannot search");
        f.model.setMessageSearchSupported(true);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Message Search")));
    }

    void navigationRowsCarryNoRoomIdAndCannotBeOpenedAsRooms()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        for (int i = 0; i < 2; ++i) {
            const QString kind =
                f.model.data(f.model.index(i, 0),
                             SpaceChannelModel::KindRole).toString();
            QVERIFY(kind == QLatin1String("lobby")
                    || kind == QLatin1String("search"));
            QVERIFY2(f.model.data(f.model.index(i, 0),
                                  SpaceChannelModel::RoomIdRole)
                         .toString().isEmpty(),
                     "a navigation row carries a room id, so something will "
                     "eventually try to open it");
        }
        // A group's synthetic id can never collide with a room id.
        QVERIFY(SpaceChannelModel::roomsGroupId().startsWith(QLatin1Char('@')));
        QVERIFY(SpaceChannelModel::invitesGroupId()
                    .startsWith(QLatin1Char('@')));
    }

    void rowForRoomNeverMatchesAFolderOrAGroup()
    {
        Fixture f;
        f.build(workspace());
        QVERIFY(f.model.rowForRoom(QStringLiteral("!general:x")) >= 0);
        // Highlighting a folder as "the room you are in" would mark the whole
        // group.
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!work:x")), -1);
        QCOMPARE(f.model.rowForRoom(SpaceChannelModel::roomsGroupId()), -1);
        QCOMPARE(f.model.rowForRoom(QString()), -1);
    }

    void unreadStateChangesInPlaceRatherThanResettingTheWholeColumn()
    {
        // A reset tears down and rebuilds every delegate — and its avatar
        // fetch — on every arriving message, which for a column this long is
        // visible. The rows did not move, so this must be a dataChanged.
        Fixture f;
        f.build(workspace());
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&f.model, &QAbstractItemModel::dataChanged);

        f.client.roomList[1].unreadCount = 7;
        f.client.roomList[1].hasUnreadMessages = true;
        f.client.announce();

        QCOMPARE(resets.count(), 0);
        QVERIFY(changes.count() >= 1);
        const int row = rowOfName(f.model, QStringLiteral("general"));
        QCOMPARE(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::UnreadCountRole).toInt(), 7);
    }

    void anEncryptedRoomOnlyClaimsEncryptionItKnowsAbout()
    {
        Fixture f;
        QList<RoomInfo> rooms = {
            space(QStringLiteral("!s:x"), QStringLiteral("Space"),
                  { QStringLiteral("!known:x"), QStringLiteral("!unknown:x") }),
            room(QStringLiteral("!known:x"), QStringLiteral("known")),
            room(QStringLiteral("!unknown:x"), QStringLiteral("unknown")),
        };
        rooms[1].encrypted = true;
        rooms[1].encryptionKnown = true;
        rooms[2].encrypted = true;
        rooms[2].encryptionKnown = false;
        f.build(rooms);

        const int known = rowOfName(f.model, QStringLiteral("known"));
        const int unknown = rowOfName(f.model, QStringLiteral("unknown"));
        QVERIFY(f.model.data(f.model.index(known, 0),
                             SpaceChannelModel::EncryptedRole).toBool());
        QVERIFY2(!f.model.data(f.model.index(unknown, 0),
                               SpaceChannelModel::EncryptedRole).toBool(),
                 "the lock glyph is a claim: 'not established yet' must not "
                 "be drawn as encrypted");
    }

    void anEmptyAccountSaysSoAndAnEmptyFilterDoesNot()
    {
        Fixture f;
        f.build({});
        QVERIFY(f.model.empty());
        QCOMPARE(f.model.rowCount(), 1);   // Lobby only
        QCOMPARE(kindsOf(f.model), QStringList{ QStringLiteral("lobby") });
    }

    void anAccountChangeMakesTheCollapseStateBeReReadNotKept()
    {
        // The collapse set is ACCOUNT-SCOPED storage (the same per-account
        // appearance path every other Appearance choice uses), so a sign-out
        // or a switch must drop the in-memory copy and read whoever is next —
        // keeping it would apply one account's collapsed folders to another
        // account's rooms.
        //
        // Proven by moving the STORED value out from under the model and then
        // announcing the account change: if the cache were kept, the stale
        // answer would survive.
        Fixture f;
        f.build(workspace());
        f.model.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        SpaceChannelModel other;
        other.setSettings(&f.settings);
        other.setSources(&f.client, &f.spaces, &f.layout);
        other.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(!other.isCollapsed(QStringLiteral("!work:x")));
        // The first model has not noticed: it is still on its own cache.
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        f.client.logout();
        QVERIFY2(!f.model.isCollapsed(QStringLiteral("!work:x")),
                 "the in-memory collapse set survived an account change, so "
                 "the previous account's collapsed folders describe the next "
                 "account's rooms");
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
