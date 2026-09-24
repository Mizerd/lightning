// Room upgrades (`m.room.tombstone`): RoomUpgradeController policy and the
// room list's de-emphasis rule.
//   * a tombstoned active room reports `upgraded` with its successor;
//   * a successor Lightning has never seen is Unknown, not NotAccessible, and
//     the banner still offers Continue;
//   * an already-joined successor navigates without joining;
//   * an invited/unknown successor is joined first and navigated to only once
//     the join settles;
//   * a failed join leaves the user in the old room with the reason shown;
//   * `chainVerified` requires the successor to point back at this room;
//   * the room list de-emphasizes an upgraded room only once its successor is
//     reachable, and demotes rather than filters it;
//   * a room switch and sign-out drop error/busy state, and a late answer for
//     the previous room never navigates the current one;
//   * observing a tombstone never changes the room or issues a join.
//
// Policy and wiring only; a real homeserver upgrade, a live tombstone over
// sync and Element interoperability are not tested here.

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
    // Upgrade (send side).
    int versionRequests = 0;
    QString lastUpgradeVersion;
    // The room /upgrade was asked to act on, recorded so a test can see which
    // room the controller targeted.
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

// The three objects under test, wired as AppController wires them: the
// upgrade controller drives Discover's join, and Discover's settled
// roomJoined navigates.
struct Harness {
    FakeClient client;
    RoomDiscoveryController discovery;
    RoomUpgradeController upgrade;
    QStringList navigations;

    Harness()
    {
        // Same order as AppController::setClient: connection order is
        // emission order, so the upgrade controller's loggedOut handler runs
        // first here, as in production.
        upgrade.setClient(&client);
        discovery.setClient(&client);
        upgrade.setDiscovery(&discovery);
        QObject::connect(&upgrade, &RoomUpgradeController::navigateRequested,
                         [this](const QString &id) { navigations.append(id); });
        // Mirrors AppController's real connection, including suppression, so
        // the roomJoinFinished -> finishWaitForRoom -> busyChanged ->
        // roomJoined -> suppression chain is covered.
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

} // namespace

class RoomUpgradeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Upgrading a room (send side).

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
        // A failed read empties the list rather than keeping a stale one.
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

    // An upgrade acts on the room the caller named, not the active room: Room
    // Information and Space settings can offer it for a room other than the
    // one open, and an upgrade cannot be undone.
    void anUpgradeDestroysTheRoomTheCallerNamedNotTheOpenOne()
    {
        const QString space = QStringLiteral("!space:example.org");
        Harness h;
        h.client.setRooms({ room(kOld), room(space) });
        // The reader is in kOld, which this controller tracks.
        h.upgrade.setRoomId(kOld);
        Q_EMIT h.client.roomVersionsReceived(
            true, QStringLiteral("10"),
            { QVariantMap{ { QStringLiteral("version"), QStringLiteral("10") },
                           { QStringLiteral("stable"), true } } });
        // Space settings upgrades the space while kOld stays open.
        h.upgrade.upgradeRoom(space, QStringLiteral("10"), false);
        QCOMPARE(h.client.lastUpgradeRoomId, space);
        QVERIFY(h.client.lastUpgradeRoomId != kOld);
    }

    // With no named room the upgrade refuses and says why, never guessing.
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

    // "The server upgraded the room but named no replacement" is not
    // "nothing changed": the old room is already tombstoned.
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
        // A stale answer (another op id) changes nothing.
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

