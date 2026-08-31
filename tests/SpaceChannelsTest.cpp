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

/// `n` seconds before a fixed instant, so "newer" is unambiguous and no case
/// depends on wall-clock timing. Rooms built by `room()` above carry NO
/// activity at all, which is why the alphabetical assertions elsewhere in this
/// file still hold: with every timestamp invalid the recency comparator falls
/// through to its name tiebreak.
QDateTime ago(int seconds)
{
    static const QDateTime base =
        QDateTime(QDate(2026, 8, 31), QTime(13, 0), QTimeZone::UTC);
    return base.addSecs(-seconds);
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
        /// The rail's three selections, spelled the way the rail spells them.
        /// Every view test goes through one of these rather than poking
        /// scopeSpaceId with a literal: the selection IS the view, and a test
        /// that sets a value the rail cannot produce proves nothing about the
        /// column the user sees (the row-window and rail-drag lesson).
        void selectHome() { model.setScopeSpaceId(SpaceManager::allRoomsId()); }
        void selectPeople() { model.setScopeSpaceId(SpaceManager::peopleId()); }
        void selectSpace(const QString &id) { model.setScopeSpaceId(id); }
    };

private Q_SLOTS:
    void homeRoomsAreNewestFirstNotAlphabetical();
    void directMessageChatsAreNewestFirst();
    void aSpacesRoomsAreNewestFirstNotInChildOrder();
    void aRoomMovesWhenSomebodySpeaksInIt();
    // THE DIRECT MESSAGES TAB MUST SURVIVE A SPACE-LIST REBUILD.
    //
    // SpaceManager drops the active scope when it is not a joined Space, which
    // is right for a Space the account has left. But the rail's selection also
    // carries TAB SENTINELS -- "@people" and "@orphans" -- and those are not
    // rooms, so they can never be in the membership set. Checking them the same
    // way threw the user out of Direct Messages back to Home every time
    // anything rebuilt the space list, which opening a DM does: reported as
    // "click on a person to chat, it throws me to home".
    void tabSentinelsSurviveARebuildThatDropsAMissingSpace()
    {
        FakeClient client;
        SpaceManager spaces;
        client.roomList = workspace();
        spaces.setClient(&client);

        for (const QString &sentinel :
             { SpaceManager::peopleId(), SpaceManager::orphansId() }) {
            spaces.setActiveSpaceId(sentinel);
            QCOMPARE(spaces.activeSpaceId(), sentinel);
            // Any room-list change rebuilds the space list. Opening a DM does
            // this, which is why the bug fired on a click that never touched
            // the rail.
            client.roomList = workspace();
            client.announce();
            QCOMPARE(spaces.activeSpaceId(), sentinel);
        }

        // And the behaviour that check exists for is unchanged: a real Space id
        // the account is not in is still dropped.
        spaces.setActiveSpaceId(QStringLiteral("!not-a-space-we-are-in:x"));
        client.roomList = workspace();
        client.announce();
        QVERIFY2(spaces.activeSpaceId().isEmpty(),
                 "a Space the account is not in must still be dropped");
    }

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

    // The rail chooses one of THREE views and the model produces exactly that
    // one. This is the whole shape of the layout in one test.
    void theRailSelectionChoosesOneOfThreeViews()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

        // HOME: the command rows, then the rooms in no Space. No Spaces —
        // the rail is already showing every one of them, and repeating the
        // set here is what made picking one look like it did nothing.
        f.selectHome();
        QCOMPARE(f.model.viewKind(), QStringLiteral("home"));
        QStringList names = namesOf(f.model);
        QCOMPARE(names.mid(0, 4),
                 QStringList({ QStringLiteral("Create Room"),
                               QStringLiteral("Join with Address"),
                               QStringLiteral("Explore Spaces"),
                               QStringLiteral("Message Search") }));
        QVERIFY2(!names.contains(QStringLiteral("Work")),
                 "Home repeated a Space the rail already lists");
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM is at Home as well as in its own tab");
        QVERIFY(names.contains(QStringLiteral("Rooms")));
        QVERIFY(names.contains(QStringLiteral("lounge")));

        // PEOPLE: Create Chat and the DMs, and nothing else at all.
        f.selectPeople();
        QCOMPARE(f.model.viewKind(), QStringLiteral("people"));
        names = namesOf(f.model);
        QCOMPARE(names.at(0), QStringLiteral("Create Chat"));
        QVERIFY(names.contains(QStringLiteral("Chats")));
        QVERIFY(names.contains(QStringLiteral("Ada")));
        QVERIFY2(!names.contains(QStringLiteral("lounge")),
                 "an ordinary room is in the Direct Messages tab");
        QVERIFY2(!names.contains(QStringLiteral("Work")),
                 "a Space is in the Direct Messages tab");

        // A SPACE: Lobby, Message Search, then its own rooms.
        f.selectSpace(QStringLiteral("!work:x"));
        QCOMPARE(f.model.viewKind(), QStringLiteral("space"));
        names = namesOf(f.model);
        QCOMPARE(names.mid(0, 2),
                 QStringList({ QStringLiteral("Lobby"),
                               QStringLiteral("Message Search") }));
        QVERIFY(names.contains(QStringLiteral("Work")));
        QVERIFY2(names.contains(QStringLiteral("Engineering")),
                 "a subspace of the selected Space is missing");
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM appeared under a Space, which Matrix cannot express");
        QVERIFY2(!names.contains(QStringLiteral("lounge")),
                 "a room in no Space appeared under a Space");
        QVERIFY(!f.model.empty());
    }

    void everyJoinedSpaceIsAFlatFolderAndSubspacesAreNotNested()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

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
        f.selectHome();

        const QStringList names = namesOf(f.model);
        const int rooms = names.indexOf(QStringLiteral("Rooms"));
        QVERIFY2(rooms >= 0, "there is no Rooms group");
        QCOMPARE(kindsOf(f.model).at(rooms), QStringLiteral("group"));
        // Sorted by name, so the list does not reshuffle between syncs.
        QCOMPARE(names.mid(rooms + 1, 1), QStringList{ QStringLiteral("lounge") });
        QVERIFY2(!names.contains(QStringLiteral("Favourites")),
                 "the Favourites group came back");
    }

    // DMs live in a TAB of their own now, and in exactly one place. Two
    // earlier designs put them in "Rooms" with every other unparented room
    // (a column of nothing but people under a heading that says Rooms) and
    // then in a "Direct messages" group that every other view had to carry so
    // a scope could not delete it. A tab settles both: one home for a DM, and
    // no other view has to keep it reachable.
    void directMessagesLiveInTheirOwnTabAndNowhereElse()
    {
        Fixture f;
        f.build(workspace());

        f.selectPeople();
        QStringList names = namesOf(f.model);
        const int chats = names.indexOf(QStringLiteral("Chats"));
        QVERIFY2(chats >= 0, "the Direct Messages tab has no Chats group");
        QCOMPARE(kindsOf(f.model).at(chats), QStringLiteral("group"));
        QCOMPARE(names.mid(chats + 1, 1), QStringList{ QStringLiteral("Ada") });
        // The synthetic id keeps the '@' prefix rule, so it can never collide
        // with a room id.
        QVERIFY(SpaceChannelModel::directsGroupId()
                    .startsWith(QLatin1Char('@')));
        QVERIFY(SpaceChannelModel::peopleViewId()
                    .startsWith(QLatin1Char('@')));
        QCOMPARE(f.model.data(f.model.index(chats, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::directsGroupId());

        // ONE place. Neither Home nor any Space carries the DM as well.
        f.selectHome();
        names = namesOf(f.model);
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "the DM is at Home as well as in its own tab");
        QCOMPARE(names.mid(names.indexOf(QStringLiteral("Rooms")) + 1, 1),
                 QStringList{ QStringLiteral("lounge") });
        f.selectSpace(QStringLiteral("!work:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Ada")));
    }

    // Every one of the four command rows carries an id the host dispatches on
    // and a glyph name the icon font actually has. A row the host cannot name
    // renders as a control that looks clickable and does nothing.
    void theCommandRowsCarryAnActionIdAndAGlyph()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

        QSet<QString> seen;
        auto sweep = [&f, &seen] {
            for (int i = 0; i < f.model.rowCount(); ++i) {
                const QModelIndex idx = f.model.index(i, 0);
                const QString kind =
                    f.model.data(idx, SpaceChannelModel::KindRole).toString();
                if (kind != QLatin1String("action"))
                    continue;
                const QString id =
                    f.model.data(idx, SpaceChannelModel::RoomIdRole).toString();
                QVERIFY2(SpaceChannelModel::actionIds().contains(id),
                         qPrintable(QStringLiteral("unknown action id %1")
                                        .arg(id)));
                QVERIFY2(!f.model.data(idx, SpaceChannelModel::IconNameRole)
                              .toString().isEmpty(),
                         qPrintable(QStringLiteral("%1 has no glyph").arg(id)));
                QVERIFY2(!f.model.data(idx, SpaceChannelModel::NameRole)
                              .toString().isEmpty(),
                         qPrintable(QStringLiteral("%1 has no label").arg(id)));
                seen.insert(id);
            }
        };
        f.selectHome();
        sweep();
        f.selectPeople();
        sweep();
        // A Space view has none: it has Lobby instead.
        f.selectSpace(QStringLiteral("!work:x"));
        for (const QString &kind : kindsOf(f.model))
            QVERIFY(kind != QLatin1String("action"));

        const QStringList all = SpaceChannelModel::actionIds();
        QCOMPARE(seen.size(), all.size());
        for (const QString &id : all)
            QVERIFY2(seen.contains(id),
                     qPrintable(QStringLiteral("%1 is in no view").arg(id)));
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
        // Each Space's own view contains it: that is what "this Space
        // contains it" means, and a first-parent-wins rule would make one of
        // the two look incomplete.
        f.selectSpace(QStringLiteral("!a:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        f.selectSpace(QStringLiteral("!b:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        // ...and it is NOT also at Home, because a Space does list it.
        f.selectHome();
        const QStringList names = namesOf(f.model);
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        QVERIFY(!names.contains(QStringLiteral("shared")));
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
        f.selectHome();
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
        f.selectHome();
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
            space(QStringLiteral("!parent:x"), QStringLiteral("Parent"),
                  { QStringLiteral("!a:x"), QStringLiteral("!b:x"),
                    QStringLiteral("!c:x") }),
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!ra:x") },
                  { QStringLiteral("!parent:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!rb:x") },
                  { QStringLiteral("!parent:x") }),
            space(QStringLiteral("!c:x"), QStringLiteral("Gamma"),
                  { QStringLiteral("!rc:x") },
                  { QStringLiteral("!parent:x") }),
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

        // A Space's view is that Space then its SUBSPACES, and the subspaces
        // are ranked by the rail's arrangement — so a Space with several
        // subspaces lists them in the order the user dragged them into,
        // rather than in whatever order the hierarchy walk reached them.
        f.selectSpace(QStringLiteral("!parent:x"));
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
                 "an unparented room survived into a Space's own view");
        // A DM IS NOT HERE, and that is now safe to assert: it has a tab of
        // its own, so hiding it here hides it from nothing. Two earlier
        // designs could not say this — the DM group had to ride along inside
        // every view because there was nowhere else for it to be.
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM appeared under a Space, which Matrix cannot express");
        QVERIFY2(!names.contains(QStringLiteral("Direct messages")),
                 "the account-wide DM group survived into a Space's view");
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

        // Home is one rail tile away, which is what makes a Space escapable.
        f.selectHome();
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("lounge")));
    }

    void aPseudoRailRowThatIsNotTheDmTabIsHome()
    {
        // "" is Home and "@orphans" is "Other rooms". Neither is a Space and
        // neither is the DM tab, so both produce the Home view rather than an
        // empty column — the selection is kept verbatim and CLASSIFIED, and
        // an unrecognised one has to land somewhere that lists something.
        Fixture f;
        f.build(workspace());
        f.selectHome();
        const int home = f.model.rowCount();
        QVERIFY(home > 0);
        f.model.setScopeSpaceId(SpaceManager::orphansId());
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("home"));
        QCOMPARE(f.model.rowCount(), home);
        // The DM tab is the one pseudo id that is NOT Home.
        f.selectPeople();
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("people"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
    }

    void aSelectionOnASpaceTheAccountNoLongerHasStaysThatSpace()
    {
        // Left the Space while it was selected. It stays the selection, and
        // the view renders its own emptiness — which is the truth. Falling
        // back to "everything" here would silently become a DIFFERENT Space's
        // view under a rail tile that is no longer there, and the account is
        // still one rail tile away either way.
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        f.selectSpace(QStringLiteral("!gone:x"));
        const QStringList names = namesOf(f.model);
        QCOMPARE(f.model.viewKind(), QStringLiteral("space"));
        QVERIFY2(!names.contains(QStringLiteral("Work")),
                 "a Space the user did not select is being shown as if they "
                 "had");
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        // Lobby is still there, so the view is navigable rather than blank...
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
        // ...and the ACCOUNT is not claimed to be empty: it is not.
        QVERIFY(!f.model.empty());
        QCOMPARE(f.model.matchCount(), 0);
    }

    void aCollapsedFolderHidesItsRoomsButNotItsActivity()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

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
        f.selectSpace(QStringLiteral("!work:x"));
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
        reopened.setScopeSpaceId(QStringLiteral("!work:x"));
        QVERIFY(reopened.isCollapsed(QStringLiteral("!work:x")));
        QVERIFY(!namesOf(reopened).contains(QStringLiteral("general")));
    }

    void aSearchOpensEveryFolderAndPutsThemBack()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
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

    // The chips still filter WITHIN a view. In Channels the host only offers
    // All and Unreads — the People/Rooms split IS the rail's two tabs now, and
    // a chip repeating it would match nothing at Home — but the model keeps
    // the whole closed set, because it is shared with Classic and a mode it
    // refused would go inert there.
    void theFilterChipsSelectRoomsWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

        f.model.setFilterMode(3);   // Unreads
        const QStringList unread = namesOf(f.model);
        QVERIFY(unread.contains(QStringLiteral("backend")));
        QVERIFY(!unread.contains(QStringLiteral("general")));
        // A filter that matched little is NOT an empty account, and the empty
        // state must not claim it is.
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        f.model.setFilterMode(0);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));

        // The People and Rooms modes still mean what they mean, in the one
        // view where both kinds can be present at once.
        f.selectPeople();
        f.model.setFilterMode(1);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        f.model.setFilterMode(2);
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("Ada")),
                 "the Rooms mode kept a DM");
    }

    // The report this whole split came from: "in channels mode people tab
    // does nothing". Two designs tried to answer it with a chip. A DM cannot
    // be a Space's child, so under a selected Space the chip had nothing to
    // find unless every view carried a DM group it did not otherwise want —
    // and at Home the chip's result was a subset of what was already there.
    //
    // The tab is the answer, and this is what it has to guarantee: whatever
    // the rail was last pointed at, DMs are ALWAYS exactly one tile away and
    // the tab lists all of them.
    void everyDirectMessageIsReachableFromTheTabWhateverElseIsSelected()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

        const QStringList froms = { SpaceManager::allRoomsId(),
                                    SpaceManager::orphansId(),
                                    QStringLiteral("!work:x"),
                                    QStringLiteral("!eng:x"),
                                    QStringLiteral("!gone:x") };
        for (const QString &from : froms) {
            f.model.setScopeSpaceId(from);
            f.selectPeople();
            const QStringList names = namesOf(f.model);
            QVERIFY2(names.contains(QStringLiteral("Ada")),
                     qPrintable(QStringLiteral("a DM is unreachable after %1")
                                    .arg(from)));
            QVERIFY2(kindsOf(f.model).contains(QStringLiteral("room")),
                     "the DM tab produced no rooms at all");
            QVERIFY(f.model.matchCount() > 0);
            QVERIFY(!f.model.empty());
            // ...and nothing that is not a DM came with them.
            QVERIFY(!names.contains(QStringLiteral("general")));
            QVERIFY(!names.contains(QStringLiteral("backend")));
            QVERIFY(!names.contains(QStringLiteral("lounge")));
            QVERIFY(!names.contains(QStringLiteral("Work")));
        }
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
        f.selectSpace(QStringLiteral("!work:x"));
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
        f.selectSpace(QStringLiteral("!work:x"));
        QSignalSpy matches(&f.model, &SpaceChannelModel::matchCountChanged);
        f.model.setFilterMode(1);   // People: a Space's rooms are not DMs
        QVERIFY2(matches.count() >= 1,
                 "the match count changed silently, so nothing in QML can "
                 "react to a filter that matched nothing");
    }

    void theMessageSearchRowIsAbsentWhenTheServerCannotSearch()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
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
        f.selectSpace(QStringLiteral("!work:x"));
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
        // An ACTION row carries a synthetic '@' id, never a room id: the host
        // dispatches on it, and anything that treated it as a room would try
        // to open a room that does not exist.
        f.selectHome();
        for (int i = 0; i < f.model.rowCount(); ++i) {
            const QModelIndex idx = f.model.index(i, 0);
            if (f.model.data(idx, SpaceChannelModel::KindRole).toString()
                != QLatin1String("action")) {
                continue;
            }
            QVERIFY(f.model.data(idx, SpaceChannelModel::RoomIdRole)
                        .toString().startsWith(QLatin1Char('@')));
        }
        // A group's synthetic id can never collide with a room id.
        QVERIFY(SpaceChannelModel::roomsGroupId().startsWith(QLatin1Char('@')));
        QVERIFY(SpaceChannelModel::invitesGroupId()
                    .startsWith(QLatin1Char('@')));
        for (const QString &id : SpaceChannelModel::actionIds())
            QVERIFY(id.startsWith(QLatin1Char('@')));
    }

    void rowForRoomNeverMatchesAFolderOrAGroup()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
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
        f.selectSpace(QStringLiteral("!work:x"));
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
        f.selectSpace(QStringLiteral("!s:x"));

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
        f.selectHome();
        QVERIFY(f.model.empty());
        // Home keeps its command rows: an account with nothing in it is
        // exactly when "Create Room" and "Join with Address" matter most, and
        // a blank column would offer no way out of being empty.
        QCOMPARE(kindsOf(f.model),
                 QStringList({ QStringLiteral("action"),
                               QStringLiteral("action"),
                               QStringLiteral("action") }));
        QCOMPARE(f.model.matchCount(), 0);
        // A Space's view has no such rows and is genuinely bare.
        f.selectSpace(QStringLiteral("!nothing:x"));
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
        f.selectPeople();
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
        f.selectPeople();
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
        f.selectPeople();
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
        f.selectPeople();
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
        f.selectPeople();
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
        f.selectPeople();
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
        f.selectPeople();
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


