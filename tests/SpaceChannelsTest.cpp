#include "matrix/RoomUnreadRule.h"
#include "models/DirectAvatarResolver.h"
#include "models/QuickSwitcherModel.h"
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

    quint64 fetchUserProfile(const QString &userId) override
    {
        profileFetches.append(userId);
        return ++nextOp;
    }
    QString lastOrderSpace;
    QStringList lastOrderIds;
    quint64 setSpaceChildOrder(const QString &spaceId, const QStringList &ids) override
    {
        lastOrderSpace = spaceId;
        lastOrderIds = ids;
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

int railUnreadTotal(const SpaceManager &spaces, const QString &spaceId)
{
    for (const QVariant &value : spaces.allSpaces()) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("spaceId")).toString() == spaceId)
            return entry.value(QStringLiteral("unreadTotal")).toInt();
    }
    return -1;
}

}

class SpaceChannelsTest : public QObject
{
    Q_OBJECT

private:
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
4),
            room(QStringLiteral("!frontend:x"), QStringLiteral("frontend"),
0,2),
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
        void selectHome() { model.setScopeSpaceId(SpaceManager::allRoomsId()); }
        void selectPeople() { model.setScopeSpaceId(SpaceManager::peopleId()); }
        void selectSpace(const QString &id) { model.setScopeSpaceId(id); }
    };

