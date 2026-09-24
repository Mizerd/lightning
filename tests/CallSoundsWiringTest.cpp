// The call sounds wired to their REAL owners on a full AppController (mock
// backend): the incoming ring through AppController's announcement path,
// and the group-call cues through SfuCallController's own signals and its
// real CallParticipantModel.
//
// CallSoundPolicyTest proves the rules; this suite proves something calls
// them. §16 has the lesson twice over: a policy hook nobody wires is dead
// code covered by a passing test. The sink is a RECORDER — nothing here
// opens an audio device, and whether a sound was HEARD is a live test.
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
        // The controller re-asserts its loop on every state change; only a
        // CHANGE is an event.
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

// `sender` matters for more than realism: AppController announces at most
// one ring per sender per 30 s, so a second call inside one test case must
// come from someone else or it is never announced at all.
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

QVariantMap departed(const QString &identity)
{
    QVariantMap row = participant(identity);
    row.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
    return row;
}

} // namespace

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

    /// Move the group call to `state` the way the controller does: the
    /// state, then its own signal.
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

    // THE RINGER REWORK. With Lightning's own ringer loaded, an announced
    // ring loops it AND the desktop card goes silent (no themed-sound
    // re-post). Before 2026-09-23 the only ring was that re-post.
    void ownRingerRingsAndTheCardGoesSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, /*ringerLoaded=*/true);
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();

        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QStringLiteral("ring"));
        // The card is still shown; only its sound moved to us.
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        QVERIFY(!notices->callRingActiveForTest());

        // Every way a ring ends stops it: here, the caller hanging up.
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

    // A machine where the ringer never loaded keeps the desktop's sound:
    // the fallback is the old behaviour, exactly.
    void anUnloadedRingerKeepsTheDesktopSound()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, /*ringerLoaded=*/false);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QVERIFY(controller.notificationsForTest()->callRingActiveForTest());
    }

    // The ring switch silences BOTH ringers.
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

    // A muted room still produces call STATE but never rang, so nothing
    // must loop for it.
    void aMutedRoomDoesNotRing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        controller.settings()->setRoomNotificationMode(kRoom, 2 /*Muted*/);
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(log->loop, QString());
    }

    // ── Silence (2026-09-23): per call, like Element's ─────────────────

    // Silencing stops OUR loop and nothing else: the call is still ringing,
    // the card is still up, and it can still be answered.
    void silenceStopsTheLoopAndLeavesTheCallRinging()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, /*ringerLoaded=*/true);
        CallSoundController *sounds = controller.callSounds();
        QSignalSpy ringingChanged(sounds,
                                  &CallSoundController::ringingCallIdChanged);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QStringLiteral("ring"));
        QCOMPARE(sounds->ringingCallId(), QStringLiteral("call-1"));
        QVERIFY(ringingChanged.count() >= 1);
        // The card offers Silence while our ringer sounds, silently itself.
        auto *notices = controller.notificationsForTest();
        QVERIFY(notices->callSilenceOfferedForTest());
        QVERIFY(!notices->callSoundActiveForTest());

        QVERIFY(sounds->silenceRing(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());
        QCOMPARE(sounds->ringingCallId(), QString());
        QVERIFY(sounds->isRingSilenced(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QCOMPARE(notices->activeCallIdForTest(), QStringLiteral("call-1"));
        // ...and the card has lost its Silence button.
        QVERIFY(!notices->callSilenceOfferedForTest());
        // Declining afterwards still works and plays nothing new.
        QVERIFY(controller.calls()->rejectIncoming());
        QCOMPARE(log->events,
                 (QStringList{ QStringLiteral("loop:ring"),
                               QStringLiteral("loop:") }));
    }

    // Only the call ringing NOW can be silenced: an empty id, some other
    // id, and the id of a call that has already stopped ringing are all
    // refused — and so is a card action that is not exactly "silence" or
    // arrives for a notification that is not the showing card.
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

        // The card: exact action key, exact notification id.
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
        // Nothing above touched the call.
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);

        // The real one, through the card, reaches the same entry point.
        QVERIFY(QMetaObject::invokeMethod(
            notices, "onActionInvoked", Q_ARG(quint32, 42u),
            Q_ARG(QString, QStringLiteral("silence"))));
        QCOMPARE(log->loop, QString());
        QVERIFY(sounds->isRingSilenced(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);

        // A call that is over cannot be silenced after the fact.
        mock(controller)->emitCallSignalForTest(
            hangup(QStringLiteral("call-1")));
        QVERIFY(!sounds->silenceRing(QStringLiteral("call-1")));
    }

    // The announcing path can run again for the SAME call. After Silence it
    // must restart neither ringer: not our loop, and — where the desktop's
    // themed sound is the ringer — not the card's sound either.
    void aReannouncedSilencedCallStaysSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, true);
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        QVERIFY(controller.callSounds()->silenceRing(QStringLiteral("call-1")));
        QCOMPARE(log->loop, QString());

        // A different sender key, so the per-sender cooldown does not
        // swallow the re-announcement and make this case vacuous.
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

        // Belt and braces, and asserted on its own: the controller refuses
        // the silenced call even from a caller that did not ask first.
        controller.callSounds()->startIncomingRing(QStringLiteral("call-1"));
        QCOMPARE(log->loop, QString());
        QCOMPARE(controller.callSounds()->ringingCallId(), QString());
    }

    void aSilencedFallbackRingerKeepsTheCardSilent()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto log = install(controller, /*ringerLoaded=*/false);
        auto *notices = controller.notificationsForTest();
        mock(controller)->emitCallSignalForTest(
            invite(QStringLiteral("call-1")));
        // The desktop's themed sound is the ringer, and the card offers
        // Silence for it; our loop never started.
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

        // A late join-gate answer redraws the card: still silent.
        notices->setCallAcceptOffered(QStringLiteral("call-1"), true, false);
        QVERIFY(!notices->callSoundActiveForTest());
        QVERIFY(!notices->callRingActiveForTest());

        // And a re-announcement does not bring the themed sound back.
        Q_EMIT controller.calls()->incomingCallStarted(
            kRoom, QStringLiteral("call-1"),
            QStringLiteral("@peer2:mock.local"), 60000);
        QVERIFY(!notices->callSoundActiveForTest());
        QVERIFY(!notices->callRingActiveForTest());
    }

    // Silence is remembered for ONE call. The next one rings.
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

    // The in-app prompt: the control exists, is offered only while OUR
    // ringer sounds for the ringing call, and its handler reaches the
    // controller. Loaded for real, so a property typo or an unresolved
    // token fails here rather than in a user's corner card.
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
        // An icon-only control: its accessible name and tooltip are the
        // only words it has, so both must be there.
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
        // The DECISION, not the button's `visible`: that is effective
        // visibility, and a card loaded with no window reads false whatever
        // the button's own binding says (legacyAcceptOffered's comment).
        QTRY_VERIFY_WITH_TIMEOUT(root->property("silenceOffered").toBool(),
                                 kSignalTimeoutMs);

        // The button's own handler, not a call made on its behalf.
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(log->loop, QString());
        QVERIFY(controller.callSounds()->isRingSilenced(
            QStringLiteral("call-1")));
        QTRY_VERIFY_WITH_TIMEOUT(!root->property("silenceOffered").toBool(),
                                 kSignalTimeoutMs);
        // The card itself is still up: silencing is not dismissing.
        QVERIFY(root->property("shouldShow").toBool());
        QVERIFY(controller.calls()->rejectIncoming());
    }

    void thePromptHidesSilenceWhenTheDesktopRings()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        install(controller, /*ringerLoaded=*/false);
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

    // The group lane: connect, the existing room, a newcomer, controls, a
    // share, a leave, and our own exit — through the controller's own
    // signals and its real participant model.
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
        // The room as it was when we arrived: no chorus.
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
        // Deafen while muted: ONE cue, the deafen one.
        call->setDeafened(true);
        QCOMPARE(log->events.last(), QStringLiteral("play:deafen"));
        const int beforeDeafenedJoin = log->events.size();
        clock += 1000;
        call->ingestParticipantsForTest({ participant(QStringLiteral("erin")) });
        QTest::qWait(20);
        QCOMPARE(log->events.size(), beforeDeafenedJoin); // deafened: silent
        call->setDeafened(false);
        QCOMPARE(log->events.last(), QStringLiteral("play:undeafen"));

        clock += 1000;
        call->ingestParticipantsForTest(
            { participant(QStringLiteral("bob"), /*sharing=*/true) });
        QTRY_COMPARE(log->events.last(), QStringLiteral("play:share-start"));

        clock += 1000;
        call->ingestParticipantsForTest({ departed(QStringLiteral("carol")) });
        QTRY_COMPARE(log->events.last(), QStringLiteral("play:leave"));

        // Our own exit: ONE "ended", and the model being emptied on the way
        // out must not read as everyone leaving.
        const int beforeExit = log->events.size();
        call->leave();
        QTest::qWait(20);
        QCOMPARE(log->events.mid(beforeExit),
                 QStringList{ QStringLiteral("play:ended") });
    }

    // The master switch reaches the controller live, through the settings
    // signal.
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
        s->setCallSoundVolume(0); // unchanged: no signal
        QCOMPARE(changed.count(), 2);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallSoundsWiringTest)
#include "CallSoundsWiringTest.moc"
