// Server-synchronized per-room notification modes — the AppController entry
// point and cache-reconciliation policy:
//   * setRoomNotificationMode writes the device-local SettingsManager value
//     FIRST (NotificationManager reads it, so policy works instantly and
//     offline) and only then involves the backend;
//   * server reports reconcile the cache (the server wins): a user-defined
//     rule is adopted, and once the account's rules are known (after the
//     first sync) "no rule" reads as "Follow account default" — except for a
//     device-only legacy mode (chosen before modes were saved to the
//     server), a failed write, or this device's own write in flight;
//   * nothing but the user's own choice and its retry is ever WRITTEN to the
//     server; a device-only legacy mode decides locally and is never sent,
//     for any account on the device;
//   * a failed push-rule write flips a per-room "kept on this device" state
//     that the pickers surface and the notification policy obeys, kept per
//     account across restarts and sign-out events, cleared by the next
//     successful user-defined report;
//   * the retry reads before it writes: the server still holding the rule
//     the choice replaced means send again, anything newer is adopted;
//   * SettingsManager stores the mode per account with a lazy read-fallback
//     to the legacy device-global key; mode 0 still removes the stored key
//     where no legacy value needs shadowing;
//   * non-server backends keep the exact pre-existing local-only behavior
//     and serverRoomNotificationModes answers false.
// Boots real AppControllers with no session and no network: the Rust FFI
// wrappers no-op without a live handle and the report signals are driven
// directly, exactly as the poll dispatcher would (see VerificationFlowTest).
// No credentials, tokens, push-rule JSON, or key material appear anywhere.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"

#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

namespace {
const QString kRoomId = QStringLiteral("!room:example.org");
const QString kAliceId = QStringLiteral("@alice:example.org");
const QString kBobId = QStringLiteral("@bob:example.org");
const QString kHomeserver = QStringLiteral("https://example.org");
}

class RoomNotificationModeTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static RustSdkMatrixClient *rustClient(AppController &app)
    {
        // The concrete client is parented to the AppController (makeClient
        // passes it as the QObject parent), mirroring how AppController
        // itself locates it via qobject_cast on m_client.
        return app.findChild<RustSdkMatrixClient *>();
    }