// ── Activity ordering (2026-08-31) ───────────────────────────────────────
//
// This column used to sort alphabetically, and the header said so as a
// CONTRACT: "nothing here is activity-ordered: a channel list whose rows move
// when somebody speaks is not a channel list". That was reversed — a stable
// order is useless if the room somebody just posted in sits wherever its name
// puts it, because the thing the user is looking for never moves to where
// they are looking.
//
// These cases stamp real activity times. Note that every other case in this
// file leaves lastActivity INVALID, which is why their alphabetical
// expectations still hold: the shared comparator falls through to its name
// tiebreak when no room has ever been spoken in.

void SpaceChannelsTest::homeRoomsAreNewestFirstNotAlphabetical()
{
    Fixture f;
    // Named so alphabetical and recency give OPPOSITE answers — otherwise the
    // case passes on the old code and proves nothing.
    auto alpha = room(QStringLiteral("!a:x"), QStringLiteral("Alpha"));
    alpha.lastActivity = ago(900);
    auto zulu = room(QStringLiteral("!z:x"), QStringLiteral("Zulu"));
    zulu.lastActivity = ago(10);
    f.build({ alpha, zulu });
    f.selectHome();

    const QStringList names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Zulu"))
                 < names.indexOf(QStringLiteral("Alpha")),
             "Home rooms are still alphabetical, not newest-first");
}

