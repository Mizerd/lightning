#pragma once

#include "app/ConversationController.h"
#include "app/RoomDiscoveryController.h"
#include "app/DraftStore.h"
#include "app/ForwardController.h"
#include "app/ModerationController.h"
#include "app/UiaController.h"
#include "models/MessageSearchController.h"
#include "models/RoomExport.h"
#include "models/WidgetController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "app/CustomThemeStore.h"
#include "media/MediaVisibilityStore.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "i18n/LocalizationManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoBootstrapModel.h"
#include "crypto/CryptoHealthModel.h"
#include "crypto/CryptoManager.h"
#include "crypto/QrImageProvider.h"
#include "app/PolicyListController.h"
#include "crypto/QrLoginController.h"
#include "crypto/OwnDeviceKeyWatch.h"
#include "storage/AppDataPaths.h"
#include "storage/BridgeLabelStore.h"
#include "app/TrayIcon.h"
#include "text/SpellChecker.h"

#include <QTimer>
#include "profile/NameColorManager.h"
#include "profile/ProfileBannerManager.h"
#include "profile/ProfileBadges.h"
#include "profile/ProfileBioManager.h"
#include "profile/UserProfileResolver.h"
#include "app/AccountAvatarStore.h"
#include "media/MediaBridge.h"
#include "media/StagedImageStore.h"
#include "media/ImageCropper.h"
#include "media/MediaPlaybackController.h"
#include "media/MediaManager.h"
#include "media/VoiceRecorder.h"
#include "models/MessageComposer.h"
#include "models/MentionSuggestionModel.h"
#include "models/EmojiCatalog.h"
#include "models/LinkPreviewController.h"
#include "gif/GifSearchController.h"
#include "gif/GifSendController.h"
#include "stickers/StickerPackManager.h"
#include "gif/MatrixGifTransport.h"
#include "models/PaginationController.h"
#include "models/QuickSwitcherModel.h"
#include "models/ReadReceiptCoordinator.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "models/TimelineModel.h"
#include "app/PinnedMessagesController.h"
#include "app/RoomUpgradeController.h"
#include "models/TimelineScrollController.h"
#include "spaces/SpaceManager.h"
#include "spaces/RoomClosureController.h"
#include "spaces/SpaceModerationController.h"
#include "threads/ThreadController.h"
#include "calls/CallController.h"
#include "calls/RtcController.h"
#include "calls/CallDeviceController.h"
#include "calls/SfuCallController.h"
#include "calls/CallSoundController.h"
#include "presence/PresenceManager.h"
#include "update/UpdateManager.h"
#include "threads/ThreadManager.h"

#include <QFont>
#include <QObject>
#include <QRect>
#include <QString>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MatrixClient;
class NotificationManager;
class ReverseListProxyModel;
class SecretStore;

