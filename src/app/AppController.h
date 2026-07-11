#pragma once

#include "app/ConversationController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#include "media/MediaBridge.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "spaces/SpaceManager.h"
#include "threads/ThreadManager.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
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
    Q_PROPERTY(QString syncModeLabel READ syncModeLabel NOTIFY syncModeChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)
    Q_PROPERTY(bool localRustResetRequired READ localRustResetRequired
               NOTIFY localRustResetRequiredChanged)

    // v0.5.0-prep+10: redacted Rust SDK device id (e.g. "GAOT...GBSK")
    // so Settings can show which Lightning session is running without
    // exposing the full id. Empty when the backend is not Rust or the
    // client has not yet logged in.
    Q_PROPERTY(QString rustDeviceIdRedacted READ rustDeviceIdRedacted NOTIFY rustDeviceIdChanged)

    // v0.5.6 Security & Recovery. Aggregate cross-signing/verification
    // state cached from the SDK; QML must not compute this. Distinct
    // from generic own-device trust because Matrix SDK reports it
    // separately.
    Q_PROPERTY(QString sessionTrustState READ sessionTrustState NOTIFY securityStateChanged)
    Q_PROPERTY(QString sessionDeviceId READ sessionDeviceId NOTIFY securityStateChanged)
    Q_PROPERTY(bool ownIdentityAvailable READ ownIdentityAvailable NOTIFY securityStateChanged)
    Q_PROPERTY(bool crossSigningAvailable READ crossSigningAvailable NOTIFY securityStateChanged)

    // v0.5.6 Encrypted room-key import.
    Q_PROPERTY(QString roomKeyImportState READ roomKeyImportState NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(int roomKeyImportImportedCount READ roomKeyImportImportedCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(int roomKeyImportTotalCount READ roomKeyImportTotalCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(int roomKeyImportAffectedRoomCount READ roomKeyImportAffectedRoomCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(QString roomKeyImportLastMessage READ roomKeyImportLastMessage NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(bool roomKeyImportRunning READ roomKeyImportRunning NOTIFY roomKeyImportStateChanged)

    // v0.5.0 SAS emoji verification. QML binds all of these.
    Q_PROPERTY(bool verificationActive READ verificationActive NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationFlowId READ verificationFlowId NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherUser READ verificationOtherUser NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherDevice READ verificationOtherDevice NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationIsSelfVerification READ verificationIsSelfVerification NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationState READ verificationState NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationEmojis READ verificationEmojis NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationDecimals READ verificationDecimals NOTIFY verificationStateChanged)

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
    // v0.5.9: conversation creation (DMs, rooms, invites), Room Information
    // (members, permissions, editing, leave) and the media bridge.
    Q_PROPERTY(ConversationController* conversations READ conversations CONSTANT)
    Q_PROPERTY(RoomInfoController* roomInfo READ roomInfo CONSTANT)
    Q_PROPERTY(MediaBridge* mediaBridge READ mediaBridge CONSTANT)

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
    QString syncModeLabel() const;
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;
    bool localRustResetRequired() const { return m_localRustResetRequired; }

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
    ConversationController *conversations() const { return m_conversations.get(); }
    RoomInfoController *roomInfo() const { return m_roomInfo.get(); }
    MediaBridge *mediaBridge() const { return m_mediaBridge.get(); }
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

    // v0.5.0-prep+11. Delete only this app's local Rust SDK store
    // for the currently-configured homeserver + user slug. Does not
    // touch other accounts, other backends, or server-side data.
    // Emits localRustStoreResetResult(ok, message). No-op on non-Rust
    // backends.
    Q_INVOKABLE void resetLocalRustStore();

    // Login-screen reset. Account identity is derived canonically in C++ from
    // the current form values; QML never computes paths or deletes files.
    Q_INVOKABLE void resetLocalRustSession(const QString &homeserver,
                                           const QString &user);

    // v0.5.0-prep+11. Manually reload the current room's recent
    // timeline via matrix-sdk's Room::messages. Safe to call at any
    // time — the wrapper dedupes by event_id. No-op on non-Rust.
    Q_INVOKABLE void reloadCurrentRoomTimeline(int limit = 30);

    // v0.5.0 SAS emoji verification invocables.
    Q_INVOKABLE void acceptVerification();
    Q_INVOKABLE void confirmVerification();
    Q_INVOKABLE void mismatchVerification();
    Q_INVOKABLE void cancelVerification();

    // v0.5.6. Initiate SAS verification of this Lightning session
    // against another session belonging to the same Matrix account.
    Q_INVOKABLE void startOwnVerification();

    // v0.5.6. Re-query the SDK trust state so the Settings pane can
    // refresh after a manual action.
    Q_INVOKABLE void refreshSessionTrustState();

    // v0.5.6. Kick off encrypted Megolm room-key import from a local
    // file. The passphrase is passed straight to Rust and never stored
    // in QML properties, C++ members, QSettings, or logs.
    Q_INVOKABLE void importRoomKeys(const QUrl &fileUrl, const QString &passphrase);

    bool verificationActive() const { return !m_verificationFlowId.isEmpty(); }
    QString verificationFlowId() const { return m_verificationFlowId; }
    QString verificationOtherUser() const { return m_verificationOtherUser; }
    QString verificationOtherDevice() const { return m_verificationOtherDevice; }
    bool verificationIsSelfVerification() const { return m_verificationIsSelf; }
    QString verificationState() const { return m_verificationState; }
    QVariantList verificationEmojis() const { return m_verificationEmojis; }
    QVariantList verificationDecimals() const { return m_verificationDecimals; }

    // v0.5.6 Security & Recovery accessors.
    QString sessionTrustState() const { return m_sessionTrustState; }
    QString sessionDeviceId() const { return m_sessionDeviceId; }
    bool ownIdentityAvailable() const { return m_ownIdentityAvailable; }
    bool crossSigningAvailable() const { return m_crossSigningAvailable; }

    QString roomKeyImportState() const { return m_roomKeyImportState; }
    int roomKeyImportImportedCount() const { return m_roomKeyImportImported; }
    int roomKeyImportTotalCount() const { return m_roomKeyImportTotal; }
    int roomKeyImportAffectedRoomCount() const { return m_roomKeyImportAffected; }
    QString roomKeyImportLastMessage() const { return m_roomKeyImportMessage; }
    bool roomKeyImportRunning() const { return m_roomKeyImportRunning; }

Q_SIGNALS:
    void currentScreenChanged();
    void initialSyncDoneChanged();
    void currentRoomIdChanged();
    void loggedInChanged();
    void connectionStatusChanged();
    void syncModeChanged();
    void errorReported(const QString &message);
    void rustDeviceIdChanged();
    void localRustResetRequiredChanged();
    // v0.5.0-prep+10. Fires once per requestRecoverFromBackup call.
    // `state`: "attempted" / "ok" / "failed". `message`: non-secret
    // detail for failures, empty on success. Never contains the
    // recovery key or imported key material.
    void recoveryStateChanged(const QString &state, const QString &message);

    // v0.5.0-prep+11. Emitted when login/restore failed because the
    // Rust SDK store on disk belongs to a different Matrix device.
    // QML LoginScreen shows a "Reset local Lightning session" button
    // in response.
    void storeDeviceMismatchDetected(const QString &displayMessage);

    // v0.5.0-prep+11. Emitted after resetLocalRustStore() finishes.
    void localRustStoreResetResult(bool ok, const QString &message);

    // v0.5.0-prep+11. Fires after reloadCurrentRoomTimeline completes.
    void currentRoomTimelineReloaded(int totalEvents,
                                     int decryptedEvents,
                                     int undecryptableEvents);

    // v0.5.0 SAS verification.
    void verificationStateChanged();

    // v0.5.6 Security & Recovery.
    void securityStateChanged();
    void roomKeyImportStateChanged();
    // Emitted after a successful room-key import completes, with the
    // aggregate counts the UI should display. Non-secret.
    void roomKeyImportCompleted(int imported, int total, int affectedRooms);

private:
    void setCurrentScreen(Screen s);
    void setConnectionStatus(const QString &s);
    void onLoginSucceeded();
    void onLoggedOut();
    void setLocalRustResetRequired(bool required);

    static std::unique_ptr<MatrixClient> makeClient(Backend backend,
                                                    SettingsManager *settings,
                                                    QObject *parent);

    Backend m_backend;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    QString m_connectionStatus;
    bool m_localRustResetRequired = false;
    bool m_resetResultPending = false;

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
    std::unique_ptr<ConversationController> m_conversations;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<MediaBridge> m_mediaBridge;

    // v0.5.0 SAS verification state cache.
    QString m_verificationFlowId;
    QString m_verificationOtherUser;
    QString m_verificationOtherDevice;
    bool    m_verificationIsSelf = false;
    QString m_verificationState;
    QVariantList m_verificationEmojis;
    QVariantList m_verificationDecimals;

    // v0.5.6 Security & Recovery cache.
    QString m_sessionTrustState = QStringLiteral("Unknown");
    QString m_sessionDeviceId;
    bool    m_ownIdentityAvailable = false;
    bool    m_crossSigningAvailable = false;

    // v0.5.6 Room-key import cache. State values:
    //   "" (idle), "importing", "done", "failed".
    QString m_roomKeyImportState;
    int     m_roomKeyImportImported = 0;
    int     m_roomKeyImportTotal = 0;
    int     m_roomKeyImportAffected = 0;
    QString m_roomKeyImportMessage;
    bool    m_roomKeyImportRunning = false;
    QStringList m_roomKeyImportAffectedRoomIds;
};
