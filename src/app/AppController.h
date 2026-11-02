#pragma once

#include "app/ConversationController.h"
#include "app/RoomDiscoveryController.h"
#include "app/DraftStore.h"
#include "app/ForwardController.h"
#include "app/BulkRedactionController.h"
#include "app/ModerationController.h"
#include "app/NotificationTroubleshooter.h"
#include "app/PushRulesController.h"
#include "app/UiaController.h"
#include "app/PasswordChangeController.h"
#include "app/RoomKeyExportController.h"
#include "models/MessageSearchController.h"
#include "models/RoomExport.h"
#include "models/RoomExportController.h"
#include "models/WidgetController.h"
#include "app/RoomInfoController.h"
#include "app/RoomBatchController.h"
#include "app/SettingsManager.h"
#include "app/CustomThemeStore.h"
#include "app/NetworkRouteController.h"
#include "media/MediaVisibilityStore.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "i18n/LocalizationManager.h"
#include "app/GlobalShortcutsPortal.h"
#include "app/ShortcutRegistry.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoBootstrapModel.h"
#include "crypto/CryptoHealthModel.h"
#include "crypto/CryptoManager.h"
#include "crypto/QrImageProvider.h"
#include "app/AboutController.h"
#include "app/PolicyListController.h"
#include "crypto/QrLoginController.h"
#include "crypto/OwnDeviceKeyWatch.h"
#include "storage/AppDataPaths.h"
#include "storage/BridgeLabelStore.h"
#include "app/CloseBehavior.h"
#include "app/TrayIcon.h"
#include "notifications/InviteNoticeTracker.h"
#include "notifications/NotificationManager.h"
#include "accounts/RevocationProofs.h"
#include "models/AccountActivityModel.h"
#include "models/AccountListModel.h"
#include "app/NotificationNotices.h"
#include "text/SpellChecker.h"

#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>
#include "profile/NameColorManager.h"
#include "profile/ProfileBannerManager.h"
#include "backdrop/ChatBackdropController.h"
#include "settings/SettingsSyncController.h"
#include "profile/ProfileBadges.h"
#include "profile/ProfileBioManager.h"
#include "profile/UserProfileResolver.h"
#include "app/AccountAvatarStore.h"
#include "media/MediaBridge.h"
#include "app/DownloadsController.h"
#include "app/FileChooser.h"
#include "app/LogExportController.h"
#include "app/FileLauncher.h"
#include "media/StagedImageStore.h"
#include "media/ImageCropper.h"
#include "media/MediaPlaybackController.h"
#include "media/VoicePlaybackController.h"
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
#include "models/DirectAvatarResolver.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "models/TimelineModel.h"
#include "app/PinnedMessagesController.h"
#include "app/SavedMessagesController.h"
#include "app/RoomUpgradeController.h"
#include "app/OpenRoomPermissions.h"
#include "models/TimelineScrollController.h"
#include "spaces/SpaceManager.h"
#include "spaces/RoomClosureController.h"
#include "spaces/SpaceModerationController.h"
#include "threads/ThreadController.h"
#include "models/ContextController.h"
#include "calls/CallController.h"
#include "calls/CallLanePolicy.h"
#include "calls/RtcController.h"
#include "calls/AudioDeviceTester.h"
#include "calls/CallDeviceController.h"
#include "calls/SfuCallController.h"
#include "calls/CallSoundController.h"
#include "presence/PresenceManager.h"
#include "presence/GameActivity.h"
#include "presence/MusicActivity.h"
#include "update/UpdateManager.h"
#include "threads/ThreadManager.h"

#include <QFont>
#include <QObject>
#include <QRect>
#include <QString>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <memory>

#include "RoomHistory.h"

namespace lightning {
class MouseNavigationFilter;
}

