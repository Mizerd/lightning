// Call sounds wired to their real owners on a full AppController (mock
// backend): the incoming ring through AppController's announcement path, and
// group-call cues through SfuCallController's signals and its real
// CallParticipantModel. CallSoundPolicyTest proves the rules; this proves
// something calls them. The sink is a recorder: no audio device is opened.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QSignalSpy>

#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallSoundController.h"
#include "calls/SfuCallController.h"
#include "matrix/CallSignal.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "notifications/NotificationManager.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
const QString kRoom = QStringLiteral("!general:mock.local");

struct Log {
    QStringList events;
    QString loop;
};

class RecordingSink : public CallSoundSink
{
public:
    RecordingSink(std::shared_ptr<Log> log, bool ringerLoaded)
        : m_log(std::move(log)), m_ringerLoaded(ringerLoaded)
    {
    }
    void play(const QString &sound, qreal, bool) override
    {
        m_log->events.append(QStringLiteral("play:") + sound);
    }
    void loop(const QString &sound, qreal, bool) override
    {
        if (sound == m_log->loop)
            return;
        m_log->loop = sound;
        m_log->events.append(QStringLiteral("loop:") + sound);
    }
    bool canPlay(const QString &) const override { return m_ringerLoaded; }

private:
    std::shared_ptr<Log> m_log;
    bool m_ringerLoaded;
};

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

CallSignal hangup(const QString &callId,
                  const QString &sender = QStringLiteral("@peer:mock.local"))
{
    CallSignal s;
    s.kind = CallSignal::Kind::Hangup;
    s.roomId = kRoom;
    s.eventId = QStringLiteral("$hangup-") + callId;
    s.sender = sender;
    s.callId = callId;
    s.partyId = QStringLiteral("peer-party");
    s.reason = QStringLiteral("user_hangup");
    return s;
}

QVariantMap participant(const QString &identity, bool sharing = false)
{
    QVariantList tracks;
    QVariantMap mic;
    mic.insert(QStringLiteral("source"), QStringLiteral("microphone"));
    mic.insert(QStringLiteral("sid"), QStringLiteral("TR_mic_") + identity);
    mic.insert(QStringLiteral("muted"), false);
    tracks.append(mic);
    if (sharing) {
        QVariantMap screen;
        screen.insert(QStringLiteral("source"), QStringLiteral("screen_share"));
        screen.insert(QStringLiteral("sid"),
                      QStringLiteral("TR_screen_") + identity);
        screen.insert(QStringLiteral("muted"), false);
        tracks.append(screen);
    }
    QVariantMap row;
    row.insert(QStringLiteral("identity"), identity);
    row.insert(QStringLiteral("sid"), QStringLiteral("PA_") + identity);
    row.insert(QStringLiteral("tracks"), tracks);
    return row;
}

class RetractingClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;
    quint64 rtcRetractMembership(const QString &roomId,
                                 const QString &delayId) override
    {
        Q_UNUSED(delayId);
        retractedRooms.append(roomId);
        return ++lastOp;
    }
    void answer(quint64 op, bool ok,
                const QString &failure = QStringLiteral("forbidden"))
    {
        Q_EMIT rtcMembershipRetracted(op, ok, ok ? QString() : failure);
    }
    QStringList retractedRooms;
    quint64 lastOp = 1000;
};

QVariantMap departed(const QString &identity)
{
    QVariantMap row = participant(identity);
    row.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
    return row;
}

}

