// v0.7: active-account switch lifecycle tests. Boot a real AppController on
// the mock backend and drive the full switch path: activate a saved account,
// switch to another, confirm the room/composer targets clear, the switching
// state toggles, the login screen is never shown mid-switch, logout falls
// back to a remaining account, and background-account removal leaves the
// active session alone.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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
        return m_values.value(userId + QLatin1Char('/') + key);
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
};

namespace {
const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:two.example");
const QString kHsOne = QStringLiteral("https://one.example");
const QString kHsTwo = QStringLiteral("https://two.example");
} // namespace

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

        // Activate alice from the (logged-out) start state.
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        // Open a room so the switch has composer/thread targets to clear.
        app.setCurrentRoomId(QStringLiteral("!mock-room:mock.local"));
        QVERIFY(!app.currentRoomId().isEmpty());

        bool sawLoginScreen = false;
        connect(&app, &AppController::currentScreenChanged, this,
                [&app, &sawLoginScreen] {
            if (app.currentScreen() == AppController::LoginScreen)
                sawLoginScreen = true;
        });

        app.switchToAccount(kBob);
        // Switching state is set synchronously and the room target is gone
        // before the previous session could route anything.
        QVERIFY(app.accountSwitching());
        QVERIFY(app.currentRoomId().isEmpty());

        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->deviceId(), QStringLiteral("BOBDEV"));
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(!sawLoginScreen);

        // Both accounts keep their credentials.
        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
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

        // The mock backend does not remove the account record on logout
        // (the Rust backend does); drop it here so the fallback set is
        // realistic.
        app.settings()->clearSessionForAccount(kBob);
        app.auth()->logout();

        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
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
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("mock-fail"));

        // The previous account's session is restored in the background; the
        // login screen stays up so the user can read the error and retry.
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QTRY_VERIFY(app.auth()->isLoggedIn());
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);

        // Back returns to a healthy shell — no stale error, live session.
        app.showMain();
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(app.auth()->isLoggedIn());

        // A retry that succeeds lands in the shell as the new account.
        app.showLogin();
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("pw"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.auth()->currentUserId(),
                 QStringLiteral("@bob:two.example"));
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

        // Give alice on-disk account state to prove removal is scoped.
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QVERIFY(QDir().mkpath(aliceRoot + QStringLiteral("/matrix-rust-sdk-store")));
        QVERIFY(QDir().mkpath(bobRoot + QStringLiteral("/matrix-rust-sdk-store")));

        app.removeAccount(kAlice);

        QVERIFY(!app.settings()->hasSavedAccount(kAlice));
        QVERIFY(app.settings()->accessTokenFor(kAlice).isEmpty());
        QVERIFY(!QDir(aliceRoot).exists());
        // The active account is untouched.
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
        QVERIFY(QDir(bobRoot).exists());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(AccountSwitchTest)
#include "AccountSwitchTest.moc"
