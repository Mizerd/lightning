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
#include "matrix/CallSignal.h"
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

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallRingPolicyTest)
#include "CallRingPolicyTest.moc"