private Q_SLOTS:
    void sharedOrderWritesOnlyTheCompleteDirectChildSet()
    {
        Fixture f;
        f.build(workspace());
        const QString spaceId = QStringLiteral("!work:x");
        const auto rows = f.spaces.defaultOrderChildren(spaceId);
        QCOMPARE(rows.size(), 3);
        QStringList ids;
        for (const auto &row : rows)
            ids.prepend(row.toMap().value(QStringLiteral("roomId")).toString());
        QSignalSpy finished(&f.spaces, &SpaceManager::childOrderFinished);
        f.spaces.setDefaultChildOrder(spaceId, {ids.first(), ids.first(), ids.last()});
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.last().at(1).toBool());
        QVERIFY(f.client.lastOrderIds.isEmpty());
        f.spaces.setDefaultChildOrder(spaceId, ids);
        QCOMPARE(f.client.lastOrderSpace, spaceId);
        QCOMPARE(f.client.lastOrderIds, ids);
        QCOMPARE(finished.size(), 1);
        const auto op = f.client.nextOp;
        Q_EMIT f.client.spaceChildOrderFinished(op + 1, spaceId, true);
        QCOMPARE(finished.size(), 1);
        Q_EMIT f.client.spaceChildOrderFinished(op, spaceId, false);
        QCOMPARE(finished.size(), 2);
        QVERIFY(!finished.last().at(1).toBool());
        f.spaces.setDefaultChildOrder(spaceId, ids);
        const auto abandoned = f.client.nextOp;
        Q_EMIT f.client.loggedOut();
        QCOMPARE(finished.size(), 3);
        QVERIFY(!finished.last().at(1).toBool());
        Q_EMIT f.client.spaceChildOrderFinished(abandoned, spaceId, true);
        QCOMPARE(finished.size(), 3);
    }

    void anAddAccountSignInAbandonsAPendingOrderSave()
    {
        Fixture f;
        f.build(workspace());
        const QString spaceId = QStringLiteral("!work:x");
        QStringList ids;
        for (const auto &row : f.spaces.defaultOrderChildren(spaceId))
            ids.append(row.toMap().value(QStringLiteral("roomId")).toString());
        QSignalSpy finished(&f.spaces, &SpaceManager::childOrderFinished);
        f.spaces.setDefaultChildOrder(spaceId, ids);
        QCOMPARE(finished.size(), 0);
        f.spaces.abandonPendingChildOps();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.last().at(1).toBool());
        f.spaces.setDefaultChildOrder(spaceId, ids);
        QVERIFY(f.spaces.childOrderPending(spaceId));
    }

    void aParentOnlyChildIsNotOfferedOrWritten()
    {
        Fixture f;
        auto rooms = workspace();
        const QString spaceId = QStringLiteral("!work:x");
        for (auto &room : rooms) {
            if (room.id != spaceId) continue;
            room.childOrderKnown = true;
            room.orderedChildIds = { QStringLiteral("!general:x"), QStringLiteral("!random:x") };
        }
        f.build(rooms);
        QStringList ids;
        for (const auto &row : f.spaces.defaultOrderChildren(spaceId))
            ids.append(row.toMap().value(QStringLiteral("roomId")).toString());
        QCOMPARE(ids, (QStringList{QStringLiteral("!general:x"), QStringLiteral("!random:x")}));
        QSignalSpy finished(&f.spaces, &SpaceManager::childOrderFinished);
        f.spaces.setDefaultChildOrder(spaceId, {QStringLiteral("!general:x"),
            QStringLiteral("!random:x"), QStringLiteral("!eng:x")});
        QCOMPARE(finished.size(), 1);
        QVERIFY(f.client.lastOrderIds.isEmpty());
        f.spaces.setDefaultChildOrder(spaceId, ids);
        QCOMPARE(f.client.lastOrderIds, ids);
    }

    void favouritesFilterWorksInChannels()
    {
        Fixture f;
        auto rooms = workspace();
        for (auto &room : rooms)
            if (room.id == QStringLiteral("!random:x")) room.isFavourite = true;
        f.build(rooms);
        f.selectSpace(QStringLiteral("!work:x"));
        f.model.setFilterMode(4);
        QCOMPARE(f.model.filterMode(), 4);
        QCOMPARE(f.model.matchCount(), 1);
        f.model.setFilterMode(0);
        QVERIFY(f.model.matchCount() > 1);
    }

    void personalArrangementWinsInsideASpace()
    {
        Fixture f;
        f.build(workspace());
        f.spaces.setPersonalRoomOrder([](const QString &, const QStringList &ids) {
            QStringList reversed = ids;
            std::reverse(reversed.begin(), reversed.end());
            return reversed;
        });
        f.model.setSortMode(conversation::SortBySpaceOrder);
        f.selectSpace(QStringLiteral("!work:x"));
        QStringList children;
        for (int row = 0; row < f.model.rowCount(); ++row) {
            const auto ix = f.model.index(row);
            const auto id = f.model.data(ix, SpaceChannelModel::RoomIdRole).toString();
            if (id == QStringLiteral("!general:x") || id == QStringLiteral("!random:x"))
                children.append(id);
        }
        QCOMPARE(children, (QStringList{QStringLiteral("!random:x"), QStringLiteral("!general:x")}));
    }

    void homeRoomsAreNewestFirstNotAlphabetical();
    void directMessageChatsAreNewestFirst();
    void aSpacesRoomsAreNewestFirstNotInChildOrder();
    void aRoomMovesWhenSomebodySpeaksInIt();
    void aFavouriteRisesToTheTopOfItsGroup();
    void lowPriorityTagReachesTheChannelMenu();
    void lowPriorityFollowsOrdinaryRoomsInEverySortMode();
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
            client.roomList = workspace();
            client.announce();
            QCOMPARE(spaces.activeSpaceId(), sentinel);
        }

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

    void theRailSelectionChoosesOneOfThreeViews()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);

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
        QVERIFY2(names.contains(QStringLiteral("Ada")),
                 "Home lost its Direct Messages group");
        QVERIFY(names.contains(QStringLiteral("Rooms")));
        QVERIFY(names.contains(QStringLiteral("lounge")));

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
        QCOMPARE(f.model.data(f.model.index(work, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);

        QCOMPARE(names.mid(work + 1, 2),
                 QStringList({ QStringLiteral("general"),
                               QStringLiteral("random") }));
        QCOMPARE(names.count(QStringLiteral("backend")), 1);
        QCOMPARE(names.count(QStringLiteral("frontend")), 1);
        QVERIFY(eng > work);
        QCOMPARE(names.mid(eng + 1, 2),
                 QStringList({ QStringLiteral("backend"),
                               QStringLiteral("frontend") }));
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
        QCOMPARE(names.mid(rooms + 1, 1), QStringList{ QStringLiteral("lounge") });
        QVERIFY2(!names.contains(QStringLiteral("Favourites")),
                 "the Favourites group came back");
    }

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
        QVERIFY(SpaceChannelModel::directsGroupId()
                    .startsWith(QLatin1Char('@')));
        QVERIFY(SpaceChannelModel::peopleViewId()
                    .startsWith(QLatin1Char('@')));
        QCOMPARE(f.model.data(f.model.index(chats, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::directsGroupId());

        f.selectHome();
        names = namesOf(f.model);
        QCOMPARE(names.mid(names.indexOf(QStringLiteral("Rooms")) + 1, 1),
                 QStringList{ QStringLiteral("lounge") });
        QVERIFY(names.contains(QStringLiteral("Ada")));
        f.selectSpace(QStringLiteral("!work:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Ada")));
    }

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

        f.model.setFilterMode(1);
        names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Ada")));
        QVERIFY(!names.contains(QStringLiteral("lounge")));
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        f.model.setFilterMode(0);

        f.selectPeople();
        names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Ada")));
        QVERIFY(names.contains(QStringLiteral("Grace")));
    }

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
        Fixture f;
        f.build({
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                  { QStringLiteral("!shared:x") }),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                  { QStringLiteral("!shared:x") }),
            room(QStringLiteral("!shared:x"), QStringLiteral("shared")),
        });
        f.selectSpace(QStringLiteral("!a:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        f.selectSpace(QStringLiteral("!b:x"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("shared")));
        f.selectHome();
        const QStringList names = namesOf(f.model);
        QVERIFY(!names.contains(QStringLiteral("Rooms")));
        QVERIFY(!names.contains(QStringLiteral("shared")));
    }

    void aRoomWhoseOnlySpaceParentIsUnjoinedStaysReachable()
    {
        Fixture f;
        f.build({
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
        QVERIFY2(f.model.data(f.model.index(row, 0),
                              SpaceChannelModel::HasUnreadRole).toBool(),
                 "an invite reads as a quiet read row");
        QVERIFY(invites < names.indexOf(QStringLiteral("Rooms")));

        for (int mode = 0; mode <= 3; ++mode) {
            f.model.setFilterMode(mode);
            QVERIFY2(namesOf(f.model).contains(QStringLiteral("Newcomers")),
                     qPrintable(QStringLiteral("filter %1 hid an invite")
                                    .arg(mode)));
        }
        f.model.setFilterMode(0);

        f.model.toggleCollapsed(SpaceChannelModel::invitesGroupId());
        const int header = rowOfName(f.model, QStringLiteral("Invites"));
        QVERIFY(header >= 0);
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("Newcomers")));
        QVERIFY2(f.model.data(f.model.index(header, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt() > 0,
                 "collapsing the Invites group hid the fact that an invite is "
                 "waiting");
    }

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
3),
        };
        f.spaces.setClient(&f.client);
        f.model.setSettings(&f.settings);
        f.model.setSources(&f.client, &f.spaces, &f.layout);

        QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!p1:x")), 3);
        QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!p2:x")), 3);

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
            room(QStringLiteral("!rb:x"), QStringLiteral("rb"),2),
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

    void theSelectedSpaceHeadsItsOwnViewNotItsSubspaces()
    {
        Fixture f;
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
        const QString folder = f.layout.createFolder(QStringLiteral("Work"));
        f.layout.setSpaceFolder(QStringLiteral("!c:x"), folder);
        f.layout.setTopLevelOrder({ QStringLiteral("!b:x"), folder,
                                    QStringLiteral("!a:x") });
        f.model.setSources(&f.client, &f.spaces, &f.layout);

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

    void selectingASpaceNarrowsTheColumnToItAndItsSubspaces()
    {
        Fixture f;
        f.build(workspace());
        f.model.setMessageSearchSupported(true);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));

        f.model.setScopeSpaceId(QStringLiteral("!work:x"));
        const QStringList names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("Work")));
        QVERIFY2(names.contains(QStringLiteral("Engineering")),
                 "a subspace of the selected Space is missing");
        QVERIFY2(!names.contains(QStringLiteral("Rooms")),
                 "the account-wide Rooms group survived the scope");
        QVERIFY2(!names.contains(QStringLiteral("lounge")),
                 "an unparented room survived into a Space's own view");
        QVERIFY2(!names.contains(QStringLiteral("Ada")),
                 "a DM appeared under a Space, which Matrix cannot express");
        QVERIFY2(!names.contains(QStringLiteral("Direct messages")),
                 "the account-wide DM group survived into a Space's view");
        QVERIFY(names.contains(QStringLiteral("general")));
        QVERIFY(names.contains(QStringLiteral("backend")));
        const int eng = names.indexOf(QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::DepthRole).toInt(), 0);
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
        QVERIFY(!f.model.empty());

        f.selectHome();
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Rooms")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("lounge")));
    }

    void aPseudoRailRowThatIsNotTheDmTabIsHome()
    {
        Fixture f;
        f.build(workspace());
        f.selectHome();
        const int home = f.model.rowCount();
        QVERIFY(home > 0);
        f.model.setScopeSpaceId(SpaceManager::orphansId());
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("home"));
        QCOMPARE(f.model.rowCount(), home);
        f.selectPeople();
        QCOMPARE(f.model.scopeSpaceId(), QString());
        QCOMPARE(f.model.viewKind(), QStringLiteral("people"));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
    }

    void aSelectionOnASpaceTheAccountNoLongerHasStaysThatSpace()
    {
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
        QCOMPARE(names.at(0), QStringLiteral("Lobby"));
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
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 4);
        QCOMPARE(f.model.data(f.model.index(eng, 0),
                              SpaceChannelModel::HiddenHighlightRole).toInt(),
                 2);
        QVERIFY(f.model.data(f.model.index(eng, 0),
                             SpaceChannelModel::CollapsedRole).toBool());

        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        const int engOpen = rowOfName(f.model, QStringLiteral("Engineering"));
        QCOMPARE(f.model.data(f.model.index(engOpen, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 0);
    }

    void importantRoomsRemainReachableInACollapsedGroup()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
        f.model.setOpenRoomId(QStringLiteral("!general:x"));
        f.model.setCallRoomId(QStringLiteral("!random:x"));
        f.model.toggleCollapsed(QStringLiteral("!work:x"));

        QVERIFY(!f.model.keepImportantRoomsVisible());
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!general:x")), -1);
        f.model.setKeepImportantRoomsVisible(true);
        const QStringList names = namesOf(f.model);
        QVERIFY(names.contains(QStringLiteral("general")));
        QVERIFY(names.contains(QStringLiteral("random")));
        QVERIFY(names.contains(QStringLiteral("backend")));
        QVERIFY(names.contains(QStringLiteral("frontend")));
        QCOMPARE(names.count(QStringLiteral("general")), 1);
        const int header = rowOfName(f.model, QStringLiteral("Work"));
        QVERIFY(f.model.data(f.model.index(header, 0),
                             SpaceChannelModel::CollapsedRole).toBool());
        QCOMPARE(f.model.data(f.model.index(header, 0),
                              SpaceChannelModel::HiddenUnreadRole).toInt(), 0);
        QCOMPARE(f.model.data(f.model.index(header, 0),
                              SpaceChannelModel::HiddenHighlightRole).toInt(), 0);

        f.model.setRoomHovered(QStringLiteral("!random:x"), true);
        f.model.setCallRoomId(QString());
        QVERIFY(f.model.rowForRoom(QStringLiteral("!random:x")) >= 0);
        f.model.setRoomHovered(QStringLiteral("!random:x"), false);
        QCoreApplication::processEvents();
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!random:x")), -1);
        f.selectHome();
        f.selectSpace(QStringLiteral("!work:x"));
        QVERIFY(f.model.rowForRoom(QStringLiteral("!general:x")) >= 0);

        f.model.setKeepImportantRoomsVisible(false);
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!general:x")), -1);
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));
    }

    void importantRoomChoiceIsAccountScoped()
    {
        Fixture f;
        const QString alice = QStringLiteral("@alice:example.org");
        const QString bob = QStringLiteral("@bob:example.org");
        f.settings.saveSession(QStringLiteral("https://example.org"), alice,
                               QStringLiteral("Alice"), QStringLiteral("test-token-a"));
        f.settings.saveSession(QStringLiteral("https://example.org"), bob,
                               QStringLiteral("Bob"), QStringLiteral("test-token-b"));
        f.settings.setActiveAccountUserId(alice);
        f.build(workspace());
        f.model.setKeepImportantRoomsVisible(true);
        f.settings.setActiveAccountUserId(bob);
        QVERIFY(!f.model.keepImportantRoomsVisible());
        f.settings.setActiveAccountUserId(alice);
        QVERIFY(f.model.keepImportantRoomsVisible());
    }

    void theAttentionLocatorFindsUnreadRoomsAndCollapsedFolders()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

        const int backend = rowOfName(f.model, QStringLiteral("backend"));
        const int frontend = rowOfName(f.model, QStringLiteral("frontend"));
        const int general = rowOfName(f.model, QStringLiteral("general"));
        QVERIFY(backend >= 0 && frontend >= 0 && general >= 0);
        const int last = f.model.rowCount() - 1;

        QCOMPARE(f.model.attentionRowFrom(0, 1), qMin(backend, frontend));
        QCOMPARE(f.model.attentionRowFrom(0, 1, true), frontend);
        QCOMPARE(f.model.attentionRowFrom(last, -1),
                 qMax(backend, frontend));
        QCOMPARE(f.model.attentionRowFrom(qMin(backend, frontend) - 1, -1), -1);
        QCOMPARE(f.model.attentionRowFrom(last + 5, 1), -1);
        QCOMPARE(f.model.attentionRowFrom(0, 0), -1);

        f.model.toggleCollapsed(QStringLiteral("!eng:x"));
        const int eng = rowOfName(f.model, QStringLiteral("Engineering"));
        QVERIFY(eng >= 0);
        QCOMPARE(f.model.attentionRowFrom(0, 1), eng);
        QCOMPARE(f.model.attentionRowFrom(0, 1, true), eng);
        QCOMPARE(f.model.attentionRowFrom(f.model.rowCount() - 1, -1), eng);
    }

    void collapseStateSurvivesARebuildAndAReload()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
        f.model.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("general")));

        f.client.roomList[1].unreadCount = 3;
        f.client.announce();
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("general")),
                 "the collapse was lost on a rebuild");

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
        QVERIFY(!found.contains(QStringLiteral("Lobby")));
        QVERIFY(!found.contains(QStringLiteral("Message Search")));
        QVERIFY(!found.contains(QStringLiteral("Rooms")));

        f.model.setSearchQuery(QString());
        QVERIFY(f.model.isCollapsed(QStringLiteral("!eng:x")));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("backend")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));
    }

    void theFilterChipsSelectRoomsWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));

        f.model.setFilterMode(3);
        const QStringList unread = namesOf(f.model);
        QVERIFY(unread.contains(QStringLiteral("backend")));
        QVERIFY(!unread.contains(QStringLiteral("general")));
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        f.model.setFilterMode(0);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("general")));

        f.selectPeople();
        f.model.setFilterMode(1);
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        f.model.setFilterMode(2);
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("Ada")),
                 "the Rooms mode kept a DM");
    }

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
            QVERIFY(!names.contains(QStringLiteral("general")));
            QVERIFY(!names.contains(QStringLiteral("backend")));
            QVERIFY(!names.contains(QStringLiteral("lounge")));
            QVERIFY(!names.contains(QStringLiteral("Work")));
        }
    }

    void aFilterThatMatchedNothingIsCountedWithoutClaimingTheAccountIsEmpty()
    {
        Fixture f;
        f.build({
            space(QStringLiteral("!work:x"), QStringLiteral("Work"),
                  { QStringLiteral("!general:x") }),
            room(QStringLiteral("!general:x"), QStringLiteral("general")),
        });
        f.selectSpace(QStringLiteral("!work:x"));
        QVERIFY(!f.model.empty());
        QVERIFY(f.model.matchCount() > 0);

        f.model.setFilterMode(1);
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY2(!f.model.empty(),
                 "a filter's result was reported as the account having "
                 "nothing");

        f.model.setFilterMode(0);
        f.model.setSearchQuery(QStringLiteral("zzzz"));
        QCOMPARE(f.model.matchCount(), 0);
        QVERIFY(!f.model.empty());

        f.model.setSearchQuery(QStringLiteral("gen"));
        QVERIFY(f.model.matchCount() > 0);
    }

    void theMatchCountAnnouncesItselfWhenTheFilterChanges()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
        QSignalSpy matches(&f.model, &SpaceChannelModel::matchCountChanged);
        f.model.setFilterMode(1);
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
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!work:x")), -1);
        QCOMPARE(f.model.rowForRoom(SpaceChannelModel::roomsGroupId()), -1);
        QCOMPARE(f.model.rowForRoom(QString()), -1);
    }

    void unreadStateChangesInPlaceRatherThanResettingTheWholeColumn()
    {
        Fixture f;
        f.build(workspace());
        f.selectSpace(QStringLiteral("!work:x"));
        QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&f.model, &QAbstractItemModel::dataChanged);

        f.client.roomList[1].unreadCount = 7;
        f.client.roomList[1].hasUnreadMessages = true;
        f.client.announce();

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
        QCOMPARE(kindsOf(f.model),
                 QStringList({ QStringLiteral("action"),
                               QStringLiteral("action"),
                               QStringLiteral("action") }));
        QCOMPARE(f.model.matchCount(), 0);
        f.selectSpace(QStringLiteral("!nothing:x"));
        QCOMPARE(kindsOf(f.model), QStringList{ QStringLiteral("lobby") });
    }

    void anAccountChangeMakesTheCollapseStateBeReReadNotKept()
    {
        Fixture f;
        f.build(workspace());
        f.model.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        SpaceChannelModel other;
        other.setSettings(&f.settings);
        other.setSources(&f.client, &f.spaces, &f.layout);
        other.toggleCollapsed(QStringLiteral("!work:x"));
        QVERIFY(!other.isCollapsed(QStringLiteral("!work:x")));
        QVERIFY(f.model.isCollapsed(QStringLiteral("!work:x")));

        f.client.logout();
        QVERIFY2(!f.model.isCollapsed(QStringLiteral("!work:x")),
                 "the in-memory collapse set survived an account change, so "
                 "the previous account's collapsed folders describe the next "
                 "account's rooms");
    }

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

    void anAvatarlessPeerIsAskedForExactlyOnce()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({ peer });
        f.selectPeople();
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

        for (int i = 0; i < 4; ++i) {
            Q_EMIT f.client.userProfileFinished(
                f.client.nextOp, true, QStringLiteral("@sam:example.org"),
                QStringLiteral("Sam"), QString(), QString());
            QCoreApplication::processEvents();
        }
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);

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

    void aBurstOfRoomUpdatesCostsOneRebuild()
    {
        Fixture f;
        f.build(workspace());
        QCoreApplication::processEvents();
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
        QTRY_COMPARE(avatar(), QStringLiteral("mxc://example.org/sam"));
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!dm:x")), row);
    }

    void aPeersChangedFaceReachesTheRowWithoutAskingAgain()
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
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);
        Q_EMIT f.client.userProfileFinished(
            f.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QString(), QString());
        QCoreApplication::processEvents();
        QVERIFY(avatar().isEmpty());

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@sam:example.org"),
            QStringLiteral("mxc://example.org/first"));
        QTRY_COMPARE(avatar(), QStringLiteral("mxc://example.org/first"));

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@sam:example.org"),
            QStringLiteral("mxc://example.org/second"));
        QTRY_COMPARE(avatar(), QStringLiteral("mxc://example.org/second"));

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@sam:example.org"), QString());
        QTRY_VERIFY(avatar().isEmpty());

        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1);
        QCOMPARE(f.model.rowForRoom(QStringLiteral("!dm:x")), row);
    }

    void aPeersMemberEventOutranksAnOlderRosterFace()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        MemberInfo member;
        member.userId = peer.directUserId;
        member.avatarMxcUrl = QStringLiteral("mxc://example.org/old");
        peer.members.insert(member.userId, member);

        Fixture f;
        f.build({ peer });
        f.selectPeople();
        const int row = f.model.rowForRoom(QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        auto avatar = [&f, row] {
            return f.model.data(f.model.index(row, 0),
                                SpaceChannelModel::AvatarUrlRole).toString();
        };
        QCOMPARE(avatar(), QStringLiteral("mxc://example.org/old"));

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@kim:example.org"),
            QStringLiteral("mxc://example.org/kim"));
        QCoreApplication::processEvents();
        QCOMPARE(avatar(), QStringLiteral("mxc://example.org/old"));

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@sam:example.org"),
            QStringLiteral("mxc://example.org/new"));
        QTRY_COMPARE(avatar(), QStringLiteral("mxc://example.org/new"));

        Q_EMIT f.client.directPeerAvatarChanged(
            QStringLiteral("!dm:x"), QStringLiteral("@sam:example.org"), QString());
        QTRY_VERIFY(avatar().isEmpty());
    }

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

    void aQueuedRebuildNeverOutlivesTheStateThatArmedIt()
    {
        Fixture f;
        f.build(workspace());
        QTest::qWait(50);
        f.client.announce();
        f.model.setSources(nullptr, nullptr, nullptr);
        int settled = f.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(f.model.rebuildCountForTest(), settled);

        Fixture g;
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        g.client.roomList = { peer };
        g.model.setSettings(&g.settings);
        g.model.setSources(&g.client, &g.spaces, &g.layout);
        QTest::qWait(50);
        Q_EMIT g.client.userProfileFinished(
            g.client.nextOp, true, QStringLiteral("@sam:example.org"),
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
            QString());
        g.client.logout();
        settled = g.model.rebuildCountForTest();
        QTest::qWait(50);
        QCOMPARE(g.model.rebuildCountForTest(), settled);
    }

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
        model.setFilterMode(1);
        QCOMPARE(model.rowCount(), 0);
        QTest::qWait(50);
        QCOMPARE(model.rowCount(), 0);
    }

    void aFailedLookupSomebodyElseStartedNeverWedgesADm()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };

        Fixture f;
        f.build({});
        f.selectPeople();
        Q_EMIT f.client.userProfileFinished(
            9999, false, QStringLiteral("@sam:example.org"), QString(),
            QString(), QStringLiteral("timeout"));
        QTest::qWait(50);
        QCOMPARE(f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 0);

        f.client.roomList = { peer };
        f.client.announce();
        QTRY_COMPARE_WITH_TIMEOUT(
            f.client.profileFetches.count(QStringLiteral("@sam:example.org")), 1,
            2000);

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

    void aStaleProfileIsAskedAgainAndAChangedFaceIsAnnounced()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        const QString sam = QStringLiteral("@sam:example.org");

        Fixture f;
        qint64 now = 1000000;
        DirectAvatarResolver resolver;
        resolver.setClient(&f.client);
        resolver.setRefreshIntervalMs(1000);
        resolver.setClockForTest([&now] { return now; });
        QSignalSpy resolved(&resolver, &DirectAvatarResolver::avatarResolved);

        resolver.resolveMissing({ peer });
        QCOMPARE(f.client.profileFetches.count(sam), 1);
        Q_EMIT f.client.userProfileFinished(f.client.nextOp, true, sam,
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/old"),
            QString());
        QCOMPARE(resolved.count(), 1);
        QCOMPARE(resolver.avatarFor(peer), QStringLiteral("mxc://example.org/old"));

        now += 500;
        resolver.resolveMissing({ peer });
        QCOMPARE(f.client.profileFetches.count(sam), 1);

        now += 2000;
        resolver.resolveMissing({ peer });
        QCOMPARE(f.client.profileFetches.count(sam), 2);
        Q_EMIT f.client.userProfileFinished(f.client.nextOp, true, sam,
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/old"),
            QString());
        QCOMPARE(resolved.count(), 1);

        now += 2000;
        resolver.resolveMissing({ peer });
        QCOMPARE(f.client.profileFetches.count(sam), 3);
        Q_EMIT f.client.userProfileFinished(f.client.nextOp, true, sam,
            QStringLiteral("Sam"), QStringLiteral("mxc://example.org/new"),
            QString());
        QCOMPARE(resolved.count(), 2);
        QCOMPARE(resolver.avatarFor(peer), QStringLiteral("mxc://example.org/new"));
    }

    void everySurfaceShowsTheSameCountForOneRoomInEachMode()
    {
        struct Case { int mode; bool encryptionKnown; };
        const QList<Case> cases{ { 0, true }, { 1, true }, { 1, false }, { 2, true } };
        for (const Case &c : cases) {
            RoomInfo r = room(QStringLiteral("!r:x"), QStringLiteral("R"), 5, 2);
            r.encryptionKnown = c.encryptionKnown;
            r.encrypted = true;
            r.parentSpaceIds = { QStringLiteral("!s:x") };
            Fixture f;
            f.settings.setRoomNotificationMode(r.id, c.mode);
            f.spaces.setNotificationModeSource([&f](const QString &id) {
                return f.settings.roomUnreadMode(id);
            });
            f.build({ space(QStringLiteral("!s:x"), QStringLiteral("S"), { r.id }), r });
            f.selectSpace(QStringLiteral("!s:x"));

            QuickSwitcherModel switcher;
            switcher.setClient(&f.client);
            switcher.setNotificationModeSource([&f](const QString &id) {
                return f.settings.roomUnreadMode(id);
            });
            switcher.refresh();

            const int expected = roomunread::visibleUnreadCount(
                r, f.settings.roomUnreadMode(r.id));
            const QByteArray label = QByteArray("mode ") + QByteArray::number(c.mode)
                + (c.encryptionKnown ? " known" : " unknown");
            const int row = f.model.rowForRoom(r.id);
            QVERIFY2(row >= 0, label.constData());
            QCOMPARE(f.model.data(f.model.index(row, 0),
                                  SpaceChannelModel::UnreadCountRole).toInt(),
                     expected);
            int switcherCount = -1;
            for (int i = 0; i < switcher.rowCount(); ++i) {
                const QModelIndex idx = switcher.index(i, 0);
                if (switcher.data(idx, QuickSwitcherModel::NameRole).toString()
                    == QLatin1String("R"))
                    switcherCount = switcher.data(
                        idx, QuickSwitcherModel::UnreadRole).toInt();
            }
            QCOMPARE(switcherCount, expected);
            if (expected == 0)
                QVERIFY2(railUnreadTotal(f.spaces, QStringLiteral("!s:x")) != 5,
                         label.constData());
            else
                QCOMPARE(railUnreadTotal(f.spaces, QStringLiteral("!s:x")), expected);
        }
    }

    void twoOwnersSharingAResolverShareItsFetches()
    {
        RoomInfo peer = dm(QStringLiteral("!dm:x"), QStringLiteral("Sam"));
        peer.directUserId = QStringLiteral("@sam:example.org");
        peer.directUserIds = { QStringLiteral("@sam:example.org") };
        const QString sam = QStringLiteral("@sam:example.org");

        for (const bool shared : { false, true }) {
            DirectAvatarResolver resolver;
            Fixture f;
            SpaceChannelModel second;
            if (shared) {
                f.model.setDirectAvatarResolver(&resolver);
                second.setDirectAvatarResolver(&resolver);
            }
            f.build({ peer });
            second.setSettings(&f.settings);
            second.setSources(&f.client, &f.spaces, &f.layout);
            f.selectPeople();
            second.setScopeSpaceId(SpaceManager::peopleId());
            QTRY_VERIFY(f.client.profileFetches.count(sam) >= 1);
            QTest::qWait(50);
            QCOMPARE(f.client.profileFetches.count(sam), shared ? 1 : 2);

            Q_EMIT f.client.userProfileFinished(f.client.nextOp, true, sam,
                QStringLiteral("Sam"), QStringLiteral("mxc://example.org/sam"),
                QString());
            if (shared) {
                for (SpaceChannelModel *m : { &f.model, &second }) {
                    QTRY_VERIFY(m->rowForRoom(QStringLiteral("!dm:x")) >= 0);
                    const int row = m->rowForRoom(QStringLiteral("!dm:x"));
                    QTRY_COMPARE(m->data(m->index(row, 0),
                                         SpaceChannelModel::AvatarUrlRole).toString(),
                                 QStringLiteral("mxc://example.org/sam"));
                }
            }
        }
    }

    void staleRefreshesAreCappedPerPass()
    {
        Fixture f;
        qint64 now = 1000000;
        DirectAvatarResolver resolver;
        resolver.setClient(&f.client);
        resolver.setRefreshIntervalMs(1000);
        resolver.setClockForTest([&now] { return now; });

        QList<RoomInfo> rooms;
        for (int i = 0; i < 10; ++i) {
            const QString user = QStringLiteral("@u%1:example.org").arg(i);
            RoomInfo r = dm(QStringLiteral("!dm%1:x").arg(i), QStringLiteral("U"));
            r.directUserId = user;
            r.directUserIds = { user };
            rooms.append(r);
        }
        resolver.resolveMissing(rooms);
        QCOMPARE(f.client.profileFetches.size(), 10);
        for (int i = 0; i < 10; ++i)
            Q_EMIT f.client.userProfileFinished(
                f.client.nextOp - 9 + i, true,
                QStringLiteral("@u%1:example.org").arg(i), QString(),
                QStringLiteral("mxc://example.org/a%1").arg(i), QString());

        now += 5000;
        resolver.resolveMissing(rooms);
        QCOMPARE(f.client.profileFetches.size(), 10 + DirectAvatarResolver::kMaxRefreshesPerPass);
    }

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

