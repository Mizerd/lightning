// The Channels navigation layout: SpaceChannelModel's flat, global list of
// Space folders. What it guarantees:
//
//  * It works at Home; there is no scoping Space the list depends on.
//  * Flat by Space: a subspace gets its own folder, never nested under its
//    parent, and a parent's folder holds only its direct children.
//  * Nothing joined is unreachable: a room with no joined Space parent is in
//    "Rooms", and a room in two Spaces is in both folders.
//  * A DM is never scoped by a Space (Matrix cannot make it a Space's child);
//    DMs have their own tab.
//  * A filter miss is not an empty account: `empty` and `matchCount` answer
//    different questions.
//  * Spaces follow the rail's arrangement; rooms are ordered by activity.
//  * A collapsed folder still reports what it hides, and collapse is stored,
//    so it survives a rebuild.
//  * A search opens every folder and restores the collapse state afterwards.
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

    // Returns a real op id: the base class's 0 means "backend refused" to
    // DirectAvatarResolver, which then skips its pending bookkeeping.
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
    // Direct children, in the Space's own m.space.child order.
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

/// `n` seconds before a fixed instant, so recency is unambiguous. Rooms built
/// by `room()` carry no activity, so the recency comparator falls through to
/// its name tiebreak and alphabetical expectations elsewhere still hold.
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

/// What the rail tile for `spaceId` shows on its badge, read through the same
/// map the rail reads.
int railUnreadTotal(const SpaceManager &spaces, const QString &spaceId)
{
    for (const QVariant &value : spaces.allSpaces()) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("spaceId")).toString() == spaceId)
            return entry.value(QStringLiteral("unreadTotal")).toInt();
    }
    return -1;
}

} // namespace

class SpaceChannelsTest : public QObject
{
    Q_OBJECT

private:
    /// Two Spaces, one a subspace of the other, plus a room in no Space and a
    /// DM. The subspace's rooms must appear under it only, not nested.
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
        /// The rail's three selections, spelled as the rail spells them, so
        /// tests only use values the rail can produce.
        void selectHome() { model.setScopeSpaceId(SpaceManager::allRoomsId()); }
        void selectPeople() { model.setScopeSpaceId(SpaceManager::peopleId()); }
        void selectSpace(const QString &id) { model.setScopeSpaceId(id); }
    };

