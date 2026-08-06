// Server-synchronized per-room notification modes — the AppController entry
// point and cache-reconciliation policy:
//   * setRoomNotificationMode writes the device-local SettingsManager value
//     FIRST (NotificationManager reads it, so policy works instantly and
//     offline) and only then involves the backend;
//   * ONLY user-defined server reports reconcile the cache (server wins for
//     explicit rules); resolved account DEFAULTS never mutate persisted
//     state — a device-local choice an upgrading user made before server
//     sync existed must survive opening the picker;
//   * a failed push-rule write flips a session-scoped per-room "kept on
//     this device" state that the pickers surface, cleared by the next
//     successful user-defined report;
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
        // Out-of-range modes and empty room ids never reach the cache.
        app.setRoomNotificationMode(kRoomId, -1);
        app.setRoomNotificationMode(kRoomId, 3);
        app.setRoomNotificationMode(QString{}, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        // Defence-in-depth: a composite thread-timeline id must never reach
        // a settings key or a protocol call.
        const QString threadId = MatrixClient::threadTimelineId(
            kRoomId, QStringLiteral("$root:example.org"));
        app.setRoomNotificationMode(threadId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(threadId), 0);
        app.requestRoomNotificationMode(threadId);
    }

    // The pre-existing SettingsManager storage semantics are pinned: with no
    // active account the default mode keeps the settings file compact by
    // REMOVING the global key, not by storing an explicit 0. (In-process
    // QSettings instances share one backing cache, so the store is
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

    // M4: the mode is account-derived state on the server-capable backend,
    // so each account keeps its own value under accounts/<slug>/…; the
    // legacy device-global key only serves reads until an account's first
    // write shadows it, and is never deleted (it stays the other accounts'
    // fallback).
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

        // A resolved account DEFAULT must NEVER mutate persisted state,
        // even when it differs: overwriting here would silently destroy a
        // device-local choice (for mode 0, by deleting the stored key) the
        // moment the picker polls. The display consequence — the picker
        // keeps showing the local value — is the accepted trade-off.
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

        // While unsynced, a user-defined report that DIFFERS from the
        // cached value is the room's OLD rule surfacing through a poll —
        // the failed write never reached the SDK's rules. Applying it
        // would silently revert the user's explicit choice and erase the
        // honest failure chip in one stroke. It must change nothing.
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

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(RoomNotificationModeTest)
#include "RoomNotificationModeTest.moc"
