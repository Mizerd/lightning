
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const auto kWork = QStringLiteral("!work:x");
const auto kGeneral = QStringLiteral("!general:x");
const auto kDmIn = QStringLiteral("!dm-in:x");
const auto kDmOut = QStringLiteral("!dm-out:x");
const auto kAda = QStringLiteral("@ada:x");
const auto kZoe = QStringLiteral("@zoe:x");

RoomInfo room(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
    info.lastActivity = QDateTime::currentDateTimeUtc();
    return info;
}

RoomInfo spaceRoom(const QString &id, const QString &name,
                   const QStringList &children)
{
    RoomInfo info = room(id, name);
    info.isSpace = true;
    info.childRoomIds = children;
    return info;
}

RoomInfo dm(const QString &id, const QString &name, const QString &peer)
{
    RoomInfo info = room(id, name);
    info.isDirect = true;
    info.directUserId = peer;
    info.directUserIds = { peer };
    return info;
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> mirror;
    QStringList memberRequests;
    quint64 nextOp = 0;
    /// 0 = "this backend cannot answer", which is what a real client returns
    /// before it has an SDK handle. The manager must not mark such a Space as
    /// asked, or its People list is unscoped for the whole session.
    bool memberRequestsSupported = true;

    quint64 requestRoomMembers(const QString &roomId) override
    {
        if (!memberRequestsSupported)
            return 0;
        memberRequests.append(roomId);
        return ++nextOp;
    }

    void deliverRoster(quint64 op, const QString &roomId,
                       const QStringList &joined, bool partial = false,
                       bool truncated = false, bool ok = true)
    {
        QVariantList members;
        for (const QString &userId : joined) {
            members.append(QVariantMap{
                { QStringLiteral("userId"), userId },
                { QStringLiteral("membership"), QStringLiteral("joined") },
            });
        }
        QVariantMap snapshot{
            { QStringLiteral("ok"), ok },
            { QStringLiteral("partial"), partial },
            { QStringLiteral("truncated"), truncated },
            { QStringLiteral("members"), members },
        };
        Q_EMIT roomMembersReceived(op, roomId, snapshot);
    }

    void deliverPermissions(quint64 op, const QString &roomId, bool allowed,
                            bool ok = true, bool partial = false,
                            bool truncated = false)
    {
        Q_EMIT roomMembersReceived(op, roomId, QVariantMap{
            {QStringLiteral("ok"), ok},
            {QStringLiteral("partial"), partial},
            {QStringLiteral("truncated"), truncated},
            {QStringLiteral("canManageSpaceChildren"), allowed},
        });
    }

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:x"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

QList<RoomInfo> workspace()
{
    return {
        spaceRoom(kWork, QStringLiteral("Work"), { kGeneral }),
        room(kGeneral, QStringLiteral("general")),
        dm(kDmIn, QStringLiteral("Ada"), kAda),
        dm(kDmOut, QStringLiteral("Zoe"), kZoe),
    };
}

QStringList idsOf(const RoomListModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i),
                              RoomListModel::RoomIdRole).toString());
    }
    out.sort();
    return out;
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

}

class SpacePeopleScopeTest : public QObject
{
    Q_OBJECT

private:
    struct Classic {
        FakeClient client;
        SpaceManager spaces;
        RoomListModel model;

        void build()
        {
            client.mirror = workspace();
            spaces.setClient(&client);
            model.setSpaceManager(&spaces);
            model.setClient(&client);
        }
        void select(const QString &spaceId)
        {
            spaces.setActiveSpaceId(spaceId);
        }
    };

    struct Channels {
        FakeClient client;
        SpaceManager spaces;
        SettingsManager settings;
        RailLayoutStore layout{ &settings };
        SpaceChannelModel model;

