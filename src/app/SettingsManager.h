#pragma once

#include "storage/AppDataPaths.h"

#include <QHash>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <utility>

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
    Q_PROPERTY(int layoutPreset READ layoutPreset WRITE setLayoutPreset
                   NOTIFY layoutPresetChanged)
    Q_PROPERTY(bool layoutPresetCustomized READ layoutPresetCustomized
                   NOTIFY layoutPresetChanged)
    Q_PROPERTY(int messageLayout READ messageLayout WRITE setMessageLayout
                   NOTIFY messageLayoutChanged)
    Q_PROPERTY(int roomNavigationLayout READ roomNavigationLayout
                   WRITE setRoomNavigationLayout
                   NOTIFY roomNavigationLayoutChanged)
    Q_PROPERTY(int roomFilterMode READ roomFilterMode WRITE setRoomFilterMode
                   NOTIFY roomFilterModeChanged)
    Q_PROPERTY(int textScale READ textScale WRITE setTextScale
                   NOTIFY textScaleChanged)
    Q_PROPERTY(QString uiFont READ uiFont WRITE setUiFont
                   NOTIFY uiFontChanged)
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
    Q_PROPERTY(int notificationPreview READ notificationPreview
                   WRITE setNotificationPreview NOTIFY notificationPreviewChanged)
    // Separate preview level for encrypted rooms: a notification body lands in
    // the notification daemon, its log and possibly a lock screen, outside the
    // room's encryption. Values are PreviewMode (0-2) plus 3 = FollowGeneral,
    // the default.
    Q_PROPERTY(int notificationPreviewEncrypted READ notificationPreviewEncrypted
                   WRITE setNotificationPreviewEncrypted
                   NOTIFY notificationPreviewEncryptedChanged)
    Q_PROPERTY(bool strictDeviceTrust READ strictDeviceTrust
                   WRITE setStrictDeviceTrust NOTIFY strictDeviceTrustChanged)
    Q_PROPERTY(int activityReadHorizonDays READ activityReadHorizonDays
                   WRITE setActivityReadHorizonDays
                   NOTIFY activityReadHorizonDaysChanged)
    Q_PROPERTY(bool labsConcurrentAccounts READ labsConcurrentAccounts
                   WRITE setLabsConcurrentAccounts
                   NOTIFY labsConcurrentAccountsChanged)
    Q_PROPERTY(bool labsConcurrentAccountsWarmSlot
                   READ labsConcurrentAccountsWarmSlot
                   WRITE setLabsConcurrentAccountsWarmSlot
                   NOTIFY labsConcurrentAccountsWarmSlotChanged)
    // Keep media the user OPENS between sessions: the SDK media store
    // (encrypted with the account's key, rust/src/mediastore.rs), plus plain
    // kept files for what it refuses by size, never from an encrypted room.
    // Encrypted-room media is kept only under a key a secure keyring holds.
    // On by default: the SDK store always kept media. Attachments the user
    // SENDS are kept by matrix-sdk's send queue whatever this says.
    Q_PROPERTY(bool keepMediaOnDevice READ keepMediaOnDevice
                   WRITE setKeepMediaOnDevice NOTIFY keepMediaOnDeviceChanged)
    Q_PROPERTY(bool developerTools READ developerTools
                   WRITE setDeveloperTools NOTIFY developerToolsChanged)
    Q_PROPERTY(bool alwaysAskWhereToSave READ alwaysAskWhereToSave
                   WRITE setAlwaysAskWhereToSave
                   NOTIFY alwaysAskWhereToSaveChanged)
    Q_PROPERTY(QString downloadFolder READ downloadFolder
                   WRITE setDownloadFolder NOTIFY downloadFolderChanged)
    Q_PROPERTY(int readReceiptMode READ readReceiptMode
                   WRITE setReadReceiptMode NOTIFY readReceiptModeChanged)
    Q_PROPERTY(bool sendTypingNotifications READ sendTypingNotifications
                   WRITE setSendTypingNotifications
                   NOTIFY sendTypingNotificationsChanged)
    Q_PROPERTY(int notificationSound READ notificationSound
                   WRITE setNotificationSound NOTIFY notificationSoundChanged)
    Q_PROPERTY(int notificationSoundSource READ notificationSoundSource
                   WRITE setNotificationSoundSource
                   NOTIFY notificationSoundSourceChanged)
    Q_PROPERTY(int notificationSoundVolume READ notificationSoundVolume
                   WRITE setNotificationSoundVolume
                   NOTIFY notificationSoundVolumeChanged)
    Q_PROPERTY(bool ringForCalls READ ringForCalls WRITE setRingForCalls
                   NOTIFY ringForCallsChanged)
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
    Q_PROPERTY(QString preferredMicrophoneId READ preferredMicrophoneId
                   WRITE setPreferredMicrophoneId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredSpeakerId READ preferredSpeakerId
                   WRITE setPreferredSpeakerId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredCameraId READ preferredCameraId
                   WRITE setPreferredCameraId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(bool autoLoadLinkPreviews READ autoLoadLinkPreviews
                   WRITE setAutoLoadLinkPreviews NOTIFY autoLoadLinkPreviewsChanged)
    Q_PROPERTY(bool loadPreviewsInEncryptedRooms READ loadPreviewsInEncryptedRooms
                   WRITE setLoadPreviewsInEncryptedRooms
                   NOTIFY loadPreviewsInEncryptedRoomsChanged)
    Q_PROPERTY(bool animateGifPreviews READ animateGifPreviews
                   WRITE setAnimateGifPreviews NOTIFY animateGifPreviewsChanged)
    Q_PROPERTY(bool showLinkMediaInline READ showLinkMediaInline
                   WRITE setShowLinkMediaInline
                   NOTIFY showLinkMediaInlineChanged)
    Q_PROPERTY(int shareMaxHeight READ shareMaxHeight WRITE setShareMaxHeight
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(int shareFps READ shareFps WRITE setShareFps
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(int shareBitrateCapKbps READ shareBitrateCapKbps
                   WRITE setShareBitrateCapKbps NOTIFY shareQualityChanged)
    Q_PROPERTY(int cameraResolution READ cameraResolution
                   WRITE setCameraResolution NOTIFY cameraQualityChanged)
    Q_PROPERTY(bool echoCancellation READ echoCancellation
                   WRITE setEchoCancellation NOTIFY voiceProcessingChanged)
    Q_PROPERTY(bool autoGainControl READ autoGainControl
                   WRITE setAutoGainControl NOTIFY voiceProcessingChanged)
    Q_PROPERTY(bool shareQualityDemanding READ shareQualityDemanding
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(bool sharePresence READ sharePresence
                   WRITE setSharePresence NOTIFY sharePresenceChanged)
    Q_PROPERTY(bool gameActivityEnabled READ gameActivityEnabled
                   WRITE setGameActivityEnabled NOTIFY gameActivityChanged)
    Q_PROPERTY(QStringList gameActivityHidden READ gameActivityHidden
                   WRITE setGameActivityHidden NOTIFY gameActivityChanged)
    Q_PROPERTY(QStringList gameActivityCustom READ gameActivityCustom
                   WRITE setGameActivityCustom NOTIFY gameActivityChanged)
    Q_PROPERTY(bool musicActivityEnabled READ musicActivityEnabled
                   WRITE setMusicActivityEnabled NOTIFY musicActivityChanged)
    Q_PROPERTY(bool spacesRailVisible READ spacesRailVisible
                   WRITE setSpacesRailVisible NOTIFY spacesRailVisibleChanged)
    Q_PROPERTY(bool spaceBannersVisible READ spaceBannersVisible
                   WRITE setSpaceBannersVisible NOTIFY spaceBannersVisibleChanged)
    Q_PROPERTY(bool keepRoomListOrderStill READ keepRoomListOrderStill
                   WRITE setKeepRoomListOrderStill
                   NOTIFY keepRoomListOrderStillChanged)
    Q_PROPERTY(int roomListSort READ roomListSort WRITE setRoomListSort
                   NOTIFY roomListSortChanged)
    Q_PROPERTY(int railSort READ railSort WRITE setRailSort
                   NOTIFY railSortChanged)
    Q_PROPERTY(bool spaceBannerExpanded READ spaceBannerExpanded
                   WRITE setSpaceBannerExpanded NOTIFY spaceBannerExpandedChanged)
    Q_PROPERTY(bool roomListVisible READ roomListVisible
                   WRITE setRoomListVisible NOTIFY roomListVisibleChanged)
    Q_PROPERTY(bool memberPanelOpen READ memberPanelOpen
                   WRITE setMemberPanelOpen NOTIFY memberPanelOpenChanged)
    Q_PROPERTY(int roomListWidth READ roomListWidth
                   WRITE setRoomListWidth NOTIFY roomListWidthChanged)
    Q_PROPERTY(int spacesRailWidth READ spacesRailWidth
                   WRITE setSpacesRailWidth NOTIFY spacesRailWidthChanged)
    Q_PROPERTY(int spacesRailDepthStyle READ spacesRailDepthStyle
                   WRITE setSpacesRailDepthStyle
                   NOTIFY spacesRailDepthStyleChanged)
    Q_PROPERTY(int sidePanelWidth READ sidePanelWidth
                   WRITE setSidePanelWidth NOTIFY sidePanelWidthChanged)
    Q_PROPERTY(int roomListMinWidth READ roomListMinWidth CONSTANT)
    Q_PROPERTY(int roomListExpandedMinWidth READ roomListExpandedMinWidth CONSTANT)
    Q_PROPERTY(int roomListSnapThreshold READ roomListSnapThreshold CONSTANT)
    Q_PROPERTY(int roomListMaxWidth READ roomListMaxWidth CONSTANT)
    Q_PROPERTY(int sidePanelMinWidth READ sidePanelMinWidth CONSTANT)
    Q_PROPERTY(int sidePanelMaxWidth READ sidePanelMaxWidth CONSTANT)
    Q_PROPERTY(int spacesRailMinWidth READ spacesRailMinWidth CONSTANT)
    Q_PROPERTY(int spacesRailMaxWidth READ spacesRailMaxWidth CONSTANT)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray
                   NOTIFY closeToTrayChanged)
    Q_PROPERTY(bool globalShortcutsEnabled READ globalShortcutsEnabled
                   WRITE setGlobalShortcutsEnabled
                   NOTIFY globalShortcutsEnabledChanged)
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
    Q_PROPERTY(int settingsReadProblem READ settingsReadProblem CONSTANT)
    Q_PROPERTY(bool verificationWarningDismissed
                   READ verificationWarningDismissed
                   WRITE setVerificationWarningDismissed
                   NOTIFY verificationWarningDismissedChanged)
    Q_PROPERTY(int gifAutoplay READ gifAutoplay WRITE setGifAutoplay
                   NOTIFY gifAutoplayChanged)
    Q_PROPERTY(bool videoPrefetchEnabled READ videoPrefetchEnabled
                   WRITE setVideoPrefetchEnabled
                   NOTIFY videoPrefetchEnabledChanged)
    Q_PROPERTY(int videoPrefetchMaxMb READ videoPrefetchMaxMb
                   WRITE setVideoPrefetchMaxMb NOTIFY videoPrefetchMaxMbChanged)
    Q_PROPERTY(int videoPrefetchMinMb READ videoPrefetchMinMb CONSTANT)
    Q_PROPERTY(int videoPrefetchLimitMb READ videoPrefetchLimitMb CONSTANT)
    Q_PROPERTY(int gifSafeSearch READ gifSafeSearch WRITE setGifSafeSearch
                   NOTIFY gifSafeSearchChanged)
    Q_PROPERTY(bool storeRecentGifs READ storeRecentGifs
                   WRITE setStoreRecentGifs NOTIFY storeRecentGifsChanged)
    Q_PROPERTY(QString gifPreferredProvider READ gifPreferredProvider
                   WRITE setGifPreferredProvider NOTIFY gifPreferredProviderChanged)
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    Q_PROPERTY(bool showMembershipEvents READ showMembershipEvents
                   WRITE setShowMembershipEvents
                   NOTIFY showMembershipEventsChanged)
    Q_PROPERTY(bool showProfileChangeEvents READ showProfileChangeEvents
                   WRITE setShowProfileChangeEvents
                   NOTIFY showProfileChangeEventsChanged)
    Q_PROPERTY(bool collapseEmbeds READ collapseEmbeds WRITE setCollapseEmbeds
                   NOTIFY collapseEmbedsChanged)
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion
                   NOTIFY reducedMotionChanged)

    Q_PROPERTY(bool highContrast READ highContrast WRITE setHighContrast
                   NOTIFY highContrastChanged)
    Q_PROPERTY(bool underlineLinks READ underlineLinks WRITE setUnderlineLinks
                   NOTIFY underlineLinksChanged)
    Q_PROPERTY(int nameColorCorrection READ nameColorCorrection
                   WRITE setNameColorCorrection
                   NOTIFY nameColorCorrectionChanged)
    Q_PROPERTY(int messageSpacing READ messageSpacing WRITE setMessageSpacing
                   NOTIFY messageSpacingChanged)
    Q_PROPERTY(int threadStyle READ threadStyle WRITE setThreadStyle
                   NOTIFY threadStyleChanged)
    Q_PROPERTY(int threadPlacement READ threadPlacement
                   WRITE setThreadPlacement NOTIFY threadPlacementChanged)
    Q_PROPERTY(QString callFormat READ callFormat WRITE setCallFormat
                   NOTIFY callFormatChanged)
    Q_PROPERTY(bool hideThreadReplies READ hideThreadReplies
                   WRITE setHideThreadReplies NOTIFY hideThreadRepliesChanged)

    Q_PROPERTY(int conversationReturnMode READ conversationReturnMode
                   WRITE setConversationReturnMode
                   NOTIFY conversationReturnModeChanged)
    Q_PROPERTY(bool showSelectionOnReturn READ showSelectionOnReturn
                   WRITE setShowSelectionOnReturn
                   NOTIFY showSelectionOnReturnChanged)

    Q_PROPERTY(bool smoothScrolling READ smoothScrolling WRITE setSmoothScrolling
                   NOTIFY smoothScrollingChanged)
    Q_PROPERTY(bool showBridgeBadges READ showBridgeBadges
                   WRITE setShowBridgeBadges NOTIFY showBridgeBadgesChanged)
    Q_PROPERTY(bool compactRoomList READ compactRoomList
                   WRITE setCompactRoomList NOTIFY compactRoomListChanged)
    Q_PROPERTY(bool disableCursorBlink READ disableCursorBlink
                   WRITE setDisableCursorBlink NOTIFY disableCursorBlinkChanged)
    Q_PROPERTY(QStringList hiddenComposerButtons READ hiddenComposerButtons
                   WRITE setHiddenComposerButtons
                   NOTIFY hiddenComposerButtonsChanged)
    Q_PROPERTY(int clockFormat READ clockFormat WRITE setClockFormat
                   NOTIFY clockFormatChanged)
    Q_PROPERTY(QString clockTimeFormat READ clockTimeFormat
                   NOTIFY clockFormatChanged)
    Q_PROPERTY(bool enterInsertsNewline READ enterInsertsNewline
                   WRITE setEnterInsertsNewline
                   NOTIFY enterInsertsNewlineChanged)
    Q_PROPERTY(QString composerMode READ composerMode WRITE setComposerMode
                   NOTIFY composerModeChanged)
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled
                   WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(QString spellCheckLanguage READ spellCheckLanguage
                   WRITE setSpellCheckLanguage NOTIFY spellCheckLanguageChanged)
    Q_PROPERTY(QStringList spellCheckExtraLanguages READ spellCheckExtraLanguages
                   WRITE setSpellCheckExtraLanguages
                   NOTIFY spellCheckExtraLanguagesChanged)
    Q_PROPERTY(int autocorrectMode READ autocorrectMode WRITE setAutocorrectMode
                   NOTIFY autocorrectModeChanged)
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption
                   NOTIFY sendTextAsCaptionChanged)
    Q_PROPERTY(QString imageMetadataMode READ imageMetadataMode
                   WRITE setImageMetadataMode
                   NOTIFY imageMetadataModeChanged)
    Q_PROPERTY(bool emojiShortcodeAutoConvert READ emojiShortcodeAutoConvert
                   WRITE setEmojiShortcodeAutoConvert
                   NOTIFY emojiShortcodeAutoConvertChanged)
    Q_PROPERTY(int timelineWheelSpeed READ timelineWheelSpeed
                   WRITE setTimelineWheelSpeed NOTIFY timelineWheelSpeedChanged)
    Q_PROPERTY(qreal mediaVolume READ mediaVolume WRITE setMediaVolume
                   NOTIFY mediaVolumeChanged)
    Q_PROPERTY(qreal mediaPlaybackRate READ mediaPlaybackRate
                   WRITE setMediaPlaybackRate NOTIFY mediaPlaybackRateChanged)
    Q_PROPERTY(int voiceMiniPlayerCorner READ voiceMiniPlayerCorner
                   WRITE setVoiceMiniPlayerCorner
                   NOTIFY voiceMiniPlayerCornerChanged)
    Q_PROPERTY(bool voicePlayerDocked READ voicePlayerDocked
                   WRITE setVoicePlayerDocked NOTIFY voicePlayerDockedChanged)
    Q_PROPERTY(bool largeAlbumArt READ largeAlbumArt WRITE setLargeAlbumArt
                   NOTIFY largeAlbumArtChanged)
    Q_PROPERTY(QRectF voiceMiniPlayerGeometry READ voiceMiniPlayerGeometry
                   WRITE setVoiceMiniPlayerGeometry
                   NOTIFY voiceMiniPlayerGeometryChanged)
    Q_PROPERTY(qreal voicePlaybackRate READ voicePlaybackRate
                   WRITE setVoicePlaybackRate NOTIFY voicePlaybackRateChanged)
    Q_PROPERTY(int interfaceZoom READ interfaceZoom WRITE setInterfaceZoom
                   NOTIFY interfaceZoomChanged)
    Q_PROPERTY(int textRendering READ textRendering WRITE setTextRendering
                   NOTIFY textRenderingChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString secretBackendName READ secretBackendName NOTIFY secretBackendChanged)
    Q_PROPERTY(bool secretsAreSecure READ secretsAreSecure NOTIFY secretBackendChanged)
    Q_PROPERTY(bool secretsArePortable READ secretsArePortable NOTIFY secretBackendChanged)

public:
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,
        DarkTheme = 2,
        GraphiteTheme = 3,
        MidnightBlueTheme = 4,
        NordTheme = 5,
        PurpleDuskTheme = 6,
        WarmTheme = 7,
        MossLightTheme = 8,
        IndigoNightTheme = 9,
        DeepTealTheme = 10,
        StormTheme = 11,
        CustomTheme = 12,
    };
    Q_ENUM(Theme)

    enum SettingsReadProblem {
        NoReadProblem = 0,
        SettingsUnreadable = 1,
        AccountsMissing = 2,
    };
    Q_ENUM(SettingsReadProblem)
    int settingsReadProblem() const { return m_settingsReadProblem; }

    static constexpr int kMaxThemeId = CustomTheme;

    explicit SettingsManager(QObject *parent = nullptr);
    ~SettingsManager() override;

    static constexpr int kFlushDelayMs = 1000;
    bool hasUnflushedWrites() const { return m_flushTimer.isActive(); }
    void flushPendingWrites();
    Q_INVOKABLE void holdWritesWhileDragging(bool held);
    bool writesHeld() const { return m_writesHeld; }
    static constexpr int kDragHoldMaxMs = 30000;

    bool eventFilter(QObject *watched, QEvent *event) override;

    // Inject the process-wide SecretStore. Call once, right after
    // construction. Scopes a store shared with other installs to this one,
    // migrates any plaintext access token from QSettings into the store, and
    // copies sessions saved before install scoping into this install's items.
    void setSecretStore(SecretStore *store);
    SecretStore *secretStore() const { return m_secretStore; }

    QString homeserverUrl() const;
    void setHomeserverUrl(const QString &url);

    QString loginHomeserverPrefill() const;
    void setLoginHomeserverPrefill(const QString &url);

    Theme theme() const;
    void setTheme(Theme t);

    static constexpr int kMaxMessageLayout = 3;
    static constexpr int kMessageLayoutSmallAvatars = 3;
    static constexpr int kLayoutPresetLightning = 0;
    static constexpr int kLayoutPresetNhekoCompact = 1;
    int layoutPreset() const;
    void setLayoutPreset(int preset);
    bool layoutPresetCustomized() const;
    Q_INVOKABLE void resetLayoutPresetOverrides();
    Q_INVOKABLE bool presetSuppliesValue(const QString &property) const;
    QVariant lightningPresentationValue(const QString &property) const;
    bool setLightningPresentationValue(const QString &property,
                                       const QVariant &value);
    static constexpr int kMaxRoomNavigationLayout = 1;
    int messageLayout() const;
    int roomNavigationLayout() const;
    void setRoomNavigationLayout(int layout);
    void setMessageLayout(int layout);
    int roomFilterMode() const;
    void setRoomFilterMode(int mode);

    static constexpr int kMinTextScale = 75;
    static constexpr int kMaxTextScale = 150;
    int textScale() const;
    QString uiFont() const;
    void setUiFont(const QString &family);
    QString monoFont() const;
    void setMonoFont(const QString &family);
    Q_INVOKABLE static QStringList uiFontChoices();
    static QString acceptableFontFamily(const QString &family);

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
    QString noiseSuppressionMode() const;
    void setNoiseSuppressionMode(const QString &mode);
    int callShareAudioMode() const;
    void setCallShareAudioMode(int mode);
    QStringList callShareAudioApps() const;
    void setCallShareAudioApps(const QStringList &keys);
    bool strictDeviceTrust() const;
    void setStrictDeviceTrust(bool v);
    static constexpr int kDefaultActivityReadHorizonDays = 60;
    static constexpr int kMaxActivityReadHorizonDays = 3650;
    int activityReadHorizonDays() const;
    void setActivityReadHorizonDays(int days);
    bool labsConcurrentAccounts() const;
    void setLabsConcurrentAccounts(bool on);
    bool labsConcurrentAccountsWarmSlot() const;
    void setLabsConcurrentAccountsWarmSlot(bool on);
    bool keepMediaOnDevice() const;
    bool developerTools() const;
    void setDeveloperTools(bool v);
    void setKeepMediaOnDevice(bool v);
    bool alwaysAskWhereToSave() const;
    void setAlwaysAskWhereToSave(bool v);
    QString downloadFolder() const;
    void setDownloadFolder(const QString &path);
    int readReceiptMode() const;
    void setReadReceiptMode(int v);
    bool sendTypingNotifications() const;
    void setSendTypingNotifications(bool v);
    int notificationSound() const;
    void setNotificationSound(int mode);
    static constexpr int kDefaultNotificationSoundVolume = 70;
    int notificationSoundSource() const;
    void setNotificationSoundSource(int source);
    int notificationSoundVolume() const;
    void setNotificationSoundVolume(int percent);
    bool ringForCalls() const;
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
    QString preferredCameraDescription() const;
    void setPreferredCameraDescription(const QString &description);
    void setRingForCalls(bool enabled);
    void setNotificationPreview(int mode);
    Q_INVOKABLE int roomNotificationMode(const QString &roomId) const;
    int roomUnreadMode(const QString &roomId) const;
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    bool roomNotificationModeUnsynced(const QString &roomId) const;
    int roomNotificationModeUnsyncedBase(const QString &roomId) const;
    void setRoomNotificationModeUnsynced(const QString &roomId, bool unsynced,
                                         int serverBase = -1);
    QStringList unsyncedRoomNotificationModes() const;
    bool roomNotificationModeDeviceOnly(const QString &roomId) const;
    /// True only when a mode key is stored for the room (account or legacy
    /// device key). roomNotificationMode() returns 0 for an absent key, so
    /// this is the only way to tell "all messages" from "never configured".
    bool hasStoredRoomNotificationMode(const QString &roomId) const;
    int roomNotificationModeFor(const QString &userId,
                                const QString &roomId) const;
    bool roomNotificationModeUnsyncedFor(const QString &userId,
                                         const QString &roomId) const;
    int roomNotificationModeUnsyncedBaseFor(const QString &userId,
                                            const QString &roomId) const;
    bool roomNotificationModeDeviceOnlyFor(const QString &userId,
                                           const QString &roomId) const;
    bool hasStoredRoomNotificationModeFor(const QString &userId,
                                          const QString &roomId) const;
    int roomUnreadModeFor(const QString &userId, const QString &roomId) const;
    bool roomNotificationModeDecidesLocallyFor(const QString &userId,
                                               const QString &roomId) const;
    QStringList accountRoomNotificationModeRooms() const;
    QVariantMap ownPresenceStatus() const;
    void setOwnPresenceStatus(const QVariantMap &status);
    QVariantList scheduledSends() const;
    void setScheduledSends(const QVariantList &rows);
    QVariantMap activityState() const;
    void setActivityState(const QVariantMap &state);

    QVariantMap roomDraft(const QString &draftKey) const;
    void setRoomDraft(const QString &draftKey, const QVariantMap &draft);

    Q_INVOKABLE QSize knownVideoDimensions(const QString &mediaKey) const;
    void setKnownVideoDimensions(const QString &mediaKey, int width,
                                 int height);
    Q_INVOKABLE double knownMediaSizeBytes(const QString &mediaKey) const;
    void setKnownMediaSizeBytes(const QString &mediaKey, qint64 bytes);

    Q_INVOKABLE int pickerWidthShare(const QString &id) const;
    Q_INVOKABLE int pickerHeightShare(const QString &id) const;
    Q_INVOKABLE void setPickerShare(const QString &id, int widthPerMille,
                                    int heightPerMille);
    void setNotificationsEnabled(bool v);

    bool autoLoadLinkPreviews() const;

    void setAutoLoadLinkPreviews(bool v);
    bool loadPreviewsInEncryptedRooms() const;
    void setLoadPreviewsInEncryptedRooms(bool v);
    bool animateGifPreviews() const;
    void setAnimateGifPreviews(bool v);
    bool showLinkMediaInline() const;
    void setShowLinkMediaInline(bool v);
    int shareMaxHeight() const;
    void setShareMaxHeight(int v);
    int effectiveShareMaxHeight() const;
    int shareFps() const;
    void setShareFps(int v);
    int effectiveShareFps() const;
    int shareBitrateCapKbps() const;
    void setShareBitrateCapKbps(int v);
    int cameraResolution() const;
    void setCameraResolution(int v);
    int effectiveCameraResolution() const;
    bool echoCancellation() const;
    void setEchoCancellation(bool v);
    bool autoGainControl() const;
    void setAutoGainControl(bool v);
    bool shareQualityDemanding() const;
    Q_INVOKABLE bool shareQualityDemandingAt(int maxHeight, int fps) const;

    bool sharePresence() const;
    bool gameActivityEnabled() const;
    void setGameActivityEnabled(bool v);
    QStringList gameActivityHidden() const;
    void setGameActivityHidden(const QStringList &names);
    QStringList gameActivityCustom() const;
    void setGameActivityCustom(const QStringList &entries);
    bool musicActivityEnabled() const;
    void setMusicActivityEnabled(bool v);
    bool spacesRailVisible() const;
    bool spaceBannersVisible() const;
    bool keepRoomListOrderStill() const;
    void setKeepRoomListOrderStill(bool v);
    int roomListSort() const;
    void setRoomListSort(int mode);
    bool roomListSortChosen() const;
    int roomListSortForModels() const;
    int railSort() const;
    void setRailSort(int mode);
    bool spaceBannerExpanded() const;
    void setSpacesRailVisible(bool v);
    void setSpaceBannersVisible(bool v);
    void setSpaceBannerExpanded(bool v);
    bool roomListVisible() const;
    void setRoomListVisible(bool v);
    bool memberPanelOpen() const;
    void setMemberPanelOpen(bool v);
    int roomListWidth() const;
    void setRoomListWidth(int px);
    Q_INVOKABLE int roomListExpandedWidth() const;
    Q_INVOKABLE void setRoomListExpandedWidth(int px);
    Q_INVOKABLE QUrl lastAttachFolder() const;
    Q_INVOKABLE void rememberAttachFolder(const QUrl &file);
    QUrl lastFileFolder(const QString &purpose) const;
    void rememberFileFolder(const QString &purpose, const QUrl &folder);
    int spacesRailWidth() const;
    void setSpacesRailWidth(int px);
    int spacesRailDepthStyle() const;
    void setSpacesRailDepthStyle(int style);
    int sidePanelWidth() const;
    void setSidePanelWidth(int px);
    bool closeToTray() const;
    void setCloseToTray(bool v);
    bool globalShortcutsEnabled() const;
    void setGlobalShortcutsEnabled(bool v);
    bool startInTray() const;
    void setStartInTray(bool v);
    QRect initialWindowGeometry() const { return m_initialWindowGeometry; }
    bool initialWindowMaximized() const { return m_initialWindowMaximized; }
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height);
    Q_INVOKABLE void saveWindowMaximized(bool maximized);
    static constexpr int kRoomListMinWidth = 64;
    static constexpr int kRoomListExpandedMinWidth = 212;
    static constexpr int kRoomListSnapThreshold = 156;
    static constexpr int kRoomListMaxWidth = 560;
    static constexpr int kSpacesRailMinWidth = 68;
    static constexpr int kCompactSpacesRailMinWidth = 62;
    static constexpr int kSpacesRailMaxWidth = 260;
    static constexpr int kRailDepthRegions = 0;
    static constexpr int kRailDepthClassic = 1;
    static constexpr int kMaxSpacesRailDepthStyle = kRailDepthClassic;
    static constexpr int kSidePanelMinWidth = 240;
    static constexpr int kSidePanelMaxWidth = 640;
    static constexpr int roomListMinWidth() { return kRoomListMinWidth; }
    static constexpr int roomListExpandedMinWidth() { return kRoomListExpandedMinWidth; }
    static constexpr int roomListSnapThreshold() { return kRoomListSnapThreshold; }
    static constexpr int roomListMaxWidth() { return kRoomListMaxWidth; }
    static constexpr int spacesRailMinWidth() { return kSpacesRailMinWidth; }
    static constexpr int spacesRailMaxWidth() { return kSpacesRailMaxWidth; }
    static constexpr int sidePanelMinWidth() { return kSidePanelMinWidth; }
    static constexpr int sidePanelMaxWidth() { return kSidePanelMaxWidth; }
    bool verificationWarningDismissed() const;
    void setVerificationWarningDismissed(bool v);
    void setSharePresence(bool v);
    int gifAutoplay() const;
    void setGifAutoplay(int mode);
    int gifSafeSearch() const;
    void setGifSafeSearch(int rating);
    bool storeRecentGifs() const;
    bool videoPrefetchEnabled() const;
    void setVideoPrefetchEnabled(bool v);
    int videoPrefetchMaxMb() const;
    void setVideoPrefetchMaxMb(int mb);
    static constexpr int videoPrefetchMinMb() { return 1; }
    static constexpr int videoPrefetchLimitMb() { return 100; }
    static constexpr int videoPrefetchDefaultMb() { return 8; }
    void setStoreRecentGifs(bool v);
    QString gifPreferredProvider() const;
    void setGifPreferredProvider(const QString &id);
    bool showRoomActivity() const;
    void setShowRoomActivity(bool v);
    bool showMembershipEvents() const;
    void setShowMembershipEvents(bool v);
    bool showProfileChangeEvents() const;
    void setShowProfileChangeEvents(bool v);
    bool collapseEmbeds() const;
    void setCollapseEmbeds(bool v);
    bool reducedMotion() const;
    bool highContrast() const;
    void setHighContrast(bool v);
    bool underlineLinks() const;
    void setUnderlineLinks(bool v);
    static constexpr int kNameColorStrong = 0;
    static constexpr int kNameColorWeak = 1;
    static constexpr int kNameColorOff = 2;
    int nameColorCorrection() const;
    void setNameColorCorrection(int mode);
    static constexpr int kSpacingCompact = 0;
    static constexpr int kSpacingCozy = 1;
    static constexpr int kSpacingRoomy = 2;
    int messageSpacing() const;
    void setMessageSpacing(int mode);
    static constexpr int kThreadStylePanel = 0;
    static constexpr int kThreadStyleInline = 1;
    int threadStyle() const;
    void setThreadStyle(int style);
    static constexpr int kThreadPlacementSide = 0;
    static constexpr int kThreadPlacementMain = 1;
    int threadPlacement() const;
    void setThreadPlacement(int placement);

    QString callFormat() const;
    void setCallFormat(const QString &format);
    bool hideThreadReplies() const;
    void setHideThreadReplies(bool hide);
    bool smoothScrolling() const;
    void setSmoothScrolling(bool v);
    static constexpr int kReturnPrevious = 0;
    static constexpr int kReturnLatest = 1;
    static constexpr int kReturnSelected = 2;
    int conversationReturnMode() const;
    void setConversationReturnMode(int mode);
    bool showSelectionOnReturn() const;
    void setShowSelectionOnReturn(bool v);
    bool showBridgeBadges() const;
    void setShowBridgeBadges(bool show);
    bool compactRoomList() const;
    void setCompactRoomList(bool compact);
    bool disableCursorBlink() const;
    void setDisableCursorBlink(bool disabled);
    QStringList hiddenComposerButtons() const;
    void setHiddenComposerButtons(const QStringList &keys);
    Q_INVOKABLE void setComposerButtonShown(const QString &key, bool shown);

    QStringList hiddenMediaKeys() const;
    void setHiddenMediaKeys(const QStringList &keys);

    Q_INVOKABLE int callParticipantVolume(const QString &userId) const;
    Q_INVOKABLE int callShareVolume(const QString &userId) const;
    Q_INVOKABLE void setCallShareVolume(const QString &userId, int percent);
    Q_INVOKABLE void setCallParticipantVolume(const QString &userId,
                                              int percent);

    Q_PROPERTY(bool callPictureInPicture READ callPictureInPicture
                   WRITE setCallPictureInPicture
                   NOTIFY callPictureInPictureChanged)
    Q_PROPERTY(int microphoneGain READ microphoneGain WRITE setMicrophoneGain
                   NOTIFY microphoneGainChanged)
    Q_PROPERTY(QString noiseSuppressionMode READ noiseSuppressionMode
                   WRITE setNoiseSuppressionMode
                   NOTIFY noiseSuppressionModeChanged)
    int microphoneGain() const;
    void setMicrophoneGain(int percent);

    void setReducedMotion(bool v);

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
    QStringList spellCheckExtraLanguages() const;
    void setSpellCheckExtraLanguages(const QStringList &tags);
    int autocorrectMode() const;
    void setAutocorrectMode(int mode);
    void setComposerMode(const QString &mode);
    bool sendTextAsCaption() const;
    void setSendTextAsCaption(bool v);
    QString imageMetadataMode() const;
    void setImageMetadataMode(const QString &mode);
    bool emojiShortcodeAutoConvert() const;
    void setEmojiShortcodeAutoConvert(bool v);

    QString shortcutSequence(const QString &actionId) const;
    void setShortcutSequence(const QString &actionId, const QString &portable);
    void clearShortcutSequence(const QString &actionId);

    static constexpr int kDefaultTimelineWheelSpeed = 1;
    static constexpr int kMinInterfaceZoom = 75;
    static constexpr int kMaxInterfaceZoom = 150;
    static constexpr int kTextRenderingSmooth = 0;
    static constexpr int kTextRenderingNative = 1;
    static constexpr auto kTextRenderingKey = "ui/textRendering";
    static int textRenderingFromValue(const QVariant &stored);
    int textRendering() const;
    void setTextRendering(int mode);
    int interfaceZoom() const;
    void setInterfaceZoom(int percent);

    int timelineWheelSpeed() const;
    void setTimelineWheelSpeed(int v);

    qreal mediaVolume() const;
    void setMediaVolume(qreal v);
    qreal mediaPlaybackRate() const;
    void setMediaPlaybackRate(qreal v);
    int voiceMiniPlayerCorner() const;
    void setVoiceMiniPlayerCorner(int corner);
    bool voicePlayerDocked() const;
    bool largeAlbumArt() const;
    Q_INVOKABLE void setLargeAlbumArt(bool large);
    void setVoicePlayerDocked(bool docked);
    QRectF voiceMiniPlayerGeometry() const;
    void setVoiceMiniPlayerGeometry(const QRectF &geometry);
    qreal voicePlaybackRate() const;
    void setVoicePlaybackRate(qreal v);

    QStringList recentEmoji() const;
    void recordRecentEmoji(const QString &emoji);
    void setRecentEmoji(const QStringList &mru);
    void clearRecentEmoji();
    QString preferredEmojiTone() const;
    void setPreferredEmojiTone(const QString &tone);

    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

    QStringList savedAccountUserIds() const;
    bool hasSavedAccount(const QString &userId) const;
    bool accountSlugConflicts(const QString &userId) const;
    QString canonicalUserIdForTypedIdentity(const QString &typedUserId,
                                            bool *ambiguous = nullptr) const;
    QVariantMap accountRecord(const QString &userId) const;
    QString indexAllOfferAnswer(const QString &userId) const;
    void setIndexAllOfferAnswer(const QString &userId, const QString &answer);
    QString lastOpenedRoom(const QString &userId) const;
    void setLastOpenedRoom(const QString &userId, const QString &roomId);
    QString accessTokenFor(const QString &userId) const;
    bool updateSessionTokens(const QString &userId,
                             const QString &accessToken,
                             const QString &refreshToken);
    QString refreshToken() const;
    QString refreshTokenFor(const QString &userId) const;
    QString oauthClientIdFor(const QString &userId) const;
    QString authTypeFor(const QString &userId) const;
    bool isOAuthAccount(const QString &userId) const;

    // Where this account's SDK store actually lives, as recorded at login.
    // Empty means never recorded. The path must never be re-derived: the typed
    // login name and the canonical user id can disagree (case, delegation).
    QString storeSlugFor(const QString &userId) const;
    QString accountOwningStoreSlug(const QString &storeSlug) const;
    // True when the secret backend cannot answer at all (no store, locked
    // keyring, no session bus). Distinct from "this account has no token";
    // anything classifying a missing token must check this first.
    bool secretBackendUnavailable() const;
    bool secretMissesAreInconclusive() const;
    bool secretsWrittenHereForRecordedDevice(const QString &userId) const;
    QString keyringBackendFor(const QString &userId) const;
    bool keyringItemsAdoptedUnverified(const QString &userId) const;
    bool keyringItemsDeclinedForOtherInstall(const QString &userId) const;
    void confirmAdoptedKeyringItems(const QString &userId);
    bool declineAdoptedKeyringItems(const QString &userId,
                                    const QString &checkedDeviceId);
    bool keyringBackendChanged(const QString &userId) const;
    void setStoreSlugFor(const QString &userId, const QString &storeSlug);
    bool resolveSavedIdentity(const QString &userId,
                              matrix::app_data::AccountIdentity *out) const;
    QString activeAccountUserId() const;
    void setActiveAccountUserId(const QString &userId);
    void updateAccountProfile(const QString &userId,
                              const QString &displayName,
                              const QString &avatarUrl);

    static constexpr int kMaxLiveAccounts = 5;
    static constexpr int kMaxPinnedAccounts = 4;
    enum class KeepRunningResult : quint8 { Ok, Unchanged, RefusedCap, RefusedNoRecord };
    KeepRunningResult setKeepRunningFor(const QString &userId, bool on);
    bool keepRunningFor(const QString &userId) const;
    QVariantMap networkRouteRecord(const QString &userId) const;
    bool setNetworkRouteRecord(const QString &userId, const QVariantMap &record);
    bool networkRouteConfigured(const QString &userId) const;
    QStringList keepRunningUserIds() const;
    qint64 lastLiveAtFor(const QString &userId) const;
    void setLastLiveAtFor(const QString &userId, qint64 unixMs);
    QStringList locallyDecidedNotificationRoomsFor(const QString &userId) const;
    bool keyringItemsDirectlyReadableFor(const QString &userId) const;
    void noteSessionTokensWrittenElsewhere(const QString &userId);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    void registerDemoAccount(const QString &homeserverUrl, const QString &userId,
                             const QString &displayName, const QString &avatarUrl,
                             int order);
    void clearDemoAccounts();