class CallSoundsWiringTest : public QObject
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

    static std::shared_ptr<Log> install(AppController &controller,
                                        bool ringerLoaded)
    {
        auto log = std::make_shared<Log>();
        controller.callSounds()->setSink(
            std::make_unique<RecordingSink>(log, ringerLoaded));
        log->events.clear();
        return log;
    }

    static void groupState(AppController &controller,
                           SfuCallController::State state)
    {
        controller.groupCall()->setCallStateForTest(state);
        Q_EMIT controller.groupCall()->stateChanged();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-sounds-wiring-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void theNotificationChimeIsRoutedThroughTheCallSounds()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller,true);
        auto *notices = controller.notificationsForTest();

        QVERIFY(notices->playOwnSoundForTest(false));
        QVERIFY(notices->playOwnSoundForTest(true));
        QCOMPARE(log->events,
                 (QStringList{ QStringLiteral("play:message"),
                               QStringLiteral("play:mention") }));

        log->events.clear();
        auto *client = mock(controller);
        QVERIFY(client);
        client->emitCallSignalForTest(invite(QStringLiteral("call-x")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QVERIFY(!notices->playOwnSoundForTest(true));
        QVERIFY(!log->events.contains(QStringLiteral("play:mention")));
        client->emitCallSignalForTest(hangup(QStringLiteral("call-x")));
        QTRY_COMPARE(log->loop, QString());
        QVERIFY(notices->playOwnSoundForTest(true));

        auto quiet = install(controller,false);
        QVERIFY(!notices->playOwnSoundForTest(false));
        QVERIFY(quiet->events.isEmpty());
    }

    void theSoundSourceSettingReachesTheNotificationManager()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *notices = controller.notificationsForTest();
        QCOMPARE(notices->soundSource(), NotificationManager::SourceLightning);
        controller.settings()->setNotificationSoundSource(1);
        QCOMPARE(notices->soundSource(), NotificationManager::SourceSystem);
        controller.settings()->setNotificationSoundSource(0);
        QCOMPARE(notices->soundSource(), NotificationManager::SourceLightning);
        controller.settings()->setNotificationSoundSource(7);
        QCOMPARE(controller.settings()->notificationSoundSource(), 0);
    }

    void ownRingerRingsAndTheCardGoesSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller,true);
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();

        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        QVERIFY(!notices->callRingActiveForTest());

        client->emitCallSignalForTest(hangup(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QCOMPARE(log->events,
                 (QStringList{ QStringLiteral("loop:ring"),
                               QStringLiteral("loop:") }));
    }

    void decliningStopsTheRing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QVERIFY(controller.calls()->rejectIncoming());
        QCOMPARE(log->loop, QString());
    }

    void anUnloadedRingerKeepsTheDesktopSound()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller,false);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QVERIFY(controller.notificationsForTest()->callRingActiveForTest());
    }

    void theRingSwitchSilencesBoth()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        controller.settings()->setRingForCalls(false);
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QString());
        QVERIFY(!controller.notificationsForTest()->callRingActiveForTest());
    }

    void aMutedRoomDoesNotRing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        controller.settings()->setRoomNotificationMode(kRoom, 2);
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QString());
    }

    void silenceStopsTheLoopAndLeavesTheCallRinging()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller,true);
        CallSoundController *sounds = controller.callSounds();
        QSignalSpy ringingChanged(sounds,
                                  &CallSoundController::ringingCallIdChanged);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(sounds->ringingCallId(), QStringLiteral("call-1"));
        QVERIFY(ringingChanged.count() >= 1);
        auto *notices = controller.notificationsForTest();
        QVERIFY(notices->callSilenceOfferedForTest());
        QVERIFY(!notices->callSoundActiveForTest());

        QVERIFY(sounds->silenceRing(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QCOMPARE(sounds->ringingCallId(), QString());
        QVERIFY(sounds->isRingSilenced(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        QVERIFY(!notices->callSilenceOfferedForTest());
        QVERIFY(controller.calls()->rejectIncoming());
        QCOMPARE(log->events,
                 (QStringList{ QStringLiteral("loop:ring"),
                               QStringLiteral("loop:") }));
    }

    void aStaleOrOtherIdCannotSilence()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        CallSoundController *sounds = controller.callSounds();
        auto *notices = controller.notificationsForTest();
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QStringLiteral("ring"));

        QVERIFY(!sounds->silenceRing(QString()));
        QVERIFY(!sounds->silenceRing(QStringLiteral("call-0")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QVERIFY(!sounds->isRingSilenced(QStringLiteral("call-0")));

        notices->setActiveCallNotificationIdForTest(42);
        const QStringList crafted{ QStringLiteral("Silence"),
                                   QStringLiteral("silence "),
                                   QStringLiteral("silence\ncall-1"),
                                   QStringLiteral("silence:call-1"),
                                   QStringLiteral("silenc") };
        for (const QString &action : crafted) {
            QVERIFY(QMetaObject::invokeMethod(
                notices, "onActionInvoked", Q_ARG(quint32, 42u),
                Q_ARG(QString, action)));
            QCOMPARE(log->loop, QStringLiteral("ring"));
        }
        QVERIFY(QMetaObject::invokeMethod(
            notices, "onActionInvoked", Q_ARG(quint32, 7u),
            Q_ARG(QString, QStringLiteral("silence"))));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);

        QVERIFY(QMetaObject::invokeMethod(
            notices, "onActionInvoked", Q_ARG(quint32, 42u),
            Q_ARG(QString, QStringLiteral("silence"))));
        QCOMPARE(log->loop, QString());
        QVERIFY(sounds->isRingSilenced(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);

        mock(controller)->emitCallSignalForTest(
            hangup(QStringLiteral("call-1")));
        QVERIFY(!sounds->silenceRing(QStringLiteral("call-1")));
    }

    void aReannouncedSilencedCallStaysSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QVERIFY(controller.callSounds()->silenceRing(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());

        Q_EMIT controller.calls()->incomingCallStarted(
            kRoom, QStringLiteral("call-1"),
            QStringLiteral("@peer2:mock.local"), 60000);
        auto *notices = controller.notificationsForTest();
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        QCOMPARE(log->loop, QString());
        QCOMPARE(controller.callSounds()->ringingCallId(), QString());
        QVERIFY(!notices->callSoundActiveForTest());
        QVERIFY(!notices->callRingActiveForTest());
        QVERIFY(!notices->callSilenceOfferedForTest());

        controller.callSounds()->startIncomingRing(QStringLiteral("call-1"));
        QCOMPARE(log->loop, QString());
        QCOMPARE(controller.callSounds()->ringingCallId(), QString());
    }

    void aSilencedFallbackRingerKeepsTheCardSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller,false);
        auto *notices = controller.notificationsForTest();
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QVERIFY(notices->callRingActiveForTest());
        QVERIFY(notices->callSoundActiveForTest());
        QVERIFY(notices->callSilenceOfferedForTest());

        notices->setActiveCallNotificationIdForTest(42);
        QVERIFY(QMetaObject::invokeMethod(
            notices, "onActionInvoked", Q_ARG(quint32, 42u),
            Q_ARG(QString, QStringLiteral("silence"))));
        QVERIFY(!notices->callRingActiveForTest());
        QVERIFY(!notices->callSoundActiveForTest());
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));

        notices->setCallAcceptOffered(QStringLiteral("call-1"), true, false);
        QVERIFY(!notices->callSoundActiveForTest());
        QVERIFY(!notices->callRingActiveForTest());

        Q_EMIT controller.calls()->incomingCallStarted(
            kRoom, QStringLiteral("call-1"),
            QStringLiteral("@peer2:mock.local"), 60000);
        QVERIFY(!notices->callSoundActiveForTest());
        QVERIFY(!notices->callRingActiveForTest());
    }

    void aNewCallRingsAfterASilencedOne()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QVERIFY(controller.callSounds()->silenceRing(QStringLiteral("call-1")));
        mock(controller)->emitCallSignalForTest(
            hangup(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());

        mock(controller)->emitCallSignalForTest(invite(
            QStringLiteral("call-2"), QStringLiteral("@carol:mock.local")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(controller.callSounds()->ringingCallId(),
                 QStringLiteral("call-2"));
        QVERIFY(!controller.callSounds()->isRingSilenced(
            QStringLiteral("call-2")));
        QVERIFY(controller.notificationsForTest()->callSilenceOfferedForTest());
    }

    void thePromptOffersSilenceOnlyWhileOurRingerRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("IncomingCallPrompt"));
        if (created.isEmpty())
            QVERIFY(created.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        auto *button =
            root->findChild<QQuickItem *>(QStringLiteral(
                "incomingCallPromptSilence"));
        QVERIFY(button);
        QVERIFY(!root->property("silenceOffered").toBool());
        QVERIFY(!QQmlProperty::read(button, QStringLiteral("Accessible.name"),
                                    qmlContext(button))
                     .toString()
                     .isEmpty());
        QVERIFY(!QQmlProperty::read(button, QStringLiteral("ToolTip.text"),
                                    qmlContext(button))
                     .toString()
                     .isEmpty());

        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QTRY_VERIFY_WITH_TIMEOUT(root->property("silenceOffered").toBool(),
                                 kSignalTimeoutMs);

        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(log->loop, QString());
        QVERIFY(controller.callSounds()->isRingSilenced(
            QStringLiteral("call-1")));
        QTRY_VERIFY_WITH_TIMEOUT(!root->property("silenceOffered").toBool(),
                                 kSignalTimeoutMs);
        QVERIFY(root->property("shouldShow").toBool());
        QVERIFY(controller.calls()->rejectIncoming());
    }

    void theDismissButtonStopsOurRingerAndSendsNothing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        auto *notices = controller.notificationsForTest();

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("IncomingCallPrompt"));
        if (created.isEmpty())
            QVERIFY(created.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        auto *dismiss = root->findChild<QQuickItem *>(
            QStringLiteral("incomingCallPromptDismiss"));
        QVERIFY(dismiss);

        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        QTRY_VERIFY_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                 kSignalTimeoutMs);

        QVERIFY(QMetaObject::invokeMethod(dismiss, "clicked"));
        QCOMPARE(log->loop, QString());
        QCOMPARE(controller.callSounds()->ringingCallId(), QString());
        QVERIFY(controller.callSounds()->isRingSilenced(
            QStringLiteral("call-1")));
        QCOMPARE(notices->activeCallIdForTest(), QString());
        QVERIFY(!root->property("shouldShow").toBool());
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QVERIFY(controller.calls()->rejectIncoming());

        QVERIFY(!controller.callSounds()->dismissRing(QStringLiteral("call-1")));
        QVERIFY(!controller.callSounds()->dismissRing(QString()));
    }

    // Sign-out leaves a live group call first and waits (bounded) for its
    // membership retraction before the session goes. Live 2026-10-07 the
    // logout ran first, the retraction "could not be dispatched" and the
    // deleted device stayed in the call as a ghost. Fails on the old tree:
    // the mock's logout is synchronous, so loggedOut had already fired with
    // the call still up.
    void signOutLeavesTheCallBeforeTheSessionGoes()
    {
        RetractingClient rtcClient;
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        SfuCallController *call = controller.groupCall();
        call->setClient(&rtcClient);
        call->setMembershipForTest(kRoom, QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy loggedOut(controller.auth(), &AuthManager::loggedOut);

        controller.auth()->logout();
        QVERIFY(!call->active());
        QCOMPARE(rtcClient.retractedRooms, QStringList{ kRoom });
        QVERIFY(call->membershipWritesPending());
        QCOMPARE(loggedOut.count(), 0);
        controller.auth()->logout();
        QCOMPARE(loggedOut.count(), 0);

        rtcClient.answer(rtcClient.lastOp, true);
        QTRY_COMPARE_WITH_TIMEOUT(loggedOut.count(), 1, kSignalTimeoutMs);
        QVERIFY(!call->membershipWritesPending());
        QCOMPARE(rtcClient.retractedRooms.size(), 1);
    }

    void signOutDuringACallIsBoundedWhenTheServerNeverAnswers()
    {
        RetractingClient rtcClient;
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        SfuCallController *call = controller.groupCall();
        call->setClient(&rtcClient);
        call->setMembershipForTest(kRoom, QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy loggedOut(controller.auth(), &AuthManager::loggedOut);

        controller.auth()->logout();
        QCOMPARE(loggedOut.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(loggedOut.count(), 1, 8000);
    }

    void aFailedRetractionIsNeverRetriedAfterTheSignOut()
    {
        RetractingClient rtcClient;
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        SfuCallController *call = controller.groupCall();
        call->setClient(&rtcClient);
        call->setMembershipForTest(kRoom, QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy loggedOut(controller.auth(), &AuthManager::loggedOut);

        controller.auth()->logout();
        QCOMPARE(rtcClient.retractedRooms.size(), 1);
        rtcClient.answer(rtcClient.lastOp, false, QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(rtcClient.retractedRooms.size(), 2, 4000);
        rtcClient.answer(rtcClient.lastOp, false, QStringLiteral("network"));
        QVERIFY(call->membershipWritesPending());
        QCOMPARE(loggedOut.count(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(loggedOut.count(), 1, 8000);
        QVERIFY(!call->membershipWritesPending());
        QTest::qWait(3000);
        QCOMPARE(rtcClient.retractedRooms.size(), 2);
    }

    void signOutWithoutACallIsImmediate()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QSignalSpy loggedOut(controller.auth(), &AuthManager::loggedOut);
        controller.auth()->logout();
        QCOMPARE(loggedOut.count(), 1);
        QVERIFY(!controller.auth()->logoutPending());
    }

    void thePromptHidesSilenceWhenTheDesktopRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        install(controller,false);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("IncomingCallPrompt"));
        if (created.isEmpty())
            QVERIFY(created.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QTRY_VERIFY_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                 kSignalTimeoutMs);
        QVERIFY(!root->property("silenceOffered").toBool());
        QVERIFY(controller.calls()->rejectIncoming());
    }

    void aGroupCallPlaysTheRightCuesInOrder()
    {
        AppController controller(AppController::MockBackend);
        qint64 clock = 1000;
        controller.callSounds()->setClockForTest([&clock] { return clock; });
        auto log = install(controller, true);
        SfuCallController *call = controller.groupCall();
        call->setOwnIdentityForTest(QStringLiteral("me"));

        groupState(controller, SfuCallController::State::Preparing);
        groupState(controller, SfuCallController::State::Connecting);
        groupState(controller, SfuCallController::State::Connected);
        call->ingestParticipantsForTest(
            { participant(QStringLiteral("me")),
              participant(QStringLiteral("bob")),
              participant(QStringLiteral("carol")) });
        QTest::qWait(20);
        QCOMPARE(log->events, QStringList{ QStringLiteral("play:connected") });

        clock += callsound::Policy::kSettleMs + 100;
        call->ingestParticipantsForTest({ participant(QStringLiteral("dave")) });
        QTRY_COMPARE(log->events.size(), 2);
        QCOMPARE(log->events.last(), QStringLiteral("play:join"));

        call->setMicrophoneMuted(true);
        QCOMPARE(log->events.last(), QStringLiteral("play:mute"));
        call->setDeafened(true);
        QCOMPARE(log->events.last(), QStringLiteral("play:deafen"));
        const int beforeDeafenedJoin = log->events.size();
        clock += 1000;
        call->ingestParticipantsForTest({ participant(QStringLiteral("erin")) });
        QTest::qWait(20);
        QCOMPARE(log->events.size(), beforeDeafenedJoin);
        call->setDeafened(false);
        QCOMPARE(log->events.last(), QStringLiteral("play:undeafen"));

        clock += 1000;
        call->ingestParticipantsForTest(
            { participant(QStringLiteral("bob"),true) });
        QTRY_COMPARE(log->events.last(), QStringLiteral("play:share-start"));

        clock += 1000;
        call->ingestParticipantsForTest({ departed(QStringLiteral("carol")) });
        QTRY_COMPARE(log->events.last(), QStringLiteral("play:leave"));

        const int beforeExit = log->events.size();
        call->leave();
        QTest::qWait(20);
        QCOMPARE(log->events.mid(beforeExit),
                 QStringList{ QStringLiteral("play:ended") });
    }

    void theMasterSwitchIsHonouredLive()
    {
        AppController controller(AppController::MockBackend);
        auto log = install(controller, true);
        controller.settings()->setCallSoundsEnabled(false);
        groupState(controller, SfuCallController::State::Preparing);
        groupState(controller, SfuCallController::State::Connected);
        controller.groupCall()->setMicrophoneMuted(true);
        QCOMPARE(log->events, QStringList{});
        controller.settings()->setCallSoundsEnabled(true);
        controller.groupCall()->setMicrophoneMuted(false);
        QCOMPARE(log->events, QStringList{ QStringLiteral("play:unmute") });
    }

    void settingsRoundTripAndClamp()
    {
        AppController controller(AppController::MockBackend);
        SettingsManager *s = controller.settings();
        QVERIFY(s->callSoundsEnabled());
        QVERIFY(s->callSoundsPresence());
        QVERIFY(s->callSoundsControls());
        QVERIFY(s->callSoundsShareAndHand());
        QCOMPARE(s->callSoundVolume(), SettingsManager::kDefaultCallSoundVolume);
        QCOMPARE(s->ringerVolume(), SettingsManager::kDefaultRingerVolume);
        QSignalSpy changed(s, &SettingsManager::callSoundSettingsChanged);
        s->setRingerVolume(250);
        QCOMPARE(s->ringerVolume(), 100);
        s->setCallSoundVolume(-5);
        QCOMPARE(s->callSoundVolume(), 0);
        s->setCallSoundVolume(0);
        QCOMPARE(changed.count(), 2);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallSoundsWiringTest)
#include "CallSoundsWiringTest.moc"
