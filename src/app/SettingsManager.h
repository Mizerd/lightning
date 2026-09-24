#pragma once

#include "storage/AppDataPaths.h"

#include <QObject>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class SecretStore;

class SettingsManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString homeserverUrl READ homeserverUrl WRITE setHomeserverUrl NOTIFY homeserverUrlChanged)
    // Login-screen homeserver prefill. Account-independent: the add-account
    // flow keeps the current account active, so binding to homeserverUrl would
    // always revert the field to the active account's server.
    Q_PROPERTY(QString loginHomeserverPrefill READ loginHomeserverPrefill
                   WRITE setLoginHomeserverPrefill
                   NOTIFY loginHomeserverPrefillChanged)
    Q_PROPERTY(Theme theme READ theme WRITE setTheme NOTIFY themeChanged)
    // Message layout (0 = Modern, 1 = Bubbles for DMs, 2 = Compact/IRC) and
    // text scale (percent, 90–140). Per-account with a global fallback that
    // doubles as the logged-out default.
    Q_PROPERTY(int messageLayout READ messageLayout WRITE setMessageLayout
                   NOTIFY messageLayoutChanged)
    /// Room-list organisation: 0 = Classic (one activity-ordered list with
    /// DM/Rooms sections), 1 = Channels (the active Space's own hierarchy).
    /// Account-scoped through appearanceValue.
    Q_PROPERTY(int roomNavigationLayout READ roomNavigationLayout
                   WRITE setRoomNavigationLayout
                   NOTIFY roomNavigationLayoutChanged)
    // Room-list filter chips (0 All, 1 People, 2 Rooms, 3 Unreads).
    // Per-account with global fallback.
    Q_PROPERTY(int roomFilterMode READ roomFilterMode WRITE setRoomFilterMode
                   NOTIFY roomFilterModeChanged)
    Q_PROPERTY(int textScale READ textScale WRITE setTextScale
                   NOTIFY textScaleChanged)
    // UI font family, per-account with global fallback. Stored verbatim after a
    // syntactic check only: this class links Qt6::Core alone and cannot ask
    // QFontDatabase. FontManager resolves the name and falls back to the
    // bundled face without rewriting this value, so a reinstalled font comes
    // back.
    Q_PROPERTY(QString uiFont READ uiFont WRITE setUiFont
                   NOTIFY uiFontChanged)
    // Monospace family (code blocks, keycaps, Matrix identifiers). Same storage
    // and resolution rules as uiFont.
    Q_PROPERTY(QString monoFont READ monoFont WRITE setMonoFont
                   NOTIFY monoFontChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged)
    // Custom application icon. Device-global: the window icon is process-wide
    // and applies before any account restores. The image itself lives at
    // matrix::app_data::customAppIconFile(); AppController owns validation.
    Q_PROPERTY(bool customAppIconEnabled READ customAppIconEnabled
                   WRITE setCustomAppIconEnabled NOTIFY customAppIconEnabledChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    // Notification privacy: 0 = sender and message, 1 = sender only (default),
    // 2 = private ("New Matrix notification").
    Q_PROPERTY(int notificationPreview READ notificationPreview
                   WRITE setNotificationPreview NOTIFY notificationPreviewChanged)
    // Separate preview level for encrypted rooms: a notification body lands in
    // the notification daemon, its log and possibly a lock screen, outside the
    // room's encryption. Values are PreviewMode (0-2) plus 3 = FollowGeneral,
    // the default.
    Q_PROPERTY(int notificationPreviewEncrypted READ notificationPreviewEncrypted
                   WRITE setNotificationPreviewEncrypted
                   NOTIFY notificationPreviewEncryptedChanged)
    // MSC4153 "invisible crypto": refuse to share room keys with, and to
    // decrypt from, devices that are not cross-signed. Off by default because
    // enabling it makes contacts with unverified devices unreadable. Takes
    // effect on restart: matrix-sdk 0.18 has no runtime setter for either half.
    Q_PROPERTY(bool strictDeviceTrust READ strictDeviceTrust
                   WRITE setStrictDeviceTrust NOTIFY strictDeviceTrustChanged)
    // Who is told this account has read a message: 0 public (default), 1
    // private (MSC2285 `m.read.private`, still clears this account's other
    // devices), 2 off. The fully-read marker is sent in every mode; it is
    // private account data.
    Q_PROPERTY(int readReceiptMode READ readReceiptMode
                   WRITE setReadReceiptMode NOTIFY readReceiptModeChanged)
    // Whether typing notices leave this device.
    Q_PROPERTY(bool sendTypingNotifications READ sendTypingNotifications
                   WRITE setSendTypingNotifications
                   NOTIFY sendTypingNotificationsChanged)
    // Notification sound: 0 = off, 1 = mentions and DMs (default), 2 = all.
    // Suppressed along with the notification it belongs to.
    Q_PROPERTY(int notificationSound READ notificationSound
                   WRITE setNotificationSound NOTIFY notificationSoundChanged)
    // Whether an incoming call rings. Device-wide; the call banner itself is
    // governed by notificationsEnabled.
    Q_PROPERTY(bool ringForCalls READ ringForCalls WRITE setRingForCalls
                   NOTIFY ringForCallsChanged)
    // Lightning's own call sounds (src/calls/CallSoundPolicy.h). Device-scoped.
    // The master switch covers every in-call cue and the outgoing ringback; the
    // three below split cues by meaning. The incoming ring stays on
    // ringForCalls. Volumes are 0-100 on a perceptual scale. One NOTIFY for the
    // group.
    Q_PROPERTY(bool callSoundsEnabled READ callSoundsEnabled
                   WRITE setCallSoundsEnabled NOTIFY callSoundSettingsChanged)
    Q_PROPERTY(bool callSoundsPresence READ callSoundsPresence
                   WRITE setCallSoundsPresence NOTIFY callSoundSettingsChanged)
    Q_PROPERTY(bool callSoundsControls READ callSoundsControls
                   WRITE setCallSoundsControls NOTIFY callSoundSettingsChanged)
    Q_PROPERTY(bool callSoundsShareAndHand READ callSoundsShareAndHand
                   WRITE setCallSoundsShareAndHand
                   NOTIFY callSoundSettingsChanged)
    Q_PROPERTY(int callSoundVolume READ callSoundVolume
                   WRITE setCallSoundVolume NOTIFY callSoundSettingsChanged)
    Q_PROPERTY(int ringerVolume READ ringerVolume WRITE setRingerVolume
                   NOTIFY callSoundSettingsChanged)
    // Call device preferences. Device-scoped: hardware belongs to the machine.
    // Stored as the PipeWire/Pulse node name; empty means "system default".
    Q_PROPERTY(QString preferredMicrophoneId READ preferredMicrophoneId
                   WRITE setPreferredMicrophoneId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredSpeakerId READ preferredSpeakerId
                   WRITE setPreferredSpeakerId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredCameraId READ preferredCameraId
                   WRITE setPreferredCameraId
                   NOTIFY callDevicePreferenceChanged)
    // Link previews. Encrypted-room previews default off.
    Q_PROPERTY(bool autoLoadLinkPreviews READ autoLoadLinkPreviews
                   WRITE setAutoLoadLinkPreviews NOTIFY autoLoadLinkPreviewsChanged)
    Q_PROPERTY(bool loadPreviewsInEncryptedRooms READ loadPreviewsInEncryptedRooms
                   WRITE setLoadPreviewsInEncryptedRooms
                   NOTIFY loadPreviewsInEncryptedRoomsChanged)
    Q_PROPERTY(bool animateGifPreviews READ animateGifPreviews
                   WRITE setAnimateGifPreviews NOTIFY animateGifPreviewsChanged)
    // Screen-share quality. Device-global.
    Q_PROPERTY(int shareMaxHeight READ shareMaxHeight WRITE setShareMaxHeight
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(int shareFps READ shareFps WRITE setShareFps
                   NOTIFY shareQualityChanged)
    /// True when the chosen combination asks more of the encoder than it can
    /// sustain in real time. Single predicate shared by every surface.
    Q_PROPERTY(bool shareQualityDemanding READ shareQualityDemanding
                   NOTIFY shareQualityChanged)
    // Publish this account's own presence. Default on; viewing others' presence
    // is passive and has no toggle.
    Q_PROPERTY(bool sharePresence READ sharePresence
                   WRITE setSharePresence NOTIFY sharePresenceChanged)
    // Shell layout. Device-level: it describes this window on this screen, and
    // an account switch must not resize it.
    Q_PROPERTY(bool spacesRailVisible READ spacesRailVisible
                   WRITE setSpacesRailVisible NOTIFY spacesRailVisibleChanged)
    // Space banner visibility and expansion. App-wide rather than per-Space.
    Q_PROPERTY(bool spaceBannersVisible READ spaceBannersVisible
                   WRITE setSpaceBannersVisible NOTIFY spaceBannersVisibleChanged)
    Q_PROPERTY(bool spaceBannerExpanded READ spaceBannerExpanded
                   WRITE setSpaceBannerExpanded NOTIFY spaceBannerExpandedChanged)
    Q_PROPERTY(bool roomListVisible READ roomListVisible
                   WRITE setRoomListVisible NOTIFY roomListVisibleChanged)
    Q_PROPERTY(int roomListWidth READ roomListWidth
                   WRITE setRoomListWidth NOTIFY roomListWidthChanged)
    Q_PROPERTY(int spacesRailWidth READ spacesRailWidth
                   WRITE setSpacesRailWidth NOTIFY spacesRailWidthChanged)
    /// How the rail shows nesting: 0 = Regions (tinted region per ancestor),
    /// 1 = Classic (indentation only). Presentation only: rows, order and drag
    /// behaviour are identical. App-wide like spacesRailWidth.
    Q_PROPERTY(int spacesRailDepthStyle READ spacesRailDepthStyle
                   WRITE setSpacesRailDepthStyle
                   NOTIFY spacesRailDepthStyleChanged)
    Q_PROPERTY(int sidePanelWidth READ sidePanelWidth
                   WRITE setSidePanelWidth NOTIFY sidePanelWidthChanged)
    // The clamps, exposed so a slider cannot invent narrower bounds than the
    // setter enforces.
    Q_PROPERTY(int roomListMinWidth READ roomListMinWidth CONSTANT)
    Q_PROPERTY(int roomListMaxWidth READ roomListMaxWidth CONSTANT)
    Q_PROPERTY(int sidePanelMinWidth READ sidePanelMinWidth CONSTANT)
    Q_PROPERTY(int sidePanelMaxWidth READ sidePanelMaxWidth CONSTANT)
    // Storage bounds for the rail width. The draggable stops are computed in
    // QML from scaled pixels (which C++ cannot know); MainScreen intersects
    // both.
    Q_PROPERTY(int spacesRailMinWidth READ spacesRailMinWidth CONSTANT)
    Q_PROPERTY(int spacesRailMaxWidth READ spacesRailMaxWidth CONSTANT)
    // Closing the window hides to the system tray instead of quitting. Off by
    // default and gated on the platform having a tray.
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray
                   NOTIFY closeToTrayChanged)
    Q_PROPERTY(bool startInTray READ startInTray WRITE setStartInTray
                   NOTIFY startInTrayChanged)
    // Window size, position and maximized state from the previous launch.
    // CONSTANT on purpose: a notifying property would feed Qt's write-back of
    // x/y/width/height into the binding that produced it. Saving goes through
    // the invokables below. An invalid rect means "never saved". Only the
    // normal geometry is stored; maximized is a separate flag. Whether the
    // position is still on a connected screen is decided by
    // AppController::restorableWindowGeometry.
    Q_PROPERTY(QRect initialWindowGeometry READ initialWindowGeometry CONSTANT)
    Q_PROPERTY(bool initialWindowMaximized READ initialWindowMaximized CONSTANT)
    // The user dismissed the "verify this session" warnings. Strictly
    // account-scoped (not appearanceValue, which mirrors into a global
    // fallback). Cleared once the session becomes verified.
    Q_PROPERTY(bool verificationWarningDismissed
                   READ verificationWarningDismissed
                   WRITE setVerificationWarningDismissed
                   NOTIFY verificationWarningDismissedChanged)
    // GIF browser policy. gifAutoplay: 0 = Always, 1 = OnHover, 2 = Never.
    // gifSafeSearch is a gif::Rating id. gifPreferredProvider is "giphy" or
    // "klipy".
    //
    // The gifAutoplay name is historical: it governs all passive media (GIF
    // animation and speculative video/audio prefetch), so Never means no
    // passive downloads. The key keeps its name so existing preferences are
    // not reset.
    Q_PROPERTY(int gifAutoplay READ gifAutoplay WRITE setGifAutoplay
                   NOTIFY gifAutoplayChanged)
    Q_PROPERTY(int gifSafeSearch READ gifSafeSearch WRITE setGifSafeSearch
                   NOTIFY gifSafeSearchChanged)
    Q_PROPERTY(bool storeRecentGifs READ storeRecentGifs
                   WRITE setStoreRecentGifs NOTIFY storeRecentGifsChanged)
    Q_PROPERTY(QString gifPreferredProvider READ gifPreferredProvider
                   WRITE setGifPreferredProvider NOTIFY gifPreferredProviderChanged)
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    // Narrower filters under the showRoomActivity master switch: joins/leaves
    // and profile changes. With the master off nothing is shown.
    Q_PROPERTY(bool showMembershipEvents READ showMembershipEvents
                   WRITE setShowMembershipEvents
                   NOTIFY showMembershipEventsChanged)
    Q_PROPERTY(bool showProfileChangeEvents READ showProfileChangeEvents
                   WRITE setShowProfileChangeEvents
                   NOTIFY showProfileChangeEventsChanged)
    // Collapse media and link embeds to one line. Presentation only and off by
    // default; see `collapseEmbedsSetting` in qml/MessageDelegate.qml for what
    // it covers. Per account with a global fallback, so it must be announced in
    // setActiveAccountUserId (SettingsSessionTest checks this).
    Q_PROPERTY(bool collapseEmbeds READ collapseEmbeds WRITE setCollapseEmbeds
                   NOTIFY collapseEmbedsChanged)
    // Reduced motion, read by AppTheme.reducedMotion. Per account with a global
    // fallback.
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion
                   NOTIFY reducedMotionChanged)

    // Smooth scrolling. Separate from reducedMotion so the wheel can land
    // directly without disabling every animation. Default on.
    Q_PROPERTY(bool smoothScrolling READ smoothScrolling WRITE setSmoothScrolling
                   NOTIFY smoothScrollingChanged)
    // Composer buttons the user has hidden, by key: "formatting", "emoji",
    // "media", "voice", "sendOptions". Stores what is hidden, so buttons added
    // later are visible by default. A notifying list rather than an invokable
    // so QML bindings re-evaluate. The attach button is not hideable: narrow
    // windows displace other actions into it.
    Q_PROPERTY(QStringList hiddenComposerButtons READ hiddenComposerButtons
                   WRITE setHiddenComposerButtons
                   NOTIFY hiddenComposerButtonsChanged)
    // Clock format: 0 = follow the system locale (default), 1 = 12-hour,
    // 2 = 24-hour. Per account with a global fallback.
    Q_PROPERTY(int clockFormat READ clockFormat WRITE setClockFormat
                   NOTIFY clockFormatChanged)
    // The Qt time-format string the clock setting resolves to. A property
    // rather than an invokable so timestamp bindings re-evaluate when the
    // setting changes.
    Q_PROPERTY(QString clockTimeFormat READ clockTimeFormat
                   NOTIFY clockFormatChanged)
    // Composer: Enter inserts a newline and Ctrl+Enter sends. Device-global.
    Q_PROPERTY(bool enterInsertsNewline READ enterInsertsNewline
                   WRITE setEnterInsertsNewline
                   NOTIFY enterInsertsNewlineChanged)
    // Composer mode: "markdown" or "rich". Device-global. Any value but "rich"
    // reads as markdown.
    Q_PROPERTY(QString composerMode READ composerMode WRITE setComposerMode
                   NOTIFY composerModeChanged)
    // Spell checking. Application preference, never synced. Language "" is
    // Automatic, otherwise a BCP-47 tag; independent of the UI language.
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled
                   WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(QString spellCheckLanguage READ spellCheckLanguage
                   WRITE setSpellCheckLanguage NOTIFY spellCheckLanguageChanged)
    // Composer: text typed alongside an attachment is sent as its caption
    // rather than as a separate message. Device-global.
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption
                   NOTIFY sendTextAsCaptionChanged)
    // Timeline mouse-wheel speed, matching TimelineScrollController::WheelSpeed
    // (0 = Standard, 1 = Fast, 2 = Very fast). Default: Fast.
    Q_PROPERTY(int timelineWheelSpeed READ timelineWheelSpeed
                   WRITE setTimelineWheelSpeed NOTIFY timelineWheelSpeedChanged)
    // Inline media playback volume (linear 0..1) and rate (0.25..4.0),
    // remembered globally.
    Q_PROPERTY(qreal mediaVolume READ mediaVolume WRITE setMediaVolume
                   NOTIFY mediaVolumeChanged)
    Q_PROPERTY(qreal mediaPlaybackRate READ mediaPlaybackRate
                   WRITE setMediaPlaybackRate NOTIFY mediaPlaybackRateChanged)
    // Whole-interface zoom percent (75..150). Global: main() turns it into
    // QT_SCALE_FACTOR before the app exists, so it applies on next launch.
    Q_PROPERTY(int interfaceZoom READ interfaceZoom WRITE setInterfaceZoom
                   NOTIFY interfaceZoomChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString secretBackendName READ secretBackendName NOTIFY secretBackendChanged)
    Q_PROPERTY(bool secretsAreSecure READ secretsAreSecure NOTIFY secretBackendChanged)

