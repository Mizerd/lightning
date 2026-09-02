#pragma once

#include "app/ConversationController.h"
#include "app/RoomDiscoveryController.h"
#include "app/DraftStore.h"
#include "app/ForwardController.h"
#include "app/ModerationController.h"
#include "app/UiaController.h"
#include "models/MessageSearchController.h"
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
#include "app/TrayIcon.h"
#include "text/SpellChecker.h"

#include <QTimer>
#include "profile/ProfileBannerManager.h"
#include "profile/ProfileBadges.h"
#include "profile/ProfileBioManager.h"
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
#include "threads/ThreadController.h"
#include "calls/CallController.h"
#include "calls/RtcController.h"
#include "calls/CallDeviceController.h"
#include "calls/SfuCallController.h"
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
    // The colour emoji face to NAME on surfaces that draw emoji, or empty when
    // the host has none. Lives here rather than on EmojiCatalog because it
    // needs QFontDatabase (Qt6::Gui) and EmojiCatalog is linked against
    // Qt6::Core alone by its own test target -- the same reason QScreen is
    // kept out of SettingsManager.
    Q_PROPERTY(QString emojiFontFamily READ emojiFontFamily CONSTANT)
    // Current application icon for in-app branding surfaces (About, the
    // Appearance preview). Either the embedded default logo resource or a
    // file: URL to the normalized custom icon with a cache-busting revision.
    // The window/taskbar icon is applied separately via
    // QGuiApplication::setWindowIcon from the same state.
    Q_PROPERTY(QString appIconSource READ appIconSource NOTIFY appIconChanged)
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

    // v0.7.4 own display name. The NAME itself is deliberately NOT mirrored
    // here: the account registry already holds it, and the rail, the
    // account menu and the Settings identity card all refresh off
    // AccountManager::accountsChanged. A second copy would be a second
    // truth. These three carry only what the editor needs and nothing else
    // does — whether the backend can write a profile at all, whether a
    // write is in flight, and the last failure's wording.
    Q_PROPERTY(bool canEditOwnDisplayName READ canEditOwnDisplayName
               NOTIFY loggedInChanged)
    Q_PROPERTY(bool ownDisplayNameBusy READ ownDisplayNameBusy
               NOTIFY ownDisplayNameStateChanged)
    Q_PROPERTY(QString ownDisplayNameError READ ownDisplayNameError
               NOTIFY ownDisplayNameStateChanged)
    // Own AVATAR write state. Deliberately a SEPARATE op and error from the
    // display name: they are two independent requests and one failing must
    // not blank the other's message.
    Q_PROPERTY(bool ownAvatarBusy READ ownAvatarBusy
               NOTIFY ownAvatarStateChanged)
    Q_PROPERTY(QString ownAvatarError READ ownAvatarError
               NOTIFY ownAvatarStateChanged)

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
    // v0.7.x verification prompts. TRUE only for the one state the user can
    // actually act on: signed in, on a crypto-capable backend, with a
    // cross-signing identity that has NOT signed this device.
    //   * "Unknown"                    — not determined yet; warning off.
    //   * "Cross-signing unavailable"  — there is no identity to verify
    //                                    against, so "verify this session"
    //                                    would be advice that cannot be
    //                                    followed; warning off.
    // The dismissal is applied by sessionVerificationWarning (the badges),
    // never by this property — the Sessions page still states the fact.
    Q_PROPERTY(bool sessionVerificationNeeded READ sessionVerificationNeeded
                   NOTIFY securityStateChanged)
    // sessionVerificationNeeded AND the user has not dismissed the badges.
    Q_PROPERTY(bool sessionVerificationWarning READ sessionVerificationWarning
                   NOTIFY sessionVerificationWarningChanged)

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
    // Rebindable keyboard shortcuts. CONSTANT like the other controllers —
    // the OBJECT never changes; its rows announce their own changes, and
    // `bindingRevision` is what a QML Shortcut binding depends on.
    Q_PROPERTY(ShortcutRegistry* shortcuts READ shortcuts CONSTANT)
    // UI language. Application-wide, not per-account: the translators are
    // installed on QCoreApplication and the layout direction is process-wide.
    Q_PROPERTY(LocalizationManager* localization READ localization CONSTANT)
    // User-authored palette overrides (Settings -> Appearance -> Custom
    // theme). Per-account appearance state, like the theme itself.
    Q_PROPERTY(CustomThemeStore* customTheme READ customTheme CONSTANT)
    // How the Spaces rail is arranged (drag order and folders).
    // Device-local; see RailLayoutStore.
    Q_PROPERTY(RailLayoutStore* railLayout READ railLayout CONSTANT)
    // The rows the Spaces rail draws, plus live drag state; see RailEntryModel.
    Q_PROPERTY(RailEntryModel* railEntries READ railEntries CONSTANT)
    // Profile banners (MSC4427 / MSC4133), read and written under both
    // the stable and the Commet field names.
    Q_PROPERTY(ProfileBannerManager* banners READ banners CONSTANT)
    // Profile bios (MSC4440 / MSC4133), read and written under both the
    // stable and the MSC's unstable field names. Plain text only.
    Q_PROPERTY(ProfileBioManager* bio READ bio CONSTANT)
    /// Decorative thank-you badges beside a name. A fixed local table — no
    /// Matrix state, no permission, no verification claim. See ProfileBadges.
    Q_PROPERTY(ProfileBadges* badges READ badges CONSTANT)
    /// Crop / adjust for every display-image upload (avatars and banners).
    /// See ImageCropper: it is a PRE-STEP that writes a cropped temp file,
    /// so every upload sink keeps the local-path contract it already had.
    Q_PROPERTY(ImageCropper* imageCrop READ imageCrop CONSTANT)
    Q_PROPERTY(AuthManager* auth READ auth CONSTANT)
    Q_PROPERTY(AccountManager* accounts READ accounts CONSTANT)
    Q_PROPERTY(RoomListModel* roomList READ roomList CONSTANT)
    /// The Channels navigation layout's model: the active Space's DIRECT
    /// hierarchy. Separate from roomList because the two answer different
    /// questions — see SpaceChannelModel's header for why a filtered
    /// roomList cannot do this.
    Q_PROPERTY(SpaceChannelModel* spaceChannels READ spaceChannels CONSTANT)
    Q_PROPERTY(QuickSwitcherModel* quickSwitcher READ quickSwitcher CONSTANT)
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineView READ timelineView CONSTANT)
    Q_PROPERTY(MessageComposer* composer READ composer CONSTANT)
    // v0.9 rich composer: the QML-facing document bridge (serialize + send,
    // toolbar formatting, draft-only markdown conversion). Typed QObject*
    // for the same reason as the spell checker below — no QML type
    // registration needed, everything QML calls is Q_INVOKABLE.
    Q_PROPERTY(QObject* richComposer READ richComposer CONSTANT)
    /// The composer's spell checker. Typed QObject* rather than
    /// SpellChecker* on purpose: it needs no QML type registration to be
    /// used from QML, and every method QML calls on it is Q_INVOKABLE.
    /// Never null — an unavailable platform reports `available: false`, and
    /// QML draws nothing rather than being handed a null object.
    Q_PROPERTY(QObject* spell READ spellChecker CONSTANT)
    // v0.7 outgoing @-mentions: the current-room member suggestion model
    // shared by the room and thread composer mention popups.
    // Whether this session actually has a system tray to close INTO. QML
    // gates both tray settings on it rather than offering a switch that
    // would hide the window into nothing.
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
    // Where the window was last time, or an invalid rect for "do not restore".
    //
    // SettingsManager holds the stored value and validates its SIZE; this adds
    // the display-layout half, which is the half that can lose a window: a
    // geometry saved on a monitor that has since been unplugged would reopen
    // Lightning at x=2560 with nothing there to show it. CONSTANT and computed
    // once, because it answers a question about the past — re-answering it
    // mid-session (a monitor plugged in, say) would move a window the user has
    // since placed themselves.
    Q_PROPERTY(QRect restorableWindowGeometry READ restorableWindowGeometry
                   CONSTANT)
    Q_PROPERTY(MentionSuggestionModel* mentionSuggestions READ mentionSuggestions
                   CONSTANT)
    Q_PROPERTY(EmojiCatalog* emojiCatalog READ emojiCatalog CONSTANT)
    Q_PROPERTY(MediaManager* media READ media CONSTANT)
    Q_PROPERTY(CryptoManager* crypto READ crypto CONSTANT)
    // v0.6.0 checkpoint 7: read-only E2EE health/readiness (app.cryptoHealth).
    Q_PROPERTY(CryptoHealthModel* cryptoHealth READ cryptoHealth CONSTANT)
    // v0.9 (phase 9): key-backup / recovery management (app.backup).
    Q_PROPERTY(QObject* backup READ backup CONSTANT)
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
    // True while the open room's timeline is still hydrating. Suppression
    // needs this as well as activeRoomAtLatest, which cannot be true until
    // the view has settled — see NotificationManager::Context::roomHydrating.
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
    // v0.7.x pinned messages for the ACTIVE room (not the Room Information
    // panel's room): the message-action menu asks it whether the message
    // under the cursor is pinned.
    Q_PROPERTY(PinnedMessagesController* pinned READ pinned CONSTANT)
    // v0.7.x room upgrades for the ACTIVE room: the timeline banner offering
    // the successor of a tombstoned room, and the link back to a
    // predecessor. Offers only — it never follows an upgrade by itself.
    Q_PROPERTY(RoomUpgradeController* roomUpgrade READ roomUpgrade CONSTANT)
    // v0.6.0: the single open SDK-backed thread panel (app.thread).
    Q_PROPERTY(ThreadController* thread READ thread CONSTANT)
    // v0.5.9: conversation creation (DMs, rooms, invites), Room Information
    // (members, permissions, editing, leave) and the media bridge.
    Q_PROPERTY(ConversationController* conversations READ conversations CONSTANT)
    Q_PROPERTY(RoomDiscoveryController* discovery READ discovery CONSTANT)
    Q_PROPERTY(MessageSearchController* messageSearch READ messageSearch CONSTANT)
    Q_PROPERTY(UiaController* uia READ uia CONSTANT)
    Q_PROPERTY(ModerationController* moderation READ moderation CONSTANT)
    // v0.7.x message forwarding (task #14): the ONE forward-picker state
    // (app.forward). See ForwardController's class comment for the
    // non-negotiable decisions (re-upload never mxc-copy, no relation, D6
    // navigation-only-after-dispatch) this follows.
    Q_PROPERTY(ForwardController* forward READ forward CONSTANT)
    Q_PROPERTY(RoomInfoController* roomInfo READ roomInfo CONSTANT)
    Q_PROPERTY(MediaBridge* mediaBridge READ mediaBridge CONSTANT)
    // Which images the reader has hidden locally; see MediaVisibilityStore.
    Q_PROPERTY(MediaVisibilityStore* mediaVisibility READ mediaVisibility
                   CONSTANT)
    // v0.7 voice round: microphone capture for MSC3245 voice messages.
    // Created LAZILY on first access (the composer only touches it on the
    // first mic press), so the audio backend never spins up for a session
    // that never records. CONSTANT is honest: the pointer is created once
    // inside the first read and never changes afterwards.
    Q_PROPERTY(VoiceRecorder* voiceRecorder READ voiceRecorder CONSTANT)
    // v0.7 thread parity: WHICH composer owns the shared recorder — "" when
    // idle, "room" or "thread" while recording.
    //
    // There is exactly ONE VoiceRecorder for the whole application, and both
    // composers listen to its ready()/failed() signals. Before this existed
    // each composer armed those Connections off its own local flag, so a
    // recording started in the room composer and then superseded by one
    // started in the thread panel left BOTH armed — and a single ready()
    // sent the same file twice, once into the room and once into the thread.
    // Opening a thread does not change currentRoomId, so the room composer's
    // cancel-on-room-change never fired for that sequence. Making ownership
    // one authoritative value, rather than two flags that must agree, is
    // what removes the whole class of bug: a composer sends only while it is
    // the owner, and at most one composer is ever the owner.
    //
    // Ownership is NEVER transferred away from a live recording. A second
    // composer's start is refused while the recorder is busy — see
    // startVoiceRecording for why stealing it orphaned the microphone.
    Q_PROPERTY(QString voiceOwner READ voiceOwner NOTIFY voiceOwnerChanged)
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
    // MSC2545 image packs: the sticker picker's controller, custom-emoji
    // lookup, and the m.sticker send / "add to my stickers" paths.
    Q_PROPERTY(StickerPackManager* stickers READ stickers CONSTANT)
    // Application updates (app.updateManager). Deliberately owns no Matrix
    // state: it is constructed once, never re-created on sign-in or account
    // switch, and nothing about it is account-scoped. See docs/updates.md.
    Q_PROPERTY(lightning::update::UpdateManager* updateManager READ updateManager CONSTANT)
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
    QString emojiFontFamily() const;

    // A FONT, not a family name, and that is the whole point.
    //
    // QML's font value type has `family` (one string) and no `families`, so a
    // QML surface holding MIXED text cannot express "this face, then the emoji
    // face". A QFont CAN: setFamilies() is real Qt font fallback, resolved
    // per character by the shaper. Handing one to a TextArea is therefore the
    // only way to get colour emoji in the composer without rendering the words
    // in an emoji face too.
    //
    // Takes the pixel size because the composer's size follows the 90-140%
    // text-size setting; returning a fixed font would freeze it.
    Q_INVOKABLE QFont textFontWithEmoji(const QString &family, int pixelSize,
                                        bool italic = false) const;

    // Custom application icon (Settings -> Appearance). The picked file's
    // bytes are validated and normalized by appicon::normalizeIconBytes
    // (byte-sniffed raster only, SVG refused, circular 512px PNG) and the
    // normalized copy is stored under app data — never a live reference to
    // the arbitrary external path. Returns an empty string on success or a
    // short translated failure sentence for the settings UI. Honest desktop
    // limitation: the runtime icon reaches the window, X11 task
    // switchers/docks, and Wayland compositors that honor xdg-toplevel-icon;
    // launchers and pinned entries resolve the hicolor theme icon from the
    // installed desktop entry and keep the packaged default.
    Q_INVOKABLE QString setCustomAppIconFromFile(const QUrl &fileUrl);
    // Removes the normalized copy, disables the setting, and restores the
    // packaged default icon immediately.
    Q_INVOKABLE void resetCustomAppIcon();

    /// One line naming how the window was placed at startup, and against
    /// what. Diagnostic only — the reported "opens half off screen, then
    /// fights being dragged" needs to be told apart from a stale stored rect,
    /// a monitor that has gone away, and metrics that were not settled yet,
    /// and those look identical from the outside. Carries geometry only:
    /// nothing here is account data.
    Q_INVOKABLE void noteWindowPlacement(const QString &how, int x, int y,
                                         int width, int height) const;
    QString appIconSource() const;
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
    ShortcutRegistry *shortcuts() const;
    LocalizationManager *localization() const;
    CustomThemeStore *customTheme() const;
    RailLayoutStore *railLayout() const;
    RailEntryModel *railEntries() const;
    ProfileBannerManager *banners() const;
    ProfileBioManager *bio() const;
    ProfileBadges *badges() const;
    ImageCropper *imageCrop() { return &m_imageCrop; }
    AuthManager *auth() const;
    AccountManager *accounts() const;
    RoomListModel *roomList() const;
    SpaceChannelModel *spaceChannels() const;
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
    CryptoHealthModel *cryptoHealth() const { return m_cryptoHealth.get(); }
    QObject *backup() const;
    CryptoBootstrapModel *cryptoBootstrap() const
    { return m_cryptoBootstrap.get(); }
    // Bounded UI refresh (Settings "Refresh" and post-operation updates).
    Q_INVOKABLE void refreshCryptoHealth();
    QVariantList sessionDevices() const { return m_sessionDevices; }
    bool sessionDevicesLoading() const { return m_sessionDevicesLoading; }
    bool sessionDevicesFailed() const { return m_sessionDevicesFailed; }
    Q_INVOKABLE void refreshSessionDevices();
    // v0.9 (phase 9): rename one of this account's sessions through the
    // standard device endpoint; the list is refetched on success. The
    // outcome lands on sessionDeviceRenameError ("" = fine) and
    // sessionDeviceRenaming.
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
    /// Start a call in `roomId`, choosing the lane.
    ///
    /// MatrixRTC (SFU) is PRIMARY: it is what current Element speaks, it
    /// carries video and screen share, and it works in a group. The legacy
    /// 1:1 lane is the FALLBACK, for a room or homeserver with no MatrixRTC
    /// — and it is audio-only and DM-only by protocol, because a legacy
    /// invite rings every member of a room.
    ///
    /// Returns false and reports through `callStartRefused` when neither
    /// lane can carry a call, rather than appearing to start one.
    Q_INVOKABLE bool startCall(const QString &roomId, bool withVideo = false);
    /// Whether `startCall` would do anything for this room — the gate the
    /// room-header button uses, so a dead button is never offered.
    Q_INVOKABLE bool canStartCall(const QString &roomId) const;
    /// Which lane `startCall` would use: "matrixrtc", "legacy" or "" for
    /// none. Diagnostics and tests; not shown in normal UI.
    Q_INVOKABLE QString preferredCallLane(const QString &roomId) const;
    RtcController *rtc() const;
    SfuCallController *groupCall() const;
    CallDeviceController *callDevices() const;
    // Registers the real WebRTC media engine (webrtcbin) when the build
    // carries it and its runtime elements resolve. Called by main.cpp for
    // the real application only; tests opt in explicitly.
    void enableCallMediaEngine();

