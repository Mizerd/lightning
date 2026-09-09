// 2026-08-18 round 2: the voice-call ring policy wired to its REAL owners
// on a full AppController (mock backend) — ignored senders, muted rooms,
// backlog suppression from the sync lifecycle, and the ringForCalls
// setting's plumbing. The prior round proved these gates inside
// CallController with hand-injected functors; this suite proves the
// production wiring exists (the review lesson: a policy hook nobody wires
// is dead code covered by a passing test).
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
    // A NOTIFICATION ACTION MUST NOT ACT UNDER THE WRONG ACCOUNT.
    //
    // This is the round's headline safety property and it rests on four
    // lines. A notification card outlives the account that raised it: the
    // user can switch accounts, or sign out, while it is on screen, and the
    // desktop delivers the action minutes later. Acting under whichever
    // account is current would mark ANOTHER account's room read, or send a
    // reply from the wrong identity into a room the current account may not
    // even be in — and it would SUCCEED, so nothing would report it.
    //
    // Lives here rather than in NotificationManagerTest because the guard is
    // AppController's: that suite proves the payload carries the raising
    // account, which is a different claim from the guard consulting it.
    void aNotificationActionUnderTheWrongAccountIsRefused()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        const int noticesBefore = notifications->genericNoticeCountForTest();

        // A card raised for an account that is NOT the one signed in.
        Q_EMIT notifications->replyRequested(
            QStringLiteral("@someone-else:other.example"),
            QStringLiteral("!general:mock.local"), QString(),
            QStringLiteral("this must not be sent"));

        // Refused, and SAID SO — a button that does nothing and explains
        // nothing is worse, because the user believes they have replied.
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 1);

        Q_EMIT notifications->markReadRequested(
            QStringLiteral("@someone-else:other.example"),
            QStringLiteral("!general:mock.local"), QString());
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 2);

        // An EMPTY account on the payload is refused too. That is the state
        // a card raised before the account was known would carry, and
        // fail-open there would be the same defect with no attacker needed.
        Q_EMIT notifications->replyRequested(
            QString(), QStringLiteral("!general:mock.local"), QString(),
            QStringLiteral("nor this"));
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 3);

        // ...and the MATCHING account is not refused: the guard must not be
        // vacuously true, which is how it would pass while blocking
        // everything.
        const QString own = controller.auth()->currentUserId();
        QVERIFY(!own.isEmpty());
        Q_EMIT notifications->markReadRequested(
            own, QStringLiteral("!general:mock.local"), QString());
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 3);
    }

    void callLanePrefersMatrixRtcAndFallsBackToDmOnlyLegacy()
    {
        // Lane selection is ONE policy question and it lives here, not in
        // QML. Two rules it must never lose:
        //
        //  * MatrixRTC is PRIMARY where it can carry a call — it is what
        //    current Element speaks, and it is the only lane with video,
        //    screen share and groups.
        //  * The legacy fallback is 1:1 DMs ONLY, because a legacy
        //    m.call.invite rings EVERY member of a room. Offering it in a
        //    group room would ring everyone.
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));

        // The mock backend has no MatrixRTC and registers no media engine,
        // so neither lane can carry a call: no button, and no pretending.
        QVERIFY(!controller.canStartCall(QStringLiteral("!general:mock.local")));
        QCOMPARE(controller.preferredCallLane(
                     QStringLiteral("!general:mock.local")),
                 QString());

        // An empty room id is never callable.
        QVERIFY(!controller.canStartCall(QString()));

        // Refusing must SAY why rather than failing silently.
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
        // The mock reports initialSyncDone true after login, so the wired
        // backlog gate must be OPEN — the prior round's default-closed
        // behavior would keep shouldRing false forever here.
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
        // Un-muting reopens the gate live (functor reads current state).
        controller.settings()->setRoomNotificationMode(kRoom, 0);
        QVERIFY(controller.calls()->shouldRing());
    }

    void ignoredSenderNeverEvenRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        // Seed the moderation cache the way the SDK list arrival does.
        Q_EMIT client->ignoredUsersChanged(
            QStringList{ QStringLiteral("@peer:mock.local") });
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.moderation()->isIgnored(
                QStringLiteral("@peer:mock.local")),
            kSignalTimeoutMs);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        // Dropped before any state: the wired ignore check reached
        // CallController.
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

    // 2026-09-05: the automatic pop-out shipped ON and fired on every
    // minimise — a window appearing on its own for a reader who only wanted
    // the app out of the way. It is the opt-in now; the call bar's manual
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

    // Missed-call notices require the ring to have been ANNOUNCED: a call
    // suppressed by mute (or backlog/ignore) must never resurface later
    // as "missed" (review round 2).
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

        // Muted room: the ring is never announced, so its end must raise
        // NO missed notice.
        controller.settings()->setRoomNotificationMode(kRoom, 2 /*Muted*/);
        // Fresh sender, so the ring cooldown cannot be the reason the
        // announcement is absent — only the mute is.
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

    // The per-sender ring cooldown bounds OS-level announcements: a
    // sender re-ringing within the window still produces call STATE, but
    // no second notification and therefore no later missed notice.
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

        // Same sender rings again immediately: state rings, announcement
        // suppressed by the cooldown, so an abandoned ring raises no
        // missed notice either.
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
        // The DBus daemon is absent under offscreen tests, so drive the
        // decline signal directly: the AppController glue must route it to
        // rejectIncoming for the MATCHING call only.
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

    // A REFUSAL THAT WAS WITHDRAWN MUST STOP BEING SHOWN.
    //
    // `errorReported` reaches Main.qml as `statusBar.lastError = msg`, a
    // one-shot copy that nothing ever takes back. So the sentence a refused
    // join put on the strip outlived the later join that SUCCEEDED: the user
    // was in the call while being told they had no permission to be.
    //
    // SfuCallController withdraws it by emitting `callFailed` with an EMPTY
    // reason once a retry reaches Authorizing or later (proven in
    // CallControllerTest). AppController used to drop that empty string on
    // the floor, which made the withdrawal invisible and is the half of the
    // defect that lives here. An empty `errorReported` is already how four
    // other paths in AppController clear the strip.
    //
    // Driven through the real signal rather than a functor: the claim is
    // about the PRODUCTION connection, and a test that calls the lambda
    // directly would pass with the connection deleted.
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

    // ── A NOTIFICATION CLICK MUST *OPEN* THE ROOM, NOT MERELY SELECT IT ──
    //
    // Reported from Windows on 0.9.3, where the tray balloon is the only
    // delivery and therefore the only way most people ever reach this path:
    // "when i click it, it sends me to this. and just doesnt load anything
    // on that room", and yes, "from notification only".
    //
    // `qml/Main.qml`'s notification handler did `app.currentRoomId = roomId`
    // — the property WRITE, i.e. setCurrentRoomId(). It is the ONLY place in
    // the application that writes currentRoomId directly; every other way
    // into a room (room list, quick switcher, search, links, the rail, Home)
    // calls app.openRoom(). And openRoom() is the sole caller of
    // RustSdkMatrixClient::openRoomTimeline(), the sole caller of
    // mx_rust_timeline_open. So a room entered from a notification never got
    // a live SDK timeline: the navigation succeeds, and the room shows only
    // what the bounded background sync mirror happens to hold, with
    // paginationReady() false forever so no history can ever arrive.
    //
    // WHAT THIS ASSERTS, AND WHY IT IS THE RIGHT PROXY. The SDK open itself
    // is behind ENABLE_RUST_SDK_BACKEND and a qobject_cast to the Rust
    // client, so no mock-backend test can observe it directly. What CAN be
    // observed is the one other thing openRoom() does and setCurrentRoomId()
    // does not: return from the Settings screen to the chat. Since openRoom()
    // is the only caller of openRoomTimeline(), proving openRoom() is on the
    // path IS proving the timeline is opened on a Rust build.
    //
    // Both routes are covered because both land on the same C++ handler: the
    // desktop notification, and an Activity Center ("bell") row.
    void aNotificationClickOpensTheRoomRatherThanJustSelectingIt()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        // Somewhere that is NOT the room, and on a screen only openRoom()
        // leaves. (Main.qml calls showMain() before the assignment, but that
        // is the QML half; this case is about the C++ routing.)
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

        // ...and the Activity Center row is the same click by another name.
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

    // A THREAD NOTIFICATION MUST NOT HAND A TIMELINE ID TO openRoom().
    //
    // CLAUDE.md §8: the composite `room + US + thread + US + root` must never
    // leave the app layer. NotificationManager reduces it before it emits, so
    // this should be unreachable — and it is asserted anyway because the cost
    // of it getting through is not "nothing happens".
    //
    // Adding the room open is exactly what turns a composite from inert into
    // destructive, which is why the reduction and the open landed together.
    // openRoomTimeline() has
    // no composite guard and the Rust side CLOSES the previous room's
    // timeline BEFORE it discovers the id will not parse, so routing a
    // composite here would tear down the live subscription of the room the
    // reader is actually sitting in.
    //
    // This case fails on all three broken forms: the pre-fix tree (nothing
    // opens, currentRoomId stays empty), the skip-and-re-emit form (same,
    // plus the composite arrives at the signal), and an unguarded open
    // (currentRoomId becomes the composite).
    void aTimelineIdIsNeverHandedToTheRoomOpener()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        NotificationManager *notifications = controller.notificationsForTest();
        QVERIFY(notifications);

        // START WITH NO ROOM OPEN. Raised in review: with kRoom already
        // open, "reduced the composite and reopened kRoom" and "did nothing
        // at all" are the same observation, so the case passed on a version
        // that skipped the open and re-emitted the composite — which is the
        // very form this reduction replaced. Starting empty makes the
        // reduction positively observable instead of merely non-destructive.
        controller.setCurrentRoomId(QString{});
        QVERIFY(controller.currentRoomId().isEmpty());

        QSignalSpy routed(&controller,
                          &AppController::notificationOpenRequested);
        const QString root = QStringLiteral("$root:mock.local");
        const QString composite = MatrixClient::threadTimelineId(kRoom, root);
        QVERIFY(MatrixClient::isThreadTimelineId(composite));
        // No root argument: the composite is the only place it exists. That
        // is routeNotificationOpen()'s recovery branch, which nothing else
        // drives at either layer.
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
