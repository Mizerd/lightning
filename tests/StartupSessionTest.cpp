// v0.7: authenticated startup lifecycle. A launch with a saved account is
// an explicit restoration state (BootScreen): the login form must never be
// instantiated — let alone flash — while the outcome is unknown. Only a
// genuine unauthenticated state (no account, or the restore actually
// failed) shows Login. The suite drives the real AppController and the
// real Main.qml window on the mock backend.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
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
    // prefill and is FREELY EDITABLE — a typed value must not be reverted.
    // The previous live binding to homeserverUrl (the active account's server)
    // re-asserted itself and made the field impossible to point at a
    // different homeserver.
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

        // Typing a new server sticks — and a settings change (which the old
        // live binding reacted to) must not revert it.
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

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(StartupSessionTest)
#include "StartupSessionTest.moc"
