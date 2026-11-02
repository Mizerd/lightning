// Active-account switch lifecycle, on a real AppController with the mock
// backend: activate a saved account, switch, check that room/composer
// targets clear, the switching state toggles, the login screen never shows
// mid-switch, logout falls back to a remaining account, and removing a
// background account leaves the active session alone.

#include "app/AccountAvatarStore.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "calls/SfuCallController.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaPlaybackController.h"
#include "models/PaginationController.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "spaces/RailLayoutStore.h"
#include "storage/AppDataPaths.h"
#include "storage/InsecureFallbackSecretStore.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>

class FakeSecretStore final : public SecretStore
{
    Q_OBJECT

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
        if (m_locked) {
            m_lastReadFailed = true;
            return {};
        }
        m_lastReadFailed = false;
        return m_values.value(userId + QLatin1Char('/') + key);
    }

    void setLocked(bool locked) { m_locked = locked; }

    void setPermanentlyUnvouched(bool unvouched) { m_unvouched = unvouched; }

    bool lastReadFailed() const override
    {
        return m_unvouched || m_lastReadFailed;
    }

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
    bool m_locked = false;
    mutable bool m_lastReadFailed = false;
    bool m_unvouched = false;
};

class RetractingCallClient final : public MockMatrixClient
{
    Q_OBJECT

public:
    using MockMatrixClient::MockMatrixClient;
    quint64 rtcRetractMembership(const QString &roomId,
                                 const QString &delayId) override
    {
        Q_UNUSED(delayId);
        retractedRooms.append(roomId);
        return ++lastOp;
    }
    void answer(quint64 op, bool ok)
    {
        Q_EMIT rtcMembershipRetracted(op, ok, QString());
    }
    QStringList retractedRooms;
    quint64 lastOp = 5000;
};

namespace {
const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:two.example");
const QString kHsOne = QStringLiteral("https://one.example");
const QString kHsTwo = QStringLiteral("https://two.example");
}

class AccountSwitchTest : public QObject
{
    Q_OBJECT

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
            QStringLiteral("account-switch-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void switchingRoomsEndsAMessageSelection()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        const QString roomA = QStringLiteral("!mock-room:mock.local");
        app.setCurrentRoomId(roomA);
        app.forward()->beginSelecting(roomA);
        QVERIFY(app.forward()->selecting());

        app.setCurrentRoomId(QString());
        QVERIFY2(!app.forward()->selecting(),
                 "the selection followed the reader out of its room");
        QCOMPARE(app.forward()->selectedCount(), 0);
    }

