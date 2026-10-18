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
#include "spaces/SpaceManager.h"

#include <QFile>
#include <QJsonArray>
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

// Answers with a fixed room list, so a Space's rooms are what the test says
// (SpaceManager rebuilds on roomsChanged, production's own path). Only the
// AppController's SpaceManager is pointed at it.
class SpaceFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomList;

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return kAliceId; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

const QString kSpaceId = QStringLiteral("!space:example.org");
const QString kAlphaId = QStringLiteral("!alpha:example.org");
const QString kBetaId = QStringLiteral("!beta:example.org");
const QString kGammaId = QStringLiteral("!gamma:example.org");
const QString kDeltaId = QStringLiteral("!delta:example.org");

RoomInfo joinedRoom(const QString &id, bool isSpace = false)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.isSpace = isSpace;
    info.membership = RoomInfo::Joined;
    return info;
}

// Points `app`'s SpaceManager at one Space holding Alpha..Delta.
void attachSpace(AppController &app)
{
    auto *fake = new SpaceFakeClient(&app);
    RoomInfo space = joinedRoom(kSpaceId, true);
    space.childRoomIds = { kAlphaId, kBetaId, kGammaId, kDeltaId };
    fake->roomList = { space, joinedRoom(kAlphaId), joinedRoom(kBetaId),
                       joinedRoom(kGammaId), joinedRoom(kDeltaId) };
    app.spaces()->setClient(fake);
    fake->announce();
    QCoreApplication::processEvents();
}
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

    // A user rule reported BEFORE the first sync may be the SDK's fallback
    // set, not this account's: it is not "what the server held", so a write
    // that fails records an unknown base (-1: the retry sends again) instead
    // of that rule. Fails on the old code, which recorded 1.
    void aRuleReportedBeforeTheFirstSyncIsNotAFailedWritesBase()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId),
                 -1);
#endif
    }

    // The user's latest choice wins. A is acknowledged, B then fails: the
    // server holds A, which is this device's own earlier write and not a rule
    // set elsewhere, so the retry sends B again. Fails on the old code, whose
    // base stayed at what the server held before A and so adopted A.
    void theRetryResendsTheLatestChoiceWhenAnEarlierOneWasAcknowledged()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(sent.count(), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId), 2);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        // The retry's read: the server holds A.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(sent.count(), 3);
        QCOMPARE(sent.last().at(1).toInt(), 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        // Its acknowledgement retires the failure.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
#endif
    }

    // The same two choices the other way round: A fails, B is acknowledged.
    // The room ends synced with B and A is never sent again, whichever of the
    // two outcomes reaches the controller first.
    void anEarlierChoicesFailureDoesNotResendItAfterALaterOneIsAcknowledged()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);

        // The failure first.
        app.setRoomNotificationMode(kRoomId, 2);
        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        QCOMPARE(sent.count(), 2);

        // The acknowledgement first: the late failure leaves the flag, and
        // the retry's read finding B on the server retires it without a send.
        app.setRoomNotificationMode(kRoomId, 0);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 4);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(sent.count(), 4);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
#endif
    }

    // A choice made while a retry's read is pending supersedes the retry: the
    // read may predate it, so neither a second send of C nor adopting
    // the old rule may follow. Fails on the old code, which sent C again.
    void aLaterChoiceSupersedesAPendingRetry()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);

        // C, while the read is on its way.
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        // The read lands: the rule the failed write replaced.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        // C's acknowledgement retires the failure.
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
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

    // Changing an account default must re-read the per-room modes at once
    // (the same refresh the first sync and a remote rule change run), not wait
    // for the next time a room is opened.
    void anAccountDefaultChangeRefreshesTheRoomModes()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);
        QVERIFY(app.pushRules() != nullptr);
        Q_EMIT app.pushRules()->accountDefaultChanged();
        QCOMPARE(refreshed.count(), 1);
