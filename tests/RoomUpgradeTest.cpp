// v0.7.x room upgrades (`m.room.tombstone`) — RoomUpgradeController policy
// and the room list's de-emphasis rule. Pins the banner-and-link contract:
//   * a tombstoned active room reports `upgraded` with its successor;
//   * a successor Lightning has never heard of is Unknown, NOT
//     NotAccessible — the banner still offers Continue;
//   * an already-joined successor navigates and performs NO join;
//   * an invited/unknown successor joins FIRST and navigates only once the
//     join has settled;
//   * a FAILED join leaves the user in the old room, with the reason shown;
//   * `chainVerified` requires the successor to point BACK at this room;
//   * the room list de-emphasizes an upgraded room only once its successor
//     is actually reachable, and DEMOTES rather than filters it;
//   * a room switch and sign-out drop error/busy state, and a late answer
//     for the previous room can never navigate the current one;
//   * nothing follows an upgrade on its own — observing a tombstone never
//     changes the current room and never issues a join.
//
// HONEST SCOPE: policy and wiring only. A real homeserver upgrading a room,
// a real `m.room.tombstone` arriving over sync, the successor's own
// `m.room.create.predecessor` as a live server would populate it, and
// Element interoperability are NOT exercised here and are NOT TESTED.

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
    // v0.9 send side.
    int versionRequests = 0;
    QString lastUpgradeVersion;
    quint64 lastUpgradeOp = 0;
    bool refuseUpgrades = false;
    void requestRoomVersions() override { ++versionRequests; }
    quint64 upgradeRoom(const QString &, const QString &newVersion) override
    {
        if (refuseUpgrades)
            return 0;
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

// The three objects under test, wired exactly as AppController wires them:
// the upgrade controller drives Discover's join, and Discover's settled
// roomJoined is what actually navigates.
struct Harness {
    FakeClient client;
    RoomDiscoveryController discovery;
    RoomUpgradeController upgrade;
    QStringList navigations;

    Harness()
    {
        // Same ORDER as AppController::setClient (AppController.cpp:460
        // then :473). Connection order is emission order, so the upgrade
        // controller's loggedOut handler must run FIRST here exactly as it
        // does in production — wiring these the other way round quietly
        // changes which handler sees m_pendingJoinRoomId still populated.
        upgrade.setClient(&client);
        discovery.setClient(&client);
        upgrade.setDiscovery(&discovery);
        QObject::connect(&upgrade, &RoomUpgradeController::navigateRequested,
                         [this](const QString &id) { navigations.append(id); });
        // Mirrors AppController's real connection, suppression included —
        // testing consumeAbandonedJoin as a bare predicate would leave the
        // wiring itself, and the whole roomJoinFinished -> finishWaitForRoom
        // -> busyChanged -> roomJoined -> suppression chain, uncovered.
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
    // 1. The basic fact: an active room carrying a tombstone reports it.
    // ── v0.9 send side (phase 8) ──────────────────────────────────────

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
        h.upgrade.upgradeRoom(QStringLiteral("99"), false);
        QVERIFY(h.client.lastUpgradeVersion.isEmpty());
        QVERIFY(!h.upgrade.upgradeError().isEmpty());
        QVERIFY(!h.upgrade.upgradeBusy());
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
        h.upgrade.upgradeRoom(QStringLiteral("10"), false);
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
        h.upgrade.upgradeRoom(QStringLiteral("10"), false);
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
        // A room with no tombstone must not claim one.
        h.upgrade.setRoomId(kNew);
        QVERIFY(!h.upgrade.upgraded());
        QVERIFY(h.upgrade.successorRoomId().isEmpty());
    }

    // 2. The honesty rule that matters most: not knowing the successor is
    // Unknown, never NotAccessible. Claiming inaccessibility we cannot
    // demonstrate would hide a room the user could have joined.
    void unknownSuccessorIsUnknownNotInaccessible()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);

        QCOMPARE(h.upgrade.successorAccess(),
                 int(RoomUpgradeController::Unknown));
        // ...and the banner is still actionable, which is the point.
        QVERIFY(h.upgrade.upgraded());
        QVERIFY(!h.upgrade.busy());
    }

    // A successor we DO hold, that the user is not in, is the one case we
    // can honestly call inaccessible.
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

    // 3. Already a member: navigate, and issue no join at all.
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

    // 4. Invited: join FIRST, and navigate only once the join has settled.
    // Navigating on the request rather than the answer would drop the user
    // into a room they are not yet in.
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
        // Nothing yet: the join is in flight.
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

    // 5. The contract's hard requirement: a failed join must leave the user
    // exactly where they were, with the reason visible in the old room.
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
        // No navigation happened, and the failure is stated rather than
        // swallowed.
        QVERIFY(h.navigations.isEmpty());
        QVERIFY(!h.upgrade.busy());
        // The category maps through Discover's own text, so the banner and
        // the Discover dialog cannot describe the same refusal differently.
        QCOMPARE(h.upgrade.error(),
                 RoomDiscoveryController::describeJoinCategory(
                     QStringLiteral("banned")));
    }

    // 6. The defensive check the maintainer asked for. An unverifiable
    // chain is "not established yet" and still actionable; a CONTRADICTED
    // one is evidence something is wrong.
    void chainVerificationRequiresThePointerBack()
    {
        Harness h;

        // Successor unknown -> cannot verify, but still offered.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        QVERIFY(!h.upgrade.chainVerified());
        QVERIFY(h.upgrade.upgraded());

        // Successor points back -> established.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        QVERIFY(h.upgrade.chainVerified());

        // Successor names a DIFFERENT predecessor -> not this room's
        // replacement, whatever the tombstone claims.
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(),
                                 QStringLiteral("!other:example.org")) });
        QVERIFY(!h.upgrade.chainVerified());
    }

    // 8. A room switch drops error and busy state; a late answer belonging
    // to the previous room must not navigate the current one.
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

        // The previous room's join lands late. It must not navigate us out
        // of the room the user just opened.
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

    // 9. The whole point of banner-and-link. Merely observing a tombstone —
    // including one that arrives live for the room the user is reading —
    // must never move them or join anything.
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
        // ...and absolutely nothing happened to the user.
        QCOMPARE(h.client.joinCalls, 0);
        QVERIFY2(h.navigations.isEmpty(),
                 "a tombstone must never navigate by itself");
    }

    // navigateRequested is a DIRECT connection to AppController::openRoom,
    // whose `const QString &` parameter aliases whatever the emitter passed.
    // openRoom calls setCurrentRoomId, which calls our setRoomId, whose
    // refresh() clears m_successorRoomId — so emitting the member itself
    // left openRoom's own parameter reading empty for the rest of the
    // function, skipping openRoomTimeline and leaving the successor's
    // timeline never opened. The harness's other slots copy the argument
    // immediately and therefore cannot see this; this one re-enters the
    // controller exactly as openRoom really does.
    void navigationArgumentSurvivesTheControllerReentering()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                            room(kNew, RoomInfo::Joined, QString(), kOld) });
        h.upgrade.setRoomId(kOld);

        QString seenAfterReentry;
        QObject::connect(&h.upgrade, &RoomUpgradeController::navigateRequested,
                         [&](const QString &roomId) {
            // Exactly what openRoom does between reading roomId the first
            // time and reading it again for openRoomTimeline.
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

    // A join the banner started and the user then walked away from must not
    // navigate them back when it settles. Pressing Continue is consent to
    // switch NOW, not whenever the bounded wait resolves.
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
        // Consumed once: a later deliberate join of the same room still
        // navigates normally.
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
        // An unrelated room is never suppressed.
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kOld));
    }

    // The suppression token must not outlive the join it belongs to. An
    // abandoned join that FAILS emits no roomJoined ever, so a token kept
    // past that failure would sit there and swallow the navigation of a
    // later, deliberate Continue — the user presses the button, the join
    // succeeds, and nothing happens. Driven end to end through the real
    // signals, not by calling the predicate directly.
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

        // 3. That join then FAILS. No roomJoined will ever follow it.
        Q_EMIT h.client.roomJoinFinished(firstOp, false, QString(),
                                         QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        // 4. Back in the old room, press Continue again — and this time it
        //    works. The user must actually end up in the new room.
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

    // Sign-out must not leave a token behind: Discover cannot emit a late
    // roomJoined across it, so the token protects nothing and could only
    // swallow the NEXT account's navigation.
    //
    // AppController wires the upgrade controller's client BEFORE Discover's
    // (AppController.cpp:460 then :473), and connection order is emission
    // order — so on sign-out OUR loggedOut handler runs first, while
    // m_pendingJoinRoomId is still populated. The version that SET a token
    // there therefore left real account-scoped residue, able to swallow the
    // next account's navigation; it was not dead code. The harness above
    // mirrors that order deliberately, so this case exercises the
    // production path rather than an accidental one — and it FAILS against
    // the version that set the token, which an earlier harness (wired in
    // the opposite order) could not show.
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

    // A join that settles while the user is still in the room they started
    // it from is NOT abandoned and must navigate.
    void joinCompletedInPlaceIsNotSuppressed()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(!h.upgrade.consumeAbandonedJoin(kNew));
    }

    // The banner must not display a refusal that belonged to some OTHER
    // Discover operation. cancelKnock() has no busy guard, so withdrawing a
    // knock while an upgrade join is in flight used to paint that unrelated
    // failure into the banner and swallow the real answer.
    void unrelatedDiscoverFailureDoesNotSurfaceInTheBanner()
    {
        Harness h;
        h.client.setRooms({ room(kOld, RoomInfo::Joined, kNew) });
        h.upgrade.setRoomId(kOld);
        h.upgrade.continueToSuccessor();
        QVERIFY(h.upgrade.busy());

        // A knock withdrawal for a completely different room fails.
        Q_EMIT h.client.knockCancelFinished(
            h.client.nextOp++, false, QStringLiteral("!knocked:example.org"),
            QStringLiteral("forbidden"));
        QCoreApplication::processEvents();

        QVERIFY2(h.upgrade.error().isEmpty(),
                 "another operation's refusal must not appear in the banner");
        QVERIFY2(h.upgrade.busy(),
                 "and it must not resolve our still-pending join");
    }

    // 7. The room list. De-emphasis waits for the successor to be ACTUALLY
    // reachable, and it is a demotion — never a filter, because the old
    // room has to stay openable and readable.
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

        // Tombstoned, successor unknown: nothing is de-emphasized, because
        // we cannot show the user can reach the replacement.
        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")) });
        QCoreApplication::processEvents();
        int oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0, "the upgraded room must stay in the list");
        QVERIFY(!supersededAt(oldRow));

        // Successor present and joined, and it points back: now it counts.
        client.setRooms({ room(kOld, RoomInfo::Joined, kNew),
                          room(QStringLiteral("!live:example.org")),
                          room(kNew, RoomInfo::Joined, QString(), kOld) });
        QCoreApplication::processEvents();
        oldRow = rowFor(kOld);
        QVERIFY2(oldRow >= 0,
                 "de-emphasis must DEMOTE the old room, never remove it");
        QVERIFY(supersededAt(oldRow));
        // Demoted below the live room rather than hidden.
        const int liveRow = rowFor(QStringLiteral("!live:example.org"));
        QVERIFY(liveRow >= 0);
        QVERIFY2(oldRow > liveRow, "a superseded room sorts below live rooms");

        // A successor naming someone else's predecessor is not this room's
        // replacement, so the row is left alone.
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

    // RoomsPanel sections the list by the `category` role, so each category
    // must stay ONE contiguous run. A superseded room therefore sinks within
    // its own category, never below every other room — a fourth top-level
    // sort group put a second "PEOPLE"/"ROOMS" header at the bottom of the
    // list and demoted a superseded INVITE out of the top block.
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

    // The de-emphasis flips when the SUCCESSOR changes, not the old room —
    // so the old row's own RoomInfo is identical across the change and
    // replaceRoom's equality check skips its dataChanged. Without an
    // explicit notification the chip would not appear until something
    // unrelated about the old room happened to change. Reading data()
    // cannot catch that; only the signal can.
    void supersededFlipNotifiesTheViewEvenThoughTheRowIsUnchanged()
    {
        FakeClient client;
        RoomListModel model;
        model.setClient(&client);

        const RoomInfo oldRoom = room(kOld, RoomInfo::Joined, kNew);
        client.setRooms({ oldRoom });
        QCoreApplication::processEvents();

        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        // Only the successor arrives. The old room's RoomInfo is byte-for-byte
        // what it already was.
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

    // The same trap one layer down: if RoomInfo::operator== ignored the new
    // fields, a room that JUST got tombstoned would compare equal to its
    // pre-tombstone self and the banner would not appear at all.
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

    // The predecessor link is independent of `upgraded`: a room can be both
    // someone's successor and someone else's predecessor.
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

    // ...but only for a predecessor we actually hold. There is no join step
    // on that link, so offering it for a room we have no record of — a user
    // who joined the successor without ever being in the old room — could
    // only ever open an empty view.
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