public:
    // Appearance presets. Ids are stable across releases; AppTheme.qml resolves
    // each to a palette.
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,         // Lightning Light
        DarkTheme = 2,          // Lightning Dark
        GraphiteTheme = 3,
        MidnightBlueTheme = 4,  // Midnight
        NordTheme = 5,          // Nordic
        PurpleDuskTheme = 6,
        WarmTheme = 7,
        MossLightTheme = 8,     // design-handoff light
        IndigoNightTheme = 9,   // design-handoff dark
        DeepTealTheme = 10,     // design-handoff dark
        StormTheme = 11,        // brand navy + bolt yellow (0.6.5 Storm)
        // User-authored palette: role overrides on top of a preset. Values live
        // in CustomThemeStore.
        CustomTheme = 12,
    };
    Q_ENUM(Theme)

    // Highest valid Theme id; an out-of-range stored value falls back to
    // SystemTheme (see theme()).
    static constexpr int kMaxThemeId = CustomTheme;

    explicit SettingsManager(QObject *parent = nullptr);

    // Inject the process-wide SecretStore. Call once, right after
    // construction. Migrates any plaintext access token from QSettings into
    // the store.
    void setSecretStore(SecretStore *store);
    SecretStore *secretStore() const { return m_secretStore; }

    QString homeserverUrl() const;
    void setHomeserverUrl(const QString &url);

    QString loginHomeserverPrefill() const;
    void setLoginHomeserverPrefill(const QString &url);

    Theme theme() const;
    void setTheme(Theme t);

    // Out-of-range message layout values read back as Modern.
    static constexpr int kMaxMessageLayout = 2;
    /// 0 Classic, 1 Channels.
    static constexpr int kMaxRoomNavigationLayout = 1;
    int messageLayout() const;
    int roomNavigationLayout() const;
    void setRoomNavigationLayout(int layout);
    void setMessageLayout(int layout);
    int roomFilterMode() const;
    void setRoomFilterMode(int mode);

    // Text scale percent. Out-of-range values read back as 100.
    static constexpr int kMinTextScale = 90;
    static constexpr int kMaxTextScale = 140;
    int textScale() const;
    QString uiFont() const;
    void setUiFont(const QString &family);
    QString monoFont() const;
    void setMonoFont(const QString &family);
    // Curated bundled UI families, shown first in the picker.
    Q_INVOKABLE static QStringList uiFontChoices();
    // Returns the trimmed family name if it is safe to persist (bounded, no
    // control characters or markup/style punctuation), else empty. Does not
    // check that the font exists.
    static QString acceptableFontFamily(const QString &family);

    // Basenames of user-imported fonts. Device-global: application fonts are
    // registered before any account restores.
    QStringList importedFontFiles() const;
    void setImportedFontFiles(const QStringList &fileNames);
    void setTextScale(int percent);

    QString language() const;
    void setLanguage(const QString &lang);

    bool startMinimized() const;
    bool customAppIconEnabled() const;
    void setCustomAppIconEnabled(bool enabled);
    void setStartMinimized(bool v);

    bool notificationsEnabled() const;
    int notificationPreview() const;
    int notificationPreviewEncrypted() const;
    void setNotificationPreviewEncrypted(int v);
    /// The preview level to apply. When encryption is unknown the stricter of
    /// the two levels wins.
    Q_INVOKABLE int effectiveNotificationPreview(bool encrypted,
                                                 bool encryptionKnown) const;
    bool callPictureInPicture() const;
    void setCallPictureInPicture(bool v);
    bool strictDeviceTrust() const;
    void setStrictDeviceTrust(bool v);
    int readReceiptMode() const;
    void setReadReceiptMode(int v);
    bool sendTypingNotifications() const;
    void setSendTypingNotifications(bool v);
    int notificationSound() const;
    void setNotificationSound(int mode);
    bool ringForCalls() const;
    // Call sounds. Volumes clamp to 0..100.
    static constexpr int kDefaultCallSoundVolume = 70;
    static constexpr int kDefaultRingerVolume = 80;
    bool callSoundsEnabled() const;
    void setCallSoundsEnabled(bool v);
    bool callSoundsPresence() const;
    void setCallSoundsPresence(bool v);
    bool callSoundsControls() const;
    void setCallSoundsControls(bool v);
    bool callSoundsShareAndHand() const;
    void setCallSoundsShareAndHand(bool v);
    int callSoundVolume() const;
    void setCallSoundVolume(int percent);
    int ringerVolume() const;
    void setRingerVolume(int percent);
    QString preferredMicrophoneId() const;
    void setPreferredMicrophoneId(const QString &id);
    QString preferredSpeakerId() const;
    void setPreferredSpeakerId(const QString &id);
    QString preferredCameraId() const;
    void setPreferredCameraId(const QString &id);
    void setRingForCalls(bool enabled);
    void setNotificationPreview(int mode);
    // Per-room notification mode: 0 = all messages, 1 = mentions & keywords, 2
    // = mute. On the Rust backend this is a local cache of the server push-rule
    // mode (server wins for explicit rules). Stored per account with a
    // read-fallback to the legacy device-global key.
    Q_INVOKABLE int roomNotificationMode(const QString &roomId) const;
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    // The account's own status message {emoji, text, expiresAtMs}, so it can be
    // re-published or cleared after a restart. Strictly account-scoped.
    QVariantMap ownPresenceStatus() const;
    void setOwnPresenceStatus(const QVariantMap &status);
    // Local scheduled-send queue, account-scoped. Never holds encrypted-room
    // rows.
    QVariantList scheduledSends() const;
    void setScheduledSends(const QVariantList &rows);
    // Activity Center state: the "seen up to" marker and keyword list.
    QVariantMap activityState() const;
    void setActivityState(const QVariantMap &state);

    // Composer drafts for unencrypted rooms only (DraftStore enforces this).
    // Strictly account-scoped, LRU-bounded, wiped with the account. An empty
    // map removes the entry.
    QVariantMap roomDraft(const QString &draftKey) const;
    void setRoomDraft(const QString &draftKey, const QVariantMap &draft);

    // Video dimensions learned from the first decoded frame, for events whose
    // metadata declares none, so the card has its real shape on later visits.
    // Account-scoped, keyed by a hash of the media key, LRU-bounded. Empty size
    // when unknown.
    Q_INVOKABLE QSize knownVideoDimensions(const QString &mediaKey) const;
    void setKnownVideoDimensions(const QString &mediaKey, int width,
                                 int height);
    // Payload size learned from the first fetch, for media whose event declares
    // none. Same hashed-key LRU storage. 0 when unknown.
    Q_INVOKABLE double knownMediaSizeBytes(const QString &mediaKey) const;
    void setKnownMediaSizeBytes(const QString &mediaKey, qint64 bytes);

    // Remembered size of a resizable picker (GIF/emoji) as a share of available
    // space, per mille 50..1000. `id` is whitelisted so QML cannot reach
    // arbitrary keys; unknown ids read 0 and write nothing. 0 means "use the
    // component default", which is also what out-of-range values degrade to.
    Q_INVOKABLE int pickerWidthShare(const QString &id) const;
    Q_INVOKABLE int pickerHeightShare(const QString &id) const;
    Q_INVOKABLE void setPickerShare(const QString &id, int widthPerMille,
                                    int heightPerMille);
    void setNotificationsEnabled(bool v);

    // Link-preview policy.
    bool autoLoadLinkPreviews() const;

    void setAutoLoadLinkPreviews(bool v);
    bool loadPreviewsInEncryptedRooms() const;
    void setLoadPreviewsInEncryptedRooms(bool v);
    bool animateGifPreviews() const;
    void setAnimateGifPreviews(bool v);
    /// Screen-share ceiling in scanlines: 720, 1080 or 1440. Never upscales.
    int shareMaxHeight() const;
    void setShareMaxHeight(int v);
    /// Screen-share frame rate: 15, 30 or 60.
    int shareFps() const;
    void setShareFps(int v);
    /// Whether the chosen height and rate exceed what software VP8 sustains.
    bool shareQualityDemanding() const;
    /// The same rule for an arbitrary combination, so a menu row can mark
    /// itself.
    Q_INVOKABLE bool shareQualityDemandingAt(int maxHeight, int fps) const;

    bool sharePresence() const;
    bool spacesRailVisible() const;
    bool spaceBannersVisible() const;
    bool spaceBannerExpanded() const;
    void setSpacesRailVisible(bool v);
    void setSpaceBannersVisible(bool v);
    void setSpaceBannerExpanded(bool v);
    bool roomListVisible() const;
    void setRoomListVisible(bool v);
    int roomListWidth() const;
    void setRoomListWidth(int px);
    int spacesRailWidth() const;
    void setSpacesRailWidth(int px);
    int spacesRailDepthStyle() const;
    void setSpacesRailDepthStyle(int style);
    int sidePanelWidth() const;
    void setSidePanelWidth(int px);
    bool closeToTray() const;
    void setCloseToTray(bool v);
    bool startInTray() const;
    void setStartInTray(bool v);
    QRect initialWindowGeometry() const { return m_initialWindowGeometry; }
    bool initialWindowMaximized() const { return m_initialWindowMaximized; }
    // Both refuse unrestorable values: Qt reports transient 0x0 geometry while
    // a window is shown, hidden to the tray or restored.
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height);
    Q_INVOKABLE void saveWindowMaximized(bool maximized);
    // Resize bounds, kept here so QML cannot drift from the clamp.
    static constexpr int kRoomListMinWidth = 200;
    static constexpr int kRoomListMaxWidth = 560;
    // Rail floor: a 40px tile with 14px margins. The ceiling limits how much
    // indentation a nested Space tree can use.
    static constexpr int kSpacesRailMinWidth = 68;
    static constexpr int kSpacesRailMaxWidth = 260;
    /// Rail depth styles. Clamped on read too: an unknown value (hand-edited,
    /// or from a newer build) must land on a style this build can draw.
    static constexpr int kRailDepthRegions = 0;
    static constexpr int kRailDepthClassic = 1;
    static constexpr int kMaxSpacesRailDepthStyle = kRailDepthClassic;
    static constexpr int kSidePanelMinWidth = 240;
    static constexpr int kSidePanelMaxWidth = 640;
    static constexpr int roomListMinWidth() { return kRoomListMinWidth; }
    static constexpr int roomListMaxWidth() { return kRoomListMaxWidth; }
    static constexpr int spacesRailMinWidth() { return kSpacesRailMinWidth; }
    static constexpr int spacesRailMaxWidth() { return kSpacesRailMaxWidth; }
    static constexpr int sidePanelMinWidth() { return kSidePanelMinWidth; }
    static constexpr int sidePanelMaxWidth() { return kSidePanelMaxWidth; }
    bool verificationWarningDismissed() const;
    void setVerificationWarningDismissed(bool v);
    void setSharePresence(bool v);
    // GIF browser policy.
    int gifAutoplay() const;
    void setGifAutoplay(int mode);
    int gifSafeSearch() const;
    void setGifSafeSearch(int rating);
    bool storeRecentGifs() const;
    void setStoreRecentGifs(bool v);
    QString gifPreferredProvider() const;
    void setGifPreferredProvider(const QString &id);
    bool showRoomActivity() const;
    void setShowRoomActivity(bool v);
    bool showMembershipEvents() const;
    void setShowMembershipEvents(bool v);
    bool showProfileChangeEvents() const;
    void setShowProfileChangeEvents(bool v);
    /// Whether attachments and loaded link previews render as a one-line
    /// summary with a disclosure control.
    bool collapseEmbeds() const;
    void setCollapseEmbeds(bool v);
    bool reducedMotion() const;
    bool smoothScrolling() const;
    void setSmoothScrolling(bool v);
    QStringList hiddenComposerButtons() const;
    void setHiddenComposerButtons(const QStringList &keys);
    /// Convenience for the settings checkboxes, which think in "shown".
    Q_INVOKABLE void setComposerButtonShown(const QString &key, bool shown);

    // Images this account has hidden in the timeline. Strictly account-scoped
    // with no global fallback. Bounded by MediaVisibilityStore's cap.
    QStringList hiddenMediaKeys() const;
    void setHiddenMediaKeys(const QStringList &keys);

    // ── Call volumes ──────────────────────────────────────────────────
    // Keyed by Matrix user id, never by SFU participant identity (which is per
    // device or per session). Strictly account-scoped with no global fallback.
    // 0..200; above 100 amplifies.

    /// This account's playback volume for one person, 0..200. 100 when unset.
    Q_INVOKABLE int callParticipantVolume(const QString &userId) const;
    /// A person's screen-share playback level. Separate from their microphone
    /// level, and keyed by user because share ids change on every restart.
    Q_INVOKABLE int callShareVolume(const QString &userId) const;
    Q_INVOKABLE void setCallShareVolume(const QString &userId, int percent);
    /// Setting exactly 100 removes the key, so reset is a real reset.
    Q_INVOKABLE void setCallParticipantVolume(const QString &userId,
                                              int percent);

    // Pop the call out into a small always-on-top window when the main window
    // is minimised or hidden to the tray. Default off.
    Q_PROPERTY(bool callPictureInPicture READ callPictureInPicture
                   WRITE setCallPictureInPicture
                   NOTIFY callPictureInPictureChanged)
    /// Own microphone gain, 0..200, applied to what others hear. Account-scoped
    /// with the global fallback: it describes this machine's hardware.
    Q_PROPERTY(int microphoneGain READ microphoneGain WRITE setMicrophoneGain
                   NOTIFY microphoneGainChanged)
    int microphoneGain() const;
    void setMicrophoneGain(int percent);

    void setReducedMotion(bool v);

    // Clock format ids.
    static constexpr int kClockFormatSystem = 0;
    static constexpr int kClockFormat12Hour = 1;
    static constexpr int kClockFormat24Hour = 2;
    int clockFormat() const;
    void setClockFormat(int mode);
    QString clockTimeFormat() const;

    bool enterInsertsNewline() const;
    void setEnterInsertsNewline(bool v);
    QString composerMode() const;
    bool spellCheckEnabled() const;
    void setSpellCheckEnabled(bool enabled);
    QString spellCheckLanguage() const;
    void setSpellCheckLanguage(const QString &tag);
    void setComposerMode(const QString &mode);
    bool sendTextAsCaption() const;
    void setSendTextAsCaption(bool v);

    // ── Rebindable keyboard shortcuts ────────────────────────────────────
    // Stored per action id as QKeySequence::PortableText, per account with a
    // global fallback. Empty means "use the default", as does an id outside
    // [A-Za-z0-9._-] (which could escape its QSettings group). ShortcutRegistry
    // owns defaults and validation; this is storage only.
    QString shortcutSequence(const QString &actionId) const;
    void setShortcutSequence(const QString &actionId, const QString &portable);
    void clearShortcutSequence(const QString &actionId);

    // Timeline wheel speed: 0 = Standard, 1 = Fast, 2 = Very fast. Out-of-range
    // values read back as Fast.
    static constexpr int kDefaultTimelineWheelSpeed = 1; // Fast
    static constexpr int kMinInterfaceZoom = 75;
    static constexpr int kMaxInterfaceZoom = 150;
    int interfaceZoom() const;
    void setInterfaceZoom(int percent);

    int timelineWheelSpeed() const;
    void setTimelineWheelSpeed(int v);

    qreal mediaVolume() const;
    void setMediaVolume(qreal v);
    qreal mediaPlaybackRate() const;
    void setMediaPlaybackRate(qreal v);

    QStringList recentEmoji() const;
    void recordRecentEmoji(const QString &emoji);
    void clearRecentEmoji();
    QString preferredEmojiTone() const;
    void setPreferredEmojiTone(const QString &tone);

    // Session storage. The access token lives in the SecretStore keyed by the
    // full user id; non-secret metadata is stored per account under
    // accounts/<slug>/, and accounts/active names the account shown. The
    // accessors below describe the active account.
    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

    // Multi-account registry. Records are keyed by safeUserSlug of the full
    // MXID; unsafe ids are rejected. Ordered oldest-added first.
    QStringList savedAccountUserIds() const;
    bool hasSavedAccount(const QString &userId) const;
    // True when a different saved account occupies this identity's slug. The
    // slug mapping is not injective, so such logins must be refused.
    bool accountSlugConflicts(const QString &userId) const;
    // Map a typed login identity onto the server-canonical user id already
    // saved for it. Localparts are case-sensitive, and the server's canonical
    // id may differ in case from what the user typed. Matches the server name
    // exactly and the localpart case-insensitively. Returns empty when nothing
    // or more than one account matches; `ambiguous` distinguishes the two. Kept
    // separate from slugForSavedAccount()/hasSavedAccount(), which must stay
    // exact.
    QString canonicalUserIdForTypedIdentity(const QString &typedUserId,
                                            bool *ambiguous = nullptr) const;
    // {userId, homeserver, deviceId, displayName, avatarUrl, addedAt}, or empty
    // when unknown. Never exposes the sync token.
    QVariantMap accountRecord(const QString &userId) const;
    // Access token for a specific saved account (SecretStore lookup).
    QString accessTokenFor(const QString &userId) const;
    // OAuth session material. Credentials kept in the SecretStore; never
    // exposed to QML. Empty is normal (password sessions often lack a refresh
    // token). updateSessionTokens() writes back tokens the SDK rotated and
    // touches nothing else.
    bool updateSessionTokens(const QString &userId,
                             const QString &accessToken,
                             const QString &refreshToken);
    QString refreshToken() const;
    QString refreshTokenFor(const QString &userId) const;
    QString oauthClientIdFor(const QString &userId) const;
    // "password" (matrix_auth) or "oauth". Kept in QSettings so restore can
    // route even when the keyring is unreadable. Older accounts report
    // "password".
    QString authTypeFor(const QString &userId) const;
    bool isOAuthAccount(const QString &userId) const;

    // Where this account's SDK store actually lives, as recorded at login.
    // Empty means never recorded. The path must never be re-derived: the typed
    // login name and the canonical user id can disagree (case, delegation).
    QString storeSlugFor(const QString &userId) const;
    // The saved account that could own store directory `storeSlug`, or empty.
    // Checks the canonical slug, the recorded storeSlug and the legacy
    // homeserver-derived slug. Anything that deletes a store must consult this.
    QString accountOwningStoreSlug(const QString &storeSlug) const;
    // True when the secret backend cannot answer at all (no store, locked
    // keyring, no session bus). Distinct from "this account has no token";
    // anything classifying a missing token must check this first.
    bool secretBackendUnavailable() const;
    /// Whether a miss could hide a secret this store cannot see. Destructive
    /// decisions key on this; see SecretStore::missesAreInconclusive().
    bool secretMissesAreInconclusive() const;
    void setStoreSlugFor(const QString &userId, const QString &storeSlug);
    // Resolve a saved account into an identity whose paths point at the
    // recorded store. Use wherever an account's files are read, deleted or
    // opened.
    bool resolveSavedIdentity(const QString &userId,
                              matrix::app_data::AccountIdentity *out) const;
    QString activeAccountUserId() const;
    // Selects which saved account the session accessors describe. An empty
    // id or an id without a saved record clears the selection.
    void setActiveAccountUserId(const QString &userId);
    // Cache the account's own display name / avatar for the account UI.
    void updateAccountProfile(const QString &userId,
                              const QString &displayName,
                              const QString &avatarUrl);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Screenshot demo only: register a fictional account as non-secret
    // metadata, never touching the SecretStore. `order` fixes addedAt for a
    // stable switcher order.
    void registerDemoAccount(const QString &homeserverUrl, const QString &userId,
                             const QString &displayName, const QString &avatarUrl,
                             int order);
    // Screenshot demo only: drop all fictional account records.
    void clearDemoAccounts();
