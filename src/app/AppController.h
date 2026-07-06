#pragma once

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "spaces/SpaceManager.h"
#include "threads/ThreadManager.h"

#include <QObject>
#include <QString>
#include <memory>

class MatrixClient;
class NotificationManager;
class SecretStore;

class AppController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(Screen currentScreen READ currentScreen NOTIFY currentScreenChanged)
    Q_PROPERTY(QString currentRoomId READ currentRoomId WRITE setCurrentRoomId NOTIFY currentRoomIdChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)

    // v0.5.0-prep+10: redacted Rust SDK device id (e.g. "GAOT...GBSK")
    // so Settings can show which Lightning session is running without
    // exposing the full id. Empty when the backend is not Rust or the
    // client has not yet logged in.
    Q_PROPERTY(QString rustDeviceIdRedacted READ rustDeviceIdRedacted NOTIFY rustDeviceIdChanged)

    Q_PROPERTY(SettingsManager* settings READ settings CONSTANT)
    Q_PROPERTY(AuthManager* auth READ auth CONSTANT)
    Q_PROPERTY(AccountManager* accounts READ accounts CONSTANT)
    Q_PROPERTY(RoomListModel* roomList READ roomList CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    Q_PROPERTY(SpaceManager* spaces READ spaces CONSTANT)
    Q_PROPERTY(ThreadManager* threads READ threads CONSTANT)

public:
    enum Screen {
        LoginScreen = 0,
        MainScreen = 1,
        SettingsScreen = 2,
    };
    Q_ENUM(Screen)

    enum Backend {
        HttpBackend = 0,
        MockBackend = 1,
        RustBackend = 2, // implemented iff ENABLE_RUST_SDK_BACKEND is defined
    };
    Q_ENUM(Backend)

    // True if this build actually contains an implementation for the given
    // backend. main.cpp uses this to reject --backend=rust cleanly when the
    // Rust backend was not compiled in.
    static bool isBackendCompiled(Backend backend);

    explicit AppController(Backend backend = HttpBackend, QObject *parent = nullptr);
    ~AppController() override;

    Screen currentScreen() const { return m_currentScreen; }
    QString currentRoomId() const { return m_currentRoomId; }
    bool loggedIn() const;
    QString appVersion() const { return QStringLiteral(APP_VERSION); }
    QString backendName() const;
    QString connectionStatus() const { return m_connectionStatus; }
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;

    SettingsManager *settings() const;
    AuthManager *auth() const;
    AccountManager *accounts() const;
    RoomListModel *roomList() const;
    TimelineModel *timeline() const;
    MessageComposer *composer() const;
    MediaManager *media() const;
    CryptoManager *crypto() const;
    SpaceManager *spaces() const;
    ThreadManager *threads() const;
    SecretStore *secretStore() const { return m_secretStore.get(); }

public Q_SLOTS:
    void setCurrentRoomId(const QString &roomId);
    void showLogin();
    void showMain();
    void showSettings();
    void openRoom(const QString &roomId);

    // v0.5.0-prep+10: GUI recovery-key restore. The QML Settings panel
    // calls this from a password-style TextField and never keeps the
    // key in a QML property beyond the invocation. The recovery key is
    // routed straight into the RustSdkMatrixClient wrapper (which sends
    // it to Rust via mx_rust_recover_from_backup) and is never logged.
    // No-op on non-Rust backends. Results arrive via
    // recoveryStateChanged().
    Q_INVOKABLE void requestRecoverFromBackup(const QString &recoveryKey);

Q_SIGNALS:
    void currentScreenChanged();
    void initialSyncDoneChanged();
    void currentRoomIdChanged();
    void loggedInChanged();
    void connectionStatusChanged();
    void errorReported(const QString &message);
    void rustDeviceIdChanged();
    // v0.5.0-prep+10. Fires once per requestRecoverFromBackup call.
    // `state`: "attempted" / "ok" / "failed". `message`: non-secret
    // detail for failures, empty on success. Never contains the
    // recovery key or imported key material.
    void recoveryStateChanged(const QString &state, const QString &message);

private:
    void setCurrentScreen(Screen s);
    void setConnectionStatus(const QString &s);
    void onLoginSucceeded();
    void onLoggedOut();

    static std::unique_ptr<MatrixClient> makeClient(Backend backend,
                                                    SettingsManager *settings,
                                                    QObject *parent);

    Backend m_backend;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    QString m_connectionStatus;

    // Order matters: SecretStore is constructed first so SettingsManager can
    // be wired to it before any code touches accessToken() / hasSession().
    std::unique_ptr<SecretStore> m_secretStore;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<MatrixClient> m_client;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<RoomListModel> m_roomList;
    std::unique_ptr<TimelineModel> m_timeline;
    std::unique_ptr<MessageComposer> m_composer;
    std::unique_ptr<NotificationManager> m_notifications;
    std::unique_ptr<MediaManager> m_media;
    std::unique_ptr<CryptoManager> m_crypto;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
};