        void build()
        {
            client.mirror = workspace();
            spaces.setClient(&client);
            model.setSettings(&settings);
            model.setSources(&client, &spaces, &layout);
        }
        void select(const QString &spaceId)
        {
            spaces.setActiveSpaceId(spaceId);
            model.setScopeSpaceId(spaceId);
        }
    };

private Q_SLOTS:
    void placementUsesTheParentSpacesOwnSdkPermission()
    {
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        spaces.setActiveSpaceId(kWork);
        const quint64 first = client.nextOp;
        QSignalSpy changes(&spaces, &SpaceManager::spacePermissionsChanged);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(first, kGeneral, true);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(first, kWork, false);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        Q_EMIT client.roomMemberEventSeen(kWork);
        const quint64 second = client.nextOp;
        QVERIFY(second > first);
        client.deliverPermissions(first, kWork, true);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(second, kWork, true, true, true);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(second, kWork, true, true, false, true);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        Q_EMIT client.roomMemberEventSeen(kWork);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(client.nextOp, kWork, true, false);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        spaces.setActiveSpaceId(QString());
        spaces.setActiveSpaceId(kWork);
        client.deliverPermissions(client.nextOp, kWork, true);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        spaces.setActiveSpaceId(SpaceManager::peopleId());
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        spaces.setActiveSpaceId(kWork);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        client.logout();
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        FakeClient replacement;
        spaces.setClient(&replacement);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        QVERIFY(changes.count() > 0);
    }

    void placementPermissionRefreshSurvivesRosterFailure_data()
    {
        QTest::addColumn<bool>("unrelatedFailure");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("current-failure-then-revocation") << false << false;
        QTest::newRow("current-failure-then-grant") << false << true;
        QTest::newRow("unrelated-failure-then-revocation") << true << false;
        QTest::newRow("unrelated-failure-then-grant") << true << true;
    }

    void placementPermissionRefreshSurvivesRosterFailure()
    {
        QFETCH(bool, unrelatedFailure);
        QFETCH(bool, allowed);
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        spaces.setActiveSpaceId(kWork);
        const quint64 first = client.nextOp;
        client.deliverPermissions(first, kWork, !allowed);
        QCOMPARE(spaces.activeSpaceCanManageChildren(), !allowed);

        if (unrelatedFailure) {
            QSignalSpy permissions(&spaces, &SpaceManager::spacePermissionsChanged);
            QSignalSpy rosters(&spaces, &SpaceManager::spaceRosterChanged);
            client.deliverPermissions(first + 100, kWork, true, false);
            QCOMPARE(spaces.activeSpaceCanManageChildren(), !allowed);
            QVERIFY(spaces.spaceRosterKnown(kWork));
            QCOMPARE(permissions.count(), 0);
            QCOMPARE(rosters.count(), 0);
            spaces.ensureSpaceRoster(kWork);
            QCOMPARE(client.nextOp, first);
        } else {
            Q_EMIT client.roomMemberEventSeen(kWork);
            QVERIFY(client.nextOp > first);
            client.deliverPermissions(client.nextOp, kWork, true, false);
            QVERIFY(!spaces.activeSpaceCanManageChildren());
        }

        const quint64 beforeRefresh = client.nextOp;
        Q_EMIT client.roomMemberEventSeen(kWork);
        QCOMPARE(client.nextOp, beforeRefresh + 1);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(client.nextOp, kWork, allowed);
        QCOMPARE(spaces.activeSpaceCanManageChildren(), allowed);
    }