#endif

    // True iff the process is using a native, secure secret backend.
    bool secretsAreSecure() const;
    QString secretBackendName() const;

    // `refreshToken` may be empty. `authType` is "password" or "oauth".
    // `oauthClientId` applies to OAuth accounts only. refreshToken and
    // oauthClientId are credentials: SecretStore only, never QSettings, QML or
    // logs.
    void saveSession(const QString &homeserverUrl,
                     const QString &userId,
                     const QString &deviceId,
                     const QString &accessToken,
                     const QString &refreshToken = QString(),
                     const QString &authType = QStringLiteral("password"),
                     const QString &oauthClientId = QString());
    void setSyncToken(const QString &token);
    // Clear the active session, including stale metadata whose token is
    // already absent. Returns false only when the account's SecretStore
    // entries could not be removed; non-secret metadata is still cleared.
    bool clearSession();

    // Shell keys that store Matrix objects (rail arrangement, collapsed Space
    // folders). Declared here so forgetDeviceGlobalAccountResidue() sweeps the
    // same spelling the stores write.
    static constexpr const char *kRailLayoutKey = "shell/railLayout";
    static constexpr const char *kChannelCollapsedKey = "shell/channelCollapsed";

    // Account-scoped clear used by signed-out reset. Always clears secrets for
    // `userId`; clears the global session metadata only if it belongs to it.
    bool clearSessionForAccount(const QString &userId);
    // Same, but reports whether a saved record was actually matched, since a
    // no-op secret clear also reports success.
    bool clearSessionForAccount(const QString &userId, bool *matchedRecord);

