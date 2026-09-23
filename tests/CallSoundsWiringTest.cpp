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

CallSignal invite(const QString &callId)
{
    CallSignal s;
    s.kind = CallSignal::Kind::Invite;
    s.roomId = kRoom;
    s.eventId = QStringLiteral("$invite-") + callId;
    s.sender = QStringLiteral("@peer:mock.local");
    s.callId = callId;
    s.partyId = QStringLiteral("peer-party");
    s.lifetimeMs = 60000;
    s.originServerTs = QDateTime::currentMSecsSinceEpoch();
    s.version = QStringLiteral("1");
    s.sessionType = QStringLiteral("offer");
    s.hasDescription = true;
    return s;
}

CallSignal hangup(const QString &callId)
{
    CallSignal s;
    s.kind = CallSignal::Kind::Hangup;
    s.roomId = kRoom;
    s.eventId = QStringLiteral("$hangup-") + callId;
    s.sender = QStringLiteral("@peer:mock.local");
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