#endif

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
            QStringLiteral("room-notification-mode-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void mockBackendKeepsTheLocalOnlyContract()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(!app.serverRoomNotificationModes());
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        // The refresh entry point is a guarded no-op without server support
        // (nothing to crash into, nothing async to wait for), and no room
        // can be in the sync-failed state.
        app.requestRoomNotificationMode(kRoomId);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
    }

    void entryPointRejectsInvalidInput()
    {
        AppController app(AppController::MockBackend);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        // Out-of-range modes and empty room ids never reach the cache. 3 is
        // valid ("follow the account default"), so the upper invalid bound
        // is 4.
        app.setRoomNotificationMode(kRoomId, -1);
        app.setRoomNotificationMode(kRoomId, 4);
        app.setRoomNotificationMode(QString{}, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        // 3 persists as an explicit choice, distinct from an absent key
        // (which reads back as 0).
        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        // Defence-in-depth: a composite thread-timeline id must never reach
        // a settings key or a protocol call.
        const QString threadId = MatrixClient::threadTimelineId(
            kRoomId, QStringLiteral("$root:example.org"));
        app.setRoomNotificationMode(threadId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(threadId), 0);
        app.requestRoomNotificationMode(threadId);
    }

    // With no active account, mode 0 removes the global key rather than
    // storing 0. (In-process QSettings share one cache, so the store is
    // inspectable without a disk sync.)
    void modeZeroStillRemovesTheLocalKey()
    {
        AppController app(AppController::MockBackend);
        const QString key =
            QStringLiteral("notifications/room-mode/") + kRoomId;
        app.setRoomNotificationMode(kRoomId, 1);
        {
            QSettings store;
            QVERIFY(store.contains(key));
        }
        app.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 0);
        {
            QSettings store;
            QVERIFY(!store.contains(key));
        }
    }

    // Each account keeps its own mode under accounts/<slug>/…; the legacy
    // device-global key serves reads until an account's first write shadows
    // it, and is never deleted (other accounts fall back to it).
    void accountsKeepIndependentModes()
    {
        // saveSession is the public account-record entry point (no
        // SecretStore wired, so the empty token fixtures stay out of every
        // secret path).
        SettingsManager settings;
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.hasSavedAccount(kBobId));

        settings.setActiveAccountUserId(kAliceId);
        settings.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        // Bob never chose anything: no inheritance from Alice.
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
        settings.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 1);

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        // Scoped mode 0 with no legacy value removes the scoped key
        // entirely (compact file) — no key for this room survives under
        // Alice's account scope.
        settings.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
        QSettings raw;
        const QStringList keys = raw.allKeys();
        int aliceKeys = 0;
        for (const QString &key : keys) {
            if (key.endsWith(QStringLiteral("notifications/room-mode/")
                             + kRoomId)
                && key.contains(QStringLiteral("alice"))) {
                ++aliceKeys;
            }
        }
        QCOMPARE(aliceKeys, 0);
    }

    void legacyGlobalModeFallsBackUntilFirstWrite()
    {
        SettingsManager settings;
        // The legacy (pre-scoping) device-global value an upgrading user
        // has on disk.
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.hasSavedAccount(kBobId));

        // Both accounts read the legacy value until they write.
        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        // Bob's explicit default must SHADOW the legacy key, not delete it
        // (deleting would steal Alice's fallback) and not be dropped
        // (removal would resurrect the legacy mute on the next read).
        settings.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);

        // Alice still falls back to her legacy mode, then writes her own.
        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);
        settings.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 1);

        // And Bob's shadowing default survived Alice's write.
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
    }

    void rustBackendReportsServerCapability()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        QVERIFY(app.serverRoomNotificationModes());
        // Without a live session the FFI wrappers refuse silently; the
        // local cache write must still land (offline-first policy).
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
#endif
    }

    void serverReportReconcilesTheCache()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();

        // An explicit user-defined rule from the server wins over the
        // (default) local value.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        // A resolved account default never mutates persisted state, even when
        // it differs, or polling the picker would destroy a device-local
        // choice.
        QSignalSpy changed(settings,
                           &SettingsManager::roomNotificationModeChanged);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(changed.count(), 0);

        // Out-of-range user-defined reports are dropped, never clamped
        // (SettingsManager would clamp to 0 — the LEAST conservative mode).
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 7, true);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, -1, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(changed.count(), 0);

        // An agreeing user-defined report is a no-op: SettingsManager's
        // idempotence means no change signal, so the pickers do not churn
        // and nothing can loop back into another server write.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(changed.count(), 0);

        // A differing user-defined report reconciles (server wins).
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 1);
#endif
    }

    void failedServerWriteFlipsTheHonestSyncState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        QSignalSpy syncState(
            &app, &AppController::roomNotificationModeSyncStateChanged);

        // The local choice stays; the room is marked kept-on-this-device.
        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 1);

        // A repeat failure for the same room does not churn the UI.
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QCOMPARE(syncState.count(), 1);

        // Other rooms are unaffected.
        QVERIFY(!app.roomNotificationModeSyncFailed(
            QStringLiteral("!other:example.org")));

        // While unsynced, a differing user-defined report is the room's old
        // rule surfacing through a poll; applying it would revert the user's
        // choice and hide the failure chip. It must change nothing.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 1);

        // Only a user-defined report EQUAL to the cached value is a real
        // write acknowledgement: it retires the failure state.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(syncState.count(), 2);

        // Once synced again, a differing user-defined report DOES
        // reconcile (another client changed the rule; server wins).
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        // A default report clears nothing (it is not a write ack) — and
        // never touches the cache.
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QCOMPARE(syncState.count(), 3);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
#endif
    }

    // A write that failed offline is retried on reconnection, and the room
    // leaves the failed state only on a server acknowledgement, not merely
    // because a retry was attempted.
    void failedWriteIsRetriedOnReconnectButNotPrematurelyCleared()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        // The retry reads the server first, which needs its rules known.
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        QSignalSpy syncState(
            &app, &AppController::roomNotificationModeSyncStateChanged);
        // Observable seam: the backend call no-ops without a live session,
        // so without this the assertions below would hold identically on
        // code that never retries at all.
        QSignalSpy retried(&app,
                           &AppController::roomNotificationModesRetried);

        // Reconnect: the edge into Syncing is what retries. Re-announcing
        // the same state must NOT retry again (no server hammering).
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        QCOMPARE(retried.first().at(0).toInt(), 1);

        // A non-Syncing state then a return to Syncing is a NEW edge.
        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 2);

        // The retry was issued but not acknowledged, so the room is still
        // disclosed as kept-on-this-device and the local value is untouched.
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 0);

        // Only the server's matching user-defined report retires it.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(syncState.count(), 1);
#endif
    }

    // Issue #15 review (MF1): a room whose choice never reached the server
    // stays "unsynced" across a restart and a sign-out event, or the account's
    // old rule would silently override the choice the picker still shows, and
    // the retry would never run again.
    void aFailedWriteSurvivesARestartAndASignOutEvent()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            app.setRoomNotificationMode(kRoomId, 1);
            Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
            QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
            // An account switch detaches the session through the same signal.
            Q_EMIT rust->loggedOut();
            QVERIFY2(app.roomNotificationModeSyncFailed(kRoomId),
                     "a sign-out event forgot the unsynced room");
        }
        // A restart.
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QVERIFY2(app.roomNotificationModeSyncFailed(kRoomId),
                 "a restart forgot the unsynced room");
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        // And the retry still runs: on the first reconnection, once the
        // account's rules are known (a read before that says "no rule").
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QCOMPARE(retried.count(), 1);
        QCOMPARE(retried.first().at(0).toInt(), 1);
        // The acknowledgement still retires it, for good.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QVERIFY(app.settings()->unsyncedRoomNotificationModes().isEmpty());
