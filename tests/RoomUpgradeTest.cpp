
#include "app/RoomDiscoveryController.h"
#include "app/RoomUpgradeController.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

constexpr int kSignalTimeoutMs = 3000;

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomSet;
    quint64 nextOp = 1;
    bool discoverySupported = true;
    bool refuseJoins = false;
    int joinCalls = 0;
    QString lastJoinTarget;
    quint64 lastJoinOp = 0;
    int versionRequests = 0;
    QString lastUpgradeVersion;
    QString lastUpgradeRoomId;
    quint64 lastUpgradeOp = 0;
    bool refuseUpgrades = false;
    void requestRoomVersions() override { ++versionRequests; }
    quint64 upgradeRoom(const QString &roomId, const QString &newVersion) override
    {
        if (refuseUpgrades)
            return 0;
        lastUpgradeRoomId = roomId;
        lastUpgradeVersion = newVersion;
        lastUpgradeOp = nextOp++;
        return lastUpgradeOp;
    }

    void setRooms(const QList<RoomInfo> &rooms)
    {
        roomSet = rooms;
        Q_EMIT roomsChanged();
    }

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
    QList<RoomInfo> rooms() const override { return roomSet; }
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
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsRoomDiscovery() const override { return discoverySupported; }
    quint64 joinRoomByIdOrAlias(const QString &target,
                                const QStringList &) override
    {
        if (refuseJoins)
            return 0;
        ++joinCalls;
        lastJoinTarget = target;
        lastJoinOp = nextOp++;
        return lastJoinOp;
    }
};

RoomInfo room(const QString &id,
              RoomInfo::Membership membership = RoomInfo::Joined,
              const QString &successor = QString(),
              const QString &predecessor = QString())
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.membership = membership;
    info.successorRoomId = successor;
    info.predecessorRoomId = predecessor;
    return info;
}

struct Harness {
    FakeClient client;
    RoomDiscoveryController discovery;
    RoomUpgradeController upgrade;
    QStringList navigations;

    Harness()
    {
        upgrade.setClient(&client);
        discovery.setClient(&client);
        upgrade.setDiscovery(&discovery);
        QObject::connect(&upgrade, &RoomUpgradeController::navigateRequested,
                         [this](const QString &id) { navigations.append(id); });
        QObject::connect(&discovery, &RoomDiscoveryController::roomJoined,
                         [this](const QString &id) {
            if (upgrade.consumeAbandonedJoin(id))
                return;
            navigations.append(id);
        });
    }
};

const QString kOld = QStringLiteral("!old:example.org");
const QString kNew = QStringLiteral("!new:example.org");

}

class RoomUpgradeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void versionsComeFromTheServerAndAFailedReadLeavesNothingToPick()
    {
        Harness h;
        h.upgrade.requestRoomVersions();
        QCOMPARE(h.client.versionRequests, 1);
        QVERIFY(!h.upgrade.versionsKnown());
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } },
              QVariantMap{ { QStringLiteral("version"), QStringLiteral("11") },
                           { QStringLiteral("stable"), false } } });
        QVERIFY(h.upgrade.versionsKnown());
        QCOMPARE(h.upgrade.defaultVersion(), QStringLiteral("10"));
        QCOMPARE(h.upgrade.availableVersions().size(), 2);
        Q_EMIT h.client.roomVersionsReceived(false, QString(), {});
        QVERIFY(!h.upgrade.versionsKnown());
        QVERIFY(h.upgrade.availableVersions().isEmpty());
    }

    void upgradeRefusesAVersionTheServerDidNotAdvertise()
    {
        Harness h;
        h.client.setRooms({ room(kOld) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(kOld, QStringLiteral("99"), false);
        QVERIFY(h.client.lastUpgradeVersion.isEmpty());
        QVERIFY(!h.upgrade.upgradeError().isEmpty());
        QVERIFY(!h.upgrade.upgradeBusy());
    }

    void anUpgradeDestroysTheRoomTheCallerNamedNotTheOpenOne()
    {
        const QString space = QStringLiteral("!space:example.org");
        Harness h;
        h.client.setRooms({ room(kOld), room(space) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(space, QStringLiteral("10"), false);
        QCOMPARE(h.client.lastUpgradeRoomId, space);
        QVERIFY(h.client.lastUpgradeRoomId != kOld);
    }

    void anUpgradeWithNoNamedRoomRefusesAndSaysSo()
    {
        Harness h;
        h.client.setRooms({ room(kOld) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(QString(), QStringLiteral("10"), false);
        QVERIFY(h.client.lastUpgradeRoomId.isEmpty());
        QVERIFY(!h.upgrade.upgradeError().isEmpty());
        QVERIFY(!h.upgrade.upgradeBusy());
    }

    void anUpgradeWithNoReplacementIdDoesNotClaimNothingChanged()
    {
        Harness h;
        h.client.setRooms({ room(kOld) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(kOld, QStringLiteral("10"), false);
        Q_EMIT h.client.roomUpgradeFinished(h.client.lastUpgradeOp, kOld, true,
                                            QString(), QString());
        QVERIFY(!h.upgrade.upgradeBusy());
        QVERIFY(!h.upgrade.upgradeError().isEmpty());
        QVERIFY(!h.upgrade.upgradeError().contains(
            QStringLiteral("Nothing was changed")));
        QVERIFY(h.navigations.isEmpty());
    }

    void aSuccessfulUpgradeNavigatesToTheReplacement()
    {
        Harness h;
        h.client.setRooms({ room(kOld) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(kOld, QStringLiteral("10"), false);
        QCOMPARE(h.client.lastUpgradeVersion, QStringLiteral("10"));
        QVERIFY(h.upgrade.upgradeBusy());
        Q_EMIT h.client.roomUpgradeFinished(h.client.lastUpgradeOp + 7, kOld,
                                            true, kNew, QString());
        QVERIFY(h.upgrade.upgradeBusy());
        QVERIFY(h.navigations.isEmpty());
        Q_EMIT h.client.roomUpgradeFinished(h.client.lastUpgradeOp, kOld, true,
                                            kNew, QString());
        QVERIFY(!h.upgrade.upgradeBusy());
        QCOMPARE(h.upgrade.lastReplacementRoomId(), kNew);
        QCOMPARE(h.navigations, QStringList{ kNew });
    }

    void aRefusedUpgradeReportsWhyAndStaysPut()
    {
        Harness h;
        h.client.setRooms({ room(kOld) });
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        h.upgrade.upgradeRoom(kOld, QStringLiteral("10"), false);
        Q_EMIT h.client.roomUpgradeFinished(h.client.lastUpgradeOp, kOld, false,
                                            QString(),
                                            QStringLiteral("forbidden"));
        QVERIFY(!h.upgrade.upgradeBusy());
        QVERIFY(h.upgrade.upgradeError().contains(QStringLiteral("not allowed")));
        QVERIFY(h.navigations.isEmpty());
        QVERIFY(h.upgrade.lastReplacementRoomId().isEmpty());
    }

    void tombstonedRoomReportsItsSuccessor()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        QVERIFY(h.upgrade.upgraded());
        QCOMPARE(h.upgrade.successorRoomId(), kNew);
        h.upgrade.setRoomId(kNew);
        QVERIFY(!h.upgrade.upgraded());
        QVERIFY(h.upgrade.successorRoomId().isEmpty());
    }

    void unknownSuccessorIsUnknownNotInaccessible()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::Unknown));
        QVERIFY(h.upgrade.upgraded());
        QVERIFY(!h.upgrade.busy());
    }

    void heldButUnjoinedSuccessorIsNotAccessible()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Left, QString(), kOld) });
        h.upgrade.setRoomId(kOld);

        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::NotAccessible));
        h.upgrade.continueToSuccessor();
        QCOMPARE(h.client.joinCalls, 0);
        QVERIFY(h.navigations.isEmpty());
        QVERIFY(!h.upgrade.error().isEmpty());
    }

    void joinedSuccessorNavigatesWithoutJoining()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kOld);
        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::Joined));

        h.upgrade.continueToSuccessor();
        QCOMPARE(h.client.joinCalls, 0);
        QCOMPARE(h.navigations, QStringList{ kNew });
        QVERIFY(h.upgrade.error().isEmpty());
    }

    void invitedSuccessorJoinsBeforeNavigating()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Invited, QString(), kOld) });
        h.upgrade.setRoomId(kOld);
        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::Invited));

        h.upgrade.continueToSuccessor();
        QCOMPARE(h.client.joinCalls, 1);
        QCOMPARE(h.client.lastJoinTarget, kNew);
        QVERIFY(h.upgrade.busy());
        QVERIFY(h.navigations.isEmpty());

        Q_EMIT h.client.roomJoinFinished(h.client.lastJoinOp, true, kNew,
                                         QString());
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QTRY_VERIFY_WITH_TIMEOUT(!h.navigations.isEmpty(), kSignalTimeoutMs);
        QCOMPARE(h.navigations.last(), kNew);
        QVERIFY(!h.upgrade.busy());
    }

    void failedJoinStaysPutAndReportsWhy()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        h.upgrade.continueToSuccessor();
        QCOMPARE(h.client.joinCalls, 1);

        Q_EMIT h.client.roomJoinFinished(h.client.lastJoinOp, false, QString(),
                                         QStringLiteral("banned"));
        QTRY_VERIFY_WITH_TIMEOUT(!h.upgrade.error().isEmpty(), kSignalTimeoutMs);
        QVERIFY(h.navigations.isEmpty());
        QVERIFY(!h.upgrade.busy());
        QCOMPARE(h.upgrade.error(),
                 RoomDiscoveryController::describeJoinCategory(
                     QStringLiteral("banned")));
    }

    void chainVerificationRequiresThePointerBack()
    {
        Harness h;

        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(!h.upgrade.chainVerified());
        QVERIFY(h.upgrade.upgraded());

        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QVERIFY(h.upgrade.chainVerified());

        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(),
                                 QStringLiteral("!other:example.org")) });
        QVERIFY(!h.upgrade.chainVerified());
    }

    void roomSwitchDropsStateAndRejectsLateAnswers()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        const quint64 staleOp = h.client.lastJoinOp;
        h.upgrade.setRoomId(QStringLiteral("!third:example.org"));
        QVERIFY(!h.upgrade.busy());
        QVERIFY(h.upgrade.error().isEmpty());

        Q_EMIT h.client.roomJoinFinished(staleOp, false, QString(),
                                         QStringLiteral("forbidden"));
        QCoreApplication::processEvents();
        QVERIFY2(h.upgrade.error().isEmpty(),
                 "a failure from the previous room must not surface here");
        QVERIFY(h.navigations.isEmpty());
    }

    void signOutClearsEverything()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(h.upgrade.upgraded());

        h.client.logout();
        QCoreApplication::processEvents();
        QVERIFY(!h.upgrade.upgraded());
        QVERIFY(h.upgrade.successorRoomId().isEmpty());
        QVERIFY(!h.upgrade.busy());
        QVERIFY(h.upgrade.error().isEmpty());
    }

    void observingATombstoneNeverFollowsIt()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(!h.upgrade.upgraded());

        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();

        QVERIFY(h.upgrade.upgraded());
        QCOMPARE(h.client.joinCalls, 0);
        QVERIFY2(h.navigations.isEmpty(),
                 "a tombstone must never navigate by itself");
    }

    void navigationArgumentSurvivesTheControllerReentering()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kOld);

        QString seenAfterReentry;
        QObject::connect(&h.upgrade, &RoomUpgradeController::navigateRequested,
                         [&](const QString &roomId) {
            h.upgrade.setRoomId(roomId);
            seenAfterReentry = roomId;
        });

        h.upgrade.continueToSuccessor();
        QCOMPARE(seenAfterReentry, kNew);
    }

    void predecessorNavigationArgumentAlsoSurvivesReentering()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kNew);
        QCOMPARE(h.upgrade.predecessorRoomId(), kOld);

        QString seenAfterReentry;
        QObject::connect(&h.upgrade, &RoomUpgradeController::navigateRequested,
                         [&](const QString &roomId) {
            h.upgrade.setRoomId(roomId);
            seenAfterReentry = roomId;
        });

        h.upgrade.goToPredecessor();
        QCOMPARE(seenAfterReentry, kOld);
    }

    void abandonedJoinDoesNotNavigateLater()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        h.upgrade.setRoomId(QStringLiteral("!third:example.org"));
        QVERIFY(h.upgrade.consumeAbandonedJoin(kNew));
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kOld));
    }

    void abandonedJoinThatFailedDoesNotSwallowALaterSuccess()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        h.upgrade.continueToSuccessor();
        const quint64 firstOp = h.client.lastJoinOp;

        h.upgrade.setRoomId(QStringLiteral("!third:example.org"));

        Q_EMIT h.client.roomJoinFinished(firstOp, false, QString(),
                                         QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QCOMPARE(h.client.joinCalls, 2);
        Q_EMIT h.client.roomJoinFinished(h.client.lastJoinOp, true, kNew,
                                         QString());
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QTRY_VERIFY_WITH_TIMEOUT(!h.navigations.isEmpty(), kSignalTimeoutMs);
        QCOMPARE(h.navigations.last(), kNew);
    }

    void signOutLeavesNoSuppressionResidue()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        h.client.logout();
        QCoreApplication::processEvents();
        QVERIFY2(!h.upgrade.consumeAbandonedJoin(kNew),
                 "a sign-out must not leave a suppression token behind");
    }

    void joinCompletedInPlaceIsNotSuppressed()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
    }

    void unrelatedDiscoverFailureDoesNotSurfaceInTheBanner()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        Q_EMIT h.client.knockCancelFinished(
            h.client.nextOp++, false, QStringLiteral("!knocked:example.org"),
            QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        QVERIFY2(h.upgrade.error().isEmpty(),
                 "another operation's refusal must not appear in the banner");
        QVERIFY2(h.upgrade.busy(),
                 "and it must not resolve our still-pending join");
    }

    void roomListDemotesOnlyOnceTheSuccessorIsReachable()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        const auto rowFor = [&model](const QString &id) {
            for (int i = 0; i < model.rowCount(); ++i) {
                const auto idx = model.index(i, 0);
                if (model.data(idx, RoomListModel::RoomIdRole).toString() == id)
                    return i;
            }
            return -1;
        };
        const auto supersededAt = [&model](int row) {
            return model.data(model.index(row, 0),
                              RoomListModel::SupersededByAccessibleSuccessorRole)
                .toBool();
        };

        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")) });
        QCoreApplication::processEvents();
        int oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0, "the upgraded room must stay in the list");
        QVERIFY(!supersededAt(oldRow));

        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")),
                          room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();
        oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0,
                 "de-emphasis must DEMOTE the old room, never remove it");
        QVERIFY(supersededAt(oldRow));
        const int liveRow = rowFor(QStringLiteral("!live:example.org"));
        QVERIFY(liveRow >= 0);
        QVERIFY2(oldRow > liveRow, "a superseded room sorts below live rooms");

        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")),
                          room(kNew, RoomInfo::Joined, QString(),
                               QStringLiteral("!other:example.org")) });
        QCoreApplication::processEvents();
        oldRow = rowFor(kOld);
        QVERIFY(oldRow >= 0);
        QVERIFY2(!supersededAt(oldRow),
                 "a contradicted chain must not bury a live room");
    }

    void demotionStaysInsideTheRoomsCategorySoSectionsRemainContiguous()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        RoomInfo dm = room(QStringLiteral("!dm:example.org"));
        dm.isDirect = true;
        dm.directUserId = QStringLiteral("@bob:example.org");
        dm.directUserIds = { dm.directUserId };
        RoomInfo dmSuccessor = room(QStringLiteral("!dm2:example.org"),
                                    RoomInfo::Joined, QString(),
                                    QStringLiteral("!dmOld:example.org"));
        dmSuccessor.isDirect = true;
        dmSuccessor.directUserId = QStringLiteral("@bob:example.org");
        dmSuccessor.directUserIds = { dmSuccessor.directUserId };
        RoomInfo dmOld = room(QStringLiteral("!dmOld:example.org"),
                              RoomInfo::Joined,
                              QStringLiteral("!dm2:example.org"));
        dmOld.isDirect = true;
        dmOld.directUserId = QStringLiteral("@bob:example.org");
        dmOld.directUserIds = { dmOld.directUserId };

        client.setRooms({ dmOld, dm, dmSuccessor,
                          room(QStringLiteral("!plain:example.org")) });
        QCoreApplication::processEvents();

        QStringList categories;
        for (int i = 0; i < model.rowCount(); ++i) {
            categories.append(model.data(model.index(i, 0),
                                         RoomListModel::CategoryRole).toString());
        }
        QVERIFY(!categories.isEmpty());
        QStringList runs;
        for (const QString &category : categories) {
            if (runs.isEmpty() || runs.last() != category)
                runs.append(category);
        }
        QSet<QString> seen(runs.begin(), runs.end());
        QCOMPARE(runs.size(), seen.size());
    }

    void supersededFlipNotifiesTheViewEvenThoughTheRowIsUnchanged()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        const RoomInfo oldRoom = room(kOld, RoomInfo::Joined, kNew);
        client.setRooms({ oldRoom });
        QCoreApplication::processEvents();

        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        client.setRooms({ oldRoom, room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();

        bool notified = false;
        for (const auto &args : changes) {
            const auto roles = args.at(2).value<QList<int>>();
            const int first = args.at(0).toModelIndex().row();
            const int last = args.at(1).toModelIndex().row();
            for (int r = first; r <= last; ++r) {
                if (model.data(model.index(r, 0), RoomListModel::RoomIdRole)
                        .toString() != kOld) {
                    continue;
                }
                if (roles.isEmpty()
                    || roles.contains(
                        RoomListModel::SupersededByAccessibleSuccessorRole)) {
                    notified = true;
                }
            }
        }
        QVERIFY2(notified,
                 "the upgraded row must be repainted when its successor "
                 "becomes reachable");
    }

    void roomInfoEqualityAccountsForTheUpgradeFields()
    {
        RoomInfo before = room(kOld);
        RoomInfo tombstoned = room(kOld, RoomInfo::Joined, kNew);
        QVERIFY2(before != tombstoned,
                 "a tombstone must make the room compare unequal");

        RoomInfo withPredecessor = room(kOld);
        withPredecessor.predecessorRoomId = QStringLiteral("!prev:example.org");
        QVERIFY(before != withPredecessor);
    }

    void predecessorLinkNeedsNoJoinAndIsIndependent()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined,
                                 QStringLiteral("!newer:example.org"), kOld) });
        h.upgrade.setRoomId(kNew);

        QCOMPARE(h.upgrade.predecessorRoomId(), kOld);
        QVERIFY2(h.upgrade.upgraded(),
                 "a room may be a successor AND be upgraded itself");

        h.upgrade.goToPredecessor();
        QCOMPARE(h.client.joinCalls, 0);
        QCOMPARE(h.navigations, QStringList{ kOld });
    }

    void predecessorLinkIsWithheldForARoomWeDoNotHold()
    {
        Harness h;
        h.client.setRooms({ room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kNew);
        QVERIFY(h.upgrade.predecessorRoomId().isEmpty());

        h.upgrade.goToPredecessor();
        QVERIFY(h.navigations.isEmpty());

        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCOMPARE(h.upgrade.predecessorRoomId(), kOld);
    }
};

QTEST_MAIN(RoomUpgradeTest)
#include "RoomUpgradeTest.moc"
