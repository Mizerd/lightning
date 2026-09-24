// The call ring policy wired to its real owners on a full AppController (mock
// backend): ignored senders, muted rooms, backlog suppression from the sync
// lifecycle, and the ringForCalls setting. CallController's own tests use
// injected functors; this proves the production wiring exists.
#include <QtTest/QtTest>

#include <QSignalSpy>

#include "app/AppController.h"
#include "app/ModerationController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/SfuCallController.h"
#include "matrix/CallSignal.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "notifications/NotificationManager.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
const QString kRoom = QStringLiteral("!general:mock.local");

CallSignal invite(const QString &callId,
                  const QString &sender = QStringLiteral("@peer:mock.local"))
{
    CallSignal s;
    s.kind = CallSignal::Kind::Invite;
    s.roomId = kRoom;
    s.eventId = QStringLiteral("$invite-") + callId;
    s.sender = sender;
    s.callId = callId;
    s.partyId = QStringLiteral("peer-party");
    s.lifetimeMs = 60000;
    s.originServerTs = QDateTime::currentMSecsSinceEpoch();
    s.version = QStringLiteral("1");
    s.sessionType = QStringLiteral("offer");
    s.hasDescription = true;
    return s;
}
} // namespace

class CallRingPolicyTest : public QObject
{
    Q_OBJECT

private:
    static bool login(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        return loginSpy.wait(kSignalTimeoutMs);
    }

    static MockMatrixClient *mock(AppController &controller)
    {
        return controller.findChild<MockMatrixClient *>();
    }

private Q_SLOTS:
    // A notification action must not act under the wrong account: a card can
    // outlive the account that raised it, and acting under the current account
    // would mark another account's room read or reply as the wrong identity.
    // The guard is AppController's; NotificationManagerTest proves the payload
    // carries the raising account.
    void aNotificationActionUnderTheWrongAccountIsRefused()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        const int noticesBefore = notifications->genericNoticeCountForTest();

        // A card raised for an account that is not signed in.
        Q_EMIT notifications->replyRequested(
            QStringLiteral("@someone-else:other.example"),
            QStringLiteral("!general:mock.local"), QString(),
            QStringLiteral("this must not be sent"));