private Q_SLOTS:
    void homeRoomsAreNewestFirstNotAlphabetical();
    void directMessageChatsAreNewestFirst();
    void aSpacesRoomsAreNewestFirstNotInChildOrder();
    void aRoomMovesWhenSomebodySpeaksInIt();
    void aFavouriteRisesToTheTopOfItsGroup();
    // The DM tab survives a space-list rebuild: SpaceManager drops a scope
    // that is not a joined Space, but the rail's tab sentinels ("@people",
    // "@orphans") are never rooms and must be kept.
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
            // Any room-list change rebuilds the space list; opening a DM does.
            client.roomList = workspace();
            client.announce();
            QCOMPARE(spaces.activeSpaceId(), sentinel);
        }

        // A real Space id the account is not in is still dropped.
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

    // The rail chooses one of three views and the model produces exactly
    // that one.
    void theRailSelectionChoosesOneOfThreeViews()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

        // Home: the command rows, then rooms in no Space. No Spaces: the rail
        // already shows them.
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
        // Home also lists the joined DMs as a group after Rooms.
        QVERIFY2(names.contains(QStringLiteral("Ada")),
                 "Home lost its Direct Messages group");
        QVERIFY(names.contains(QStringLiteral("Rooms")));
        QVERIFY(names.contains(QStringLiteral("lounge")));

        // People: Create Chat and the DMs, nothing else.
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

        // A Space: Lobby, Message Search, then its own rooms.
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
        // Both folders sit at depth 0: the subspace is a sibling, not a level.
        QCOMPARE(f.model.data(f.model.index(work, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);

        // Work holds only its direct rooms; backend/frontend appear once,
        // under Engineering.
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

    // DMs live in their own tab and never under a Space.
    void directMessagesLiveInTheirOwnTabAndNeverUnderASpace()
    {
        Fixture f;
        f.build(workspace());

        f.selectPeople();
        QStringList names = namesOf(f.model);
        const int chats = names.indexOf(QStringLiteral("Chats"));
        QVERIFY2(chats >= 0, "the Direct Messages tab has no Chats group");
        QCOMPARE(kindsOf(f.model).at(chats), QStringLiteral("group"));
        QCOMPARE(names.mid(chats + 1, 1), QStringList{ QStringLiteral("Ada") });
        // The synthetic id keeps the '@' prefix, so it never collides with a
        // room id.
        QVERIFY(SpaceChannelModel::directsGroupId()
                    .startsWith(QLatin1Char('@')));
        QVERIFY(SpaceChannelModel::peopleViewId()
                    .startsWith(QLatin1Char('@')));
        QCOMPARE(f.model.data(f.model.index(chats, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::directsGroupId());

        // The tab is the complete list. Home lists joined DMs as a group after
        // Rooms; a Space view never carries one.
        f.selectHome();
        names = namesOf(f.model);
        QCOMPARE(names.mid(names.indexOf(QStringLiteral("Rooms")) + 1, 1),
                 QStringList{ QStringLiteral("lounge") });
        QVERIFY(names.contains(QStringLiteral("Ada")));
        f.selectSpace(QStringLiteral("!work:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Ada")));
    }

    // Home lists the joined DMs in a Direct Messages group after Rooms; a DM
    // invite stays in the tab's Invites group; the People chip at Home narrows
    // to that group; the tab itself is unchanged.
    void homeListsTheJoinedDirectMessagesUnderRooms()
    {
        Fixture f;
        QList<RoomInfo> rooms = workspace();
        auto pending = dm(QStringLiteral("!dm-invite:x"), QStringLiteral("Grace"));
        pending.membership = RoomInfo::Invited;
        rooms.append(pending);
        f.build(rooms);

        f.selectHome();
        QStringList names = namesOf(f.model);
        const int roomsAt = names.indexOf(QStringLiteral("Rooms"));
        const int directsAt = names.indexOf(QStringLiteral("Direct Messages"));
        QVERIFY2(directsAt >= 0, "Home has no Direct Messages group");
        QVERIFY2(directsAt > roomsAt, "the Direct Messages group must follow Rooms");
        QCOMPARE(kindsOf(f.model).at(directsAt), QStringLiteral("group"));
        QCOMPARE(names.mid(directsAt + 1), QStringList{ QStringLiteral("Ada") });
        QCOMPARE(f.model.data(f.model.index(directsAt, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::homeDirectsGroupId());
        QVERIFY(SpaceChannelModel::homeDirectsGroupId().startsWith(QLatin1Char('@')));
        QVERIFY2(SpaceChannelModel::homeDirectsGroupId()
                     != SpaceChannelModel::directsGroupId(),
                 "Home's group and the tab's group must collapse independently");
        QVERIFY2(!names.contains(QStringLiteral("Grace")),
                 "a DM invite belongs to the tab's Invites group, not to Home");

        // The People chip at Home: the Direct Messages group alone.
        f.model.setFilterMode(1);
        names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Ada")));
        QVERIFY(!names.contains(QStringLiteral("lounge")));
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        f.model.setFilterMode(0);

        // The tab is untouched: the complete list, invite included.
        f.selectPeople();
        names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Ada")));
        QVERIFY(names.contains(QStringLiteral("Grace")));
    }

    // Every command row carries an action id the host dispatches on and a
    // glyph name the icon font has.
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
        // A Space view has none; it has Lobby instead.
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
        // Matrix permits it, and both Spaces genuinely contain it.
        Fixture f;
        f.build({
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!shared:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!shared:x") }),
            room(QStringLiteral("!shared:x"), QStringLiteral("shared")),
        });
        // Each Space's view contains it.
        f.selectSpace(QStringLiteral("!a:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        f.selectSpace(QStringLiteral("!b:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        // ...and it is not also at Home, since a Space lists it.
        f.selectHome();
        const QStringList names = namesOf(f.model);
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        QVERIFY(!names.contains(QStringLiteral("shared")));
    }

    void aRoomWhoseOnlySpaceParentIsUnjoinedStaysReachable()
    {
        Fixture f;
        f.build({
            // The parent Space is not joined, so only the child's parent
            // pointer names it.
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
        // An invite always reads as unread: it is an action waiting on the
        // user and has no counters of its own.
        QVERIFY2(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::HasUnreadRole).toBool(),
                 "an invite reads as a quiet read row");
        // Invites come before the ordinary rooms.
        QVERIFY(invites < names.indexOf(QStringLiteral("Rooms")));

        // ...and it survives every filter chip, as in Classic.
        for (int mode = 0; mode <= 3; ++mode) {
            f.model.setFilterMode(mode);
            QVERIFY2(namesOf(f.model).contains(QStringLiteral("Newcomers")),
                     qPrintable(QStringLiteral("filter %1 hid an invite")
                                    .arg(mode)));
        }
        f.model.setFilterMode(0);

        // A collapsed Invites group still reports that something is waiting.
        f.model.toggleCollapsed(SpaceChannelModel::invitesGroupId());
        const int header = rowOfName(f.model, QStringLiteral("Invites"));
        QVERIFY(header >= 0);
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Newcomers")));
        QVERIFY2(f.model.data(f.model.index(header, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt() > 0,
                 "collapsing the Invites group hid the fact that an invite is "
                 "waiting");
    }

    // A subspace with two joined parents is listed under both. SpaceManager
    // nests it under one parent for the rail tree, but both parents' unread
    // badges count it, so both views must list it.
    void aSubspaceWithTwoParentsIsListedUnderBothOfThem()
    {
        Fixture f;
        f.client.roomList = {
            space(QStringLiteral("!p1:x"), QStringLiteral("Product"),
                  { QStringLiteral("!shared:x") }),
            space(QStringLiteral("!p2:x"), QStringLiteral("Platform"),
                  { QStringLiteral("!shared:x") }),
            space(QStringLiteral("!shared:x"), QStringLiteral("Shared"),
                  { QStringLiteral("!design:x") },
                  { QStringLiteral("!p1:x"), QStringLiteral("!p2:x") }),
            room(QStringLiteral("!design:x"), QStringLiteral("design"),
                 /*unread=*/3),
        };
        f.spaces.setClient(&f.client);
        f.model.setSettings(&f.settings);
        f.model.setSources(&f.client, &f.spaces, &f.layout);

        // Both rail tiles count the shared subspace's room...
        QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!p1:x")), 3);
        QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!p2:x")), 3);

        // ...so both views list it.
        const QStringList parents{ QStringLiteral("!p1:x"),
                                   QStringLiteral("!p2:x") };
        for (const QString &parent : parents) {
            f.selectSpace(parent);
            const QStringList names = namesOf(f.model);
            QVERIFY2(names.contains(QStringLiteral("Shared")),
                     qPrintable(QStringLiteral(
                                    "%1 counts the shared subspace on its rail "
                                    "badge and does not list it")
                                    .arg(parent)));
            QVERIFY2(names.contains(QStringLiteral("design")),
                     qPrintable(QStringLiteral(
                                    "%1's badge counts an unread room its own "
                                    "view refuses to show")
                                    .arg(parent)));
        }
    }

    // A -> B -> A is legal m.space.child state: the walk terminates via its
    // visited set, both Spaces stay reachable, each is listed once, and the
    // rooms behind the cycle are visible.
    void aCyclicSubspaceHierarchyTerminatesAndListsEachSpaceOnce()
    {
        Fixture f;
        f.client.roomList = {
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!ra:x"), QStringLiteral("!b:x") },
                  { QStringLiteral("!b:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!rb:x"), QStringLiteral("!a:x") },
                  { QStringLiteral("!a:x") }),
            room(QStringLiteral("!ra:x"), QStringLiteral("ra")),
            room(QStringLiteral("!rb:x"), QStringLiteral("rb"), /*unread=*/2),
        };
        f.spaces.setClient(&f.client);
        f.model.setSettings(&f.settings);
        f.model.setSources(&f.client, &f.spaces, &f.layout);
        f.selectSpace(QStringLiteral("!a:x"));

        const QStringList names = namesOf(f.model);
        QCOMPARE(names.count(QStringLiteral("Alpha")), 1);
        QCOMPARE(names.count(QStringLiteral("Beta")), 1);
        QVERIFY(names.contains(QStringLiteral("ra")));
        QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!a:x")), 2);
        QVERIFY2(names.contains(QStringLiteral("rb")),
                 "the other half of the cycle is on Alpha's rail badge and "
                 "missing from Alpha's view");
    }

    // The selected Space heads its own view, whatever the rail order says. The
    // rail only ranks roots, so a subspace's order falls back to the room
    // list's, which may put it before its parent. No layout order is set: a
    // fresh account has none.
    void theSelectedSpaceHeadsItsOwnViewNotItsSubspaces()
    {
        Fixture f;
        // The subspace first in the room list.
        f.client.roomList = {
            space(QStringLiteral("!eng:x"), QStringLiteral("Engineering"),
                  { QStringLiteral("!backend:x") },
                  { QStringLiteral("!work:x") }),
            space(QStringLiteral("!work:x"), QStringLiteral("Work"),
                  { QStringLiteral("!general:x"), QStringLiteral("!eng:x") }),
            room(QStringLiteral("!general:x"), QStringLiteral("general")),
            room(QStringLiteral("!backend:x"), QStringLiteral("backend")),
        };
        f.spaces.setClient(&f.client);
        f.model.setSettings(&f.settings);
        f.model.setSources(&f.client, &f.spaces, &f.layout);
        f.selectSpace(QStringLiteral("!work:x"));

        const QStringList names = namesOf(f.model);
        const int work = names.indexOf(QStringLiteral("Work"));
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        QVERIFY2(work >= 0, "the selected Space is not in its own view");
        QVERIFY2(eng >= 0, "the subspace is not in its parent's view");
        QVERIFY2(work < eng,
                 "the selected Space's own folder was ranked BELOW its "
                 "subspace: the rail's order does not rank subspaces, so it "
                 "cannot say where the selection goes among them");
        QVERIFY2(names.indexOf(QStringLiteral("general"))
                     < names.indexOf(QStringLiteral("backend")),
                 "the rooms of the Space the user clicked are below the "
                 "subspace's rooms");
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

        // A Space's view is the Space then its subspaces, ranked by the rail's
        // arrangement rather than hierarchy walk order.
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

    // Clicking a Space in the rail narrows the column to it and its
    // subspaces.
    void selectingASpaceNarrowsTheColumnToItAndItsSubspaces()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));

        f.model.setScopeSpaceId(QStringLiteral("!work:x"));
        const QStringList names = namesOf(f.model);
        // The Space and its subspace, without the account-wide groups.
        QVERIFY(names.contains(QStringLiteral("Work")));
        QVERIFY2(names.contains(QStringLiteral("Engineering")),
                 "a subspace of the selected Space is missing");
        QVERIFY2(!names.contains(QStringLiteral("Rooms")),
                 "the account-wide Rooms group survived the scope");
        QVERIFY2(!names.contains(QStringLiteral("lounge")),
                 "an unparented room survived into a Space's own view");
        // No DM here: DMs have their own tab.
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM appeared under a Space, which Matrix cannot express");
        QVERIFY2(!names.contains(QStringLiteral("Direct messages")),
                 "the account-wide DM group survived into a Space's view");
        QVERIFY(names.contains(QStringLiteral("general")));
        QVERIFY(names.contains(QStringLiteral("backend")));
        // A subspace is still a flat folder, not a level.
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        // Lobby is one row away, so the scope is escapable.
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
        // ...and the account is not claimed to be empty.
        QVERIFY(!f.model.empty());

        // Home is one rail tile away.
        f.selectHome();
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("lounge")));
    }

    void aPseudoRailRowThatIsNotTheDmTabIsHome()
    {
        // "" (Home) and "@orphans" (Other rooms) are neither a Space nor the
        // DM tab, so both produce the Home view: an unrecognised selection
        // lands somewhere that lists something.
        Fixture f;
        f.build(workspace());
        f.selectHome();
        const int home = f.model.rowCount();
        QVERIFY(home > 0);
        f.model.setScopeSpaceId(SpaceManager::orphansId());
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("home"));
        QCOMPARE(f.model.rowCount(), home);
        // The DM tab is the one pseudo id that is not Home.
        f.selectPeople();
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("people"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
    }

    void aSelectionOnASpaceTheAccountNoLongerHasStaysThatSpace()
    {
        // A Space left while selected stays the selection and renders its own
        // emptiness; falling back would show a different view under a tile
        // that no longer exists.
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
        // Lobby is still there, so the view is navigable...
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
        // ...and the account is not claimed to be empty.
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

        // Expanded, the header reports nothing: the rows carry their own
        // badges.
        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        const int engOpen = rowOfName(f.model, QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(engOpen, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 0);
    }

    void collapseStateSurvivesARebuildAndAReload()
    {
        // Collapse state is stored, since rows are rebuilt on every arriving
        // message.
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
        // Navigation rows step aside while searching; they match nothing.
        QVERIFY(!found.contains(QStringLiteral("Lobby")));
        QVERIFY(!found.contains(QStringLiteral("Message Search")));
        // A folder with no match is dropped.
        QVERIFY(!found.contains(QStringLiteral("Rooms")));

        // Clearing restores exactly what was collapsed: filtering must not
        // mutate collapse state.
        f.model.setSearchQuery(QString());
        QVERIFY(f.model.isCollapsed(QStringLiteral("!eng:x")));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("backend")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));
    }

    // The filter chips work within a view. Channels only offers All and
    // Unreads, but the model keeps every mode because it is shared with
    // Classic.
    void theFilterChipsSelectRoomsWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

        f.model.setFilterMode(3);   // Unreads
        const QStringList unread = namesOf(f.model);
        QVERIFY(unread.contains(QStringLiteral("backend")));
        QVERIFY(!unread.contains(QStringLiteral("general")));
        // A filter that matched little is not an empty account.
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        f.model.setFilterMode(0);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));

        // People and Rooms modes still apply where both kinds are present.
        f.selectPeople();
        f.model.setFilterMode(1);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        f.model.setFilterMode(2);
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("Ada")),
                 "the Rooms mode kept a DM");
    }

    // Whatever the rail last selected, every DM is one tile away and the tab
    // lists all of them.
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

    // `empty` and `matchCount` answer different questions: "you have no
    // conversations" versus "this filter found none".
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

        // A search that matches nothing reports the same way.
        f.model.setFilterMode(0);
        f.model.setSearchQuery(QStringLiteral("zzzz"));
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY(!f.model.empty());

        // ...and the count recovers when something matches again.
        f.model.setSearchQuery(QStringLiteral("gen"));
        QVERIFY(f.model.matchCount() > 0);
    }

    // `matchCount` notifies on its own: it can change without the row count
    // changing, so a binding on countChanged alone would miss it.
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
        // Action rows carry a synthetic '@' id, never a room id, so nothing
        // tries to open them as rooms.
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
        // A folder is never "the room you are in".
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!work:x")), -1);
        QCOMPARE(f.model.rowForRoom(SpaceChannelModel::roomsGroupId()), -1);
        QCOMPARE(f.model.rowForRoom(QString()), -1);
    }

    void unreadStateChangesInPlaceRatherThanResettingTheWholeColumn()
    {
        // Unread changes are dataChanged, not a reset: a reset rebuilds every
        // delegate and its avatar fetch.
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&f.model, &QAbstractItemModel::dataChanged);

        f.client.roomList[1].unreadCount = 7;
        f.client.roomList[1].hasUnreadMessages = true;
        f.client.announce();

        // The rebuild is coalesced to one per event-loop turn, so wait for it.
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
        // Home keeps its command rows: an empty account most needs Create Room
        // and Join with Address.
        QCOMPARE(kindsOf(f.model),
                 QStringList({ QStringLiteral("action"),
                               QStringLiteral("action"),
                               QStringLiteral("action") }));
        QCOMPARE(f.model.matchCount(), 0);
        // A Space's view has no such rows.
        f.selectSpace(QStringLiteral("!nothing:x"));
        QCOMPARE(kindsOf(f.model), QStringList{ QStringLiteral("lobby") });
    }

    void anAccountChangeMakesTheCollapseStateBeReReadNotKept()
    {
        // The collapse set is account-scoped, so an account change drops the
        // in-memory copy and re-reads. Proven by changing the stored value
        // under the model and then announcing the change.
        Fixture f;
        f.build(workspace());
        f.model.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        SpaceChannelModel other;
        other.setSettings(&f.settings);
        other.setSources(&f.client, &f.spaces, &f.layout);
        other.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(!other.isCollapsed(QStringLiteral("!work:x")));
        // The model has not noticed yet: it still reads its cache.
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        f.client.logout();
        QVERIFY2(!f.model.isCollapsed(QStringLiteral("!work:x")),
                 "the in-memory collapse set survived an account change, so "
                 "the previous account's collapsed folders describe the next "
                 "account's rooms");
    }

    // A DM with no room avatar wears the peer's face, derived as the Classic
    // list does (unambiguous 1:1, the peer, their known picture).
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

    // A peer with no avatar (or whose lookup failed) is asked about exactly
    // once: the resolver remembers negative answers and announces only faces
    // it learned, or every rebuild would re-request it.
    void anAvatarlessPeerIsAskedForExactlyOnce()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({ peer });
        f.selectPeople();
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

        // A real homeserver's answer for a user with no avatar: ok, a display
        // name, and an empty avatar url.
        for (int i = 0; i < 4; ++i) {
            Q_EMIT f.client.userProfileFinished(
                f.client.nextOp, true, QStringLiteral("@sam:example.org"),
                QStringLiteral("Sam"), QString(), QString());
            QCoreApplication::processEvents();
        }
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

        // The same for a lookup that failed.
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

    // A burst of room updates in one event-loop turn costs one rebuild.
    // Asserted as a delta, since setup itself rebuilds several times.
    void aBurstOfRoomUpdatesCostsOneRebuild()
    {
        Fixture f;
        f.build(workspace());
        QCoreApplication::processEvents();
        // Counted on the model: the client's rooms() is also called by
        // SpaceManager's own rebuild.
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

    // A late peer profile reaches the row: rows hold a snapshot, so a bare
    // dataChanged would repaint the same initials. On the Rust backend the
    // profile is the only route to a DM's face.
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
        // The row did not move.
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!dm:x")), row);
    }

    // A group DM (`m.direct` naming two targets) borrows nobody's face.
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

    // A coalesced rebuild never outlives the state that armed it: after the
    // sources change or the session ends, nothing queued runs.
    void aQueuedRebuildNeverOutlivesTheStateThatArmedIt()
    {
        // The sources are replaced; the rebuild queued for the old ones must
        // not run at all.
        Fixture f;
        f.build(workspace());
        // qWait, not processEvents: the coalescer is a zero-interval QTimer,
        // which processEvents() does not reliably fire.
        QTest::qWait(50);
        f.client.announce();
        f.model.setSources(nullptr, nullptr, nullptr);
        int settled = f.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(f.model.rebuildCountForTest(), settled);

        // The sign-out shape, with the SpaceManager left clientless: it
        // re-arms a legitimate rebuild after logout, which no count could tell
        // from the stale one.
        Fixture g;
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        g.client.roomList = { peer };
        g.model.setSettings(&g.settings);
        g.model.setSources(&g.client, &g.spaces, &g.layout);
        QTest::qWait(50);
        // A late profile arms a rebuild.
        Q_EMIT g.client.userProfileFinished(
            g.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        g.client.logout();
        settled = g.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(g.model.rebuildCountForTest(), settled);
    }

    // A destroyed source leaves a null, not a dangling pointer: AppController
    // destroys SpaceManager before this model. On unfixed code this is a
    // use-after-free that usually passes and fails reliably only under ASan.
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
        // Any rebuild: a filter chip is the cheapest one.
        model.setFilterMode(1);
        QCOMPARE(model.rowCount(), 0);
        QTest::qWait(50);
        QCOMPARE(model.rowCount(), 0);
    }

    // A failed profile lookup someone else started never marks a DM peer as
    // pictureless: a failure is about one request, not the user.
    // `userProfileFinished` is shared by every consumer of the client.
    void aFailedLookupSomebodyElseStartedNeverWedgesADm()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({});
        f.selectPeople();
        // An op this model never issued reports a failure for that user.
        Q_EMIT f.client.userProfileFinished(
            9999, false, QStringLiteral("@sam:example.org"), QString(),
            QString(), QStringLiteral("timeout"));
        QTest::qWait(50);
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 0);

        // The DM arrives, and its peer is looked up.
        f.client.roomList = { peer };
        f.client.announce();
        QTRY_COMPARE_WITH_TIMEOUT(
            f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1,
            2000);

        // The answer reaches the row.
        Q_EMIT f.client.userProfileFinished(
            f.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        QTRY_COMPARE(f.model.data(f.model.index(row, 0),
                                  SpaceChannelModel::AvatarUrlRole).toString(),
                     QStringLiteral("mxc://example.org/sam"));

        // Our own failures are still remembered (see
        // anAvatarlessPeerIsAskedForExactlyOnce).
    }

    // A face returned under a normalised user id is cached under the id we
    // asked with, which is what both owners look it up by. Structural: no
    // real normalisation has been captured, but the failure would be silent.
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