#endif
    }

    // The unsynced set is per account, like the modes themselves.
    void unsyncedRoomsArePerAccount()
    {
        SettingsManager settings;
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        settings.setActiveAccountUserId(kAliceId);
        settings.setRoomNotificationModeUnsynced(kRoomId, true);
        QVERIFY(settings.roomNotificationModeUnsynced(kRoomId));
        QCOMPARE(settings.unsyncedRoomNotificationModes(),
                 QStringList{kRoomId});

        settings.setActiveAccountUserId(kBobId);
        QVERIFY(!settings.roomNotificationModeUnsynced(kRoomId));
        QVERIFY(settings.unsyncedRoomNotificationModes().isEmpty());

        settings.setActiveAccountUserId(kAliceId);
        QVERIFY(settings.roomNotificationModeUnsynced(kRoomId));
        settings.setRoomNotificationModeUnsynced(kRoomId, false);
        QVERIFY(!settings.roomNotificationModeUnsynced(kRoomId));
        QVERIFY(settings.unsyncedRoomNotificationModes().isEmpty());
    }

    // Issue #15 review (S2, round 3): a mode from before 0.6.6 was chosen as
    // "Local setting: it does not change this room's server push rules". It
    // keeps deciding on this device, stays off the retry list, and is never
    // written to any account's rules, although every account reads it.
    void aDeviceOnlyLegacyModeDecidesLocallyAndIsNeverSent()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->saveSession(kHomeserver, kBobId,
                              QStringLiteral("BOBDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);

        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(app.roomNotificationModeDecidesLocally(kRoomId));
        // No "Couldn't save to the server": nobody tried to.
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        // The refresh asks only about the account's own keys.
        QCOMPARE(refreshed.count(), 1);
        QCOMPARE(refreshed.first().at(0).toInt(), 0);
        // Not on the retry list.
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        // A picker opening hears "no rule": the device-only mode stays.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));

        // Another account on the same device: nothing of it reaches Bob.
        settings->setActiveAccountUserId(kBobId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QVERIFY(settings->accountRoomNotificationModeRooms().isEmpty());
        QVERIFY(settings->unsyncedRoomNotificationModes().isEmpty());

        QCOMPARE(sent.count(), 0);
        {
            QSettings raw;
            const QStringList keys = raw.allKeys();
            for (const QString &key : keys) {
                QVERIFY2(!(key.startsWith(QStringLiteral("accounts/"))
                           && key.contains(QStringLiteral("room-mode"))),
                         qPrintable(QStringLiteral("an account key was "
                                                   "written: ") + key));
            }
        }

        // Once Alice's account has its own rule for the room, the server
        // decides for her again.
        settings->setActiveAccountUserId(kAliceId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(!app.roomNotificationModeDecidesLocally(kRoomId));
#endif
    }

    // Round 4: the base a failed write records is what the SERVER held, never
    // the local value. A device-only legacy mode was never on the server, so
    // taking it as the base made the retry read "no rule" as "the mute was
    // lifted elsewhere" and throw away the user's explicit choice.
    void anExplicitChoiceInADeviceOnlyRoomIsSentAgainNotAdopted()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        const QString unasked = QStringLiteral("!unasked:example.org");
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
            raw.setValue(QStringLiteral("notifications/room-mode/") + unasked,
                         1);
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(settings->roomNotificationModeDeviceOnly(unasked));

        // The picker opens: the server has no rule for the room.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        // The user picks Mentions & keywords, and in the other room (whose
        // picker never asked) Mute, and both writes fail.
        app.setRoomNotificationMode(kRoomId, 1);
        app.setRoomNotificationMode(unasked, 2);
        QCOMPARE(sent.count(), 2);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->roomNotificationModeWriteFailed(unasked);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(kRoomId), 3);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(unasked), -1);

        // The retry hears "no rule" for both: still what the server held, so
        // the choices are sent again, not adopted as "Follow account default".
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        Q_EMIT rust->roomNotificationModeChanged(unasked, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 1);
        QCOMPARE(settings->roomNotificationMode(unasked), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QVERIFY(app.roomNotificationModeSyncFailed(unasked));
        QCOMPARE(sent.count(), 4);
        QCOMPARE(sent.at(2).at(1).toInt(), 1);
        QCOMPARE(sent.at(3).at(1).toInt(), 2);
#endif
    }

    // Round 4: choosing the very mode the legacy key holds is still a choice
    // for this account, and it ends the room being device-only.
    void anExplicitChoiceEqualToTheLegacyModeWritesTheAccountKey()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        AppController app(AppController::RustBackend);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(settings->accountRoomNotificationModeRooms().contains(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
#endif
    }

    // Round 3: a mode this account stored whose rule the server no longer has
    // (lifted on another client) follows the server, and the picker says so.
    // Only reads: nothing is sent.
    void aRuleTheServerNoLongerHasIsAdoptedAsFollowDefault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);

        // Muted here earlier, and the server has that rule.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        // Before the first sync "no rule" may be the SDK's fallback rules.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        // The refresh asks about the room this account stored a mode for.
        QCOMPARE(refreshed.count(), 1);
        QCOMPARE(refreshed.first().at(0).toInt(), 1);

        // Unmuted on another client: no rule now.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 3);
        QVERIFY(!app.roomNotificationModeDecidesLocally(kRoomId));
        QCOMPARE(sent.count(), 0);
#endif
    }

    // Round 3: a read that started before this device's own write can land
    // after it. It must not replace the choice, or a failure would then keep
    // (and retry) the old value.
    void aReadRacingThisDevicesOwnWriteIsNotAdopted()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);

        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        // The failure keeps the choice, and what the server still holds.
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(kRoomId), 1);
#endif
    }

    // Round 3: the persisted retry must not overwrite a newer choice made on
    // another client. It reads first: the rule the choice replaced still
    // there means send again; anything else is newer and adopted.
    void theRetryReadsBeforeItWrites()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
            Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
            app.setRoomNotificationMode(kRoomId, 2);
            Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
            QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId),
                     1);
        }
        // A restart keeps what the retry compares with.
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        QCOMPARE(sent.count(), 0);   // a read, not a write

        // The server still holds the rule the choice replaced: sent again.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.first().at(1).toInt(), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        // Next reconnection: "All messages" was chosen on another client
        // since. Newer, so adopted, and nothing is sent.
        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 0);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(sent.count(), 1);