void SpaceChannelsTest::directMessageChatsAreNewestFirst()
{
    Fixture f;
    auto ada = dm(QStringLiteral("!ada:x"), QStringLiteral("Ada"));
    ada.lastActivity = ago(900);
    auto zoe = dm(QStringLiteral("!zoe:x"), QStringLiteral("Zoe"));
    zoe.lastActivity = ago(10);
    f.build({ ada, zoe });
    f.selectPeople();

    const QStringList names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Zoe"))
                 < names.indexOf(QStringLiteral("Ada")),
             "People chats are still alphabetical, not newest-first");
}

void SpaceChannelsTest::aSpacesRoomsAreNewestFirstNotInChildOrder()
{
    Fixture f;
    // m.space.child order deliberately puts the STALE room first, so a list
    // that still follows it fails here. That order is the Space admin's idea
    // of importance, which is a different question from where the
    // conversation is.
    auto space = room(QStringLiteral("!space:x"), QStringLiteral("Work"));
    space.isSpace = true;
    space.childRoomIds = { QStringLiteral("!stale:x"), QStringLiteral("!live:x") };
    auto stale = room(QStringLiteral("!stale:x"), QStringLiteral("Archive"));
    stale.lastActivity = ago(9000);
    auto live = room(QStringLiteral("!live:x"), QStringLiteral("General"));
    live.lastActivity = ago(5);
    f.build({ space, stale, live });
    f.selectSpace(space.id);

    const QStringList names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("General"))
                 < names.indexOf(QStringLiteral("Archive")),
             "a Space's rooms still follow m.space.child order");
    // The STRUCTURE is untouched: the Space is still the group header above
    // its own rooms.
    QVERIFY(names.indexOf(QStringLiteral("Work"))
            < names.indexOf(QStringLiteral("General")));
}

void SpaceChannelsTest::aRoomMovesWhenSomebodySpeaksInIt()
{
    // The reason lastActivity had to be carried ON the row: applyRows diffs
    // rows BY VALUE, so a sort key the row does not hold is a key the diff
    // cannot see change — the list would claim to be activity-ordered and
    // then sit still when a message arrived.
    Fixture f;
    auto alpha = room(QStringLiteral("!a:x"), QStringLiteral("Alpha"));
    alpha.lastActivity = ago(10);
    auto zulu = room(QStringLiteral("!z:x"), QStringLiteral("Zulu"));
    zulu.lastActivity = ago(900);
    f.build({ alpha, zulu });
    f.selectHome();
    QStringList names = namesOf(f.model);
    QVERIFY(names.indexOf(QStringLiteral("Alpha"))
            < names.indexOf(QStringLiteral("Zulu")));

    // Somebody speaks in the older room.
    f.client.roomList[1].lastActivity = ago(0);
    f.client.announce();
    // Source signals coalesce onto a zero-timer so a burst costs one rebuild;
    // settle it before asserting.
    QCoreApplication::processEvents();

    names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Zulu"))
                 < names.indexOf(QStringLiteral("Alpha")),
             "the row did not move when the room received a message");
}

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