    // An active room carrying a tombstone reports it.
    void tombstonedRoomReportsItsSuccessor()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        QVERIFY(h.upgrade.upgraded());
        QCOMPARE(h.upgrade.successorRoomId(), kNew);
        // A room with no tombstone does not claim one.
        h.upgrade.setRoomId(kNew);
        QVERIFY(!h.upgrade.upgraded());
        QVERIFY(h.upgrade.successorRoomId().isEmpty());
    }

    // An unknown successor is Unknown, never NotAccessible: claiming
    // inaccessibility we cannot show would hide a joinable room.
    void unknownSuccessorIsUnknownNotInaccessible()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::Unknown));
        // ...and the banner stays actionable.
        QVERIFY(h.upgrade.upgraded());
        QVERIFY(!h.upgrade.busy());
    }

    // A successor we hold but the user is not in is the one case we can call
    // inaccessible.
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

    // Already a member: navigate without joining.
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

    // Invited: join first and navigate only once the join settles, not on the
    // request.
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
        // The join is in flight.
        QVERIFY(h.navigations.isEmpty());

        // The server accepts, and the room turns up as joined.
        Q_EMIT h.client.roomJoinFinished(h.client.lastJoinOp, true, kNew,
                                         QString());
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QTRY_VERIFY_WITH_TIMEOUT(!h.navigations.isEmpty(), kSignalTimeoutMs);
        QCOMPARE(h.navigations.last(), kNew);
        QVERIFY(!h.upgrade.busy());
    }

    // A failed join leaves the user where they were, with the reason shown in
    // the old room.
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
        // No navigation, and the failure is stated.
        QVERIFY(h.navigations.isEmpty());
        QVERIFY(!h.upgrade.busy());
        // The category maps through Discover's own text, so the banner and the
        // Discover dialog describe a refusal the same way.
        QCOMPARE(h.upgrade.error(),
                 RoomDiscoveryController::describeJoinCategory(
                     QStringLiteral("banned")));
    }

    // An unverifiable chain is "not established yet" and still actionable; a
    // contradicted one is a warning sign.
    void chainVerificationRequiresThePointerBack()
    {
        Harness h;

        // Successor unknown: cannot verify, but still offered.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(!h.upgrade.chainVerified());
        QVERIFY(h.upgrade.upgraded());

        // Successor points back: established.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QVERIFY(h.upgrade.chainVerified());

        // Successor names a different predecessor: not this room's
        // replacement, whatever the tombstone says.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(),
                                 QStringLiteral("!other:example.org")) });
        QVERIFY(!h.upgrade.chainVerified());
    }

    // A room switch drops error and busy state; a late answer for the
    // previous room does not navigate the current one.
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

        // The previous room's join lands late and must not move the user.
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

    // Observing a tombstone, even live in the open room, never moves the user
    // or joins anything.
    void observingATombstoneNeverFollowsIt()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(!h.upgrade.upgraded());

        // The tombstone arrives while the user is in the room.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();

        QVERIFY(h.upgrade.upgraded());
        // ...and nothing happened to the user.
        QCOMPARE(h.client.joinCalls, 0);
        QVERIFY2(h.navigations.isEmpty(),
                 "a tombstone must never navigate by itself");
    }

    // navigateRequested is a direct connection to AppController::openRoom,
    // whose const-reference parameter aliases what the emitter passed, and
    // openRoom re-enters setRoomId (which clears m_successorRoomId). The
    // emitted argument must be a copy that survives that re-entry.
    void navigationArgumentSurvivesTheControllerReentering()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kOld);

        QString seenAfterReentry;
        QObject::connect(&h.upgrade, &RoomUpgradeController::navigateRequested,
                         [&](const QString &roomId) {
            // What openRoom does between its two reads of roomId.
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

    // A join the user walked away from does not navigate them back when it
    // settles: Continue means switch now, not whenever the wait resolves.
    void abandonedJoinDoesNotNavigateLater()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        // The user opens another room while the join is in flight.
        h.upgrade.setRoomId(QStringLiteral("!third:example.org"));
        QVERIFY(h.upgrade.consumeAbandonedJoin(kNew));
        // Consumed once: a later deliberate join of the same room navigates.
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
        // An unrelated room is never suppressed.
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kOld));
    }

    // The suppression token does not outlive its join: an abandoned join that
    // fails never emits roomJoined, so a lingering token would swallow a later
    // deliberate Continue. Driven end to end through the real signals.
    void abandonedJoinThatFailedDoesNotSwallowALaterSuccess()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        // 1. Press Continue; the join is slow.
        h.upgrade.continueToSuccessor();
        const quint64 firstOp = h.client.lastJoinOp;

        // 2. The user opens another room while it is in flight.
        h.upgrade.setRoomId(QStringLiteral("!third:example.org"));

        // 3. That join fails; no roomJoined follows.
        Q_EMIT h.client.roomJoinFinished(firstOp, false, QString(),
                                         QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        // 4. Back in the old room, Continue again, and this time it works.
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

    // Sign-out leaves no suppression token behind (it could only swallow the
    // next account's navigation). The upgrade controller's loggedOut handler
    // runs before Discover's (wiring order), while m_pendingJoinRoomId is still
    // set; the harness mirrors that order.
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

    // A join that settles while the user is still in the originating room is
    // not abandoned and navigates.
    void joinCompletedInPlaceIsNotSuppressed()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
    }

    // The banner does not show a refusal from another Discover operation
    // (cancelKnock() has no busy guard, so it can fail mid-upgrade).
    void unrelatedDiscoverFailureDoesNotSurfaceInTheBanner()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        // A knock withdrawal for a different room fails.
        Q_EMIT h.client.knockCancelFinished(
            h.client.nextOp++, false, QStringLiteral("!knocked:example.org"),
            QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        QVERIFY2(h.upgrade.error().isEmpty(),
                 "another operation's refusal must not appear in the banner");
        QVERIFY2(h.upgrade.busy(),
                 "and it must not resolve our still-pending join");
    }

    // Room list de-emphasis waits until the successor is actually reachable
    // and demotes rather than filters: the old room stays openable.
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

        // Tombstoned, successor unknown: nothing is de-emphasized.
        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")) });
        QCoreApplication::processEvents();
        int oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0, "the upgraded room must stay in the list");
        QVERIFY(!supersededAt(oldRow));

        // Successor present, joined and pointing back: now it counts.
        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")),
                          room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();
        oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0,
                 "de-emphasis must DEMOTE the old room, never remove it");
        QVERIFY(supersededAt(oldRow));
        // Demoted below the live room, not hidden.
        const int liveRow = rowFor(QStringLiteral("!live:example.org"));
        QVERIFY(liveRow >= 0);
        QVERIFY2(oldRow > liveRow, "a superseded room sorts below live rooms");

        // A successor naming another predecessor is not this room's
        // replacement.
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

    // Each category stays one contiguous run (RoomsPanel sections by
    // `category`), so a superseded room sinks within its own category rather
    // than forming another sort group.
    void demotionStaysInsideTheRoomsCategorySoSectionsRemainContiguous()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        RoomInfo dm = room(QStringLiteral("!dm:example.org"));
        dm.isDirect = true;
        dm.directUserId = QStringLiteral("@bob:example.org");
        dm.directUserIds = { dm.directUserId };
        // An upgraded DM whose successor is reachable.
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
        // Every category appears as exactly one contiguous run.
        QStringList runs;
        for (const QString &category : categories) {
            if (runs.isEmpty() || runs.last() != category)
                runs.append(category);
        }
        QSet<QString> seen(runs.begin(), runs.end());
        QCOMPARE(runs.size(), seen.size());
    }

    // The de-emphasis flips when the successor changes, while the old row's
    // RoomInfo is unchanged (so replaceRoom emits no dataChanged); an explicit
    // notification is required, which only the signal can show.
    void supersededFlipNotifiesTheViewEvenThoughTheRowIsUnchanged()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        const RoomInfo oldRoom = room(kOld, RoomInfo::Joined, kNew);
        client.setRooms({ oldRoom });
        QCoreApplication::processEvents();

        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        // Only the successor arrives; the old room's RoomInfo is unchanged.
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

    // RoomInfo::operator== includes the upgrade fields, or a newly tombstoned
    // room would compare equal to its old self and no banner would appear.
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

    // The predecessor link is independent of `upgraded`: a room can be both a
    // successor and a predecessor.
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

    // The predecessor link is offered only for a room we hold: it has no join
    // step, so for an unknown room it could only open an empty view.
    void predecessorLinkIsWithheldForARoomWeDoNotHold()
    {
        Harness h;
        h.client.setRooms({ room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kNew);
        QVERIFY(h.upgrade.predecessorRoomId().isEmpty());

        h.upgrade.goToPredecessor();
        QVERIFY(h.navigations.isEmpty());

        // Once the old room is present, the link appears.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCOMPARE(h.upgrade.predecessorRoomId(), kOld);
    }
};

QTEST_MAIN(RoomUpgradeTest)
#include "RoomUpgradeTest.moc"