        // Refused, and the user is told.
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 1);

        Q_EMIT notifications->markReadRequested(
            QStringLiteral("@someone-else:other.example"),
            QStringLiteral("!general:mock.local"), QString());
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 2);

        // An empty account on the payload is refused too (fail closed).
        Q_EMIT notifications->replyRequested(
            QString(), QStringLiteral("!general:mock.local"), QString(),
            QStringLiteral("nor this"));
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 3);

        // The matching account is not refused, so the guard is not vacuous.
        const QString own = controller.auth()->currentUserId();
        QVERIFY(!own.isEmpty());
        Q_EMIT notifications->markReadRequested(
            own, QStringLiteral("!general:mock.local"), QString());
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 3);
    }

    void callLanePrefersMatrixRtcAndFallsBackToDmOnlyLegacy()
    {
        // Lane selection lives here, not in QML:
        //  * MatrixRTC is primary where it can carry a call (video, screen
        //    share, groups; what current Element speaks);
        //  * the legacy fallback is 1:1 DMs only, since m.call.invite rings
        //    every room member.
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));

        // The mock has no MatrixRTC and no media engine, so neither lane is
        // available: no button.
        QVERIFY(!controller.canStartCall(QStringLiteral("!general:mock.local")));
        QCOMPARE(controller.preferredCallLane(
                     QStringLiteral("!general:mock.local")),
                 QString());

        // An empty room id is never callable.
        QVERIFY(!controller.canStartCall(QString()));

        // Refusing says why.
        QSignalSpy refused(&controller, &AppController::callStartRefused);
        QVERIFY(!controller.startCall(QStringLiteral("!general:mock.local")));
        QCOMPARE(refused.count(), 1);
        QVERIFY(!refused.at(0).at(0).toString().isEmpty());
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-ring-policy-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void backlogGateFollowsInitialSyncState()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        // The mock reports initialSyncDone after login, so the wired backlog
        // gate is open.
        QVERIFY(controller.calls()->shouldRing());
    }

    void mutedRoomRingsStateButNotPolicy()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        controller.settings()->setRoomNotificationMode(kRoom, 2 /*Muted*/);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QVERIFY(!controller.calls()->shouldRing());
        // Un-muting reopens the gate live (the functor reads current state).
        controller.settings()->setRoomNotificationMode(kRoom, 0);
        QVERIFY(controller.calls()->shouldRing());
    }

    void ignoredSenderNeverEvenRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        // Seed the moderation cache as the SDK list arrival does.
        Q_EMIT client->ignoredUsersChanged(
            QStringList{ QStringLiteral("@peer:mock.local") });
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.moderation()->isIgnored(
                QStringLiteral("@peer:mock.local")),
            kSignalTimeoutMs);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        // Dropped before any state: the ignore check reached CallController.
        QCOMPARE(controller.calls()->state(), CallController::State::Idle);
    }

    void ringForCallsSettingRoundTrips()
    {
        AppController controller(AppController::MockBackend);
        QCOMPARE(controller.settings()->ringForCalls(), true); // default ON
        QSignalSpy changed(controller.settings(),
                           &SettingsManager::ringForCallsChanged);
        controller.settings()->setRingForCalls(false);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(controller.settings()->ringForCalls(), false);
        controller.settings()->setRingForCalls(false); // no-op
        QCOMPARE(changed.count(), 1);
    }

    // The automatic pop-out on minimise is opt-in; the call bar's manual
    // pop-out is unaffected.
    void theAutomaticPopOutIsOptIn()
    {
        AppController controller(AppController::MockBackend);
        QCOMPARE(controller.settings()->callPictureInPicture(), false);
        QSignalSpy changed(controller.settings(),
                           &SettingsManager::callPictureInPictureChanged);
        controller.settings()->setCallPictureInPicture(true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(controller.settings()->callPictureInPicture(), true);
    }

    void missedCallClassificationIsExact()
    {
        using ER = CallController::EndReason;
        QVERIFY(CallController::isMissedCallReason(ER::InviteTimeout));
        QVERIFY(CallController::isMissedCallReason(ER::RemoteHangup));
        QVERIFY(!CallController::isMissedCallReason(ER::LocalReject));
        QVERIFY(!CallController::isMissedCallReason(ER::AnsweredElsewhere));
        QVERIFY(!CallController::isMissedCallReason(ER::DeclinedElsewhere));
        QVERIFY(!CallController::isMissedCallReason(ER::SessionLost));
        QVERIFY(!CallController::isMissedCallReason(ER::GlareReplaced));
        QVERIFY(!CallController::isMissedCallReason(ER::MediaFailed));
    }

    // Missed-call notices require the ring to have been announced: a call
    // suppressed by mute, backlog or ignore never resurfaces as "missed".
    void missedNoticeOnlyForAnnouncedRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();

        // Announced ring, caller gives up → exactly one missed notice.
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        const int before = notices->genericNoticeCountForTest();
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = kRoom;
        hangup.eventId = QStringLiteral("$hangup-1");
        hangup.sender = QStringLiteral("@peer:mock.local");
        hangup.callId = QStringLiteral("call-1");
        hangup.partyId = QStringLiteral("peer-party");
        hangup.reason = QStringLiteral("user_hangup");
        client->emitCallSignalForTest(hangup);
        QCOMPARE(notices->genericNoticeCountForTest(), before + 1);

        // Muted room: never announced, so no missed notice.
        controller.settings()->setRoomNotificationMode(kRoom, 2 /*Muted*/);
        // A fresh sender, so only the mute can explain the missing
        // announcement.
        client->emitCallSignalForTest(
            invite(QStringLiteral("call-2"),
                   QStringLiteral("@peer2:mock.local")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        const int muted = notices->genericNoticeCountForTest();
        CallSignal hangup2 = hangup;
        hangup2.eventId = QStringLiteral("$hangup-2");
        hangup2.callId = QStringLiteral("call-2");
        hangup2.sender = QStringLiteral("@peer2:mock.local");
        client->emitCallSignalForTest(hangup2);
        QCOMPARE(notices->genericNoticeCountForTest(), muted);
    }

    // The per-sender ring cooldown bounds OS announcements: a quick re-ring
    // still produces call state but no second notification or missed notice.
    void senderRingCooldownBoundsAnnouncements()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();

        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QVERIFY(controller.calls()->rejectIncoming());
        const int before = notices->genericNoticeCountForTest();

        // Same sender again: state rings, the announcement is suppressed, so
        // no missed notice either.
        client->emitCallSignalForTest(invite(QStringLiteral("call-2")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = kRoom;
        hangup.eventId = QStringLiteral("$hangup-x");
        hangup.sender = QStringLiteral("@peer:mock.local");
        hangup.callId = QStringLiteral("call-2");
        hangup.partyId = QStringLiteral("peer-party");
        hangup.reason = QStringLiteral("user_hangup");
        client->emitCallSignalForTest(hangup);
        QCOMPARE(notices->genericNoticeCountForTest(), before);
    }

    void declineFromNotificationReachesTheCall()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        // No D-Bus daemon offscreen, so emit the decline signal directly: the
        // glue must route it to rejectIncoming for the matching call only.
        Q_EMIT controller.notificationsForTest()->callDeclineRequested(
            QStringLiteral("some-other-call"));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        Q_EMIT controller.notificationsForTest()->callDeclineRequested(
            QStringLiteral("call-1"));
        QCOMPARE(controller.calls()->state(), CallController::State::Ended);
        QCOMPARE(controller.calls()->endReason(),
                 CallController::EndReason::LocalReject);
    }

    // `canStartCall()` is a Q_INVOKABLE, so a binding on it records no
    // dependency, and its answer rides asynchronous RtcController state. Both
    // call gates read `callGateRevision`; this pins that it moves.
    void theCallGateRevisionMovesWithTheStateItsAnswerDependsOn()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QVERIFY(controller.rtc());
        QSignalSpy bumped(&controller,
                          &AppController::callGateRevisionChanged);
        const int before = controller.callGateRevision();

        // Transport availability, the first thing canStartCall consults.
        controller.rtc()->setMediaAvailable(
            !controller.rtc()->mediaAvailable());
        QVERIFY2(controller.callGateRevision() > before,
                 "RTC availability changed and the call gate's revision did "
                 "not move, so every binding on canStartCall() keeps the "
                 "answer it had at room-open");
        QVERIFY(bumped.count() >= 1);

        // ...and one room's session, which arrives later.
        const int afterAvailability = controller.callGateRevision();
        controller.rtc()->setRoomEncrypted(QStringLiteral("!sess:mock.local"),
                                           true);
        QVERIFY2(controller.callGateRevision() > afterAvailability,
                 "a room's session changed and the call gate's revision did "
                 "not move");
    }

    // The call lane must know a room's encryption without the room being
    // opened: IncomingCallPrompt's Join opens nothing, and a missing entry
    // falls back to "encrypted", so an unencrypted call gets one-way audio.
    // This case never opens the room.
    void theCallLaneSeesARoomsEncryptionWithoutOpeningIt()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QVERIFY(controller.rtc());
        // The mock reports this room as known-unencrypted; asserted so the
        // case cannot pass on broken code if the fixture changes.
        const QVariantMap room = controller.roomList()->findRoom(kRoom);
        QVERIFY2(!room.isEmpty(), "the fixture room is not in the room list");
        QVERIFY2(room.value(QStringLiteral("encryptionKnown")).toBool(),
                 "the fixture room's encryption is UNKNOWN, so fail-closed "
                 "is the correct answer and this case proves nothing");
        QVERIFY(!room.value(QStringLiteral("encrypted")).toBool());

        QCOMPARE(controller.currentRoomId(), QString());
        QVERIFY2(!controller.rtc()->roomEncrypted(kRoom),
                 "a room the client knows is unencrypted still read as "
                 "encrypted because nothing had opened it — so a call "
                 "answered from the incoming-call card requires encryption "
                 "the caller is not using, and drops all of their media");
    }

    // Opening a room before its encryption state has synced must not pin it
    // as encrypted. Recording `!known || encrypted` turns unknown into a
    // known "encrypted", and setRoomEncrypted's downgrade guard (encryption
    // cannot be removed in Matrix) then refuses the real answer.
    void openingARoomBeforeItsEncryptionIsKnownDoesNotPinItEncrypted()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        // The mock gives a freshly joined room `encryptionKnown = false`.
        const QString fresh = QStringLiteral("!fresh:mock.local");
        QVERIFY(client->joinRoomByIdOrAlias(fresh, {}) != 0);
        QTRY_VERIFY(!controller.roomList()->findRoom(fresh).isEmpty());
        QVERIFY2(!controller.roomList()
                      ->findRoom(fresh)
                      .value(QStringLiteral("encryptionKnown"))
                      .toBool(),
                 "the fixture room's encryption is already KNOWN, so this "
                 "case never enters the window it is written for");

        controller.openRoom(fresh);
        QTRY_COMPARE(controller.currentRoomId(), fresh);
        // Unknown still fails closed.
        QVERIFY2(controller.rtc()->roomEncrypted(fresh),
                 "an unknown room stopped failing closed, which is the "
                 "silent downgrade §6 forbids");

        // ...and then the first known answer arrives.
        controller.rtc()->setRoomEncrypted(fresh, false);
        QVERIFY2(!controller.rtc()->roomEncrypted(fresh),
                 "opening the room while its encryption was unknown pinned "
                 "it as encrypted for the session, so every call in it "
                 "required encryption no peer was using");
    }

    // The notification's Answer action reaches the call and opens its room.
    // Starts with no room open, so "opened the right room" and "did nothing"
    // are distinguishable.
    void acceptFromNotificationOpensTheCallsRoom()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        QCOMPARE(controller.currentRoomId(), QString());
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        // A stale card from an earlier call must not answer this one.
        Q_EMIT controller.notificationsForTest()->callAcceptRequested(
            QStringLiteral("some-other-call"));
        QCOMPARE(controller.currentRoomId(), QString());
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        Q_EMIT controller.notificationsForTest()->callAcceptRequested(
            QStringLiteral("call-1"));
        QVERIFY2(!controller.currentRoomId().isEmpty(),
                 "Answer on the notification left the user wherever they "
                 "were: the room is opened BEFORE the answer is attempted "
                 "precisely so a refusal is visible");
        QCOMPARE(controller.currentRoomId(),
                 controller.calls()->activeRoomId());
    }

    // A withdrawn call refusal clears the status strip. SfuCallController
    // withdraws by emitting `callFailed` with an empty reason, and an empty
    // `errorReported` is how AppController clears the strip. Driven through
    // the real signal so the production connection is what is tested.
    void aWithdrawnCallRefusalReachesTheStatusStrip()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QSignalSpy errors(&controller, &AppController::errorReported);
        QVERIFY(errors.isValid());

        auto *call = controller.groupCall();
        QVERIFY(call);
        QVERIFY(QMetaObject::invokeMethod(
            call, "callFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("You cannot join calls here."))));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0).at(0).toString(),
                 QStringLiteral("You cannot join calls here."));

        QVERIFY(QMetaObject::invokeMethod(call, "callFailed",
                                          Qt::DirectConnection,
                                          Q_ARG(QString, QString{})));
        QVERIFY2(errors.size() == 2,
                 "the withdrawal never reached the status strip, so a refusal "
                 "from an earlier attempt goes on being displayed over a call "
                 "the user successfully joined");
        QVERIFY2(errors.at(1).at(0).toString().isEmpty(),
                 "the withdrawal arrived carrying text, which would replace "
                 "one stale sentence with another instead of clearing it");
    }

    // A notification click must open the room (app.openRoom()), not merely set
    // currentRoomId: openRoom() is the only caller of openRoomTimeline(), so
    // otherwise the room never gets a live SDK timeline or history. The SDK
    // open is not observable on the mock, so this checks openRoom()'s other
    // effect: leaving the Settings screen. Covers the desktop notification and
    // the Activity Center row, which share the handler.
    void aNotificationClickOpensTheRoomRatherThanJustSelectingIt()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        // Start away from the room, on a screen only openRoom() leaves.
        // (Main.qml calls showMain() first; this case is about C++ routing.)
        controller.setCurrentRoomId(QString());
        controller.showSettings();
        QCOMPARE(controller.currentScreen(), AppController::SettingsScreen);

        Q_EMIT notifications->openRequested(kRoom, QStringLiteral("$ev:mock"),
                                            QString());

        QCOMPARE(controller.currentRoomId(), kRoom);
        QVERIFY2(controller.currentScreen() == AppController::MainScreen,
                 "a notification click did not go through openRoom(), so the "
                 "room was SELECTED and never OPENED: on the Rust backend no "
                 "SDK timeline is subscribed for it, the room shows only the "
                 "background sync mirror and can never paginate");

        // The Activity Center row takes the same route.
        controller.setCurrentRoomId(QString());
        controller.showSettings();
        auto *activity = controller.activity();
        QVERIFY(activity);
        QVERIFY(QMetaObject::invokeMethod(
            activity, "openRequested", Qt::DirectConnection,
            Q_ARG(QString, kRoom), Q_ARG(QString, QStringLiteral("$ev:mock")),
            Q_ARG(QString, QString{})));
        QCOMPARE(controller.currentRoomId(), kRoom);
        QVERIFY2(controller.currentScreen() == AppController::MainScreen,
                 "an Activity Center row selects the room without opening it, "
                 "which is the same broken half-state a notification click "
                 "produced");
    }

    // A thread notification must not hand the composite timeline id to
    // openRoom(). openRoomTimeline() has no composite guard, and the Rust side
    // closes the current room's timeline before failing to parse the id.
    // Fails on no open, on re-emitting the composite, and on an unguarded open.
    void aTimelineIdIsNeverHandedToTheRoomOpener()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        // Start with no room open, so the reduction is positively observable.
        controller.setCurrentRoomId(QString{});
        QVERIFY(controller.currentRoomId().isEmpty());

        QSignalSpy routed(&controller,
                          &AppController::notificationOpenRequested);
        const QString root = QStringLiteral("$root:mock.local");
        const QString composite = MatrixClient::threadTimelineId(kRoom, root);
        QVERIFY(MatrixClient::isThreadTimelineId(composite));
        // No root argument: the composite is the only place it exists
        // (routeNotificationOpen()'s recovery branch).
        Q_EMIT notifications->openRequested(composite, QStringLiteral("$ev:mock"),
                                            QString{});

        QVERIFY2(controller.currentRoomId() == kRoom,
                 qPrintable(QStringLiteral(
                     "a notification for a thread opened \"%1\" instead of "
                     "its room: an empty result means the composite was "
                     "refused and nothing opened, and the composite itself "
                     "means it reached the opener — which on the Rust backend "
                     "closes the reader's live timeline and then fails to "
                     "parse")
                     .arg(controller.currentRoomId())));
        QCOMPARE(routed.count(), 1);
        QCOMPARE(routed.at(0).at(0).toString(), kRoom);
        QCOMPARE(routed.at(0).at(2).toString(), root);
    }


private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallRingPolicyTest)
#include "CallRingPolicyTest.moc"