#endif
    }

    // B013: "Mute space" then "Unmute space" deleted a room's own
    // "Mentions & keywords" rule. Unmute now restores what each room held
    // when the Space was muted, across a restart, and leaves alone a room
    // changed on its own while the Space was muted.
    void unmutingASpaceRestoresEachRoomsOwnMode()
    {
        {
            SettingsManager settings;
            settings.saveSession(kHomeserver, kAliceId,
                                 QStringLiteral("ALICEDEV"), QString{});
            settings.setActiveAccountUserId(kAliceId);
        }
        {
            AppController app(AppController::MockBackend);
            QCOMPARE(app.settings()->activeAccountUserId(), kAliceId);
            attachSpace(app);
            QCOMPARE(app.spaces()->roomsInSpace(kSpaceId).size(), 4);
            // Alpha never configured; Beta on mentions; Gamma muted on its
            // own; Delta following the default explicitly.
            app.setRoomNotificationMode(kBetaId, 1);
            app.setRoomNotificationMode(kGammaId, 2);
            app.setRoomNotificationMode(kDeltaId, 3);

            app.setSpaceMuted(kSpaceId, true);
            QVERIFY(app.spaceIsMuted(kSpaceId));
            // Muting twice must not record "muted" as anyone's prior mode.
            app.setSpaceMuted(kSpaceId, true);
            // Delta is changed on its own while the Space is muted.
            app.setRoomNotificationMode(kDeltaId, 1);
        }
        // A restart between mute and unmute.
        AppController app(AppController::MockBackend);
        QCOMPARE(app.settings()->activeAccountUserId(), kAliceId);
        attachSpace(app);
        auto *settings = app.settings();
        app.setSpaceMuted(kSpaceId, false);
        QCOMPARE(settings->roomNotificationMode(kAlphaId), 3);
        QCOMPARE(settings->roomNotificationMode(kBetaId), 1);
        QCOMPARE(settings->roomNotificationMode(kGammaId), 2);
        QCOMPARE(settings->roomNotificationMode(kDeltaId), 1);
        QVERIFY(!app.spaceIsMuted(kSpaceId));

        // The records are spent: a second unmute asserts nothing, and a room
        // muted again with no record follows the account default.
        app.setRoomNotificationMode(kBetaId, 2);
        app.setSpaceMuted(kSpaceId, false);
        QCOMPARE(settings->roomNotificationMode(kBetaId), 3);
        QCOMPARE(settings->roomNotificationMode(kGammaId), 3);
    }

    // ── Open-room lifecycle (B030, B035) ──
    // Here because this suite may feed RustSdkMatrixClient real room-list
    // payloads (handleRustEventForTest), which these cases need.

    // B035: the open room leaving the list (an invite declined from the room
    // list, a kick or leave elsewhere) closes it, as Leave does. A room listed
    // as left goes at once, one merely absent after the invite grace, and one
    // never seen listed (a restore before the list loaded) stays.
    void anOpenRoomThatLeavesTheListIsClosed()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The room list is driven through the Rust backend here.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString joined = QStringLiteral("!joined:example.org");
        const QString invite = QStringLiteral("!invite:example.org");
        const auto listed = [](const QString &id, const char *membership) {
            return QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), id},
                               {QStringLiteral("membership"),
                                QString::fromLatin1(membership)}};
        };
        const auto publish = [rust](const QJsonArray &rooms) {
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("rooms")},
                {QStringLiteral("rooms"), rooms}});
        };
        publish({listed(joined, "joined"), listed(invite, "invited")});

        // Declined from the list: the SDK's list drops it.
        app.openRoom(invite);
        QCOMPARE(app.currentRoomId(), invite);
        publish({listed(joined, "joined")});
        QCOMPARE(app.currentRoomId(), invite);   // a re-sort looks the same
        QTRY_COMPARE_WITH_TIMEOUT(app.currentRoomId(), QString(),
                                  InviteNoticeTracker::kGoneGraceMs + 2000);

        // Back within the grace: nothing happens.
        publish({listed(joined, "joined"), listed(invite, "invited")});
        app.openRoom(invite);
        publish({listed(joined, "joined")});
        publish({listed(joined, "joined"), listed(invite, "invited")});
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), invite);

        // The close is keyed to the opening that armed it: the same id
        // opened again within the grace (here by moving away and back; after
        // an account switch the next session can do the same before its list
        // loads) is a new opening, and an unlisted one is left alone.
        app.openRoom(invite);
        publish({listed(joined, "joined")});
        app.openRoom(joined);
        app.openRoom(invite);
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), invite);

        // Listed as left (kicked, or left elsewhere): closed at once.
        app.openRoom(joined);
        publish({listed(joined, "left")});
        QCOMPARE(app.currentRoomId(), QString());

        // Never listed: left alone.
        const QString unlisted = QStringLiteral("!unlisted:example.org");
        app.openRoom(unlisted);
        publish({listed(joined, "joined")});
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), unlisted);
#endif
    }

    // Break-it rerun section 4 F3 / B034: a room left from another session,
    // or one we were kicked or banned from, is never named again by some
    // servers' sliding sync; rust/src/membership_lost.rs asks the server and
    // reports `room_membership_lost`. The open room closes at once (no
    // grace: the server said so) and the strip says why. A kick or ban of a
    // room that was not open is still said; our own leave elsewhere is not.
    void aRoomLostElsewhereClosesAtOnceAndSaysWhy()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The membership reconciler runs on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString open = QStringLiteral("!open:example.org");
        const QString other = QStringLiteral("!other:example.org");
        const QString quiet = QStringLiteral("!quiet:example.org");
        QJsonArray rooms;
        for (const QString &id : {open, other, quiet}) {
            rooms.append(QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("name"), id},
                                     {QStringLiteral("membership"),
                                      QStringLiteral("joined")}});
        }
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("rooms")},
            {QStringLiteral("rooms"), rooms}});
        const auto lost = [rust](const QString &id, const char *loss,
                                 const QString &name) {
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("room_membership_lost")},
                {QStringLiteral("room_id"), id},
                {QStringLiteral("loss"), QString::fromLatin1(loss)},
                {QStringLiteral("name"), name}});
        };
        QSignalSpy reported(&app, &AppController::errorReported);

        app.openRoom(open);
        QCOMPARE(app.currentRoomId(), open);
        lost(open, "kicked", QStringLiteral("Ops"));
        // At once: no event-loop turn, no grace timer.
        QCOMPARE(app.currentRoomId(), QString());
        QVERIFY(!reported.isEmpty());
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You were removed from Ops."));

        // Not open: a ban is said, with the registry's name when the event
        // carries none; our own leave elsewhere is not.
        reported.clear();
        lost(other, "banned", QString());
        QCOMPARE(reported.size(), 1);
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You were banned from %1.").arg(other));
        reported.clear();
        lost(quiet, "left", QStringLiteral("Quiet"));
        QVERIFY(reported.isEmpty());

        // Open and left from another session: closed, and said.
        app.openRoom(quiet);
        lost(quiet, "left", QStringLiteral("Quiet"));
        QCOMPARE(app.currentRoomId(), QString());
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You left Quiet from another session."));
#endif
    }

    // A3 (2026-10-18 inventory): `room_membership_lost` carries no account
    // stamp of its own. Its stamp is the handle it was queued on: the drain
    // dispatches with that handle's generation, and an account switch always
    // starts a new handle and generation. A loss the old account queued and
    // a drain delivers after the switch must not close the same room id in
    // the new account, nor say anything there.
    void aLossFromTheAccountBeforeASwitchLeavesTheSameRoomOpen()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The membership reconciler runs on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString shared = QStringLiteral("!shared:example.org");
        const QJsonObject list{
            {QStringLiteral("type"), QStringLiteral("rooms")},
            {QStringLiteral("rooms"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), shared},
                                    {QStringLiteral("name"), shared},
                                    {QStringLiteral("membership"),
                                     QStringLiteral("joined")}}}}};
        const QJsonObject lost{
            {QStringLiteral("type"), QStringLiteral("room_membership_lost")},
            {QStringLiteral("room_id"), shared},
            {QStringLiteral("loss"), QStringLiteral("kicked")},
            {QStringLiteral("name"), QStringLiteral("Ops")}};

        // The old account's handle queues the loss.
        rust->handleRustEventForTest(list);
        const quint64 oldAccount = rust->m_lifecycle.activeGeneration();
        QVERIFY(oldAccount != 0);

        // The switch: what ensureRustHandleForStorePath() does for the next
        // account's fresh handle. That account lists and opens the same id.
        rust->m_handleGeneration = rust->m_lifecycle.beginSession();
        QVERIFY(rust->m_handleGeneration != oldAccount);
        rust->handleRustEventForTest(list);
        app.openRoom(shared);
        QCOMPARE(app.currentRoomId(), shared);
        QSignalSpy reported(&app, &AppController::errorReported);

        // The drain delivers the old account's loss.
        rust->handleRustEvent(lost, oldAccount);
        QCOMPARE(app.currentRoomId(), shared);
        QVERIFY(reported.isEmpty());

        // The same loss from the new account's own handle closes it: the
        // handler was reachable, so the stamp is what kept it open.
        rust->handleRustEvent(lost, rust->m_lifecycle.activeGeneration());
        QCOMPARE(app.currentRoomId(), QString());
        QCOMPARE(reported.size(), 1);
