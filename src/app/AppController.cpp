#include "app/AppController.h"

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#include "matrix/CppHttpMatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "notifications/NotificationManager.h"
#include "spaces/SpaceManager.h"
#include "storage/SecretStore.h"
#include "threads/ThreadManager.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

#include "matrix/MatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApp, "matrix.app")

bool AppController::isBackendCompiled(Backend backend)
{
    switch (backend) {
    case MockBackend:
    case HttpBackend:
        return true;
    case RustBackend:
#ifdef ENABLE_RUST_SDK_BACKEND
        return true;
#else
        return false;
#endif
    }
    return false;
}

std::unique_ptr<MatrixClient> AppController::makeClient(Backend backend,
                                                        SettingsManager *settings,
                                                        QObject *parent)
{
    switch (backend) {
    case MockBackend:
        return std::make_unique<MockMatrixClient>(parent);
    case RustBackend:
#ifdef ENABLE_RUST_SDK_BACKEND
        return std::make_unique<RustSdkMatrixClient>(settings, parent);
#else
        // Construction should be rejected upstream (main.cpp checks
        // isBackendCompiled). Fall back to HTTP defensively so the app does
        // not crash if we ever reach this path.
        qCCritical(lcApp)
            << "RustBackend selected but not compiled in; falling back to HTTP";
        return std::make_unique<CppHttpMatrixClient>(settings, parent);
#endif
    case HttpBackend:
    default:
        return std::make_unique<CppHttpMatrixClient>(settings, parent);
    }
}

