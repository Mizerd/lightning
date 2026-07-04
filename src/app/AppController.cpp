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
#include "threads/ThreadManager.h"

std::unique_ptr<MatrixClient> AppController::makeClient(Backend backend,
                                                        SettingsManager *settings,
                                                        QObject *parent)
{
    switch (backend) {
    case MockBackend:
        return std::make_unique<MockMatrixClient>(parent);
    case HttpBackend:
    default:
        return std::make_unique<CppHttpMatrixClient>(settings, parent);
    }
}

AppController::AppController(Backend backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_settings(std::make_unique<SettingsManager>(this))
    , m_client(makeClient(backend, m_settings.get(), this))
    , m_accounts(std::make_unique<AccountManager>(this))
    , m_auth(std::make_unique<AuthManager>(m_client.get(), this))
    , m_roomList(std::make_unique<RoomListModel>(this))
    , m_timeline(std::make_unique<TimelineModel>(this))
    , m_composer(std::make_unique<MessageComposer>(this))
    , m_notifications(std::make_unique<NotificationManager>(this))
    , m_media(std::make_unique<MediaManager>(this))
    , m_crypto(std::make_unique<CryptoManager>(this))
    , m_spaces(std::make_unique<SpaceManager>(this))
    , m_threads(std::make_unique<ThreadManager>(this))
{
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

    if (m_backend == HttpBackend && m_settings->hasSession()) {
        // Fire-and-forget: whoami callback drives the transition to Main.
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
    return m_backend == MockBackend
        ? QStringLiteral("mock")
        : QStringLiteral("http");
}

SettingsManager *AppController::settings() const { return m_settings.get(); }
AuthManager *AppController::auth() const { return m_auth.get(); }
AccountManager *AppController::accounts() const { return m_accounts.get(); }
RoomListModel *AppController::roomList() const { return m_roomList.get(); }
TimelineModel *AppController::timeline() const { return m_timeline.get(); }
MessageComposer *AppController::composer() const { return m_composer.get(); }
MediaManager *AppController::media() const { return m_media.get(); }

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
