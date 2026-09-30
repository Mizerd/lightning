// A session restored from the local store must read as Offline, and keep
// reading so: `loginSucceeded` runs synchronously into startSync(), which
// sets Syncing, and the sync lane then reports "starting" (Syncing again),
// which AppController shows as "Loading rooms…". Only a sync response may
// release the override.
//
// Drives the real event dispatcher with the real payload names; no FFI
// handle, store or network.

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

    /// The `login_ok` a RESTORE produces: a user and a device, and no access
    /// token (the restore already had one, which is why the handler's
    /// saveSession branch is keyed on a non-empty token).
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

    // What restoreSession() records before it opens the store: the account
    // the client's session events, the revocation included, are reported for.
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

    // `userId` on the main screen the way an offline-first restore gets
    // there: from the store, before the server has judged the credential.
    // `others` are saved accounts that never run. No tokens: the secret store
    // is detached, so nothing reaches a real keyring.
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

    // The control, and it has to come first: without the offline marker the
    // states are exactly what they have always been. An override that fired
    // on every session would be far worse than the defect it fixes.
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

        // Enqueued by build_client_for_restore BEFORE its login_ok, which is
        // what lets the flag be set by the time the state is decided.
        client.handleRustEventForTest(
            event(QStringLiteral("session_restored_offline")));
        client.handleRustEventForTest(restoredLoginOk());
        QCOMPARE(client.connectionState(), MatrixClient::Offline);
#endif
    }

    // Everything between login_ok and the first sync failure reports Syncing;
    // the state must stay Offline.
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

        // `startSync()` needs an FFI handle but only does setState(Syncing);
        // the lane's "starting" and "retrying" arrive as these events.
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

    // And a session that reconnects is not stranded: a sync response proves
    // the server was reached and releases the override.
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

        // Permanently: a later reconnect cycle behaves like any other
        // session's, or one outage would pin the label for the rest of the
        // run.
        client.handleRustEventForTest(syncState(QStringLiteral("offline")));
        QCOMPARE(client.connectionState(), MatrixClient::Offline);
        client.handleRustEventForTest(syncState(QStringLiteral("retrying")));
        QCOMPARE(client.connectionState(), MatrixClient::Syncing);
#endif
    }

    // The reported defect: the offline-first restore reaches the main screen
    // before the server refuses the credential, so the failed-startup path
    // that shows the login card never runs, and the app sat on Main saying
    // "You appear to be offline". The session must end LOCALLY: the record,
    // the active account, the crypto store and every per-account file stay,
    // and no other account is switched to.
    void aRevocationAfterMainShowsTheLoginCard()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The offline restore path exists on the Rust backend only.");
#else
        // Its own settings file, so no account of another case is restored
        // when the controller is constructed.
        QCoreApplication::setApplicationName(
            QStringLiteral("offline-restore-revoke-main"));
        const QString alice = QStringLiteral("@alice:example.org");
        const QString bob = QStringLiteral("@bob:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = app.findChild<RustSdkMatrixClient *>();
        QVERIFY(rust);
        QVERIFY(restoredOnMain(app, rust, alice, {bob}));

        // What a sign-out would delete, and a revocation must not.
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
        // Nothing downstream may put Main back.
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

        // A session that signs in again is healthy again.
        rust->handleRustEventForTest(restoredLoginOkFor(alice));
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.localSessionFailureReasonCode(), QString());
        QVERIFY(!app.accounts()->sessionRevoked(alice));
#endif
    }

    // Review RV1: only the running handle reports a revocation, so it is the
    // running account's, whatever login() last opened. An add-account attempt
    // refused as "already signed in here" leaves another saved account in the
    // opening identity while this session keeps running; the revocation must
    // still end THIS session and mark THIS account, and leave the other alone.
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

    // The end is queued. If another session replaced the revoked one before it
    // runs, it must end nothing: the revocation was not that session's.
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

        // Revoked while alice runs; before the queued end is delivered, alice
        // is gone and bob is running.
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

    // A network outage, a sync that stops and even the room list's own
    // authentication error are not the SDK's definitive "could not renew":
    // none of them ends the session. And the home page must not call a sync
    // that stopped "offline".
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