    void supersededPermissionFailureLeavesTheCurrentRequestAndRosterAlone()
    {
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        spaces.setActiveSpaceId(kWork);
        const quint64 first = client.nextOp;
        client.deliverPermissions(first, kWork, true, true, true);
        Q_EMIT client.roomMemberEventSeen(kWork);
        const quint64 second = client.nextOp;
        QVERIFY(second > first);
        client.deliverPermissions(second, kWork, true, true, true);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        QVERIFY(!spaces.spaceRosterKnown(kWork));
        QSignalSpy permissions(&spaces, &SpaceManager::spacePermissionsChanged);
        QSignalSpy rosters(&spaces, &SpaceManager::spaceRosterChanged);

        client.deliverPermissions(first, kWork, false, false);
        client.deliverPermissions(second, kGeneral, false, false);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        QVERIFY(!spaces.spaceRosterKnown(kWork));
        QCOMPARE(permissions.count(), 0);
        QCOMPARE(rosters.count(), 0);
        spaces.ensureSpaceRoster(kWork);
        QCOMPARE(client.nextOp, second);

        client.deliverPermissions(second, kWork, true);
        QVERIFY(spaces.spaceRosterKnown(kWork));
        permissions.clear();
        rosters.clear();
        client.deliverPermissions(first, kWork, false, false);
        QVERIFY(spaces.activeSpaceCanManageChildren());
        QVERIFY(spaces.spaceRosterKnown(kWork));
        QCOMPARE(permissions.count(), 0);
        QCOMPARE(rosters.count(), 0);
        spaces.ensureSpaceRoster(kWork);
        QCOMPARE(client.nextOp, second);

        Q_EMIT client.roomMemberEventSeen(kWork);
        QCOMPARE(client.nextOp, second + 1);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
        client.deliverPermissions(client.nextOp, kWork, false);
        QVERIFY(!spaces.activeSpaceCanManageChildren());
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("space-people-scope-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void theClassicPeopleChipIsScopedToTheSelectedSpacesPeople()
    {
        Classic f;
        f.build();
        f.select(kWork);
        QCOMPARE(f.client.memberRequests, QStringList{ kWork });

        f.model.setFilterMode(1);
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));

        f.client.deliverRoster(1, kWork, { QStringLiteral("@me:x"), kAda });
        QVERIFY(f.spaces.spaceRosterKnown(kWork));

        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });
        f.model.setFilterMode(2);
        QCOMPARE(idsOf(f.model), QStringList{ kGeneral });
        f.model.setFilterMode(0);
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kGeneral }));
    }

    void aDirectMessageIsNeverScopedOutsideARealSpace()
    {
        Classic f;
        f.build();
        f.select(kWork);
        f.client.deliverRoster(1, kWork, { kAda });
        f.model.setFilterMode(1);
        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });

        f.select(SpaceManager::allRoomsId());
        QTRY_COMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));
        f.select(SpaceManager::orphansId());
        QTRY_COMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));
        QCOMPARE(f.client.memberRequests, QStringList{ kWork });
    }

    void anUnansweredOrRefusedRosterLeavesEveryDirectMessageInPlace()
    {
        Classic quiet;
        quiet.client.memberRequestsSupported = false;
        quiet.build();
        quiet.select(kWork);
        quiet.model.setFilterMode(1);
        QCOMPARE(idsOf(quiet.model), (QStringList{ kDmIn, kDmOut }));
        QVERIFY(!quiet.spaces.spaceRosterKnown(kWork));

        Classic failed;
        failed.build();
        failed.select(kWork);
        failed.client.deliverRoster(1, kWork, {},false,
false,false);
        failed.model.setFilterMode(1);
        QCOMPARE(idsOf(failed.model), (QStringList{ kDmIn, kDmOut }));
        QVERIFY(!failed.spaces.spaceRosterKnown(kWork));
        failed.select(SpaceManager::allRoomsId());
        failed.select(kWork);
        QCOMPARE(failed.client.memberRequests, (QStringList{ kWork, kWork }));
    }

    void aPartialOrTruncatedRosterIsNotASmallerTruth()
    {
        Classic f;
        f.build();
        f.select(kWork);
        f.model.setFilterMode(1);
        f.client.deliverRoster(1, kWork, { kAda },true);
        QVERIFY2(!f.spaces.spaceRosterKnown(kWork),
                 "a cache-only partial snapshot was published as a roster");
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));

        f.client.deliverRoster(1, kWork, { kAda });
        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });

        // A TRUNCATED roster is refused for the same reason, and this one is
        // easy to miss: it arrives ok, complete-looking, and 500 members
        // short. Zoe is absent from it and must still be listed.
        Classic capped;
        capped.build();
        capped.select(kWork);
        capped.model.setFilterMode(1);
        capped.client.deliverRoster(1, kWork, { kAda },false,
true);
        QVERIFY2(!capped.spaces.spaceRosterKnown(kWork),
                 "a truncated roster was published as a complete one");
        QCOMPARE(idsOf(capped.model), (QStringList{ kDmIn, kDmOut }));

        capped.client.deliverRoster(99, kGeneral, { kZoe });
        QVERIFY(!capped.spaces.spaceRosterKnown(kGeneral));
        QCOMPARE(idsOf(capped.model), (QStringList{ kDmIn, kDmOut }));
    }

    void theChannelsSpaceViewGainsThePeopleOfThatSpace()
    {
        Channels f;
        f.build();
        f.select(kWork);
        QTRY_VERIFY(!namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("People")));

        f.client.deliverRoster(1, kWork, { kAda });

        f.model.setFilterMode(0);
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("People")),
                 "People is pinned to the Space view instead of filtered");
        f.model.setFilterMode(2);
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("People")),
                 "the Rooms filter still listed people");

        f.model.setFilterMode(1);
        QTRY_VERIFY2(namesOf(f.model).contains(QStringLiteral("People")),
                     "the Space view never grew a People group");
        const QStringList names = namesOf(f.model);
        const int people = names.indexOf(QStringLiteral("People"));
        QCOMPARE(names.mid(people + 1), QStringList{ QStringLiteral("Ada") });
        QVERIFY2(!names.contains(QStringLiteral("Zoe")),
                 "a DM with somebody outside the Space was listed in it");
        QCOMPARE(f.model.data(f.model.index(people, 0),
                              SpaceChannelModel::KindRole).toString(),
                 QStringLiteral("group"));
        QCOMPARE(f.model.data(f.model.index(people, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::spacePeopleGroupId());

        f.select(SpaceManager::peopleId());
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Zoe")));
        f.select(SpaceManager::allRoomsId());
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Zoe")));
    }

    void aRosterIsAnAnswerAboutOneAccount()
    {
        Channels f;
        f.build();
        f.select(kWork);
        f.client.deliverRoster(1, kWork, { kAda });
        f.model.setFilterMode(1);
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("People")));

        FakeClient next;
        next.mirror = workspace();
        f.spaces.setClient(&next);
        QVERIFY2(!f.spaces.spaceRosterKnown(kWork),
                 "a roster survived the account it was fetched for");
        QCOMPARE(next.memberRequests, QStringList{ kWork });
        f.model.setSources(&next, &f.spaces, &f.layout);
        QTRY_VERIFY(!namesOf(f.model).contains(QStringLiteral("People")));
    }

    void aJustJoinedSpaceKeepsTheSelectionUntilItIsListed()
    {
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        const QString fresh = QStringLiteral("!fresh:x");

        spaces.setActiveSpaceId(fresh);
        client.announce();
        QVERIFY(spaces.activeSpaceId().isEmpty());

        spaces.selectJoinedSpace(fresh);
        client.announce();
        QCOMPARE(spaces.activeSpaceId(), fresh);

        client.mirror.append(spaceRoom(fresh, QStringLiteral("Fresh"), {}));
        client.announce();
        QCOMPARE(spaces.activeSpaceId(), fresh);

        client.mirror.last().membership = RoomInfo::Left;
        client.announce();
        QVERIFY(spaces.activeSpaceId().isEmpty());
    }

    void aPendingSelectionIsDroppedForAnOrdinaryRoom()
    {
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        const QString plain = QStringLiteral("!plain:x");
        spaces.selectJoinedSpace(plain);
        client.announce();
        QCOMPARE(spaces.activeSpaceId(), plain);
        client.mirror.append(room(plain, QStringLiteral("Plain")));
        client.announce();
        QVERIFY(spaces.activeSpaceId().isEmpty());
    }

    void aPendingSelectionDoesNotSurviveAnAccountSwitch()
    {
        FakeClient client;
        client.mirror = workspace();
        SpaceManager spaces;
        spaces.setClient(&client);
        const QString fresh = QStringLiteral("!fresh:x");
        spaces.selectJoinedSpace(fresh);
        client.announce();
        QCOMPARE(spaces.activeSpaceId(), fresh);
        FakeClient next;
        next.mirror = workspace();
        spaces.setClient(&next);
        next.announce();
        QVERIFY2(spaces.activeSpaceId().isEmpty(),
                 "the previous account's pending Space kept the selection");
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(SpacePeopleScopeTest)
#include "SpacePeopleScopeTest.moc"
