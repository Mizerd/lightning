// A SESSION RESTORED FROM THE LOCAL STORE MUST READ AS OFFLINE, AND SAYING
// SO ONCE WAS NOT ENOUGH.
//
// 2026-09-14: a homeserver went down and Lightning put the user back on the
// login page with a complete local store on disk. `build_client_for_restore`
// fixed the restore itself; this covers the other half, which is what the
// user actually sees afterwards.
//
// The first cut set `Offline` when `login_ok` arrived and believed it was
// done. It was not: `loginSucceeded` is a synchronous chain of direct
// connections ending in AppController::onLoginSucceeded -> startSync(), which
// sets `Syncing` unconditionally, and the sync lane then reports "starting",
// which is `Syncing` again. AppController maps `Syncing` to "Loading rooms…"
// — precisely the sentence an offline restore must not show over a room list
// that is already complete and will never load anything. No event-loop
// iteration separates any of it, so the Offline state was never rendered.
//
// FAIL-ON-OLD: with the `m_restoredOffline && state == Syncing` override
// removed from `setState`, `theOfflineStateSurvivesTheSyncLaneStarting` reads
// Syncing.
//
// Everything here drives the REAL event dispatcher with the REAL payload
// names. No FFI handle, no store, no network: the events are the contract
// between the Rust lane and this class, and they are what a test can hold.

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

    // THE CASE THE FIRST CUT FAILED. Everything that runs between login_ok
    // and the first sync failure reports Syncing on its way to a server that
    // is not there.
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

        // `startSync()` is unreachable without an FFI handle, but it does the
        // same single thing this does: setState(Syncing). The lane's own
        // "starting" and "retrying" are the other two, and they arrive as
        // these events verbatim.
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

    // AND IT MUST NOT STRAND A SESSION THAT RECONNECTS. A sync response is
    // the only thing that proves the server was reached, so it — and nothing
    // else — releases the override.
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