// Activity ordering: rooms sort by recency within their group. Other cases
// leave lastActivity invalid, so the comparator falls back to names there.

void SpaceChannelsTest::homeRoomsAreNewestFirstNotAlphabetical()
{
    Fixture f;
    // Named so alphabetical and recency orders disagree.
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

// Within its group a favourite sorts first and recency decides the rest.
// Named so recency alone gives the opposite order.
void SpaceChannelsTest::aFavouriteRisesToTheTopOfItsGroup()
{
    Fixture f;
    auto quiet = room(QStringLiteral("!quiet:x"), QStringLiteral("Quiet"));
    quiet.lastActivity = ago(900);
    quiet.isFavourite = true;
    auto busy = room(QStringLiteral("!busy:x"), QStringLiteral("Busy"));
    busy.lastActivity = ago(10);
    auto oldDm = dm(QStringLiteral("!olddm:x"), QStringLiteral("Old friend"));
    oldDm.lastActivity = ago(900);
    oldDm.isFavourite = true;
    auto newDm = dm(QStringLiteral("!newdm:x"), QStringLiteral("New friend"));
    newDm.lastActivity = ago(10);
    f.build({ quiet, busy, oldDm, newDm });

    f.selectHome();
    QStringList names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Quiet")) < names.indexOf(QStringLiteral("Busy")),
             "a favourite room did not rise above a busier one at Home");
    QVERIFY2(names.indexOf(QStringLiteral("Old friend"))
                 < names.indexOf(QStringLiteral("New friend")),
             "a favourite DM did not rise above a busier one in Home's group");

    f.selectPeople();
    names = namesOf(f.model);
    QVERIFY(names.indexOf(QStringLiteral("Old friend"))
            < names.indexOf(QStringLiteral("New friend")));
}

void SpaceChannelsTest::aSpacesRoomsAreNewestFirstNotInChildOrder()
{
    Fixture f;
    // m.space.child order puts the stale room first, so following it fails.
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
    // The Space is still the group header above its rooms.
    QVERIFY(names.indexOf(QStringLiteral("Work"))
            < names.indexOf(QStringLiteral("General")));
}

void SpaceChannelsTest::aRoomMovesWhenSomebodySpeaksInIt()
{
    // lastActivity is carried on the row: applyRows diffs rows by value, so a
    // sort key the row does not hold cannot be seen to change.
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
    // Source signals coalesce onto a zero-timer; settle before asserting.
    QCoreApplication::processEvents();

    names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Zulu"))
                 < names.indexOf(QStringLiteral("Alpha")),
             "the row did not move when the room received a message");
}

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