class MatrixClient;
class MprisServer;
class NotificationManager;
namespace lightning::accounts {
class SessionPool;
}
class QQuickItem;
class QQuickWindow;
class QSettings;
class QWindow;
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
    Q_PROPERTY(QString appIconSource READ appIconSource NOTIFY appIconChanged)
    Q_PROPERTY(bool screenshotDemoActive READ screenshotDemoActive CONSTANT)
    Q_PROPERTY(QObject* demo READ demoController CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    Q_PROPERTY(bool serverRoomNotificationModes READ serverRoomNotificationModes
               CONSTANT)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncModeLabel READ syncModeLabel NOTIFY syncModeChanged)
    Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged)
    // True when the scene graph runs on Qt Quick's software backend, which
    // cannot draw video. Set once from main.cpp from the actual renderer
    // interface; false until then.
    Q_PROPERTY(bool softwareRenderer READ softwareRenderer
               NOTIFY softwareRendererChanged)
    Q_PROPERTY(bool softwareRendererNoticeVisible
               READ softwareRendererNoticeVisible
               NOTIFY softwareRendererNoticeVisibleChanged)
    Q_PROPERTY(QString softwareRendererNoticeContext
               READ softwareRendererNoticeContext
               NOTIFY softwareRendererNoticeVisibleChanged)
    Q_PROPERTY(bool animationsSuspended READ animationsSuspended
               NOTIFY animationsSuspendedChanged)
    Q_PROPERTY(bool initialSyncDone READ initialSyncDone NOTIFY initialSyncDoneChanged)
    Q_PROPERTY(bool localRustResetRequired READ localRustResetRequired
               NOTIFY localRustResetRequiredChanged)
    Q_PROPERTY(bool signInErrorExplainedByCard READ signInErrorExplainedByCard
               NOTIFY signInErrorExplainedByCardChanged)
    Q_PROPERTY(QString localSessionFailureReasonCode
               READ localSessionFailureReasonCode
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureUserId
               READ localSessionFailureUserId
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureHomeserver
               READ localSessionFailureHomeserver
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QStringList accountRemovalLeftovers
               READ accountRemovalLeftovers
               NOTIFY localSessionFailureChanged)
    Q_PROPERTY(bool accountSwitching READ accountSwitching
               NOTIFY accountSwitchingChanged)
    // A saved account's sign-in cannot be read because the secret store says
    // it cannot answer (no Secret Service on the bus, a store that failed to
    // open, a lookup that errors): the login screen says so instead of
    // looking signed out, and offers retryKeyring(). NOT detected: an unlock
    // prompt the user dismissed, which libsecret reports as "no such item",
    // exactly like an empty keyring. Never true where no native store exists
    // at all, since the plaintext store is the real one there (CLAUDE.md §6).
    Q_PROPERTY(bool keyringUnavailable READ keyringUnavailable
               NOTIFY keyringUnavailableChanged)
    Q_PROPERTY(QString mediaKeepState READ mediaKeepState
               NOTIFY mediaKeepStateChanged)

    Q_PROPERTY(bool canEditOwnDisplayName READ canEditOwnDisplayName
               NOTIFY loggedInChanged)
    Q_PROPERTY(bool ownDisplayNameBusy READ ownDisplayNameBusy
               NOTIFY ownDisplayNameStateChanged)
    Q_PROPERTY(QString ownDisplayNameError READ ownDisplayNameError
               NOTIFY ownDisplayNameStateChanged)
    Q_PROPERTY(bool ownAvatarBusy READ ownAvatarBusy
               NOTIFY ownAvatarStateChanged)
    Q_PROPERTY(QString ownAvatarError READ ownAvatarError
               NOTIFY ownAvatarStateChanged)

    Q_PROPERTY(QString rustDeviceIdRedacted READ rustDeviceIdRedacted NOTIFY rustDeviceIdChanged)

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
    Q_PROPERTY(bool sessionVerificationNeeded READ sessionVerificationNeeded
                   NOTIFY securityStateChanged)
    Q_PROPERTY(bool sessionVerificationWarning READ sessionVerificationWarning
                   NOTIFY sessionVerificationWarningChanged)

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

    Q_PROPERTY(bool verificationActive READ verificationActive NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationFlowId READ verificationFlowId NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherUser READ verificationOtherUser NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationOtherDevice READ verificationOtherDevice NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationIsSelfVerification READ verificationIsSelfVerification NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationState READ verificationState NOTIFY verificationStateChanged)
    // Wording only, never trust: which side the flow verifies, read when it
    // started ("this_session", "other_session", "other_user", or "" when
    // this session's state was undetermined), and why a "cancelled" flow
    // ended ("mismatch", "user", "timeout", or "").
    Q_PROPERTY(QString verificationRole READ verificationRole NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationCancelReason READ verificationCancelReason NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationEmojis READ verificationEmojis NOTIFY verificationStateChanged)
    Q_PROPERTY(QVariantList verificationDecimals READ verificationDecimals NOTIFY verificationStateChanged)

    Q_PROPERTY(bool verificationQrAvailable READ verificationQrAvailable NOTIFY verificationStateChanged)
    Q_PROPERTY(QString verificationQrImage READ verificationQrImage NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrScanned READ verificationQrScanned NOTIFY verificationStateChanged)
    Q_PROPERTY(bool verificationQrConfirming READ verificationQrConfirming NOTIFY verificationStateChanged)

    Q_PROPERTY(SettingsManager* settings READ settings CONSTANT)
    Q_PROPERTY(ShortcutRegistry* shortcuts READ shortcuts CONSTANT)
    Q_PROPERTY(GlobalShortcutsPortal* globalShortcuts READ globalShortcuts CONSTANT)
    Q_PROPERTY(LocalizationManager* localization READ localization CONSTANT)
    Q_PROPERTY(CustomThemeStore* customTheme READ customTheme CONSTANT)
    Q_PROPERTY(NetworkRouteController* networkRoute READ networkRoute CONSTANT)
    Q_PROPERTY(RailLayoutStore* railLayout READ railLayout CONSTANT)
    Q_PROPERTY(RailEntryModel* railEntries READ railEntries CONSTANT)
    Q_PROPERTY(ProfileBannerManager* banners READ banners CONSTANT)
    Q_PROPERTY(ChatBackdropController* backdrops READ backdrops CONSTANT)
    Q_PROPERTY(SettingsSyncController* settingsSync READ settingsSync CONSTANT)
    Q_PROPERTY(NameColorManager* nameColors READ nameColors CONSTANT)
    Q_PROPERTY(ProfileBioManager* bio READ bio CONSTANT)
    Q_PROPERTY(UserProfileResolver* userProfiles READ userProfiles CONSTANT)
    Q_PROPERTY(ProfileBadges* badges READ badges CONSTANT)
    Q_PROPERTY(ImageCropper* imageCrop READ imageCrop CONSTANT)
    Q_PROPERTY(AuthManager* auth READ auth CONSTANT)
    Q_PROPERTY(AccountManager* accounts READ accounts CONSTANT)
    Q_PROPERTY(bool liveAccountsLabsAvailable READ liveAccountsLabsAvailable
               CONSTANT)
    Q_PROPERTY(bool liveAccountsActive READ liveAccountsActive
               NOTIFY liveAccountsActiveChanged)
    Q_PROPERTY(AccountActivityModel* accountActivity READ accountActivity
               NOTIFY liveAccountsActiveChanged)
    Q_PROPERTY(AccountListModel* accountList READ accountList
               NOTIFY liveAccountsActiveChanged)
    Q_PROPERTY(int liveAccountCount READ liveAccountCount
               NOTIFY liveAccountCountChanged)
    Q_PROPERTY(int maxLiveAccounts READ maxLiveAccounts CONSTANT)
    Q_PROPERTY(int maxPinnedAccounts READ maxPinnedAccounts CONSTANT)
    Q_PROPERTY(RoomListModel* roomList READ roomList CONSTANT)
    Q_PROPERTY(RoomBatchController* roomBatch READ roomBatch CONSTANT)
    Q_PROPERTY(RoomListModel* allRooms READ allRooms CONSTANT)
    Q_PROPERTY(SpaceChannelModel* spaceChannels READ spaceChannels CONSTANT)
    Q_PROPERTY(WidgetController* widgets READ widgets CONSTANT)
    Q_PROPERTY(QuickSwitcherModel* quickSwitcher READ quickSwitcher CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineView READ timelineView CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    Q_PROPERTY(QObject* richComposer READ richComposer CONSTANT)
    Q_PROPERTY(QObject* spell READ spellChecker CONSTANT)
    // Whether a system tray exists; QML hides the tray settings otherwise.
    // NOT constant: a StatusNotifierWatcher (Linux tray host) can appear or
    // go away while Lightning is running, so this is re-checked and the
    // property re-evaluated rather than frozen at the first read.
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)
    Q_PROPERTY(QRect restorableWindowGeometry READ restorableWindowGeometry
                   CONSTANT)
    Q_PROPERTY(MentionSuggestionModel* mentionSuggestions READ mentionSuggestions
                   CONSTANT)
    Q_PROPERTY(EmojiCatalog* emojiCatalog READ emojiCatalog CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    Q_PROPERTY(QrLoginController* qrLogin READ qrLogin CONSTANT)
    Q_PROPERTY(PolicyListController* policy READ policy CONSTANT)
    Q_PROPERTY(AboutController* about READ about CONSTANT)
    Q_PROPERTY(CryptoHealthModel* cryptoHealth READ cryptoHealth CONSTANT)
    Q_PROPERTY(QObject* backup READ backup CONSTANT)
    Q_PROPERTY(QObject* accountPrefs READ accountPrefs CONSTANT)
    Q_PROPERTY(QObject* scheduledSends READ scheduledSends CONSTANT)
    Q_PROPERTY(QObject* activity READ activity CONSTANT)
    Q_PROPERTY(QObject* mediaHistory READ mediaHistory CONSTANT)
    Q_PROPERTY(CryptoBootstrapModel* cryptoBootstrap READ cryptoBootstrap CONSTANT)
    Q_PROPERTY(QVariantList sessionDevices READ sessionDevices NOTIFY sessionDevicesChanged)
    Q_PROPERTY(bool activeRoomAtLatest READ activeRoomAtLatest
                   WRITE setActiveRoomAtLatest NOTIFY activeRoomAtLatestChanged)
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
    Q_PROPERTY(GameActivity* gameActivity READ gameActivity CONSTANT)
    Q_PROPERTY(MusicActivity* musicActivity READ musicActivity CONSTANT)
    Q_PROPERTY(CallController* calls READ calls CONSTANT)
    Q_PROPERTY(RtcController* rtc READ rtc CONSTANT)
    Q_PROPERTY(SfuCallController* groupCall READ groupCall CONSTANT)
    Q_PROPERTY(CallDeviceController* callDevices READ callDevices CONSTANT)
    Q_PROPERTY(CallSoundController* callSounds READ callSounds CONSTANT)
    Q_PROPERTY(AudioDeviceTester* audioTester READ audioTester CONSTANT)
    Q_PROPERTY(PinnedMessagesController* pinned READ pinned CONSTANT)
    Q_PROPERTY(SavedMessagesController* savedMessages READ savedMessages CONSTANT)
    Q_PROPERTY(RoomUpgradeController* roomUpgrade READ roomUpgrade CONSTANT)
    Q_PROPERTY(OpenRoomPermissions* openRoomPermissions
                   READ openRoomPermissions CONSTANT)
    Q_PROPERTY(ThreadController* thread READ thread CONSTANT)
    Q_PROPERTY(ContextController* eventContext READ eventContext CONSTANT)
    Q_PROPERTY(ConversationController* conversations READ conversations CONSTANT)
    Q_PROPERTY(RoomDiscoveryController* discovery READ discovery CONSTANT)
    Q_PROPERTY(MessageSearchController* messageSearch READ messageSearch CONSTANT)
    Q_PROPERTY(UiaController* uia READ uia CONSTANT)
    Q_PROPERTY(PasswordChangeController* passwordChange READ passwordChange CONSTANT)
    Q_PROPERTY(RoomKeyExportController* roomKeyExport READ roomKeyExport CONSTANT)
    Q_PROPERTY(RoomExportController* roomExport READ roomExport CONSTANT)
    Q_PROPERTY(ModerationController* moderation READ moderation CONSTANT)
    Q_PROPERTY(PushRulesController* pushRules READ pushRules CONSTANT)
    Q_PROPERTY(NotificationTroubleshooter* notificationTroubleshooter
                   READ notificationTroubleshooter CONSTANT)
    Q_PROPERTY(ForwardController* forward READ forward CONSTANT)
    Q_PROPERTY(BulkRedactionController* bulkRedaction READ bulkRedaction CONSTANT)
    Q_PROPERTY(RoomInfoController* roomInfo READ roomInfo CONSTANT)
    Q_PROPERTY(SpaceModerationController* spaceModeration
                   READ spaceModeration CONSTANT)
    Q_PROPERTY(RoomClosureController* roomClosure READ roomClosure CONSTANT)
    Q_PROPERTY(MediaBridge* mediaBridge READ mediaBridge CONSTANT)
    Q_PROPERTY(FileChooser* files READ files CONSTANT)
    Q_PROPERTY(LogExportController* logExport READ logExport CONSTANT)
    Q_PROPERTY(DownloadsController* downloads READ downloads CONSTANT)
    Q_PROPERTY(AccountAvatarStore* accountAvatars READ accountAvatars CONSTANT)
    Q_PROPERTY(MediaVisibilityStore* mediaVisibility READ mediaVisibility
                   CONSTANT)
    Q_PROPERTY(VoiceRecorder* voiceRecorder READ voiceRecorder CONSTANT)
    Q_PROPERTY(QString voiceOwner READ voiceOwner NOTIFY voiceOwnerChanged)
    Q_PROPERTY(MediaPlaybackController* playback READ playback CONSTANT)
    Q_PROPERTY(VoicePlaybackController* voicePlayback READ voicePlayback
                   CONSTANT)
    Q_PROPERTY(PaginationController* pagination READ pagination CONSTANT)
    Q_PROPERTY(ReadReceiptCoordinator* readReceipts READ readReceipts CONSTANT)
    Q_PROPERTY(LinkPreviewController* linkPreviews READ linkPreviews CONSTANT)
    Q_PROPERTY(GifSearchController* gif READ gif CONSTANT)
    Q_PROPERTY(GifSendController* gifSend READ gifSend CONSTANT)
    Q_PROPERTY(StickerPackManager* stickers READ stickers CONSTANT)
    Q_PROPERTY(lightning::update::UpdateManager* updateManager READ updateManager CONSTANT)
    Q_PROPERTY(TimelineScrollController* timelineScroll READ timelineScroll CONSTANT)
    Q_PROPERTY(TimelineScrollController* threadScroll READ threadScroll CONSTANT)

public:
    enum Screen {
        LoginScreen = 0,
        MainScreen = 1,
        SettingsScreen = 2,
        BootScreen = 3,
    };
    Q_ENUM(Screen)

    enum Backend {
        HttpBackend = 0,
        MockBackend = 1,
        RustBackend = 2,
    };
    Q_ENUM(Backend)

    static bool isBackendCompiled(Backend backend);

    explicit AppController(Backend backend = HttpBackend,
                           bool screenshotDemo = false,
                           QObject *parent = nullptr);
    using SecretStoreFactory =
        std::function<std::unique_ptr<SecretStore>(QObject *parent)>;
    AppController(Backend backend, bool screenshotDemo,
                  SecretStoreFactory secretStoreFactory,
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
    Q_INVOKABLE void resetCustomAppIcon();

    Q_INVOKABLE static QRect centredWindowRect(int width, int height);

    static bool windowGeometryIsReachable(const QRect &geometry);
    static QRect fitWindowGeometry(const QRect &geometry);
    static QRect fitWindowGeometry(const QRect &geometry,
                                   const QList<QRect> &screenAreas);

    Q_INVOKABLE void noteWindowPlacement(const QString &how, int x, int y,
                                         int width, int height) const;
    QString appIconSource() const;
    bool screenshotDemoActive() const { return m_screenshotDemoActive; }
    // Development-only: enter demo mode and auto-login the demo account. A
    // no-op unless the mock backend is active.
    void beginScreenshotDemo(const QString &initialAccount = QString());
    void applyDemoLaunchOptions(const QString &scenario, const QString &theme,
                                const QString &appearance, const QString &size,
                                bool hideControls);
    QObject *demoController() const { return m_demoController; }
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    void stageDemoVerification(const QString &otherDevice,
                               const QVariantList &emojis);
#endif
    QString backendName() const;
    bool serverRoomNotificationModes() const;
    QString connectionStatus() const { return m_connectionStatus; }
    QString syncModeLabel() const;
    bool systemDarkMode() const;
    bool softwareRenderer() const { return m_softwareRenderer; }
    void setSoftwareRenderer(bool software);
    bool softwareRendererNoticeVisible() const
    {
        return m_softwareRendererNoticeVisible;
    }
    void setGlRenderer(const QString &glRenderer);
    Q_INVOKABLE void dismissSoftwareRendererNotice();
    QString softwareRendererNoticeContext() const
    {
        return m_softwareRendererNoticeContext;
    }
    bool animationsSuspended() const { return m_animationsSuspended; }
    bool initialSyncDone() const;
    QString rustDeviceIdRedacted() const;
    bool localRustResetRequired() const { return m_localRustResetRequired; }
    // Public (not Q_INVOKABLE) so C++ tests can set the state; the real
    // source is a Rust-backend rejection a mock test cannot produce.
    void setLocalSessionFailure(const QString &reasonCode,
                                const QString &userId,
                                const QString &homeserver);
    void clearLocalSessionFailure()
    { setLocalSessionFailure(QString{}, QString{}, QString{}); }
    bool signInErrorExplainedByCard() const;
    QString localSessionFailureReasonCode() const
    { return m_localSessionFailureReason; }
    QString localSessionFailureUserId() const
    { return m_localSessionFailureUserId; }
    QString localSessionFailureHomeserver() const
    { return m_localSessionFailureHomeserver; }
    QStringList accountRemovalLeftovers() const { return m_removalLeftovers; }
    bool accountSwitching() const { return m_accountSwitching; }
    bool keyringUnavailable() const { return m_keyringUnavailable; }
    QString mediaKeepState() const;

    SettingsManager *settings() const;
    ShortcutRegistry *shortcuts() const;
    GlobalShortcutsPortal *globalShortcuts() const { return m_globalShortcuts.get(); }
    LocalizationManager *localization() const;
    CustomThemeStore *customTheme() const;
    NetworkRouteController *networkRoute() const;
    RailLayoutStore *railLayout() const;
    RailEntryModel *railEntries() const;
    ProfileBannerManager *banners() const;
    ChatBackdropController *backdrops() const { return m_backdrops.get(); }
    SettingsSyncController *settingsSync() const { return m_settingsSync.get(); }
    NameColorManager *nameColors() const;
    ProfileBioManager *bio() const;
    UserProfileResolver *userProfiles() const { return m_userProfiles.get(); }
    ProfileBadges *badges() const;
    ImageCropper *imageCrop() { return &m_imageCrop; }
    AuthManager *auth() const;
    AccountManager *accounts() const;
    bool liveAccountsLabsAvailable() const
    { return m_liveAccountsLabsAvailable; }
    bool liveAccountsActive() const { return m_sessionPool != nullptr; }
    AccountActivityModel *accountActivity() const
    { return m_accountActivity.get(); }
    AccountListModel *accountList() const { return m_accountList.get(); }
    int liveAccountCount() const { return m_liveAccountCount; }
    int maxLiveAccounts() const;
    int maxPinnedAccounts() const;
    Q_INVOKABLE QString setAccountKeepRunning(const QString &userId, bool on);
    RoomListModel *roomList() const;
    RoomBatchController *roomBatch() const { return m_roomBatch.get(); }
    Q_INVOKABLE int currentKeyboardModifiers() const;
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
    AboutController *about() const { return m_about.get(); }
    CryptoHealthModel *cryptoHealth() const { return m_cryptoHealth.get(); }
    QObject *backup() const;
    QObject *accountPrefs() const;
    QObject *scheduledSends() const;
    QObject *activity() const;
    QObject *mediaHistory() const;
    CryptoBootstrapModel *cryptoBootstrap() const
    { return m_cryptoBootstrap.get(); }
    Q_INVOKABLE void refreshCryptoHealth();
    QVariantList sessionDevices() const { return m_sessionDevices; }
    bool sessionDevicesLoading() const { return m_sessionDevicesLoading; }
    bool sessionDevicesFailed() const { return m_sessionDevicesFailed; }
    Q_INVOKABLE void refreshSessionDevices();
    Q_INVOKABLE bool clearStoredMedia();
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
    GameActivity *gameActivity() const;
    MusicActivity *musicActivity() const;
    CallController *calls() const;
    Q_INVOKABLE bool startCall(const QString &roomId, bool withVideo = false);
    Q_INVOKABLE bool canStartCall(const QString &roomId) const;
    QString legacyCallPeer(const QString &roomId) const;
    Q_INVOKABLE QVariantMap callFormats(const QString &roomId) const;
    Q_INVOKABLE QString preferredCallLane(const QString &roomId) const;
    int callGateRevision() const { return m_callGateRevision; }
    RtcController *rtc() const;
    SfuCallController *groupCall() const;
    CallDeviceController *callDevices() const;
    CallSoundController *callSounds() const;
    AudioDeviceTester *audioTester() const;
    void enableCallSounds();
    void enableCallMediaEngine();

private:

    void applyPrivacyPreferences();
    void applyThreadStyle(bool reopen);
    void applyReceiptThreading();
    void applyStrictDeviceTrust();
    void applyKeepMediaOnDevice();
    bool notificationActionIsForCurrentAccount(const QString &accountUserId);
    void copyImageBytesToClipboard(const QString &mediaKey, bool ok,
                                   const QByteArray &bytes,
                                   const QString &category);

public:
    QString restoreRoomIdForTest() const { return m_restoreRoomId; }
    void setRestoreWindowMsForTest(int ms) { m_restoreWindowMs = ms; }
    void armRestoreForTest(const QString &roomId) { armRestoreRoom(roomId); }
    NotificationManager *notificationsForTest() const
    { return m_notifications.get(); }
    lightning::accounts::SessionPool *sessionPoolForTest() const
    { return m_sessionPool.get(); }
    const lightning::accounts::RevocationProofs &revocationProofsForTest() const
    { return m_revocationProofs; }
    QStringList liveAccountsStepsForTest() const { return m_liveAccountsSteps; }
    void setLiveAccountsQuitBudgetMsForTest(int ms)
    { m_liveAccountsQuitBudgetMs = ms; }
    void setParkedOpenTimeoutMsForTest(int ms) { m_parkedOpenTimeoutMs = ms; }
    bool hasParkedOpenForTest() const { return m_parkedOpen.has_value(); }
    PinnedMessagesController *pinned() const { return m_pinned.get(); }
    SavedMessagesController *savedMessages() const { return m_savedMessages.get(); }
    RoomUpgradeController *roomUpgrade() const { return m_roomUpgrade.get(); }
    OpenRoomPermissions *openRoomPermissions() const
    { return m_openRoomPermissions.get(); }
    ThreadController *thread() const { return m_thread.get(); }
    ContextController *eventContext() const { return m_eventContext.get(); }
    ConversationController *conversations() const { return m_conversations.get(); }
    RoomDiscoveryController *discovery() const { return m_discovery.get(); }
    MessageSearchController *messageSearch() const { return m_messageSearch.get(); }
    UiaController *uia() const { return m_uia.get(); }
    PasswordChangeController *passwordChange() const
    {
        return m_passwordChange.get();
    }
    RoomKeyExportController *roomKeyExport() const
    {
        return m_roomKeyExport.get();
    }
    RoomExportController *roomExport() const { return m_roomExport.get(); }
    ModerationController *moderation() const { return m_moderation.get(); }
    PushRulesController *pushRules() const { return m_pushRules.get(); }
    NotificationTroubleshooter *notificationTroubleshooter() const
    { return m_notificationTroubleshooter.get(); }
    ForwardController *forward() const { return m_forward.get(); }
    BulkRedactionController *bulkRedaction() const { return m_bulkRedaction.get(); }
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
    FileChooser *files() const { return m_fileChooser.get(); }
    LogExportController *logExport() const { return m_logExport.get(); }
    DownloadsController *downloads() const { return m_downloads.get(); }
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
    Q_INVOKABLE bool startVoiceRecording(const QString &owner);
    Q_INVOKABLE bool voiceRecordingBusy() const;
    void setVoiceRecorderForTest(VoiceRecorder *recorder);
    Q_INVOKABLE void endVoiceRecording();
    Q_INVOKABLE void cancelVoiceRecording();
    // Delete a finalized recording the user chose not to send. Only deletes
    // a file the recorder itself produced, never an arbitrary QML path.
    Q_INVOKABLE bool discardPreparedVoice(const QString &localPath);
    MediaPlaybackController *playback() const { return m_playback.get(); }
    VoicePlaybackController *voicePlayback() const
    {
        return m_voicePlayback.get();
    }
    QString voicePrefetchKey() const { return m_voicePrefetchKey; }
    int voicePrefetchDropsForTest() const { return m_voicePrefetchDrops; }
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
    bool showMain();
    void showSettings();
    void openRoom(const QString &roomId);
    void openMatrixLink(const QString &link) { Q_EMIT matrixLinkRequested(link); }

    void openSpaceHome(const QString &spaceId);
    Q_INVOKABLE void openLobby();
    Q_INVOKABLE bool navigateBack();
    Q_INVOKABLE bool navigateRoomBack();
    Q_INVOKABLE bool navigateRoomForward();
    Q_INVOKABLE void recordSearchResult(const QString &roomId,
                                      const QString &eventId);
    Q_INVOKABLE bool navigateRoomHistoryFromKeyboard(QQuickItem *within,
                                                     bool forward);
    Q_INVOKABLE void installMouseNavigation(QQuickItem *within);
    Q_INVOKABLE void installSettingsNavigation(QQuickItem *within);
    Q_INVOKABLE bool hasOpenPopup(QQuickItem *within) const;
    void clearRoomHistoryForTest() { m_roomHistory.clear(); }
    Q_INVOKABLE bool dismissPopupForEscape(QQuickItem *within);
    Q_INVOKABLE void requestComposerFocus(const QString &roomId);

    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    Q_INVOKABLE void setSpaceMuted(const QString &spaceId, bool mute);
    Q_INVOKABLE bool spaceIsMuted(const QString &spaceId) const;
    Q_INVOKABLE void markSpaceRead(const QString &spaceId);
    Q_INVOKABLE void registerMainWindow(QObject *window);
    static bool focusBelongsToMainWindow(const QWindow *main,
                                         const QWindow *focus);
    Q_INVOKABLE quint64 jumpToDate(qint64 timestampMs);

    Q_INVOKABLE int exportableMessageCount() const;
    Q_INVOKABLE QString suggestedExportFileName(const QString &format) const;
    Q_INVOKABLE QString startRoomExport(const QUrl &fileUrl,
                                        const QString &format,
                                        const QString &range, int count,
                                        const QString &fromDate,
                                        const QString &toDate,
                                        bool includeAttachments,
                                        int sizeLimitMb,
                                        bool includeEncryptedText);
private:
    roomexport::Options exportOptions() const;
public:
    Q_INVOKABLE void requestRoomNotificationMode(const QString &roomId);
    Q_INVOKABLE void requestRoomBridgeInfo(const QString &roomId,
                                           bool allowNetwork);
    Q_INVOKABLE bool roomNotificationModeSyncFailed(const QString &roomId) const;
    bool roomNotificationModeDecidesLocally(const QString &roomId) const;

    Q_INVOKABLE void switchToAccount(const QString &userId);

    bool canEditOwnDisplayName() const;
    bool ownDisplayNameBusy() const { return m_displayNameOp != 0; }
    QString ownDisplayNameError() const { return m_displayNameError; }
    Q_INVOKABLE int ownDisplayNameMaxLength() const { return 255; }
    Q_INVOKABLE int displayNameLength(const QString &name) const;
    Q_INVOKABLE bool submitOwnDisplayName(const QString &name);
    Q_INVOKABLE bool clearOwnDisplayName();
    Q_INVOKABLE void dismissOwnDisplayNameError();

    bool ownAvatarBusy() const { return m_avatarOp != 0; }
    QString ownAvatarError() const { return m_avatarError; }
    Q_INVOKABLE bool canEditOwnAvatar() const;
    Q_INVOKABLE bool submitOwnAvatar(const QUrl &fileUrl);
    Q_INVOKABLE bool clearOwnAvatar();
    Q_INVOKABLE void dismissOwnAvatarError();

    Q_INVOKABLE void removeAccount(const QString &userId);
    Q_INVOKABLE void retryAccountRemoval();

    Q_INVOKABLE void showSettingsSection(const QString &section);
    Q_INVOKABLE QString takeRequestedSettingsSection();
    Q_INVOKABLE void requestSettingsSearchFocus();
    Q_INVOKABLE bool takeSettingsSearchFocusRequested();

    Q_INVOKABLE void applyControlPalette(const QVariantMap &roles);

    // Recovery-key restore. The key goes straight to Rust and is never
    // stored or logged. No-op on non-Rust backends; results arrive via
    // recoveryStateChanged().
    Q_INVOKABLE void requestRecoverFromBackup(const QString &recoveryKey);

    Q_INVOKABLE void resetLocalRustStore();

    // Reset for an explicit identity; QML never computes paths or deletes
    // files. The login screen uses repairLocalSession() instead.
    Q_INVOKABLE void resetLocalRustSession(const QString &homeserver,
                                           const QString &user);

    Q_INVOKABLE void repairLocalSession();

    Q_INVOKABLE bool localResetHelpsFor(const QString &reasonCode) const;

    Q_INVOKABLE bool retryKeyring();

    // Sanitized support bundle for the clipboard: versions, capabilities,
    // lifecycle state and error categories only. Never tokens, keys,
    // recovery material, message bodies, room ids or paths.
    Q_INVOKABLE QString sessionDiagnosticsText() const;
    Q_INVOKABLE void copySessionDiagnostics();

    void retryDecryptionInCurrentRoom();

    Q_INVOKABLE void reloadCurrentRoomTimeline(int limit = 30);
    Q_INVOKABLE void showThread(const QString &roomId,
                                const QString &rootEventId);
    Q_INVOKABLE void dismissStatusError();
    static int statusErrorExpiryMs(int length);
    void setStatusErrorExpiryMsForTest(int ms);

    Q_INVOKABLE bool trimHistoryAndJumpToLive();
    static bool historyTrimAllowed(bool rustBackend, bool roomOpen,
                                   bool paginationBusy, bool threadOpen,
                                   int loadedRows, int rowThreshold)
    {
        if (!rustBackend || !roomOpen)
            return false;
        if (paginationBusy)
            return false;
        if (threadOpen)
            return false;
        return loadedRows > rowThreshold;
    }
    Q_INVOKABLE int historyTrimRowThreshold() const { return 400; }

    Q_INVOKABLE void copyImageToClipboard(const QString &mediaKey);
    Q_INVOKABLE void starChatGif(const QString &mediaKey);
    Q_INVOKABLE bool isChatGifStarred(const QString &mediaKey) const;
    Q_INVOKABLE void unstarChatGif(const QString &mediaKey);

    Q_INVOKABLE void acceptVerification();
    Q_INVOKABLE void confirmVerification();
    Q_INVOKABLE void mismatchVerification();
    Q_INVOKABLE void cancelVerification();

    Q_INVOKABLE void confirmQrVerification();

    Q_INVOKABLE void startOwnVerification();

    Q_INVOKABLE void requestEncryptionKeys();

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
    QString verificationRole() const { return m_verificationRole; }
    QString verificationCancelReason() const { return m_verificationCancelReason; }
    static QString verificationCancelReasonFor(const QString &message);
    QVariantList verificationEmojis() const { return m_verificationEmojis; }
    QVariantList verificationDecimals() const { return m_verificationDecimals; }
    bool verificationQrAvailable() const { return !m_verificationQrToken.isEmpty(); }
    QString verificationQrImage() const
    {
        return m_verificationQrToken.isEmpty()
            ? QString{}
            : QStringLiteral("image://lightning-qr/") + m_verificationQrToken;
    }
    bool trayAvailable() const { return m_trayAvailable; }
    Q_INVOKABLE bool closeHidesWindow(bool quitIntended) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    Q_INVOKABLE bool activationRestoresWindow(bool hiddenByClose,
                                              bool windowHidden) const;
    Q_INVOKABLE QUrl defaultFileDialogFolder() const;
    QObject *spellChecker() { return &m_spell; }
    QRect restorableWindowGeometry() const { return m_restorableWindowGeometry; }
    bool verificationQrScanned() const { return m_verificationQrScanned; }
    bool verificationQrConfirming() const { return m_verificationQrConfirming; }

    // Owned here so it outlives the QML engine that holds the provider.
    QrCodeStore *qrCodeStore() { return &m_qrCodeStore; }
    StagedImageStore *stagedImages() { return &m_stagedImages; }

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
    void trayShowRequested();
    void storedMediaCleared(bool ok, qint64 files, qint64 bytes);
    void mediaKeepStateChanged();

    void voiceOwnerChanged();
    void roomNotificationModesRetried(int roomCount);
    void roomNotificationModesRefreshed(int roomCount);
    void roomNotificationModeSent(const QString &roomId, int mode);
    void currentScreenChanged();
    void settingsSectionRequested(const QString &section);
    void settingsDialogActionRequested(const QString &action);
    void settingsSearchFocusRequested();
    void trayAvailableChanged();
    void appIconChanged();
    void initialSyncDoneChanged();
    void accountSwitchingChanged();
    void liveAccountsActiveChanged();
    void liveAccountCountChanged();
    void keyringUnavailableChanged();
    void ownDisplayNameStateChanged();
    void ownAvatarStateChanged();
    void ownAvatarSaved();
    void applicationQuitIntended();
    void ownDisplayNameSaved();
    void currentRoomIdChanged();
    void composerFocusRequested(const QString &roomId);
    void loggedInChanged();
    void connectionStatusChanged();
    void syncModeChanged();
    void systemDarkModeChanged();
    void softwareRendererChanged();
    void softwareRendererNoticeVisibleChanged();
    void animationsSuspendedChanged();
    void errorReported(const QString &message);
    void noticeReported(const QString &message);
    void callStartRefused(const QString &message);
    void matrixLinkRequested(const QString &link);
    void rustDeviceIdChanged();
    void localRustResetRequiredChanged();
    void localSessionFailureChanged();
    void signInErrorExplainedByCardChanged();
    void recoveryStateChanged(const QString &state, const QString &message);

    void localRustStoreResetResult(bool ok, const QString &message);

    void timelinesReloadedAfterIgnoreChange(bool room, bool thread);

    void currentRoomTimelineReloaded(int totalEvents,
                                     int decryptedEvents,
                                     int undecryptableEvents);

    void verificationStateChanged();
    void sessionVerificationWarningChanged();
    void sessionDevicesChanged();
    void activeRoomAtLatestChanged();
    void activeRoomHydratingChanged();
    void copyImageFinished(bool ok, const QString &message);
    void jumpToDateFinished(quint64 opId, bool ok, const QString &category);
    void notificationOpenRequested(const QString &roomId,
                                   const QString &eventId,
                                   const QString &threadRootId);
    void roomTimelineReady(const QString &roomId);

    void roomNotificationModeSyncStateChanged(const QString &roomId);

    void securityStateChanged();
    void encryptionIdentityBrokenChanged();
    void roomKeyImportStateChanged();
    void callGateRevisionChanged();
    void roomKeyImportCompleted(int imported, int total, int affectedRooms);

private:
    QString spaceMutePriorKey(const QString &roomId) const;
    void recordSpaceMutePrior(QSettings &store, const QString &roomId,
                              int current) const;
    int spaceUnmuteMode(QSettings &store, const QString &roomId, int current,
                        int *restored) const;
    void refreshOwnProfile(qint64 minIntervalMs);
    static constexpr qint64 kOwnProfileEventRefreshMs = 2000;
    static constexpr qint64 kOwnProfileActivationRefreshMs = 60000;
    void playNextVoiceMessage(const QString &roomId,
                              const QString &afterEventId,
                              const QString &threadRootId,
                              const QVariantList &snapshot);
    QVariantList liveVoiceRun(const QString &roomId,
                              const QString &afterEventId,
                              const QString &threadRootId,
                              bool *known) const;
    QString verificationRoleAtStart(bool isSelf) const;
    void armStatusErrorExpiry(quint64 seq);
    void clearStatusError(bool keepCallNotice);
    bool statusErrorIsRunningCallNotice() const;
    void closeCurrentRoomNoLongerIn(const QString &roomId);
    bool callAcceptOffered(const QString &roomId, bool rtcLane) const;
    void routeNotificationOpen(const QString &roomId, const QString &eventId,
                               const QString &threadRootId);
    void setCurrentScreen(Screen s);
    void setConnectionStatus(const QString &s);
    void applyAppIcon();
    void onLoginSucceeded();
    void onLoggedOut();
    void endRevokedSession(const QString &userId);
    void setLocalRustResetRequired(bool required);
    void setAccountSwitching(bool switching);
    void clearQueuedAccountSwitch(const char *reason);
    void failAccountSwitch(const QString &message);

    enum class SignInState {
        Usable,
        Gone,
        Unreadable,
        KeyringLost,
    };
    SignInState signInStateFor(const QString &userId) const;
    void refreshKeyringUnavailable();
    void setKeyringUnavailable(bool unavailable);
    void explainUnrestoredLaunch();
    void noteKeyringLostSession(const QString &userId);
    // Clears every cache that must not leak across accounts. Used on account
    // change; logout clears the same state through loggedOut connections.
    void clearCrossAccountCaches();
    void armSwitchTraceFirstFrame();
    void disarmSwitchTraceFirstFrame();
    QMetaObject::Connection m_switchTraceRowsConnection;
    QMetaObject::Connection m_switchTraceResetConnection;
    QMetaObject::Connection m_switchTraceFrameConnection;

    bool resolveRemovalIdentity(const QString &userId,
                                matrix::app_data::AccountIdentity *identity) const;
    bool removeAccountLocalState(const matrix::app_data::AccountIdentity &identity,
                                 bool *refused = nullptr);
    bool removeAccountLocalStateFiles(
        const matrix::app_data::AccountIdentity &identity,
        bool *refused = nullptr);

    // ── Concurrent live accounts (Labs). Nothing here runs while
    // liveAccountsActive() is false; every caller checks m_sessionPool. ──
    // Turns live accounts on or off to match the setting, the test override
    // and the bridge.
    void applyLiveAccountsLabs();
    void startLiveAccounts();
    void stopLiveAccounts();
    void feedLiveAccountsForeground();
    void refreshLiveAccountCount();
    bool beginLiveAccountSwitch(const QString &target);
    bool claimCrossAccountOpen(const QString &accountUserId,
                               const QString &roomId, const QString &eventId,
                               const QString &threadRootId);
    void tryFireParkedOpen();
    void cancelParkedOpenFor(const QString &accountUserId);
    void removeLiveBackgroundAccount(const QString &userId);
    void finishLiveBackgroundRemoval(
        const QString &userId,
        const matrix::app_data::AccountIdentity &identity, bool resolved,
        bool storeFree, int serverSignOut);
    void onLiveSwitchReleased(const QString &target, int result);
    void abandonLiveRelease(const QString &userId);
    void endLiveSwitchWithoutSession();
    void cancelLiveSwitchRelease();
    void finishFallbackRelease(const QString &fallback, int result);
    void reopenLiveAccountActivity(const QString &userId);
    void onHostSignInExpired(const QString &userId);
    QString liveAccountLabel(const QString &userId) const;
    void noteLiveAccountsStep(const QString &step, const QString &userId);
    QStringList accountRemovalLeftoversFor(
        const matrix::app_data::AccountIdentity &identity) const;

    void requestOwnDeviceKeyCheck();
    void applyOwnDeviceKeyAgreement(matrix::crypto::KeyAgreement agreement);

    static std::unique_ptr<MatrixClient> makeClient(Backend backend,
                                                    SettingsManager *settings,
                                                    QObject *parent);

    Backend m_backend;
    int m_appIconRevision = 0;
    bool m_screenshotDemo = false;
    Screen m_currentScreen = LoginScreen;
    QString m_currentRoomId;
    lightning::RoomHistory m_roomHistory;
    bool m_roomHistoryNavigating = false;
    QString m_restoreRoomId;
    int m_restoreWindowMs = 30000;
    void armRestoreRoom(const QString &roomId);
    bool stepRoomHistory(bool forward);
    bool roomHistoryNavigationAllowed() const;
    QPointer<lightning::MouseNavigationFilter> m_mouseNavigation;
    QPointer<QQuickItem> m_settingsNavigation;
    notices::RecentIds m_notifiedEventIds{2048};
    QSet<QString> m_joinedRoomIds;
    QPointer<QQuickWindow> m_mainWindow;
    QQuickWindow *mainWindow();
    void refreshMainWindowState();
    void alertMainWindow();
public:
    Q_INVOKABLE void activateMainWindowForNotificationClick();
private:
    void handleLiveEvent(const QString &composedRoomId,
                         const TimelineEvent &event);
    NotificationManager::Context notificationContextFor(
        const QString &roomId, const TimelineEvent &event,
        bool fromThread) const;
    int noticePreviewFor(const QString &roomId) const;
    bool masterMuteIsOn() const;
    void resetSessionNotificationState();
    QSet<QString> m_memberHydratedRooms;
    QHash<QString, bool> m_bridgeReadRooms;
    BridgeLabelStore m_bridgeLabels;
    QTimer m_bridgeSaveTimer;
    QString m_requestedSettingsSection;
    bool m_settingsSearchFocusRequested = false;
    QString m_connectionStatus;
    bool m_localRustResetRequired = false;
    QString m_localSessionFailureReason;
    QString m_localSessionFailureUserId;
    QString m_localSessionFailureHomeserver;
    matrix::app_data::AccountIdentity m_removalRetryIdentity;
    QStringList m_removalLeftovers;
    bool m_resetResultPending = false;
    bool m_accountSwitching = false;
    QString m_queuedSwitchTarget;
    QString m_adoptTarget;
    quint64 m_queuedSwitchEpoch = 0;
    bool m_keyringUnavailable = false;
    bool m_revokedDetach = false;
    QString m_switchFallbackUserId;
    QString m_lastSessionUserId;
    void signOutAccountResidue(const QString &userId);
    // Removing the active account needs a server logout first, so the
    // removal finishes in onLoggedOut. The identity is resolved before the
    // logout, because sign-out removes the saved record it is keyed on.
    // Empty user id = no removal pending.
    QString m_pendingRemovalUserId;
    matrix::app_data::AccountIdentity m_pendingRemovalIdentity;
    bool m_pendingRemovalResolved = false;
    quint64 m_displayNameOp = 0;
    quint64 m_avatarOp = 0;
    quint64 m_avatarOpCounter = 0;
    QString m_avatarError;
    quint64 m_displayNameOpCounter = 0;
    QString m_displayNameError;
    bool dispatchOwnDisplayName(const QString &name);
    QString ownDisplayNameUnavailableReason() const;
    QString cachedOwnDisplayName() const;
    void retireOwnDisplayNameWrite();
    void retireOwnAvatarWrite();
    QString m_addAccountReturnTo;
    bool m_abandoningBrowserSignIn = false;
    bool m_backgroundRestore = false;
    bool m_localSessionRaiseInFlight = false;
    quint64 m_localSessionRaiseSerial = 0;
    QString m_cardExplainedError;
    quint64 m_loginFailures = 0;

    std::unique_ptr<SecretStore> m_secretStore;
    // Stores replaced by retryKeyring(). Kept alive for the life of the
    // process, never destroyed: a caller may still hold the old pointer.
    std::vector<std::unique_ptr<SecretStore>> m_retiredSecretStores;
    SecretStoreFactory m_secretStoreFactory;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<LocalizationManager> m_localization;
    std::unique_ptr<ShortcutRegistry> m_shortcuts;
    std::unique_ptr<GlobalShortcutsPortal> m_globalShortcuts;
    std::unique_ptr<CustomThemeStore> m_customTheme;
    std::unique_ptr<NetworkRouteController> m_networkRoute;
    std::unique_ptr<RailLayoutStore> m_railLayout;
    std::unique_ptr<RailEntryModel> m_railEntries;
    std::unique_ptr<ProfileBannerManager> m_banners;
    std::unique_ptr<ChatBackdropController> m_backdrops;
    std::unique_ptr<SettingsSyncController> m_settingsSync;
    std::unique_ptr<NameColorManager> m_nameColors;
    std::unique_ptr<ProfileBioManager> m_bio;
    std::unique_ptr<UserProfileResolver> m_userProfiles;
    std::unique_ptr<ProfileBadges> m_badges;
    bool m_shuttingDown = false;
    bool m_screenshotDemoActive = false;
    bool m_softwareRenderer = false;
    bool m_softwareRendererNoticeVisible = false;
    QString m_softwareRendererNoticeContext = QStringLiteral("generic");
    QObject *m_demoController = nullptr;
    lightning::accounts::RevocationProofs m_revocationProofs;
    bool m_liveAccountsLabsAvailable = false;
    qint64 m_appStartWallMs = 0;
    std::unique_ptr<AccountActivityModel> m_accountActivity;
    std::unique_ptr<AccountListModel> m_accountList;
    std::unique_ptr<lightning::accounts::SessionPool> m_sessionPool;
    QPointer<QObject> m_retiredLivePool;
    QPointer<QObject> m_retiredAccountList;
    QPointer<QObject> m_retiredAccountActivity;
    bool m_liveAccountsStopping = false;
    bool m_liveAccountsQuitBegun = false;
    QElapsedTimer m_liveAccountsQuitTimer;
    int m_liveAccountsQuitBudgetMs = 15000;
    int m_liveAccountCount = 0;
    QString m_liveAccountsForeground;
    quint64 m_liveSwitchTicket = 0;
    QString m_liveSwitchTarget;
    QString m_liveSwitchReleased;
    quint64 m_liveFallbackTicket = 0;
    QString m_liveFallbackTarget;
    struct ParkedOpen {
        QString userId;
        QString roomId;
        QString eventId;
        QString threadRootId;
        quint64 epoch = 0;
    };
    std::optional<ParkedOpen> m_parkedOpen;
    std::unique_ptr<QTimer> m_parkedOpenTimer;
    int m_parkedOpenTimeoutMs = 30000;
    struct ClaimedOpen {
        QString roomId;
        QString eventId;
        QString threadRootId;
    };
    std::optional<ClaimedOpen> m_claimedOpen;
    QSet<QString> m_liveRemovals;
    QStringList m_liveAccountsSteps;
    std::unique_ptr<MatrixClient> m_client;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AuthManager> m_auth;
    // One DM-avatar resolver for every room-list owner, so its cache and its
    // profile fetches (and the periodic refresh) are not multiplied. Declared
    // BEFORE the models that use it: they are destroyed first.
    std::unique_ptr<DirectAvatarResolver> m_directAvatars;
    std::unique_ptr<RoomListModel> m_roomList;
    std::unique_ptr<RoomBatchController> m_roomBatch;
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
    std::unique_ptr<class AccountPrefsController> m_accountPrefs;
    std::unique_ptr<class ScheduledSendController> m_scheduledSends;
    std::unique_ptr<class ActivityModel> m_activity;
    std::unique_ptr<class MediaHistoryModel> m_mediaHistory;
    bool m_activitySeeded = false;
    QTimer m_activitySeedRetryTimer;
    int m_activitySeedAttempts = 0;
    QHash<quint64, QString> m_pendingDateJumps;
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
    InviteNoticeTracker m_inviteNotices;
    QTimer m_inviteNoticeTimer;
    void processInviteNotices();
    QSet<QString> m_notificationModeRetryReads;
    QHash<QString, int> m_notificationModeWritesInFlight;
    QHash<QString, int> m_notificationModeServerSeen;
    QHash<QString, int> m_notificationModeSentModes;
    bool m_retryNotificationModesWhenSynced = false;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
    std::unique_ptr<PresenceManager> m_presence;
    std::unique_ptr<GameActivity> m_gameActivity;
    std::unique_ptr<MusicActivity> m_musicActivity;
    std::unique_ptr<CallController> m_calls;
    int m_callGateRevision = 0;
    lightning::calls::LegacyFallbackArm m_legacyFallback;
    lightning::calls::CallFailureNotice m_callNotice;
    void withdrawCallFailure();
    std::unique_ptr<RtcController> m_rtc;
    std::unique_ptr<SfuCallController> m_groupCall;
    std::unique_ptr<CallDeviceController> m_callDevices;
    std::unique_ptr<CallSoundController> m_callSounds;
    std::unique_ptr<AudioDeviceTester> m_audioTester;
    QString m_announcedCallId;
    QString m_announcedCaller;
    bool m_announcedVideo = false;
    QHash<QString, bool> m_callVideoIntent;
    notices::RingAnnounceLimiter m_ringLimiter;
    QHash<QString, quint64> m_callNoticeGeneration;
    quint64 m_callNoticeSerial = 0;
    void noteRtcSessionForRing(const QString &roomId);
    std::unique_ptr<PinnedMessagesController> m_pinned;
    std::unique_ptr<SavedMessagesController> m_savedMessages;
    std::unique_ptr<RoomUpgradeController> m_roomUpgrade;
    std::unique_ptr<OpenRoomPermissions> m_openRoomPermissions;
    std::unique_ptr<ThreadController> m_thread;
    std::unique_ptr<ContextController> m_eventContext;
    std::unique_ptr<ConversationController> m_conversations;
    std::unique_ptr<RoomDiscoveryController> m_discovery;
    std::unique_ptr<MessageSearchController> m_messageSearch;
    std::unique_ptr<UiaController> m_uia;
    std::unique_ptr<PasswordChangeController> m_passwordChange;
    std::unique_ptr<RoomKeyExportController> m_roomKeyExport;
    std::unique_ptr<RoomExportController> m_roomExport;
    std::unique_ptr<ModerationController> m_moderation;
    std::unique_ptr<PushRulesController> m_pushRules;
    std::unique_ptr<NotificationTroubleshooter> m_notificationTroubleshooter;
    std::unique_ptr<ForwardController> m_forward;
    std::unique_ptr<BulkRedactionController> m_bulkRedaction;
    QSet<QString> m_pendingStarKeys;
    QSet<QString> m_pendingCopyKeys;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<SpaceModerationController> m_spaceModeration;
    std::unique_ptr<RoomClosureController> m_roomClosure;
    std::unique_ptr<MediaBridge> m_mediaBridge;
    std::unique_ptr<FileChooser> m_fileChooser;
    std::unique_ptr<LogExportController> m_logExport;
    QString logExportDiagnostics() const;
    QStringList logExportLiterals() const;
    std::unique_ptr<FileLauncher> m_fileLauncher;
    // Declared after the bridge, chooser and launcher it points at, so it is
    // destroyed first.
    std::unique_ptr<DownloadsController> m_downloads;
    std::unique_ptr<AccountAvatarStore> m_accountAvatars;
    std::unique_ptr<MediaVisibilityStore> m_mediaVisibility;
    std::unique_ptr<VoiceRecorder> m_voiceRecorder;
    QString m_voiceOwner;
    void retryFailedNotificationModes();
    void refreshStoredNotificationModes();
    void sendRoomNotificationMode(const QString &roomId, int mode);
    void adoptServerNotificationMode(const QString &roomId, int mode);
    int m_lastConnectionState = -1;
    std::unique_ptr<MediaPlaybackController> m_playback;
    std::unique_ptr<VoicePlaybackController> m_voicePlayback;
    void updateVoicePrefetch();
    void scheduleVoicePrefetchUpdate();
    void resetVoicePrefetch();
    QString m_voicePrefetchKey;
    qint64 m_ownProfileRefreshMs = 0;
    QString m_voiceAutoplayEventId;
    bool m_voiceAutoplayStarting = false;
    int m_voiceAutoplaySkips = 0;
    static constexpr int kMaxVoiceAutoplaySkips = 3;
    bool m_voicePrefetchOwned = false;
    bool m_voicePrefetchQueued = false;
    int m_voicePrefetchDrops = 0;
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

    QrCodeStore m_qrCodeStore;
    std::unique_ptr<QrLoginController> m_qrLogin;
    std::unique_ptr<PolicyListController> m_policy;
    std::unique_ptr<AboutController> m_about;
    StagedImageStore m_stagedImages;
    ImageCropper m_imageCrop;
    TrayIcon m_tray;
    SpellChecker m_spell;
    void applyTrayState();
    void refreshTrayAccountLabel();
    int m_lastBadgeNumber = -1;
    void refreshTrayUnread();
    QTimer m_trayUnreadCoalesce;
    void refreshTrayState();
    void refreshTrayAvailability();
    bool m_trayAvailable = false;
    QTimer m_trayAvailabilityTimer;
    QRect m_restorableWindowGeometry;
    bool m_currentRoomListed = false;
    QString m_statusError;
    quint64 m_statusErrorSeq = 0;
    bool m_statusErrorOwnNavigation = false;
    QString m_callStatusNotice;
    int m_statusErrorExpiryForTestMs = 0;
    bool m_conversationsBusySeen = false;
    bool m_discoveryBusySeen = false;
    quint64 m_currentRoomOpenSerial = 0;
    QString m_verificationQrToken;
    bool m_verificationQrScanned = false;
    bool m_verificationQrConfirming = false;
    void clearVerificationQr();

    QString m_verificationFlowId;
    QString m_verificationOtherUser;
    QString m_verificationNoticeKey;
    QString m_verificationNotifiedFlow;
    QString m_verificationOtherDevice;
    bool    m_verificationIsSelf = false;
    QString m_verificationRole;
    QString m_verificationCancelReason;
    QString m_verificationState;
    QVariantList m_verificationEmojis;
    QVariantList m_verificationDecimals;

    matrix::crypto::OwnDeviceKeyWatch m_ownDeviceKeyWatch;
    QTimer m_ownDeviceKeyTimer;

    QString m_sessionTrustState = QStringLiteral("Unknown");
    QString m_sessionDeviceId;
    bool    m_ownIdentityAvailable = false;
    bool    m_crossSigningAvailable = false;

    QString m_roomKeyImportState;
    int     m_roomKeyImportImported = 0;
    int     m_roomKeyImportTotal = 0;
    int     m_roomKeyImportAffected = 0;
    QString m_roomKeyImportMessage;
    bool    m_roomKeyImportRunning = false;
    QStringList m_roomKeyImportAffectedRoomIds;

    bool m_animationsSuspended = false;
    void refreshAnimationsSuspended();
    // MPRIS for the voice/audio player. Declared last so it is destroyed
    // first, before the player it reads.
    std::unique_ptr<MprisServer> m_mpris;
};
