// A session restored from the local store must read as Offline, and keep
// reading so: `loginSucceeded` runs synchronously into startSync(), which
// sets Syncing, and the sync lane then reports "starting" (Syncing again),
// which AppController shows as "Loading rooms…". Only a sync response may
// release the override.
//
// Drives the real event dispatcher with the real payload names; no FFI
// handle, store or network.

#include "app/SettingsManager.h"

#include <QJsonObject>
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

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(OfflineRestoreStateTest)
#include "OfflineRestoreStateTest.moc"