class AppController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(Screen currentScreen READ currentScreen NOTIFY currentScreenChanged)
    Q_PROPERTY(QString currentRoomId READ currentRoomId WRITE setCurrentRoomId NOTIFY currentRoomIdChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    // The colour emoji family, or empty when the host has none. Here rather
    // than on EmojiCatalog, whose test target links only Qt6::Core.
    Q_PROPERTY(QString emojiFontFamily READ emojiFontFamily CONSTANT)
    // Application icon for in-app surfaces: the default resource or a file:
    // URL to the custom icon with a cache-busting revision.
    Q_PROPERTY(QString appIconSource READ appIconSource NOTIFY appIconChanged)
    // Present in every build so bindings resolve; true only in a demo build
    // launched with --screenshot-demo.
    Q_PROPERTY(bool screenshotDemoActive READ screenshotDemoActive CONSTANT)
    // Null outside demo builds.
    Q_PROPERTY(QObject* demo READ demoController CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    // True when per-room notification modes sync with server push rules
    // (Rust backend). Fixed for the process lifetime.
    Q_PROPERTY(bool serverRoomNotificationModes READ serverRoomNotificationModes
               CONSTANT)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncModeLabel READ syncModeLabel NOTIFY syncModeChanged)
    // QStyleHints::colorScheme(), for the "System" theme.
    Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged)
    // True when the scene graph runs on Qt Quick's software backend, which
    // cannot draw video. Set once from main.cpp from the actual renderer
    // interface; false until then.
    Q_PROPERTY(bool softwareRenderer READ softwareRenderer
               NOTIFY softwareRendererChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)
    Q_PROPERTY(bool localRustResetRequired READ localRustResetRequired
               NOTIFY localRustResetRequiredChanged)
    // The local-session failure blocking sign-in, as the backend's diagnostic
    // token (matrix::rust_session::diagnosticName; empty = none), plus the
    // account it applies to. QML owns the user-facing copy.
    //
    // The identity is captured at detection time: during an add-account
    // attempt the settings' active account is still the previous one.
    Q_PROPERTY(QString localSessionFailureReasonCode
               READ localSessionFailureReasonCode
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureUserId
               READ localSessionFailureUserId
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureHomeserver
               READ localSessionFailureHomeserver
               NOTIFY localSessionFailureChanged)
    // True while an account switch is in flight; the previous session is
    // already detached.
    Q_PROPERTY(bool accountSwitching READ accountSwitching
               NOTIFY accountSwitchingChanged)

    // Own display name editor state. The name itself lives in the account
    // registry (AccountManager) and is deliberately not mirrored here.
    Q_PROPERTY(bool canEditOwnDisplayName READ canEditOwnDisplayName
               NOTIFY loggedInChanged)
    Q_PROPERTY(bool ownDisplayNameBusy READ ownDisplayNameBusy
               NOTIFY ownDisplayNameStateChanged)
    Q_PROPERTY(QString ownDisplayNameError READ ownDisplayNameError
               NOTIFY ownDisplayNameStateChanged)
    // Own avatar write state, independent of the display name's.
    Q_PROPERTY(bool ownAvatarBusy READ ownAvatarBusy
               NOTIFY ownAvatarStateChanged)
    Q_PROPERTY(QString ownAvatarError READ ownAvatarError
               NOTIFY ownAvatarStateChanged)

    // Redacted device id (e.g. "GAOT...GBSK"); empty when not on the Rust
    // backend or not logged in.
    Q_PROPERTY(QString rustDeviceIdRedacted READ rustDeviceIdRedacted NOTIFY rustDeviceIdChanged)

    // Cross-signing/verification state cached from the SDK; QML must not
    // derive it.
    Q_PROPERTY(QString sessionTrustState READ sessionTrustState NOTIFY securityStateChanged)
    Q_PROPERTY(QString sessionDeviceId READ sessionDeviceId NOTIFY securityStateChanged)
    Q_PROPERTY(bool ownIdentityAvailable READ ownIdentityAvailable NOTIFY securityStateChanged)
    Q_PROPERTY(bool crossSigningAvailable READ crossSigningAvailable NOTIFY securityStateChanged)

    // True only when the check answered that the curve25519 key published
    // on the server differs from the local Olm account's, so nothing sent to
    // this device can be decrypted. Never true when the check could not run
    // (offline, no keys yet, server error). Has its own notify signal so
    // per-row bindings do not re-evaluate on every trust update.
    Q_PROPERTY(bool encryptionIdentityBroken READ encryptionIdentityBroken
                   NOTIFY encryptionIdentityBrokenChanged)
    // True only when actionable: signed in, crypto-capable backend, and a
    // cross-signing identity that has not signed this device. False for
    // "Unknown" and "Cross-signing unavailable". Ignores dismissal.
    Q_PROPERTY(bool sessionVerificationNeeded READ sessionVerificationNeeded
                   NOTIFY securityStateChanged)
    // sessionVerificationNeeded AND the user has not dismissed the badges.
    Q_PROPERTY(bool sessionVerificationWarning READ sessionVerificationWarning
                   NOTIFY sessionVerificationWarningChanged)

    // Encrypted room-key import.
    Q_PROPERTY(QString roomKeyImportState READ roomKeyImportState NOTIFY roomKeyImportStateChanged)
    /// Bumped whenever an input of `canStartCall()` changes. QML bindings
    /// that call `canStartCall()` must also read this, or they never
    /// re-evaluate.
    Q_PROPERTY(int callGateRevision READ callGateRevision
                   NOTIFY callGateRevisionChanged)
    Q_PROPERTY(int roomKeyImportImportedCount READ roomKeyImportImportedCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(int roomKeyImportTotalCount READ roomKeyImportTotalCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(int roomKeyImportAffectedRoomCount READ roomKeyImportAffectedRoomCount NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(QString roomKeyImportLastMessage READ roomKeyImportLastMessage NOTIFY roomKeyImportStateChanged)
    Q_PROPERTY(bool roomKeyImportRunning READ roomKeyImportRunning NOTIFY roomKeyImportStateChanged)

    // SAS emoji verification.
    Q_PROPERTY(bool verificationActive READ verificationActive NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationFlowId READ verificationFlowId NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherUser READ verificationOtherUser NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherDevice READ verificationOtherDevice NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationIsSelfVerification READ verificationIsSelfVerification NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationState READ verificationState NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationEmojis READ verificationEmojis NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationDecimals READ verificationDecimals NOTIFY verificationStateChanged)

    // Show-QR verification: an alternative presentation of the same flow,
    // independent of verificationState.
    Q_PROPERTY(bool verificationQrAvailable READ verificationQrAvailable NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationQrImage READ verificationQrImage NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrScanned READ verificationQrScanned NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrConfirming READ verificationQrConfirming NOTIFY verificationStateChanged)

    Q_PROPERTY(SettingsManager* settings READ settings CONSTANT)
    // Rebindable shortcuts; QML Shortcut bindings depend on `bindingRevision`.
    Q_PROPERTY(ShortcutRegistry* shortcuts READ shortcuts CONSTANT)
    // UI language; application-wide, not per-account.
    Q_PROPERTY(LocalizationManager* localization READ localization CONSTANT)
    // Per-account custom palette overrides.
    Q_PROPERTY(CustomThemeStore* customTheme READ customTheme CONSTANT)
    // Device-local Spaces rail order and folders.
    Q_PROPERTY(RailLayoutStore* railLayout READ railLayout CONSTANT)
    // The rows the Spaces rail draws, plus live drag state; see RailEntryModel.
    Q_PROPERTY(RailEntryModel* railEntries READ railEntries CONSTANT)
    // Profile banners (MSC4427 / MSC4133), stable and Commet field names.
    Q_PROPERTY(ProfileBannerManager* banners READ banners CONSTANT)
    Q_PROPERTY(NameColorManager* nameColors READ nameColors CONSTANT)
    // Profile bios (MSC4440 / MSC4133), stable and unstable field names.
    // Plain text only.
    Q_PROPERTY(ProfileBioManager* bio READ bio CONSTANT)
    // Global profiles for users missing from the room member snapshot.
    Q_PROPERTY(UserProfileResolver* userProfiles READ userProfiles CONSTANT)
    /// Decorative badges from a fixed local table; no Matrix state and no
    /// verification claim.
    Q_PROPERTY(ProfileBadges* badges READ badges CONSTANT)
    /// Crop step for avatar and banner uploads; writes a cropped temp file.
    Q_PROPERTY(ImageCropper* imageCrop READ imageCrop CONSTANT)
    Q_PROPERTY(AuthManager* auth READ auth CONSTANT)
    Q_PROPERTY(AccountManager* accounts READ accounts CONSTANT)
    Q_PROPERTY(RoomListModel* roomList READ roomList CONSTANT)
    /// Every joined room with no Space filter, for room pickers such as
    /// forwarding. `roomList` shows only the active Space's rooms.
    Q_PROPERTY(RoomListModel* allRooms READ allRooms CONSTANT)
    /// The Channels layout's model: the active Space's direct hierarchy
    /// (see SpaceChannelModel).
    Q_PROPERTY(SpaceChannelModel* spaceChannels READ spaceChannels CONSTANT)
    /// The open room's widgets, opened in the browser rather than embedded
    /// (docs/widgets.md).
    Q_PROPERTY(WidgetController* widgets READ widgets CONSTANT)
    Q_PROPERTY(QuickSwitcherModel* quickSwitcher READ quickSwitcher CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineView READ timelineView CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    // Rich composer document bridge. QObject* so no QML type registration
    // is needed; everything QML calls is Q_INVOKABLE.
    Q_PROPERTY(QObject* richComposer READ richComposer CONSTANT)
    /// The composer's spell checker, as QObject* for the same reason. Never
    /// null; an unavailable backend reports `available: false`.
    Q_PROPERTY(QObject* spell READ spellChecker CONSTANT)
    // Whether a system tray exists; QML hides the tray settings otherwise.
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
    // The saved window geometry, or an invalid rect when it no longer fits
    // on any connected screen. Computed once at startup.
    Q_PROPERTY(QRect restorableWindowGeometry READ restorableWindowGeometry
                   CONSTANT)
    // Member suggestions for the room and thread composers' @-mention popups.
    Q_PROPERTY(MentionSuggestionModel* mentionSuggestions READ mentionSuggestions
                   CONSTANT)
    Q_PROPERTY(EmojiCatalog* emojiCatalog READ emojiCatalog CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    // MSC4108: signing another device in from this one.
    Q_PROPERTY(QrLoginController* qrLogin READ qrLogin CONSTANT)
    // Mjolnir-style policy lists.
    Q_PROPERTY(PolicyListController* policy READ policy CONSTANT)
    // Read-only E2EE health/readiness.
    Q_PROPERTY(CryptoHealthModel* cryptoHealth READ cryptoHealth CONSTANT)
    // Key backup and recovery management.
    Q_PROPERTY(QObject* backup READ backup CONSTANT)
    // "Send later".
    Q_PROPERTY(QObject* scheduledSends READ scheduledSends CONSTANT)
    // Activity Center model.
    Q_PROPERTY(QObject* activity READ activity CONSTANT)
    /// The room's media, files and links, walked independently of the
    /// timeline — see MediaHistoryModel.
    Q_PROPERTY(QObject* mediaHistory READ mediaHistory CONSTANT)
    // Verified-session key-bootstrap status.
    Q_PROPERTY(CryptoBootstrapModel* cryptoBootstrap READ cryptoBootstrap CONSTANT)
    // The account's sessions (server metadata + SDK trust), current first,
    // then by last seen.
    Q_PROPERTY(QVariantList sessionDevices READ sessionDevices NOTIFY sessionDevicesChanged)
    // Set by QML: the timeline is on screen, focused and at the latest
    // message. Used for active-room notification suppression.
    Q_PROPERTY(bool activeRoomAtLatest READ activeRoomAtLatest
                   WRITE setActiveRoomAtLatest NOTIFY activeRoomAtLatestChanged)
    // True while the open room is still hydrating (activeRoomAtLatest cannot
    // be true yet); see NotificationManager::Context::roomHydrating.
    Q_PROPERTY(bool activeRoomHydrating READ activeRoomHydrating
                   WRITE setActiveRoomHydrating
                   NOTIFY activeRoomHydratingChanged)
    Q_PROPERTY(bool sessionDevicesLoading READ sessionDevicesLoading NOTIFY sessionDevicesChanged)
    Q_PROPERTY(QString sessionDeviceRenameError READ sessionDeviceRenameError
                   NOTIFY sessionDevicesChanged)
    Q_PROPERTY(bool sessionDeviceRenaming READ sessionDeviceRenaming
                   NOTIFY sessionDevicesChanged)
    Q_PROPERTY(bool sessionDevicesFailed READ sessionDevicesFailed NOTIFY sessionDevicesChanged)
    Q_PROPERTY(SpaceManager* spaces READ spaces CONSTANT)
    Q_PROPERTY(ThreadManager* threads READ threads CONSTANT)
    Q_PROPERTY(PresenceManager* presence READ presence CONSTANT)
    Q_PROPERTY(CallController* calls READ calls CONSTANT)
    // MatrixRTC observation/discovery (modern group calling).
    Q_PROPERTY(RtcController* rtc READ rtc CONSTANT)
    // MatrixRTC group calling (SFU-backed).
    Q_PROPERTY(SfuCallController* groupCall READ groupCall CONSTANT)
    // Microphone/speaker/camera selection for calls.
    Q_PROPERTY(CallDeviceController* callDevices READ callDevices CONSTANT)
    // Call sounds. QML only uses it for Settings previews.
    Q_PROPERTY(CallSoundController* callSounds READ callSounds CONSTANT)
    // Pinned messages for the active room (not the Room Information panel's).
    Q_PROPERTY(PinnedMessagesController* pinned READ pinned CONSTANT)
    // Room upgrade links for the active room. Never follows an upgrade by
    // itself.
    Q_PROPERTY(RoomUpgradeController* roomUpgrade READ roomUpgrade CONSTANT)
    // The single open thread panel.
    Q_PROPERTY(ThreadController* thread READ thread CONSTANT)
    // Conversation creation, Room Information and the media bridge.
    Q_PROPERTY(ConversationController* conversations READ conversations CONSTANT)
    Q_PROPERTY(RoomDiscoveryController* discovery READ discovery CONSTANT)
    Q_PROPERTY(MessageSearchController* messageSearch READ messageSearch CONSTANT)
    Q_PROPERTY(UiaController* uia READ uia CONSTANT)
    Q_PROPERTY(ModerationController* moderation READ moderation CONSTANT)
    // Message forwarding; see ForwardController.
    Q_PROPERTY(ForwardController* forward READ forward CONSTANT)
    Q_PROPERTY(RoomInfoController* roomInfo READ roomInfo CONSTANT)
    // Kick / ban / unban from a Space, with the optional cascade to its rooms.
    Q_PROPERTY(SpaceModerationController* spaceModeration
                   READ spaceModeration CONSTANT)
    // Close a room or Space; delete one from the server as its administrator.
    Q_PROPERTY(RoomClosureController* roomClosure READ roomClosure CONSTANT)
    Q_PROPERTY(MediaBridge* mediaBridge READ mediaBridge CONSTANT)
    /// Every signed-in account's last known avatar, on disk: the media cache
    /// is memory-only and an inactive account's avatar cannot be fetched.
    Q_PROPERTY(AccountAvatarStore* accountAvatars READ accountAvatars CONSTANT)
    // Which images the reader has hidden locally; see MediaVisibilityStore.
    Q_PROPERTY(MediaVisibilityStore* mediaVisibility READ mediaVisibility
                   CONSTANT)
    // Microphone capture for MSC3245 voice messages. Created lazily on first
    // read so the audio backend only starts when needed; never changes after.
    Q_PROPERTY(VoiceRecorder* voiceRecorder READ voiceRecorder CONSTANT)
    // Which composer owns the shared recorder: "" when idle, "room" or
    // "thread". Both composers listen to the one recorder, so only the owner
    // may send its result. Ownership is never taken from a live recording.
    Q_PROPERTY(QString voiceOwner READ voiceOwner NOTIFY voiceOwnerChanged)
    // One audible media card at a time; stopped on room/account switch and
    // sign-out.
    Q_PROPERTY(MediaPlaybackController* playback READ playback CONSTANT)
    // Backward-pagination policy and automatic read receipts.
    Q_PROPERTY(PaginationController* pagination READ pagination CONSTANT)
    Q_PROPERTY(ReadReceiptCoordinator* readReceipts READ readReceipts CONSTANT)
    // Client-side link previews (Rust HTTPS fetcher).
    Q_PROPERTY(LinkPreviewController* linkPreviews READ linkPreviews CONSTANT)
    // GIF provider search and send pipeline.
    Q_PROPERTY(GifSearchController* gif READ gif CONSTANT)
    Q_PROPERTY(GifSendController* gifSend READ gifSend CONSTANT)
    // MSC2545 image packs: the sticker picker's controller, custom-emoji
    // lookup, and the m.sticker send / "add to my stickers" paths.
    Q_PROPERTY(StickerPackManager* stickers READ stickers CONSTANT)
    // Application updates; not account-scoped. See docs/updates.md.
    Q_PROPERTY(lightning::update::UpdateManager* updateManager READ updateManager CONSTANT)
    // Device-aware timeline wheel-scroll policy.
    Q_PROPERTY(TimelineScrollController* timelineScroll READ timelineScroll CONSTANT)
    // The thread panel's own wheel engine: same policy, separate motion
    // state from the room timeline.
    Q_PROPERTY(TimelineScrollController* threadScroll READ threadScroll CONSTANT)

public:
    enum Screen {
        LoginScreen = 0,
        MainScreen = 1,
        SettingsScreen = 2,
        // Shown while a saved session restores, so the login form never
        // flashes. Appended: QML routes on the integer values.
        BootScreen = 3,
    };
    Q_ENUM(Screen)

    enum Backend {
        HttpBackend = 0,
        MockBackend = 1,
        RustBackend = 2, // implemented iff ENABLE_RUST_SDK_BACKEND is defined
    };
    Q_ENUM(Backend)

    // True if this build contains an implementation of the backend.
    static bool isBackendCompiled(Backend backend);

    // `screenshotDemo` (demo builds only) makes the constructor use an
    // in-memory SecretStore and skip the normal session restore.
    explicit AppController(Backend backend = HttpBackend,
                           bool screenshotDemo = false,
                           QObject *parent = nullptr);
    ~AppController() override;

    // Quiesce background work (playback, sync) while the event dispatcher is
    // still valid, so no worker posts events during teardown (avoids
    // QEventDispatcherWin32 "Invalid window handle" on close). Wired to
    // aboutToQuit; idempotent.
    void prepareForShutdown();
    bool isShuttingDown() const { return m_shuttingDown; }

    Screen currentScreen() const { return m_currentScreen; }
    QString currentRoomId() const { return m_currentRoomId; }
    bool loggedIn() const;
    QString appVersion() const { return QStringLiteral(APP_VERSION); }
    QString emojiFontFamily() const;

    // A QFont with the emoji face as a fallback family. QML's font type has
    // no `families`, so this is the only way to give mixed text colour emoji.
    Q_INVOKABLE QFont textFontWithEmoji(const QString &family, int pixelSize,
                                        bool italic = false) const;

    // Custom application icon. The file is validated and normalized by
    // appicon::normalizeIconBytes (raster only, SVG refused) and a copy is
    // stored under app data. Returns "" on success or a translated error.
    // Launchers keep the packaged icon from the installed desktop entry.
    Q_INVOKABLE QString setCustomAppIconFromFile(const QUrl &fileUrl);
    // Removes the copy and restores the default icon immediately.
    Q_INVOKABLE void resetCustomAppIcon();

    /// A rect centred on one screen for a window of this size, or an empty
    /// rect when none can be computed safely (let the platform place it).
    Q_INVOKABLE static QRect centredWindowRect(int width, int height);

    /// True when the window's top grab band meets some screen.
    static bool windowGeometryIsReachable(const QRect &geometry);

    /// Logs how the window was placed at startup (geometry only).
    Q_INVOKABLE void noteWindowPlacement(const QString &how, int x, int y,
                                         int width, int height) const;
    QString appIconSource() const;
    bool screenshotDemoActive() const { return m_screenshotDemoActive; }
    // Development-only: enter demo mode and auto-login the demo account. A
    // no-op unless the mock backend is active.
    void beginScreenshotDemo(const QString &initialAccount = QString());
    // Development-only: forward the --demo-* options to the demo controller.
    void applyDemoLaunchOptions(const QString &scenario, const QString &theme,
                                const QString &appearance, const QString &size,
                                bool hideControls);
    // Development-only demo controller; null unless demo mode is active.
    QObject *demoController() const { return m_demoController; }
    QString backendName() const;
    bool serverRoomNotificationModes() const;
    QString connectionStatus() const { return m_connectionStatus; }
    QString syncModeLabel() const;
    bool systemDarkMode() const;
    bool softwareRenderer() const { return m_softwareRenderer; }
    /// Called once from main.cpp when the scene graph reports what it got.
    void setSoftwareRenderer(bool software);
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;
    bool localRustResetRequired() const { return m_localRustResetRequired; }
    // Public (not Q_INVOKABLE) so C++ tests can set the state; the real
    // source is a Rust-backend rejection a mock test cannot produce.
    void setLocalSessionFailure(const QString &reasonCode,
                                const QString &userId,
                                const QString &homeserver);
    // Only the caller knows whether the failure was resolved (login success
    // or a completed repair).
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
    ShortcutRegistry *shortcuts() const;
    LocalizationManager *localization() const;
    CustomThemeStore *customTheme() const;
    RailLayoutStore *railLayout() const;
    RailEntryModel *railEntries() const;
    ProfileBannerManager *banners() const;
    NameColorManager *nameColors() const;
    ProfileBioManager *bio() const;
    UserProfileResolver *userProfiles() const { return m_userProfiles.get(); }
    ProfileBadges *badges() const;
    ImageCropper *imageCrop() { return &m_imageCrop; }
    AuthManager *auth() const;
    AccountManager *accounts() const;
    RoomListModel *roomList() const;
    RoomListModel *allRooms() const;
    SpaceChannelModel *spaceChannels() const;
    WidgetController *widgets() const { return m_widgets.get(); }
    QuickSwitcherModel *quickSwitcher() const;
    TimelineModel *timeline() const;
    QAbstractItemModel *timelineView() const;
    MessageComposer *composer() const;
    QObject *richComposer() const;
    MentionSuggestionModel *mentionSuggestions() const
    { return m_mentionSuggestions.get(); }
    EmojiCatalog *emojiCatalog() const { return m_emojiCatalog.get(); }
    MediaManager *media() const;
    CryptoManager *crypto() const;
    QrLoginController *qrLogin() const { return m_qrLogin.get(); }
    PolicyListController *policy() const { return m_policy.get(); }
    CryptoHealthModel *cryptoHealth() const { return m_cryptoHealth.get(); }
    QObject *backup() const;
    QObject *scheduledSends() const;
    QObject *activity() const;
    QObject *mediaHistory() const;
    CryptoBootstrapModel *cryptoBootstrap() const
    { return m_cryptoBootstrap.get(); }
    // Bounded UI refresh (Settings "Refresh" and post-operation updates).
    Q_INVOKABLE void refreshCryptoHealth();
    QVariantList sessionDevices() const { return m_sessionDevices; }
    bool sessionDevicesLoading() const { return m_sessionDevicesLoading; }
    bool sessionDevicesFailed() const { return m_sessionDevicesFailed; }
    Q_INVOKABLE void refreshSessionDevices();
    // Rename one of this account's sessions; refetches on success. Result in
    // sessionDeviceRenameError ("" = fine).
    Q_INVOKABLE void renameSessionDevice(const QString &deviceId,
                                         const QString &name);
    QString sessionDeviceRenameError() const { return m_sessionDeviceRenameError; }
    bool sessionDeviceRenaming() const { return m_sessionDeviceRenameOp != 0; }
    bool activeRoomAtLatest() const { return m_activeRoomAtLatest; }
    bool activeRoomHydrating() const { return m_activeRoomHydrating; }
    void setActiveRoomAtLatest(bool atLatest);
    void setActiveRoomHydrating(bool hydrating);
    SpaceManager *spaces() const;
    ThreadManager *threads() const;
    PresenceManager *presence() const;
    CallController *calls() const;
    /// Start a call in `roomId`. MatrixRTC (SFU) is primary; the legacy 1:1
    /// lane is an audio-only, DM-only fallback. Returns false and emits
    /// `callStartRefused` when neither lane is usable.
    Q_INVOKABLE bool startCall(const QString &roomId, bool withVideo = false);
    /// Whether `startCall` would do anything for this room. Bindings must
    /// also read `callGateRevision`, since the inputs change asynchronously.
    Q_INVOKABLE bool canStartCall(const QString &roomId) const;
    /// The single user a legacy 1:1 call in `roomId` would be placed to, or
    /// empty when the room is not a genuine two-party DM (see the .cpp).
    QString legacyCallPeer(const QString &roomId) const;
    /// Which lane `startCall` would use: "matrixrtc", "legacy" or "" for
    /// none. Diagnostics and tests; not shown in normal UI.
    Q_INVOKABLE QString preferredCallLane(const QString &roomId) const;
    int callGateRevision() const { return m_callGateRevision; }
    RtcController *rtc() const;
    SfuCallController *groupCall() const;
    CallDeviceController *callDevices() const;
    CallSoundController *callSounds() const;
    // Installs the call-sound audio player. main.cpp only; tests never open
    // an audio device.
    void enableCallSounds();
    // Registers the WebRTC media engine when built and its elements resolve.
    // main.cpp only; tests opt in explicitly.
    void enableCallMediaEngine();

private:

    /// Push read-receipt privacy and typing-notice settings into the client
    /// and composer. Also on every client attach: a fresh bridge starts
    /// permissive.
    void applyPrivacyPreferences();
    /// Push MSC4153 into the Rust process global; takes effect at the next
    /// sign-in.
    void applyStrictDeviceTrust();
    /// Whether a notification action's account is still the active one;
    /// otherwise refuses and tells the user.
    bool notificationActionIsForCurrentAccount(const QString &accountUserId);
    void copyImageBytesToClipboard(const QString &mediaKey, bool ok,
                                   const QByteArray &bytes,
                                   const QString &category);

public:
    // Test seam: no notification daemon under offscreen runs.
    NotificationManager *notificationsForTest() const
    { return m_notifications.get(); }
    PinnedMessagesController *pinned() const { return m_pinned.get(); }
    RoomUpgradeController *roomUpgrade() const { return m_roomUpgrade.get(); }
    ThreadController *thread() const { return m_thread.get(); }
    ConversationController *conversations() const { return m_conversations.get(); }
    RoomDiscoveryController *discovery() const { return m_discovery.get(); }
    MessageSearchController *messageSearch() const { return m_messageSearch.get(); }
    UiaController *uia() const { return m_uia.get(); }
    ModerationController *moderation() const { return m_moderation.get(); }
    ForwardController *forward() const { return m_forward.get(); }
    // OAuth accounts manage devices in the account console, not via a
    // password prompt. Evaluated when the Sessions page opens.
    Q_INVOKABLE bool activeAccountIsOAuth() const
    {
        return m_settings
               && m_settings->isOAuthAccount(m_settings->activeAccountUserId());
    }
    RoomInfoController *roomInfo() const { return m_roomInfo.get(); }
    SpaceModerationController *spaceModeration() const
    {
        return m_spaceModeration.get();
    }
    RoomClosureController *roomClosure() const { return m_roomClosure.get(); }
    MediaBridge *mediaBridge() const { return m_mediaBridge.get(); }
    AccountAvatarStore *accountAvatars() const
    {
        return m_accountAvatars.get();
    }
    MediaVisibilityStore *mediaVisibility() const
    { return m_mediaVisibility.get(); }
    VoiceRecorder *voiceRecorder()
    {
        if (!m_voiceRecorder)
            m_voiceRecorder = std::make_unique<VoiceRecorder>(this);
        return m_voiceRecorder.get();
    }
    QString voiceOwner() const { return m_voiceOwner; }
    // Start a recording owned by `owner` ("room" or "thread"). Returns false
    // when no device/encoder is available or a recording is already in
    // progress (voiceRecordingBusy() tells them apart). Ownership is taken
    // only after a successful start.
    Q_INVOKABLE bool startVoiceRecording(const QString &owner);
    // True while any recording is in progress or finalizing.
    Q_INVOKABLE bool voiceRecordingBusy() const;
    // Test seam: replaces the recorder (takes ownership) so ownership rules
    // can be tested without a microphone.
    void setVoiceRecorderForTest(VoiceRecorder *recorder);
    // Release ownership without touching the recorder. Called after the
    // owning composer has consumed ready()/failed().
    Q_INVOKABLE void endVoiceRecording();
    // Discard an in-progress recording and release ownership.
    Q_INVOKABLE void cancelVoiceRecording();
    // Delete a finalized recording the user chose not to send. Only deletes
    // a file the recorder itself produced, never an arbitrary QML path.
    Q_INVOKABLE bool discardPreparedVoice(const QString &localPath);
    MediaPlaybackController *playback() const { return m_playback.get(); }
    PaginationController *pagination() const { return m_pagination.get(); }
    ReadReceiptCoordinator *readReceipts() const { return m_readReceipts.get(); }
    LinkPreviewController *linkPreviews() const { return m_linkPreviews.get(); }
    GifSearchController *gif() const { return m_gif.get(); }
    lightning::update::UpdateManager *updateManager() const { return m_updateManager.get(); }
    GifSendController *gifSend() const { return m_gifSend.get(); }
    StickerPackManager *stickers() const { return m_stickers.get(); }
    TimelineScrollController *timelineScroll() const { return m_timelineScroll.get(); }
    TimelineScrollController *threadScroll() const { return m_threadScroll.get(); }
    SecretStore *secretStore() const { return m_secretStore.get(); }

public Q_SLOTS:
    void setCurrentRoomId(const QString &roomId);
    void showLogin();
    void showMain();
    void showSettings();
    void openRoom(const QString &roomId);
    // A room link (matrix.to or matrix: URI) activated in the app. Routed to
    // Discover, which resolves it through the SDK; QML never parses it.
    void openMatrixLink(const QString &link) { Q_EMIT matrixLinkRequested(link); }

    // Select the Space and clear the open room so the Space overview shows.
    void openSpaceHome(const QString &spaceId);
    // The Channels layout's "Lobby": no room open and no Space selected.
    // Navigation only, not a fake room.
    Q_INVOKABLE void openLobby();

    // Per-room notification mode (0 = all, 1 = mentions & keywords,
    // 2 = mute). Writes the device-local value first so policy applies
    // immediately and offline, then the server push rule where supported.
    // Server reports update the cache only when user-defined; after a failed
    // write, only a report equal to the cached value is accepted. Mode 0 sets
    // an explicit server "all messages" rule.
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    // Mute or unmute every joined room in a Space (Matrix has no Space-level
    // mute). Unmuting restores mode 3 (account default), not "all messages".
    // Each room reports through the per-room notification-mode path.
    Q_INVOKABLE void setSpaceMuted(const QString &spaceId, bool mute);
    // Whether every joined room in the Space is muted; false for no rooms.
    Q_INVOKABLE bool spaceIsMuted(const QString &spaceId) const;
    // Mark every joined room in the Space (including subspaces) read, via
    // the room list's own Mark as read path.
    Q_INVOKABLE void markSpaceRead(const QString &spaceId);
    /// "Jump to date" (MSC3030): jump the open room to the first event at or
    /// after `timestampMs`. Returns the op id, or 0 when the backend cannot
    /// ask; a server without the endpoint reports
    /// jumpToDateFinished(ok=false, "not_found"). No client-side pagination
    /// fallback.
    Q_INVOKABLE quint64 jumpToDate(qint64 timestampMs);

    // Room export: writes the open room's loaded timeline to a user-chosen
    // file (see models/RoomExport.h).
    //
    // Security: the only path that writes encrypted-room plaintext to disk.
    // `includeEncryptedText` must come from a surface that asked the user
    // explicitly; when false, encrypted bodies are replaced by a withheld
    // marker.

    /// How many messages an export of the open room would contain.
    Q_INVOKABLE int exportableMessageCount() const;
    /// A safe leaf filename to suggest. `format` is "text" or "json".
    Q_INVOKABLE QString suggestedExportFileName(const QString &format) const;
    /// Write the export. `fileUrl` is a local file URL from a save dialog.
    /// Returns "" on success, or a short human-readable reason. Never logs
    /// the path's contents and never logs a message body.
    Q_INVOKABLE QString exportCurrentRoom(const QUrl &fileUrl,
                                          const QString &format,
                                          bool includeEncryptedText);
private:
    /// Shared by the count, the suggested name and the export itself.
    roomexport::Options exportOptions() const;
public:
    // Poll-on-open refresh: re-query the server rule when a notification
    // picker opens so changes made in another client land in the cache.
    // No-op on backends without server support.
    Q_INVOKABLE void requestRoomNotificationMode(const QString &roomId);
    /// Ask what a room's bridge advertises (MSC2346), once per room per
    /// session, for the room list badge. `allowNetwork` permits the `/state`
    /// fallback, needed because the SDK store lacks this event type (see
    /// matrix/BridgeNetwork.h). A room asked without the network may be
    /// asked once more with it.
    Q_INVOKABLE void requestRoomBridgeInfo(const QString &roomId,
                                           bool allowNetwork);
    // True while the room's last server push-rule write is known to have
    // failed (the local mode still applies). Retried on reconnection; cleared
    // only when the server acknowledges the value. Session-scoped.
    Q_INVOKABLE bool roomNotificationModeSyncFailed(const QString &roomId) const;

    // Switch the whole Matrix context to another saved account. The previous
    // account stays signed in; only its runtime is detached. No-op when
    // already switching or the target is unusable.
    Q_INVOKABLE void switchToAccount(const QString &userId);

    // Own display name. Single-flight, matched by op id recorded before the
    // backend call; answers with any other id are dropped.
    bool canEditOwnDisplayName() const;
    bool ownDisplayNameBusy() const { return m_displayNameOp != 0; }
    QString ownDisplayNameError() const { return m_displayNameError; }
    // Client-side ceiling (Matrix specifies none); mirrors the Rust bound.
    Q_INVOKABLE int ownDisplayNameMaxLength() const { return 255; }
    // Length in code points; QML's `text.length` counts UTF-16 units.
    Q_INVOKABLE int displayNameLength(const QString &name) const;
    // Returns true when a write was dispatched. Refuses when busy,
    // unsupported, empty (use clearOwnDisplayName), over the ceiling or
    // unchanged; ownDisplayNameError explains all but "unchanged".
    Q_INVOKABLE bool submitOwnDisplayName(const QString &name);
    // Removes the field on the server (sent as `None`, not an empty name).
    Q_INVOKABLE bool clearOwnDisplayName();
    Q_INVOKABLE void dismissOwnDisplayNameError();

    bool ownAvatarBusy() const { return m_avatarOp != 0; }
    QString ownAvatarError() const { return m_avatarError; }
    // Whether this backend can write the own avatar; QML hides it otherwise.
    Q_INVOKABLE bool canEditOwnAvatar() const;
    // Returns true when a write was dispatched. Takes a local file (the crop
    // output); Rust sniffs the bytes and refuses non-raster images.
    Q_INVOKABLE bool submitOwnAvatar(const QUrl &fileUrl);
    Q_INVOKABLE bool clearOwnAvatar();
    Q_INVOKABLE void dismissOwnAvatarError();

    // Remove one saved account from this device: a server logout if active,
    // otherwise delete its local store, token and record. Other accounts are
    // unaffected.
    Q_INVOKABLE void removeAccount(const QString &userId);

    // Open Settings on a category; SettingsScreen consumes it once on load.
    Q_INVOKABLE void showSettingsSection(const QString &section);
    Q_INVOKABLE QString takeRequestedSettingsSection();

    // Apply the theme to the application palette, which popups, menus and
    // tooltips paint from (an item palette does not reach them). Keys are
    // palette role names ("window", "text", "highlight", ...), plus
    // "disabled"-prefixed variants.
    Q_INVOKABLE void applyControlPalette(const QVariantMap &roles);

    // Recovery-key restore. The key goes straight to Rust and is never
    // stored or logged. No-op on non-Rust backends; results arrive via
    // recoveryStateChanged().
    Q_INVOKABLE void requestRecoverFromBackup(const QString &recoveryKey);

    // Delete the local Rust SDK store for the configured account only.
    // Emits localRustStoreResetResult(ok, message). No-op on non-Rust.
    Q_INVOKABLE void resetLocalRustStore();

    // Reset for an explicit identity; QML never computes paths or deletes
    // files. The login screen uses repairLocalSession() instead.
    Q_INVOKABLE void resetLocalRustSession(const QString &homeserver,
                                           const QString &user);

    // Repair the local session of the account that failed (captured at
    // detection), or the active account when none is pending. Takes no
    // arguments so it cannot be aimed at the wrong account.
    Q_INVOKABLE void repairLocalSession();

    // Whether clearing local data can repair this failure; QML shows the
    // destructive action only then. Unknown or empty codes answer false.
    Q_INVOKABLE bool localResetHelpsFor(const QString &reasonCode) const;

    // Sanitized support bundle for the clipboard: versions, capabilities,
    // lifecycle state and error categories only. Never tokens, keys,
    // recovery material, message bodies, room ids or paths.
    Q_INVOKABLE QString sessionDiagnosticsText() const;
    Q_INVOKABLE void copySessionDiagnostics();

    // Retry decryption in place after a key arrives, keeping the loaded
    // history, media and open thread. Used by every automatic trigger.
    void retryDecryptionInCurrentRoom();

    // Rebuilds the room's timeline (new subscription, re-pagination, media
    // and thread cleared). Only for the explicit user Refresh; never call it
    // automatically. No-op on non-Rust.
    Q_INVOKABLE void reloadCurrentRoomTimeline(int limit = 30);

    // Release the paginated backlog and reopen at the live edge (as Element's
    // jumpToLiveTimeline() does). Returns true only when a trim was
    // dispatched; see historyTrimAllowed() for the conditions. Only for an
    // explicit user action, never from scrolling or pagination.
    Q_INVOKABLE bool trimHistoryAndJumpToLive();
    // The trim policy as a pure predicate, testable without a Rust event
    // cache. Requires the Rust backend, an open room not mid-pagination, and
    // more loaded rows than the threshold.
    static bool historyTrimAllowed(bool rustBackend, bool roomOpen,
                                   bool paginationBusy, bool threadOpen,
                                   int loadedRows, int rowThreshold)
    {
        if (!rustBackend || !roomOpen)
            return false;
        if (paginationBusy)
            return false;
        // An open thread view holds its own subscription, which the reload
        // would tear down.
        if (threadOpen)
            return false;
        return loadedRows > rowThreshold;
    }
    // The loaded-row count above which a jump-to-live trims.
    Q_INVOKABLE int historyTrimRowThreshold() const { return 400; }

    // Copy the decrypted image to the clipboard on explicit user action.
    // Result on copyImageFinished.
    Q_INVOKABLE void copyImageToClipboard(const QString &mediaKey);
    // Star a timeline GIF: fetch its decrypted bytes through the media bridge
    // and hand them to GifStarredStore. Result on
    // app.gif.starredStore.starFinished. The only place MediaBridge and the
    // star store meet, so neither depends on the other.
    Q_INVOKABLE void starChatGif(const QString &mediaKey);
    // Star state and unstar for the hover star. Checks this session's stars
    // by media key first, then falls back to the content hash of bytes the
    // display cache already holds (never a new fetch, and no Matrix id is
    // persisted). See GifStarredStore.
    Q_INVOKABLE bool isChatGifStarred(const QString &mediaKey) const;
    Q_INVOKABLE void unstarChatGif(const QString &mediaKey);

    // SAS emoji verification.
    Q_INVOKABLE void acceptVerification();
    Q_INVOKABLE void confirmVerification();
    Q_INVOKABLE void mismatchVerification();
    Q_INVOKABLE void cancelVerification();

    // The user confirms the other device scanned the QR code. The SDK alone
    // performs the trust change.
    Q_INVOKABLE void confirmQrVerification();

    // Start SAS verification against another session of this account.
    Q_INVOKABLE void startOwnVerification();

    // "Request keys again": a fresh secret-request round and a re-armed
    // bootstrap wait. Rust backend only.
    Q_INVOKABLE void requestEncryptionKeys();

    // Re-query SDK trust state after a manual action.
    Q_INVOKABLE void refreshSessionTrustState();

    // Import encrypted room keys from a local file. The passphrase goes
    // straight to Rust and is never stored or logged.
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
        // An opaque per-code token, so no flow id reaches a URL and QML's
        // image cache is busted per code.
        return m_verificationQrToken.isEmpty()
            ? QString{}
            : QStringLiteral("image://lightning-qr/") + m_verificationQrToken;
    }
    bool trayAvailable() const { return TrayIcon::platformSupportsTray(); }
    QObject *spellChecker() { return &m_spell; }
    QRect restorableWindowGeometry() const { return m_restorableWindowGeometry; }
    bool verificationQrScanned() const { return m_verificationQrScanned; }
    bool verificationQrConfirming() const { return m_verificationQrConfirming; }

    // Owned here so it outlives the QML engine that holds the provider.
    QrCodeStore *qrCodeStore() { return &m_qrCodeStore; }
    // Ditto (read from the QML loader thread). Bytes of queued clipboard
    // images, which have no file to preview.
    StagedImageStore *stagedImages() { return &m_stagedImages; }

    // Security & Recovery accessors.
    QString sessionTrustState() const { return m_sessionTrustState; }
    bool sessionVerificationNeeded() const;
    bool sessionVerificationWarning() const;
    // Dismiss the verification badges for the active account (persisted).
    // Does not claim the session is verified.
    Q_INVOKABLE void dismissVerificationWarning();
    QString sessionDeviceId() const { return m_sessionDeviceId; }
    bool ownIdentityAvailable() const { return m_ownIdentityAvailable; }
    bool crossSigningAvailable() const { return m_crossSigningAvailable; }
    bool encryptionIdentityBroken() const
    { return m_ownDeviceKeyWatch.broken(); }

    QString roomKeyImportState() const { return m_roomKeyImportState; }
    int roomKeyImportImportedCount() const { return m_roomKeyImportImported; }
    int roomKeyImportTotalCount() const { return m_roomKeyImportTotal; }
    int roomKeyImportAffectedRoomCount() const { return m_roomKeyImportAffected; }
    QString roomKeyImportLastMessage() const { return m_roomKeyImportMessage; }
    bool roomKeyImportRunning() const { return m_roomKeyImportRunning; }

Q_SIGNALS:
    // The tray icon was clicked; Main.qml restores and raises the window.
    void trayShowRequested();

    void voiceOwnerChanged();
    // Emitted when a reconnect retry batch is issued, with the room count,
    // so tests can observe the retry without a live session.
    void roomNotificationModesRetried(int roomCount);
    void currentScreenChanged();
    // From showSettingsSection(), for the kept-alive settings screen.
    void settingsSectionRequested(const QString &section);
    void appIconChanged();
    void initialSyncDoneChanged();
    void accountSwitchingChanged();
    void ownDisplayNameStateChanged();
    void ownAvatarStateChanged();
    void ownAvatarSaved();
    // A deliberate quit: windows must not refuse it via close-to-tray, or
    // the quit is aborted.
    void applicationQuitIntended();
    // Server-confirmed success; the editor closes on this. Waiting for the
    // registry would hang when the name did not change.
    void ownDisplayNameSaved();
    void currentRoomIdChanged();
    void loggedInChanged();
    void connectionStatusChanged();
    void syncModeChanged();
    void systemDarkModeChanged();
    void softwareRendererChanged();
    void errorReported(const QString &message);
    /// A call could not be started, with wording already fit to show.
    void callStartRefused(const QString &message);
    // See openMatrixLink().
    void matrixLinkRequested(const QString &link);
    void rustDeviceIdChanged();
    void localRustResetRequiredChanged();
    void localSessionFailureChanged();
    // Once per requestRecoverFromBackup call. `state`: "attempted", "ok" or
    // "failed"; `message` is non-secret failure detail, never key material.
    void recoveryStateChanged(const QString &state, const QString &message);

    // Emitted after resetLocalRustStore() finishes.
    void localRustStoreResetResult(bool ok, const QString &message);

    // Fires after reloadCurrentRoomTimeline completes.
    void currentRoomTimelineReloaded(int totalEvents,
                                     int decryptedEvents,
                                     int undecryptableEvents);

    // SAS verification.
    void verificationStateChanged();
    void sessionVerificationWarningChanged();
    void sessionDevicesChanged();
    void activeRoomAtLatestChanged();
    void activeRoomHydratingChanged();
    void copyImageFinished(bool ok, const QString &message);
    /// "Jump to date" answered; on success the jump was already dispatched.
    /// `category` is sanitized ("not_found", "forbidden", "rate_limited",
    /// "network"), never server prose.
    void jumpToDateFinished(quint64 opId, bool ok, const QString &category);
    // A notification was clicked: QML raises the window, opens the thread
    // and locates the event. Identifiers only.
    void notificationOpenRequested(const QString &roomId,
                                   const QString &eventId,
                                   const QString &threadRootId);

    // roomNotificationModeSyncFailed() changed for this room.
    void roomNotificationModeSyncStateChanged(const QString &roomId);

    // Security & Recovery.
    void securityStateChanged();
    void encryptionIdentityBrokenChanged();
    void roomKeyImportStateChanged();
    void callGateRevisionChanged();
    // After a successful room-key import, with aggregate counts.
    void roomKeyImportCompleted(int imported, int total, int affectedRooms);

private:
    /// Whether the incoming-call notification may offer to answer. Shared by
    /// the raise site and the later update so they cannot disagree.
    bool callAcceptOffered(const QString &roomId, bool rtcLane) const;
    // Common landing for a notification click and an Activity Center row:
    // opens the room (subscribing its timeline), then emits
    // notificationOpenRequested for the QML half.
    void routeNotificationOpen(const QString &roomId, const QString &eventId,
                               const QString &threadRootId);
    void setCurrentScreen(Screen s);
    void setConnectionStatus(const QString &s);
    // Applies the custom icon when enabled and readable, else the default.
    void applyAppIcon();
    void onLoginSucceeded();
    void onLoggedOut();
    void setLocalRustResetRequired(bool required);
    void setAccountSwitching(bool switching);
    // Failure path of switchToAccount: falls back to the previous account
    // once; if that is impossible, lands on the login screen.
    void failAccountSwitch(const QString &message);

    // How a saved account's stored credential reads right now. Three states:
    // an unreadable secret store (locked keyring, no session bus) says nothing
    // about whether the account is signed out.
    enum class SignInState {
        Usable,      // a token was read
        Gone,        // no token AND the backend could answer: really signed out
        Unreadable,  // the backend could not answer; says nothing about the account
    };
    SignInState signInStateFor(const QString &userId) const;
    // Clears every cache that must not leak across accounts. Used on account
    // change; logout clears the same state through loggedOut connections.
    void clearCrossAccountCaches();

    // Resolve an account's on-disk layout for removal: the recorded identity
    // first, then the canonical layout. False = nothing to act on.
    bool resolveRemovalIdentity(const QString &userId,
                                matrix::app_data::AccountIdentity *identity) const;
    // Delete every local trace of a resolved account (SDK store, account
    // directory, cache.sqlite, starred GIFs, bridge labels). Shared by both
    // removal paths. Logs deleted, absent and failed distinctly.
    void removeAccountLocalState(const matrix::app_data::AccountIdentity &identity);

    // Rate-limited own-identity-key check and its answer, shared by every
    // caller.
    void requestOwnDeviceKeyCheck();
    void applyOwnDeviceKeyAgreement(matrix::crypto::KeyAgreement agreement);

    static std::unique_ptr<MatrixClient> makeClient(Backend backend,
                                                    SettingsManager *settings,
                                                    QObject *parent);

    Backend m_backend;
    // Cache-busting revision for appIconSource.
    int m_appIconRevision = 0;
    // See the constructor.
    bool m_screenshotDemo = false;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    /// Event ids already notified via a thread timeline, so the room copy
    /// does not notify again. Bounded; cleared wholesale.
    QSet<QString> m_notifiedThreadEventIds;
    // Rooms whose members were fetched this session (once per room).
    QSet<QString> m_memberHydratedRooms;
    // Rooms whose MSC2346 bridge state was read -> whether the network was
    // allowed. Cleared on logout/switch.
    QHash<QString, bool> m_bridgeReadRooms;
    // The bridge answers, persisted per account so badges paint before any
    // request (see matrix::app_data::bridgeLabelsFile).
    BridgeLabelStore m_bridgeLabels;
    QString m_requestedSettingsSection;
    QString m_connectionStatus;
    bool m_localRustResetRequired = false;
    QString m_localSessionFailureReason;
    QString m_localSessionFailureUserId;
    QString m_localSessionFailureHomeserver;
    bool m_resetResultPending = false;
    bool m_accountSwitching = false;
    // The account to fall back to if activating the switch target fails.
    // Consumed by the loginFailed handler; empty = no fallback pending.
    QString m_switchFallbackUserId;
    // The account whose session most recently succeeded — used to detect a
    // cross-account transition in onLoginSucceeded.
    QString m_lastSessionUserId;
    // Removing the active account needs a server logout first, so the
    // removal finishes in onLoggedOut. The identity is resolved before the
    // logout, because sign-out removes the saved record it is keyed on.
    // Empty user id = no removal pending.
    QString m_pendingRemovalUserId;
    matrix::app_data::AccountIdentity m_pendingRemovalIdentity;
    bool m_pendingRemovalResolved = false;
    // Own profile writes: 0 = idle, else the caller-owned id of the write
    // in flight.
    quint64 m_displayNameOp = 0;
    quint64 m_avatarOp = 0;
    quint64 m_avatarOpCounter = 0;
    QString m_avatarError;
    quint64 m_displayNameOpCounter = 0;
    QString m_displayNameError;
    // Shared by set and clear; records the op id before the backend call.
    bool dispatchOwnDisplayName(const QString &name);
    // Empty when a write may be dispatched; otherwise the reason it may not.
    QString ownDisplayNameUnavailableReason() const;
    // The registry's cached name, used only to refuse an unchanged write.
    QString cachedOwnDisplayName() const;
    // Retire the in-flight write on every session teardown so a late answer
    // is stale for the next account.
    void retireOwnDisplayNameWrite();
    void retireOwnAvatarWrite();
    // Add-account mode: the account to return to when the login fails or
    // the user presses Back.
    QString m_addAccountReturnTo;
    // True while the previous account restores after a failed add-account
    // attempt, so its loginSucceeded does not leave the login screen.
    bool m_backgroundRestore = false;

    // Order matters: SecretStore is constructed first so SettingsManager can
    // be wired to it before any code touches accessToken() / hasSession().
    std::unique_ptr<SecretStore> m_secretStore;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<LocalizationManager> m_localization;
    std::unique_ptr<ShortcutRegistry> m_shortcuts;
    std::unique_ptr<CustomThemeStore> m_customTheme;
    std::unique_ptr<RailLayoutStore> m_railLayout;
    std::unique_ptr<RailEntryModel> m_railEntries;
    std::unique_ptr<ProfileBannerManager> m_banners;
    std::unique_ptr<NameColorManager> m_nameColors;
    std::unique_ptr<ProfileBioManager> m_bio;
    std::unique_ptr<UserProfileResolver> m_userProfiles;
    std::unique_ptr<ProfileBadges> m_badges;
    bool m_shuttingDown = false;
    // Never true in a release build.
    bool m_screenshotDemoActive = false;
    bool m_softwareRenderer = false;
    // QObject* keeps ScreenshotDemoController out of this header. Parented to
    // this; null in non-demo builds.
    QObject *m_demoController = nullptr;
    std::unique_ptr<MatrixClient> m_client;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<RoomListModel> m_roomList;
    // Same rows, no Space filter. See the allRooms property.
    std::unique_ptr<RoomListModel> m_allRooms;
    std::unique_ptr<SpaceChannelModel> m_spaceChannels;
    std::unique_ptr<WidgetController> m_widgets;
    std::unique_ptr<QuickSwitcherModel> m_quickSwitcher;
    std::unique_ptr<TimelineModel> m_timeline;
    std::unique_ptr<ReverseListProxyModel> m_timelineView;
    std::unique_ptr<MessageComposer> m_composer;
    std::unique_ptr<class RichComposerBridge> m_richComposer;
    std::unique_ptr<DraftStore> m_draftStore;
    std::unique_ptr<MentionSuggestionModel> m_mentionSuggestions;
    std::unique_ptr<EmojiCatalog> m_emojiCatalog;
    std::unique_ptr<NotificationManager> m_notifications;
    std::unique_ptr<MediaManager> m_media;
    std::unique_ptr<CryptoManager> m_crypto;
    std::unique_ptr<CryptoHealthModel> m_cryptoHealth;
    std::unique_ptr<class BackupController> m_backup;
    std::unique_ptr<class ScheduledSendController> m_scheduledSends;
    std::unique_ptr<class ActivityModel> m_activity;
    std::unique_ptr<class MediaHistoryModel> m_mediaHistory;
    bool m_activitySeeded = false;
    /// In-flight "jump to date" requests: op id -> room.
    QHash<quint64, QString> m_pendingDateJumps;
    /// Periodic, idempotent local search index sweep (rather than hooks in
    /// every ingest path).
    QTimer m_searchIndexTimer;
    std::unique_ptr<CryptoBootstrapModel> m_cryptoBootstrap;
    // Generation captured when a crypto-health query is dispatched, so an
    // answer arriving after a logout or switch is rejected.
    quint64 m_cryptoQueryGeneration = 1;
    QVariantList m_sessionDevices;
    bool m_sessionDevicesLoading = false;
    quint64 m_sessionDeviceRenameOp = 0;
    QString m_sessionDeviceRenameError;
    bool m_sessionDevicesFailed = false;
    bool m_activeRoomAtLatest = false;
    bool m_activeRoomHydrating = false;
    QSet<QString> m_knownInvites;
    // Rooms whose last server push-rule write failed ("kept on this
    // device"). Cleared on logout/account switch.
    QSet<QString> m_notificationModeSyncFailures;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
    std::unique_ptr<PresenceManager> m_presence;
    std::unique_ptr<CallController> m_calls;
    /// See the Q_PROPERTY: bumped from RtcController's own change signals.
    int m_callGateRevision = 0;
    std::unique_ptr<RtcController> m_rtc;
    std::unique_ptr<SfuCallController> m_groupCall;
    std::unique_ptr<CallDeviceController> m_callDevices;
    std::unique_ptr<CallSoundController> m_callSounds;
    // The call whose ring was actually announced; only that one can become
    // a missed-call notice. Plus a per-sender ring cooldown.
    QString m_announcedCallId;
    QHash<QString, qint64> m_lastCallRingBySender;
    std::unique_ptr<PinnedMessagesController> m_pinned;
    std::unique_ptr<RoomUpgradeController> m_roomUpgrade;
    std::unique_ptr<ThreadController> m_thread;
    std::unique_ptr<ConversationController> m_conversations;
    std::unique_ptr<RoomDiscoveryController> m_discovery;
    std::unique_ptr<MessageSearchController> m_messageSearch;
    std::unique_ptr<UiaController> m_uia;
    std::unique_ptr<ModerationController> m_moderation;
    std::unique_ptr<ForwardController> m_forward;
    // Media keys this account asked to star or copy; the shared media fetch
    // result must be claimed. Cleared on sign-out.
    QSet<QString> m_pendingStarKeys;
    QSet<QString> m_pendingCopyKeys;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<SpaceModerationController> m_spaceModeration;
    std::unique_ptr<RoomClosureController> m_roomClosure;
    std::unique_ptr<MediaBridge> m_mediaBridge;
    std::unique_ptr<AccountAvatarStore> m_accountAvatars;
    std::unique_ptr<MediaVisibilityStore> m_mediaVisibility;
    std::unique_ptr<VoiceRecorder> m_voiceRecorder; // lazy — see getter
    QString m_voiceOwner;                           // "", "room", "thread"
    // Re-issue push-rule writes that failed offline, once per genuine
    // transition into Syncing. Never clears the failure set itself.
    void retryFailedNotificationModes();
    // MatrixClient::ConnectionState as an int (forward-declared here); -1 =
    // none seen yet, so the first transition into Syncing counts.
    int m_lastConnectionState = -1;
    std::unique_ptr<MediaPlaybackController> m_playback;
    std::unique_ptr<PaginationController> m_pagination;
    std::unique_ptr<ReadReceiptCoordinator> m_readReceipts;
    std::unique_ptr<LinkPreviewController> m_linkPreviews;
    std::unique_ptr<MatrixGifTransport> m_gifTransport;
    std::unique_ptr<GifSearchController> m_gif;
    std::unique_ptr<lightning::update::UpdateManager> m_updateManager;
    std::unique_ptr<GifSendController> m_gifSend;
    std::unique_ptr<StickerPackManager> m_stickers;
    std::unique_ptr<TimelineScrollController> m_timelineScroll;
    std::unique_ptr<TimelineScrollController> m_threadScroll;

    // Show-QR verification: the grid lives in the store (memory only); this
    // keeps the URL token and progress flags. Every flow-ending path calls
    // clearVerificationQr().
    QrCodeStore m_qrCodeStore;
    std::unique_ptr<QrLoginController> m_qrLogin;
    std::unique_ptr<PolicyListController> m_policy;
    StagedImageStore m_stagedImages;
    ImageCropper m_imageCrop;
    TrayIcon m_tray;
    SpellChecker m_spell;
    // Push unread state to the tray from the local room snapshot; never
    // issues requests.
    void refreshTrayUnread();
    // Coalesced to one pass per event-loop turn: rooms() copies the whole
    // snapshot and roomUpdated fires per room.
    QTimer m_trayUnreadCoalesce;
    void refreshTrayState();
    // Computed once in the constructor; see restorableWindowGeometry().
    QRect m_restorableWindowGeometry;
    QString m_verificationQrToken;
    bool m_verificationQrScanned = false;
    bool m_verificationQrConfirming = false;
    void clearVerificationQr();

    // SAS verification state cache.
    QString m_verificationFlowId;
    QString m_verificationOtherUser;
    QString m_verificationOtherDevice;
    bool    m_verificationIsSelf = false;
    QString m_verificationState;
    QVariantList m_verificationEmojis;
    QVariantList m_verificationDecimals;

    // Tri-state latch and re-check rate limit; "unknown" never becomes
    // "broken" (see OwnDeviceKeyWatch).
    matrix::crypto::OwnDeviceKeyWatch m_ownDeviceKeyWatch;
    // Periodic re-check while signed in; stops once the fault is latched,
    // since it cannot recover without a new session.
    QTimer m_ownDeviceKeyTimer;

    // Security & Recovery cache.
    QString m_sessionTrustState = QStringLiteral("Unknown");
    QString m_sessionDeviceId;
    bool    m_ownIdentityAvailable = false;
    bool    m_crossSigningAvailable = false;

    // Room-key import state: "" (idle), "importing", "done" or "failed".
    QString m_roomKeyImportState;
    int     m_roomKeyImportImported = 0;
    int     m_roomKeyImportTotal = 0;
    int     m_roomKeyImportAffected = 0;
    QString m_roomKeyImportMessage;
    bool    m_roomKeyImportRunning = false;
    QStringList m_roomKeyImportAffectedRoomIds;
};