private:
    void copyImageBytesToClipboard(const QString &mediaKey, bool ok,
                                   const QByteArray &bytes,
                                   const QString &category);

public:
    // Test seam: integration tests drive/inspect notification glue (the
    // DBus daemon is absent under offscreen runs).
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
    // v0.7.x sessions page: MAS/OAuth accounts manage devices in the
    // account console, never through a password prompt. Invokable (not a
    // bound property) — the page evaluates it when it opens, matching the
    // capture-at-open idiom the popovers use.
    Q_INVOKABLE bool activeAccountIsOAuth() const
    {
        return m_settings
               && m_settings->isOAuthAccount(m_settings->activeAccountUserId());
    }
    RoomInfoController *roomInfo() const { return m_roomInfo.get(); }
    MediaBridge *mediaBridge() const { return m_mediaBridge.get(); }
    MediaVisibilityStore *mediaVisibility() const
    { return m_mediaVisibility.get(); }
    VoiceRecorder *voiceRecorder()
    {
        if (!m_voiceRecorder)
            m_voiceRecorder = std::make_unique<VoiceRecorder>(this);
        return m_voiceRecorder.get();
    }
    QString voiceOwner() const { return m_voiceOwner; }
    // Start a recording owned by `owner` ("room" or "thread"). Constructs
    // the recorder on first use exactly as the getter does, so a session
    // that never records never spins up the audio backend. Returns false
    // when no device or encoder is available, AND when a recording is
    // already in progress anywhere — ownership is taken only after a
    // successful start and is never stolen from a live recorder, so at most
    // one composer is ever armed to send the result. Use
    // voiceRecordingBusy() to tell the two refusals apart.
    Q_INVOKABLE bool startVoiceRecording(const QString &owner);
    // True while any recording is in progress (including one owned by the
    // other composer, and including the finalizing window). Lets a refused
    // start report "already recording" instead of "unavailable".
    Q_INVOKABLE bool voiceRecordingBusy() const;
    // Test seam, in the shape of AttachmentQueueModel::setPosterRequestHook.
    // Replaces the real capture chain so the ownership rules — a second
    // composer cannot steal a live recording, and a refused start leaves the
    // existing owner intact — are assertable without a microphone. The
    // AppController takes ownership of the recorder. Production never calls
    // this; the lazy getter builds the real one.
    void setVoiceRecorderForTest(VoiceRecorder *recorder);
    // Release ownership without touching the recorder. Called after the
    // owning composer has consumed ready()/failed().
    Q_INVOKABLE void endVoiceRecording();
    // Discard an in-progress recording and release ownership.
    Q_INVOKABLE void cancelVoiceRecording();
    // 2026-08-18: delete a FINALIZED recording the user decided not to send.
    // ready() transfers file ownership to the composer, so a discarded
    // preview would otherwise leave the audio on disk until the session
    // ends. Deletes only a file the recorder itself produced (the recorder
    // answers that), never an arbitrary path handed over from QML.
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
    // v0.7.x: a room-oriented Matrix link (matrix.to permalink or matrix:
    // URI) activated inside the app. Routed to the Discover surface, which
    // resolves it through the SDK and opens/joins from there — QML never
    // parses Matrix identifiers itself.
    void openMatrixLink(const QString &link) { Q_EMIT matrixLinkRequested(link); }

    // Space Home: select the Space in the rail AND clear the open room so
    // the Space overview surface becomes visible. The overview pane itself
    // has existed since the v0.7 UI checkpoints but was unreachable once
    // any room had been opened — nothing on space selection cleared the
    // room (user report, 2026-08-14). Reached from a rail double-click and
    // the workspace header.
    void openSpaceHome(const QString &spaceId);
    // The Channels layout's "Lobby": back to the home / all-conversations
    // surface. Deliberately NOT a fake room and NOT a persisted event —
    // Lobby is navigation, and the state it names is one the shell already
    // has ("no room open, no real Space selected").
    Q_INVOKABLE void openLobby();

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
    // Mute or unmute EVERY joined room in a Space, in one action.
    //
    // Matrix has no "mute a Space" primitive — a Space is a room with no
    // timeline, and muting it would silence nothing. So this is exactly what a
    // person would otherwise do by hand: set each member room's notification
    // mode. `mute` false restores mode 3 (follow the account default) rather
    // than "all messages", because that is the state a room is in before
    // anyone touched it, and asserting "all messages" for rooms that never
    // asked for it would be a different, louder choice than undoing the mute.
    //
    // Bounded by the Space's own membership and idempotent per room. Reports
    // nothing of its own: each room's write already reports through the
    // existing per-room notification-mode path, including the honest
    // kept-on-this-device disclosure when the server refuses.
    Q_INVOKABLE void setSpaceMuted(const QString &spaceId, bool mute);
    // Whether every joined room in the Space is currently muted. False for a
    // Space with no rooms — there is nothing muted, and offering "unmute"
    // for it would be offering to change nothing.
    Q_INVOKABLE bool spaceIsMuted(const QString &spaceId) const;
    // Marks every joined room in the Space read. Matrix has no "mark a Space
    // read" primitive — a Space is a room with no timeline, so a receipt on
    // the Space itself would clear nothing — so this does what a person would
    // otherwise do by hand to each room inside it, through the SAME entry
    // point the room list's own Mark as read uses (which takes its target from
    // the room's latest event and sends the public receipt and m.fully_read
    // together, and works for rooms other than the open one).
    //
    // Bounded by the Space's own membership, which SpaceManager resolves
    // transitively: marking a Space read has to cover the rooms a subspace
    // brought into it, or it is a half-read.
    Q_INVOKABLE void markSpaceRead(const QString &spaceId);
    // Poll-on-open refresh: re-query the server rule when a notification
    // picker opens so changes made in another client land in the cache.
    // No-op on backends without server support.
    Q_INVOKABLE void requestRoomNotificationMode(const QString &roomId);
    // True while the room's LAST server push-rule write is known to have
    // failed (the device-local mode still applies). Cleared by the next
    // successful user-defined report for the room; session-scoped, never
    // persisted. Rooms in this state are retried automatically on the next
    // reconnection (see retryFailedNotificationModes); a room leaves the
    // failed set only when the SERVER acknowledges the value, never merely
    // because a retry was attempted.
    Q_INVOKABLE bool roomNotificationModeSyncFailed(const QString &roomId) const;

    // v0.7 multi-account. Switch the whole Matrix context (client session,
    // stores, crypto, models, notifications) to another saved account
    // without a login form. The previous account stays signed in — its
    // session, store, and token are untouched; only its local runtime is
    // detached. No-op when already switching or the target is unusable.
    Q_INVOKABLE void switchToAccount(const QString &userId);

    // ── v0.7.4 own display name ─────────────────────────────────────────
    // The write is a single-flight command matched by op id, exactly like
    // presence: the id is recorded before the backend is called, and an
    // answer carrying any other id is dropped (it belongs to a previous
    // account, or to an attempt this controller already retired).
    bool canEditOwnDisplayName() const;
    bool ownDisplayNameBusy() const { return m_displayNameOp != 0; }
    QString ownDisplayNameError() const { return m_displayNameError; }
    // A client-side ceiling: Matrix specifies no maximum and servers
    // differ. Mirrors the bound Rust applies before the request goes out.
    Q_INVOKABLE int ownDisplayNameMaxLength() const { return 255; }
    // Length in Unicode CODE POINTS. QML's `text.length` counts UTF-16
    // code units, so an emoji reads as two there and would have the editor
    // refuse names the server accepts.
    Q_INVOKABLE int displayNameLength(const QString &name) const;
    // Returns true when a write was DISPATCHED. False means nothing was
    // sent: a write is already in flight, the backend cannot write
    // profiles, the name is empty (clearing is a separate deliberate
    // action — an empty editor must never silently erase the name), it is
    // over the ceiling, or it is unchanged. ownDisplayNameError explains
    // all of those except "unchanged", which is a silent no-op because the
    // editor disables Save in that state and never reaches here.
    Q_INVOKABLE bool submitOwnDisplayName(const QString &name);
    // The explicit CLEAR. Reaches the SDK as `None`, which asks the server
    // to remove the field rather than to store an empty name.
    Q_INVOKABLE bool clearOwnDisplayName();
    Q_INVOKABLE void dismissOwnDisplayNameError();

    bool ownAvatarBusy() const { return m_avatarOp != 0; }
    QString ownAvatarError() const { return m_avatarError; }
    // True when this backend can write the account's own avatar at all.
    // Mirrors canEditOwnDisplayName: a control that cannot work is worse
    // than no control, so QML hides the whole block on false.
    Q_INVOKABLE bool canEditOwnAvatar() const;
    // Returns true when a write was DISPATCHED. The path is a LOCAL FILE
    // (the crop dialog's output); Rust sniffs its MIME from the bytes and
    // refuses anything that is not a raster image it accepts.
    Q_INVOKABLE bool submitOwnAvatar(const QUrl &fileUrl);
    Q_INVOKABLE bool clearOwnAvatar();
    Q_INVOKABLE void dismissOwnAvatarError();

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

    // 2026-08-19 jump-to-live history trim. Releases the paginated backlog
    // and re-opens the live timeline at the newest message — Element's
    // jumpToLiveTimeline() policy, which rebuilds at the live edge rather
    // than scrolling through thousands of retained rows.
    //
    // Returns TRUE only when a trim was actually dispatched, so the caller
    // can fall back to its ordinary jump. It refuses (returns false) unless
    // ALL of these hold, because an un-asked-for timeline reset is a far
    // worse outcome than a large but correct timeline:
    //   * the Rust backend is active (the mock/HTTP backends have no event
    //     cache to release);
    //   * a room is open and not mid-pagination;
    //   * the loaded row count exceeds `historyTrimRowThreshold()` — below
    //     that the reset costs more than the rows it would release.
    // NEVER call this from scrolling or pagination: it is for one explicit
    // user action.
    Q_INVOKABLE bool trimHistoryAndJumpToLive();
    // The refusal policy as a PURE predicate, so every clause is testable
    // without a live Rust event cache. trimHistoryAndJumpToLive() gathers
    // the state and calls this; the offline suites drive it directly
    // (short-circuit evaluation inside the gatherer otherwise makes the
    // later clauses unreachable on the mock backend — review finding).
    static bool historyTrimAllowed(bool rustBackend, bool roomOpen,
                                   bool paginationBusy, bool threadOpen,
                                   int loadedRows, int rowThreshold)
    {
        if (!rustBackend || !roomOpen)
            return false;
        if (paginationBusy)
            return false;
        // A thread panel / Threads view holds its own event-cache
        // subscriber for this room, so the SDK's auto-shrink cannot fire
        // while either is open — and the reload would tear the panel's live
        // subscription out from under it.
        if (threadOpen)
            return false;
        return loadedRows > rowThreshold;
    }
    // The loaded-row count above which a jump-to-live trims. Exposed so QML
    // and the tests read ONE value.
    Q_INVOKABLE int historyTrimRowThreshold() const { return 400; }

    // v0.6.6: "Star GIF" — the Discord-style hover star overlaid on GIF
    // media in the timeline (see MessageDelegate.qml's imageComponent).
    // Fetches `mediaKey`'s decrypted bytes through the existing controlled
    // media bridge (works in encrypted rooms exactly like Save As) and hands
    // them to the local-starred store once they arrive; see
    // GifStarredStore's header for the documented rationale. Progress/result
    // is observable via app.gif.starredStore.starFinished. This is the ONLY
    // place MediaBridge and the gif:: local-star store meet, so neither
    // gains a dependency on the other.
    // 2026-08-18: Copy image (Discord-style) — a transient clipboard
    // export of the decrypted bytes on explicit user action (Save-As
    // precedent; nothing persists). Result on copyImageFinished.
    Q_INVOKABLE void copyImageToClipboard(const QString &mediaKey);
    Q_INVOKABLE void starChatGif(const QString &mediaKey);
    // v0.6.6 fix: the two QML-facing entry points for the hover star's
    // filled/outline state and its unstar action. Both are two-tier — a
    // fast, exact, SESSION-only check/action (GifStarredStore's own
    // isStarredThisSession/unstarByMediaKey, correct for a GIF starred in
    // this run) with a DURABLE, content-addressed fallback for everything
    // else (a restart, or a second message carrying the identical GIF):
    // MediaBridge::cachedFullContentHash turns `mediaKey` into the sha256
    // GifStarredStore already indexes by, using only bytes MediaBridge's
    // ordinary display cache already fetched for showing the row — never a
    // fresh fetch just to answer this, and never a Matrix identifier
    // (mediaKey/event id) persisted anywhere. See GifStarredStore's class
    // comment ("DURABLE STARRED-STATE DESIGN") for the full rationale and
    // its honest staleness trade-off. This is the ONLY place MediaBridge and
    // the gif:: local-star store meet for this purpose, mirroring
    // starChatGif() above.
    Q_INVOKABLE bool isChatGifStarred(const QString &mediaKey) const;
    Q_INVOKABLE void unstarChatGif(const QString &mediaKey);

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
    bool trayAvailable() const { return TrayIcon::platformSupportsTray(); }
    QObject *spellChecker() { return &m_spell; }
    QRect restorableWindowGeometry() const { return m_restorableWindowGeometry; }
    bool verificationQrScanned() const { return m_verificationQrScanned; }
    bool verificationQrConfirming() const { return m_verificationQrConfirming; }

    // Owned here so it outlives the QML engine that holds the provider.
    QrCodeStore *qrCodeStore() { return &m_qrCodeStore; }
    // Ditto: StagedImageProvider reads it from the QML loader thread, so it
    // must outlive the engine. Holds the encoded bytes of clipboard images
    // that are queued to send but not sent — the only way a chip can preview
    // a paste, which never becomes a file.
    StagedImageStore *stagedImages() { return &m_stagedImages; }

    // v0.5.6 Security & Recovery accessors.
    QString sessionTrustState() const { return m_sessionTrustState; }
    bool sessionVerificationNeeded() const;
    bool sessionVerificationWarning() const;
    // Dismiss the verification badges for the active account (persisted).
    // The Sessions page keeps stating the fact — dismissal silences the
    // nagging, it does not claim the session is verified.
    Q_INVOKABLE void dismissVerificationWarning();
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
    // The tray icon was clicked; Main.qml restores and raises the window.
    void trayShowRequested();

    void voiceOwnerChanged();
    // Emitted when a reconnect retry batch is ISSUED, carrying how many
    // rooms were re-sent. Exists so the retry is observable: the backend
    // call itself no-ops without a live session, so a test asserting only
    // on state cannot tell "retried" from "never ran" — which is exactly
    // what a first version of the retry test could not distinguish.
    void roomNotificationModesRetried(int roomCount);
    void currentScreenChanged();
    void appIconChanged();
    void initialSyncDoneChanged();
    void accountSwitchingChanged();
    void ownDisplayNameStateChanged();
    void ownAvatarStateChanged();
    void ownAvatarSaved();
    // Server-CONFIRMED success. The editor closes on this and on nothing
    // else: renaming to the value the account record already held emits no
    // accountsChanged at all (SettingsManager::updateAccountProfile writes
    // only on a real change), so waiting for the registry would hang the
    // editor on exactly the case that succeeded.
    void ownDisplayNameSaved();
    void currentRoomIdChanged();
    void loggedInChanged();
    void connectionStatusChanged();
    void syncModeChanged();
    void systemDarkModeChanged();
    void errorReported(const QString &message);
    /// A call could not be started, with wording already fit to show.
    void callStartRefused(const QString &message);
    // v0.7.x: see openMatrixLink().
    void matrixLinkRequested(const QString &link);
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
    void sessionVerificationWarningChanged();
    void sessionDevicesChanged();
    void activeRoomAtLatestChanged();
    void activeRoomHydratingChanged();
    // v0.6.0 checkpoint 11: a notification was clicked — QML raises the
    // window, selects the room, opens the thread, and locates the event.
    // Identity only, never tokens.
    void copyImageFinished(bool ok, const QString &message);
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
    // Applies the persisted icon choice to QGuiApplication::setWindowIcon —
    // the normalized custom file when enabled and readable, else the packaged
    // default (theme icon with the embedded 256px fallback).
    void applyAppIcon();
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
    // Cache-busting revision for appIconSource: bumped every time the
    // normalized custom-icon file is rewritten so QML's image cache reloads.
    int m_appIconRevision = 0;
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
    // v0.7.4 own display name. 0 = idle; otherwise the id of the ONE write
    // in flight. The counter is separate from the backend's own op ids on
    // purpose — this is a caller-owned id, like PresenceManager's.
    quint64 m_displayNameOp = 0;
    quint64 m_avatarOp = 0;
    quint64 m_avatarOpCounter = 0;
    QString m_avatarError;
    quint64 m_displayNameOpCounter = 0;
    QString m_displayNameError;
    // Dispatch helper shared by the set and clear paths, so the op id is
    // recorded before the backend call in both.
    bool dispatchOwnDisplayName(const QString &name);
    // Empty when a write may be dispatched; otherwise the honest reason it
    // may not. "Not signed in" and "this backend cannot write a profile"
    // are different facts and are worded differently.
    QString ownDisplayNameUnavailableReason() const;
    // The registry's cached name for the active account, or empty when
    // there is none. Read only to refuse an unchanged write.
    QString cachedOwnDisplayName() const;
    // Retire an in-flight write and its error. Called on every session
    // teardown (sign-out AND account switch) — retiring the op id is what
    // makes a late answer stale, so the next account's editor can never
    // take it as its own.
    void retireOwnDisplayNameWrite();
    void retireOwnAvatarWrite();
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
    std::unique_ptr<LocalizationManager> m_localization;
    std::unique_ptr<ShortcutRegistry> m_shortcuts;
    std::unique_ptr<CustomThemeStore> m_customTheme;
    std::unique_ptr<RailLayoutStore> m_railLayout;
    std::unique_ptr<RailEntryModel> m_railEntries;
    std::unique_ptr<ProfileBannerManager> m_banners;
    std::unique_ptr<ProfileBioManager> m_bio;
    std::unique_ptr<ProfileBadges> m_badges;
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
    std::unique_ptr<SpaceChannelModel> m_spaceChannels;
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
    quint64 m_sessionDeviceRenameOp = 0;
    QString m_sessionDeviceRenameError;
    bool m_sessionDevicesFailed = false;
    bool m_activeRoomAtLatest = false;
    bool m_activeRoomHydrating = false;
    QSet<QString> m_knownInvites;
    // Rooms whose last server push-rule write failed, so the pickers can
    // say "kept on this device" instead of claiming the mode was saved to
    // the account. Session-scoped: cleared on logout/account switch.
    QSet<QString> m_notificationModeSyncFailures;
    std::unique_ptr<SpaceManager> m_spaces;
    std::unique_ptr<ThreadManager> m_threads;
    std::unique_ptr<PresenceManager> m_presence;
    std::unique_ptr<CallController> m_calls;
    std::unique_ptr<RtcController> m_rtc;
    std::unique_ptr<SfuCallController> m_groupCall;
    std::unique_ptr<CallDeviceController> m_callDevices;
    // The one call whose ring was actually announced (notification shown):
    // the missed-call notice requires it, so suppressed rings never
    // resurface as "missed". Bounded per-sender ring cooldown alongside.
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
    // Media keys THIS account asked to star. The star fetch is
    // media-generic and has more than one caller now, so its result must be
    // claimed rather than assumed — see the handler in setClient(). Cleared
    // on sign-out with the rest of the account-scoped state.
    QSet<QString> m_pendingStarKeys;
    QSet<QString> m_pendingCopyKeys;
    std::unique_ptr<RoomInfoController> m_roomInfo;
    std::unique_ptr<MediaBridge> m_mediaBridge;
    std::unique_ptr<MediaVisibilityStore> m_mediaVisibility;
    std::unique_ptr<VoiceRecorder> m_voiceRecorder; // lazy — see getter
    QString m_voiceOwner;                           // "", "room", "thread"
    // Re-issue push-rule writes that failed offline, once per genuine
    // transition into Syncing. Never clears the failure set itself.
    void retryFailedNotificationModes();
    // MatrixClient::ConnectionState as an int — the class is only
    // forward-declared here. -1 is "no state seen yet", which is distinct
    // from every real enumerator, so the first transition into Syncing
    // counts as an edge.
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

    // Show-QR verification. The grid itself lives in the store (memory
    // only); the controller keeps just the opaque URL token and the
    // SDK-reported progress flags. `clearVerificationQr` is the single
    // point that drops both, and every flow-ending path calls it.
    QrCodeStore m_qrCodeStore;
    StagedImageStore m_stagedImages;
    ImageCropper m_imageCrop;
    TrayIcon m_tray;
    SpellChecker m_spell;
    // Pushes the account's unread state onto the tray. It READS the room
    // snapshot the client already holds and asks the server for nothing —
    // the same rule the room-list call glyph is held to, because a tray
    // badge that refreshed itself would issue one request per room per
    // rebuild.
    void refreshTrayUnread();
    // COALESCED, for the same reason RoomListModel coalesces its own
    // reconcile through a zero-interval single shot: `roomUpdated` fires per
    // room and `MatrixClient::rooms()` returns the whole snapshot BY VALUE,
    // so reacting to each one directly would copy every RoomInfo in the
    // account once per delivered event. One pass per event-loop turn.
    QTimer m_trayUnreadCoalesce;
    void refreshTrayState();
    // Computed once in the constructor; see restorableWindowGeometry().
    QRect m_restorableWindowGeometry;
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