#endif
    }

    // Round 3, the "no rule" answers to the retry's read.
    void theRetryHandlesARuleThatIsGone()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        const QString wasMuted = QStringLiteral("!was-muted:example.org");
        const QString hadNoRule = QStringLiteral("!no-rule:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        // One room had a rule (Mute), the other followed the default.
        Q_EMIT rust->roomNotificationModeChanged(wasMuted, 2, true);
        app.setRoomNotificationMode(hadNoRule, 3);
        Q_EMIT rust->roomNotificationModeCleared(hadNoRule);
        QCOMPARE(settings->roomNotificationMode(hadNoRule), 3);
        // Both choices fail.
        app.setRoomNotificationMode(wasMuted, 1);
        app.setRoomNotificationMode(hadNoRule, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(wasMuted);
        Q_EMIT rust->roomNotificationModeWriteFailed(hadNoRule);

        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        // The mute this device replaced is gone: lifted elsewhere, adopted.
        Q_EMIT rust->roomNotificationModeChanged(wasMuted, 0, false);
        QCOMPARE(settings->roomNotificationMode(wasMuted), 3);
        QVERIFY(!app.roomNotificationModeSyncFailed(wasMuted));
        // Still no rule where there was none: the choice is sent again.
        Q_EMIT rust->roomNotificationModeChanged(hadNoRule, 0, false);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.first().at(0).toString(), hadNoRule);
        QCOMPARE(settings->roomNotificationMode(hadNoRule), 1);
        QVERIFY(app.roomNotificationModeSyncFailed(hadNoRule));
#endif
    }

    // Mode 3 is "follow the account default": server-side a rule REMOVAL,
    // never a rule whose value is 3.
    void followAccountDefaultIsStoredDistinctlyFromAnAbsentKey()
    {
        AppController app(AppController::MockBackend);
        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        // Distinct from an absent key, which reads back as 0. This is the
        // entire reason mode 3 is stored explicitly rather than represented
        // by removing the setting.
        QCOMPARE(app.settings()->roomNotificationMode(
                     QStringLiteral("!never-configured:example.org")), 0);
    }

    // A successful rule removal is the only acknowledgement a "follow account
    // default" choice can receive, and it must retire the failed state.
    void clearAcknowledgementRetiresAFailedFollowDefault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        // A rule-VALUE report must not retire it — this room has no rule.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        // The clear acknowledgement does.
        Q_EMIT rust->roomNotificationModeCleared(kRoomId);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);

        // A clear acknowledged AFTER the user picked an explicit mode
        // belongs to a superseded choice and must not retire that newer
        // choice's pending state.
        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->roomNotificationModeCleared(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(RoomNotificationModeTest)
#include "RoomNotificationModeTest.moc"