void SpaceChannelsTest::homeRoomsAreNewestFirstNotAlphabetical()
{
    Fixture f;
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

void SpaceChannelsTest::lowPriorityTagReachesTheChannelMenu()
{
    Fixture f;
    auto tagged = room(QStringLiteral("!tagged:x"), QStringLiteral("Tagged"));
    tagged.lowPriority = true;
    f.build({tagged});
    f.selectHome();
    int row = f.model.rowForRoom(tagged.id);
    QVERIFY(row >= 0);
    QVERIFY(f.model.data(f.model.index(row, 0),
                         SpaceChannelModel::IsLowPriorityRole).toBool());
    f.client.roomList[0].lowPriority = false;
    const int before = f.model.rebuildCountForTest();
    f.client.announce();
    QTRY_VERIFY(f.model.rebuildCountForTest() > before);
    row = f.model.rowForRoom(tagged.id);
    QVERIFY(row >= 0);
    QVERIFY(!f.model.data(f.model.index(row, 0),
                          SpaceChannelModel::IsLowPriorityRole).toBool());
}

void SpaceChannelsTest::lowPriorityFollowsOrdinaryRoomsInEverySortMode()
{
    Fixture f;
    auto tagged = room(QStringLiteral("!tagged:x"), QStringLiteral("A Tagged"));
    tagged.lowPriority = true;
    tagged.lastActivity = ago(1);
    auto ordinary = room(QStringLiteral("!ordinary:x"), QStringLiteral("Z Ordinary"));
    ordinary.lastActivity = ago(900);
    f.build({tagged, ordinary});
    f.selectHome();
    for (int mode : {0, 1, 2, 3}) {
        f.model.setSortMode(mode);
        QVERIFY2(rowOfName(f.model, ordinary.name) < rowOfName(f.model, tagged.name),
                 "m.lowpriority room must follow the ordinary room");
    }
}

void SpaceChannelsTest::aSpacesRoomsAreNewestFirstNotInChildOrder()
{
    Fixture f;
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
    QVERIFY(names.indexOf(QStringLiteral("Work"))
            < names.indexOf(QStringLiteral("General")));
}

void SpaceChannelsTest::aRoomMovesWhenSomebodySpeaksInIt()
{
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

    f.client.roomList[1].lastActivity = ago(0);
    f.client.announce();
    QCoreApplication::processEvents();

    names = namesOf(f.model);
    QVERIFY2(names.indexOf(QStringLiteral("Zulu"))
                 < names.indexOf(QStringLiteral("Alpha")),
             "the row did not move when the room received a message");
}

QTEST_MAIN(SpaceChannelsTest)
#include "SpaceChannelsTest.moc"