AppController::AppController(Backend backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_secretStore(SecretStore::createDefault(this))
    , m_settings(std::make_unique<SettingsManager>(this))
{
    // Wire the SecretStore before anything reads accessToken(). Migrates any
    // legacy plaintext token into the SecretStore on first entry.
    m_settings->setSecretStore(m_secretStore.get());

    m_client       = makeClient(backend, m_settings.get(), this);
    m_accounts     = std::make_unique<AccountManager>(this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    m_composer     = std::make_unique<MessageComposer>(this);
    m_notifications= std::make_unique<NotificationManager>(this);
    m_media        = std::make_unique<MediaManager>(this);
    m_crypto       = std::make_unique<CryptoManager>(this);
    m_spaces       = std::make_unique<SpaceManager>(this);
    m_threads      = std::make_unique<ThreadManager>(this);

    m_crypto->setBackendName(backendName());

    m_spaces->setClient(m_client.get());
    m_threads->setClient(m_client.get());
    m_roomList->setClient(m_client.get());
    m_roomList->setSpaceManager(m_spaces.get());
    m_timeline->setClient(m_client.get());
    m_composer->setClient(m_client.get());
    m_media->setClient(m_client.get());

    connect(m_auth.get(), &AuthManager::loginSucceeded,
            this, &AppController::onLoginSucceeded);
    connect(m_auth.get(), &AuthManager::loggedOut,
            this, &AppController::onLoggedOut);
    connect(m_client.get(), &MatrixClient::errorOccurred,
            this, &AppController::errorReported);
    auto refreshConnectionStatus = [this]() {
        switch (m_client->connectionState()) {
        case MatrixClient::Disconnected:
            setConnectionStatus(m_client->isLoggedIn()
                ? tr("Idle")
                : tr("Not connected"));
            break;
        case MatrixClient::Connecting:
            setConnectionStatus(tr("Connecting…"));
            break;
        case MatrixClient::Syncing:
            // v0.4.6: distinguish the initial-sync wait from steady-state
            // long-poll so a user with no rooms yet loaded doesn't stare
            // at "Syncing" and assume the app is frozen.
            //
            // v0.4.8: after initial sync completes, the long-poll is the
            // normal healthy state; label it "Connected" instead of
            // "Syncing" so users don't think Lightning is still catching
            // up when it is just waiting for new events. Real ongoing
            // work (initial catch-up, /messages backfill) has its own
            // labels ("Loading rooms…", "Refreshing…").
            setConnectionStatus(m_client->initialSyncDone()
                ? tr("Connected")
                : tr("Loading rooms…"));
            break;
        case MatrixClient::Error:
            setConnectionStatus(tr("Error"));
            break;
        }
    };
    connect(m_client.get(), &MatrixClient::connectionStateChanged,
            this, refreshConnectionStatus);
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, this, [this, refreshConnectionStatus] {
        refreshConnectionStatus();
        Q_EMIT initialSyncDoneChanged();
    });
    setConnectionStatus(tr("Not connected"));

    // v0.5.0-prep+10: recovery-key restore wiring. The Rust backend
    // exposes keyBackupResult(state, message); we bridge it into
    // AppController::recoveryStateChanged so QML can bind without
    // caring which concrete backend is active. Also proxy the redacted
    // device id so the Settings screen can show which Lightning
    // session is running.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
        connect(rust, &RustSdkMatrixClient::keyBackupResult,
                this, [this](const QString &state, const QString &message) {
            Q_EMIT recoveryStateChanged(state, message);
            // v0.5.0-prep+11: on successful key backup recovery, ask
            // the SDK to reload the current room so previously
            // undecryptable events get another chance.
            if (state == QLatin1String("ok") && !m_currentRoomId.isEmpty()) {
                reloadCurrentRoomTimeline(30);
            }
        });
        // The device id becomes available once login/restore
        // completes; propagate on both.
        connect(rust, &MatrixClient::loginSucceeded,
                this, [this](const QString &) {
            Q_EMIT rustDeviceIdChanged();
        });
        // v0.5.0-prep+11: bubble timeline reload results.
        connect(rust, &RustSdkMatrixClient::roomTimelineReloaded,
                this, [this](const QString &roomId, int t, int d, int u) {
            if (roomId == m_currentRoomId)
                Q_EMIT currentRoomTimelineReloaded(t, d, u);
        });
        // v0.5.0 SAS verification signal bridge. QML binds
        // verificationStateChanged() and reads the seven derived
        // properties. All state lives in AppController so QML doesn't
        // reach into concrete backend types.
        connect(rust, &RustSdkMatrixClient::verificationRequestReceived,
                this, [this](const QString &flowId, const QString &otherUser,
                             const QString &otherDevice, bool isSelf) {
            m_verificationFlowId = flowId;
            m_verificationOtherUser = otherUser;
            m_verificationOtherDevice = otherDevice;
            m_verificationIsSelf = isSelf;
            m_verificationState = QStringLiteral("requested");
            m_verificationEmojis.clear();
            m_verificationDecimals.clear();
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationSasReady,
                this, [this](const QString &flowId,
                             const QVariantList &emojis,
                             const QVariantList &decimals) {
            if (flowId != m_verificationFlowId) return;
            m_verificationEmojis = emojis;
            m_verificationDecimals = decimals;
            m_verificationState = QStringLiteral("sas_ready");
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationDone,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("done");
            Q_EMIT verificationStateChanged();
            // v0.5.1: post-verification retry. matrix-sdk 0.18 does not
            // expose an explicit per-event "request room key" API on
            // Client — internal event_cache/redecryptor.rs re-runs
            // automatically as keys arrive. Best surface action is a
            // Room::messages reload; if verified peers have shared
            // keys since we first saw the events, decryption succeeds
            // this time. Idempotent by event_id.
            if (!m_currentRoomId.isEmpty()) {
                qCInfo(lcApp) << "verification=done; reloading current room"
                              << m_currentRoomId.right(12);
                reloadCurrentRoomTimeline(50);
            }
        });
        connect(rust, &RustSdkMatrixClient::verificationCancelled,
                this, [this](const QString &flowId, const QString &) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("cancelled");
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationFailed,
                this, [this](const QString &flowId, const QString &msg) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("failed:%1").arg(msg);
            Q_EMIT verificationStateChanged();
        });

        connect(rust, &RustSdkMatrixClient::localSessionResetRequired,
                this, [this](const QString &) {
            setLocalRustResetRequired(true);
            Q_EMIT storeDeviceMismatchDetected(tr(
                "This local Lightning Rust SDK store belongs to a different "
                "Matrix session or device. Reset the local Lightning session "
                "for this account, then sign in again. This does not delete "
                "server messages or Element data."));
        });
        connect(rust, &RustSdkMatrixClient::localSessionCleanupFinished,
                this, [this](bool ok, const QString &message) {
            setLocalRustResetRequired(!ok);
            if (m_resetResultPending) {
                m_resetResultPending = false;
                Q_EMIT localRustStoreResetResult(ok, message);
            }
        });
    }
#endif

    // Session restore is only meaningful for backends that can actually talk
    // to a homeserver. The mock backend synthesizes its own state.
    if ((m_backend == HttpBackend || m_backend == RustBackend)
        && m_settings->hasSession()) {
        m_client->restoreSession();
        Q_EMIT rustDeviceIdChanged();
    }
}