#endif

    bool secretsAreSecure() const;
    bool secretsArePortable() const;
    QString secretBackendName() const;

    void saveSession(const QString &homeserverUrl,
                     const QString &userId,
                     const QString &deviceId,
                     const QString &accessToken,
                     const QString &refreshToken = QString(),
                     const QString &authType = QStringLiteral("password"),
                     const QString &oauthClientId = QString());
    void setSyncToken(const QString &token);
    bool clearSession();

    static constexpr const char *kRailLayoutKey = "shell/railLayout";
    static constexpr const char *kChannelCollapsedKey = "shell/channelCollapsed";

    bool clearSessionForAccount(const QString &userId);
    bool clearSessionForAccount(const QString &userId, bool *matchedRecord);

    // A sign-out that began and has not yet deleted its store (B044). Written
    // (and flushed) before the server logout, cleared once the store is gone,
    // so a quit, crash or kill in between leaves a record the next start
    // finishes. It names the store the session opened, never a secret. A new
    // sign-in of the account (saveSession) supersedes it.
    void recordPendingSignOut(const matrix::app_data::AccountIdentity &identity);
    void clearPendingSignOut(const QString &userId);
    QList<matrix::app_data::AccountIdentity> pendingSignOuts();

    // A fresh sign-in whose store the SDK is about to create, before the
    // server has accepted the password (R6). Written and flushed before the
    // store exists, under the identity as TYPED (a bare username's stand-in,
    // the typed casing). Cleared when the sign-in finishes: by the attempt
    // itself (clearPendingSignIn(attempt)), by saveSession() for an
    // identical user id, and by setStoreSlugFor() for the store it records;
    // and dropped at every start that finds its store gone (a failed attempt
    // rolls it back). Its only reader is the start-up check behind the
    // settingsReadProblem property: a store left by a cancelled, quit or
    // killed sign-in is not an account the settings file lost. Names a store,
    // never a secret.
    void recordPendingSignIn(const matrix::app_data::AccountIdentity &identity);
    void clearPendingSignIn(const QString &userId);
    void clearPendingSignIn(const matrix::app_data::AccountIdentity &attempt);

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
    void noiseSuppressionModeChanged();
    void strictDeviceTrustChanged();
    void activityReadHorizonDaysChanged();
    void labsConcurrentAccountsChanged();
    void labsConcurrentAccountsWarmSlotChanged();
    void keepMediaOnDeviceChanged();
    void developerToolsChanged();
    void alwaysAskWhereToSaveChanged();
    void downloadFolderChanged();
    void readReceiptModeChanged();
    void sendTypingNotificationsChanged();
    void notificationSoundChanged();
    void notificationSoundSourceChanged();
    void notificationSoundVolumeChanged();
    void ringForCallsChanged();
    void callSoundSettingsChanged();
    void callDevicePreferenceChanged();
    void roomNotificationModeChanged(const QString &roomId);
    void autoLoadLinkPreviewsChanged();
    void shareQualityChanged();
    void cameraQualityChanged();
    void voiceProcessingChanged();
    void loadPreviewsInEncryptedRoomsChanged();
    void animateGifPreviewsChanged();
    void showLinkMediaInlineChanged();
    void sharePresenceChanged();
    void gameActivityChanged();
    void musicActivityChanged();
    void spacesRailVisibleChanged();
    void spaceBannersVisibleChanged();
    void keepRoomListOrderStillChanged();
    void roomListSortChanged();
    void railSortChanged();
    void spaceBannerExpandedChanged();
    void roomListVisibleChanged();
    void memberPanelOpenChanged();
    void roomListWidthChanged();
    void spacesRailWidthChanged();
    void spacesRailDepthStyleChanged();
    void sidePanelWidthChanged();
    void closeToTrayChanged();
    void globalShortcutsEnabledChanged();
    void startInTrayChanged();
    void verificationWarningDismissedChanged();
    void gifAutoplayChanged();
    void gifSafeSearchChanged();
    void storeRecentGifsChanged();
    void videoPrefetchEnabledChanged();
    void videoPrefetchMaxMbChanged();
    void gifPreferredProviderChanged();
    void showRoomActivityChanged();
    void showMembershipEventsChanged();
    void showProfileChangeEventsChanged();
    void collapseEmbedsChanged();
    void reducedMotionChanged();
    void highContrastChanged();
    void underlineLinksChanged();
    void nameColorCorrectionChanged();
    void messageSpacingChanged();
    void threadStyleChanged();
    void threadPlacementChanged();
    void layoutPresetChanged();
    void callFormatChanged();
    void hideThreadRepliesChanged();
    void smoothScrollingChanged();
    void conversationReturnModeChanged();
    void showSelectionOnReturnChanged();
    void showBridgeBadgesChanged();
    void compactRoomListChanged();
    void disableCursorBlinkChanged();
    void hiddenComposerButtonsChanged();
    void microphoneGainChanged();
    void callParticipantVolumeChanged(const QString &userId, int percent);
    void callShareVolumeChanged(const QString &userId, int percent);
    void clockFormatChanged();
    void enterInsertsNewlineChanged();
    void composerModeChanged();
    void spellCheckEnabledChanged();
    void spellCheckLanguageChanged();
    void spellCheckExtraLanguagesChanged();
    void autocorrectModeChanged();
    void sendTextAsCaptionChanged();
    void imageMetadataModeChanged();
    void emojiShortcodeAutoConvertChanged();
    void timelineWheelSpeedChanged();
    void mediaVolumeChanged();
    void mediaPlaybackRateChanged();
    void voiceMiniPlayerCornerChanged();
    void voicePlayerDockedChanged();
    void largeAlbumArtChanged();
    void voiceMiniPlayerGeometryChanged();
    void voicePlaybackRateChanged();
    void interfaceZoomChanged();
    void textRenderingChanged();
    void sessionChanged();
    void secretBackendChanged();
    void accountsChanged();
    void keepRunningChanged(const QString &userId);
    void networkRouteChanged(const QString &userId);