#endif
    }

    // The stamp above holds only while no event queue outlives its account:
    // a new handle (and so a new queue and generation) is made only after
    // the running one was released, and both drain loops dispatch only at
    // the live generation. A source contract, since the drain needs a live
    // FFI handle.
    void anEventQueueNeverOutlivesItsAccount()
    {
        QFile file(QStringLiteral(QT_TESTCASE_SOURCEDIR
                                  "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(file.errorString()));
        const QString source = QString::fromUtf8(file.readAll());
        const auto body = [&source](const QString &signature) {
            const qsizetype start = source.indexOf(signature);
            if (start < 0)
                return QString();
            const qsizetype end = source.indexOf(
                QLatin1String("\nbool RustSdkMatrixClient::"), start + 1);
            const qsizetype otherEnd = source.indexOf(
                QLatin1String("\nvoid RustSdkMatrixClient::"), start + 1);
            qsizetype stop = end < 0 ? source.size() : end;
            if (otherEnd > 0 && otherEnd < stop)
                stop = otherEnd;
            return source.mid(start, stop - start);
        };

        const QString ensure = body(QStringLiteral(
            "bool RustSdkMatrixClient::ensureRustHandleForStorePath("));
        QVERIFY(!ensure.isEmpty());
        const qsizetype released = ensure.indexOf(QLatin1String("releaseRustHandle();"));
        const qsizetype created = ensure.indexOf(QLatin1String("mx_rust_create("));
        const qsizetype stamped = ensure.indexOf(
            QLatin1String("m_handleGeneration = m_lifecycle.beginSession();"));
        QVERIFY2(released >= 0 && created > released,
                 "a new handle is created while the old one may still be polled");
        QVERIFY2(stamped > created, "a new handle reuses the old generation");

        const QString poll = body(QStringLiteral("void RustSdkMatrixClient::pollRustEvents()"));
        QVERIFY(!poll.isEmpty());
        QVERIFY(poll.contains(QLatin1String("const quint64 eventGeneration = m_handleGeneration;")));
        QCOMPARE(poll.count(QStringLiteral("if (m_lifecycle.acceptsActive(eventGeneration)) {")), 2);
    }

    // B030: matrix-sdk clears every room's event cache on an ignore-list
    // change, which emptied the open timelines for good. The open room and
    // thread are reopened, and only if still the ones open.
    void anIgnoreListChangeReopensTheOpenTimelines()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The SDK event cache exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString room = QStringLiteral("!room-a:example.org");
        const QString other = QStringLiteral("!room-b:example.org");
        const QString root = QStringLiteral("$root:example.org");
        QSignalSpy reloaded(&app,
                            &AppController::timelinesReloadedAfterIgnoreChange);

        app.openRoom(room);
        app.thread()->openThread(room, root);
        QVERIFY(app.thread()->state() != ThreadController::Closed);
        Q_EMIT rust->ignoredUsersChanged({QStringLiteral("@troll:example.org")});
        QTRY_COMPARE(reloaded.count(), 1);
        QCOMPARE(reloaded.at(0).at(0).toBool(), true);
        QCOMPARE(reloaded.at(0).at(1).toBool(), true);
        QCOMPARE(app.thread()->rootEventId(), root);

        // The user moved on before the reload: nothing stale is reopened.
        Q_EMIT rust->ignoredUsersChanged({});
        app.openRoom(other);
        QTRY_COMPARE(reloaded.count(), 2);
        QCOMPARE(reloaded.at(1).at(0).toBool(), false);
        QCOMPARE(reloaded.at(1).at(1).toBool(), false);
        QCOMPARE(app.currentRoomId(), other);
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(RoomNotificationModeTest)
#include "RoomNotificationModeTest.moc"
