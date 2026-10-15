// AppController's notification glue on a full controller (mock backend,
// offscreen): which owner each notification signal reaches, and what the
// call announcement does with the user's switches. The pure decisions behind
// it are in NotificationNoticesTest; this proves the production wiring uses
// them.
//
// Each case fails on the code before it, for the reason written beside it.
#include <QtTest/QtTest>

#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallSoundController.h"
#include "matrix/CallSignal.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "models/ActivityModel.h"
#include "models/ReadReceiptCoordinator.h"
#include "notifications/NotificationManager.h"
#include "storage/SecretStore.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
const QString kRoom = QStringLiteral("!general:mock.local");
const QString kPeer = QStringLiteral("@peer:mock.local");

CallSignal invite(const QString &callId, const QString &sender = kPeer)
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

CallSignal hangup(const QString &callId, const QString &sender = kPeer)
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

// Enough of a keyring for the add-account flow; never the host's.
class FakeSecretStore final : public SecretStore
{
public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    bool lastReadFailed() const override { return false; }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};
} // namespace

class NotificationGlueTest : public QObject
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
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("notification-glue-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // Old code: a second call from the same sender inside 30 s was dropped
    // before m_announcedCallId was set, so it neither rang nor produced a
    // missed-call notice. A redial is the most ordinary thing a call does.
    void aRedialInsideTheOldCooldownIsAnnouncedInFull()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();

        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QVERIFY(controller.calls()->rejectIncoming());

        client->emitCallSignalForTest(invite(QStringLiteral("call-2")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        const int before = notices->genericNoticeCountForTest();
        client->emitCallSignalForTest(hangup(QStringLiteral("call-2")));
        QVERIFY2(notices->genericNoticeCountForTest() == before + 1,
                 "the redial was never announced, so its missed-call notice "
                 "never came");
    }

    // The bound still exists: a burst of fresh call ids from one sender is
    // refused past the cap, announcement and missed notice alike.
    void aBurstOfFreshCallIdsIsStillBounded()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        auto *notices = controller.notificationsForTest();
        for (int i = 1; i <= notices::RingAnnounceLimiter::kMaxNewCallsPerWindow;
             ++i) {
            client->emitCallSignalForTest(
                invite(QStringLiteral("burst-%1").arg(i)));
            QVERIFY(controller.calls()->rejectIncoming());
        }
        client->emitCallSignalForTest(invite(QStringLiteral("burst-over")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        const int before = notices->genericNoticeCountForTest();
        client->emitCallSignalForTest(hangup(QStringLiteral("burst-over")));
        QCOMPARE(notices->genericNoticeCountForTest(), before);
    }

    // Old code: `sound = ringForCalls && notificationSound != Off`, so turning
    // chat chimes off made every call arrive silently. No player is installed
    // under test, so the ring is the desktop's call sound on the card.
    void turningMessageSoundsOffDoesNotSilenceCalls()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        controller.settings()->setNotificationSound(0 /* Off */);
        QVERIFY(controller.settings()->ringForCalls());
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QVERIFY2(controller.notificationsForTest()->callSoundActiveForTest(),
                 "the message sound setting silenced the call ring");
    }

    // The ring switch itself still decides.
    void theRingSwitchStillSilencesTheRing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        controller.settings()->setRingForCalls(false);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QVERIFY(!controller.notificationsForTest()->callSoundActiveForTest());
    }

    // Old code: closing the desktop call card stopped only the card's own
    // re-post timer; Lightning's ringer kept ringing with nothing on screen
    // to stop it.
    void closingTheCallCardStopsOurRinger()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *client = mock(controller);
        QVERIFY(client);
        client->emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
        QVERIFY(!controller.callSounds()->isRingSilenced(
            QStringLiteral("call-1")));
        Q_EMIT controller.notificationsForTest()->callCardDismissed(
            QStringLiteral("call-1"));
        QVERIFY(controller.callSounds()->isRingSilenced(
            QStringLiteral("call-1")));
        // Dismissed locally only: the call itself is still ringing.
        QCOMPARE(controller.calls()->state(), CallController::State::Ringing);
    }

    // Old code: a card for a room the client does not know (left and
    // forgotten, a declined invite) opened it anyway, onto nothing.
    void aCardForARoomTheUserIsNotInDoesNotOpenIt()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        controller.setCurrentRoomId(QString());
        QSignalSpy told(&controller, &AppController::errorReported);
        QSignalSpy routed(&controller,
                          &AppController::notificationOpenRequested);
        Q_EMIT controller.notificationsForTest()->openRequested(
            QStringLiteral("!gone:mock.local"), QStringLiteral("$e:mock"),
            QString());
        QVERIFY2(controller.currentRoomId().isEmpty(),
                 "a card for a room the user is not in opened that room");
        bool explained = false;
        for (const auto &call : told) {
            if (!call.at(0).toString().isEmpty())
                explained = true;
        }
        QVERIFY2(explained, "the refusal was silent");
        // The window is still raised (an empty room id), so the notice is
        // seen.
        QCOMPARE(routed.count(), 1);
        QVERIFY(routed.at(0).at(0).toString().isEmpty());
        // A joined room still opens.
        Q_EMIT controller.notificationsForTest()->openRequested(
            kRoom, QStringLiteral("$e:mock"), QString());
        QCOMPARE(controller.currentRoomId(), kRoom);
    }

    // Old code: add-account never reached ActivityModel's loggedOut reset, so
    // the new account's bell listed the previous account's mentions.
    void addingAnAccountStartsWithAnEmptyBell()
    {
        const QString alice = QStringLiteral("@alice:one.example");
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(QStringLiteral("https://one.example"),
                                    alice, QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(alice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), alice);

        auto *activity = qobject_cast<ActivityModel *>(app.activity());
        QVERIFY(activity);
        TimelineEvent mention;
        mention.eventId = QStringLiteral("$alice-mention:mock.local");
        mention.roomId = kRoom;
        mention.sender = QStringLiteral("@carol:mock.local");
        mention.type = TimelineEvent::TextMessage;
        mention.body = QStringLiteral("hey alice");
        mention.mentionsMe = true;
        mention.timestamp = QDateTime::currentDateTimeUtc();
        activity->ingest(mention, QStringLiteral("General"));
        // Looked up by event id rather than counted: the new account may
        // legitimately list rows of its own.
        const auto listsAlicesMention = [activity] {
            const int role = activity->roleNames().key(QByteArrayLiteral("eventId"),
                                                       -1);
            for (int row = 0; row < activity->rowCount(); ++row) {
                if (activity->index(row, 0).data(role).toString()
                    == QStringLiteral("$alice-mention:mock.local"))
                    return true;
            }
            return false;
        };
        QVERIFY2(listsAlicesMention(), "fixture: the mention was not listed");

        app.showLogin();
        app.auth()->login(QStringLiteral("https://two.example"),
                          QStringLiteral("bob"), QStringLiteral("pw"));
        QTRY_COMPARE(app.auth()->currentUserId(),
                     QStringLiteral("@bob:two.example"));
        // The auth manager names the new user before AppController's
        // onLoginSucceeded (which clears the bell) has run, so wait for it.
        QTRY_VERIFY2(!listsAlicesMention(),
                     "the added account's bell lists the previous account's "
                     "mention");
    }

    // Old code: receipts followed APPLICATION activation, so focusing the
    // call's picture-in-picture window (a top-level window of the same
    // application) marked the hidden main window's room read.
    void receiptsFollowTheMainWindowNotTheApplication()
    {
        AppController controller(AppController::MockBackend);
        QQuickWindow main;
        QQuickWindow pip;
        controller.registerMainWindow(&main);
        main.resize(200, 200);
        pip.resize(100, 100);
        main.show();
        main.requestActivate();
        if (!QTest::qWaitForWindowActive(&main))
            QSKIP("this platform plugin cannot activate windows");
        QTRY_VERIFY(controller.readReceipts()->windowActive());

        pip.show();
        pip.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&pip));
        // The application is still the active one: only the main window's
        // own state can tell these apart.
        QCOMPARE(QGuiApplication::applicationState(), Qt::ApplicationActive);
        QTRY_VERIFY2(!controller.readReceipts()->windowActive(),
                     "a focused call window counted as the user reading the "
                     "room in the main window");

        main.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTRY_VERIFY(controller.readReceipts()->windowActive());

        // The full-screen call window (CallStage.qml) is a QML Window
        // declared inside Main.qml, so Qt makes the main window its
        // TRANSIENT PARENT. QWindow::isActive() on a parentless window
        // includes transient children, so the main window still reads as
        // active while the call window has focus. Old code (isActive())
        // kept receipts on here; the unrelated pip above cannot show it.
        QQuickWindow fullScreen;
        fullScreen.setTransientParent(&main);
        fullScreen.resize(100, 100);
        fullScreen.show();
        fullScreen.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&fullScreen));
        QVERIFY2(main.isActive(),
                 "fixture: isActive() no longer includes transient children, "
                 "so this case cannot tell the implementations apart");
        QTRY_VERIFY2(!controller.readReceipts()->windowActive(),
                     "a focused full-screen call window (transient child of "
                     "the main window) counted as the user reading the room");
        fullScreen.hide();

        // A window the main window opened that is not a Qt Quick window of
        // its own (a native menu or dialog) still counts as the main window.
        main.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTRY_VERIFY(controller.readReceipts()->windowActive());
        QWindow menu;
        menu.setTransientParent(&main);
        menu.resize(50, 50);
        menu.show();
        menu.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&menu));
        QTest::qWait(50);
        QVERIFY2(controller.readReceipts()->windowActive(),
                 "a menu window opened by the main window paused receipts");
        menu.hide();

        main.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTRY_VERIFY(controller.readReceipts()->windowActive());
        main.hide();
        QTRY_VERIFY(!controller.readReceipts()->windowActive());
    }

    // The rule itself, with no window activation (runs on every platform
    // plugin): focus on the main window or on something it opened counts;
    // focus in another Qt Quick window does not, transient child or not.
    void focusOnACallWindowIsNotFocusOnTheMainWindow()
    {
        QQuickWindow main;
        QQuickWindow fullScreen;
        fullScreen.setTransientParent(&main);
        QQuickWindow pip;
        QWindow menu;
        menu.setTransientParent(&main);
        QWindow menuOfTheCall;
        menuOfTheCall.setTransientParent(&fullScreen);

        QVERIFY(AppController::focusBelongsToMainWindow(&main, &main));
        QVERIFY(AppController::focusBelongsToMainWindow(&main, &menu));
        QVERIFY(!AppController::focusBelongsToMainWindow(&main, nullptr));
        QVERIFY2(!AppController::focusBelongsToMainWindow(&main, &fullScreen),
                 "a transient Qt Quick window counted as the main window");
        QVERIFY(!AppController::focusBelongsToMainWindow(&main, &pip));
        QVERIFY2(!AppController::focusBelongsToMainWindow(&main,
                                                          &menuOfTheCall),
                 "a menu of the full-screen call counted as the main window");
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(NotificationGlueTest)
#include "NotificationGlueTest.moc"