private:
    friend class CustomThemeStore;
    friend class RailLayoutStore;
    friend class ChatBackdropController;
    friend class SpaceChannelModel;

    void migratePlaintextTokenIfPresent();
    void migrateInsecureSecretsGroup();
    void migrateLegacySessionRecord();
    void migratePrefetchFromAnimation();
    void migrateComposerModeToMarkdown();
    QSet<QString> pendingSignInStorePaths();
    void dropPendingSignInsNaming(const QString &storePath);
    QString keyringInstallId();
    void adoptLegacyKeyringSessions();
    bool keyringItemsAreCurrent(const QString &slug) const;
    void markKeyringItems(const QString &slug, const char *how) const;
    void noteKeyringBackend(const QString &userId) const;
    mutable QSet<QString> m_keyringBackendChangeLogged;
    QString keyringBackendForStore(const QString &userId) const;
    std::pair<QString, QString> m_keyringBackendBeingCleared;
    enum class LegacyAdoption { Adopted, CopyFailed, NotAdoptable, Unreadable };
    LegacyAdoption adoptLegacyKeyringSession(
        const QString &userId, QHash<QString, QString> *legacyOut) const;
    QString readAccountSecret(const QString &userId, const char *key) const;
    QVariant appearanceValue(const char *globalKey,
                             const QVariant &fallback) const;
    void setAppearanceValue(const char *globalKey, const QVariant &value);
    bool threadPlacementIsExplicit() const;
    QString paneWidthKey(const char *key) const;
    QVariant paneWidthValue(const char *key, int fallback) const;
    void announcePaneWidthsIfMoved(int roomList, int rail, int sidePanel);
    QVariant appearanceValueUnannounced(const char *globalKey,
                                        const QVariant &fallback) const
    { return appearanceValue(globalKey, fallback); }
    void setAppearanceValueUnannounced(const char *globalKey,
                                       const QVariant &value)
    { setAppearanceValue(globalKey, value); }
    QVariant accountScopedValue(const char *globalKey,
                                const QVariant &fallback) const;
    void setAccountScopedValue(const char *globalKey, const QVariant &value);
    void forgetDeviceGlobalAccountResidue();
    QString accountKey(const QString &slug, const char *subKey) const;
    QString slugForSavedAccount(const QString &userId) const;
    static QString roomNotificationModeGlobalKey(const QString &roomId);
    QString roomNotificationModeScopedKey(const QString &roomId) const;
    QString roomNotificationScopePrefix() const;
    static QString scopePrefixFor(const QString &slug);
    static QString scopedKeyFor(const QString &slug, const QString &roomId);
    int roomNotificationModeInScope(const QString &slug,
                                    const QString &roomId) const;
    bool roomNotificationModeUnsyncedInScope(const QString &slug,
                                             const QString &roomId) const;
    int roomNotificationModeUnsyncedBaseInScope(const QString &slug,
                                                const QString &roomId) const;
    bool roomNotificationModeDeviceOnlyInScope(const QString &slug,
                                               const QString &roomId) const;
    bool hasStoredRoomNotificationModeInScope(const QString &slug,
                                              const QString &roomId) const;
    int roomUnreadModeInScope(const QString &slug, const QString &roomId) const;
    static QString mediaInfoIndexKeyForSlug(const QString &slug);
    void touchMediaInfoIndex(const QString &slug, const QString &hash);
    // roomNotificationMode() runs for every appended remote event, so the
    // active slug is cached keyed by user id. Every removal path clears the
    // active id, and setActiveAccountUserId clears the cache explicitly.
    QString activeAccountSlugCached() const;
    QString slugForAccountScope(const QString &userId) const;
    void loadWindowGeometry();
    static constexpr int kWindowMinWidth = 640;
    static constexpr int kWindowMinHeight = 420;
    bool upsertAccountRecord(const QString &userId,
                             const QString &homeserver,
                             const QString &deviceId);

    std::unique_ptr<QSettings> m_store;
    QTimer m_flushTimer;
    QTimer m_holdTimer;
    bool m_writesHeld = false;
    SecretStore *m_secretStore = nullptr;
    QRect m_initialWindowGeometry;
    bool m_initialWindowMaximized = false;
    int m_settingsReadProblem = NoReadProblem;
    mutable QString m_activeSlugCacheUserId;
    mutable QString m_activeSlugCache;
};
