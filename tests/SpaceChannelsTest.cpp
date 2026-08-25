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
//    "Rooms". A room in two Spaces is in both folders, because that is what
//    "this Space contains it" means.
//  * A DIRECT MESSAGE IS NEVER SCOPED BY A SPACE, and it has a group of its
//    own. Matrix gives no way for a DM to be a Space's child, so a scope that
//    dropped DMs dropped them everywhere — and the People chip, whose entire
//    result set is DMs, then produced a column of two navigation rows over
//    blank space. Reported as "in channels mode people tab does nothing".
//    Classic reached the same conclusion first (RoomListModel).
//  * A FILTER MISS IS NOT AN EMPTY ACCOUNT. `empty` answers one question only;
//    `matchCount` answers the other, so the column can say which of the two it
//    is instead of rendering silence.
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
    QList<RoomInfo> rooms() const override
    {
        ++roomsCalls;
        return roomList;
    }
    QList<RoomInfo> roomList;
    mutable int roomsCalls = 0;

    // The base class returns 0 here, which DirectAvatarResolver reads as "the
    // backend refused" — it then skips its own pending bookkeeping entirely,
    // so the resolver's most important failure mode was structurally
    // unreachable in this harness. Returning a real op id is what lets a test
    // see the profile fan-out at all.
    quint64 fetchUserProfile(const QString &userId) override
    {
        profileFetches.append(userId);
        return ++nextOp;
    }
    QStringList profileFetches;
    quint64 nextOp = 0;
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
        // Sorted by name, so the list does not reshuffle between syncs.
        QCOMPARE(names.mid(rooms + 1, 1), QStringList{ QStringLiteral("lounge") });
        QVERIFY2(!names.contains(QStringLiteral("Favourites")),
                 "the Favourites group came back");
    }

    // A DM used to live in "Rooms" with every other unparented room, on the
    // reasoning that a DM has no Space parent so it belongs there. It reads as
    // a mislabel the moment the People chip is the reason a row is on screen:
    // a column of nothing but people, under a heading that says "Rooms", looks
    // like the chip did not take. On the unfixed tree the DM group does not
    // exist, so both halves of this fail: "Direct messages" is absent and
    // "Ada" is found under "Rooms".
    void directMessagesAreTheirOwnGroupRatherThanRowsUnderRooms()
    {
        Fixture f;
        f.build(workspace());

        const QStringList names = namesOf(f.model);
        const int directs = names.indexOf(QStringLiteral("Direct messages"));
        QVERIFY2(directs >= 0,
                 "there is no Direct messages group, so a DM is filed under a "
                 "heading that says Rooms");
        QCOMPARE(kindsOf(f.model).at(directs), QStringLiteral("group"));
        QCOMPARE(names.mid(directs + 1, 1), QStringList{ QStringLiteral("Ada") });
        // The synthetic id keeps the '@' prefix rule, so it can never collide
        // with a room id.
        QVERIFY(SpaceChannelModel::directsGroupId()
                    .startsWith(QLatin1Char('@')));
        QCOMPARE(f.model.data(f.model.index(directs, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::directsGroupId());
        // ...and it sits ABOVE Rooms, which still holds the non-DM unparented
        // rooms and only those.
        const int rooms = names.indexOf(QStringLiteral("Rooms"));
        QVERIFY(rooms > directs);
        QCOMPARE(names.mid(rooms + 1, 1), QStringList{ QStringLiteral("lounge") });
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
        QVERIFY2(!names.contains(QStringLiteral("lounge")),
                 "an unparented room that is not a DM survived the scope");
        // NARROWED, not dropped. This used to assert that "Ada" was gone too,
        // which pinned the defect as intended behaviour and is why no test
        // caught it: Matrix gives no way for a DM to be a Space's child, so a
        // DM hidden by a scope is a DM hidden EVERYWHERE — and the People chip
        // then had nothing left to find. The account-wide ROOM group is still
        // a statement about the whole account and still goes; the people the
        // user talks to are not.
        QVERIFY2(names.contains(QStringLiteral("Ada")),
                 "a DM was deleted by a Space scope, so it is reachable from "
                 "nowhere while that Space is selected");
        QVERIFY2(names.contains(QStringLiteral("Direct messages")),
                 "the surviving DM has no heading of its own");
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

    // THE reported defect: "in channels mode people tab does nothing".
    //
    // The test above proves nothing about it, because it never sets a scope —
    // the same "a policy test that never reaches production" shape as the row
    // window and the rail drop. In production the rail almost always has a
    // Space selected, and while scoped the only group a DM could live in was
    // deleted wholesale, so People produced literally [Lobby, Message Search].
    // On the unfixed tree every assertion here fails.
    void thePeopleChipStillFindsPeopleWhileASpaceIsScoped()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        f.model.setScopeSpaceId(QStringLiteral("!work:x"));
        f.model.setFilterMode(1);   // People

        const QStringList names = namesOf(f.model);
        QVERIFY2(names.contains(QStringLiteral("Ada")),
                 "the People chip found no people while a Space was scoped");
        QVERIFY2(kindsOf(f.model).contains(QStringLiteral("room")),
                 "People produced a column with no rooms in it — two "
                 "navigation rows over blank space");
        // The chip did match something, so the column must not be told to
        // draw a filter-miss message over a row that is right there.
        QVERIFY(f.model.matchCount() > 0);
        QVERIFY(!f.model.empty());
        // A Space's own child rooms are not DMs, so its folder is gone — the
        // chip narrowed the column rather than merely relabelling it.
        QVERIFY(!names.contains(QStringLiteral("general")));
        QVERIFY(!names.contains(QStringLiteral("backend")));
        // And the scope is still escapable: Lobby is the head of the column.
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
    }

    // `empty` and `matchCount` answer two different questions, and the column
    // needs both: "you have no conversations" and "the People chip found none"
    // look identical when the only thing rendered is silence, which is exactly
    // how a working chip read as a dead control.
    //
    // On the unfixed tree matchCount does not exist, so this does not compile
    // — which is the strongest form of "fails on the old code" available for a
    // property that had to be added.
    void aFilterThatMatchedNothingIsCountedWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        // Rooms, one Space, no DM anywhere in the account.
        f.build({
            space(QStringLiteral("!work:x"), QStringLiteral("Work"),
                  { QStringLiteral("!general:x") }),
            room(QStringLiteral("!general:x"), QStringLiteral("general")),
        });
        QVERIFY(!f.model.empty());
        QVERIFY(f.model.matchCount() > 0);

        f.model.setFilterMode(1);   // People, and there are none
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        // A search that matches nothing is the same shape and must report the
        // same way.
        f.model.setFilterMode(0);
        f.model.setSearchQuery(QStringLiteral("zzzz"));
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY(!f.model.empty());

        // ...and it goes back up when something matches again, or the message
        // would be permanent once shown.
        f.model.setSearchQuery(QStringLiteral("gen"));
        QVERIFY(f.model.matchCount() > 0);
    }

    // The count has to be a NOTIFYING property or the message it drives never
    // appears: `matchCount` changes without the row count changing (a group
    // header leaving with its only room keeps neither), so a QML binding that
    // only woke on countChanged would miss it.
    void theMatchCountAnnouncesItselfWhenTheFilterChanges()
    {
        Fixture f;
        f.build(workspace());
        QSignalSpy matches(&f.model, &SpaceChannelModel::matchCountChanged);
        f.model.setFilterMode(1);   // People: one DM out of eight rooms
        QVERIFY2(matches.count() >= 1,
                 "the match count changed silently, so nothing in QML can "
                 "react to a filter that matched nothing");
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

        // The rebuild is COALESCED to one per event-loop turn (a burst of
        // synced room updates used to cost one full rebuild each, and each
        // rebuild materialises the whole room list), so the answer arrives on
        // the next turn rather than inside announce(). QTRY_ is the honest
        // spelling of that; a synchronous QCOMPARE here would be asserting the
        // absence of the coalescing rather than the presence of the update.
        QTRY_VERIFY(changes.count() >= 1);
        QCOMPARE(resets.count(), 0);
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

    // A DM usually carries NO room avatar: the face belongs to the other
    // person, and deriving it is three jobs (is this an unambiguous 1:1? who
    // is the peer? does anyone know their picture?) that the Classic list has
    // done privately since 0.6.x. This column read RoomInfo::avatarUrl raw and
    // drew initials for every DM, next to a Home strip showing the real faces
    // — reported with an arrow pointing at both at once.
    void aDirectMessageWearsThePeersFace()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        MemberInfo member;
        member.userId = peer.directUserId;
        member.avatarMxcUrl = QStringLiteral("mxc://example.org/sam");
        peer.members.insert(member.userId, member);

        Fixture f;
        f.build({ peer });
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        QCOMPARE(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::AvatarUrlRole).toString(),
                 QStringLiteral("mxc://example.org/sam"));
    }

    // The Rust backend never populates the per-room member snapshot — it is
    // fetched separately, on demand, only for Room Information's People tab —
    // so the ONLY route to a DM's face there is the peer's profile. It arrives
    // late, and it has to reach the row: this model's rows hold a SNAPSHOT, so
    // a bare dataChanged would repaint the same initials.
    // THE ACCOUNT SWITCH THAT "TAKES LONGER NOW", stated as a number.
    //
    // rebuild() asks the resolver to look up every DM peer it cannot answer
    // for; the resolver announced EVERY answer; this model rebuilds when a
    // peer resolves. For a peer with no avatar set — or one whose profile
    // 404s — nothing was cached, so the rebuild asked again, forever: one
    // /profile request and one full model rebuild per network round trip, per
    // such peer, for the entire session. An account switch clears the
    // resolver's caches, which is exactly what re-armed it every time.
    //
    // ON THE UNFIXED TREE the count below climbs monotonically with every
    // answer fed in. The fix is that the resolver REMEMBERS a negative answer
    // and announces only a face it actually learned.
    void anAvatarlessPeerIsAskedForExactlyOnce()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({ peer });
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

        // How a real homeserver answers a user who has never set an avatar:
        // ok, a display name, and an EMPTY avatar url.
        for (int i = 0; i < 4; ++i) {
            Q_EMIT f.client.userProfileFinished(
                f.client.nextOp, true, QStringLiteral("@sam:example.org"),
                QStringLiteral("Sam"), QString(), QString());
            QCoreApplication::processEvents();
        }
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

        // And the same for a lookup that FAILED. Retrying that on every
        // rebuild is the same loop reached by another route.
        RoomInfo other = dm(QStringLiteral("!dm2:x"), QStringLiteral("Kit"));
        other.directUserId = QStringLiteral("@kit:example.org");
        other.directUserIds = { QStringLiteral("@kit:example.org") };
        f.client.roomList.append(other);
        f.client.announce();
        QTRY_COMPARE(f.client.profileFetches.count(QStringLiteral("@kit:example.org")), 1);
        for (int i = 0; i < 4; ++i) {
            Q_EMIT f.client.userProfileFinished(
                f.client.nextOp, false, QStringLiteral("@kit:example.org"),
                QString(), QString(), QStringLiteral("not_found"));
            QCoreApplication::processEvents();
        }
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@kit:example.org")), 1);
    }

    // The second guard, and the one that bounds the cost of a sync BURST: a
    // run of room updates in one event-loop turn must cost ONE rebuild, not
    // one each. Asserted as a DELTA because the fixture's own setup
    // legitimately materialises the room list several times.
    void aBurstOfRoomUpdatesCostsOneRebuild()
    {
        Fixture f;
        f.build(workspace());
        QCoreApplication::processEvents();
        // Counted on the MODEL, not on the client: the client's rooms() is
        // also called by SpaceManager's own per-update rebuild, which is not
        // this model's cost and would drown the signal.
        const int before = f.model.rebuildCountForTest();
        for (int i = 0; i < 10; ++i)
            f.client.announce();
        QCoreApplication::processEvents();
        QTRY_VERIFY(f.model.rebuildCountForTest() > before);
        const int rebuilds = f.model.rebuildCountForTest() - before;
        QVERIFY2(rebuilds == 1,
                 qPrintable(QStringLiteral("ten room updates in one turn cost "
                                           "%1 rebuilds; they are not "
                                           "coalesced")
                                .arg(rebuilds)));
    }

    void aLateProfileReachesTheRowRatherThanJustRepaintingIt()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({ peer });
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        auto avatar = [&f, row] {
            return f.model.data(f.model.index(row, 0),
                                SpaceChannelModel::AvatarUrlRole).toString();
        };
        QVERIFY2(avatar().isEmpty(),
                 "an avatar was invented before anyone looked the peer up");

        Q_EMIT f.client.userProfileFinished(
            0, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        // Coalesced, as above.
        QTRY_COMPARE(avatar(), QStringLiteral("mxc://example.org/sam"));
        // And the column did not move: this layout's whole point is that rows
        // hold still, so a profile landing mid-scroll must not be the
        // exception.
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!dm:x")), row);
    }

    // Never an arbitrary face for a group DM. `m.direct` naming two targets is
    // the authoritative "this is not a 1:1" signal.
    void aGroupDirectMessageBorrowsNobodysFace()
    {
        RoomInfo group = dm(QStringLiteral("!group:x"), QStringLiteral("Three"));
        group.directUserId = QStringLiteral("@sam:example.org");
        group.directUserIds = { QStringLiteral("@sam:example.org"),
                                QStringLiteral("@kim:example.org") };
        MemberInfo member;
        member.userId = group.directUserId;
        member.avatarMxcUrl = QStringLiteral("mxc://example.org/sam");
        group.members.insert(member.userId, member);

        Fixture f;
        f.build({ group });
        const int row = f.model.rowForRoom(QStringLiteral("!group:x"));
        QVERIFY(row >= 0);
        QVERIFY2(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::AvatarUrlRole)
                     .toString().isEmpty(),
                 "a group DM wears one participant's face");
    }

    // An explicit room avatar always wins, even on a DM.
    void anExplicitRoomAvatarIsNeverOverridden()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.avatarUrl = QStringLiteral("mxc://example.org/room");
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        MemberInfo member;
        member.userId = peer.directUserId;
        member.avatarMxcUrl = QStringLiteral("mxc://example.org/sam");
        peer.members.insert(member.userId, member);

        Fixture f;
        f.build({ peer });
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QCOMPARE(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::AvatarUrlRole).toString(),
                 QStringLiteral("mxc://example.org/room"));
    }

    // THE COALESCING TIMER MUST NOT OUTLIVE THE STATE THAT ARMED IT.
    //
    // Coalescing bought a real cost reduction and brought a hazard with it:
    // rebuild() no longer runs where the signal was received, it runs on the
    // next event-loop turn — and the account switch happens in between. The
    // sequence is ordinary, not exotic: the outgoing account's last sync arms
    // a rebuild, the user clicks the switcher, detachSession() invalidates the
    // session and emits loggedOut, and only THEN does the queued rebuild run,
    // against a client that has been detached and caches that have been
    // cleared.
    //
    // It survives that today because every dereference in rebuild() happens to
    // be guarded — which is a property of the current guards, not a contract
    // anybody wrote down, and the next field added to that pass inherits no
    // protection at all. The invariant is the fix: after rebuild() returns,
    // nothing is queued.
    //
    // ON THE UNFIXED TREE both halves below count one EXTRA rebuild after the
    // state changed, because the timer armed beforehand still fires.
    void aQueuedRebuildNeverOutlivesTheStateThatArmedIt()
    {
        // The sources are replaced outright: the queued rebuild belongs to
        // ones that are gone, and would run against whatever is wired up
        // instead. It early-returns harmlessly TODAY; the point is that it
        // does not run at all, which is the only version of that guarantee
        // the next field added to rebuild() inherits.
        Fixture f;
        f.build(workspace());
        // qWait, not processEvents: the coalescer is a ZERO-interval QTimer,
        // and processEvents() alone is not a reliable way to make one fire.
        // A test that silently fails to deliver the queued work would pass on
        // the broken tree, which is the one outcome that would make this
        // decoration.
        QTest::qWait(50);
        f.client.announce();
        f.model.setSources(nullptr, nullptr, nullptr);
        int settled = f.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(f.model.rebuildCountForTest(), settled);

        // The sign-out / account-switch shape, wired by hand for one reason:
        // SpaceManager also rebuilds on loggedOut and announces afterwards, so
        // with it attached to the client a rebuild is LEGITIMATELY re-armed
        // after the detach and no count can distinguish that from the stale
        // one. Leaving the manager clientless removes the second arming, and
        // what is left is exactly the question — a rebuild armed by the
        // outgoing account's last update, delivered after the session ended.
        Fixture g;
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        g.client.roomList = { peer };
        g.model.setSettings(&g.settings);
        g.model.setSources(&g.client, &g.spaces, &g.layout);
        QTest::qWait(50);
        // Arms one: a late profile is announced, and this model rebuilds to
        // get the face into the row's snapshot.
        Q_EMIT g.client.userProfileFinished(
            g.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        g.client.logout();
        settled = g.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(g.model.rebuildCountForTest(), settled);
    }

    // The other half of the same hazard, and the one that is not merely
    // wasteful. AppController declares `m_spaces` AFTER `m_spaceChannels`, so
    // members are destroyed in reverse declaration order and the SpaceManager
    // this model reads dies FIRST — leaving a non-null pointer to freed memory
    // that rebuild()'s `!m_spaces` guard cannot see. Nothing spins an event
    // loop in that window today, which is what makes it safe, and nothing
    // states that.
    //
    // HONEST ABOUT WHAT THIS PROVES: on the unfixed tree this is a
    // use-after-free, which is undefined — it will usually pass, and fails
    // deterministically only under ASan. It is here to pin the CLEARED
    // POINTER, not to reproduce a crash.
    void aDestroyedSourceLeavesANullRatherThanADanglingPointer()
    {
        FakeClient client;
        SettingsManager settings;
        RailLayoutStore layout{ &settings };
        auto *spaces = new SpaceManager;
        client.roomList = workspace();
        spaces->setClient(&client);

        SpaceChannelModel model;
        model.setSettings(&settings);
        model.setSources(&client, spaces, &layout);
        QVERIFY(model.rowCount() > 0);

        delete spaces;
        // Any rebuild at all now: a filter chip is the cheapest one that is
        // not a source signal.
        model.setFilterMode(1);
        QCOMPARE(model.rowCount(), 0);
        QTest::qWait(50);
        QCOMPARE(model.rowCount(), 0);
    }

    // `userProfileFinished` is ONE signal shared by every consumer of the
    // client — the account switcher, member lists, the profile popover, this
    // resolver. The resolver reads answers to ops it did not start on purpose
    // (that is what lets a self-DM adopt the signed-in account's own face),
    // and the negative cache added this round did the same thing with
    // FAILURES. It should not: "the profile says there is no avatar" is a fact
    // about the user, but "the request failed" is a fact about one request.
    //
    // ON THE UNFIXED TREE a single failed lookup anywhere in the application
    // permanently marks that user as pictureless here, so a DM with them
    // renders initials for the rest of the session and nothing ever retries —
    // resolveMissing() skips a cached negative, and no code path clears one
    // short of sign-out.
    void aFailedLookupSomebodyElseStartedNeverWedgesADm()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({});
        // An op this model never issued — note the id is not one the fake
        // client ever handed out — reporting a failure for that same user.
        Q_EMIT f.client.userProfileFinished(
            9999, false, QStringLiteral("@sam:example.org"), QString(),
            QString(), QStringLiteral("timeout"));
        QTest::qWait(50);
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 0);

        // The DM now arrives. Its peer has to be looked up: nothing here ever
        // asked about them.
        f.client.roomList = { peer };
        f.client.announce();
        QTRY_COMPARE_WITH_TIMEOUT(
            f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1,
            2000);

        // And the answer still reaches the row.
        Q_EMIT f.client.userProfileFinished(
            f.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        QTRY_COMPARE(f.model.data(f.model.index(row, 0),
                                  SpaceChannelModel::AvatarUrlRole).toString(),
                     QStringLiteral("mxc://example.org/sam"));

        // The loop guard is untouched: OUR OWN failure is still remembered, so
        // a peer whose lookup we incurred and lost is not re-asked on every
        // rebuild. (anAvatarlessPeerIsAskedForExactlyOnce is the full case.)
    }

    // The answer may not come back under the id we asked with — the id is
    // normalised by the SDK. The pending release already takes BOTH keys
    // because getting that wrong once left a peer stuck pending forever; the
    // CACHE did not, so a face could be learned and filed under a key nobody
    // queries: both owners look an avatar up by the room's `directUserId`,
    // which is the id we asked with.
    //
    // Structural, not observed — no capture of a real normalisation exists.
    // It is here because the failure mode is silent and indistinguishable from
    // "the server has no picture for them".
    void aFaceReturnedUnderANormalisedIdStillReachesItsRow()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@Sam:example.org");
        peer.directUserIds = { QStringLiteral("@Sam:example.org") };

        Fixture f;
        f.build({ peer });
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@Sam:example.org")), 1);

        Q_EMIT f.client.userProfileFinished(
            f.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        QTRY_COMPARE(f.model.data(f.model.index(row, 0),
                                  SpaceChannelModel::AvatarUrlRole).toString(),
                     QStringLiteral("mxc://example.org/sam"));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
