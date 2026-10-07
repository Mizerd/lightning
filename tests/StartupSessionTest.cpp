// Authenticated startup lifecycle. A launch with a saved account is an
// explicit restoration state (BootScreen): the login form must never be
// instantiated, let alone flash, while the outcome is unknown. Only a genuine
// unauthenticated state (no account, or the restore failed) shows Login.
// Drives the real AppController and Main.qml window on the mock backend.
#include <QtTest/QtTest>

#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "storage/InsecureFallbackSecretStore.h"
#include "storage/SecretStore.h"

class FakeSecretStore final : public SecretStore
{
    Q_OBJECT

public:
    explicit FakeSecretStore(QObject *parent = nullptr)
        : SecretStore(parent) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fake"); }
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

// A native keyring the test can take off the session bus. Whether it answers
// is decided once, when a store is made, as LibSecretStore's probe does; the
// items outlive any one store object, as the keyring's do.
struct TestKeyring
{
    bool answering = true;
    QHash<QString, QString> items;
    int storesMade = 0;
    // Lookups asked of it: each one, on a locked collection, can be an unlock
    // prompt.
    int reads = 0;
};

class TestKeyringStore final : public SecretStore
{
    Q_OBJECT

public:
    TestKeyringStore(TestKeyring *keyring, QObject *parent)
        : SecretStore(parent), m_keyring(keyring), m_available(keyring->answering) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return m_available; }
    QString backendName() const override { return QStringLiteral("test keyring"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        if (!m_available)
            return false;
        m_keyring->items.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        ++m_keyring->reads;
        m_lastReadFailed = !m_available;
        return m_available ? m_keyring->items.value(userId + QLatin1Char('/') + key)
                           : QString();
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        if (!m_available)
            return false;
        m_keyring->items.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        if (!m_available)
            return false;
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_keyring->items.begin(); it != m_keyring->items.end();) {
            if (it.key().startsWith(prefix))
                it = m_keyring->items.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }
    bool lastReadFailed() const override { return m_lastReadFailed; }

private:
    TestKeyring *m_keyring;
    bool m_available;
    mutable bool m_lastReadFailed = false;
};

// What SecretStore::createDefault() gives a build with libsecret compiled in:
// the keyring when it answers, else the plaintext store standing in for it.
AppController::SecretStoreFactory keyringFactory(TestKeyring *keyring)
{
    return [keyring](QObject *parent) -> std::unique_ptr<SecretStore> {
        ++keyring->storesMade;
        if (keyring->answering)
            return std::make_unique<TestKeyringStore>(keyring, parent);
        return std::make_unique<InsecureFallbackSecretStore>(
            parent, /*substitutedForNative=*/true);
    };
}

namespace {
const QString kUser = QStringLiteral("@alice:mock.local");
const QString kHs = QStringLiteral("https://mock.local");

// Persist a mock account registry entry the way a previous run would have.
void seedSavedAccount()
{
    SettingsManager settings;
    FakeSecretStore secrets;
    settings.setSecretStore(&secrets);
    settings.saveSession(kHs, kUser, QStringLiteral("MOCKDEV"),
                         QStringLiteral("mock-token"));
}

// The same, with the token in `keyring`, as a run with the keyring up leaves it.
void seedKeyringAccount(TestKeyring *keyring, const QString &userId = kUser)
{
    TestKeyringStore secrets(keyring, nullptr);
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(kHs, userId, QStringLiteral("MOCKDEV"),
                         QStringLiteral("mock-token"));
}

// What a later launch restores from: the account records, the active
// account, the keyring marks and install id, and any plaintext secret.
QMap<QString, QVariant> sessionState()
{
    static const QStringList recordKeys = {
        QStringLiteral("userId"), QStringLiteral("homeserver"),
        QStringLiteral("deviceId"), QStringLiteral("authType"),
        QStringLiteral("storeSlug"), QStringLiteral("syncToken"),
        QStringLiteral("keyringItems"), QStringLiteral("keyringDevice"),
    };
    QSettings settings;
    QMap<QString, QVariant> out;
    for (const QString &key : settings.allKeys()) {
        const QStringList parts = key.split(QLatin1Char('/'));
        const bool record = parts.size() == 3 && parts.at(0) == QLatin1String("accounts")
            && recordKeys.contains(parts.at(2));
        if (record || key == QLatin1String("accounts/active")
            || key.startsWith(QLatin1String("keyring/"))
            || key.startsWith(QLatin1String("secrets/"))
            || key.startsWith(QLatin1String("session/"))) {
            out.insert(key, settings.value(key));
        }
    }
    return out;
}

QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, AppController &app)
{
    engine.rootContext()->setContextProperty("app", &app);
    QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
    engine.loadFromModule(QStringLiteral("MatrixClient"), QStringLiteral("Main"));
    if (createdSpy.isEmpty() && !createdSpy.wait(5000))
        return nullptr;
    return qobject_cast<QQuickWindow *>(createdSpy.at(0).at(0).value<QObject *>());
}
} // namespace