AppController::~AppController() = default;

bool AppController::loggedIn() const
{
    return m_auth && m_auth->isLoggedIn();
}

QString AppController::backendName() const
{
    switch (m_backend) {
    case MockBackend: return QStringLiteral("mock");
    case RustBackend: return QStringLiteral("rust");
    case HttpBackend:
    default:          return QStringLiteral("http");
    }
}

SettingsManager *AppController::settings() const { return m_settings.get(); }
AuthManager *AppController::auth() const { return m_auth.get(); }
AccountManager *AppController::accounts() const { return m_accounts.get(); }
RoomListModel *AppController::roomList() const { return m_roomList.get(); }
TimelineModel *AppController::timeline() const { return m_timeline.get(); }
MessageComposer *AppController::composer() const { return m_composer.get(); }
MediaManager *AppController::media() const { return m_media.get(); }
CryptoManager *AppController::crypto() const { return m_crypto.get(); }
SpaceManager *AppController::spaces() const { return m_spaces.get(); }
ThreadManager *AppController::threads() const { return m_threads.get(); }

bool AppController::initialSyncDone() const
{
    return m_client && m_client->initialSyncDone();
}

QString AppController::rustDeviceIdRedacted() const
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client)
        return {};
    auto *rust = qobject_cast<const RustSdkMatrixClient *>(m_client.get());
    if (!rust) return {};
    const QString id = rust->currentDeviceId().trimmed();
    if (id.isEmpty()) return {};
    if (id.size() <= 8) return id;
    return id.left(4) + QLatin1String("...") + id.right(4);
#else
    return {};
#endif
}

void AppController::requestRecoverFromBackup(const QString &recoveryKey)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Recovery is only available on the Rust backend."));
        return;
    }
    if (recoveryKey.trimmed().isEmpty()) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Recovery key is empty."));
        return;
    }
    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    if (!rust) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Rust backend not available."));
        return;
    }
    // Emit "attempted" first so the button flips to "Running…" the
    // moment the user clicks. matrix-sdk will subsequently emit its
    // own attempted event over the FFI; the QML side is idempotent.
    Q_EMIT recoveryStateChanged(QStringLiteral("attempted"), QString());
    rust->recoverFromBackup(recoveryKey);
#else
    Q_UNUSED(recoveryKey);
    Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::setCurrentRoomId(const QString &roomId)
{
    if (m_currentRoomId == roomId)
        return;
    m_currentRoomId = roomId;
    m_timeline->setRoomId(roomId);
    m_composer->setRoomId(roomId);
    Q_EMIT currentRoomIdChanged();
}

void AppController::showLogin()      { setCurrentScreen(LoginScreen); }
void AppController::showMain()       { setCurrentScreen(MainScreen); }
void AppController::showSettings()   { setCurrentScreen(SettingsScreen); }

void AppController::openRoom(const QString &roomId)
{
    setCurrentRoomId(roomId);
    // v0.5.0-prep+11: Rust backend doesn't persist encrypted event
    // plaintext to CacheStore (by design), so after a restart the
    // room appears empty. Reload recent history from matrix-sdk on
    // room selection; the wrapper dedupes by event_id.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend == RustBackend && !roomId.isEmpty()) {
        reloadCurrentRoomTimeline(30);
    }
#endif
}

