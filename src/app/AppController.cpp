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

    m_roomList->setClient(m_client.get());
    m_timeline->setClient(m_client.get());
    m_composer->setClient(m_client.get());
    m_media->setClient(m_client.get());

    connect(m_auth.get(), &AuthManager::loginSucceeded,
            this, &AppController::onLoginSucceeded);
    connect(m_auth.get(), &AuthManager::loggedOut,
            this, &AppController::onLoggedOut);
    connect(m_client.get(), &MatrixClient::errorOccurred,
            this, &AppController::errorReported);
    connect(m_client.get(), &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState s) {
        switch (s) {
        case MatrixClient::Disconnected:
            setConnectionStatus(m_client->isLoggedIn()
                ? tr("Idle")
                : tr("Not connected"));
            break;
        case MatrixClient::Connecting:
            setConnectionStatus(tr("Connecting…"));
            break;
        case MatrixClient::Syncing:
            setConnectionStatus(tr("Syncing"));
            break;
        case MatrixClient::Error:
            setConnectionStatus(tr("Error"));
            break;
        }
    });
    setConnectionStatus(tr("Not connected"));

    // Session restore is only meaningful for backends that can actually talk
    // to a homeserver. The mock backend synthesizes its own state.
    if ((m_backend == HttpBackend || m_backend == RustBackend)
        && m_settings->hasSession()) {
        m_client->restoreSession();
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
}

void AppController::setCurrentScreen(Screen s)
{
    if (m_currentScreen == s)
        return;
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
    m_accounts->setActiveUser(m_auth->currentUserId());
    m_client->startSync();
    setCurrentScreen(MainScreen);
    Q_EMIT loggedInChanged();
}

void AppController::onLoggedOut()
{
    m_currentRoomId.clear();
    Q_EMIT currentRoomIdChanged();
    m_accounts->clearActiveUser();
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}