class StartupSessionTest : public QObject
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
            QStringLiteral("startup-session-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
        qunsetenv("LIGHTNING_MOCK_RESTORE_DELAY_MS");
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
    }

    // A QML collection that ran in 5 ms slices across Main.qml's build swept
    // the storage of live popup contents, and the first Connections among
    // them crashed this suite (src/app/QmlGcPolicy.h). The process collects
    // to completion, as the application does. Fails if QV4_GC_TIMELIMIT was
    // set to anything else on purpose.
    void qmlCollectionsRunToCompletion()
    {
        QCOMPARE(qgetenv("QV4_GC_TIMELIMIT"), QByteArray("0"));
    }

    // prepareForShutdown() must quiesce media playback and sync before teardown
    // and be idempotent. The actual Windows "Invalid window handle" race is
    // native-only (NOT TESTED here); this proves the ordering hook runs.
    void prepareForShutdownStopsWorkersIdempotently()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(!app.isShuttingDown());
        const int stopGen0 = app.playback()->stopGeneration();

        app.prepareForShutdown();
        QVERIFY(app.isShuttingDown());
        QVERIFY(app.playback()->stopGeneration() > stopGen0); // stopAll ran

        // A second call is a no-op: no further stop, no crash.
        const int stopGen1 = app.playback()->stopGeneration();
        app.prepareForShutdown();
        QCOMPARE(app.playback()->stopGeneration(), stopGen1);
    }

    // A valid saved session boots through the restoration state straight
    // to the main shell; the LoginScreen value never appears.
    void validSessionNeverShowsLogin()
    {
        seedSavedAccount();
        AppController app(AppController::MockBackend);
        QCOMPARE(app.currentScreen(), AppController::BootScreen);

        bool sawLogin = false;
        connect(&app, &AppController::currentScreenChanged, this,
                [&app, &sawLogin] {
            if (app.currentScreen() == AppController::LoginScreen)
                sawLogin = true;
        });
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(!sawLogin);
        QVERIFY(app.loggedIn());
        QCOMPARE(app.auth()->currentUserId(), kUser);
    }

    // The real Main.qml never instantiates the login form during a
    // valid-session launch: the restoration surface shows, then the shell.
    void mainWindowNeverInstantiatesLoginForm()
    {
        seedSavedAccount();
        // Hold the restoration state open long enough to observe it.
        qputenv("LIGHTNING_MOCK_RESTORE_DELAY_MS", "400");
        AppController app(AppController::MockBackend);
        QCOMPARE(app.currentScreen(), AppController::BootScreen);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &app);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(window != nullptr);

        // While restoring: the branded surface exists, the login form does
        // not.
        QTRY_VERIFY(window->findChild<QQuickItem *>(
                        QStringLiteral("startupRestoreSurface")) != nullptr);
        QVERIFY(window->findChild<QQuickItem *>(
                    QStringLiteral("loginScreen")) == nullptr);

        // Restoration completes into the shell; the login form still never
        // existed.
        QTRY_COMPARE_WITH_TIMEOUT(app.currentScreen(),
                                  AppController::MainScreen, 5000);
        QTRY_VERIFY(window->findChild<QQuickItem *>(
                        QStringLiteral("startupRestoreSurface")) == nullptr);
        QVERIFY(window->findChild<QQuickItem *>(
                    QStringLiteral("loginScreen")) == nullptr);
        window->close();
    }

    // The login homeserver field prefills from the account-independent login
    // prefill and stays freely editable: a typed value is never reverted.
    void loginHomeserverFieldPrefillsAndStaysEditable()
    {
        // No saved account: the app lands on the login screen.
        AppController app(AppController::MockBackend);
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &app);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(window != nullptr);

        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = window->findChild<QQuickItem *>(
                         QStringLiteral("homeserverField"))) != nullptr);

        // Prefilled from the account-independent login prefill.
        QCOMPARE(field->property("text").toString(),
                 app.settings()->loginHomeserverPrefill());

        // Typing a new server sticks, and a settings change does not revert
        // it.
        QVERIFY(field->setProperty("text",
                                   QStringLiteral("https://typed.example")));
        Q_EMIT app.settings()->homeserverUrlChanged();
        Q_EMIT app.settings()->loginHomeserverPrefillChanged();
        QCoreApplication::processEvents();
        QCOMPARE(field->property("text").toString(),
                 QStringLiteral("https://typed.example"));
        QCOMPARE(warnings, QStringList{});
        window->close();
    }

    // No saved account: the genuine unauthenticated state shows Login
    // directly (no restoration detour).
    void noAccountShowsLoginDirectly()
    {
        AppController app(AppController::MockBackend);
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(!app.loggedIn());
    }

    // A restore that actually fails is a real unauthenticated state: Boot
    // routes to Login only after the failure is known.
    void failedRestoreLandsOnLogin()
    {
        seedSavedAccount();
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend);
        QCOMPARE(app.currentScreen(), AppController::BootScreen);
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(!app.loggedIn());
    }

    // The Secret Service off the bus at launch (ksecretd exited, reported on
    // a Flatpak 2026-09-30). This used to show the plain sign-in form as if
    // signed out, and "Unlock it and try again" could not work before a
    // restart: the store is chosen once, at construction (live, 2026-09-30:
    // the keyring back, the account chip still refused). Now the screen says
    // why, and "Try again" asks the keyring again and restores in place.
    void aKeyringThatCannotAnswerIsSaidAndTryAgainRestores()
    {
        TestKeyring keyring;
        seedKeyringAccount(&keyring);
        keyring.answering = false;
        // The mock restores from the registry alone, which the Rust backend
        // cannot do without the token: fail its launch restore so the launch
        // ends where the real one does.
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(!app.loggedIn());
        QVERIFY(app.keyringUnavailable());
        QVERIFY(!app.settings()->secretsAreSecure());
        QCOMPARE(keyring.storesMade, 1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        QQuickWindow *window = loadMainWindow(engine, app);
        QVERIFY(window != nullptr);
        QPointer<QQuickItem> notice;
        QTRY_VERIFY((notice = window->findChild<QQuickItem *>(
                         QStringLiteral("keyringUnavailableNotice"))) != nullptr);
        QTRY_VERIFY(notice->isVisible());
        auto *retry = window->findChild<QQuickItem *>(QStringLiteral("keyringRetryButton"));
        QVERIFY(retry != nullptr);
        QVERIFY(retry->isVisible());
        // "Back" led to a shell reading "Reconnecting" with no token to
        // reconnect with.
        auto *back = window->findChild<QQuickItem *>(QStringLiteral("backToAppButton"));
        QVERIFY(back != nullptr);
        QVERIFY(!back->isVisible());

        // Still away: asked again, nothing changes, and it says so.
        QSignalSpy errors(&app, &AppController::errorReported);
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QCOMPARE(keyring.storesMade, 2);
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(app.keyringUnavailable());
        QVERIFY(notice->isVisible());
        QVERIFY(!app.settings()->secretsAreSecure());
        QVERIFY(!errors.isEmpty());
        QVERIFY(!errors.last().first().toString().isEmpty());

        // Back on the bus: the same process reads the saved sign-in and
        // restores it, without a restart and without a new sign-in.
        keyring.answering = true;
        QSignalSpy backend(app.settings(), &SettingsManager::secretBackendChanged);
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QCOMPARE(keyring.storesMade, 3);
        QVERIFY(!app.keyringUnavailable());
        QCOMPARE(backend.count(), 1);
        QVERIFY(app.settings()->secretsAreSecure());
        QCOMPARE(app.settings()->secretBackendName(), QStringLiteral("test keyring"));
        QCOMPARE(app.settings()->accessTokenFor(kUser), QStringLiteral("mock-token"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(app.loggedIn());
        QTRY_VERIFY(notice.isNull() || !notice->isVisible());
        QCOMPARE(warnings.filter(QStringLiteral("LoginScreen.qml")), QStringList{});
        window->close();
    }

    // Where no native store exists at all, the plaintext store is the real
    // one and a miss is a fact: no keyring notice, sign in as before.
    void noNativeKeyringAtAllShowsNoKeyringNotice()
    {
        TestKeyring elsewhere;
        seedKeyringAccount(&elsewhere);
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend, false, [](QObject *parent) {
            return std::unique_ptr<SecretStore>(
                std::make_unique<InsecureFallbackSecretStore>(parent));
        });
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(app.settings()->accessTokenFor(kUser).isEmpty());
        QVERIFY(!app.keyringUnavailable());
    }

    // CLAUDE.md §6: an unavailable keyring is transient. A launch without it
    // must leave every account record, keyring mark and secret as it found
    // them, so the next launch with the keyring back restores (measured live
    // 2026-09-30: settings file byte-identical across such a launch).
    void aLaunchWithoutTheKeyringWritesNoAccountState()
    {
        TestKeyring keyring;
        seedKeyringAccount(&keyring);
        const QMap<QString, QVariant> before = sessionState();
        QVERIFY(before.contains(QStringLiteral("accounts/active")));
        keyring.answering = false;
        // Held in the restoration state, so nothing but the launch runs.
        qputenv("LIGHTNING_MOCK_RESTORE_DELAY_MS", "600000");
        {
            AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
            QVERIFY(app.keyringUnavailable());
            QVERIFY(!app.retryKeyring());
        }
        qunsetenv("LIGHTNING_MOCK_RESTORE_DELAY_MS");
        QCOMPARE(sessionState(), before);
        QCOMPARE(keyring.items.value(kUser + QStringLiteral("/accessToken")),
                 QStringLiteral("mock-token"));
    }

    // Reported 2026-09-30 on a Flatpak: the keyring came back without the
    // sign-in this install had saved (a portal secret that changed, or
    // libsecret switching backends). The launch showed the plain sign-in form,
    // the account said "expired", and signing in led to "Quarantine and
    // rebuild". The account had not signed out. Now the launch shows why, with
    // Try again and no rebuild, and Try again restores once it is back.
    void aSignInTheKeyringLostIsExplainedWithoutARebuild()
    {
        TestKeyring keyring;
        seedKeyringAccount(&keyring);
        // It answers, and no longer has it.
        keyring.items.clear();
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(!app.keyringUnavailable());
        QCOMPARE(app.localSessionFailureReasonCode(), QStringLiteral("keyring_lost_session"));
        QCOMPARE(app.localSessionFailureUserId(), kUser);
        QVERIFY(!app.localResetHelpsFor(app.localSessionFailureReasonCode()));

        QQmlApplicationEngine engine;
        QQuickWindow *window = loadMainWindow(engine, app);
        QVERIFY(window != nullptr);
        QPointer<QQuickItem> card;
        QTRY_VERIFY((card = window->findChild<QQuickItem *>(
                         QStringLiteral("loginRepairCard"))) != nullptr);
        QTRY_VERIFY(card->isVisible());
        auto *retry = window->findChild<QQuickItem *>(QStringLiteral("loginRepairRetryKeyring"));
        QVERIFY(retry != nullptr);
        QVERIFY(retry->isVisible());
        for (const char *name : {"loginRepairPrimaryAction", "loginRepairRetry",
                                 "loginRepairRemoveAccount"}) {
            auto *button = window->findChild<QQuickItem *>(QLatin1String(name));
            QVERIFY2(!button || !button->isVisible(), name);
        }

        // Still not there: nothing changes, and it says so.
        QSignalSpy errors(&app, &AppController::errorReported);
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QCOMPARE(app.localSessionFailureReasonCode(), QStringLiteral("keyring_lost_session"));
        QVERIFY(!errors.isEmpty());
        QVERIFY(!errors.last().first().toString().isEmpty());

        // Back in the keyring: Try again restores, and the card goes.
        keyring.items.insert(kUser + QStringLiteral("/accessToken"),
                             QStringLiteral("mock-token"));
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(app.loggedIn());
        QVERIFY(app.localSessionFailureReasonCode().isEmpty());
        window->close();
    }

    // Reported 2026-10-01: a matrix.org OAuth account was signed out
    // remotely and its repair form named ANOTHER server. The failed
    // account's own record decides the server the form signs in to, never
    // what the emitter had at hand (the running handle's server, or
    // nothing), and the form built afterwards shows it.
    void aRepairFormOpensOnTheFailedAccountsRecordedServer()
    {
        const QString other = QStringLiteral("@bee:example.org");
        TestKeyring keyring;
        {
            TestKeyringStore secrets(&keyring, nullptr);
            SettingsManager settings;
            settings.setSecretStore(&secrets);
            settings.saveSession(QStringLiteral("https://example.org"), other,
                                 QStringLiteral("BEEDEV"), QStringLiteral("bee-token"),
                                 QStringLiteral("bee-refresh"), QStringLiteral("oauth"),
                                 QStringLiteral("bee-client"));
            // Saved last, so it is the active account the launch restores.
            settings.saveSession(kHs, kUser, QStringLiteral("MOCKDEV"),
                                 QStringLiteral("mock-token"));
            settings.setLoginHomeserverPrefill(QStringLiteral("https://last-used.example"));
        }
        AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        const QString recorded = app.settings()->accountRecord(other)
                                     .value(QStringLiteral("homeserver")).toString();
        QCOMPARE(recorded, QStringLiteral("https://example.org"));

        // A failure for B, named with the RUNNING account's server.
        app.setLocalSessionFailure(QStringLiteral("access_token_revoked"), other, kHs);
        QCOMPARE(app.localSessionFailureUserId(), other);
        QCOMPARE(app.localSessionFailureHomeserver(), recorded);
        // Named with no server at all.
        app.clearLocalSessionFailure();
        app.setLocalSessionFailure(QStringLiteral("access_token_revoked"), other, QString());
        QCOMPARE(app.localSessionFailureHomeserver(), recorded);
        // An account with no record keeps what it was given.
        app.clearLocalSessionFailure();
        app.setLocalSessionFailure(QStringLiteral("access_token_revoked"),
                                   QStringLiteral("@nobody:elsewhere.example"),
                                   QStringLiteral("https://elsewhere.example"));
        QCOMPARE(app.localSessionFailureHomeserver(),
                 QStringLiteral("https://elsewhere.example"));

        // The form opened after the failure (here add-account, A still
        // running) signs in to B's server, not A's and not the last-used one.
        app.setLocalSessionFailure(QStringLiteral("access_token_revoked"), other, kHs);
        QQmlApplicationEngine engine;
        QQuickWindow *window = loadMainWindow(engine, app);
        QVERIFY(window != nullptr);
        app.showLogin();
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = window->findChild<QQuickItem *>(
                         QStringLiteral("homeserverField"))) != nullptr);
        QCoreApplication::processEvents();
        QCOMPARE(field->property("text").toString(), recorded);
        auto *user = window->findChild<QQuickItem *>(QStringLiteral("userField"));
        QVERIFY(user != nullptr);
        QCOMPARE(user->property("text").toString(), other);
        app.clearLocalSessionFailure();
        window->close();
    }

    // Each lookup in a locked collection can raise an unlock prompt, and a
    // dismissed prompt reads as "no such item", so a loop over the saved
    // accounts never stopped early: one prompt per account at the launch and
    // again at every Try again. A native store answers for the whole backend
    // in one read.
    void aKeyringIsAskedOnceNotOncePerAccount()
    {
        TestKeyring keyring;
        seedKeyringAccount(&keyring, QStringLiteral("@bob:mock.local"));
        seedKeyringAccount(&keyring, QStringLiteral("@carol:mock.local"));
        seedKeyringAccount(&keyring);   // the active one
        // What three dismissed prompts look like.
        keyring.items.clear();
        keyring.reads = 0;
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
        // hasSession() is the launch's only read.
        QCOMPARE(keyring.reads, 1);
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        keyring.reads = 0;
        app.retryKeyring();
        // One to ask whether the keyring answers, one for the session.
        QVERIFY2(keyring.reads <= 2, qPrintable(QString::number(keyring.reads)));
    }

    // Removing an account whose sign-in is in a keyring that cannot be read
    // deleted its record and left a live token in the keyring, with nothing
    // left that could ever remove it.
    void anAccountIsNotRemovedWhileTheKeyringCannotBeRead()
    {
        TestKeyring keyring;
        seedKeyringAccount(&keyring);
        keyring.answering = false;
        qputenv("LIGHTNING_MOCK_FAIL_RESTORE", "1");
        AppController app(AppController::MockBackend, false, keyringFactory(&keyring));
        qunsetenv("LIGHTNING_MOCK_FAIL_RESTORE");
        QTRY_COMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(app.keyringUnavailable());
        QSignalSpy errors(&app, &AppController::errorReported);
        app.removeAccount(kUser);
        QVERIFY(app.settings()->hasSavedAccount(kUser));
        QVERIFY(!errors.isEmpty());
        QVERIFY(!errors.last().first().toString().isEmpty());
        QCOMPARE(keyring.items.value(kUser + QStringLiteral("/accessToken")),
                 QStringLiteral("mock-token"));
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(StartupSessionTest)
#include "StartupSessionTest.moc"