    void roomOpenHydratesMemberRosterOncePerSession()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client != nullptr);
        QSignalSpy rosters(client, &MatrixClient::roomMembersReceived);

        const QString roomA = QStringLiteral("!mock-room:mock.local");
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 1);

        app.setCurrentRoomId(QString());
        app.setCurrentRoomId(roomA);
        QTest::qWait(10);
        QCOMPARE(rosters.count(), 1);

        // A failed fetch un-marks the room so the next open retries. The mock
        // always succeeds, so the failure is injected through the backend's
        // signal.
        QVariantMap failed;
        failed.insert(QStringLiteral("ok"), false);
        Q_EMIT client->roomMembersReceived(0, roomA, failed);
        app.setCurrentRoomId(QString());
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 3);

        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 4);
    }

    void aSignOutWaitingForItsCallNeverSignsOutTheNextAccount()
    {
        RetractingCallClient rtc;
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);

        const QString general = QStringLiteral("!general:mock.local");
        const QString devs = QStringLiteral("!devs:mock.local");
        app.setCurrentRoomId(general);
        app.setCurrentRoomId(devs);
        QVERIFY(app.navigateRoomBack());
        QCOMPARE(app.currentRoomId(), general);
        QVERIFY(app.navigateRoomForward());
        QCOMPARE(app.currentRoomId(), devs);

        SfuCallController *call = app.groupCall();
        call->setClient(&rtc);
        call->setMembershipForTest(QStringLiteral("!call:one.example"),
                                   QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy pendingChanged(app.auth(),
                                  &AuthManager::logoutPendingChanged);
        QSignalSpy loggedOut(app.auth(), &AuthManager::loggedOut);
        QSignalSpy abandoned(app.auth(), &AuthManager::logoutAbandoned);

        app.auth()->logout();
        QVERIFY(app.auth()->logoutPending());
        QCOMPARE(pendingChanged.count(), 1);
        QCOMPARE(loggedOut.count(), 0);

        app.switchToAccount(kBob);
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);
        app.removeAccount(kBob);
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        app.showLogin();
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY2(!app.navigateRoomBack(),
                 "Back navigated while a sign-out was waiting");
        QCOMPARE(app.currentRoomId(), devs);

        rtc.answer(rtc.lastOp, true);
        QTRY_COMPARE(loggedOut.count(), 1);
        QCOMPARE(abandoned.count(), 0);
        QVERIFY(!app.auth()->logoutPending());
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    void aDeferredSignOutIsDroppedWhenTheSessionChangedMeanwhile()
    {
        MockMatrixClient client;
        AuthManager auth(&client);
        QSignalSpy loggedIn(&auth, &AuthManager::loginSucceeded);
        auth.login(QStringLiteral("https://mock.local"),
                   QStringLiteral("alice"), QStringLiteral("unused"));
        QVERIFY(loggedIn.count() == 1 || loggedIn.wait(3000));
        const QString alice = client.currentUserId();
        QVERIFY(!alice.isEmpty());

        std::function<void()> proceed;
        auth.setBeforeLogout([&proceed](std::function<void()> go) {
            proceed = std::move(go);
        });
        QSignalSpy loggedOut(&auth, &AuthManager::loggedOut);
        QSignalSpy abandoned(&auth, &AuthManager::logoutAbandoned);
        auth.logout();
        QVERIFY(auth.logoutPending());
        QVERIFY(proceed);

        Q_EMIT client.loginSucceeded(alice);
        proceed();
        QCOMPARE(abandoned.count(), 1);
        QCOMPARE(loggedOut.count(), 0);
        QVERIFY(!auth.logoutPending());
        QVERIFY(client.isLoggedIn());

        proceed = nullptr;
        auth.logout();
        QVERIFY(proceed);
        proceed();
        QCOMPARE(loggedOut.count(), 1);
    }

    void switchActivatesTargetWithoutLoginScreen()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        // Rooms the mock really lists, so Back would have somewhere to go;
        // the switch must forget them (room history is per account).
        app.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        app.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        app.setCurrentRoomId(QStringLiteral("!mock-room:mock.local"));
        QVERIFY(!app.currentRoomId().isEmpty());

        bool sawLoginScreen = false;
        connect(&app, &AppController::currentScreenChanged, this,
                [&app, &sawLoginScreen] {
            if (app.currentScreen() == AppController::LoginScreen)
                sawLoginScreen = true;
        });

        app.switchToAccount(kBob);
        QVERIFY(app.accountSwitching());
        QVERIFY(app.currentRoomId().isEmpty());

        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->deviceId(), QStringLiteral("BOBDEV"));
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(!sawLoginScreen);

        app.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QVERIFY2(!app.navigateRoomBack(),
                 "Back after an account switch reached the previous "
                 "account's rooms");
        QCOMPARE(app.currentRoomId(), QStringLiteral("!general:mock.local"));

        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    void aClickDuringARunningSwitchRunsWhenItCompletes()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);

        app.switchToAccount(kBob);
        QVERIFY(app.accountSwitching());
        app.switchToAccount(kAlice);
        app.switchToAccount(kAlice);
        QVERIFY(QTest::qWaitFor([&app] {
            return !app.accountSwitching()
                   && app.auth()->currentUserId() == kAlice;
        }, 5000));
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QTest::qWait(200);
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);
    }

    void aQueuedSwitchIsDroppedWhenAddAccountStarts()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        app.switchToAccount(kBob);
        QVERIFY(app.accountSwitching());
        app.switchToAccount(kAlice);
        app.showLogin();
        QTRY_VERIFY(!app.accountSwitching());
        QTest::qWait(300);
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
    }

    void removingTheQueuedAccountDropsTheQueuedSwitch()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        app.switchToAccount(kBob);
        QVERIFY(app.accountSwitching());
        app.switchToAccount(kAlice);
        app.removeAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTest::qWait(300);
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QVERIFY(app.settings()->hasSavedAccount(kAlice));
    }

    void repeatedSwitchingKeepsEveryRowSelectable()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(), kAlice);

        const auto rowState = [&app](const QString &userId) -> QVariantMap {
            const QVariantList rows = app.accounts()->accounts();
            for (const QVariant &row : rows) {
                const QVariantMap record = row.toMap();
                if (record.value(QStringLiteral("userId")).toString()
                    == userId)
                    return record;
            }
            return {};
        };

        const QStringList hops = { kBob, kAlice, kBob, kAlice };
        for (const QString &target : hops) {
            QSignalSpy listRefreshed(app.accounts(),
                                     &AccountManager::accountsChanged);
            QSignalSpy activeChanged(app.accounts(),
                                     &AccountManager::activeUserIdChanged);

            const QVariantMap targetRow = rowState(target);
            QVERIFY(!targetRow.isEmpty());
            QCOMPARE(targetRow.value(QStringLiteral("isActive")).toBool(),
                     false);
            QVERIFY(target != app.accounts()->activeUserId());

            app.switchToAccount(target);
            QTRY_VERIFY(!app.accountSwitching());
            QCOMPARE(app.auth()->currentUserId(), target);
            QCOMPARE(app.accounts()->activeUserId(), target);
            QCOMPARE(app.currentScreen(), AppController::MainScreen);

            QVERIFY(listRefreshed.count() > 0);
            QVERIFY(activeChanged.count() > 0);

            QCOMPARE(rowState(target).value(QStringLiteral("isActive"))
                         .toBool(),
                     true);
            const QString other = target == kAlice ? kBob : kAlice;
            QCOMPARE(rowState(other).value(QStringLiteral("isActive"))
                         .toBool(),
                     false);
        }

        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    void theSwitchedToAccountIsTheOneTheNextLaunchOpens()
    {
        {
            AppController app(AppController::MockBackend);
            FakeSecretStore secrets;
            app.settings()->setSecretStore(&secrets);
            app.settings()->saveSession(kHsOne, kAlice,
                                        QStringLiteral("ALICEDEV"),
                                        QStringLiteral("alice-token-fixture"));
            app.settings()->saveSession(kHsTwo, kBob,
                                        QStringLiteral("BOBDEV"),
                                        QStringLiteral("bob-token-fixture"));
            app.switchToAccount(kAlice);
            QTRY_VERIFY(!app.accountSwitching());
            QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

            app.switchToAccount(kBob);
            QTRY_VERIFY(!app.accountSwitching());
            QTRY_COMPARE(app.auth()->currentUserId(), kBob);
            QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        }

        AppController relaunched(AppController::MockBackend);
        QCOMPARE(relaunched.settings()->activeAccountUserId(), kBob);
        QTRY_COMPARE(relaunched.auth()->currentUserId(), kBob);
    }

    void switchToUnknownAccountFailsCleanly()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(QStringLiteral("@nobody:one.example"));
        QCOMPARE(errors.count(), 1);
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.auth()->currentUserId(), kAlice);
    }

    void logoutContinuesWithRemainingAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        // The mock does not remove the account record on logout (the Rust
        // backend does); drop it so the fallback set is realistic.
        app.settings()->clearSessionForAccount(kBob);
        app.auth()->logout();

        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
    }

    void videoPositionsGoWithTheAccountNotTheRoom()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        MediaPlaybackController *playback = app.playback();
        const QString clip = QStringLiteral("$clip:one.example");
        const QString watched = QStringLiteral("$watched:one.example");

        app.setCurrentRoomId(QStringLiteral("!mock-room:mock.local"));
        playback->rememberVideoPosition(clip, 55000, 180000);
        playback->markVideoFinished(watched);
        app.setCurrentRoomId(QString());
        QCOMPARE(playback->videoResumePosition(clip), qint64(55000));
        QVERIFY(playback->videoFinished(watched));

        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(playback->videoResumePosition(clip), qint64(0));
        QVERIFY(!playback->videoFinished(watched));

        playback->rememberVideoPosition(clip, 55000, 180000);
        playback->markVideoFinished(watched);
        app.settings()->clearSessionForAccount(kAlice);
        app.settings()->clearSessionForAccount(kBob);
        app.auth()->logout();
        QTRY_COMPARE(playback->videoResumePosition(clip), qint64(0));
        QVERIFY(!playback->videoFinished(watched));
    }

    void failedAddAccountRestoresPreviousAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        // Enter add-account mode and fail the attempt (mock magic password).
        app.showLogin();
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QSignalSpy notice(&app, &AppController::errorReported);
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("mock-fail"));

        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QTRY_VERIFY(app.auth()->isLoggedIn());
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);

        QCOMPARE(app.auth()->lastError(), QStringLiteral("mock: invalid credentials"));
        QVERIFY2(!notice.isEmpty(),
                 "no notice was shown for the failed/redirected add-account "
                 "attempt");
        bool sawReason = false;
        for (const auto &call : notice) {
            if (call.at(0).toString()
                == QStringLiteral("mock: invalid credentials"))
                sawReason = true;
        }
        QVERIFY2(sawReason,
                 "the add-account failure's real reason never reached "
                 "errorReported()");

        app.showMain();
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(app.auth()->isLoggedIn());

        app.showLogin();
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("pw"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.auth()->currentUserId(),
                 QStringLiteral("@bob:two.example"));
    }

    void savedNavigationIsPerAccountAndSurvivesASwitchBack()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QString shared = QStringLiteral("!shared:mock.local");
        app.pagination()->saveSelection(shared, QStringLiteral("$alice-pick"));
        app.pagination()->saveScrollAnchor(shared,
                                           QStringLiteral("$alice-anchor"),
                                           12.0, false);
        QVERIFY(app.pagination()->hasHistoricalScrollAnchor(shared));

        app.showLogin();
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("pw"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        const QString bob = app.auth()->currentUserId();
        QCOMPARE(bob, QStringLiteral("@bob:two.example"));
        QCOMPARE(app.pagination()->savedSelection(shared), QString());
        QVERIFY(!app.pagination()->hasHistoricalScrollAnchor(shared));

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.pagination()->savedSelection(shared),
                 QStringLiteral("$alice-pick"));
        QVERIFY(app.pagination()->hasHistoricalScrollAnchor(shared));
    }

    void removingBackgroundAccountKeepsActiveSession()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QVERIFY(QDir().mkpath(aliceRoot + QStringLiteral("/matrix-rust-sdk-store")));
        QVERIFY(QDir().mkpath(bobRoot + QStringLiteral("/matrix-rust-sdk-store")));

        app.removeAccount(kAlice);

        QVERIFY(!app.settings()->hasSavedAccount(kAlice));
        QVERIFY(app.settings()->accessTokenFor(kAlice).isEmpty());
        QVERIFY(!QDir(aliceRoot).exists());
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
        QVERIFY(QDir(bobRoot).exists());
    }

    void signOutDeletesStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        const QString starredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir(starredDir).exists());

        app.auth()->logout();

        // The mock never clears the account record on logout, so with one saved
        // account the fallback signs the same account back in. What matters is
        // that the directory was deleted: reopening the store does not create
        // it (creation waits for the first star), so the deletion stays
        // observable.
        QTRY_VERIFY(!app.accountSwitching());
        QVERIFY(!QDir(starredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 0);
    }

    void signOutDeletesTheAvatarAndTheEmptiedAccountDirectory()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QVERIFY(!aliceRoot.isEmpty() && !bobRoot.isEmpty());
        QDir(aliceRoot).removeRecursively();
        QDir(bobRoot).removeRecursively();
        app.accountAvatars()->forget(kAlice);
        app.accountAvatars()->forget(kBob);

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QVERIFY(QDir(aliceRoot).exists());
        const QByteArray png =
            QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(24, '\x01');
        QVERIFY(app.accountAvatars()->store(kAlice, png));
        QVERIFY(app.accountAvatars()->store(kBob, png));
        QVERIFY(!app.accountAvatars()->avatarUrlFor(kAlice).isEmpty());
        QVERIFY(QDir().mkpath(bobRoot));

        // The mock keeps the record on logout (the Rust backend removes it);
        // drop it so the fallback set is realistic.
        app.settings()->clearSessionForAccount(kAlice);
        app.auth()->logout();
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        QVERIFY2(app.accountAvatars()->avatarUrlFor(kAlice).isEmpty(),
                 "the signed-out account's avatar is still on disk");
        QVERIFY2(!QDir(aliceRoot).exists(),
                 "the signed-out account's emptied directory is still on disk");
        QVERIFY(!app.accountAvatars()->avatarUrlFor(kBob).isEmpty());
        QVERIFY(QDir(bobRoot).exists());
        app.accountAvatars()->forget(kBob);
    }

    void signOutKeepsAnAccountDirectoryThatStillHoldsSomething()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        QDir(aliceRoot).removeRecursively();
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QString kept = aliceRoot + QStringLiteral("/unrelated/keep.txt");
        QVERIFY(QDir().mkpath(QFileInfo(kept).absolutePath()));
        {
            QFile file(kept);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not empty");
        }

        app.settings()->clearSessionForAccount(kAlice);
        app.auth()->logout();
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        QVERIFY(QFile::exists(kept));
        QDir(aliceRoot).removeRecursively();
    }

    void removingActiveAccountDeletesStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir(aliceStarredDir).exists());

        app.removeAccount(kAlice);

        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);
        QVERIFY(!QDir(aliceStarredDir).exists());
    }

    void removingBackgroundAccountDeletesOnlyItsStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir().mkpath(aliceStarredDir));
        QFile aliceGif(aliceStarredDir + QStringLiteral("/") + QString(64, QLatin1Char('a'))
                       + QStringLiteral(".gif"));
        QVERIFY(aliceGif.open(QIODevice::WriteOnly));
        aliceGif.write("GIF89a\x10\x00\x10\x00", 10);
        aliceGif.close();

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        const QString bobStarredDir = matrix::app_data::starredGifsDir(kBob);
        QVERIFY(QDir(bobStarredDir).exists());

        app.removeAccount(kAlice);

        QVERIFY(!QDir(aliceStarredDir).exists());
        QVERIFY(QDir(bobStarredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        QCOMPARE(app.auth()->currentUserId(), kBob);
    }

    void switchingAccountsPreservesTheOutgoingStarredGifStore()
    {
        const QString aliceDirToClean = matrix::app_data::starredGifsDir(kAlice);
        const QString bobDirToClean = matrix::app_data::starredGifsDir(kBob);
        QVERIFY(!aliceDirToClean.isEmpty());
        QVERIFY(!bobDirToClean.isEmpty());
        QDir(aliceDirToClean).removeRecursively();
        QDir(bobDirToClean).removeRecursively();

        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        const QString aliceGifFile = QDir(aliceStarredDir)
                .entryList({ QStringLiteral("*.gif") }, QDir::Files)
                .value(0);
        QVERIFY(!aliceGifFile.isEmpty());

        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
        // The live store now belongs to Bob. QTRY: the open rides
        // onLoginSucceeded, which the mock delivers after accountSwitching
        // flips false.
        QTRY_COMPARE(app.gif()->starredStore()->currentDirectory(),
                     matrix::app_data::starredGifsDir(kBob));
        QCOMPARE(app.gif()->starredStore()->count(), 0);
        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
    }

    void theRailArrangementDoesNotFollowTheUserIntoTheNextAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QString aliceSpace = QStringLiteral("!alice-space:one.example");
        RailLayoutStore *rail = app.railLayout();
        QVERIFY(rail);
        const QString folder = rail->createFolder(QStringLiteral("Work"));
        QVERIFY(!folder.isEmpty());
        rail->setSpaceFolder(aliceSpace, folder);
        QCOMPARE(rail->folders().size(), 1);
        QCOMPARE(rail->folderOf(aliceSpace), folder);

        {
            QSettings raw;
            QVERIFY2(!raw.contains(QString::fromLatin1(
                         SettingsManager::kRailLayoutKey)),
                     "the arrangement was mirrored into a device-global key");
        }

        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        QVERIFY2(app.railLayout()->folders().isEmpty(),
                 "the next account inherited the previous one's folders");
        QVERIFY2(app.railLayout()->folderOf(aliceSpace).isEmpty(),
                 "the next account inherited the previous one's Space ids");

        const QString bobSpace = QStringLiteral("!bob-space:two.example");
        const QString bobFolder =
            app.railLayout()->createFolder(QStringLiteral("Personal"));
        QVERIFY(!bobFolder.isEmpty());
        app.railLayout()->setSpaceFolder(bobSpace, bobFolder);

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.railLayout()->folders().size(), 1);
        QCOMPARE(app.railLayout()->folderOf(aliceSpace), folder);
        QVERIFY(app.railLayout()->folderOf(bobSpace).isEmpty());
    }

    void removingTheActiveAccountStillDeletesItsLocalState()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        QVERIFY(!aliceRoot.isEmpty());
        QVERIFY(QDir(aliceRoot).exists());
        {
            QFile cache(aliceRoot + QStringLiteral("/cache.sqlite"));
            QVERIFY(cache.open(QIODevice::WriteOnly));
            cache.write("not a real database");
        }
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QDir().mkpath(bobRoot);
        {
            QFile bobCache(bobRoot + QStringLiteral("/cache.sqlite"));
            QVERIFY(bobCache.open(QIODevice::WriteOnly));
            bobCache.write("bob's, and nobody asked about bob");
        }

        app.removeAccount(kAlice);

        QTRY_VERIFY2(!QDir(aliceRoot).exists(),
                     "removing the signed-in account left its local data — "
                     "the account directory is named after the localpart");
        QVERIFY(!app.settings()->hasSavedAccount(kAlice));
        QVERIFY(app.settings()->accessTokenFor(kAlice).isEmpty());

        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QVERIFY(QFile::exists(bobRoot + QStringLiteral("/cache.sqlite")));
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);
    }

    void anUnreadableFallbackSecretStoreReportsThatItsReadFailed()
    {
        const QString appName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(
            QStringLiteral("account-switch-test-corrupt-secrets"));
        QString path;
        {
            QSettings probe;
            path = probe.fileName();
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("[secrets]\nthis line has no equals sign\n");
        }
        {
            InsecureFallbackSecretStore store(nullptr, false);
            QCOMPARE(store.readSecret(kAlice, QStringLiteral("accessToken")),
                     QString());
            QVERIFY2(store.lastReadFailed(),
                     "an unparsable store answered empty and called it a "
                     "fact about the account");
            QVERIFY(!store.lastError().isEmpty());
        }
        QFile::remove(path);
        QCoreApplication::setApplicationName(
            QStringLiteral("account-switch-test-healthy-secrets"));
        {
            QSettings probe;
            path = probe.fileName();
        }
        {
            InsecureFallbackSecretStore store(nullptr, false);
            QVERIFY(store.storeSecret(kAlice, QStringLiteral("accessToken"),
                                      QStringLiteral("tok")));
            QCOMPARE(store.readSecret(kAlice, QStringLiteral("accessToken")),
                     QStringLiteral("tok"));
            QVERIFY(!store.lastReadFailed());
            QCOMPARE(store.readSecret(kBob, QStringLiteral("accessToken")),
                     QString());
            QVERIFY2(!store.lastReadFailed(),
                     "a genuine miss was reported as an unreadable backend");
            QVERIFY(store.clearAccountSecrets(kAlice));
        }
        QFile::remove(path);
        QCoreApplication::setApplicationName(appName);
    }

    // ---- an unreadable credential store is not a signed-out account ----
    // A locked keyring or missing session bus makes every lookup empty. That
    // must not read as "sign-in expired": a fresh password login is refused
    // as ExistingStoreNeedsRestore, which loops back to the switch. Uses
    // HttpBackend because the classifier exempts the mock.
    void aLockedKeyringIsNotAnExpiredSignIn()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        const QString expired = QCoreApplication::translate(
            "AppController",
            "That account's sign-in has expired. Sign in to it again.");

        secrets.setLocked(true);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QCOMPARE(errors.count(), 1);
        const QString said = errors.first().first().toString();
        QVERIFY2(!said.isEmpty(), "the refusal said nothing at all");
        QVERIFY2(said != expired,
                 "a locked keyring was reported as an expired sign-in; the "
                 "one action that advice asks for is refused as "
                 "ExistingStoreNeedsRestore and lands the user back here");

        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    void aMachineWithNoKeyringStillSwitchesAccounts()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        secrets.setPermanentlyUnvouched(true);
        QVERIFY2(!app.settings()->accessTokenFor(kBob).isEmpty(),
                 "the fixture is wrong: the substituted fallback must still "
                 "read the token back");

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QVERIFY2(errors.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a machine with no keyring was refused its own account: "
                     "\"%1\"")
                     .arg(errors.isEmpty()
                              ? QString()
                              : errors.first().first().toString())));
    }

    void anAccountWithNoTokenAndAReadableBackendIsStillExpired()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);
        QVERIFY(secrets.clearAccountSecrets(kBob));
        QVERIFY(!app.settings()->secretBackendUnavailable());

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.first().first().toString(),
                 QCoreApplication::translate(
                     "AppController",
                     "Your system keyring no longer returns that account's "
                     "saved sign-in. Nothing was deleted: sign in to it again "
                     "to continue on this device."));
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    void aSignOutIntoALockedKeyringSaysWhichFailureItIs()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        secrets.setLocked(true);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.auth()->logout();

        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(errors.count() >= 1);
        QVERIFY2(!errors.last().first().toString().isEmpty(),
                 "the sign-out fallback dropped to the login screen without "
                 "saying that the credential store, not the accounts, is "
                 "what failed");
    }

    void trayAvailableCanNotifyItsBindings()
    {
        AppController app(AppController::MockBackend);
        const QMetaObject *mo = app.metaObject();
        const int idx = mo->indexOfProperty("trayAvailable");
        QVERIFY(idx >= 0);
        const QMetaProperty prop = mo->property(idx);
        QVERIFY2(prop.hasNotifySignal(),
                 "trayAvailable has no NOTIFY signal — a binding on it "
                 "freezes at whatever the platform answered on first read");
        QCOMPARE(QString::fromUtf8(prop.notifySignal().name()),
                 QStringLiteral("trayAvailableChanged"));
    }

    void defaultFileDialogFolderIsNeverRoot()
    {
        AppController app(AppController::MockBackend);
        const QUrl folder = app.defaultFileDialogFolder();
        QVERIFY2(folder.isLocalFile(),
                 "no writable Pictures/Documents/home folder was found at "
                 "all on this machine");
        QVERIFY2(folder.toLocalFile() != QStringLiteral("/"),
                 "the file dialog default folder is the filesystem root");
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(AccountSwitchTest)
#include "AccountSwitchTest.moc"
