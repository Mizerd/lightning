
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class OfflineRestoreStateTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static QJsonObject event(const QString &type)
    {
        QJsonObject out;
        out.insert(QStringLiteral("type"), type);
        return out;
    }

    static QJsonObject restoredLoginOk()
    {
        QJsonObject out = event(QStringLiteral("login_ok"));
        out.insert(QStringLiteral("homeserver"),
                   QStringLiteral("https://matrix.example"));
        out.insert(QStringLiteral("user_id"),
                   QStringLiteral("@someone:matrix.example"));
        out.insert(QStringLiteral("device_id"), QStringLiteral("DEVICEID"));
        return out;
    }

    static QJsonObject syncState(const QString &state)
    {
        QJsonObject out = event(QStringLiteral("room_list_sync_state"));
        out.insert(QStringLiteral("state"), state);
        return out;
    }

    static QString revokeServer() { return QStringLiteral("https://example.org"); }

    static QJsonObject restoredLoginOkFor(const QString &userId)
    {
        QJsonObject out = event(QStringLiteral("login_ok"));
        out.insert(QStringLiteral("homeserver"), revokeServer());
        out.insert(QStringLiteral("user_id"), userId);
        out.insert(QStringLiteral("device_id"), QStringLiteral("DEVICEID"));
        return out;
    }

    static void openingAs(RustSdkMatrixClient *client, const QString &userId)
    {
        matrix::app_data::AccountIdentity identity;
        matrix::app_data::resolveAccountIdentity(revokeServer(), userId,
                                                 &identity);
        client->m_openingIdentity = identity;
    }

    static bool touch(const QString &path)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write("x");
        return true;
    }

    static bool restoredOnMain(AppController &app, RustSdkMatrixClient *rust,
                               const QString &userId, const QStringList &others)
    {
        app.settings()->setSecretStore(nullptr);
        for (const QString &other : others) {
            app.settings()->saveSession(revokeServer(), other,
                                        QStringLiteral("OTHERDEVICE"), QString());
        }
        app.settings()->saveSession(revokeServer(), userId,
                                    QStringLiteral("DEVICEID"), QString());
        openingAs(rust, userId);
        rust->handleRustEventForTest(restoredLoginOkFor(userId));
        return rust->isLoggedIn()
               && app.currentScreen() == AppController::MainScreen;
    }
#endif

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
    }

    void anOrdinaryRestoreStillReportsSyncing()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);

        client.handleRustEventForTest(restoredLoginOk());
        QCOMPARE(client.connectionState(), MatrixClient::Disconnected);

        client.handleRustEventForTest(syncState(QStringLiteral("starting")));
        QCOMPARE(client.connectionState(), MatrixClient::Syncing);
#endif
    }

    void anOfflineRestoreReportsOfflineFromTheStart()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);

        client.handleRustEventForTest(
            event(QStringLiteral("session_restored_offline")));
        client.handleRustEventForTest(restoredLoginOk());
        QCOMPARE(client.connectionState(), MatrixClient::Offline);
#endif
    }

    void theOfflineStateSurvivesTheSyncLaneStarting()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.handleRustEventForTest(
            event(QStringLiteral("session_restored_offline")));
        client.handleRustEventForTest(restoredLoginOk());

        QJsonObject syncing = event(QStringLiteral("status"));
        syncing.insert(QStringLiteral("state"), QStringLiteral("syncing"));
        client.handleRustEventForTest(syncing);
        QVERIFY2(client.connectionState() == MatrixClient::Offline,
                 "a session that has never reached its homeserver reported "
                 "Syncing, which AppController renders as \"Loading rooms…\" "
                 "over a room list that is already complete");

        client.handleRustEventForTest(syncState(QStringLiteral("starting")));
        QCOMPARE(client.connectionState(), MatrixClient::Offline);

        client.handleRustEventForTest(syncState(QStringLiteral("retrying")));
        QCOMPARE(client.connectionState(), MatrixClient::Offline);

        client.handleRustEventForTest(syncState(QStringLiteral("offline")));
        QCOMPARE(client.connectionState(), MatrixClient::Offline);
#endif
    }

    void aSyncResponseReleasesTheOverride()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.handleRustEventForTest(
            event(QStringLiteral("session_restored_offline")));
        client.handleRustEventForTest(restoredLoginOk());
        QCOMPARE(client.connectionState(), MatrixClient::Offline);

        client.handleRustEventForTest(syncState(QStringLiteral("running")));
        QCOMPARE(client.connectionState(), MatrixClient::Syncing);

        client.handleRustEventForTest(syncState(QStringLiteral("offline")));
        QCOMPARE(client.connectionState(), MatrixClient::Offline);
        client.handleRustEventForTest(syncState(QStringLiteral("retrying")));
        QCOMPARE(client.connectionState(), MatrixClient::Syncing);