Q_SIGNALS:
    void homeserverUrlChanged();
    void loginHomeserverPrefillChanged();
    void themeChanged();
    void messageLayoutChanged();
    void roomNavigationLayoutChanged();
    void roomFilterModeChanged();
    void textScaleChanged();
    void uiFontChanged();
    void monoFontChanged();
    void importedFontFilesChanged();
    void languageChanged();
    void startMinimizedChanged();
    void customAppIconEnabledChanged();
    void notificationsEnabledChanged();
    void notificationPreviewChanged();
    void notificationPreviewEncryptedChanged();
    void callPictureInPictureChanged();
    void strictDeviceTrustChanged();
    void readReceiptModeChanged();
    void sendTypingNotificationsChanged();
    void notificationSoundChanged();
    void ringForCallsChanged();
    void callSoundSettingsChanged();
    void callDevicePreferenceChanged();
    void roomNotificationModeChanged(const QString &roomId);
    void autoLoadLinkPreviewsChanged();
    void shareQualityChanged();
    void loadPreviewsInEncryptedRoomsChanged();
    void animateGifPreviewsChanged();
    void sharePresenceChanged();
    void spacesRailVisibleChanged();
    void spaceBannersVisibleChanged();
    void spaceBannerExpandedChanged();
    void roomListVisibleChanged();
    void roomListWidthChanged();
    void spacesRailWidthChanged();
    void spacesRailDepthStyleChanged();
    void sidePanelWidthChanged();
    void closeToTrayChanged();
    void startInTrayChanged();
    void verificationWarningDismissedChanged();
    void gifAutoplayChanged();
    void gifSafeSearchChanged();
    void storeRecentGifsChanged();
    void gifPreferredProviderChanged();
    void showRoomActivityChanged();
    void showMembershipEventsChanged();
    void showProfileChangeEventsChanged();
    void collapseEmbedsChanged();
    void reducedMotionChanged();
    void smoothScrollingChanged();
    void hiddenComposerButtonsChanged();
    void microphoneGainChanged();
    /// One person's stored volume changed. Carries the USER ID.
    void callParticipantVolumeChanged(const QString &userId, int percent);
    void callShareVolumeChanged(const QString &userId, int percent);
    void clockFormatChanged();
    void enterInsertsNewlineChanged();
    void composerModeChanged();
    void spellCheckEnabledChanged();
    void spellCheckLanguageChanged();
    void sendTextAsCaptionChanged();
    void timelineWheelSpeedChanged();
    void mediaVolumeChanged();
    void mediaPlaybackRateChanged();
    void interfaceZoomChanged();
    void sessionChanged();
    void secretBackendChanged();
    // A saved-account record was added, removed, or updated.
    void accountsChanged();

