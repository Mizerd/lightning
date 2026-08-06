#pragma once

#include "app/ConversationController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoBootstrapModel.h"
#include "crypto/CryptoHealthModel.h"
#include "crypto/CryptoManager.h"
#include "crypto/QrImageProvider.h"
#include "media/MediaBridge.h"
#include "media/MediaPlaybackController.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/MentionSuggestionModel.h"
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
    // Development-only screenshot/demo mode indicator (see beginScreenshotDemo).
    // Always present so QML bindings resolve in every build; only ever true in a
    // build compiled with LIGHTNING_ENABLE_SCREENSHOT_DEMO and launched with
    // --screenshot-demo.
    Q_PROPERTY(bool screenshotDemoActive READ screenshotDemoActive CONSTANT)
    // Development-only demo-session controller (see demoController()). Always
    // present so QML bindings resolve; null in every non-demo build.
    Q_PROPERTY(QObject* demo READ demoController CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    // True when the active backend synchronizes per-room notification modes
    // with the account's server push rules (Rust SDK backend). Fixed per
    // backend for the whole process lifetime, like backendName. The QML
    // notification pickers phrase their disclaimer from this.
    Q_PROPERTY(bool serverRoomNotificationModes READ serverRoomNotificationModes
               CONSTANT)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncModeLabel READ syncModeLabel NOTIFY syncModeChanged)
    // v0.5.11: platform colour-scheme hint for the "System" theme. Reflects
    // QStyleHints::colorScheme(); QML binds AppTheme.systemDark to it.
    Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)
    Q_PROPERTY(bool localRustResetRequired READ localRustResetRequired
               NOTIFY localRustResetRequiredChanged)
    // Machine-readable classification of the local-session failure that is
    // currently blocking sign-in, plus the account it actually applies to.
    // The reason code is the backend's own diagnostic token (see
    // matrix::rust_session::diagnosticName); an empty code means "no
    // failure". C++ deliberately does NOT author user-facing prose for these
    // states — QML owns the copy, and a single generic sentence for five
    // distinct causes is exactly what made the old login dead end unusable.
    //
    // The identity is captured at DETECTION time, not read from settings on
    // demand: during an add-account attempt the settings' active account is
    // still the previously signed-in one, so a settings-derived repair would
    // target the wrong account.
    Q_PROPERTY(QString localSessionFailureReasonCode
               READ localSessionFailureReasonCode
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureUserId
               READ localSessionFailureUserId
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureHomeserver
               READ localSessionFailureHomeserver
               NOTIFY localSessionFailureChanged)
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

    // Show-QR verification, orthogonal to verificationState on purpose:
    // the QR leg is an alternative presentation of the SAME flow, so the
    // existing state machine is untouched and the card simply prefers the
    // QR panel while one is available.
    Q_PROPERTY(bool verificationQrAvailable READ verificationQrAvailable NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationQrImage READ verificationQrImage NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrScanned READ verificationQrScanned NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrConfirming READ verificationQrConfirming NOTIFY verificationStateChanged)

    Q_PROPERTY(SettingsManager* settings READ settings CONSTANT)
    Q_PROPERTY(AuthManager* auth READ auth CONSTANT)
    Q_PROPERTY(AccountManager* accounts READ accounts CONSTANT)
    Q_PROPERTY(RoomListModel* roomList READ roomList CONSTANT)
    Q_PROPERTY(QuickSwitcherModel* quickSwitcher READ quickSwitcher CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    // v0.7 outgoing @-mentions: the current-room member suggestion model
    // shared by the room and thread composer mention popups.
    Q_PROPERTY(MentionSuggestionModel* mentionSuggestions READ mentionSuggestions
                   CONSTANT)
    Q_PROPERTY(EmojiCatalog* emojiCatalog READ emojiCatalog CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    // v0.6.0 checkpoint 7: read-only E2EE health/readiness (app.cryptoHealth).
    Q_PROPERTY(CryptoHealthModel* cryptoHealth READ cryptoHealth CONSTANT)
    // v0.7: verified-session key-bootstrap status (app.cryptoBootstrap).
    Q_PROPERTY(CryptoBootstrapModel* cryptoBootstrap READ cryptoBootstrap CONSTANT)
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
    // v0.7: shared inline-playback coordinator (one audible media card at a
    // time; stopped on room/account switches and sign-out).
    Q_PROPERTY(MediaPlaybackController* playback READ playback CONSTANT)
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
        // v0.7: explicit session-restoration state so an authenticated
        // launch never instantiates (or flashes) the login form while the
        // saved session restores. Appended so 0/1/2 stay stable for the
        // integer-based QML routing.
        BootScreen = 3,
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

    // `screenshotDemo` is only ever true in a build compiled with
    // LIGHTNING_ENABLE_SCREENSHOT_DEMO and launched with --screenshot-demo (main
    // forces the mock backend for it). It is injected here — not discovered
    // later — so the constructor can (a) build an in-memory SecretStore instead
    // of probing the production libsecret/keychain store, and (b) skip the
    // normal startup session-restore and let beginScreenshotDemo drive it.
    explicit AppController(Backend backend = HttpBackend,
                           bool screenshotDemo = false,
                           QObject *parent = nullptr);
    ~AppController() override;

    // Quiesce background work (media playback, sync) while the window and the
    // platform event dispatcher are still valid, so no worker posts an event
    // after teardown begins. Wired to QGuiApplication::aboutToQuit; idempotent.
    // Addresses the Windows "QEventDispatcherWin32: Failed to post a message
    // (Invalid window handle.)" seen on close. Safe to call with no GUI.
    void prepareForShutdown();
    bool isShuttingDown() const { return m_shuttingDown; }

    Screen currentScreen() const { return m_currentScreen; }
    QString currentRoomId() const { return m_currentRoomId; }
    bool loggedIn() const;
    QString appVersion() const { return QStringLiteral(APP_VERSION); }
    bool screenshotDemoActive() const { return m_screenshotDemoActive; }
    // Development-only: enter screenshot/demo mode. Enriches the mock scene,
    // marks screenshotDemoActive, and auto-logs-in the deterministic demo
    // account so the app opens directly into the real chat UI. A no-op unless
    // the active backend is the in-memory mock (guaranteed by preflight, which
    // forces --screenshot-demo to the mock backend). Never networks.
    void beginScreenshotDemo(const QString &initialAccount = QString());
    // Development-only: forward launcher/CLI demo options (scenario, theme,
    // appearance, size, hide-controls) to the demo controller. A no-op unless
    // the screenshot demo is active. Called by main() after beginScreenshotDemo.
    void applyDemoLaunchOptions(const QString &scenario, const QString &theme,
                                const QString &appearance, const QString &size,
                                bool hideControls);
    // Development-only: the demo-session controller (scenarios, control panel,
    // window presets, per-account selected-room memory). Null unless the
    // screenshot demo is active. Exposed to QML as `app.demo`.
    QObject *demoController() const { return m_demoController; }
    QString backendName() const;
    bool serverRoomNotificationModes() const;
    QString connectionStatus() const { return m_connectionStatus; }
    QString syncModeLabel() const;
    bool systemDarkMode() const;
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;
    bool localRustResetRequired() const { return m_localRustResetRequired; }
    // Public so a C++ test can drive the classified-failure state directly on
    // the controller. The real emitter is a Rust-backend rejection, which a
    // MockBackend test cannot produce; exposing a QML-invokable debug hook
    // instead would put a state-injection seam into the shipped UI surface.
    void setLocalSessionFailure(const QString &reasonCode,
                                const QString &userId,
                                const QString &homeserver);
    // Explicit, because only the caller knows whether a failure was actually
    // resolved. Login success and a completed repair clear it; merely
    // disarming the destructive action does not.
    void clearLocalSessionFailure()
    { setLocalSessionFailure(QString{}, QString{}, QString{}); }
    QString localSessionFailureReasonCode() const
    { return m_localSessionFailureReason; }
    QString localSessionFailureUserId() const
    { return m_localSessionFailureUserId; }
    QString localSessionFailureHomeserver() const
    { return m_localSessionFailureHomeserver; }
    bool accountSwitching() const { return m_accountSwitching; }

    SettingsManager *settings() const;
    AuthManager *auth() const;
    AccountManager *accounts() const;
    RoomListModel *roomList() const;
    QuickSwitcherModel *quickSwitcher() const;
    TimelineModel *timeline() const;
    MessageComposer *composer() const;
    MentionSuggestionModel *mentionSuggestions() const
    { return m_mentionSuggestions.get(); }
    EmojiCatalog *emojiCatalog() const { return m_emojiCatalog.get(); }
    MediaManager *media() const;
    CryptoManager *crypto() const;
    CryptoHealthModel *cryptoHealth() const { return m_cryptoHealth.get(); }
    CryptoBootstrapModel *cryptoBootstrap() const
    { return m_cryptoBootstrap.get(); }
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
    MediaPlaybackController *playback() const { return m_playback.get(); }
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

    // Per-room notification mode (0 = all, 1 = mentions & keywords,
    // 2 = mute) — the single UI entry point. Always writes the
    // device-local SettingsManager value first (NotificationManager reads
    // it, so policy works instantly and offline); on a backend with
    // server push-rule support it then issues the SDK write. The async
    // roomNotificationModeChanged report reconciles the cache only when
    // it is USER-DEFINED (a resolved account default is never persisted),
    // and while the room carries kept-on-this-device failure state only a
    // report EQUAL to the cached value — the real write acknowledgement —
    // is accepted. Note the deliberate semantic shift on such backends:
    // mode 0 used to merely remove the local key — it now ALSO sets an
    // explicit server "all messages" rule (label-faithful mapping; a
    // separate "follow account default" choice is an accepted follow-up).
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    // Poll-on-open refresh: re-query the server rule when a notification
    // picker opens so changes made in another client land in the cache.
    // No-op on backends without server support.
    Q_INVOKABLE void requestRoomNotificationMode(const QString &roomId);
    // True while the room's LAST server push-rule write is known to have
    // failed (the device-local mode still applies). Cleared by the next
    // successful user-defined report for the room; session-scoped, never
    // persisted. Automatic retry on reconnect is an accepted follow-up.
    Q_INVOKABLE bool roomNotificationModeSyncFailed(const QString &roomId) const;

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
    //
    // Superseded by repairLocalSession() for the login screen and kept only
    // for callers that genuinely have an explicit identity in hand. It must
    // never be driven from raw form text again: the login form has no user
    // field prefill, so a startup restore failure passed empty strings here
    // and the repair could not run at all without the user retyping their
    // Matrix ID from memory.
    Q_INVOKABLE void resetLocalRustSession(const QString &homeserver,
                                           const QString &user);

    // Repair the local session for the account that ACTUALLY failed. Uses the
    // identity captured when the failure was detected, falling back to the
    // active account only when no failure is in flight (the Settings danger
    // zone case). Takes no arguments precisely so no caller can point it at
    // the wrong account.
    Q_INVOKABLE void repairLocalSession();

    // Whether clearing this device's local data can actually repair the given
    // failure. QML binds the destructive action's visibility to THIS rather
    // than to a per-reason list maintained by hand, so a card can never offer
    // a button that repairLocalSession() will refuse. Unknown or empty codes
    // answer false — the safe direction.
    Q_INVOKABLE bool localResetHelpsFor(const QString &reasonCode) const;

    // Sanitized, user-invoked support bundle for the clipboard. Contains
    // versions, capability flags, session lifecycle state and error
    // categories only — never tokens, keys, recovery material, message
    // bodies, room identifiers, or filesystem paths.
    Q_INVOKABLE QString sessionDiagnosticsText() const;
    Q_INVOKABLE void copySessionDiagnostics();

    // v0.5.0-prep+11. Manually reload the current room's recent
    // timeline via matrix-sdk's Room::messages. Safe to call at any
    // time — the wrapper dedupes by event_id. No-op on non-Rust.
    Q_INVOKABLE void reloadCurrentRoomTimeline(int limit = 30);

    // v0.5.0 SAS emoji verification invocables.
    Q_INVOKABLE void acceptVerification();
    Q_INVOKABLE void confirmVerification();
    Q_INVOKABLE void mismatchVerification();
    Q_INVOKABLE void cancelVerification();

    // The user confirmed that the OTHER device reported a successful scan
    // of the displayed QR code. Only meaningful once the SDK has reported
    // the scan; the SDK alone performs the trust change.
    Q_INVOKABLE void confirmQrVerification();

    // v0.5.6. Initiate SAS verification of this Lightning session
    // against another session belonging to the same Matrix account.
    Q_INVOKABLE void startOwnVerification();

    // v0.7.2. "Request keys again": ask the Rust recovery coordinator for
    // a fresh standards-based encryption-secret request round and re-arm
    // the bootstrap model's bounded wait. Only meaningful on the Rust
    // backend while signed in.
    Q_INVOKABLE void requestEncryptionKeys();

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
    bool verificationQrAvailable() const { return !m_verificationQrToken.isEmpty(); }
    QString verificationQrImage() const
    {
        // The token is the whole URL identity: opaque, per-code, and never
        // derived from the flow id, so no flow id ever reaches a URL. A
        // fresh token per code also busts QML's image cache.
        return m_verificationQrToken.isEmpty()
            ? QString{}
            : QStringLiteral("image://lightning-qr/") + m_verificationQrToken;
    }
    bool verificationQrScanned() const { return m_verificationQrScanned; }
    bool verificationQrConfirming() const { return m_verificationQrConfirming; }

    // Owned here so it outlives the QML engine that holds the provider.
    QrCodeStore *qrCodeStore() { return &m_qrCodeStore; }

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
    void localSessionFailureChanged();
    // v0.5.0-prep+10. Fires once per requestRecoverFromBackup call.
    // `state`: "attempted" / "ok" / "failed". `message`: non-secret
    // detail for failures, empty on success. Never contains the
    // recovery key or imported key material.
    void recoveryStateChanged(const QString &state, const QString &message);

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

    // The room's server notification-mode sync state flipped (a push-rule
    // write failed, or a later server report cleared the failure). The
    // pickers re-query roomNotificationModeSyncFailed() on this.
    void roomNotificationModeSyncStateChanged(const QString &roomId);

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
    // Injected at construction (never discovered later): true only in a
    // LIGHTNING_ENABLE_SCREENSHOT_DEMO build launched with --screenshot-demo.
    // Gates the in-memory SecretStore and the skipped startup restore.
    bool m_screenshotDemo = false;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    // Rooms whose member roster was hydrated this session (one bounded
    // requestRoomMembers per room per account; cleared on logout/switch).
    QSet<QString> m_memberHydratedRooms;
    QString m_requestedSettingsSection;
    QString m_connectionStatus;
    bool m_localRustResetRequired = false;
    QString m_localSessionFailureReason;
    QString m_localSessionFailureUserId;
    QString m_localSessionFailureHomeserver;
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
    bool m_shuttingDown = false;
    // Development-only screenshot/demo mode (never true in a release build; the
    // compile option that enables beginScreenshotDemo cannot coexist with a
    // Rust-only release).
    bool m_screenshotDemoActive = false;
    // Development-only demo-session controller (scenarios/panel/window presets).
    // Parented to this AppController; null in non-demo builds. Owned as a raw
    // QObject* so the concrete ScreenshotDemoController type stays behind the
    // LIGHTNING_ENABLE_SCREENSHOT_DEMO compile guard and out of this header.
    QObject *m_demoController = nullptr;
    std::unique_ptr<MatrixClient> m_client;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<RoomListModel> m_roomList;
    std::unique_ptr<QuickSwitcherModel> m_quickSwitcher;
    std::unique_ptr<TimelineModel> m_timeline;
    std::unique_ptr<MessageComposer> m_composer;
    std::unique_ptr<MentionSuggestionModel> m_mentionSuggestions;
    std::unique_ptr<EmojiCatalog> m_emojiCatalog;
    std::unique_ptr<NotificationManager> m_notifications;
    std::unique_ptr<MediaManager> m_media;
    std::unique_ptr<CryptoManager> m_crypto;
    std::unique_ptr<CryptoHealthModel> m_cryptoHealth;
    std::unique_ptr<CryptoBootstrapModel> m_cryptoBootstrap;
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
    // Rooms whose last server push-rule write failed, so the pickers can
    // say "kept on this device" instead of claiming the mode was saved to
    // the account. Session-scoped: cleared on logout/account switch.
    QSet<QString> m_notificationModeSyncFailures;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
    std::unique_ptr<ThreadController> m_thread;
    std::unique_ptr<ConversationController> m_conversations;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<MediaBridge> m_mediaBridge;
    std::unique_ptr<MediaPlaybackController> m_playback;
    std::unique_ptr<PaginationController> m_pagination;
    std::unique_ptr<ReadReceiptCoordinator> m_readReceipts;
    std::unique_ptr<LinkPreviewController> m_linkPreviews;
    std::unique_ptr<MatrixGifTransport> m_gifTransport;
    std::unique_ptr<GifSearchController> m_gif;
    std::unique_ptr<GifSendController> m_gifSend;
    std::unique_ptr<TimelineScrollController> m_timelineScroll;
    std::unique_ptr<TimelineScrollController> m_threadScroll;

    // Show-QR verification. The grid itself lives in the store (memory
    // only); the controller keeps just the opaque URL token and the
    // SDK-reported progress flags. `clearVerificationQr` is the single
    // point that drops both, and every flow-ending path calls it.
    QrCodeStore m_qrCodeStore;
    QString m_verificationQrToken;
    bool m_verificationQrScanned = false;
    bool m_verificationQrConfirming = false;
    void clearVerificationQr();

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