void AppController::reloadCurrentRoomTimeline(int limit)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_currentRoomId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->reloadRoomTimeline(m_currentRoomId, limit);
#else
    Q_UNUSED(limit);
#endif
}

void AppController::acceptVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->acceptVerification(m_verificationFlowId);
#endif
}

void AppController::confirmVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->confirmVerification(m_verificationFlowId);
#endif
}

void AppController::mismatchVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->mismatchVerification(m_verificationFlowId);
#endif
}

void AppController::cancelVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->cancelVerification(m_verificationFlowId);
    m_verificationFlowId.clear();
    m_verificationState.clear();
    m_verificationEmojis.clear();
    m_verificationDecimals.clear();
    Q_EMIT verificationStateChanged();
#endif
}

void AppController::resetLocalRustStore()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Reset is only available on the Rust backend."));
        return;
    }
    const QString homeserver = m_client && !m_client->homeserverUrl().isEmpty()
        ? m_client->homeserverUrl()
        : (m_settings ? m_settings->homeserverUrl() : QString{});
    const QString userId = m_client && !m_client->currentUserId().isEmpty()
        ? m_client->currentUserId()
        : (m_settings ? m_settings->userId() : QString{});
    if (homeserver.isEmpty() || userId.isEmpty()) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Enter a valid homeserver and Matrix user ID before resetting "
               "the local Lightning session."));
        return;
    }

    if (m_client && m_client->isLoggedIn()) {
        // The normal Rust sign-out lifecycle performs the same account-scoped
        // deletion after server logout and handle release.
        m_resetResultPending = true;
        m_auth->logout();
    } else {
        resetLocalRustSession(homeserver, userId);
    }
#else
    Q_EMIT localRustStoreResetResult(false,
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::resetLocalRustSession(const QString &homeserver,
                                          const QString &user)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Reset is only available on the Rust backend."));
        return;
    }

    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(
            homeserver, user, &identity)) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Enter a valid homeserver and Matrix user ID before resetting "
               "the local Lightning session."));
        return;
    }

    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    QString message;
    const bool ok = rust && rust->resetLocalSession(identity, &message);
    setLocalRustResetRequired(!ok);
    if (ok) {
        m_auth->clearLastError();
        Q_EMIT errorReported(QString{});
    }
    Q_EMIT localRustStoreResetResult(ok, message.isEmpty()
        ? tr("Lightning could not completely reset the local session for this "
             "account. Check the application logs and filesystem permissions, "
             "then try again.")
        : message);
#else
    Q_UNUSED(homeserver);
    Q_UNUSED(user);
    Q_EMIT localRustStoreResetResult(false,
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::setLocalRustResetRequired(bool required)
{
    if (m_localRustResetRequired == required)
        return;
    m_localRustResetRequired = required;
    Q_EMIT localRustResetRequiredChanged();
}

void AppController::setCurrentScreen(Screen s)
{
    if (m_currentScreen == s)
        return;
    qCInfo(lcApp) << "screen change" << int(m_currentScreen) << "->" << int(s)
                  << "(0=Login, 1=Main, 2=Settings)";
    m_currentScreen = s;
    Q_EMIT currentScreenChanged();
}

void AppController::setConnectionStatus(const QString &s)
{
    if (m_connectionStatus == s)
        return;
    m_connectionStatus = s;
    Q_EMIT connectionStatusChanged();
}

void AppController::onLoginSucceeded()
{
    const QString uid = m_auth->currentUserId();
    qCInfo(lcApp) << "login succeeded slug="
                  << matrix::app_data::safeUserSlug(uid)
                  << "— switching to main + starting sync";
    m_accounts->setActiveUser(uid);
    setLocalRustResetRequired(false);
    Q_EMIT errorReported(QString{});
    m_client->startSync();
    setCurrentScreen(MainScreen);
    Q_EMIT loggedInChanged();
}

void AppController::onLoggedOut()
{
    m_currentRoomId.clear();
    Q_EMIT currentRoomIdChanged();
    m_accounts->clearActiveUser();
    Q_EMIT errorReported(QString{});
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}