private:
    // Per-account appearance state; friends so the private helpers stay
    // private.
    friend class CustomThemeStore;
    friend class RailLayoutStore;
    friend class SpaceChannelModel;

    void migratePlaintextTokenIfPresent();
    void migrateInsecureSecretsGroup();
    void migrateLegacySessionRecord();
    // Per-account appearance storage: reads prefer the active account, writes
    // update the account and the global fallback used by the logged-out shell.
    QVariant appearanceValue(const char *globalKey,
                             const QVariant &fallback) const;
    void setAppearanceValue(const char *globalKey, const QVariant &value);
    // Strictly per-account storage for values that name Matrix objects (room
    // ids, user-typed labels), which must not be mirrored into a device-global
    // copy a new account would inherit. Writes go to the account key alone
    // while an account is active; reads still fall back to the global key as
    // the migration source for pre-upgrade data, which
    // forgetDeviceGlobalAccountResidue() sweeps once no account remains.
    QVariant accountScopedValue(const char *globalKey,
                                const QVariant &fallback) const;
    void setAccountScopedValue(const char *globalKey, const QVariant &value);
    // Called when the last saved account is removed: drops the device-global
    // keys that name Matrix rooms/Spaces.
    void forgetDeviceGlobalAccountResidue();
    QString accountKey(const QString &slug, const char *subKey) const;
    QString slugForSavedAccount(const QString &userId) const;
    // Per-room notification-mode keys: the account-scoped key (empty when
    // no account is active) and the legacy device-global fallback key.
    static QString roomNotificationModeGlobalKey(const QString &roomId);
    QString roomNotificationModeScopedKey(const QString &roomId) const;
    // Learned-media store: one LRU index covers dimension and size keys.
    static QString mediaInfoIndexKeyForSlug(const QString &slug);
    void touchMediaInfoIndex(const QString &slug, const QString &hash);
    // roomNotificationMode() runs for every appended remote event, so the
    // active slug is cached keyed by user id. Every removal path clears the
    // active id, and setActiveAccountUserId clears the cache explicitly.
    QString activeAccountSlugCached() const;
    // Reads and validates the stored window geometry once, from the
    // constructor.
    void loadWindowGeometry();
    // Main.qml's minimums, duplicated because validation runs before any window
    // exists.
    static constexpr int kWindowMinWidth = 640;
    static constexpr int kWindowMinHeight = 420;
    bool upsertAccountRecord(const QString &userId,
                             const QString &homeserver,
                             const QString &deviceId);

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
    // Captured once in the constructor; see the CONSTANT properties.
    QRect m_initialWindowGeometry;
    bool m_initialWindowMaximized = false;
    mutable QString m_activeSlugCacheUserId;
    mutable QString m_activeSlugCache;
};