#endif
    }

    void aRevocationAfterMainShowsTheLoginCard()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        QCoreApplication::setApplicationName(
            QStringLiteral("offline-restore-revoke-main"));
        const QString alice = QStringLiteral("@alice:example.org");
        const QString bob = QStringLiteral("@bob:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = app.findChild<RustSdkMatrixClient *>();
        QVERIFY(rust);
        QVERIFY(restoredOnMain(app, rust, alice, {bob}));

        const QString starred =
            matrix::app_data::starredGifsDir(alice) + QStringLiteral("/marker");
        const QString bridge = matrix::app_data::bridgeLabelsFile(alice);
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(revokeServer(), alice,
                                                         &identity));
        const QString keys =
            identity.rustStorePath + QStringLiteral("/matrix-sdk-crypto.sqlite3");
        QVERIFY(touch(starred));
        QVERIFY(touch(bridge));
        QVERIFY(touch(keys));

        rust->handleRustEventForTest(
            event(QStringLiteral("session_token_revoked")));
        QTRY_COMPARE_WITH_TIMEOUT(app.currentScreen(),
                                  AppController::LoginScreen, 2000);
        QTest::qWait(100);
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);

        QVERIFY(!rust->isLoggedIn());
        QVERIFY(!app.loggedIn());
        QCOMPARE(app.localSessionFailureReasonCode(),
                 QStringLiteral("access_token_revoked"));
        QCOMPARE(app.localSessionFailureUserId(), alice);
        QVERIFY(app.settings()->hasSavedAccount(alice));
        QCOMPARE(app.settings()->activeAccountUserId(), alice);
        QVERIFY2(QFileInfo::exists(starred) && QFileInfo::exists(bridge),
                 "the revoked account's files were deleted as on a sign-out");
        QVERIFY(QFileInfo::exists(keys));
        QVERIFY(app.accounts()->sessionRevoked(alice));

        rust->handleRustEventForTest(restoredLoginOkFor(alice));
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.localSessionFailureReasonCode(), QString());
        QVERIFY(!app.accounts()->sessionRevoked(alice));
#endif
    }

    void aRevocationEndsTheRunningSessionWhateverLoginLastOpened()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        QCoreApplication::setApplicationName(
            QStringLiteral("offline-restore-revoke-other"));
        const QString alice = QStringLiteral("@alice2:example.org");
        const QString bob = QStringLiteral("@bob2:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = app.findChild<RustSdkMatrixClient *>();
        QVERIFY(rust);
        QVERIFY(restoredOnMain(app, rust, alice, {bob}));

        openingAs(rust, bob);
        rust->handleRustEventForTest(
            event(QStringLiteral("session_token_revoked")));
        QTRY_COMPARE_WITH_TIMEOUT(app.currentScreen(),
                                  AppController::LoginScreen, 2000);

        QVERIFY(!rust->isLoggedIn());
        QCOMPARE(app.localSessionFailureUserId(), alice);
        QVERIFY(app.accounts()->sessionRevoked(alice));
        QVERIFY(!app.accounts()->sessionRevoked(bob));
        int listed = 0;
        for (const QVariant &row : app.accounts()->accounts()) {
            const QVariantMap account = row.toMap();
            const QString id = account.value(QStringLiteral("userId")).toString();
            if (id != alice && id != bob)
                continue;
            ++listed;
            QVERIFY2(account.value(QStringLiteral("sessionRevoked")).toBool()
                         == (id == alice),
                     qPrintable(QStringLiteral("switcher mark wrong for ") + id));
        }
        QCOMPARE(listed, 2);
#endif
    }

    void aQueuedEndNeverEndsTheSessionThatReplacedIt()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        QCoreApplication::setApplicationName(
            QStringLiteral("offline-restore-revoke-replaced"));
        const QString alice = QStringLiteral("@alice4:example.org");
        const QString bob = QStringLiteral("@bob4:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = app.findChild<RustSdkMatrixClient *>();
        QVERIFY(rust);
        QVERIFY(restoredOnMain(app, rust, alice, {bob}));

        rust->handleRustEventForTest(
            event(QStringLiteral("session_token_revoked")));
        QVERIFY(rust->detachSession());
        openingAs(rust, bob);
        rust->handleRustEventForTest(restoredLoginOkFor(bob));
        QCOMPARE(rust->currentUserId(), bob);
        QTest::qWait(100);

        QVERIFY2(rust->isLoggedIn(), "a queued end for alice ended bob");
        QCOMPARE(rust->currentUserId(), bob);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
#endif
    }

    void aNetworkOrSyncErrorNeverEndsTheSession()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        QCoreApplication::setApplicationName(
            QStringLiteral("offline-restore-revoke-network"));
        const QString alice = QStringLiteral("@alice3:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = app.findChild<RustSdkMatrixClient *>();
        QVERIFY(rust);
        QVERIFY(restoredOnMain(app, rust, alice, {}));

        rust->handleRustEventForTest(syncState(QStringLiteral("offline")));
        QJsonObject network = event(QStringLiteral("room_list_error"));
        network.insert(QStringLiteral("category"), QStringLiteral("network"));
        rust->handleRustEventForTest(network);
        QJsonObject refused = event(QStringLiteral("room_list_error"));
        refused.insert(QStringLiteral("category"),
                       QStringLiteral("authentication"));
        rust->handleRustEventForTest(refused);
        QTest::qWait(100);

        QCOMPARE(rust->connectionState(), MatrixClient::Error);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(rust->isLoggedIn());
        QCOMPARE(app.localSessionFailureReasonCode(), QString());
        QVERIFY(!app.accounts()->sessionRevoked(alice));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("HomePane"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        QObject *root = created.at(0).at(0).value<QObject *>();
        QVERIFY(root);
        QObject *notice =
            root->findChild<QObject *>(QStringLiteral("homeConnectionNoticeText"));
        QVERIFY(notice);
        const QString shown = notice->property("text").toString();
        QVERIFY2(!shown.isEmpty(), "sync stopped and the home page said nothing");
        QVERIFY2(!shown.contains(QStringLiteral("offline"), Qt::CaseInsensitive),
                 qPrintable(shown));
#endif
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(OfflineRestoreStateTest)
#include "OfflineRestoreStateTest.moc"
