#pragma once

#include "app/ConversationController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoHealthModel.h"
#include "crypto/CryptoManager.h"
#include "media/MediaBridge.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/EmojiCatalog.h"
#include "models/LinkPreviewController.h"
#include "gif/GifSearchController.h"
#include "gif/GifSendController.h"
#include "gif/MatrixGifTransport.h"
#include "models/PaginationController.h"
#include "models/QuickSwitcherModel.h"
#include "models/ReadReceiptCoordinator.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "spaces/SpaceManager.h"
#include "threads/ThreadController.h"
#include "threads/ThreadManager.h"

#include <QObject>
#include <QString>
#include <QSet>
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
    // v0.5.11: platform colour-scheme hint for the "System" theme. Reflects
    // QStyleHints::colorScheme(); QML binds AppTheme.systemDark to it.
    Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)
    Q_PROPERTY(bool localRustResetRequired READ localRustResetRequired
               NOTIFY localRustResetRequiredChanged)
    // v0.7: true while an account switch is in flight. QML disables sending
    // and shows the switching state; the previous account's session is
    // already detached, so nothing can route through it.
    Q_PROPERTY(bool accountSwitching READ accountSwitching
               NOTIFY accountSwitchingChanged)

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
    Q_PROPERTY(QuickSwitcherModel* quickSwitcher READ quickSwitcher CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    Q_PROPERTY(EmojiCatalog* emojiCatalog READ emojiCatalog CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    // v0.6.0 checkpoint 7: read-only E2EE health/readiness (app.cryptoHealth).
    Q_PROPERTY(CryptoHealthModel* cryptoHealth READ cryptoHealth CONSTANT)
    // v0.6.0 checkpoint 9: the account's devices/sessions (server metadata +
    // SDK crypto trust; current session first, then by last-seen).
    Q_PROPERTY(QVariantList sessionDevices READ sessionDevices NOTIFY sessionDevicesChanged)
    // v0.6.0 checkpoint 11: whether the room timeline is on screen, focused,
    // and following the latest message (QML supplies it; notifications use
    // it for active-room suppression).
    Q_PROPERTY(bool activeRoomAtLatest READ activeRoomAtLatest
                   WRITE setActiveRoomAtLatest NOTIFY activeRoomAtLatestChanged)
    Q_PROPERTY(bool sessionDevicesLoading READ sessionDevicesLoading NOTIFY sessionDevicesChanged)
    Q_PROPERTY(bool sessionDevicesFailed READ sessionDevicesFailed NOTIFY sessionDevicesChanged)
    Q_PROPERTY(SpaceManager* spaces READ spaces CONSTANT)
    Q_PROPERTY(ThreadManager* threads READ threads CONSTANT)
    // v0.6.0: the single open SDK-backed thread panel (app.thread).
    Q_PROPERTY(ThreadController* thread READ thread CONSTANT)
    // v0.5.9: conversation creation (DMs, rooms, invites), Room Information
    // (members, permissions, editing, leave) and the media bridge.
    Q_PROPERTY(ConversationController* conversations READ conversations CONSTANT)
    Q_PROPERTY(RoomInfoController* roomInfo READ roomInfo CONSTANT)
    Q_PROPERTY(MediaBridge* mediaBridge READ mediaBridge CONSTANT)
    // v0.5.11: backward-pagination policy and automatic read receipts.
    Q_PROPERTY(PaginationController* pagination READ pagination CONSTANT)
    Q_PROPERTY(ReadReceiptCoordinator* readReceipts READ readReceipts CONSTANT)
    // v0.5.12: safe client-side link-preview backend (Rust HTTPS fetcher).
    Q_PROPERTY(LinkPreviewController* linkPreviews READ linkPreviews CONSTANT)
    // v0.6.1: multi-provider client-side GIF browser (app.gif) + send pipeline.
    Q_PROPERTY(GifSearchController* gif READ gif CONSTANT)
    Q_PROPERTY(GifSendController* gifSend READ gifSend CONSTANT)
    // v0.5.19: device-aware timeline wheel-scroll policy.
    Q_PROPERTY(TimelineScrollController* timelineScroll READ timelineScroll CONSTANT)
    // v0.6.0 checkpoint 6: the thread panel's OWN wheel motion engine.
    // Same policy/speed as the room timeline, but fully isolated motion
    // state — the two panels can never share an active contentY target.
    Q_PROPERTY(TimelineScrollController* threadScroll READ threadScroll CONSTANT)

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
    bool systemDarkMode() const;
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;
    bool localRustResetRequired() const { return m_localRustResetRequired; }
    bool accountSwitching() const { return m_accountSwitching; }

    SettingsManager *settings() const;
    AuthManager *auth() const;
    AccountManager *accounts() const;
    RoomListModel *roomList() const;
    QuickSwitcherModel *quickSwitcher() const;
    TimelineModel *timeline() const;
    MessageComposer *composer() const;
    EmojiCatalog *emojiCatalog() const { return m_emojiCatalog.get(); }
    MediaManager *media() const;
    CryptoManager *crypto() const;
    CryptoHealthModel *cryptoHealth() const { return m_cryptoHealth.get(); }
    // Bounded UI refresh (Settings "Refresh" and post-operation updates).
    Q_INVOKABLE void refreshCryptoHealth();
    QVariantList sessionDevices() const { return m_sessionDevices; }
    bool sessionDevicesLoading() const { return m_sessionDevicesLoading; }
    bool sessionDevicesFailed() const { return m_sessionDevicesFailed; }
    Q_INVOKABLE void refreshSessionDevices();
    bool activeRoomAtLatest() const { return m_activeRoomAtLatest; }
    void setActiveRoomAtLatest(bool atLatest);
    SpaceManager *spaces() const;
    ThreadManager *threads() const;
    ThreadController *thread() const { return m_thread.get(); }
    ConversationController *conversations() const { return m_conversations.get(); }
    RoomInfoController *roomInfo() const { return m_roomInfo.get(); }
    MediaBridge *mediaBridge() const { return m_mediaBridge.get(); }
    PaginationController *pagination() const { return m_pagination.get(); }
    ReadReceiptCoordinator *readReceipts() const { return m_readReceipts.get(); }
    LinkPreviewController *linkPreviews() const { return m_linkPreviews.get(); }
    GifSearchController *gif() const { return m_gif.get(); }
    GifSendController *gifSend() const { return m_gifSend.get(); }
    TimelineScrollController *timelineScroll() const { return m_timelineScroll.get(); }
    TimelineScrollController *threadScroll() const { return m_threadScroll.get(); }
    SecretStore *secretStore() const { return m_secretStore.get(); }

public Q_SLOTS:
    void setCurrentRoomId(const QString &roomId);
    void showLogin();
    void showMain();
    void showSettings();
    void openRoom(const QString &roomId);

    // v0.7 multi-account. Switch the whole Matrix context (client session,
    // stores, crypto, models, notifications) to another saved account
    // without a login form. The previous account stays signed in — its
    // session, store, and token are untouched; only its local runtime is
    // detached. No-op when already switching or the target is unusable.
    Q_INVOKABLE void switchToAccount(const QString &userId);

    // v0.7. Fully remove one saved account from this device: if it is the
    // active account this performs a real (server) logout, otherwise it
    // deletes the account's local store, token, and record without touching
    // the active session. Other accounts are never affected.
    Q_INVOKABLE void removeAccount(const QString &userId);

    // v0.5.9: open Settings on a specific category (account menu entries
    // "Settings" vs "Security & Recovery"). The section name is consumed
    // once by SettingsScreen on load.
    Q_INVOKABLE void showSettingsSection(const QString &section);
    Q_INVOKABLE QString takeRequestedSettingsSection();

    // v0.7: apply the active theme to the QGuiApplication palette. Fusion
    // paints ComboBox popups, Menus, ToolTips, ScrollBars and Dialogs from
    // the *application* palette — an ApplicationWindow item palette does not
    // reach popups, so a dark theme otherwise left them the default light
    // colour. Main.qml calls this with the resolved AppTheme tokens whenever
    // the theme changes; the map is keyed by palette role name ("window",
    // "windowText", "base", "text", "button", "buttonText", "highlight",
    // "highlightedText", "toolTipBase", "toolTipText", "placeholderText",
    // "light", "midlight", "mid", "dark", "brightText", "link", and the
    // disabled-prefixed "disabledText"/"disabledButtonText"/…).
    Q_INVOKABLE void applyControlPalette(const QVariantMap &roles);

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
    void accountSwitchingChanged();
    void currentRoomIdChanged();
    void loggedInChanged();
    void connectionStatusChanged();
    void syncModeChanged();
    void systemDarkModeChanged();
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
    void sessionDevicesChanged();
    void activeRoomAtLatestChanged();
    // v0.6.0 checkpoint 11: a notification was clicked — QML raises the
    // window, selects the room, opens the thread, and locates the event.
    // Identity only, never tokens.
    void notificationOpenRequested(const QString &roomId,
                                   const QString &eventId,
                                   const QString &threadRootId);

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
    void setAccountSwitching(bool switching);
    // Failure path of switchToAccount: falls back to the previous account
    // once; if that is impossible, lands on the login screen.
    void failAccountSwitch(const QString &message);
    // Clears every cache that must not leak across accounts (media bytes,
    // pending notifications, invite memory, verification/security state,
    // session devices, room-list profile lookups). Used on account change;
    // a real logout clears the same state through loggedOut connections.
    void clearCrossAccountCaches();

    static std::unique_ptr<MatrixClient> makeClient(Backend backend,
                                                    SettingsManager *settings,
                                                    QObject *parent);

    Backend m_backend;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    QString m_requestedSettingsSection;
    QString m_connectionStatus;
    bool m_localRustResetRequired = false;
    bool m_resetResultPending = false;
    // v0.7 account switching.
    bool m_accountSwitching = false;
    // The account to fall back to if activating the switch target fails.
    // Consumed by the loginFailed handler; empty = no fallback pending.
    QString m_switchFallbackUserId;
    // The account whose session most recently succeeded — used to detect a
    // cross-account transition in onLoginSucceeded.
    QString m_lastSessionUserId;
    // v0.7 add-account mode: the account to return to when an add-account
    // login fails or the user presses Back. Entering the login screen while
    // a session is active sets it; success with a new account clears it.
    QString m_addAccountReturnTo;
    // True while the previous account is being restored in the background
    // after a failed add-account attempt: the restore's loginSucceeded must
    // not yank the user off the login screen.
    bool m_backgroundRestore = false;

    // Order matters: SecretStore is constructed first so SettingsManager can
    // be wired to it before any code touches accessToken() / hasSession().
    std::unique_ptr<SecretStore> m_secretStore;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<MatrixClient> m_client;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<RoomListModel> m_roomList;
    std::unique_ptr<QuickSwitcherModel> m_quickSwitcher;
    std::unique_ptr<TimelineModel> m_timeline;
    std::unique_ptr<MessageComposer> m_composer;
    std::unique_ptr<EmojiCatalog> m_emojiCatalog;
    std::unique_ptr<NotificationManager> m_notifications;
    std::unique_ptr<MediaManager> m_media;
    std::unique_ptr<CryptoManager> m_crypto;
    std::unique_ptr<CryptoHealthModel> m_cryptoHealth;
    // The CryptoHealthModel generation captured at the moment a crypto-health
    // query is DISPATCHED. Comparing this (not the model's live generation)
    // against the model epoch when the async answer arrives lets a logout /
    // account switch that happened in between correctly reject the stale
    // answer — the 0.6.0 code passed the model's own current generation, so
    // the guard could never reject anything.
    quint64 m_cryptoQueryGeneration = 1;
    QVariantList m_sessionDevices;
    bool m_sessionDevicesLoading = false;
    bool m_sessionDevicesFailed = false;
    bool m_activeRoomAtLatest = false;
    QSet<QString> m_knownInvites;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
    std::unique_ptr<ThreadController> m_thread;
    std::unique_ptr<ConversationController> m_conversations;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<MediaBridge> m_mediaBridge;
    std::unique_ptr<PaginationController> m_pagination;
    std::unique_ptr<ReadReceiptCoordinator> m_readReceipts;
    std::unique_ptr<LinkPreviewController> m_linkPreviews;
    std::unique_ptr<MatrixGifTransport> m_gifTransport;
    std::unique_ptr<GifSearchController> m_gif;
    std::unique_ptr<GifSendController> m_gifSend;
    std::unique_ptr<TimelineScrollController> m_timelineScroll;
    std::unique_ptr<TimelineScrollController> m_threadScroll;

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
