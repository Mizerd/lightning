import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    property string renamingSessionDeviceId: ""
    objectName: "settingsScreenRoot"
    // A minted recovery key must not stay in memory once the user leaves the
    // card, including on a section change: this screen is a warm Loader kept
    // alive across opens. An armed destructive confirmation is cleared too, or
    // coming back later would turn a two-press action into one.
    function forgetTransientBackupState() {
        if (app.backup)
            app.backup.dismissRecoveryKey()
        if (root.backupCardRef)
            root.backupCardRef.pendingConfirm = ""
    }
    onVisibleChanged: {
        if (!visible) {
            root.forgetTransientBackupState()
            root.keptSections = []
            root.anchorCache = ({})
            if (settingsSearchField)
                settingsSearchField.text = ""
            if (root.nameColorPickerRef)
                root.nameColorPickerRef.open = false
        } else {
            Qt.callLater(root.ensurePageFocus)
        }
    }
    property bool shortcutRowsBuilt: false
    Timer {
        interval: 1500
        running: !root.shortcutRowsBuilt
        onTriggered: root.shortcutRowsBuilt = true
    }
    onSectionChanged: {
        root.anchorCache = ({})
        if (root.keptSections.indexOf(root.section) < 0)
            root.keptSections = root.keptSections.concat([root.section])
        if (root.section === "shortcuts")
            root.shortcutRowsBuilt = true
        root.setOutlineExpanded(root.section, true)
        root.forgetTransientBackupState()
        root.outlinePinned = -1
        root.scheduleOutlineRefresh()
    }

    property bool showRecoveryDiagnostics: false
    property bool storedMediaClearing: false
    property string storedMediaStatus: ""

    // FontManager, or null. `fonts` is a context property from main.cpp that
    // several suites do not install, and `visible: false` does not stop
    // bindings being created, so every reference goes through this guarded
    // alias.
    readonly property var fontManager:
        (typeof fonts !== "undefined") ? fonts : null

    property string settingsSearchQuery: ""
    readonly property var timelineVisibilityChoices: [
        { label: qsTr("Show"), value: 0 },
        { label: qsTr("On hover"), value: 1 },
        { label: qsTr("Hide"), value: 2 }
    ]
    readonly property var searchIndex: [
        { title: qsTr("Account"), keywords: QT_TR_NOOP("account profile"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "accountIdentityCard" },
        { title: qsTr("Change password"),
          keywords: QT_TR_NOOP("password change account security"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "changePasswordCard" },
        { title: qsTr("Network and proxy"),
          keywords: QT_TR_NOOP("network proxy socks http connection tor"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "networkRouteCard" },
        { title: qsTr("Homeserver"), keywords: QT_TR_NOOP("homeserver server url"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "homeserverField" },
        { title: qsTr("Start minimized"), keywords: QT_TR_NOOP("startup minimized"),
          section: "account", breadcrumb: qsTr("Account · Startup"),
          anchor: "startMinimizedCheck" },

        { title: qsTr("Theme"),
          keywords: QT_TR_NOOP("theme moss indigo teal light dark graphite midnight nordic purple warm"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "featuredThemeFlow" },
        { title: qsTr("Match system light/dark"),
          keywords: QT_TR_NOOP("match system auto theme"), section: "appearance",
          breadcrumb: qsTr("Appearance · Theme"), control: "matchSystem",
          anchor: "matchSystemSwitch" },
        { title: qsTr("Layout preset"),
          keywords: QT_TR_NOOP("layout preset nheko compact density dense lightning "
                         + "one click"),
          section: "layout", breadcrumb: qsTr("Layout · Messages and threads"),
          anchor: "layoutPresetControl" },
        { title: qsTr("Message layout"),
          keywords: QT_TR_NOOP("message layout modern bubbles compact small avatars"),
          section: "layout", breadcrumb: qsTr("Layout · Messages and threads"),
          control: "messageLayout",
          anchor: "messageLayoutControl" },
        { title: qsTr("Thread style"),
          keywords: QT_TR_NOOP("thread threads style panel inline nheko element interleaved"),
          section: "layout", breadcrumb: qsTr("Layout · Messages and threads"),
          anchor: "threadStyleControl" },
        { title: qsTr("Threads open in"),
          keywords: QT_TR_NOOP("thread threads open placement main view side panel "
                         + "column full width nheko element"),
          section: "layout", breadcrumb: qsTr("Layout · Messages and threads"),
          anchor: "threadPlacementControl" },
        { title: qsTr("Message spacing"),
          keywords: QT_TR_NOOP("message spacing density compact cozy roomy gap padding"),
          section: "layout", breadcrumb: qsTr("Layout · Messages and threads"),
          anchor: "messageSpacingControl" },
        { title: qsTr("Chat background"),
          keywords: QT_TR_NOOP("chat background wallpaper picture image photo backdrop "
                         + "room conversation own set custom others shared hide"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Background and depth"),
          anchor: "chatBackgroundSettingsSection" },
        { title: qsTr("Gradients"),
          keywords: QT_TR_NOOP("gradient gradients linear radial fade glow two-tone "
                         + "colour color custom theme background room list "
                         + "rail surface"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Custom colours and gradients"),
          anchor: "customThemeGradientButton" },
        { title: qsTr("Custom theme"),
          keywords: QT_TR_NOOP("custom theme own colours colors palette editor "
                         + "create make build accent"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Custom colours and gradients"),
          anchor: "customThemeRow" },
        { title: qsTr("Depth"),
          keywords: QT_TR_NOOP("depth gradient shading shadow flat surfaces 3d"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Background and depth"),
          anchor: "surfaceDepthControl" },
        { title: qsTr("Spaces rail depth"),
          keywords: QT_TR_NOOP("spaces rail depth space bar sidebar nesting regions "
                         + "classic old style flat tint indent"),
          section: "layout",
          breadcrumb: qsTr("Layout · Room list and panels"),
          control: "spacesRailDepth",
          anchor: "spacesRailDepthControl" },
        { title: qsTr("Text size"), keywords: QT_TR_NOOP("text size font scale"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "textScaleSlider" },
        { title: qsTr("Interface zoom"),
          keywords: QT_TR_NOOP("interface zoom scale bigger ui size"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "interfaceZoomSlider" },
        { title: qsTr("Text rendering"),
          keywords: QT_TR_NOOP("text rendering anti-aliasing antialiasing aliasing "
                         + "smooth hinting font native blurry fuzzy sharp "
                         + "subpixel fontconfig"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "textRenderingCombo" },
        { title: qsTr("Font"), keywords: QT_TR_NOOP("font family typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "uiFontSelector" },
        { title: qsTr("Code font"),
          keywords: QT_TR_NOOP("code font monospace mono fixed width typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance · Font"),
          anchor: "monoFontCombo" },
        { title: qsTr("Your own fonts"),
          keywords: QT_TR_NOOP("import font file ttf otf install custom typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance · Font"),
          anchor: "importedFontCard" },
        { title: qsTr("Language"), keywords: QT_TR_NOOP("language locale"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "languageCombo" },
        { title: qsTr("Show room activity"),
          keywords: QT_TR_NOOP("room activity membership joins leaves profile"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "showRoomActivity",
          anchor: "showRoomActivityCheck" },
        { title: qsTr("Mouse-wheel speed"),
          keywords: QT_TR_NOOP("wheel speed scroll timeline"), section: "chat",
          breadcrumb: qsTr("Chat · Timeline"),
          anchor: "timelineWheelSpeedCombo" },
        { title: qsTr("When returning to a conversation"),
          keywords: QT_TR_NOOP("return reopen room position latest selected message "
                         + "reading restore remember scroll"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          anchor: "conversationReturnCombo" },
        { title: qsTr("Show the message selection outline when returning"),
          keywords: QT_TR_NOOP("selection outline highlight return reopen keyboard"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          anchor: "showSelectionOnReturnCheck" },
        { title: qsTr("Joins, leaves and invites"),
          keywords: QT_TR_NOOP("membership join leave invite kick ban activity hide"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "showMembershipEvents",
          anchor: "showMembershipEventsCheck" },
        { title: qsTr("Display name and avatar changes"),
          keywords: QT_TR_NOOP("profile change display name avatar activity hide"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "showProfileChangeEvents",
          anchor: "showProfileChangeEventsCheck" },
        { title: qsTr("Collapse media and link embeds"),
          keywords: QT_TR_NOOP("embed embeds collapse collapsed compact single line "
                         + "clutter declutter media image picture gif sticker "
                         + "video audio voice file attachment link preview "
                         + "expand arrow"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "collapseEmbeds",
          anchor: "collapseEmbedsCheck" },
        { title: qsTr("Avatars in messages"),
          keywords: QT_TR_NOOP("avatar avatars picture profile sender hide hover "
                         + "gutter declutter"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          anchor: "timelineAvatarsCombo" },
        { title: qsTr("Reactions"),
          keywords: QT_TR_NOOP("reaction reactions emoji chips hide hover declutter"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          anchor: "timelineReactionsCombo" },
        { title: qsTr("Read receipts"),
          keywords: QT_TR_NOOP("read receipts seen faces avatars hide hover declutter"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          anchor: "timelineReadReceiptsCombo" },
        { title: qsTr("Show emoji-only messages large"),
          keywords: QT_TR_NOOP("big large emoji jumbo size"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "largeEmoji",
          anchor: "largeEmojiCheck" },
        { title: qsTr("Go to the newest message when I send"),
          keywords: QT_TR_NOOP("send jump scroll bottom latest newest stay place history"),
          section: "chat", breadcrumb: qsTr("Chat · Timeline"),
          control: "jumpToNewestOnSend",
          anchor: "jumpToNewestOnSendCheck" },
        { title: qsTr("Reduce motion"),
          keywords: QT_TR_NOOP("reduced motion animation accessibility vestibular"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          control: "reducedMotion",
          anchor: "reducedMotionCheck" },
        { title: qsTr("Animated images"),
          keywords: QT_TR_NOOP("gif gifs autoplay animated animation avatar avatars "
                         + "banner sticker stickers emoji webp background play "
                         + "hover pause stop focus cpu battery resources"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          anchor: "gifAutoplayCombo" },
        { title: qsTr("Increase contrast"),
          keywords: QT_TR_NOOP("high contrast accessibility borders text readable vision"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Accessibility"),
          anchor: "highContrastCheck" },
        { title: qsTr("Always underline links"),
          keywords: QT_TR_NOOP("underline links mentions accessibility colour blind"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Accessibility"),
          anchor: "underlineLinksCheck" },
        { title: qsTr("Name colour readability"),
          keywords: QT_TR_NOOP("name color colour contrast sender readable strong weak off"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Accessibility"),
          anchor: "nameColorCorrectionControl" },
        { title: qsTr("Disable cursor blinking"),
          keywords: QT_TR_NOOP("text cursor caret blink steady accessibility"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          anchor: "disableCursorBlinkCheck" },
        { title: qsTr("Smooth scrolling"),
          keywords: QT_TR_NOOP("smooth scrolling scroll wheel glide animation instant jumpy mouse"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          control: "smoothScrolling",
          anchor: "smoothScrollingCheck" },
        { title: qsTr("Clock"),
          keywords: QT_TR_NOOP("clock 24 hour time format am pm timestamp"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          anchor: "clockFormatCombo" },
        { title: qsTr("Show Space banners"),
          keywords: QT_TR_NOOP("space banner header image hide show"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          control: "spaceBannersVisible",
          anchor: "spaceBannersVisibleCheck" },
        { title: qsTr("Show bridge badges in the room list"),
          keywords: QT_TR_NOOP("bridge network badge chip tag whatsapp rcs sms telegram room list hide"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "showBridgeBadgesCheck" },
        { title: qsTr("Compact room list"),
          keywords: QT_TR_NOOP("room list compact one line preview last message hide dense"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "compactRoomListCheck" },
        { title: qsTr("Conversation list width"),
          keywords: QT_TR_NOOP("room list width panel size sidebar"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "roomListWidthSlider" },
        { title: qsTr("Keep the room list still while I use it"),
          keywords: QT_TR_NOOP("room list order sort reorder jump move still hold "
                         + "freeze stable recent activity conversation list "
                         + "sidebar channels new message"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          control: "keepRoomListOrderStill",
          anchor: "keepRoomListOrderStillCheck" },
        { title: qsTr("Keep running in the tray"),
          keywords: QT_TR_NOOP("tray system tray minimize close background icon "
                         + "taskbar quit close to tray menu bar"),
          section: "layout", breadcrumb: qsTr("Layout · System tray"),
          anchor: "closeToTrayCheck", requiresTray: true },
        { title: qsTr("Start in the tray"),
          keywords: QT_TR_NOOP("tray start startup minimized background icon "
                         + "menu bar hidden"),
          section: "layout", breadcrumb: qsTr("Layout · System tray"),
          anchor: "startInTrayCheck", requiresTray: true },
        { title: qsTr("Side panel width"),
          keywords: QT_TR_NOOP("side panel width members threads size"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "sidePanelWidthSlider" },
        { title: qsTr("Enter starts a new line"),
          keywords: QT_TR_NOOP("enter newline send composer message box return"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          control: "enterInsertsNewline",
          anchor: "enterInsertsNewlineCheck" },
        { title: qsTr("Send text with an attachment as its caption"),
          keywords: QT_TR_NOOP("caption attachment upload text description"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          control: "sendTextAsCaption",
          anchor: "sendTextAsCaptionCheck" },
        { title: qsTr("Emoji and sticker packs"),
          keywords: QT_TR_NOOP("emoji sticker pack custom emote use everywhere "
                         + "room space export import zip share"),
          section: "chat",
          breadcrumb: qsTr("Chat · Emoji and sticker packs"),
          anchor: "stickerPacksSettings" },
        { title: qsTr("Convert :shortcode: to emoji"),
          keywords: QT_TR_NOOP("emoji shortcode colon convert autoconvert "
                         + "auto-convert thumbsup smiley slack discord"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          control: "emojiShortcodeAutoConvert",
          anchor: "emojiShortcodeAutoConvertCheck" },
        { title: qsTr("Message box buttons"),
          keywords: QT_TR_NOOP("composer buttons hide show emoji gif sticker stickers "
                         + "voice microphone formatting schedule send later "
                         + "declutter simplify"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "composerButtonsHeading" },
        { title: qsTr("Check spelling as you type"),
          keywords: QT_TR_NOOP("spell spelling checker dictionary typo underline language"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "spellCheckEnabledCheck" },
        { title: qsTr("Spelling language"),
          keywords: QT_TR_NOOP("spell spelling language dictionary automatic system"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "spellLanguageCombo" },
        { title: qsTr("Also check"),
          keywords: QT_TR_NOOP("spell spelling languages several multiple second extra dictionary bilingual"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "spellExtraLanguageCombo" },
        { title: qsTr("Correct obvious typos as you type"),
          keywords: QT_TR_NOOP("autocorrect auto correct autocorrection typo fix replace capital"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "autocorrectCheck" },
        { title: qsTr("Personal dictionary"),
          keywords: QT_TR_NOOP("spell spelling dictionary personal words added learned remove"),
          section: "chat", breadcrumb: qsTr("Chat · Message box"),
          anchor: "personalDictionaryHeading" },

        { title: qsTr("Stream Info"),
          keywords: QT_TR_NOOP("call video screen share codec encoder bitrate FPS debug overlay"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "shortcutsHeading" },
        { title: qsTr("Keyboard shortcuts"),
          keywords: QT_TR_NOOP("keyboard shortcut shortcuts key keys binding rebind hotkey"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "shortcutsHeading" },
        { title: qsTr("Use call keys from any window"),
          keywords: QT_TR_NOOP("call keys global shortcuts mute deafen leave hotkey desktop portal"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "globalShortcutsCheck", platform: "linux" },
        { title: qsTr("Reset all shortcuts"),
          keywords: QT_TR_NOOP("reset shortcuts default keys"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "shortcutResetAllButton" },
        { title: qsTr("Bold, italic and code keys"),
          keywords: QT_TR_NOOP("bold italic strikethrough code quote list formatting keys"),
          section: "shortcuts",
          breadcrumb: qsTr("Keyboard shortcuts · Message formatting"),
          anchor: "shortcutRow_composer.bold" },

        { title: qsTr("Microphone"),
          keywords: QT_TR_NOOP("microphone mic input device voice call audio sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          control: "callDevice_microphone",
          anchor: "callDevice_microphone" },
        { title: qsTr("Microphone volume"),
          keywords: QT_TR_NOOP("microphone volume gain mic input level loud quiet boost amplify sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          anchor: "microphoneGainSlider" },
        { title: qsTr("Microphone test"),
          keywords: QT_TR_NOOP("microphone mic test check try level meter hear "
                         + "myself yourself echo loopback working"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          anchor: "microphoneTestSection" },
        { title: qsTr("Noise suppression"),
          keywords: QT_TR_NOOP("noise suppression cancellation denoise background "
                         + "microphone mic fan keyboard"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          anchor: "soundNoiseSuppressionRow" },
        { title: qsTr("Output device"),
          keywords: QT_TR_NOOP("speaker output headphones headset device voice call audio sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Output"),
          control: "callDevice_speaker",
          anchor: "callDevice_speaker" },
        { title: qsTr("Media playback volume"),
          keywords: QT_TR_NOOP("volume sound audio video voice message playback level media loud"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Media"),
          anchor: "mediaVolumeSettingSlider" },
        { title: qsTr("Audio player position"),
          keywords: QT_TR_NOOP("audio player voice message music now playing strip dock docked pop out floating mini player"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Media"),
          anchor: "voicePlayerDockedCheck" },
        { title: qsTr("Album art"),
          keywords: QT_TR_NOOP("album art cover artwork audio file music big large square compact"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Media playback"),
          anchor: "albumArtStyleCombo" },
        { title: qsTr("Camera"),
          keywords: QT_TR_NOOP("camera webcam video device call"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Camera"),
          control: "callDevice_camera",
          anchor: "callDevice_camera" },
        { title: qsTr("Call sounds"),
          keywords: QT_TR_NOOP("call sounds join leave mute deafen unmute screen share hand chime beep effects"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Call sounds"),
          anchor: "callSoundsEnabledCheck" },
        { title: qsTr("Ringer volume"),
          keywords: QT_TR_NOOP("ringer ringtone ring volume incoming call loud"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Call sounds"),
          anchor: "ringerVolumeSlider" },
        { title: qsTr("Float the call when Lightning is minimised"),
          keywords: QT_TR_NOOP("float picture in picture pip call window minimised"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Calls"),
          anchor: "callPictureInPictureCheck" },
        { title: qsTr("Ring for incoming voice calls"),
          keywords: QT_TR_NOOP("ring ringer ringtone sound incoming call alert"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "ringForCallsCheck" },
        { title: qsTr("Desktop notifications"),
          keywords: QT_TR_NOOP("notifications desktop enable"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          control: "notificationsEnabled",
          anchor: "notificationsEnabledCheck" },
        { title: qsTr("Unread dot"),
          keywords: QT_TR_NOOP("unread dot badge indicator room list bold "
                         + "muted mentions hide"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "unreadDotModeCombo" },
        { title: qsTr("Mark all rooms read automatically"),
          keywords: QT_TR_NOOP("mark all read automatically auto timer interval "
                         + "minutes unread clear"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "autoMarkReadCombo" },
        { title: qsTr("Notification preview"),
          keywords: QT_TR_NOOP("notification preview privacy sender message"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "notificationPreviewCombo" },

        { title: qsTr("Notification preview in encrypted rooms"),
          keywords: QT_TR_NOOP("notification preview encrypted body privacy hide message text"),
          section: "notifications",
          breadcrumb: qsTr("Notifications"),
          anchor: "notificationPreviewEncryptedCombo" },
        { title: qsTr("Notification sound"),
          keywords: QT_TR_NOOP("notification sound mute"), section: "notifications",
          breadcrumb: qsTr("Notifications"),
          anchor: "notificationSoundCombo" },
        { title: qsTr("Notification sound style and volume"),
          keywords: QT_TR_NOOP("notification sound chime lightning system default "
                         + "volume mention test"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "notificationSoundSourceCombo" },
        { title: qsTr("Mute all notifications"),
          keywords: QT_TR_NOOP("mute all silence everything pause notifications "
                         + "push rules account"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "pushMasterMuteSwitch" },
        { title: qsTr("Default alerts"),
          keywords: QT_TR_NOOP("default alerts direct messages rooms all messages "
                         + "mentions only push rules"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "pushDmDefaultCombo" },
        { title: qsTr("Mentions"),
          keywords: QT_TR_NOOP("mention mentions @room user id display name "
                         + "username highlight alert"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "pushMentionUserIdCombo" },
        { title: qsTr("Keywords"),
          keywords: QT_TR_NOOP("keyword keywords word notify when someone says "
                         + "push rules"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "pushKeywordField" },
        { title: qsTr("Other events"),
          keywords: QT_TR_NOOP("other events reactions invitations invites bots "
                         + "bot notices"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "pushEventReactionsCombo" },
        { title: qsTr("Not getting notifications?"),
          keywords: QT_TR_NOOP("troubleshoot troubleshooter not getting notifications "
                         + "missing no notification test check"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Account notification rules"),
          anchor: "troubleshooterRunButton" },

        { title: qsTr("Only exchange messages with verified devices"),
          keywords: QT_TR_NOOP("invisible crypto msc4153 cross-signed verified "
                         + "device trust insecure exclude encryption"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Device trust"),
          anchor: "strictDeviceTrustCheck" },

        { title: qsTr("Keep downloaded media on this device"),
          keywords: QT_TR_NOOP("media cache video image picture file download "
                         + "offline storage disk keep clear"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Media kept on this device"),
          anchor: "keepMediaOnDeviceCheck" },

        { title: qsTr("Download folder"),
          keywords: QT_TR_NOOP("download downloads folder save saved files location "
                         + "directory where"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Downloads"),
          anchor: "downloadFolderLabel" },
        { title: qsTr("Always ask where to save files"),
          keywords: QT_TR_NOOP("download downloads save folder location directory "
                         + "ask where file"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Downloads"),
          anchor: "alwaysAskWhereToSaveCheck" },

        { title: qsTr("Read receipts"),
          keywords: QT_TR_NOOP("read receipt receipts private seen ticks blue "
                         + "m.read.private privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Reading and typing"),
          anchor: "readReceiptModeCombo" },

        { title: qsTr("Let others see when I am typing"),
          keywords: QT_TR_NOOP("typing notice notification composing indicator "
                         + "privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Reading and typing"),
          anchor: "sendTypingCheck" },

        { title: qsTr("Share my online status"),
          keywords: QT_TR_NOOP("presence online idle offline status share"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Presence"),
          anchor: "sharePresenceCheck" },

        { title: qsTr("Ignored users"),
          keywords: QT_TR_NOOP("ignore ignored block user mute person hide"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Ignored users"),
          anchor: "ignoredUsersCard" },
        { title: qsTr("Index encrypted rooms"),
          keywords: QT_TR_NOOP("message search index all rooms history local "
                         + "encrypted older messages backfill"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Message search index"),
          anchor: "indexAllRoomsButton" },
        { title: qsTr("Sign out other sessions"),
          keywords: QT_TR_NOOP("sessions devices sign out remove device delete"),
          section: "sessions",
          breadcrumb: qsTr("Sessions"),
          anchor: "signOutOtherSessionsButton" },

        { title: qsTr("Automatically load previews in unencrypted rooms"),
          keywords: QT_TR_NOOP("link preview privacy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          control: "autoLoadLinkPreviews",
          anchor: "autoPreviewCheck" },
        { title: qsTr("Allow incoming room invitations"),
          keywords: QT_TR_NOOP("invite invitation block reject spam privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Media previews"),
          anchor: "allowInvitesCheck" },
        { title: qsTr("Show avatars in room invitations"),
          keywords: QT_TR_NOOP("invite invitation avatar picture privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Media previews"),
          anchor: "inviteAvatarsCheck" },
        { title: qsTr("Load previews in encrypted rooms"),
          keywords: QT_TR_NOOP("link preview encrypted"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          anchor: "encryptedPreviewCheck" },
        { title: qsTr("Show images and videos from links inline"),
          keywords: QT_TR_NOOP("link preview image video embed inline media player viewer"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          anchor: "inlineLinkMediaCheck" },
        { title: qsTr("Preload videos and audio"),
          keywords: QT_TR_NOOP("video audio voice preload prefetch download size megabytes data disk instant play"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Media"),
          anchor: "videoPrefetchCheck" },
        { title: qsTr("Photo details when sending"),
          keywords: QT_TR_NOOP("photo image exif gps location metadata strip remove "
                         + "privacy upload picture camera"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Media"),
          anchor: "imageMetadataCombo" },
        { title: qsTr("GIF safe search"),
          keywords: QT_TR_NOOP("gif safe search rating"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "gifRatingCombo" },
        { title: qsTr("Preferred GIF provider"),
          keywords: QT_TR_NOOP("gif provider giphy klipy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "gifProviderCombo" },
        { title: qsTr("Store recently used GIFs"),
          keywords: QT_TR_NOOP("gif recents store"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "starredGifsSummaryLabel" },
        { title: qsTr("Security status"),
          keywords: QT_TR_NOOP("e2ee encryption status cross-signing backup"),
          section: "privacy", breadcrumb: qsTr("Privacy & security"),
          anchor: "cryptoHealthSummary" },
        { title: qsTr("Recovery key or passphrase"),
          keywords: QT_TR_NOOP("recovery key passphrase backup restore"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "recoveryInputField" },
        { title: qsTr("Import room keys"),
          keywords: QT_TR_NOOP("import room keys export"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "importRoomKeysHeading" },
        { title: qsTr("Export room keys"),
          keywords: QT_TR_NOOP("export room keys backup file passphrase element"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "exportRoomKeysHeading" },
        { title: qsTr("Danger Zone"),
          keywords: QT_TR_NOOP("reset danger local session"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "dangerZone" },

        { title: qsTr("Sessions"), keywords: QT_TR_NOOP("sessions devices"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "sessionsHeading" },
        { title: qsTr("Current session"),
          keywords: QT_TR_NOOP("device id session status"), section: "sessions",
          breadcrumb: qsTr("Sessions"),
          anchor: "currentSessionHeading" },
        { title: qsTr("Sign in another device"),
          keywords: QT_TR_NOOP("qr code scan sign in another device phone link "
                         + "msc4108 login"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "qrLoginOpenButton" },

        { title: qsTr("Verify this session"),
          keywords: QT_TR_NOOP("verify verification sas cross-signing"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "verificationStatusCard" },

        { title: qsTr("Microphone noise suppression"),
          keywords: QT_TR_NOOP("noise suppression cancellation microphone background "
                         + "webrtc rnnoise deepfilternet denoise"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "noiseSuppressionCard" },
        { title: qsTr("Run several accounts at once"),
          keywords: QT_TR_NOOP("accounts several multiple background always signed "
                         + "in live keep running switch"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsConcurrentAccountsSwitch" },
        { title: qsTr("Game activity"),
          keywords: QT_TR_NOOP("playing game status steam presence activity"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "gameActivityCard" },
        { title: qsTr("Music activity"),
          keywords: QT_TR_NOOP("listening music spotify song status presence activity"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "musicActivityCard" },
        { title: qsTr("Backend"), keywords: QT_TR_NOOP("backend rust http mock"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsBackendLine" },
        { title: qsTr("Sync mode"), keywords: QT_TR_NOOP("sync sliding"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsSyncModeLine" },
        { title: qsTr("Connection"), keywords: QT_TR_NOOP("connection status"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsConnectionLine" },
        { title: qsTr("Refresh current room"),
          keywords: QT_TR_NOOP("refresh reload timeline"), section: "labs",
          breadcrumb: qsTr("Labs"),
          anchor: "labsRefreshRoomButton" },

        { title: qsTr("Export and import settings"),
          keywords: QT_TR_NOOP("export import backup settings file restore"),
          section: "account", breadcrumb: qsTr("Account · Settings file"),
          anchor: "settingsFileCard" },
        { title: qsTr("Sync settings across your devices"),
          keywords: QT_TR_NOOP("sync settings devices account data backup"),
          section: "account", breadcrumb: qsTr("Account · Sync"),
          anchor: "settingsSyncCard" },

        { title: qsTr("About"), keywords: QT_TR_NOOP("about version license"),
          section: "about", breadcrumb: qsTr("About"),
          anchor: "aboutAppLogo" },

        { title: qsTr("Updates"),
          keywords: QT_TR_NOOP("update version upgrade check download install"),
          section: "updates", breadcrumb: qsTr("Updates"),
          anchor: "updatesSection" },
        { title: qsTr("Automatically check for updates"),
          keywords: QT_TR_NOOP("update automatic check background"),
          section: "updates", breadcrumb: qsTr("Updates · Automatic checks"),
          anchor: "updatesSection" },
        { title: qsTr("Conversation list"),
          keywords: QT_TR_NOOP("conversation list classic channels folders room list layout"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "roomNavigationLayoutCards" },
        { title: qsTr("Show the Spaces rail"),
          keywords: QT_TR_NOOP("spaces rail sidebar show hide"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "showSpacesRailCheck" },
        { title: qsTr("Show the room list"),
          keywords: QT_TR_NOOP("room list sidebar show hide"),
          section: "layout", breadcrumb: qsTr("Layout · Room list and panels"),
          anchor: "showRoomListCheck" },
        { title: qsTr("Application icon"),
          keywords: QT_TR_NOOP("application icon app icon window icon taskbar"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "chooseAppIconButton" },
    ]
    readonly property var matchedSearchResults: {
        var words = root.settingsSearchQuery.trim().toLowerCase().split(/\s+/)
                        .filter(function(w) { return w.length > 0 })
        if (words.length === 0) return []
        return root.searchIndex.filter(function(e) {
            if (e.platform !== undefined && e.platform !== Qt.platform.os)
                return false
            if (e.requiresTray === true && !app.trayAvailable)
                return false
            // The keywords are marked, not translated, in the index: the
            // haystack carries the translation AND the English source, so an
            // English term still finds the entry in any language.
            var hay = (e.title + " " + qsTr(e.keywords) + " " + e.keywords
                       + " " + e.breadcrumb).toLowerCase()
            for (var i = 0; i < words.length; ++i) {
                if (hay.indexOf(words[i]) === -1)
                    return false
            }
            return true
        })
    }
    readonly property var matchedSearchSections: {
        var s = {}
        for (var i = 0; i < root.matchedSearchResults.length; ++i)
            s[root.matchedSearchResults[i].section] = true
        return s
    }
    function trustStateText(state) {
        switch (state) {
        case "Verified": return qsTr("Verified")
        case "Not verified": return qsTr("Not verified")
        case "Cross-signing unavailable": return qsTr("Cross-signing unavailable")
        case "Unknown": return qsTr("Unknown")
        default: return state
        }
    }

    function backupStateText(state) {
        switch (state) {
        case "unknown": return qsTr("unknown")
        case "creating": return qsTr("creating")
        case "enabling": return qsTr("enabling")
        case "resuming": return qsTr("resuming")
        case "enabled": return qsTr("enabled")
        case "downloading": return qsTr("downloading")
        case "disabling": return qsTr("disabling")
        case "uploading": return qsTr("uploading")
        case "done": return qsTr("done")
        case "error": return qsTr("error")
        case "idle": return qsTr("idle")
        default: return state
        }
    }

    function escapeHtml(s) {
        return String(s)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function highlightedTitle(title, query) {
        var safe = escapeHtml(title)
        var q = (query || "").trim()
        if (q.length === 0) return safe
        var lowerSafe = safe.toLowerCase()
        var lowerQ = escapeHtml(q).toLowerCase()
        var idx = lowerSafe.indexOf(lowerQ)
        if (idx === -1) {
            var words = lowerQ.split(/\s+/)
            for (var w = 0; w < words.length && idx === -1; ++w) {
                if (words[w].length > 0) {
                    idx = lowerSafe.indexOf(words[w])
                    if (idx !== -1)
                        lowerQ = words[w]
                }
            }
        }
        if (idx === -1) return safe
        return safe.slice(0, idx) + "<font color=\"" + AppTheme.bolt + "\">"
             + safe.slice(idx, idx + lowerQ.length) + "</font>"
             + safe.slice(idx + lowerQ.length)
    }

    property string searchRevealAnchor: ""

    function findInPane(node, name) {
        if (!node)
            return null
        if (node.objectName === name)
            return node
        var kids = node.children
        for (var i = 0; i < kids.length; ++i) {
            var hit = root.findInPane(kids[i], name)
            if (hit)
                return hit
        }
        return null
    }

    function revealSearchResult(entry) {
        if (!entry)
            return
        root.section = entry.section
        root.searchRevealAnchor = entry.anchor || ""
        root.applySearchReveal(true)
        // Repeat until the pane holds still: a section change relayouts the
        // pane in the polish pass before the next frame, so the first y read
        // can be stale. One frame apart, stopping when two passes agree,
        // bounded at twelve.
        searchRevealSettle.ticks = 0
        searchRevealSettle.lastTop = -1
        searchRevealSettle.restart()
    }

    function sectionHeadingOf(node) {
        for (var n = node; n && n !== contentColumn; n = n.parent) {
            var sibs = n.parent ? n.parent.children : []
            var at = -1
            for (var i = 0; i < sibs.length; ++i) {
                if (sibs[i] === n) { at = i; break }
            }
            for (var j = at - 1; j >= 0; --j) {
                var s = sibs[j]
                if (s.visible && typeof s.text === "string"
                        && s.font && s.font.pixelSize === AppTheme.textTitle)
                    return s
            }
        }
        return null
    }

    function applySearchReveal(flash) {
        if (root.searchRevealAnchor.length === 0)
            return -1
        var target = root.findInPane(contentColumn, root.searchRevealAnchor)
        if (!target)
            return -1
        var top = target.mapToItem(contentFlick.contentItem, 0, 0).y
        var maxY = Math.max(0, contentFlick.contentHeight - contentFlick.height)
        var land = top - AppTheme.spacing24 * 2
        var heading = root.sectionHeadingOf(target)
        if (heading) {
            var headTop = heading.mapToItem(contentFlick.contentItem, 0, 0).y
                          - AppTheme.spacing8
            var targetBottom = top + target.height
            if (headTop < land && targetBottom - headTop <= contentFlick.height)
                land = headTop
        }
        settingsWheelArea.stopGlide()
        contentFlick.contentY = Math.max(0, Math.min(land, maxY))
        if (flash)
            searchRevealHalo.flashOver(target)
        else
            searchRevealHalo.placeOver(target)
        return top
    }

    function jumpToFirstShown(anchors) {
        for (var i = 0; i < anchors.length; ++i) {
            var target = root.findInPane(contentColumn, anchors[i])
            if (target && target.visible) {
                root.revealSearchResult({ section: root.section,
                                          anchor: anchors[i] })
                return anchors[i]
            }
        }
        return ""
    }

    readonly property var pageOutlines: ({
        "account": [
            { label: qsTr("Profile"), anchors: ["accountIdentityCard"] },
            { label: qsTr("Password"), anchors: ["changePasswordCard"] },
            { label: qsTr("Network"), anchors: ["networkRouteCard"] },
        ],
        "appearance": [
            { label: qsTr("Theme"), anchors: ["featuredThemeFlow"] },
            { label: qsTr("Custom colours and gradients"),
              anchors: ["customThemeRow"] },
            { label: qsTr("Chat background and depth"),
              anchors: ["chatBackgroundSettingsSection"] },
            { label: qsTr("Text and fonts"), anchors: ["textScaleSlider"] },
            { label: qsTr("Application icon"), anchors: ["appIconPreview"] },
            { label: qsTr("Language"), anchors: ["languageCombo"] },
            { label: qsTr("Accessibility"), anchors: ["accessibilityHeading"] },
            { label: qsTr("Motion and time"), anchors: ["reducedMotionCheck"] },
        ],
        "layout": [
            { label: qsTr("Messages and threads"),
              anchors: ["layoutPresetHeading", "layoutPresetControl"] },
            { label: qsTr("Room list and panels"),
              anchors: ["roomListLayoutHeading", "roomNavigationLayoutCards"] },
            { label: qsTr("System tray"), anchors: ["closeToTrayCheck"] },
        ],
        "chat": [
            { label: qsTr("Timeline"), anchors: ["showRoomActivityCheck"] },
            { label: qsTr("Message box"),
              anchors: ["enterInsertsNewlineCheck"] },
            { label: qsTr("Emoji and sticker packs"),
              anchors: ["stickerPacksSettings"] },
        ],
        "notifications": [
            { label: qsTr("Desktop notifications"),
              anchors: ["notificationsEnabledCheck"] },
            { label: qsTr("Previews"), anchors: ["notificationPreviewCombo"] },
            { label: qsTr("Sound"), anchors: ["notificationSoundCombo"] },
            { label: qsTr("Calls"), anchors: ["ringForCallsCheck"] },
            { label: qsTr("Unread dot"), anchors: ["unreadDotModeCombo"] },
            { label: qsTr("Mute all"), anchors: ["pushMasterMuteSwitch"] },
            { label: qsTr("Default alerts"), anchors: ["pushDmDefaultCombo"] },
            { label: qsTr("Mentions"), anchors: ["pushMentionUserIdCombo"] },
            { label: qsTr("Keywords"), anchors: ["pushKeywordField"] },
            { label: qsTr("Other events"),
              anchors: ["pushEventReactionsCombo"] },
            { label: qsTr("Not getting notifications?"),
              anchors: ["troubleshooterRunButton"] },
        ],
        "sound": [
            { label: qsTr("Microphone"), anchors: ["callDevice_microphone"] },
            { label: qsTr("Speaker"), anchors: ["callDevice_speaker"] },
            { label: qsTr("Camera"), anchors: ["callDevice_camera"] },
            { label: qsTr("Media volume"),
              anchors: ["mediaVolumeSettingSlider"] },
            { label: qsTr("Call sounds"), anchors: ["callSoundsEnabledCheck"] },
        ],
        "privacy": [
            { label: qsTr("Device trust"), anchors: ["strictDeviceTrustCheck"] },
            { label: qsTr("Reading and typing"),
              anchors: ["readReceiptModeCombo"] },
            { label: qsTr("Ignored users"), anchors: ["ignoredUsersCard"] },
            { label: qsTr("Message search"), anchors: ["searchIndexHelpText"] },
            { label: qsTr("Link previews and GIFs"),
              anchors: ["autoPreviewCheck"] },
            { label: qsTr("Media and downloads"),
              anchors: ["keepMediaOnDeviceCheck"] },
            { label: qsTr("Encryption"),
              anchors: ["cryptoHealthSummary", "privacyStorageFactsCard"] },
            { label: qsTr("Recovery key"),
              anchors: ["recoveryKeyCard", "recoveryEntry"] },
        ],
    })
    function outlineEntries(sectionKey) {
        return root.pageOutlines[sectionKey] || []
    }

    property int outlineActive: -1
    property int outlinePinned: -1
    property real outlinePinnedY: 0

    property var anchorCache: ({})
    function anchorItem(anchor) {
        var hit = root.anchorCache[anchor]
        if (hit)
            return hit
        hit = root.findInPane(contentColumn, anchor)
        if (hit)
            root.anchorCache[anchor] = hit
        return hit
    }
    function firstShownAnchor(anchors) {
        for (var i = 0; i < anchors.length; ++i) {
            var target = root.anchorItem(anchors[i])
            if (target && target.visible)
                return target
        }
        return null
    }

    function refreshOutlineActive() {
        var entries = root.outlineEntries(root.section)
        if (entries.length === 0) {
            root.outlineActive = -1
            return
        }
        var y = contentFlick.contentY
        var maxY = Math.max(0, contentFlick.contentHeight - contentFlick.height)
        if (root.outlinePinned >= 0) {
            if (root.outlinePinned < entries.length
                && (searchRevealSettle.running
                    || Math.abs(y - root.outlinePinnedY) <= 2)) {
                root.outlineActive = root.outlinePinned
                return
            }
            root.outlinePinned = -1
        }
        var probe = y + AppTheme.spacing24 * 2 + 8
        var active = -1
        var firstShown = -1
        var lastShown = -1
        for (var i = 0; i < entries.length; ++i) {
            var target = root.firstShownAnchor(entries[i].anchors)
            if (!target)
                continue
            if (firstShown < 0)
                firstShown = i
            lastShown = i
            if (target.mapToItem(contentFlick.contentItem, 0, 0).y <= probe)
                active = i
        }
        if (maxY > 0 && y >= maxY - 2 && lastShown >= 0)
            active = lastShown
        root.outlineActive = active >= 0 ? active : firstShown
    }
    Timer {
        id: outlineTimer
        interval: 50
        onTriggered: root.refreshOutlineActive()
    }
    function scheduleOutlineRefresh() {
        if (!outlineTimer.running)
            outlineTimer.start()
    }
    function revealOutlineEntry(sectionKey, index) {
        var entries = root.outlineEntries(sectionKey)
        if (index < 0 || index >= entries.length)
            return ""
        if (sectionKey !== root.section)
            root.section = sectionKey
        var used = root.jumpToFirstShown(entries[index].anchors)
        if (used.length > 0) {
            root.outlinePinned = index
            root.outlinePinnedY = contentFlick.contentY
            root.outlineActive = index
        }
        return used
    }

    readonly property string settingsLinkPrefix: "lightning://settings/"
    function settingsLink(sectionKey, anchor) {
        return root.settingsLinkPrefix + sectionKey + "?focus="
               + encodeURIComponent(anchor)
    }
    function parseSettingsLink(text) {
        var t = String(text || "").trim()
        if (t.indexOf(root.settingsLinkPrefix) !== 0)
            return null
        var rest = t.slice(root.settingsLinkPrefix.length)
        var q = rest.indexOf("?")
        var sectionKey = q < 0 ? rest : rest.slice(0, q)
        if (root.sectionTitle(sectionKey) === sectionKey)
            return null
        var anchor = ""
        if (q >= 0) {
            var m = /(?:^|&)focus=([^&]*)/.exec(rest.slice(q + 1))
            if (m) {
                try { anchor = decodeURIComponent(m[1]) } catch (e) { return null }
            }
        }
        if (anchor.length > 0 && !/^[A-Za-z0-9_.-]+$/.test(anchor))
            return null
        return { section: sectionKey, anchor: anchor }
    }
    function openSettingsLink(text) {
        var link = root.parseSettingsLink(text)
        if (!link)
            return false
        root.revealSearchResult({
            section: root.sectionForAnchor(link.anchor, link.section),
            anchor: link.anchor })
        return true
    }
    function sectionForAnchor(anchor, fallback) {
        if (!anchor || anchor.length === 0)
            return fallback
        for (var i = 0; i < root.searchIndex.length; ++i) {
            if (root.searchIndex[i].anchor === anchor)
                return root.searchIndex[i].section
        }
        for (var key in root.pageOutlines) {
            var entries = root.pageOutlines[key]
            for (var j = 0; j < entries.length; ++j) {
                if (entries[j].anchors.indexOf(anchor) >= 0)
                    return key
            }
        }
        return fallback
    }
    function indexEntryAtContentY(y) {
        var best = null
        var bestDistance = 1e9
        for (var i = 0; i < root.searchIndex.length; ++i) {
            var e = root.searchIndex[i]
            if (e.section !== root.section)
                continue
            var t = root.anchorItem(e.anchor)
            if (!t || !t.visible)
                continue
            var top = t.mapToItem(contentFlick.contentItem, 0, 0).y
            var bottom = top + t.height
            if (y < top - AppTheme.spacing8 || y > bottom + AppTheme.spacing8)
                continue
            var d = Math.abs(y - (top + bottom) / 2)
            if (d < bestDistance) {
                best = e
                bestDistance = d
            }
        }
        return best
    }
    property string linkMenuTarget: ""

    property string themeEditorStartRole: ""

    Timer {
        id: searchRevealSettle
        interval: 16
        repeat: true
        property int ticks: 0
        property real lastTop: -1
        onTriggered: {
            var top = root.applySearchReveal(false)
            if (top < 0 || (lastTop >= 0 && Math.abs(top - lastTop) < 0.5)
                || ++ticks >= 12) {
                root.outlinePinnedY = contentFlick.contentY
                running = false
                ticks = 0
                lastTop = -1
                return
            }
            lastTop = top
        }
    }

    component SettingsGroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }

    component ConfirmDialog: Dialog {
        id: confirmDialog
        property string confirmText: qsTr("Confirm")
        property string confirmKind: "primary"
        anchors.centerIn: parent
        modal: true
        property Item focusBeforeOpen: null
        onAboutToShow: {
            var win = confirmDialog.parent ? confirmDialog.parent.Window.window : null
            focusBeforeOpen = win ? win.activeFocusItem : null
        }
        onOpened: confirmCancelButton.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: {
            if (focusBeforeOpen && focusBeforeOpen.visible)
                focusBeforeOpen.forceActiveFocus(Qt.PopupFocusReason)
            focusBeforeOpen = null
        }
        width: 340
        padding: AppTheme.spacing20
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            radius: AppTheme.radiusLg
        }
        header: Label {
            text: confirmDialog.title
            visible: text.length > 0
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing20
            rightPadding: AppTheme.spacing20
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }
        footer: Item {
            implicitHeight: confirmFooter.implicitHeight + AppTheme.spacing20 * 2
            RowLayout {
                id: confirmFooter
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing20
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing8
                AppButton {
                    id: confirmCancelButton
                    objectName: "confirmDialogCancel"
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: confirmDialog.reject()
                }
                AppButton {
                    objectName: "confirmDialogConfirm"
                    storm: true
                    kind: confirmDialog.confirmKind
                    text: confirmDialog.confirmText
                    onClicked: confirmDialog.accept()
                }
            }
        }
    }

    component SettingsCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            objectName: "settingsCardSurface"
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    component SettingsSlider: Slider {
        id: styledSlider
        // The custom background and handle have no implicit height: without
        // this the slider is zero pixels tall and cannot be pressed.
        implicitHeight: 24
        snapMode: Slider.SnapAlways
        background: Rectangle {
            x: styledSlider.leftPadding
            y: styledSlider.topPadding + styledSlider.availableHeight / 2 - 2
            width: styledSlider.availableWidth
            height: 4
            radius: AppTheme.radiusPill
            color: AppTheme.stormInset
            Rectangle {
                width: styledSlider.visualPosition * parent.width
                height: parent.height
                radius: AppTheme.radiusPill
                color: AppTheme.bolt
            }
        }
        handle: Rectangle {
            x: styledSlider.leftPadding
               + styledSlider.visualPosition * (styledSlider.availableWidth - width)
            y: styledSlider.topPadding + styledSlider.availableHeight / 2 - height / 2
            width: 16
            height: 16
            radius: 8
            color: "#FFFFFF"
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: -1
                radius: 8
                z: -1
                color: "#40000000"
            }
            border.width: styledSlider.visualFocus ? 2 : 1
            border.color: styledSlider.visualFocus ? AppTheme.bolt
                                                   : AppTheme.stormBorderStrong
        }
    }

    function navGroupShown(keys) {
        if (root.navCollapsed)
            return false
        if (root.settingsSearchQuery.trim().length === 0
                || root.matchedSearchResults.length === 0)
            return true
        for (var i = 0; i < keys.length; ++i) {
            if (root.matchedSearchSections[keys[i]] === true)
                return true
        }
        return false
    }

    property var expandedOutlines: ({})
    function outlineExpanded(key) { return root.expandedOutlines[key] === true }
    function outlineCollapsed(key) { return !root.outlineExpanded(key) }
    function setOutlineExpanded(key, open) {
        if (root.outlineExpanded(key) === open)
            return
        var next = Object.assign({}, root.expandedOutlines)
        if (open)
            next[key] = true
        else
            delete next[key]
        root.expandedOutlines = next
    }
    function toggleOutline(key) {
        root.setOutlineExpanded(key, !root.outlineExpanded(key))
    }

    component SettingsNavOutlineFor: SettingsNavOutline {
        required property string sectionKey
        objectName: "settingsNavOutline_" + sectionKey
        shown: root.outlineExpanded(sectionKey) && !root.navCollapsed
               && root.settingsSearchQuery.trim().length === 0
        entries: root.outlineEntries(sectionKey)
        activeIndex: root.section === sectionKey ? root.outlineActive : -1
        onEntryActivated: (index) => root.revealOutlineEntry(sectionKey, index)
        onEntryFocused: (entry) => navFlick.ensureVisibleItem(entry)
        onParentRequested: {
            var row = root.findInPane(navColumn, "settingsNavRow_" + sectionKey)
            if (row)
                row.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    component SettingsNavRow: ItemDelegate {
        id: navRow
        property string sectionKey: ""
        property string iconName: ""
        property string navLabel: ""
        property bool alert: false
        readonly property bool hasOutline:
            root.outlineEntries(sectionKey).length > 0
        readonly property bool outlineOpen:
            hasOutline && root.outlineExpanded(sectionKey)
        objectName: "settingsNavRow_" + sectionKey
        Layout.fillWidth: true
        implicitHeight: Math.max(32, navRowText.implicitHeight + AppTheme.spacing12)
        readonly property int caretGutter: 4 + 11 + AppTheme.spacing4
        leftPadding: root.navCollapsed ? padding : padding + caretGutter
        PlainToolTip {
            objectName: "settingsNavRowTip_" + navRow.sectionKey
            visible: root.navCollapsed && navRow.hovered
            text: navRow.navLabel
            delay: 300
            x: navRow.width + AppTheme.spacing4
            y: (navRow.height - height) / 2
        }
        topPadding: 0
        bottomPadding: 0
        visible: root.settingsSearchQuery.trim().length === 0
                 || root.matchedSearchResults.length === 0
                 || root.matchedSearchSections[sectionKey] === true
        highlighted: root.section === sectionKey
        focusPolicy: Qt.TabFocus
        onActiveFocusChanged: if (activeFocus) navFlick.ensureVisibleItem(navRow)
        Accessible.name: navLabel
        Accessible.selected: navRow.highlighted
        function activate() {
            if (!navRow.hasOutline) {
                root.section = navRow.sectionKey
            } else {
                var fold = navRow.outlineOpen && root.section === navRow.sectionKey
                root.section = navRow.sectionKey
                root.setOutlineExpanded(navRow.sectionKey, !fold)
            }
        }
        onClicked: navRow.activate()
        Keys.onPressed: (event) => {
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier) {
                if (!event.isAutoRepeat)
                    navRow.activate()
                event.accepted = true
                return
            }
            if (!navRow.hasOutline || event.modifiers !== Qt.NoModifier)
                return
            var openKey = navRow.mirrored ? Qt.Key_Left : Qt.Key_Right
            var closeKey = navRow.mirrored ? Qt.Key_Right : Qt.Key_Left
            if (event.key === openKey && !navRow.outlineOpen) {
                root.setOutlineExpanded(navRow.sectionKey, true)
                event.accepted = true
            } else if (event.key === closeKey && navRow.outlineOpen) {
                root.setOutlineExpanded(navRow.sectionKey, false)
                event.accepted = true
            }
        }
        contentItem: RowLayout {
            spacing: AppTheme.spacing8
            Icon {
                name: navRow.iconName
                size: 16
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: navRowText.implicitHeight
                color: navRow.highlighted ? AppTheme.settingsNavSelectedMark
                                          : AppTheme.stormTextMuted
            }
            Label {
                id: navRowText
                text: navRow.navLabel
                visible: !root.navCollapsed
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                color: navRow.highlighted ? AppTheme.stormText
                                          : AppTheme.settingsNavText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.settingsOptionSize
                font.weight: navRow.highlighted ? AppTheme.weightBold : AppTheme.weightStrong
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
            }
            AbstractButton {
                id: navChevron
                objectName: "settingsNavChevronButton_" + navRow.sectionKey
                visible: navRow.hasOutline && !root.navCollapsed
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 24
                implicitHeight: 24
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: navRow.outlineOpen
                                 ? qsTr("Collapse %1").arg(navRow.navLabel)
                                 : qsTr("Expand %1").arg(navRow.navLabel)
                Accessible.description: navRow.outlineOpen
                                        ? qsTr("Sub-sections shown")
                                        : qsTr("Sub-sections hidden")
                onClicked: root.toggleOutline(navRow.sectionKey)
                Keys.onReturnPressed: (event) => {
                    if (!event.isAutoRepeat)
                        root.toggleOutline(navRow.sectionKey)
                    event.accepted = true
                }
                Keys.onEnterPressed: (event) => {
                    if (!event.isAutoRepeat)
                        root.toggleOutline(navRow.sectionKey)
                    event.accepted = true
                }
                background: Rectangle {
                    radius: AppTheme.radiusSm
                    color: navChevron.down ? AppTheme.settingsNavSelected
                         : navChevron.hovered
                           ? Qt.alpha(AppTheme.settingsNavSelected, 0.7)
                           : "transparent"
                    border.width: navChevron.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
                contentItem: Icon {
                    objectName: "settingsNavChevron_" + navRow.sectionKey
                    name: navRow.outlineOpen ? "expand_more"
                        : (navChevron.mirrored ? "chevron_left" : "chevron_right")
                    size: 16
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: navRow.highlighted ? AppTheme.stormText
                                              : AppTheme.stormTextMuted
                }
            }
            Rectangle {
                objectName: "settingsNavAlert_" + navRow.sectionKey
                visible: navRow.alert
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: AppTheme.spacing4
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: AppTheme.stormDanger
                Accessible.role: Accessible.Indicator
                Accessible.name: qsTr("Needs attention")
            }
        }
        background: Rectangle {
            objectName: "settingsNavRowFill_" + navRow.sectionKey
            radius: AppTheme.radiusTile
            color: navRow.highlighted ? AppTheme.settingsNavSelected
                 : navRow.hovered ? Qt.alpha(AppTheme.settingsNavSelected, 0.55)
                 : "transparent"
            border.width: navRow.visualFocus ? 2 : 0
            border.color: AppTheme.bolt
            Icon {
                objectName: "settingsNavCaret"
                visible: navRow.highlighted
                name: "bolt"
                size: 11
                color: AppTheme.settingsNavSelectedMark
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 4
            }
        }
    }

    property string section: "appearance"
    property var keptSections: []
    function sectionLive(key) {
        return root.section === key || root.keptSections.indexOf(key) >= 0
    }
    property bool identityEditing: false
    property var nameColorPickerRef: null
    property var backupCardRef: null

    readonly property bool navNarrow: root.width < 720
    property bool navOpenAtNarrow: false
    readonly property bool navCollapsed: navNarrow && !navOpenAtNarrow
    onNavNarrowChanged: if (!navNarrow) navOpenAtNarrow = false
    onNavCollapsedChanged: {
        if (navCollapsed)
            settingsSearchField.text = ""
    }

    function sectionTitle(key) {
        if (key === "account") return qsTr("Account")
        if (key === "appearance") return qsTr("Appearance")
        if (key === "layout") return qsTr("Layout")
        if (key === "chat") return qsTr("Chat")
        if (key === "shortcuts") return qsTr("Keyboard shortcuts")
        if (key === "notifications") return qsTr("Notifications")
        if (key === "sound") return qsTr("Sound & video")
        if (key === "privacy") return qsTr("Privacy & security")
        if (key === "sessions") return qsTr("Sessions")
        if (key === "labs") return qsTr("Labs")
        if (key === "updates") return qsTr("Updates")
        if (key === "about") return qsTr("About")
        return key
    }
    function sectionIcon(key) {
        if (key === "account") return "account_circle"
        if (key === "appearance") return "palette"
        if (key === "layout") return "grid_view"
        if (key === "chat") return "forum"
        if (key === "shortcuts") return "keyboard_return"
        if (key === "notifications") return "notifications"
        if (key === "sound") return "volume_up"
        if (key === "privacy") return "verified_user"
        if (key === "sessions") return "devices"
        if (key === "labs") return "science"
        if (key === "updates") return "download"
        if (key === "about") return "info"
        return "settings"
    }

    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return Number(n / (1024 * 1024)).toLocaleString(Qt.locale(AppLocale.name), "f", 1) + " MB"
    }

    function mapLegacySection(key) {
        if (key === "general") return "appearance"
        if (key === "security") return "privacy"
        if (key === "advanced") return "labs"
        return key
    }

    Component.onCompleted: {
        app.installSettingsNavigation(root)
        var requested = app.takeRequestedSettingsSection()
        if (requested.length > 0)
            section = mapLegacySection(requested)
        root.setOutlineExpanded(root.section, true)
        if (app.takeSettingsSearchFocusRequested())
            settingsSearchField.forceActiveFocus()
    }
    Connections {
        target: app
        function onSettingsSectionRequested(requested) {
            if (requested.length > 0)
                section = mapLegacySection(requested)
            app.takeRequestedSettingsSection()
        }
        function onSettingsSearchFocusRequested() {
            settingsSearchField.forceActiveFocus()
        }
    }

    function goBack() {
        app.loggedIn ? app.showMain() : app.showLogin()
    }

    function spellLanguageLabel(tag) {
        if (app.spell) {
            var options = app.spell.languageOptions
            for (var i = 0; i < options.length; ++i) {
                if (options[i].tag === tag)
                    return options[i].label
            }
        }
        return tag
    }

    function copyToClipboard(value) {
        if (!value || value.length === 0) return
        settingsClipboardHelper.text = value
        settingsClipboardHelper.selectAll()
        settingsClipboardHelper.copy()
        settingsClipboardHelper.text = ""
    }
    TextEdit {
        id: settingsClipboardHelper
        objectName: "settingsClipboardHelper"
        visible: false
        width: 0
        height: 0
    }
    Shortcut {
        sequence: "Escape"
        // Shortcuts are dispatched before the focused item sees the key, so
        // disable this while an inline editor is open or its own Escape handler
        // is dead. `root.visible` is required: the screen is kept alive when
        // hidden, and a second enabled Escape in the window would make Qt fire
        // neither.
        enabled: root.visible && !root.identityEditing
                 && root.renamingSessionDeviceId === ""
        onActivated: root.dismissLayer()
    }
    function dismissLayer() {
        var focused = root.Window.window ? root.Window.window.activeFocusItem
                                         : null
        if (settingsSearchField && settingsSearchField.text.length > 0)
            settingsSearchField.text = ""
        else if (root.nameColorPickerRef && root.nameColorPickerRef.open)
            root.nameColorPickerRef.open = false
        else if (root.isTextEditor(focused)
                 && focused !== settingsSearchField
                 && root.isInsideContent(focused)) {
            if (typeof focused.revertTyped === "function")
                focused.revertTyped()
            contentFlick.forceActiveFocus()
        } else
            root.goBack()
    }

    function moveSection(delta) {
        var rows = []
        for (var i = 0; i < navColumn.children.length; ++i) {
            var child = navColumn.children[i]
            if (child.objectName.indexOf("settingsNavRow_") === 0
                    && child.visible && child.enabled)
                rows.push(child)
        }
        if (rows.length === 0)
            return
        var current = -1
        for (var j = 0; j < rows.length; ++j) {
            if (rows[j].sectionKey === root.section)
                current = j
        }
        var next = current < 0 ? (delta < 0 ? rows.length - 1 : 0)
                               : Math.max(0, Math.min(rows.length - 1, current + delta))
        root.section = rows[next].sectionKey
        rows[next].forceActiveFocus(Qt.ShortcutFocusReason)
    }
    function moveSubsection(delta) {
        if (settingsSearchField.text.length > 0) {
            settingsSearchField.text = ""
            Qt.callLater(function() { root.moveSubsection(delta) })
            return
        }
        var entries = root.outlineEntries(root.section)
        var available = []
        for (var i = 0; i < entries.length; ++i) {
            if (root.firstShownAnchor(entries[i].anchors))
                available.push(i)
        }
        if (available.length === 0)
            return
        root.refreshOutlineActive()
        var current = available.indexOf(root.outlineActive)
        var next = current < 0 ? (delta < 0 ? available.length - 1 : 0)
                               : Math.max(0, Math.min(available.length - 1, current + delta))
        if (root.outlineCollapsed(root.section))
            root.toggleOutline(root.section)
        root.revealOutlineEntry(root.section, available[next])
        var outline = root.findInPane(navColumn, "settingsNavOutline_" + root.section)
        if (outline)
            outline.focusEntry(available[next])
    }
    readonly property bool dialogShortcutsEnabled:
        !root.identityEditing && root.renamingSessionDeviceId === ""
    Connections {
        target: app
        function onSettingsDialogActionRequested(action) {
            if (action === "dialog.close")
                root.dismissLayer()
            else if (action === "dialog.previousItem" || action === "dialog.nextItem"
                     || action === "settings.previousPage" || action === "settings.nextPage")
                root.moveSection(action === "dialog.previousItem" || action === "settings.previousPage" ? -1 : 1)
            else if (action === "settings.previousSubsection" || action === "settings.nextSubsection")
                root.moveSubsection(action === "settings.previousSubsection" ? -1 : 1)
            else if (action === "settings.halfPageUp" || action === "settings.halfPageDown")
                root.pageScrollBy(Math.max(24, contentFlick.height / 2)
                                  * (action === "settings.halfPageUp" ? -1 : 1))
        }
    }

    function isTextEditor(it) {
        return !!it && it.cursorPosition !== undefined
               && it.inputMethodHints !== undefined
    }
    function isInsideContent(it) {
        for (var p = it; p; p = p.parent) {
            if (p === contentFlick.contentItem || p === contentFlick)
                return true
        }
        return false
    }
    function pageScrollBy(dy) {
        var maxY = Math.max(0, contentFlick.contentHeight - contentFlick.height)
        settingsWheelArea.stopGlide()
        contentFlick.contentY = Math.max(0, Math.min(maxY, contentFlick.contentY + dy))
    }
    function openLinkMenuForFocus() {
        var f = root.Window.window ? root.Window.window.activeFocusItem : null
        if (!f || !root.isInsideContent(f))
            return false
        var p = f.mapToItem(contentFlick.contentItem, f.width / 2, f.height / 2)
        var entry = root.indexEntryAtContentY(p.y)
        if (!entry)
            return false
        root.linkMenuTarget = root.settingsLink(entry.section, entry.anchor)
        settingLinkMenu.popup(contentFlick, Math.max(0, p.x),
                              Math.max(0, p.y - contentFlick.contentY))
        return true
    }
    function ensurePageFocus() {
        if (!root.visible || !root.Window.window)
            return
        var f = root.Window.window.activeFocusItem
        for (var p = f; p; p = p.parent) {
            if (p === root)
                return
        }
        contentFlick.forceActiveFocus()
    }
    function isInsideNav(it) {
        for (var p = it; p; p = p.parent) {
            if (p === navFlick.contentItem || p === navFlick)
                return true
        }
        return false
    }
    Keys.onPressed: (event) => {
        var focused = root.Window.window ? root.Window.window.activeFocusItem : null
        var inContent = root.isInsideContent(focused)
        var inNav = root.isInsideNav(focused)
        var pageTarget = inNav ? navFlick : contentFlick
        var pagesHere = inContent || inNav || focused === root
        var page = Math.max(48, pageTarget.height - 48)
        var scrollTarget = function (dy) {
            if (inNav) {
                var maxY = Math.max(0, navFlick.contentHeight - navFlick.height)
                navFlick.contentY = Math.max(0, Math.min(maxY, navFlick.contentY + dy))
            } else {
                root.pageScrollBy(dy)
            }
        }
        switch (event.key) {
        case Qt.Key_PageDown:
            if (pagesHere) { scrollTarget(page); event.accepted = true }
            break
        case Qt.Key_PageUp:
            if (pagesHere) { scrollTarget(-page); event.accepted = true }
            break
        case Qt.Key_Home:
            if (pagesHere) { scrollTarget(-1e9); event.accepted = true }
            break
        case Qt.Key_End:
            if (pagesHere) { scrollTarget(1e9); event.accepted = true }
            break
        case Qt.Key_Down:
            if (inContent) { root.pageScrollBy(48); event.accepted = true }
            break
        case Qt.Key_Up:
            if (inContent) { root.pageScrollBy(-48); event.accepted = true }
            break
        case Qt.Key_Menu:
            event.accepted = root.openLinkMenuForFocus()
            break
        case Qt.Key_F10:
            if (event.modifiers & Qt.ShiftModifier)
                event.accepted = root.openLinkMenuForFocus()
            break
        }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.openSettings")]
        }
        enabled: root.visible
        onActivated: settingsSearchField.forceActiveFocus()
    }

    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoFocusSettingsSearch(query) {
            settingsSearchField.text = query
            settingsSearchField.forceActiveFocus()
        }
    }

    ConfirmDialog {
        id: clearIndexConfirm
        title: qsTr("Clear the search index?")
        confirmText: qsTr("Clear index")
        confirmKind: "dangerPrimary"
        onAccepted: app.messageSearch.clearIndex()
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.settingsOptionSize
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("Searching your history stops working until Lightning "
                + "has indexed it again, which it does on its own. No "
                + "messages are deleted — the index is only a copy Lightning "
                + "built so it can search.")
        }
    }

    ConfirmDialog {
        id: gifClearConfirm
        property string kind: ""
        function open(k) { kind = k; title = k === "favorites"
            ? qsTr("Clear saved provider GIFs?") : qsTr("Clear recent GIFs?"); visible = true }
        confirmText: qsTr("Clear")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.settingsOptionSize
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: gifClearConfirm.kind === "favorites"
                ? qsTr("Remove every provider GIF you've saved on this device? "
                       + "GIFs you saved out of chats are unaffected. This "
                       + "cannot be undone.")
                : qsTr("Clear the list of recently used GIFs on this device?")
        }
        onAccepted: {
            if (kind === "favorites") app.gif.favorites.clearAll()
            else if (kind === "recent") app.gif.recent.clearAll()
        }
    }

    ConfirmDialog {
        id: starredGifsClearConfirm
        objectName: "starredGifsClearConfirm"
        title: qsTr("Clear images saved on this device?")
        confirmText: qsTr("Delete")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.settingsOptionSize
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("Delete every image you saved out of a chat from this "
                       + "device? Saved provider GIFs are unaffected — they "
                       + "are only links. This cannot be undone.")
        }
        onAccepted: app.gif.starredStore.clearAll()
    }

    ConfirmDialog {
        id: displayNameClearConfirm
        objectName: "displayNameClearConfirm"
        title: qsTr("Clear your display name?")
        confirmText: qsTr("Clear name")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.settingsOptionSize
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("People will see your Matrix ID instead. You can set a "
                       + "new display name at any time.")
        }
        onAccepted: app.clearOwnDisplayName()
    }

    Rectangle {
        objectName: "settingsPageGround"
        anchors.fill: parent
        color: AppTheme.settingsPage
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "settingsHeaderBar"
            Layout.fillWidth: true
            implicitHeight: AppTheme.headerBandHeight
            color: AppTheme.settingsPage
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                spacing: AppTheme.spacing8 + 2
                Icon {
                    name: root.sectionIcon(root.section)
                    size: 22
                    color: AppTheme.bolt
                }
                Label {
                    objectName: "settingsHeaderTitle"
                    text: qsTr("Settings")
                    color: AppTheme.settingsHeading
                    font.family: AppTheme.menuFont
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                IconButton {
                    storm: true
                    objectName: "settingsCloseButton"
                    iconName: "close"
                    iconSize: 20
                    Accessible.name: qsTr("Close settings")
                    ToolTip.text: qsTr("Close settings")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.goBack()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.settingsDivider }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                objectName: "settingsNavColumn"
                Layout.fillHeight: true
                Layout.preferredWidth: root.navCollapsed
                    ? 56 : Math.max(200, Math.min(300, root.width * 0.3))
                Layout.minimumWidth: root.navCollapsed ? 56 : 200
                color: AppTheme.settingsNav
                clip: true

                Flickable {
                    id: navFlick
                    objectName: "settingsNavFlick"
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: navColumn.height + AppTheme.spacing12 * 2
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                    SmoothWheelArea {}
                    function ensureVisibleItem(it) {
                        var top = it.mapToItem(navFlick.contentItem, 0, 0).y
                        var bottom = top + it.height
                        if (top < navFlick.contentY)
                            navFlick.contentY = Math.max(0, top - AppTheme.spacing12)
                        else if (bottom > navFlick.contentY + navFlick.height)
                            navFlick.contentY = Math.min(
                                Math.max(0, navFlick.contentHeight - navFlick.height),
                                bottom - navFlick.height + AppTheme.spacing12)
                    }
                ColumnLayout {
                    id: navColumn
                    x: AppTheme.spacing12
                    y: AppTheme.spacing12
                    width: navFlick.width - AppTheme.spacing12 * 2
                    height: Math.max(implicitHeight,
                                     navFlick.height - AppTheme.spacing12 * 2)
                    spacing: 2

                    IconButton {
                        objectName: "settingsNavExpandButton"
                        visible: root.navCollapsed
                        storm: true
                        iconName: "search"
                        Layout.alignment: Qt.AlignHCenter
                        Accessible.name: qsTr("Show settings names and search")
                        ToolTip.text: qsTr("Show settings names and search")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.navOpenAtNarrow = true
                    }
                    RowLayout {
                        visible: !root.navCollapsed
                        spacing: AppTheme.spacing8
                        Layout.leftMargin: AppTheme.spacing8
                        Layout.topMargin: AppTheme.spacing4
                        Layout.bottomMargin: 0
                        Icon {
                            name: "bolt"
                            size: 15
                            color: AppTheme.bolt
                        }
                        Label {
                            text: qsTr("Settings")
                            color: AppTheme.settingsHeading
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                            Layout.fillWidth: true
                        }
                        IconButton {
                            objectName: "settingsNavCollapseButton"
                            visible: root.navNarrow
                            storm: true
                            iconName: "chevron_left"
                            size: "sm"
                            Accessible.name: qsTr("Show icons only")
                            ToolTip.text: qsTr("Show icons only")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.navOpenAtNarrow = false
                        }
                    }

                    RowLayout {
                        visible: !root.navCollapsed
                        Layout.fillWidth: true
                        Layout.bottomMargin: AppTheme.spacing4
                        spacing: AppTheme.spacing6
                        AppTextField {
                            id: settingsSearchField
                            objectName: "settingsSearchField"
                            Layout.fillWidth: true
                            searchIcon: true
                            clearButton: true
                            storm: true
                            placeholderText: qsTr("Search settings…")
                            Accessible.name: qsTr("Search settings")
                            onTextChanged: {
                                root.settingsSearchQuery = text
                                navFlick.contentY = 0
                                if (root.openSettingsLink(text))
                                    Qt.callLater(function() {
                                        settingsSearchField.text = ""
                                    })
                            }
                            onAccepted: {
                                if (root.matchedSearchResults.length > 0)
                                    root.revealSearchResult(
                                        root.matchedSearchResults[0])
                            }
                        }
                        MenuKeycap {
                            keys: {
                                var _rev = app.shortcuts.bindingRevision
                                return app.shortcuts.sequenceFor(
                                    "app.openSettings")
                            }
                        }
                    }

                    ColumnLayout {
                        objectName: "settingsSearchResults"
                        visible: !root.navCollapsed
                                 && root.settingsSearchQuery.trim().length > 0
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing4

                        Label {
                            objectName: "settingsSearchNoResults"
                            visible: root.matchedSearchResults.length === 0
                            Layout.fillWidth: true
                            text: qsTr("No matching settings")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                        }

                        Repeater {
                            model: root.matchedSearchResults
                            delegate: Rectangle {
                                id: resultRow
                                required property var modelData
                                required property int index
                                objectName: "settingsSearchResult_" + index
                                Layout.fillWidth: true
                                radius: AppTheme.radiusLg
                                color: resultHover.hovered
                                       ? AppTheme.selectedHover : "transparent"
                                implicitHeight: resultContent.implicitHeight
                                                + AppTheme.spacing8

                                RowLayout {
                                    id: resultContent
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.margins: AppTheme.spacing8
                                    spacing: AppTheme.spacing8

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            objectName: "settingsSearchResultTitle_"
                                                        + resultRow.index
                                            Layout.fillWidth: true
                                            textFormat: Text.StyledText
                                            text: root.highlightedTitle(
                                                resultRow.modelData.title,
                                                root.settingsSearchQuery)
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            font.weight: AppTheme.weightStrong
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: resultRow.modelData.breadcrumb
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.settingsHintSize
                                            elide: Label.ElideRight
                                        }
                                        TapHandler {
                                            onTapped: root.revealSearchResult(
                                                resultRow.modelData)
                                        }

                                        SegmentedControl {
                                            storm: true
                                            objectName: "settingsSearchInlineMessageLayout_" + resultRow.index
                                            visible: resultRow.modelData.control === "messageLayout"
                                            dense: true
                                            Layout.topMargin: AppTheme.spacing4
                                            model: [
                                                { label: qsTr("Modern"), value: 0 },
                                                { label: qsTr("Bubbles"), value: 1 },
                                                { label: qsTr("Compact"), value: 2 },
                                            ]
                                            current: app.settings.messageLayout
                                            onActivated: (value) =>
                                                app.settings.messageLayout = value
                                        }

                                        SegmentedControl {
                                            storm: true
                                            objectName: "settingsSearchInlineRailDepth_" + resultRow.index
                                            visible: resultRow.modelData.control === "spacesRailDepth"
                                            dense: true
                                            enabled: app.settings.spacesRailVisible
                                            opacity: enabled ? 1.0 : 0.5
                                            Layout.topMargin: AppTheme.spacing4
                                            model: [
                                                { label: qsTr("Regions"), value: 0 },
                                                { label: qsTr("Classic"), value: 1 },
                                            ]
                                            current: app.settings.spacesRailDepthStyle
                                            onActivated: (value) =>
                                                app.settings.spacesRailDepthStyle = value
                                        }
                                    }

                                    IconButton {
                                        storm: true
                                        objectName: "settingsSearchResultCopyLink_"
                                                    + resultRow.index
                                        iconName: "link"
                                        size: "sm"
                                        Accessible.name: qsTr("Copy link to this setting")
                                        ToolTip.text: qsTr("Copy link to this setting")
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 500
                                        onClicked: root.copyToClipboard(
                                            root.settingsLink(
                                                resultRow.modelData.section,
                                                resultRow.modelData.anchor))
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineMatchSystem_" + resultRow.index
                                        visible: resultRow.modelData.control === "matchSystem"
                                        checked: app.settings.theme === 0
                                        Accessible.name: qsTr("Match system light/dark")
                                        onToggled: app.settings.theme =
                                            app.settings.theme === 0
                                                ? AppTheme.effectiveTheme : 0
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineShowRoomActivity_" + resultRow.index
                                        visible: resultRow.modelData.control === "showRoomActivity"
                                        checked: app.settings.showRoomActivity
                                        Accessible.name: qsTr("Show room activity")
                                        onToggled: app.settings.showRoomActivity =
                                            !app.settings.showRoomActivity
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineAutoLoadPreviews_" + resultRow.index
                                        visible: resultRow.modelData.control === "autoLoadLinkPreviews"
                                        checked: app.settings.autoLoadLinkPreviews
                                        Accessible.name: qsTr(
                                            "Automatically load previews in unencrypted rooms")
                                        onToggled: app.settings.autoLoadLinkPreviews =
                                            !app.settings.autoLoadLinkPreviews
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineNotificationsEnabled_" + resultRow.index
                                        visible: resultRow.modelData.control === "notificationsEnabled"
                                        checked: app.settings.notificationsEnabled
                                        Accessible.name: qsTr("Desktop notifications")
                                        onToggled: app.settings.notificationsEnabled =
                                            !app.settings.notificationsEnabled
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineShowMembership_" + resultRow.index
                                        visible: resultRow.modelData.control === "showMembershipEvents"
                                        enabled: app.settings.showRoomActivity
                                        checked: app.settings.showMembershipEvents
                                        Accessible.name: qsTr("Joins, leaves and invites")
                                        onToggled: app.settings.showMembershipEvents =
                                            !app.settings.showMembershipEvents
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineShowProfileChanges_" + resultRow.index
                                        visible: resultRow.modelData.control === "showProfileChangeEvents"
                                        enabled: app.settings.showRoomActivity
                                        checked: app.settings.showProfileChangeEvents
                                        Accessible.name: qsTr("Display name and avatar changes")
                                        onToggled: app.settings.showProfileChangeEvents =
                                            !app.settings.showProfileChangeEvents
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineCollapseEmbeds_" + resultRow.index
                                        visible: resultRow.modelData.control === "collapseEmbeds"
                                        checked: app.settings.collapseEmbeds
                                        Accessible.name: qsTr("Collapse media and link embeds")
                                        onToggled: app.settings.collapseEmbeds =
                                            !app.settings.collapseEmbeds
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineLargeEmoji_" + resultRow.index
                                        visible: resultRow.modelData.control === "largeEmoji"
                                        checked: app.settings.largeEmoji
                                        Accessible.name: qsTr("Show emoji-only messages large")
                                        onToggled: app.settings.largeEmoji =
                                            !app.settings.largeEmoji
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineJumpToNewestOnSend_" + resultRow.index
                                        visible: resultRow.modelData.control === "jumpToNewestOnSend"
                                        checked: app.settings.jumpToNewestOnSend
                                        Accessible.name: qsTr("Go to the newest message when I send")
                                        onToggled: app.settings.jumpToNewestOnSend =
                                            !app.settings.jumpToNewestOnSend
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineReducedMotion_" + resultRow.index
                                        visible: resultRow.modelData.control === "reducedMotion"
                                        checked: app.settings.reducedMotion
                                        Accessible.name: qsTr("Reduce motion")
                                        onToggled: app.settings.reducedMotion =
                                            !app.settings.reducedMotion
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineSmoothScrolling_" + resultRow.index
                                        visible: resultRow.modelData.control === "smoothScrolling"
                                        checked: app.settings.smoothScrolling
                                        Accessible.name: qsTr("Smooth scrolling")
                                        onToggled: app.settings.smoothScrolling =
                                            !app.settings.smoothScrolling
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineSpaceBanners_" + resultRow.index
                                        visible: resultRow.modelData.control === "spaceBannersVisible"
                                        checked: app.settings.spaceBannersVisible
                                        Accessible.name: qsTr("Show Space banners")
                                        onToggled: app.settings.spaceBannersVisible =
                                            !app.settings.spaceBannersVisible
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineKeepRoomOrder_" + resultRow.index
                                        visible: resultRow.modelData.control === "keepRoomListOrderStill"
                                        checked: app.settings.keepRoomListOrderStill
                                        Accessible.name: qsTr("Keep the room list still while I use it")
                                        onToggled: app.settings.keepRoomListOrderStill =
                                            !app.settings.keepRoomListOrderStill
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineEnterNewline_" + resultRow.index
                                        visible: resultRow.modelData.control === "enterInsertsNewline"
                                        checked: app.settings.enterInsertsNewline
                                        Accessible.name: qsTr("Enter starts a new line")
                                        onToggled: app.settings.enterInsertsNewline =
                                            !app.settings.enterInsertsNewline
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineTextAsCaption_" + resultRow.index
                                        visible: resultRow.modelData.control === "sendTextAsCaption"
                                        checked: app.settings.sendTextAsCaption
                                        Accessible.name: qsTr(
                                            "Send text with an attachment as its caption")
                                        onToggled: app.settings.sendTextAsCaption =
                                            !app.settings.sendTextAsCaption
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineEmojiShortcodeAutoConvert_" + resultRow.index
                                        visible: resultRow.modelData.control === "emojiShortcodeAutoConvert"
                                        checked: app.settings.emojiShortcodeAutoConvert
                                        Accessible.name: qsTr("Convert :shortcode: to emoji")
                                        onToggled: app.settings.emojiShortcodeAutoConvert =
                                            !app.settings.emojiShortcodeAutoConvert
                                    }
                                }
                                HoverHandler { id: resultHover }
                                Accessible.role: Accessible.Button
                                Accessible.name: resultRow.modelData.title + " "
                                    + resultRow.modelData.breadcrumb
                            }
                        }

                        Flow {
                            visible: root.matchedSearchResults.length === 0
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing4
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: [
                                    qsTr("theme"), qsTr("background"),
                                    qsTr("gradient"), qsTr("microphone"),
                                    qsTr("notifications"), qsTr("privacy"),
                                ]
                                delegate: Rectangle {
                                    id: quickChip
                                    required property string modelData
                                    radius: AppTheme.radiusPill
                                    color: quickChipHover.hovered
                                           ? AppTheme.stormSelection : AppTheme.stormInset
                                    implicitWidth: quickChipLabel.implicitWidth
                                                   + AppTheme.spacing12
                                    implicitHeight: quickChipLabel.implicitHeight
                                                    + AppTheme.spacing6
                                    Label {
                                        id: quickChipLabel
                                        anchors.centerIn: parent
                                        text: quickChip.modelData
                                        font.pixelSize: AppTheme.settingsHintSize
                                        font.weight: AppTheme.weightMedium
                                        color: AppTheme.stormTextSecondary
                                    }
                                    HoverHandler { id: quickChipHover }
                                    TapHandler {
                                        onTapped: {
                                            settingsSearchField.text = quickChip.modelData
                                            settingsSearchField.forceActiveFocus()
                                        }
                                    }
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("Search for %1")
                                        .arg(quickChip.modelData)
                                }
                            }
                        }
                    }

                    SettingsNavGroupLabel {
                        objectName: "settingsNavGroup_user"
                        text: qsTr("User")
                        shown: root.navGroupShown(["account", "privacy", "sessions"])
                    }
                    SettingsNavRow {
                        sectionKey: "account"
                        iconName: "account_circle"
                        navLabel: qsTr("Account")
                    }
                    SettingsNavOutlineFor { sectionKey: "account" }
                    SettingsNavRow {
                        sectionKey: "privacy"
                        iconName: "verified_user"
                        navLabel: qsTr("Privacy & security")
                    }
                    SettingsNavOutlineFor { sectionKey: "privacy" }
                    SettingsNavRow {
                        sectionKey: "sessions"
                        iconName: "devices"
                        navLabel: qsTr("Sessions")
                        alert: app.sessionVerificationWarning
                    }

                    SettingsNavGroupLabel {
                        objectName: "settingsNavGroup_interface"
                        text: qsTr("Interface")
                        shown: root.navGroupShown(["appearance", "layout", "chat",
                                                   "shortcuts"])
                    }
                    SettingsNavRow {
                        sectionKey: "appearance"
                        iconName: "palette"
                        navLabel: qsTr("Appearance")
                    }
                    SettingsNavOutlineFor { sectionKey: "appearance" }
                    SettingsNavRow {
                        sectionKey: "layout"
                        iconName: "grid_view"
                        navLabel: qsTr("Layout")
                    }
                    SettingsNavOutlineFor { sectionKey: "layout" }
                    SettingsNavRow {
                        sectionKey: "chat"
                        iconName: "forum"
                        navLabel: qsTr("Chat")
                    }
                    SettingsNavOutlineFor { sectionKey: "chat" }
                    SettingsNavRow {
                        sectionKey: "shortcuts"
                        iconName: "keyboard_return"
                        navLabel: qsTr("Keyboard shortcuts")
                    }

                    SettingsNavGroupLabel {
                        objectName: "settingsNavGroup_behavior"
                        text: qsTr("Behavior")
                        shown: root.navGroupShown(["notifications", "sound", "labs"])
                    }
                    SettingsNavRow {
                        sectionKey: "notifications"
                        iconName: "notifications"
                        navLabel: qsTr("Notifications")
                    }
                    SettingsNavOutlineFor { sectionKey: "notifications" }
                    SettingsNavRow {
                        sectionKey: "sound"
                        iconName: "volume_up"
                        navLabel: qsTr("Sound & video")
                    }
                    SettingsNavOutlineFor { sectionKey: "sound" }
                    SettingsNavRow {
                        sectionKey: "labs"
                        iconName: "science"
                        navLabel: qsTr("Labs")
                    }
                    Item { Layout.fillHeight: true }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        visible: !root.navCollapsed
                                 && root.settingsSearchQuery.trim().length > 0
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: AppTheme.stormBorder
                        }
                        Label {
                            objectName: "settingsSearchMatchCounter"
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing8
                            Layout.bottomMargin: AppTheme.spacing4
                            Layout.leftMargin: AppTheme.spacing8
                            text: {
                                var sections = 0
                                for (var k in root.matchedSearchSections) {
                                    if (root.matchedSearchSections[k] === true)
                                        sections++
                                }
                                return qsTr("Matches · %1").arg(
                                    qsTr("%n section(s)", "", sections) + " · "
                                    + qsTr("%n setting(s)", "",
                                           root.matchedSearchResults.length))
                            }
                            font.pixelSize: AppTheme.settingsHintSize
                            font.weight: AppTheme.weightMedium
                            color: AppTheme.stormTextMuted
                            elide: Label.ElideRight
                        }
                    }

                    SettingsNavGroupLabel {
                        objectName: "settingsNavGroup_about"
                        text: qsTr("About")
                        shown: root.navGroupShown(["updates", "about"])
                    }
                    SettingsNavRow {
                        sectionKey: "updates"
                        iconName: "download"
                        navLabel: qsTr("Updates")
                    }
                    SettingsNavRow {
                        sectionKey: "about"
                        iconName: "info"
                        navLabel: qsTr("About")
                    }
                }
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: AppTheme.settingsDivider }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
            StormBand {
                objectName: "aboutStormBand"
                visible: root.section === "about"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 190
                backdropColor: AppTheme.stormDeep
            }
            Flickable {
                id: contentFlick
                objectName: "settingsContentFlick"
                anchors.fill: parent
                contentHeight: contentColumn.implicitHeight + AppTheme.spacing24 * 2
                clip: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                SmoothWheelArea { id: settingsWheelArea }
                Connections {
                    target: root
                    function onSectionChanged() {
                        settingsWheelArea.stopGlide()
                        contentFlick.contentY = 0
                        root.ensurePageFocus()
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: {
                        var f = root.Window.window
                                ? root.Window.window.activeFocusItem : null
                        if (!root.isInsideContent(f))
                            contentFlick.forceActiveFocus(Qt.MouseFocusReason)
                    }
                }
                onContentYChanged: root.scheduleOutlineRefresh()
                onContentHeightChanged: root.scheduleOutlineRefresh()

                Item {
                    id: linkMenuLayer
                    objectName: "settingLinkMenuLayer"
                    z: 50
                    x: 0
                    y: 0
                    width: contentFlick.width
                    height: contentFlick.contentHeight
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onTapped: (point) => {
                            var entry = root.indexEntryAtContentY(
                                point.position.y)
                            if (!entry)
                                return
                            root.linkMenuTarget = root.settingsLink(
                                entry.section, entry.anchor)
                            settingLinkMenu.popup(
                                contentFlick, point.position.x,
                                point.position.y - contentFlick.contentY)
                        }
                    }
                }
                AppMenu {
                    id: settingLinkMenu
                    objectName: "settingLinkMenu"
                    AppMenuItem {
                        objectName: "settingLinkMenuCopy"
                        iconName: "link"
                        text: qsTr("Copy link to this setting")
                        onTriggered: root.copyToClipboard(root.linkMenuTarget)
                    }
                }

                Rectangle {
                    id: searchRevealHalo
                    objectName: "settingsSearchRevealHalo"
                    z: 5
                    opacity: 0
                    enabled: false
                    visible: opacity > 0
                    color: "transparent"
                    radius: AppTheme.radiusMd
                    border.width: 2
                    border.color: AppTheme.bolt
                    function placeOver(target) {
                        var p = target.mapToItem(contentFlick.contentItem, 0, 0)
                        searchRevealHalo.x = p.x - AppTheme.spacing6
                        searchRevealHalo.y = p.y - AppTheme.spacing6
                        searchRevealHalo.width = target.width + AppTheme.spacing6 * 2
                        searchRevealHalo.height = target.height + AppTheme.spacing6 * 2
                    }
                    function flashOver(target) {
                        searchRevealHalo.placeOver(target)
                        haloFlash.restart()
                    }
                    SequentialAnimation {
                        id: haloFlash
                        PropertyAction {
                            target: searchRevealHalo
                            property: "opacity"
                            value: 1.0
                        }
                        PauseAnimation { duration: 400 }
                        NumberAnimation {
                            target: searchRevealHalo
                            property: "opacity"
                            to: 0.0
                            duration: 650
                        }
                    }
                }

                ColumnLayout {
                    id: contentColumn
                    x: AppTheme.spacing24
                    y: AppTheme.spacing24
                    width: Math.min(860, contentFlick.width - AppTheme.spacing24 * 2)
                    spacing: AppTheme.spacing16

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("appearance")
                        visible: root.section === "appearance"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Appearance")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("Theme, colours and gradients, chat background, "
                                       + "text and fonts — per account. How the window "
                                       + "is arranged is under Layout; the timeline and "
                                       + "the message box are under Chat.")
                            color: AppTheme.settingsText
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsGroupLabel { text: qsTr("Theme") }
                        Flow {
                            id: featuredThemeFlow
                            objectName: "featuredThemeFlow"
                            Layout.fillWidth: true
                            spacing: 14
                            function previewFor(id, name) {
                                var p = AppTheme.paletteForTheme(id)
                                return { id: id, name: name,
                                         frame: p.background, rail: p.sidebar,
                                         bar1: p.border, bar2: p.surface,
                                         accent: p.accent }
                            }
                            Repeater {
                                model: [
                                    featuredThemeFlow.previewFor(9,  qsTr("Indigo Night")),
                                    featuredThemeFlow.previewFor(8,  qsTr("Moss Light")),
                                    featuredThemeFlow.previewFor(10, qsTr("Deep Teal")),
                                    featuredThemeFlow.previewFor(11, qsTr("Storm")),
                                ]
                                delegate: Rectangle {
                                    id: themeCard
                                    required property var modelData
                                    objectName: "featuredThemeCard_" + modelData.id
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    readonly property bool inEffect:
                                        app.settings.theme === 0
                                        && AppTheme.effectiveTheme === modelData.id
                                    readonly property bool cardIsLive:
                                        selectedTheme || inEffect
                                    implicitWidth: 150
                                    implicitHeight: previewTop.height
                                                    + Math.ceil(cardFoot.height)
                                    radius: AppTheme.radiusLg
                                    color: AppTheme.stormPanel
                                    border.width: 0
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: themeCard.inEffect
                                        ? qsTr("%1 (in effect)").arg(modelData.name)
                                        : modelData.name
                                    Accessible.checked: themeCard.selectedTheme
                                    Accessible.focusable: true
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: app.settings.theme = modelData.id
                                    Keys.onSpacePressed: app.settings.theme = modelData.id

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        radius: parent.radius + 3
                                        z: -1
                                        visible: themeCard.cardIsLive
                                        color: "transparent"
                                        border.width: 3
                                        border.color: Qt.alpha(AppTheme.bolt,
                                                               0.18)
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -6
                                        radius: parent.radius + 6
                                        z: -1
                                        visible: themeCard.activeFocus
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                    }

                                    Rectangle {
                                        id: previewTop
                                        objectName: "themeCardPreview_" + themeCard.modelData.id
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: 96
                                        color: themeCard.modelData.frame
                                        topLeftRadius: AppTheme.radiusLg - 1
                                        topRightRadius: AppTheme.radiusLg - 1
                                        Rectangle {
                                            x: 10; y: 10
                                            width: 26
                                            height: parent.height - 20
                                            radius: 6
                                            color: themeCard.modelData.rail
                                        }
                                        Column {
                                            id: previewBars
                                            x: 10 + 26 + 6
                                            y: 10
                                            spacing: 6
                                            readonly property real barSpan:
                                                previewTop.width - x - 10
                                            Rectangle {
                                                width: previewBars.barSpan * 0.7
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.bar1
                                            }
                                            Rectangle {
                                                width: previewBars.barSpan * 0.5
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.bar2
                                            }
                                            Rectangle {
                                                objectName: "themeCardAccentBar_" + themeCard.modelData.id
                                                width: previewBars.barSpan * 0.6
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.accent
                                            }
                                        }
                                    }

                                    Rectangle {
                                        id: cardFoot
                                        objectName: "themeCardFoot_"
                                                    + themeCard.modelData.id
                                        anchors.top: previewTop.bottom
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: footRow.implicitHeight + 20
                                        color: AppTheme.stormPanel
                                        bottomLeftRadius: AppTheme.radiusLg - 1
                                        bottomRightRadius: AppTheme.radiusLg - 1
                                        RowLayout {
                                            id: footRow
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            spacing: 8
                                            Rectangle {
                                                objectName: "themeCardRadio_"
                                                            + themeCard.modelData.id
                                                implicitWidth: 14
                                                implicitHeight: 14
                                                radius: 7
                                                color: themeCard.selectedTheme
                                                       ? AppTheme.bolt : "transparent"
                                                border.width: 2
                                                border.color: themeCard.cardIsLive
                                                              ? AppTheme.bolt
                                                              : AppTheme.stormTextMuted
                                                Rectangle {
                                                    anchors.centerIn: parent
                                                    width: 5; height: 5; radius: 2.5
                                                    visible: themeCard.selectedTheme
                                                    color: AppTheme.stormText
                                                }
                                            }
                                            Label {
                                                text: themeCard.modelData.name
                                                textFormat: Text.PlainText
                                                color: AppTheme.stormText
                                                font.pixelSize: AppTheme.settingsOptionSize
                                                font.weight: AppTheme.weightStrong
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }

                                    Rectangle {
                                        objectName: "themeCardOutline_"
                                                    + themeCard.modelData.id
                                        anchors.fill: parent
                                        z: 5
                                        radius: AppTheme.radiusLg
                                        color: "transparent"
                                        border.width: themeCard.cardIsLive ? 2 : 1
                                        border.color: themeCard.cardIsLive
                                                      ? AppTheme.bolt
                                                      : AppTheme.stormTextMuted
                                    }

                                    TapHandler {
                                        onTapped: app.settings.theme =
                                            themeCard.modelData.id
                                    }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                }
                            }
                        }

                        SettingsGroupLabel { text: qsTr("More themes") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Repeater {
                                model: AppTheme.themeList.filter(
                                    (t) => t.id !== 8 && t.id !== 9 && t.id !== 10
                                           && t.id !== 11 && t.id !== 12)
                                delegate: Rectangle {
                                    id: miniThemeCard
                                    required property var modelData
                                    objectName: "miniThemeCard_" + modelData.id
                                    readonly property var pal:
                                        AppTheme.paletteForTheme(modelData.id)
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    implicitWidth: miniRow.implicitWidth + 24
                                    implicitHeight: 34
                                    radius: AppTheme.radiusTile
                                    color: selectedTheme ? AppTheme.stormSelection
                                           : miniHover.hovered
                                             ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                             : AppTheme.stormInset
                                    // Integer border: 1.5px renders as two
                                    // half-covered rows.
                                    border.width: selectedTheme ? 2 : 0
                                    border.color: AppTheme.bolt
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: modelData.name
                                    Accessible.focusable: true
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: app.settings.theme = modelData.id
                                    Keys.onSpacePressed: app.settings.theme = modelData.id
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        radius: parent.radius + 3
                                        visible: miniThemeCard.activeFocus
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                    }
                                    RowLayout {
                                        id: miniRow
                                        anchors.centerIn: parent
                                        spacing: 6
                                        Rectangle {
                                            implicitWidth: 12; implicitHeight: 12
                                            radius: 4
                                            color: miniThemeCard.pal.background
                                            border.color: miniThemeCard.pal.border
                                            Rectangle {
                                                anchors.right: parent.right
                                                anchors.bottom: parent.bottom
                                                width: 5; height: 5; radius: 2
                                                color: miniThemeCard.pal.accent
                                            }
                                        }
                                        Label {
                                            text: miniThemeCard.modelData.name
                                            textFormat: Text.PlainText
                                            color: miniThemeCard.selectedTheme
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextSecondary
                                            font.pixelSize: AppTheme.settingsHintSize
                                            font.weight: AppTheme.weightStrong
                                        }
                                    }
                                    TapHandler {
                                        onTapped: app.settings.theme =
                                            miniThemeCard.modelData.id
                                    }
                                    HoverHandler {
                                        id: miniHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                }
                            }
                        }

                        SettingsGroupLabel { text: qsTr("Custom colours and gradients") }
                        Rectangle {
                            id: customThemeRow
                            objectName: "customThemeRow"
                            Layout.fillWidth: true
                            implicitHeight: customThemeLayout.implicitHeight
                                            + AppTheme.spacing12 * 2
                            radius: AppTheme.radiusTile
                            readonly property bool selectedTheme:
                                app.settings.theme === 12
                            color: selectedTheme ? AppTheme.stormSelection
                                                 : AppTheme.stormInset
                            border.width: selectedTheme ? 2 : 0
                            border.color: AppTheme.bolt

                            RowLayout {
                                id: customThemeLayout
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing12
                                spacing: AppTheme.spacing12

                                Row {
                                    spacing: 2
                                    Repeater {
                                        model: app.customTheme.exists
                                               ? ["rail", "roomList", "timelineBackground",
                                                  "sidePanel", "accent"]
                                               : []
                                        delegate: Rectangle {
                                            id: stripSwatch
                                            required property string modelData
                                            width: 10
                                            height: 28
                                            radius: 2
                                            color: {
                                                var o = app.customTheme.colors
                                                var pal = AppTheme.paletteForTheme(12)
                                                return pal[modelData] !== undefined
                                                       ? pal[modelData]
                                                       : AppTheme.stormTextMuted
                                            }
                                            readonly property var gradientSpec:
                                                AppTheme.inheritedGradient(
                                                    modelData,
                                                    app.customTheme.gradients,
                                                    app.customTheme.colors)
                                            ThemedSurface {
                                                anchors.fill: parent
                                                visible: stripSwatch.gradientSpec !== null
                                                role: stripSwatch.modelData
                                                flatFill: false
                                                specOverride: stripSwatch.gradientSpec
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Label {
                                        text: !app.customTheme.exists
                                              ? qsTr("Build your own theme")
                                              : app.customTheme.name.length > 0
                                                ? app.customTheme.name
                                                : qsTr("Your theme")
                                        textFormat: Text.PlainText
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        text: !app.customTheme.exists
                                              ? qsTr("Pick a colour for any part of the window, or a gradient for its large areas, and watch a sample room repaint.")
                                              : app.customTheme.themes.length > 1
                                                ? qsTr("%n colour(s) changed. %1 themes saved.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                                      .arg(app.customTheme.themes.length)
                                                : qsTr("%n colour(s) changed. Pick a colour for any part of the window.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    Label {
                                        objectName: "customThemeGradientSummary"
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        elide: Text.ElideRight
                                        textFormat: Text.PlainText
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: {
                                            var g = app.customTheme.gradients
                                            var roles = app.customTheme.roles
                                            var names = []
                                            for (var i = 0; g && i < roles.length; ++i) {
                                                if (g[roles[i].key])
                                                    names.push(roles[i].label)
                                            }
                                            return names.length > 0
                                                   ? qsTr("Gradients: %1").arg(names.join(", "))
                                                   : ""
                                        }
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        Layout.topMargin: AppTheme.spacing6
                                        spacing: AppTheme.spacing8
                                        AppButton {
                                            objectName: "customThemeEditButton"
                                            kind: app.customTheme.exists ? "secondary"
                                                                         : "primary"
                                            storm: true
                                            text: app.customTheme.exists ? qsTr("Edit colours")
                                                                         : qsTr("Create a theme")
                                            onClicked: {
                                                root.themeEditorStartRole = ""
                                                themeEditorLoader.active = true
                                            }
                                        }
                                        AppButton {
                                            objectName: "customThemeGradientButton"
                                            storm: true
                                            iconName: "palette"
                                            text: qsTr("Add a gradient")
                                            Accessible.description: qsTr(
                                                "Opens the theme editor on the "
                                                + "conversation background, where "
                                                + "you can choose a gradient")
                                            onClicked: {
                                                root.themeEditorStartRole = "background"
                                                themeEditorLoader.active = true
                                            }
                                        }
                                        AppButton {
                                            objectName: "customThemeUseButton"
                                            storm: true
                                            visible: app.customTheme.exists
                                                     && !customThemeRow.selectedTheme
                                            text: qsTr("Use this theme")
                                            onClicked: app.settings.theme = 12
                                        }
                                    }
                                }
                            }
                        }

                        Loader {
                            id: themeEditorLoader
                            objectName: "themeEditorLoader"
                            active: false
                            sourceComponent: ThemeEditorDialog {
                                initialRole: root.themeEditorStartRole
                            }
                            onLoaded: item.open()
                            onActiveChanged: {
                                if (!active)
                                    root.themeEditorStartRole = ""
                            }
                            Connections {
                                target: themeEditorLoader.item
                                function onClosed() {
                                    themeEditorLoader.active = false
                                }
                            }
                            Connections {
                                target: app.demo
                                enabled: app.screenshotDemoActive
                                function onDemoOpenThemeEditor(role) {
                                    root.themeEditorStartRole = role
                                    themeEditorLoader.active = true
                                }
                            }
                        }

                        AbstractButton {
                            id: matchSystemSwitch
                            objectName: "matchSystemSwitch"
                            Layout.topMargin: AppTheme.spacing8
                            implicitWidth: matchRow.implicitWidth
                            implicitHeight: 24
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            checkable: true
                            checked: app.settings.theme === 0
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: qsTr("Match system light/dark")
                            onClicked: app.settings.theme =
                                app.settings.theme === 0 ? AppTheme.effectiveTheme : 0
                            contentItem: RowLayout {
                                id: matchRow
                                spacing: 10
                                Rectangle {
                                    objectName: "matchSystemTrack"
                                    readonly property bool hot:
                                        matchSystemSwitch.hovered
                                        || matchSystemSwitch.down
                                    implicitWidth: 36
                                    implicitHeight: 20
                                    radius: AppTheme.radiusPill
                                    color: {
                                        if (app.settings.theme === 0)
                                            return hot ? AppTheme.accentHover
                                                       : AppTheme.bolt
                                        return hot ? Qt.lighter(
                                                         AppTheme.stormBorderStrong, 1.18)
                                                   : AppTheme.stormBorderStrong
                                    }
                                    Behavior on color {
                                        enabled: !AppTheme.reducedMotion
                                        ColorAnimation { duration: 120 }
                                    }
                                    Rectangle {
                                        width: 16; height: 16; radius: 8
                                        scale: matchSystemSwitch.down ? 1.12 : 1.0
                                        Behavior on scale {
                                            enabled: !AppTheme.reducedMotion
                                            NumberAnimation { duration: 90 }
                                        }
                                        color: app.settings.theme === 0
                                               ? AppTheme.boltInk : "#FFFFFF"
                                        y: 2
                                        x: app.settings.theme === 0 ? 18 : 2
                                        Behavior on x {
                                            NumberAnimation { duration: 150 }
                                        }
                                    }
                                }
                                Label {
                                    text: qsTr("Match system light/dark")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                            }
                            background: Item {}
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -4
                                radius: AppTheme.radiusMd
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.bolt
                                visible: matchSystemSwitch.visualFocus
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                            text: qsTr("When on, Lightning follows the system scheme: "
                                       + "Moss Light in light mode, Indigo Night in dark mode.")
                        }

                        ChatBackgroundSettings {
                            objectName: "chatBackgroundSettingsSection"
                            Layout.fillWidth: true
                            onMakeGradientRequested: {
                                root.themeEditorStartRole = "background"
                                themeEditorLoader.active = true
                            }
                        }

                        SettingsGroupLabel { text: qsTr("Text size") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 12
                                Layout.preferredWidth: 20
                            }
                            Slider {
                                id: textScaleSlider
                                objectName: "textScaleSlider"
                                implicitHeight: 24
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 75
                                to: 150
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.textScale
                                onMoved: if (!pressed)
                                    app.settings.textScale = Math.round(value)
                                onPressedChanged: if (!pressed)
                                    app.settings.textScale = Math.round(value)
                                Accessible.name: qsTr("Message text size")
                                background: Rectangle {
                                    x: textScaleSlider.leftPadding
                                    y: textScaleSlider.topPadding
                                       + textScaleSlider.availableHeight / 2 - 2
                                    width: textScaleSlider.availableWidth
                                    height: 4
                                    radius: AppTheme.radiusPill
                                    color: AppTheme.stormInset
                                    Rectangle {
                                        width: textScaleSlider.visualPosition
                                               * parent.width
                                        height: parent.height
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.bolt
                                    }
                                }
                                handle: Rectangle {
                                    x: textScaleSlider.leftPadding
                                       + textScaleSlider.visualPosition
                                         * (textScaleSlider.availableWidth - width)
                                    y: textScaleSlider.topPadding
                                       + textScaleSlider.availableHeight / 2
                                       - height / 2
                                    width: 16; height: 16; radius: 8
                                    color: "#FFFFFF"
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.topMargin: 1
                                        anchors.bottomMargin: -1
                                        radius: 8
                                        z: -1
                                        color: "#40000000"
                                    }
                                    border.width: textScaleSlider.visualFocus ? 2 : 1
                                    border.color: textScaleSlider.visualFocus
                                                  ? AppTheme.bolt
                                                  : AppTheme.stormBorderStrong
                                }
                            }
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 18
                                Layout.preferredWidth: 20
                            }
                            SettingsPercentBox {
                                objectName: "textScalePercentBox"
                                slider: textScaleSlider
                                follows: app.settings.textScale
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                            text: qsTr("Scales message, list and Settings text, "
                                       + "and the Spaces rail with it, so its "
                                       + "nesting levels stay readable at any "
                                       + "size. Icons and the window layout keep "
                                       + "their size — Interface zoom below "
                                       + "scales the whole window.")
                        }

                        SettingsGroupLabel { text: qsTr("Interface zoom") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Item { Layout.preferredWidth: 20 }
                            Slider {
                                id: interfaceZoomSlider
                                objectName: "interfaceZoomSlider"
                                implicitHeight: 24
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 75
                                to: 150
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.interfaceZoom
                                onMoved: if (!pressed)
                                    app.settings.interfaceZoom = Math.round(value)
                                onPressedChanged: if (!pressed)
                                    app.settings.interfaceZoom = Math.round(value)
                                Accessible.name: qsTr("Interface zoom")
                                background: Rectangle {
                                    x: interfaceZoomSlider.leftPadding
                                    y: interfaceZoomSlider.topPadding
                                       + interfaceZoomSlider.availableHeight / 2
                                       - 2
                                    width: interfaceZoomSlider.availableWidth
                                    height: 4
                                    radius: AppTheme.radiusPill
                                    color: AppTheme.stormInset
                                    Rectangle {
                                        width: interfaceZoomSlider.visualPosition
                                               * parent.width
                                        height: parent.height
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.bolt
                                    }
                                }
                                handle: Rectangle {
                                    x: interfaceZoomSlider.leftPadding
                                       + interfaceZoomSlider.visualPosition
                                         * (interfaceZoomSlider.availableWidth
                                            - width)
                                    y: interfaceZoomSlider.topPadding
                                       + interfaceZoomSlider.availableHeight / 2
                                       - height / 2
                                    width: 16; height: 16; radius: 8
                                    color: "#FFFFFF"
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.topMargin: 1
                                        anchors.bottomMargin: -1
                                        radius: 8
                                        z: -1
                                        color: "#40000000"
                                    }
                                    border.width: interfaceZoomSlider.visualFocus
                                                  ? 2 : 1
                                    border.color: interfaceZoomSlider.visualFocus
                                                  ? AppTheme.bolt
                                                  : AppTheme.stormBorderStrong
                                }
                            }
                            Item { Layout.preferredWidth: 20 }
                            SettingsPercentBox {
                                objectName: "interfaceZoomPercentBox"
                                slider: interfaceZoomSlider
                                follows: app.settings.interfaceZoom
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                            text: qsTr("Scales the entire interface — text, "
                                       + "icons and layout. Ctrl+= and Ctrl+- "
                                       + "adjust it anywhere. Takes effect the "
                                       + "next time Lightning starts.")
                        }

                        SettingsGroupLabel { text: qsTr("Text rendering") }
                        AppComboBox {
                            id: textRenderingCombo
                            objectName: "textRenderingCombo"
                            storm: true
                            Layout.fillWidth: true
                            Layout.maximumWidth: 420
                            textRole: "label"
                            valueRole: "value"
                            model: [
                                { label: qsTr("Smooth (default)"), value: 0 },
                                { label: qsTr("Native (follows your system's "
                                              + "font hinting and anti-aliasing)"),
                                  value: 1 }
                            ]
                            function syncFromSetting() {
                                syncToValue(app.settings.textRendering)
                            }
                            Component.onCompleted: syncFromSetting()
                            Connections {
                                target: app.settings
                                function onTextRenderingChanged() {
                                    textRenderingCombo.syncFromSetting()
                                }
                            }
                            onActivated: app.settings.textRendering = currentValue
                            Accessible.name: qsTr("Text rendering")
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                            text: qsTr("Native hands text to your system's "
                                       + "font renderer, so it follows its "
                                       + "hinting and anti-aliasing settings. "
                                       + "Takes effect the next time "
                                       + "Lightning starts.")
                        }

                        SettingsGroupLabel { text: qsTr("Font") }
                        ColumnLayout {
                            objectName: "uiFontSelector"
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: app.settings.uiFontChoices()
                                ItemDelegate {
                                    id: fontRow
                                    required property string modelData
                                    readonly property bool selected:
                                        app.settings.uiFont === modelData
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 420
                                    implicitHeight: 56
                                    topPadding: 0
                                    bottomPadding: 0
                                    Accessible.name:
                                        qsTr("Use the %1 font").arg(modelData)
                                    onClicked:
                                        app.settings.uiFont = modelData
                                    background: Rectangle {
                                        radius: AppTheme.radiusMd
                                        color: fontRow.selected
                                               ? AppTheme.stormSelection
                                               : (fontRow.hovered || fontRow.down)
                                                 ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                                 : AppTheme.stormInset
                                        border.width: 1
                                        border.color: fontRow.visualFocus
                                                      ? AppTheme.bolt
                                                      : fontRow.selected
                                                        ? AppTheme.bolt
                                                        : AppTheme.stormBorder
                                    }
                                    contentItem: RowLayout {
                                        spacing: AppTheme.spacing12
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Label {
                                                text: fontRow.modelData
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.settingsOptionSize
                                                font.weight: AppTheme.weightStrong
                                                color: AppTheme.stormText
                                            }
                                            Label {
                                                text: qsTr("Messages, rooms and settings")
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.settingsOptionSize
                                                color: AppTheme.stormTextMuted
                                            }
                                        }
                                        Icon {
                                            visible: fontRow.selected
                                            name: "check"
                                            size: 16
                                            color: AppTheme.bolt
                                        }
                                    }
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsHintSize
                            text: qsTr("Applies to the whole interface. Code, Matrix "
                                       + "IDs, icons, and emoji keep their own fonts.")
                        }

                        SettingsCard {
                            objectName: "systemFontCard"
                            visible: root.fontManager !== null
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    text: qsTr("Interface font")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    id: systemUiFontCombo
                                    objectName: "systemUiFontCombo"
                                    storm: true
                                    filterable: true
                                    filterPlaceholder: qsTr("Search fonts")
                                    Layout.fillWidth: true
                                    model: root.fontManager ? root.fontManager.uiFamilies : []
                                    restoreAfterModelReset: false
                                    Accessible.name: qsTr("Interface font family")
                                    // Never bind currentIndex (see
                                    // AppComboBox): indexOfValue() is -1 at
                                    // creation.
                                    Component.onCompleted:
                                        syncToValue(root.fontManager ? root.fontManager.storedUiFamily : "")
                                    onModelChanged:
                                        syncToValue(root.fontManager ? root.fontManager.storedUiFamily : "")
                                    Connections {
                                        target: root.fontManager
                                        function onSelectionChanged() {
                                            systemUiFontCombo.syncToValue(
                                                root.fontManager.storedUiFamily)
                                        }
                                    }
                                    onActivated: root.fontManager.setUiFamily(currentText)
                                }
                                Label {
                                    objectName: "uiFontMissingNotice"
                                    visible: root.fontManager && !root.fontManager.uiFamilyAvailable
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: root.fontManager
                                        ? (root.fontManager.uiFamilyUnavailableReason === "unusable"
                                           ? qsTr("\u201C%1\u201D has no letters to draw text "
                                                  + "with — it is an emoji or icon face — so "
                                                  + "Lightning is using %2. Pick another font "
                                                  + "above.")
                                                 .arg(root.fontManager.storedUiFamily)
                                           : qsTr("\u201C%1\u201D is not installed on this "
                                               + "computer, so Lightning is drawing %2 "
                                               + "instead. Your choice is kept — install "
                                               + "the font and it comes back.")
                                              .arg(root.fontManager.storedUiFamily))
                                              .arg(root.fontManager.uiFamily)
                                        : ""
                                }

                                Label {
                                    text: qsTr("Code font")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    id: monoFontCombo
                                    objectName: "monoFontCombo"
                                    storm: true
                                    filterable: true
                                    filterPlaceholder: qsTr("Search fonts")
                                    Layout.fillWidth: true
                                    model: root.fontManager ? root.fontManager.monospaceFamilies : []
                                    restoreAfterModelReset: false
                                    Accessible.name: qsTr("Monospace font family")
                                    Component.onCompleted:
                                        syncToValue(root.fontManager ? root.fontManager.storedMonospaceFamily : "")
                                    onModelChanged:
                                        syncToValue(root.fontManager ? root.fontManager.storedMonospaceFamily : "")
                                    Connections {
                                        target: root.fontManager
                                        function onSelectionChanged() {
                                            monoFontCombo.syncToValue(
                                                root.fontManager.storedMonospaceFamily)
                                        }
                                    }
                                    onActivated: root.fontManager.setMonospaceFamily(currentText)
                                }
                                Label {
                                    objectName: "monoFontMissingNotice"
                                    visible: root.fontManager && !root.fontManager.monospaceFamilyAvailable
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: root.fontManager
                                        ? (root.fontManager.monospaceFamilyUnavailableReason === "unusable"
                                           ? qsTr("\u201C%1\u201D cannot draw letters and "
                                                  + "digits — it is an emoji or icon face — so "
                                                  + "code is drawn in %2. Pick another font "
                                                  + "above.")
                                                 .arg(root.fontManager.storedMonospaceFamily)
                                                 .arg(root.fontManager.monospaceFamily)
                                           : qsTr("\u201C%1\u201D is not installed, so code "
                                               + "is drawn in %2. Your choice is kept.")
                                              .arg(root.fontManager.storedMonospaceFamily)
                                              .arg(root.fontManager.monospaceFamily))
                                        : ""
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("The code font is used for code blocks, "
                                               + "keyboard shortcuts and Matrix "
                                               + "identifiers.")
                                }
                            }
                        }

                        SettingsCard {
                            objectName: "importedFontCard"
                            visible: root.fontManager !== null
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Your own fonts")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Load a TrueType or OpenType file that is "
                                               + "not installed system-wide. Lightning "
                                               + "keeps its own copy, so moving or "
                                               + "deleting the original changes nothing. "
                                               + "Only fonts you pick here are ever "
                                               + "loaded.")
                                }
                                Repeater {
                                    model: root.fontManager ? root.fontManager.importedFonts : []
                                    RowLayout {
                                        id: importedRow
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                            color: importedRow.modelData.available
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            text: importedRow.modelData.available
                                                  ? importedRow.modelData.families.join(", ")
                                                  : qsTr("Unavailable — the file is gone")
                                        }
                                        AppButton {
                                            storm: true
                                            text: qsTr("Remove")
                                            Accessible.name: qsTr("Remove this imported font")
                                            onClicked: root.fontManager.removeImportedFont(
                                                           importedRow.modelData.fileName)
                                        }
                                    }
                                }
                                Label {
                                    objectName: "fontImportError"
                                    visible: root.fontManager && root.fontManager.lastImportError.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: {
                                        if (!root.fontManager) return ""
                                        switch (root.fontManager.lastImportError) {
                                        case "unsupported_extension":
                                            return qsTr("Only .ttf and .otf font files "
                                                        + "can be loaded.")
                                        case "not_a_font":
                                            return qsTr("That file is not a font.")
                                        case "too_large":
                                            return qsTr("That file is too large to load "
                                                        + "as a font.")
                                        case "empty":
                                            return qsTr("That file is empty.")
                                        case "not_a_file":
                                        case "unreadable":
                                            return qsTr("That file could not be read.")
                                        case "not_a_local_file":
                                            return qsTr("Only a file on this computer can "
                                                        + "be loaded.")
                                        case "already_imported":
                                            return qsTr("That font is already loaded.")
                                        case "store_full":
                                            return qsTr("Remove one of your loaded fonts "
                                                        + "first — Lightning keeps at "
                                                        + "most %1.")
                                                       .arg(root.fontManager.importedFontLimit)
                                        case "rejected_by_font_database":
                                            return qsTr("That font file could not be "
                                                        + "read as a font.")
                                        case "copy_failed":
                                        case "no_store":
                                            return qsTr("Lightning could not save a copy "
                                                        + "of that font.")
                                        default:
                                            return root.fontManager.lastImportError.length > 0
                                                   ? qsTr("That font could not be loaded.")
                                                   : ""
                                        }
                                    }
                                }
                                AppButton {
                                    objectName: "importFontButton"
                                    storm: true
                                    text: qsTr("Load a font file…")
                                    onClicked: fontFileDialog.open()
                                }
                                NativeFileDialog {
                                    id: fontFileDialog
                                    purpose: "font"
                                    currentFolder: app.defaultFileDialogFolder()
                                    title: qsTr("Choose a font file")
                                    fileMode: "open"
                                    nameFilters: [
                                        qsTr("Fonts (*.ttf *.otf)")
                                    ]
                                    onAccepted: root.fontManager.importFontFile(selectedFile)
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Application icon")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Image {
                                        objectName: "appIconPreview"
                                        source: app.appIconSource
                                        sourceSize.width: 48
                                        sourceSize.height: 48
                                        Layout.preferredWidth: 48
                                        Layout.preferredHeight: 48
                                        fillMode: Image.PreserveAspectFit
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name:
                                            qsTr("Current application icon")
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: qsTr("Applies to the window and to task "
                                                   + "switchers that follow the running "
                                                   + "window. The desktop launcher keeps "
                                                   + "the packaged Lightning icon.")
                                    }
                                }
                                Label {
                                    id: customAppIconError
                                    objectName: "customAppIconError"
                                    property string message: ""
                                    visible: message.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: message
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "chooseAppIconButton"
                                        storm: true
                                        text: qsTr("Choose image…")
                                        onClicked: appIconDialog.open()
                                    }
                                    AppButton {
                                        objectName: "resetAppIconButton"
                                        storm: true
                                        visible: app.settings.customAppIconEnabled
                                        text: qsTr("Reset to Lightning default")
                                        onClicked: {
                                            customAppIconError.message = ""
                                            app.resetCustomAppIcon()
                                        }
                                    }
                                }
                                NativeFileDialog {
                                    id: appIconDialog
                                    purpose: "image"
                                    currentFolder: app.defaultFileDialogFolder()
                                    title: qsTr("Choose an application icon image")
                                    fileMode: "open"
                                    nameFilters: [
                                        qsTr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"),
                                        qsTr("All files (*)")
                                    ]
                                    onAccepted: customAppIconError.message =
                                        app.setCustomAppIconFromFile(selectedFile)
                                }
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("appearance")
                        visible: root.section === "appearance"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Language")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    id: languageCombo
                                    objectName: "languageCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    model: app.localization.languages
                                    textRole: "endonym"
                                    valueRole: "code"
                                    enabled: app.localization.translationsAvailable

                                    function syncIndex() {
                                        syncToValue(app.localization.language)
                                    }
                                    Component.onCompleted: syncIndex()
                                    onModelChanged: if (languageResync) languageResync.restart()
                                    Timer {
                                        id: languageResync
                                        interval: 0
                                        onTriggered: languageCombo.syncIndex()
                                    }
                                    Connections {
                                        target: app.localization
                                        function onLanguageChanged() {
                                            languageResync.restart()
                                        }
                                    }
                                    onActivated: app.localization.language = currentValue
                                    Accessible.name: qsTr("Interface language")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    text: {
                                        if (!app.localization.translationsAvailable)
                                            return qsTr("This build was compiled without translations, so the interface stays in English.")
                                        if (app.localization.language === "system")
                                            return qsTr("Following your desktop: %1.")
                                                .arg(app.localization.endonymOf(
                                                    app.localization.effectiveLanguage))
                                        return qsTr("The interface changes immediately. A few strings already on screen update when you next open their panel.")
                                    }
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                }
                            }
                        }

                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("appearance")
                        visible: root.section === "appearance"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    objectName: "accessibilityHeading"
                                    text: qsTr("Accessibility")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "highContrastCheck"
                                    text: qsTr("Increase contrast")
                                    checked: app.settings.highContrast
                                    onToggled: app.settings.highContrast = checked
                                    Accessible.description: qsTr(
                                        "Make secondary text and borders stronger")
                                }
                                SettingsHint {
                                    text: qsTr("Pulls secondary text toward the main text "
                                               + "colour and strengthens borders, on any theme.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "underlineLinksCheck"
                                    text: qsTr("Always underline links")
                                    checked: app.settings.underlineLinks
                                    onToggled: app.settings.underlineLinks = checked
                                    Accessible.description: qsTr(
                                        "Underline mentions and in-app links as well")
                                }
                                SettingsHint {
                                    text: qsTr("Web links in messages are always underlined. "
                                               + "This adds mentions and the link texts in "
                                               + "cards and previews.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Name colour readability")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                SegmentedControl {
                                    storm: true
                                    objectName: "nameColorCorrectionControl"
                                    model: [
                                        { label: qsTr("Strong"), value: 0 },
                                        { label: qsTr("Weak"), value: 1 },
                                        { label: qsTr("Off"), value: 2 },
                                    ]
                                    current: app.settings.nameColorCorrection
                                    onActivated: (value) =>
                                        app.settings.nameColorCorrection = value
                                    Accessible.description: qsTr(
                                        "How strongly sender name colours are adjusted "
                                        + "to stay readable")
                                }
                                SettingsHint {
                                    text: qsTr("Sender names are lightened or darkened until "
                                               + "they read against the background: Strong "
                                               + "aims for 4.5:1, Weak for 3:1, Off keeps the "
                                               + "colour exactly as chosen.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Motion and time")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "reducedMotionCheck"
                                    text: qsTr("Reduce motion")
                                    checked: app.settings.reducedMotion
                                    onToggled: app.settings.reducedMotion = checked
                                    Accessible.description: qsTr(
                                        "Shorten or remove interface animations")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "disableCursorBlinkCheck"
                                    text: qsTr("Disable cursor blinking")
                                    checked: app.settings.disableCursorBlink
                                    onToggled: app.settings.disableCursorBlink = checked
                                    Accessible.description: qsTr(
                                        "Keep text cursors steady in all text inputs. When off, follow the system cursor setting.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "smoothScrollingCheck"
                                    text: qsTr("Smooth scrolling")
                                    checked: app.settings.smoothScrolling
                                    onToggled: app.settings.smoothScrolling = checked
                                    Accessible.description: qsTr(
                                        "Glide the view when you turn the mouse wheel. Turning this off moves the same distance instantly.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Shortens or removes interface "
                                               + "animations — panel slides, "
                                               + "fades and list reorders. "
                                               + "Media you play still plays.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Animated images")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: gifAutoplayCombo
                                    objectName: "gifAutoplayCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Animated images")
                                    Accessible.description: qsTr(
                                        "Whether GIFs, stickers and animated avatars "
                                        + "play on their own, only under the pointer, "
                                        + "or not at all")
                                    model: [
                                        { label: qsTr("Play automatically"), value: 0 },
                                        { label: qsTr("Play on hover"),      value: 1 },
                                        { label: qsTr("Don't play"),         value: 2 },
                                    ]
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifAutoplay)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifAutoplayChanged() {
                                            gifAutoplayCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifAutoplay = currentValue
                                }
                                Label {
                                    objectName: "gifAutoplayHint"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("GIFs, stickers, animated avatars, "
                                               + "banners and chat backgrounds. They "
                                               + "pause while Lightning is not "
                                               + "focused; with Reduce motion on, "
                                               + "they play only under the pointer.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Clock")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    id: clockFormatCombo
                                    objectName: "clockFormatCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: [
                                        { label: qsTr("Follow the system"), value: 0 },
                                        { label: qsTr("12-hour (1:05 PM)"),  value: 1 },
                                        { label: qsTr("24-hour (13:05)"),    value: 2 }
                                    ]
                                    function syncFromSetting() {
                                        syncToValue(app.settings.clockFormat)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onClockFormatChanged() {
                                            clockFormatCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.clockFormat = currentValue
                                    Accessible.name: qsTr("Clock format")
                                }
                            }
                        }

                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("layout")
                        visible: root.section === "layout"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Layout")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("How the window is arranged: messages and threads, "
                                       + "the room list, the Spaces rail and the panels "
                                       + "— per account.")
                            color: AppTheme.settingsText
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsGroupLabel {
                            objectName: "layoutPresetHeading"
                            text: qsTr("Messages and threads")
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing12
                                SettingsChoiceRow {
                                    label: qsTr("Layout preset")
                                    SegmentedControl {
                                        storm: true
                                        objectName: "layoutPresetControl"
                                        model: [
                                            { label: qsTr("Lightning"), value: 0 },
                                            { label: qsTr("Nheko compact"), value: 1 },
                                        ]
                                        current: app.settings.layoutPreset
                                        onActivated: (value) =>
                                            app.settings.layoutPreset = value
                                        Accessible.description: layoutPresetHint.text
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            id: layoutPresetHint
                                            objectName: "layoutPresetHint"
                                            textFormat: Text.PlainText
                                            Layout.fillWidth: true
                                            Layout.leftMargin: AppTheme.spacing4
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.settingsHintSize
                                            text: app.settings.layoutPreset === 1
                                                  ? (app.settings.layoutPresetCustomized
                                                     ? qsTr("Nheko compact, customized. Your "
                                                            + "Lightning layout is kept and "
                                                            + "comes back when you switch.")
                                                     : qsTr("Flat rows with a small avatar "
                                                            + "above each sender's messages, "
                                                            + "Nheko's close row spacing, and threads in "
                                                            + "the main view. Your fonts, text "
                                                            + "size and colours are unchanged."))
                                                  : qsTr("Lightning's own layout. Nheko "
                                                         + "compact is a denser layout you can "
                                                         + "switch back from at any time. A "
                                                         + "preset sets the three choices just "
                                                         + "below it; a choice it set says so.")
                                        }
                                        AppButton {
                                            objectName: "layoutPresetResetButton"
                                            storm: true
                                            size: "sm"
                                            visible: app.settings.layoutPreset === 1
                                                     && app.settings.layoutPresetCustomized
                                            text: qsTr("Reset preset")
                                            Accessible.description: qsTr(
                                                "Returns message layout, spacing and thread "
                                                + "placement to the Nheko compact values. "
                                                + "Nothing else changes.")
                                            onClicked: app.settings.resetLayoutPresetOverrides()
                                        }
                                    }
                                }
                                LayoutPresetPreview {
                                    Layout.fillWidth: true
                                    objectName: "layoutPresetPreview"
                                    messageLayout: app.settings.messageLayout
                                    messageSpacing: app.settings.messageSpacing
                                    threadPlacement: app.settings.threadPlacement
                                    roomListWidth: app.settings.roomListWidth
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Preview uses your current font and colours. "
                                               + "The preset changes message layout, spacing "
                                               + "and where threads open; reset restores only "
                                               + "those three choices.")
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                }
                                SettingsChoiceRow {
                                    label: qsTr("Message layout")
                                    SegmentedControl {
                                        storm: true
                                        objectName: "messageLayoutControl"
                                        model: [
                                            { label: qsTr("Modern"), value: 0 },
                                            { label: qsTr("Bubbles"), value: 1 },
                                            { label: qsTr("Compact"), value: 2 },
                                            { label: qsTr("Small avatars"), value: 3 },
                                        ]
                                        current: app.settings.messageLayout
                                        onActivated: (value) =>
                                            app.settings.messageLayout = value
                                    }
                                    PresetOriginLabel {
                                        objectName: "messageLayoutPresetOrigin"
                                        key: "messageLayout"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: AppTheme.spacing4
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: qsTr("Bubbles applies to direct messages; rooms keep "
                                                   + "the Modern rows. Compact tightens every timeline. "
                                                   + "Small avatars puts a small avatar beside each "
                                                   + "sender's name, above their messages.")
                                    }
                                }
                                SettingsChoiceRow {
                                    label: qsTr("Message spacing")
                                    SegmentedControl {
                                        storm: true
                                        objectName: "messageSpacingControl"
                                        model: [
                                            { label: qsTr("Compact"), value: 0 },
                                            { label: qsTr("Cozy"), value: 1 },
                                            { label: qsTr("Roomy"), value: 2 },
                                        ]
                                        current: app.settings.messageSpacing
                                        onActivated: (value) =>
                                            app.settings.messageSpacing = value
                                        Accessible.description: qsTr(
                                            "Space between messages and between senders")
                                    }
                                    PresetOriginLabel {
                                        objectName: "messageSpacingPresetOrigin"
                                        key: "messageSpacing"
                                    }
                                }
                                SettingsChoiceRow {
                                    label: qsTr("Threads open in")
                                    SegmentedControl {
                                        storm: true
                                        objectName: "threadPlacementControl"
                                        model: [
                                            { label: qsTr("Main view"), value: 1 },
                                            { label: qsTr("Side panel"), value: 0 },
                                        ]
                                        current: app.settings.threadPlacement
                                        onActivated: (value) =>
                                            app.settings.threadPlacement = value
                                    }
                                    PresetOriginLabel {
                                        objectName: "threadPlacementPresetOrigin"
                                        key: "threadPlacement"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: AppTheme.spacing4
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                        text: qsTr("Where an opened thread is shown: in place of "
                                                   + "the room, with Back to return, or beside it. "
                                                   + "Either way it loads the thread's whole "
                                                   + "history and keeps its own draft.")
                                    }
                                }
                                SettingsChoiceRow {
                                    label: qsTr("Thread style")
                                    SegmentedControl {
                                        storm: true
                                        objectName: "threadStyleControl"
                                        model: [
                                            { label: qsTr("Thread panel"), value: 0 },
                                            { label: qsTr("Inline"), value: 1 },
                                        ]
                                        current: app.settings.threadStyle
                                        onActivated: (value) =>
                                            app.settings.threadStyle = value
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: AppTheme.spacing4
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                        text: qsTr("Thread panel keeps thread replies out of the "
                                                   + "room, like Element. Inline keeps them in the "
                                                   + "room in time order with a thread tag, like "
                                                   + "Nheko; click a tag to open that thread.")
                                    }
                                }
                            }
                        }

                        SettingsGroupLabel {
                            objectName: "roomListLayoutHeading"
                            text: qsTr("Room list and panels")
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing12
                                SettingsChoiceRow {
                                    label: qsTr("Conversation list")
                                    RowLayout {
                                        objectName: "roomNavigationLayoutCards"
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing12

                                        NavigationLayoutCard {
                                            objectName: "navLayoutClassicCard"
                                            Layout.fillWidth: true
                                            compact: true
                                            variant: "classic"
                                            title: qsTr("Classic")
                                            subtitle: qsTr("One list, most recent first, "
                                                           + "with message previews.")
                                            current: app.settings.roomNavigationLayout === 0
                                            onClicked: app.settings.roomNavigationLayout = 0
                                        }
                                        NavigationLayoutCard {
                                            objectName: "navLayoutChannelsCard"
                                            Layout.fillWidth: true
                                            compact: true
                                            variant: "channels"
                                            title: qsTr("Channels")
                                            subtitle: qsTr("Every space as a collapsible "
                                                           + "folder of its rooms.")
                                            current: app.settings.roomNavigationLayout === 1
                                            onClicked: app.settings.roomNavigationLayout = 1
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: AppTheme.spacing4
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: qsTr("Channels lists every space you are in as "
                                                   + "a folder, with the rooms it contains "
                                                   + "underneath. Rooms in no space, and your "
                                                   + "direct messages, stay together in Rooms.")
                                    }
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showSpacesRailCheck"
                                    text: qsTr("Show the Spaces rail (Ctrl+Shift+B)")
                                    checked: app.settings.spacesRailVisible
                                    onToggled: app.settings.spacesRailVisible = checked
                                    Accessible.description: qsTr(
                                        "Show the narrow strip of Spaces down the far edge")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showRoomListCheck"
                                    text: qsTr("Show the room list (Ctrl+B)")
                                    checked: app.settings.roomListVisible
                                    onToggled: app.settings.roomListVisible = checked
                                    Accessible.description: qsTr(
                                        "Show the column of rooms and people")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Drag the line between two panels to "
                                               + "resize them. Widths are remembered.")
                                }

                                Label {
                                    objectName: "spacesRailDepthLabel"
                                    Layout.topMargin: AppTheme.spacing4
                                    Layout.leftMargin: AppTheme.spacing4
                                    text: qsTr("Spaces rail depth")
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.settingsHintSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SegmentedControl {
                                    storm: true
                                    objectName: "spacesRailDepthControl"
                                    Layout.leftMargin: AppTheme.spacing4 - 12
                                    enabled: app.settings.spacesRailVisible
                                    opacity: enabled ? 1.0 : 0.5
                                    model: [
                                        { label: qsTr("Regions"), value: 0 },
                                        { label: qsTr("Classic"), value: 1 },
                                    ]
                                    current: app.settings.spacesRailDepthStyle
                                    onActivated: (value) =>
                                        app.settings.spacesRailDepthStyle = value
                                    Accessible.description: qsTr(
                                        "Choose how the Spaces rail shows which "
                                        + "Space contains which")
                                    SettingsNeedsBadge {
                                        needs: qsTr("Show the Spaces rail")
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Regions tint the rail behind a Space "
                                               + "and everything inside it. Classic is "
                                               + "the flat rail from earlier versions, "
                                               + "where a nested Space steps in instead. "
                                               + "Both show the same Spaces.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Room list")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showBridgeBadgesCheck"
                                    text: qsTr("Show bridge badges in the room list")
                                    checked: app.settings.showBridgeBadges
                                    onToggled: app.settings.showBridgeBadges = checked
                                    Accessible.description: qsTr(
                                        "Show the network name, such as WhatsApp, beside bridged rooms")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "compactRoomListCheck"
                                    text: qsTr("Compact room list (hide message previews)")
                                    checked: app.settings.compactRoomList
                                    onToggled: app.settings.compactRoomList = checked
                                    Accessible.description: qsTr(
                                        "One line per room, without the last message")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: app.settings.roomListWidth
                                          < app.settings.roomListExpandedMinWidth
                                          ? qsTr("Conversation list: compact")
                                          : qsTr("Conversation list width: %1 px")
                                              .arg(app.settings.roomListWidth)
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                SettingsSlider {
                                    objectName: "roomListWidthSlider"
                                    Layout.fillWidth: true
                                    from: app.settings.roomListExpandedMinWidth
                                    to: app.settings.roomListMaxWidth
                                    stepSize: 10
                                    value: Math.max(app.settings.roomListExpandedMinWidth,
                                                    app.settings.roomListWidth)
                                    onMoved: {
                                        app.settings.roomListWidth = Math.round(value)
                                        app.settings.setRoomListExpandedWidth(Math.round(value))
                                    }
                                    Accessible.name: qsTr("Conversation list width")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "keepRoomListOrderStillCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Keep the room list still while I use it")
                                    checked: app.settings.keepRoomListOrderStill
                                    onToggled: app.settings.keepRoomListOrderStill = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("New messages still update a room's preview "
                                               + "and badge at once, but it only moves up "
                                               + "when you open another room, switch "
                                               + "Space, tab or search, or come back to "
                                               + "the window.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Banners and side panel")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spaceBannersVisibleCheck"
                                    text: qsTr("Show Space banners")
                                    checked: app.settings.spaceBannersVisible
                                    onToggled: app.settings.spaceBannersVisible = checked
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spaceBannerExpandedCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.spaceBannersVisible
                                    text: qsTr("Show the whole banner instead of a strip")
                                    checked: app.settings.spaceBannerExpanded
                                    onToggled: app.settings.spaceBannerExpanded = checked
                                    SettingsNeedsBadge { needs: qsTr("Show Space banners") }
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Side panel width: %1 px")
                                        .arg(app.settings.sidePanelWidth)
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                SettingsSlider {
                                    objectName: "sidePanelWidthSlider"
                                    Layout.fillWidth: true
                                    from: app.settings.sidePanelMinWidth
                                    to: app.settings.sidePanelMaxWidth
                                    stepSize: 10
                                    value: app.settings.sidePanelWidth
                                    onMoved: app.settings.sidePanelWidth = Math.round(value)
                                    Accessible.name: qsTr("Side panel width")
                                }
                            }
                        }

                        SettingsGroupLabel {
                            visible: app.trayAvailable
                            text: Qt.platform.os === "osx" ? qsTr("Menu bar") : qsTr("System tray")
                        }
                        SettingsCard {
                            visible: app.trayAvailable
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "closeToTrayCheck"
                                    text: Qt.platform.os === "osx" ? qsTr("Show Lightning in the menu bar")
                                                       : qsTr("Keep running in the tray when the window is closed")
                                    readonly property bool iconForced:
                                        Qt.platform.os === "osx"
                                        && app.settings.notificationsEnabled
                                    enabled: !iconForced
                                    checked: iconForced || app.settings.closeToTray
                                    onToggled: app.settings.closeToTray = checked
                                    Accessible.description: iconForced
                                        ? qsTr("Notifications need the menu bar icon")
                                        : ""
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "startInTrayCheck"
                                    text: Qt.platform.os === "osx" ? qsTr("Start hidden in the menu bar")
                                                       : qsTr("Start in the tray")
                                    enabled: app.settings.closeToTray
                                    checked: app.settings.startInTray
                                    onToggled: app.settings.startInTray = checked
                                    SettingsNeedsBadge { needs: qsTr("Keep running in the tray") }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: Qt.platform.os === "osx"
                                          ? (app.settings.notificationsEnabled
                                             ? qsTr("Notifications need the menu bar icon, so it stays "
                                                    + "while desktop notifications are on. Closing the "
                                                    + "window always hides it, and Lightning keeps "
                                                    + "running. Click the icon to bring the window "
                                                    + "back. Cmd+Q quits.")
                                             : qsTr("Closing the window always hides it, and Lightning keeps "
                                                    + "running. Click the menu bar icon to bring the "
                                                    + "window back. Cmd+Q quits."))
                                          : qsTr("Click the tray icon to bring the window "
                                                 + "back. Ctrl+Q quits.")
                                }
                            }
                        }

                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("chat")
                        visible: root.section === "chat"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Chat")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("What a room shows, and how you write in it "
                                       + "— per account.")
                            color: AppTheme.settingsText
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Timeline")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showRoomActivityCheck"
                                    text: qsTr("Show room activity")
                                    checked: app.settings.showRoomActivity
                                    onToggled: app.settings.showRoomActivity = checked
                                    Accessible.description: qsTr(
                                        "Show membership, profile, and room setting updates in timelines")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Hide routine joins, leaves, profile changes, "
                                               + "and room setting updates. Messages and "
                                               + "decryption warnings remain visible.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showMembershipEventsCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.showRoomActivity
                                    text: qsTr("Joins, leaves and invites")
                                    checked: app.settings.showMembershipEvents
                                    onToggled: app.settings.showMembershipEvents = checked
                                    Accessible.description: qsTr(
                                        "Show membership changes in timelines")
                                    SettingsNeedsBadge { needs: qsTr("Show room activity") }
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showProfileChangeEventsCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.showRoomActivity
                                    text: qsTr("Display name and avatar changes")
                                    checked: app.settings.showProfileChangeEvents
                                    onToggled: app.settings.showProfileChangeEvents = checked
                                    Accessible.description: qsTr(
                                        "Show profile changes in timelines")
                                    SettingsNeedsBadge { needs: qsTr("Show room activity") }
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "collapseEmbedsCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Collapse media and link embeds")
                                    checked: app.settings.collapseEmbeds
                                    onToggled: app.settings.collapseEmbeds = checked
                                    Accessible.description: qsTr(
                                        "Show attachments and link previews as one line "
                                        + "with an arrow that expands them")
                                }
                                Label {
                                    objectName: "collapseEmbedsHint"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Pictures, GIFs, stickers, video, audio, "
                                               + "voice messages, files and loaded link "
                                               + "previews each become one line naming what "
                                               + "they are. Click the arrow, or focus the "
                                               + "line and press Enter, to show one. Reply "
                                               + "quotes, thread cards, polls and shared "
                                               + "places are not affected. A collapsed "
                                               + "attachment is not downloaded until you "
                                               + "expand it.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Avatars in messages")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: timelineAvatarsCombo
                                    objectName: "timelineAvatarsCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: root.timelineVisibilityChoices
                                    function syncFromSetting() {
                                        syncToValue(app.settings.timelineAvatars)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onTimelineAvatarsChanged() {
                                            timelineAvatarsCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.timelineAvatars = currentValue
                                    Accessible.description: qsTr("Show sender pictures beside messages, only on the message under the pointer, or not at all")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Reactions")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: timelineReactionsCombo
                                    objectName: "timelineReactionsCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: root.timelineVisibilityChoices
                                    function syncFromSetting() {
                                        syncToValue(app.settings.timelineReactions)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onTimelineReactionsChanged() {
                                            timelineReactionsCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.timelineReactions = currentValue
                                    Accessible.description: qsTr("Show reactions under messages, only on the message under the pointer, or not at all")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Read receipts")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: timelineReadReceiptsCombo
                                    objectName: "timelineReadReceiptsCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: root.timelineVisibilityChoices
                                    function syncFromSetting() {
                                        syncToValue(app.settings.timelineReadReceipts)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onTimelineReadReceiptsChanged() {
                                            timelineReadReceiptsCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.timelineReadReceipts = currentValue
                                    Accessible.description: qsTr("Show who has read up to a message, only on the message under the pointer, or not at all")
                                }
                                Label {
                                    objectName: "timelineVisibilityHint"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Applies to rooms, threads and jump-to-message "
                                               + "views. \"On hover\" also shows them on the "
                                               + "message selected with the keyboard. Hiding "
                                               + "read receipts here does not stop sending "
                                               + "your own; that is under Privacy.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "largeEmojiCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Show emoji-only messages large")
                                    checked: app.settings.largeEmoji
                                    onToggled: app.settings.largeEmoji = checked
                                    Accessible.description: qsTr(
                                        "Draw a message of one to three emoji at a larger size")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "jumpToNewestOnSendCheck"
                                    text: qsTr("Go to the newest message when I send")
                                    checked: app.settings.jumpToNewestOnSend
                                    onToggled: app.settings.jumpToNewestOnSend = checked
                                    Accessible.description: qsTr(
                                        "When off, sending from up in history keeps your place. "
                                        + "A reply always keeps your place.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Mouse-wheel speed")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: wheelSpeedCombo
                                    objectName: "timelineWheelSpeedCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: [
                                        { label: qsTr("Standard"),  value: 0 },
                                        { label: qsTr("Fast"),      value: 1 },
                                        { label: qsTr("Very fast"), value: 2 }
                                    ]
                                    function syncFromSetting() {
                                        syncToValue(app.settings.timelineWheelSpeed)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onTimelineWheelSpeedChanged() {
                                            wheelSpeedCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.timelineWheelSpeed = currentValue
                                    Accessible.description: qsTr(
                                        "How far one physical mouse-wheel notch scrolls the timeline")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("How far one physical mouse-wheel notch moves "
                                               + "the timeline. Touchpad and precision scrolling "
                                               + "stay fine-grained regardless of this setting.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("When returning to a conversation")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: returnModeCombo
                                    objectName: "conversationReturnCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    model: [
                                        { label: qsTr("Previous position"), value: 0 },
                                        { label: qsTr("Latest messages"), value: 1 },
                                        { label: qsTr("Selected message when available"),
                                          value: 2 }
                                    ]
                                    function syncFromSetting() {
                                        syncToValue(app.settings.conversationReturnMode)
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onConversationReturnModeChanged() {
                                            returnModeCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.conversationReturnMode = currentValue
                                    Accessible.description: qsTr(
                                        "Where a room opens when you come back to it")
                                }
                                Label {
                                    objectName: "conversationReturnHint"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Previous position puts you back where you were "
                                               + "reading, or at the newest messages if you "
                                               + "were following them. Selected message opens "
                                               + "at the message you last selected with the "
                                               + "keyboard, and otherwise at your previous "
                                               + "position. Opening a link or a notification "
                                               + "always goes to its message.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showSelectionOnReturnCheck"
                                    text: qsTr("Show the message selection outline when returning")
                                    checked: app.settings.showSelectionOnReturn
                                    onToggled: app.settings.showSelectionOnReturn = checked
                                    Accessible.description: qsTr(
                                        "Outline the message you last selected without "
                                        + "moving the keyboard focus")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Message box")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "enterInsertsNewlineCheck"
                                    text: qsTr("Enter starts a new line")
                                    checked: app.settings.enterInsertsNewline
                                    onToggled: app.settings.enterInsertsNewline = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: app.settings.enterInsertsNewline
                                        ? qsTr("Enter starts a new line; Ctrl+Enter sends.")
                                        : qsTr("Enter sends; Shift+Enter starts a new line.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spellCheckEnabledCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Check spelling as you type")
                                    checked: app.settings.spellCheckEnabled
                                    enabled: !app.spell || app.spell.available
                                    onToggled: app.settings.spellCheckEnabled = checked
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    spacing: AppTheme.spacing8
                                    enabled: app.settings.spellCheckEnabled
                                    Label {
                                        text: qsTr("Spelling language")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                    }
                                    AppComboBox {
                                        id: spellLanguageCombo
                                        objectName: "spellLanguageCombo"
                                        storm: true
                                        Layout.fillWidth: true
                                        Layout.maximumWidth: 320
                                        textRole: "label"
                                        valueRole: "tag"
                                        model: app.spell ? app.spell.languageOptions : []
                                        restoreAfterModelReset: false
                                        Accessible.name: qsTr("Spelling language")
                                        Component.onCompleted:
                                            syncToValue(app.settings.spellCheckLanguage)
                                        onModelChanged:
                                            syncToValue(app.settings.spellCheckLanguage)
                                        Connections {
                                            target: app.settings
                                            function onSpellCheckLanguageChanged() {
                                                spellLanguageCombo.syncToValue(
                                                    app.settings.spellCheckLanguage)
                                            }
                                        }
                                        onActivated: app.settings.spellCheckLanguage =
                                                         currentValue === undefined
                                                         ? "" : currentValue
                                    }
                                }
                                Label {
                                    objectName: "spellCheckDetail"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: app.spell && app.spell.available
                                           ? AppTheme.stormTextMuted : AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: {
                                        if (!app.spell)
                                            return ""
                                        if (app.spell.available) {
                                            var name = app.spell.backendName
                                            var engine = name === "windows"
                                                ? qsTr("Windows spell checker")
                                                : name === "macos"
                                                  ? qsTr("macOS spell checker")
                                                  : name === "hunspell"
                                                    ? qsTr("Hunspell (system and bundled dictionaries)")
                                                    : name === "enchant"
                                                      ? qsTr("Enchant (system dictionaries)")
                                                      : qsTr("Enchant and Hunspell")
                                            return engine + " · " + qsTr("Dictionary: %1")
                                                .arg(app.spell.languageLabel.length > 0
                                                     ? app.spell.languageLabel
                                                     : app.spell.language)
                                        }
                                        var reason = app.spell.unavailableReason
                                        var os = Qt.platform.os
                                        if (reason === "no-platform")
                                            return qsTr("Spell checking is not available in this build for this operating system.")
                                        if (reason === "no-library") {
                                            if (os === "windows")
                                                return qsTr("Spell checking is unavailable: the Windows spell-checking service could not be started.")
                                            return qsTr("Spell checking is unavailable because neither the Hunspell nor the enchant-2 spelling library could be loaded.")
                                        }
                                        var wanted = app.settings.spellCheckLanguage.length > 0
                                            ? app.settings.spellCheckLanguage : qsTr("your system language")
                                        var how = os === "windows"
                                            ? qsTr("Add the language under Windows Settings › Time & language › Language to install its spelling.")
                                            : os === "osx"
                                              ? qsTr("Add the language under System Settings › Keyboard › Text Input.")
                                              : qsTr("Install your distribution's spelling dictionary for that language (usually a hunspell package). Flatpak and Snap see only the dictionaries they bundle.")
                                        return qsTr("Spell checking is unavailable because no system dictionary is installed for %1.").arg(wanted) + " " + how
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    spacing: AppTheme.spacing8
                                    enabled: app.settings.spellCheckEnabled
                                    visible: !!app.spell
                                             && app.spell.availableLanguages.length > 1
                                    Label {
                                        text: qsTr("Also check")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                    }
                                    AppComboBox {
                                        id: spellExtraCombo
                                        objectName: "spellExtraLanguageCombo"
                                        storm: true
                                        Layout.fillWidth: true
                                        Layout.maximumWidth: 320
                                        textRole: "label"
                                        valueRole: "tag"
                                        displayText: qsTr("Add a language…")
                                        restoreAfterModelReset: false
                                        Accessible.name: qsTr("Add a spelling language")
                                        model: {
                                            var out = []
                                            if (!app.spell)
                                                return out
                                            var chosen = app.settings.spellCheckExtraLanguages
                                            var options = app.spell.languageOptions
                                            for (var i = 0; i < options.length; ++i) {
                                                var tag = options[i].tag
                                                if (tag === ""
                                                        || tag === app.settings.spellCheckLanguage
                                                        || chosen.indexOf(tag) >= 0)
                                                    continue
                                                out.push(options[i])
                                            }
                                            return out
                                        }
                                        onActivated: (index) => {
                                            var tag = valueAt(index)
                                            if (tag === undefined || tag === "")
                                                return
                                            var list = app.settings.spellCheckExtraLanguages.slice()
                                            list.push(tag)
                                            app.settings.spellCheckExtraLanguages = list
                                        }
                                    }
                                }
                                Flow {
                                    objectName: "spellExtraLanguageChips"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    spacing: AppTheme.spacing6
                                    visible: app.settings.spellCheckExtraLanguages.length > 0
                                    Repeater {
                                        model: app.settings.spellCheckExtraLanguages
                                        delegate: Rectangle {
                                            id: extraChip
                                            objectName: "spellExtraLanguageChip"
                                            required property string modelData
                                            radius: height / 2
                                            color: AppTheme.stormInset
                                            border.width: 1
                                            border.color: AppTheme.stormFieldBorder
                                            implicitHeight: 28
                                            implicitWidth: chipRow.implicitWidth
                                                           + AppTheme.spacing8 + AppTheme.spacing4
                                            RowLayout {
                                                id: chipRow
                                                anchors.verticalCenter: parent.verticalCenter
                                                x: AppTheme.spacing8
                                                spacing: AppTheme.spacing4
                                                Label {
                                                    textFormat: Text.PlainText
                                                    text: root.spellLanguageLabel(extraChip.modelData)
                                                    color: AppTheme.stormText
                                                    font.pixelSize: AppTheme.settingsHintSize
                                                }
                                                IconButton {
                                                    objectName: "spellExtraLanguageRemove"
                                                    implicitWidth: 20
                                                    implicitHeight: 20
                                                    radius: AppTheme.radiusControl
                                                    iconName: "close"
                                                    iconSize: 12
                                                    Accessible.name: qsTr("Stop checking %1")
                                                        .arg(root.spellLanguageLabel(extraChip.modelData))
                                                    onClicked: {
                                                        var list = app.settings.spellCheckExtraLanguages.slice()
                                                        var at = list.indexOf(extraChip.modelData)
                                                        if (at >= 0)
                                                            list.splice(at, 1)
                                                        app.settings.spellCheckExtraLanguages = list
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Label {
                                    objectName: "spellMissingLanguages"
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsHintSize
                                    visible: !!app.spell && app.settings.spellCheckEnabled
                                             && app.spell.missingLanguages.length > 0
                                    text: {
                                        if (!app.spell)
                                            return ""
                                        var names = []
                                        var missing = app.spell.missingLanguages
                                        for (var i = 0; i < missing.length; ++i)
                                            names.push(root.spellLanguageLabel(missing[i]))
                                        return qsTr("No dictionary is installed for %1, so it is not checked.")
                                            .arg(names.join(", "))
                                    }
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "autocorrectCheck"
                                    Layout.topMargin: AppTheme.spacing4
                                    text: qsTr("Correct obvious typos as you type")
                                    checked: !!app.spell && app.spell.autocorrectEnabled
                                    enabled: app.settings.spellCheckEnabled
                                             && (!app.spell || app.spell.available)
                                    onToggled: app.settings.autocorrectMode = checked ? 1 : 2
                                }
                                Label {
                                    objectName: "autocorrectDetail"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: Qt.platform.os === "osx"
                                        ? qsTr("Only clear typos are changed, after you finish the word, and never in code, links, mentions or commands. Press Backspace or ⌘Z right after a correction to undo it.")
                                        : qsTr("Only clear typos are changed, after you finish the word, and never in code, links, mentions or commands. Press Backspace or Ctrl+Z right after a correction to undo it.")
                                }
                                Label {
                                    objectName: "personalDictionaryHeading"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Personal dictionary")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                Label {
                                    objectName: "personalDictionaryEmpty"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    visible: !app.spell || app.spell.personalWords.length === 0
                                    text: qsTr("Words you add with “Add to dictionary” are listed here and accepted in every language.")
                                }
                                ListView {
                                    id: personalWordsList
                                    objectName: "personalDictionaryList"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    Layout.maximumWidth: 420
                                    Layout.preferredHeight: Math.min(contentHeight, 180)
                                    visible: !!app.spell && app.spell.personalWords.length > 0
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    model: app.spell ? app.spell.personalWords : []
                                    ScrollBar.vertical: AppScrollBar { thin: true }
                                    SmoothWheelArea {
                                        enabled: personalWordsList.contentHeight
                                                 > personalWordsList.height + 0.5
                                    }
                                    delegate: RowLayout {
                                        id: wordRow
                                        required property string modelData
                                        width: ListView.view ? ListView.view.width : 0
                                        height: 28
                                        spacing: AppTheme.spacing4
                                        Label {
                                            Layout.fillWidth: true
                                            textFormat: Text.PlainText
                                            text: wordRow.modelData
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            elide: Label.ElideRight
                                        }
                                        IconButton {
                                            objectName: "personalDictionaryRemove"
                                            implicitWidth: 24
                                            implicitHeight: 24
                                            radius: AppTheme.radiusControl
                                            iconName: "close"
                                            iconSize: 14
                                            Accessible.name: qsTr("Remove “%1” from the dictionary")
                                                .arg(wordRow.modelData)
                                            onClicked: app.spell.removeFromDictionary(wordRow.modelData)
                                        }
                                    }
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "sendTextAsCaptionCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Send text with an attachment as its caption")
                                    checked: app.settings.sendTextAsCaption
                                    onToggled: app.settings.sendTextAsCaption = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("One event instead of two. Clients "
                                               + "that do not understand captions "
                                               + "show the attachment without the "
                                               + "text.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "emojiShortcodeAutoConvertCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Convert :shortcode: to emoji")
                                    checked: app.settings.emojiShortcodeAutoConvert
                                    onToggled: app.settings.emojiShortcodeAutoConvert = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Typing a shortcode like "
                                               + "\":thumbsup:\" turns it into "
                                               + "the emoji as soon as it is "
                                               + "complete. Custom pack "
                                               + "shortcodes are unaffected.")
                                }

                                Label {
                                    objectName: "composerButtonsHeading"
                                    Layout.topMargin: AppTheme.spacing16
                                    Layout.leftMargin: AppTheme.spacing4
                                    text: qsTr("Message box buttons")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Turn off what you do not use. "
                                               + "Attach stays: in a narrow "
                                               + "window it is where the other "
                                               + "actions move to.")
                                }
                                Repeater {
                                    model: [
                                        { key: "formatting",
                                          label: qsTr("Formatting") },
                                        { key: "emoji", label: qsTr("Emoji") },
                                        { key: "media",
                                          label: qsTr("GIFs and stickers") },
                                        { key: "voice",
                                          label: qsTr("Voice message") },
                                        { key: "sendOptions",
                                          label: qsTr("Send options") },
                                    ]
                                    SettingsCheckBox {
                                        required property var modelData
                                        palette.windowText: AppTheme.stormText
                                        objectName: "composerButtonCheck_"
                                                    + modelData.key
                                        Layout.leftMargin: AppTheme.spacing4
                                        text: modelData.label
                                        // Guarded because a binding that throws
                                        // sticks at its last value, and this one
                                        // decides whether a control exists (see
                                        // Avatar.qml's `bridge`).
                                        checked: {
                                            if (typeof app === "undefined"
                                                    || !app || !app.settings)
                                                return true
                                            return app.settings
                                                .hiddenComposerButtons
                                                .indexOf(modelData.key) < 0
                                        }
                                        onToggled: {
                                            if (app && app.settings)
                                                app.settings.setComposerButtonShown(
                                                    modelData.key, checked)
                                        }
                                    }
                                }
                            }
                        }

                        SettingsCard {
                            StickerPacksSettings {
                                objectName: "stickerPacksSettings"
                                width: parent.width
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("shortcuts")
                        visible: root.section === "shortcuts"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            objectName: "shortcutsHeading"
                            text: qsTr("Keyboard shortcuts")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            text: qsTr("Per account, like your theme. Press Change "
                                       + "and then the combination you want.")
                            color: AppTheme.settingsText
                            font.pixelSize: AppTheme.settingsHintSize
                        }

                        SettingsCard {
                            visible: Qt.platform.os === "linux"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "globalShortcutsCheck"
                                    text: qsTr("Use call keys from any window")
                                    checked: app.settings.globalShortcutsEnabled
                                    onToggled: app.settings.globalShortcutsEnabled = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Mute, deafen and leave the call while another "
                                               + "window has the focus. Your desktop asks "
                                               + "before it allows this.")
                                }
                                Label {
                                    objectName: "globalShortcutsStatus"
                                    visible: app.settings.globalShortcutsEnabled
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: app.globalShortcuts.statusText
                                }
                                AppButton {
                                    objectName: "globalShortcutsConfigure"
                                    storm: true
                                    visible: app.globalShortcuts.canConfigure
                                    text: qsTr("Change the keys in your desktop's settings")
                                    onClicked: app.globalShortcuts.configure()
                                }
                            }
                        }

                        Loader {
                            Layout.fillWidth: true
                            active: app.shortcuts.conflictCount > 0
                            visible: active
                            sourceComponent: Rectangle {
                                objectName: "shortcutConflictBanner"
                                implicitHeight: conflictBannerLabel.implicitHeight
                                                + AppTheme.spacing12 * 2
                                radius: AppTheme.radiusMd
                                color: AppTheme.stormDangerSoft
                                border.width: 1
                                border.color: AppTheme.stormDangerBorder
                                Label {
                                    id: conflictBannerLabel
                                    anchors.fill: parent
                                    anchors.margins: AppTheme.spacing12
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Some shortcuts share a key. Qt fires "
                                               + "neither of two shortcuts on the same "
                                               + "combination, so both actions are "
                                               + "currently doing nothing. Change one "
                                               + "of each pair, or reset everything.")
                                }
                            }
                        }

                        Repeater {
                            model: {
                                app.shortcuts.bindingRevision
                                return root.shortcutRowsBuilt
                                       ? app.shortcuts.categories() : []
                            }
                            delegate: SettingsCard {
                                id: shortcutCategoryCard
                                required property string modelData
                                ColumnLayout {
                                    width: parent.width
                                    spacing: AppTheme.spacing8
                                    Label {
                                        text: shortcutCategoryCard.modelData
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Repeater {
                                        model: app.shortcuts
                                        delegate: Loader {
                                            id: shortcutRowLoader
                                            required property var model
                                            active: model.shortcutCategory
                                                    === shortcutCategoryCard.modelData
                                            visible: active
                                            asynchronous: root.section !== "shortcuts"
                                            Layout.fillWidth: true
                                            sourceComponent: ShortcutRow {
                                                // ShortcutRegistry::roleNames prefixes
                                                // its roles so they can be assigned to
                                                // this component's like-named properties.
                                                actionId: shortcutRowLoader.model.shortcutId
                                                description: shortcutRowLoader.model.shortcutDescription
                                                currentSequence: shortcutRowLoader.model.shortcutCurrent
                                                defaultSequence: shortcutRowLoader.model.shortcutDefault
                                                isDefault: shortcutRowLoader.model.shortcutIsDefault
                                                conflictsWith: shortcutRowLoader.model.shortcutConflict
                                                shadowNote: shortcutRowLoader.model.shortcutShadow
                                                compact: contentColumn.width < 560
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("A global shortcut can be one key on "
                                               + "its own, such as a letter, F5 or a "
                                               + "numpad key; the numpad counts "
                                               + "separately from the main keyboard. "
                                               + "A lone letter, digit or symbol works "
                                               + "only while you are not typing in a "
                                               + "text field, so typing always wins.\n\n"
                                               + "Settings page actions also accept Tab "
                                               + "and Shift+Tab; those bindings replace "
                                               + "normal focus traversal only in Settings.\n\n"
                                               + "Message formatting keys apply only "
                                               + "while the message box has focus. "
                                               + "Everywhere else, the same key still "
                                               + "does its usual job.\n\n"
                                               + "Escape, and the single letters the "
                                               + "message menu uses while it is open, "
                                               + "are reserved and cannot be assigned.")
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    objectName: "shortcutResetAllButton"
                                    text: qsTr("Reset all shortcuts")
                                    enabled: app.shortcuts.anyCustomised
                                    onClicked: app.shortcuts.resetAll()
                                }
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("privacy")
                        visible: root.section === "privacy"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Privacy & security")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Network privacy, encryption health, and recovery.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        Label {
                            text: qsTr("Device trust")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                SettingsCheckBox {
                                    objectName: "strictDeviceTrustCheck"
                                    Layout.fillWidth: true
                                    palette.windowText: AppTheme.stormText
                                    contentItem: Label {
                                        objectName: "strictDeviceTrustLabel"
                                        leftPadding: parent.indicator.width
                                                     + parent.spacing
                                        wrapMode: Text.WordWrap
                                        verticalAlignment: Text.AlignVCenter
                                        color: parent.enabled
                                               ? AppTheme.stormText
                                               : AppTheme.stormTextMuted
                                        font: parent.font
                                        text: parent.text
                                    }
                                    Rectangle {
                                        objectName: "checkFocusRing"
                                        anchors.fill: parent
                                        radius: 4
                                        color: "transparent"
                                        border.color: AppTheme.stormText
                                        border.width: 2
                                        visible: parent.visualFocus
                                    }
                                    text: qsTr("Only exchange messages with "
                                               + "verified devices")
                                    checked: app.settings.strictDeviceTrust
                                    onToggled:
                                        app.settings.strictDeviceTrust = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Room keys are not shared with "
                                               + "devices their owner has not "
                                               + "cross-signed, and messages "
                                               + "from such devices are not "
                                               + "decrypted. This is stricter "
                                               + "and it has a cost: anyone "
                                               + "who has not verified their "
                                               + "own devices becomes "
                                               + "unreadable to you, and you "
                                               + "to them. Messages you can "
                                               + "already read stay readable.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    font.weight: AppTheme.weightStrong
                                    text: qsTr("Takes effect the next time "
                                               + "Lightning starts.")
                                }
                            }
                        }

                        Label {
                            text: qsTr("Reading and typing")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    text: qsTr("Read receipts")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "readReceiptModeCombo"
                                    Layout.fillWidth: true
                                    model: [
                                        qsTr("Send read receipts"),
                                        qsTr("Private — only my own devices")
                                    ]
                                    currentIndex: app.settings.readReceiptMode
                                    onActivated: (index) =>
                                        app.settings.readReceiptMode = index
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Private receipts still clear the "
                                               + "unread badge on your own other "
                                               + "devices; nobody else sees them. "
                                               + "Receipts you have already sent "
                                               + "cannot be taken back, and other "
                                               + "people's receipts are still "
                                               + "shown to you.")
                                }

                                SettingsCheckBox {
                                    objectName: "sendTypingCheck"
                                    palette.windowText: AppTheme.stormText
                                    Rectangle {
                                        objectName: "checkFocusRing"
                                        anchors.fill: parent
                                        radius: 4
                                        color: "transparent"
                                        border.color: AppTheme.stormText
                                        border.width: 2
                                        visible: parent.visualFocus
                                    }
                                    text: qsTr("Let others see when I am typing")
                                    checked: app.settings.sendTypingNotifications
                                    onToggled:
                                        app.settings.sendTypingNotifications = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Typing notices are the most frequent "
                                               + "thing a chat client discloses — "
                                               + "every few keystrokes, to everyone "
                                               + "in the room. Turning this off does "
                                               + "not hide other people's.")
                                }
                            }
                        }

                        Label {
                            visible: app.presence && app.presence.supported
                            text: qsTr("Presence")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            visible: app.presence && app.presence.supported
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "sharePresenceCheck"
                                    Rectangle {
                                        objectName: "checkFocusRing"
                                        anchors.fill: parent
                                        radius: 4
                                        color: "transparent"
                                        border.color: AppTheme.stormText
                                        border.width: 2
                                        visible: parent.visualFocus
                                    }
                                    text: qsTr("Share my online status")
                                    checked: app.settings.sharePresence
                                    onToggled: app.settings.sharePresence = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Tells your homeserver when you are online or "
                                               + "idle, so people you share rooms with can see "
                                               + "it. Turning this off publishes offline once "
                                               + "and stops updates; whether others' status is "
                                               + "visible to you is decided by their servers, "
                                               + "not by this switch.")
                                }
                            }
                        }

                        Label {
                            visible: app.moderation.supported
                            text: qsTr("Ignored users")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            id: ignoredUsersCard
                            objectName: "ignoredUsersCard"
                            visible: app.moderation.supported
                            property string unignoreError: ""
                            property string unignoreUserId: ""
                            property var removedIds: []
                            property var shownIds: []
                            function syncShown() {
                                var cur = app.moderation.ignoredUsers
                                var out = []
                                for (var i = 0; i < shownIds.length; ++i) {
                                    var id = shownIds[i]
                                    if (cur.indexOf(id) >= 0
                                            || removedIds.indexOf(id) >= 0
                                            || id === unignoreUserId)
                                        out.push(id)
                                }
                                for (var j = 0; j < cur.length; ++j) {
                                    if (out.indexOf(cur[j]) < 0)
                                        out.push(cur[j])
                                }
                                if (out.join("\n") !== shownIds.join("\n"))
                                    shownIds = out
                                var kept = removedIds.filter(
                                    function (id) { return cur.indexOf(id) < 0 })
                                if (kept.length !== removedIds.length)
                                    removedIds = kept
                            }
                            function focusNeighbour(id) {
                                var idx = shownIds.indexOf(id)
                                var order = []
                                for (var a = idx + 1; a < shownIds.length; ++a)
                                    order.push(a)
                                for (var b = idx - 1; b >= 0; --b)
                                    order.push(b)
                                for (var k = 0; k < order.length; ++k) {
                                    var row = ignoredRows.itemAt(order[k])
                                    if (row && !row.gone && row.stopButton.enabled) {
                                        row.stopButton.forceActiveFocus(
                                            Qt.TabFocusReason)
                                        return
                                    }
                                }
                                ignoredUsersCard.forceActiveFocus(
                                    Qt.TabFocusReason)
                            }
                            property bool restoreFocus: false
                            readonly property bool paneShown:
                                root.visible && root.section === "privacy"
                            onPaneShownChanged: {
                                if (!paneShown && removedIds.length > 0) {
                                    removedIds = []
                                    syncShown()
                                }
                            }
                            Component.onCompleted: syncShown()
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Connections {
                                    target: app.moderation
                                    function onStateChanged() {
                                        ignoredUsersCard.syncShown()
                                    }
                                    function onIgnoreActionFinished(
                                        userId, ignored, ok, message) {
                                        if (userId !== ignoredUsersCard.unignoreUserId)
                                            return
                                        ignoredUsersCard.unignoreUserId = ""
                                        ignoredUsersCard.unignoreError =
                                            ok ? "" : message
                                        if (ok && !ignored) {
                                            ignoredUsersCard.removedIds =
                                                ignoredUsersCard.removedIds
                                                    .concat([userId])
                                            ignoredUsersCard.syncShown()
                                            if (ignoredUsersCard.restoreFocus) {
                                                ignoredUsersCard.restoreFocus =
                                                    false
                                                Qt.callLater(
                                                    ignoredUsersCard
                                                        .focusNeighbour,
                                                    userId)
                                            }
                                        } else {
                                            ignoredUsersCard.restoreFocus = false
                                        }
                                    }
                                }

                                Label {
                                    objectName: "ignoredEmptyState"
                                    visible: app.moderation.ignoredUsers.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: qsTr("Nobody is ignored. Ignore a "
                                               + "person from their profile "
                                               + "to hide their messages "
                                               + "everywhere, on every "
                                               + "device.")
                                }
                                Repeater {
                                    id: ignoredRows
                                    model: ignoredUsersCard.shownIds
                                    delegate: RowLayout {
                                        id: ignoredRow
                                        required property string modelData
                                        readonly property bool gone:
                                            ignoredUsersCard.removedIds
                                                .indexOf(modelData) >= 0
                                        property alias stopButton: stopBtn
                                        Layout.fillWidth: true
                                        Layout.minimumHeight: 26
                                        spacing: AppTheme.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData
                                            color: ignoredRow.gone
                                                   ? AppTheme.stormTextMuted
                                                   : AppTheme.stormText
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            elide: Label.ElideRight
                                            HoverHandler { id: idHover }
                                            ToolTip.text: modelData
                                            ToolTip.delay: 500
                                            ToolTip.visible:
                                                idHover.hovered && truncated
                                        }
                                        Label {
                                            objectName: "ignoredRowGone"
                                            visible: ignoredRow.gone
                                            text: qsTr("No longer ignored")
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.settingsHintSize
                                        }
                                        AppButton {
                                            id: stopBtn
                                            objectName: "stopIgnoringButton"
                                            visible: !ignoredRow.gone
                                            storm: true
                                            implicitHeight: 26
                                            leftPadding: 10
                                            rightPadding: 10
                                            enabled: !app.moderation.busy
                                            text: qsTr("Stop ignoring")
                                            Accessible.name:
                                                qsTr("Stop ignoring %1")
                                                    .arg(modelData)
                                            onClicked: {
                                                ignoredUsersCard.restoreFocus =
                                                    stopBtn.visualFocus
                                                ignoredUsersCard.unignoreError = ""
                                                ignoredUsersCard.unignoreUserId =
                                                    modelData
                                                app.moderation
                                                    .unignoreUser(modelData)
                                            }
                                        }
                                    }
                                }
                                Label {
                                    objectName: "ignoredUsersWriteError"
                                    visible: ignoredUsersCard.unignoreError.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: ignoredUsersCard.unignoreError
                                }
                                Label {
                                    visible: app.moderation.ignoredUsers.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Ignoring hides a person's "
                                               + "messages and invites in "
                                               + "every room. The list is "
                                               + "stored in your Matrix "
                                               + "account and applies on "
                                               + "all your clients.")
                                }
                            }
                        }

                        // The local message index stores decrypted message text
                        // (the sanctioned exception in CLAUDE.md §6), so the
                        // user must be able to see it and clear it.
                        Label {
                            text: qsTr("Message search index")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                            visible: app.messageSearch.localAvailable
                        }
                        SettingsCard {
                            visible: app.messageSearch.localAvailable
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    objectName: "searchIndexHelpText"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    textFormat: Text.PlainText
                                    text: qsTr("Lightning keeps its own index "
                                        + "of the messages it has seen, so "
                                        + "you can search rooms your "
                                        + "homeserver cannot — encrypted ones "
                                        + "included. It stores the message "
                                        + "text on this device only, in this "
                                        + "account's own folder, and it is "
                                        + "deleted when the account is "
                                        + "removed. It is not encrypted on "
                                        + "disk — nor is the Matrix SDK's own "
                                        + "message store beside it — so your "
                                        + "device's disk encryption is what "
                                        + "protects both.")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing12
                                    Label {
                                        objectName: "searchIndexStatsLabel"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        textFormat: Text.PlainText
                                        text: qsTr("%n message(s) indexed",
                                                   "",
                                                   app.messageSearch.indexedMessages)
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    AppButton {
                                        objectName: "clearSearchIndexButton"
                                        text: qsTr("Clear index")
                                        kind: "danger"
                                        enabled: app.messageSearch.indexedMessages > 0
                                        onClicked: clearIndexConfirm.visible = true
                                    }
                                }

                                Label {
                                    objectName: "indexAllHelpText"
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    textFormat: Text.PlainText
                                    text: qsTr("Index encrypted rooms fetches "
                                        + "older history for every encrypted room "
                                        + "you are in, one room at a time and up "
                                        + "to the same per-room limit as "
                                        + "Index this room, so you can "
                                        + "search it here. It can "
                                        + "take a long time and uses "
                                        + "bandwidth, and like the rest of "
                                        + "the index it stores the decrypted "
                                        + "message text on this device. "
                                        + "Messages that cannot be decrypted "
                                        + "yet are skipped until their keys "
                                        + "arrive — run it again then and "
                                        + "they are added. It pauses during "
                                        + "calls and continues after a "
                                        + "restart.")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                }
                                Label {
                                    objectName: "indexAllProgressLabel"
                                    visible: text !== ""
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: {
                                        var s = app.messageSearch
                                        var st = s.indexAllState
                                        var lines = []
                                        var underWay = s.indexAllActive
                                            || s.indexAllResumable
                                        if (underWay) {
                                            var where = qsTr("%1 of %2 rooms")
                                                .arg(s.indexAllPosition)
                                                .arg(s.indexAllTotal)
                                            if (s.indexAllActive
                                                    && s.indexAllRoomName !== "")
                                                where += " · " + s.indexAllRoomName
                                            lines.push(where)
                                            lines.push(qsTr("%n message(s) added",
                                                            "", s.indexAllWritten))
                                            if (st === "held")
                                                lines.push(qsTr("Waiting while a "
                                                    + "call is active or a "
                                                    + "timeline is scrolling"))
                                            else if (st === "backoff")
                                                lines.push(qsTr("The server asked "
                                                    + "Lightning to slow down; "
                                                    + "trying again shortly"))
                                            else if (st === "paused")
                                                lines.push(qsTr("Paused"))
                                            else if (st === "stopped")
                                                lines.push(qsTr("Continues once "
                                                    + "this account is connected"))
                                            else if (s.indexAllEtaMs > 0)
                                                lines.push(qsTr("About %n minute(s) left",
                                                    "", Math.max(1, Math.round(
                                                        s.indexAllEtaMs / 60000))))
                                        } else if (st === "done") {
                                            lines.push(s.indexAllTotal > 0
                                                ? qsTr("Finished: each room was "
                                                       + "indexed back to its "
                                                       + "start or to the "
                                                       + "per-room limit")
                                                : qsTr("Nothing new to do: an "
                                                       + "earlier run already "
                                                       + "covered every room"))
                                            if (s.indexAllWritten > 0)
                                                lines.push(qsTr("%n message(s) added",
                                                                "", s.indexAllWritten))
                                        } else if (st === "cancelled") {
                                            lines.push(qsTr("Stopped. Rooms "
                                                + "already indexed stay "
                                                + "indexed."))
                                        }
                                        if (s.indexAllUndecryptable > 0)
                                            lines.push(qsTr("%n message(s) could "
                                                + "not be decrypted yet and "
                                                + "were skipped", "",
                                                s.indexAllUndecryptable))
                                        if (s.indexAllFailedRooms > 0)
                                            lines.push(qsTr("%n room(s) could not "
                                                + "be fetched and will be "
                                                + "tried next time", "",
                                                s.indexAllFailedRooms))
                                        return lines.join("\n")
                                    }
                                }
                                AppProgressBar {
                                    objectName: "indexAllProgressBar"
                                    visible: app.messageSearch.indexAllActive
                                             || app.messageSearch.indexAllResumable
                                    Layout.fillWidth: true
                                    value: app.messageSearch.indexAllTotal > 0
                                           ? app.messageSearch.indexAllPosition
                                             / app.messageSearch.indexAllTotal
                                           : 0
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing12
                                    AppButton {
                                        objectName: "indexAllRoomsButton"
                                        text: app.messageSearch.indexAllActive
                                              ? qsTr("Pause")
                                              : app.messageSearch.indexAllResumable
                                                ? qsTr("Resume")
                                                : qsTr("Index encrypted rooms")
                                        kind: app.messageSearch.indexAllActive
                                              ? "secondary" : "primary"
                                        onClicked: {
                                            if (app.messageSearch.indexAllActive)
                                                app.messageSearch.pauseIndexAll()
                                            else
                                                app.messageSearch.indexAllRooms()
                                        }
                                    }
                                    AppButton {
                                        objectName: "indexAllCancelButton"
                                        text: qsTr("Stop")
                                        kind: "ghost"
                                        visible: app.messageSearch.indexAllActive
                                                 || app.messageSearch.indexAllResumable
                                        onClicked: app.messageSearch.cancelIndexAll()
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                        MediaPreviewSettings {
                            objectName: "mediaPreviewSettingsSection"
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Link previews & media")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    id: autoPreviewCheck
                                    objectName: "autoPreviewCheck"
                                    text: qsTr("Automatically load previews in unencrypted rooms")
                                    checked: app.settings.autoLoadLinkPreviews
                                    onToggled: app.settings.autoLoadLinkPreviews = checked
                                }
                                SettingsHint {
                                    objectName: "autoPreviewHint"
                                    text: qsTr("If your homeserver cannot build the preview, "
                                               + "Lightning fetches the link itself and the "
                                               + "site sees your IP address.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "encryptedPreviewCheck"
                                    text: qsTr("Load previews in encrypted rooms")
                                    checked: app.settings.loadPreviewsInEncryptedRooms
                                    onToggled: app.settings.loadPreviewsInEncryptedRooms = checked
                                }
                                SettingsHint {
                                    objectName: "encryptedPreviewHint"
                                    text: qsTr("Asking for a preview tells your homeserver which "
                                               + "link was in an encrypted message.")
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "inlineLinkMediaCheck"
                                    text: qsTr("Show images and videos from links inline")
                                    checked: app.settings.showLinkMediaInline
                                    onToggled: app.settings.showLinkMediaInline = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("A picture opens in Lightning's viewer and a video plays here. "
                                               + "A video, or a picture too large to preview, is downloaded "
                                               + "from the link's site only when you press Play or open it.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    spacing: AppTheme.spacing8
                                    Rectangle {
                                        Layout.fillHeight: true
                                        implicitWidth: 2
                                        radius: 1
                                        color: AppTheme.stormDangerBorder
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: qsTr("Your homeserver loads the preview, so the "
                                                   + "linked site sees your server rather than "
                                                   + "you. If your server cannot — many have "
                                                   + "previews turned off — Lightning loads it "
                                                   + "directly instead, which may reveal your IP "
                                                   + "address and request timing to a site the "
                                                   + "sender chose. Asking your homeserver also "
                                                   + "tells it which link was previewed, which in "
                                                   + "an encrypted room it would not otherwise "
                                                   + "know. No JavaScript is executed. Both "
                                                   + "switches are off by default; leave them off "
                                                   + "and use the “Show” button on each message's "
                                                   + "link card to decide one at a time.")
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.stormBorder
                                }

                                Label {
                                    text: qsTr("Media")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }

                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "videoPrefetchCheck"
                                    text: qsTr("Preload videos and audio")
                                    checked: app.settings.videoPrefetchEnabled
                                    onToggled: app.settings.videoPrefetchEnabled = checked
                                    Accessible.name: qsTr("Preload videos and audio")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Downloads videos up to the size below, and audio "
                                               + "and voice messages, before you press Play, so "
                                               + "playback starts at once. This uses data and disk "
                                               + "space even for media you never play. When off, "
                                               + "nothing is downloaded until you press Play.")
                                }
                                ColumnLayout {
                                    objectName: "videoPrefetchLimitRow"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing4
                                    visible: app.settings.videoPrefetchEnabled
                                    Label {
                                        objectName: "videoPrefetchLimitLabel"
                                        text: qsTr("Up to %1 MB").arg(app.settings.videoPrefetchMaxMb)
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        SettingsSlider {
                                            objectName: "videoPrefetchSlider"
                                            Layout.fillWidth: true
                                            from: app.settings.videoPrefetchMinMb
                                            to: app.settings.videoPrefetchLimitMb
                                            stepSize: 1
                                            value: app.settings.videoPrefetchMaxMb
                                            onMoved: app.settings.videoPrefetchMaxMb = Math.round(value)
                                            Accessible.name: qsTr("Largest video to preload, in megabytes")
                                        }
                                        AppTextField {
                                            id: videoPrefetchField
                                            storm: true
                                            objectName: "videoPrefetchField"
                                            implicitWidth: 72
                                            horizontalAlignment: TextInput.AlignRight
                                            inputMethodHints: Qt.ImhDigitsOnly
                                            validator: RegularExpressionValidator { regularExpression: /[0-9]{0,4}/ }
                                            text: String(app.settings.videoPrefetchMaxMb)
                                            Accessible.name: qsTr("Largest video to preload, in megabytes")
                                            onEditingFinished: {
                                                var n = parseInt(text, 10)
                                                if (!isNaN(n))
                                                    app.settings.videoPrefetchMaxMb = n
                                                videoPrefetchField.syncFromSettings()
                                            }
                                            function syncFromSettings() {
                                                text = String(app.settings.videoPrefetchMaxMb)
                                            }
                                            Connections {
                                                target: app.settings
                                                function onVideoPrefetchMaxMbChanged() {
                                                    videoPrefetchField.syncFromSettings()
                                                }
                                            }
                                        }
                                        Label {
                                            text: qsTr("MB")
                                            color: AppTheme.stormTextSecondary
                                            font.pixelSize: AppTheme.settingsOptionSize
                                        }
                                    }
                                }

                                Label { text: qsTr("Photo details when sending"); color: AppTheme.stormTextSecondary }
                                PhotoMetadataSetting { Layout.fillWidth: true }

                                Label { text: qsTr("GIF safe search"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifRatingCombo
                                    objectName: "gifRatingCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("GIF safe search rating")
                                    model: [
                                        { label: qsTr("G — strict"),  value: 0 },
                                        { label: qsTr("PG"),          value: 1 },
                                        { label: qsTr("PG-13"),       value: 2 },
                                        { label: qsTr("R — all"),     value: 3 },
                                    ]
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifSafeSearch)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifSafeSearchChanged() {
                                            gifRatingCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifSafeSearch = currentValue
                                }

                                Label { text: qsTr("Preferred GIF provider"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifProviderCombo
                                    objectName: "gifProviderCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Preferred GIF provider")
                                    model: [
                                        { label: "GIPHY", value: "giphy" },
                                        { label: "KLIPY", value: "klipy" },
                                    ]
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifPreferredProvider)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifPreferredProviderChanged() {
                                            gifProviderCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifPreferredProvider = currentValue
                                }
                                SettingsHint {
                                    objectName: "gifProviderHint"
                                    text: qsTr("Only the words you search for go to the provider. "
                                               + "Your Matrix ID, rooms and messages never do.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("GIPHY: %1 · KLIPY: %2")
                                        .arg(app.gif.providerConfigured("giphy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                        .arg(app.gif.providerConfigured("klipy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                }

                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Store recently used GIFs")
                                    checked: app.settings.storeRecentGifs
                                    onToggled: app.settings.storeRecentGifs = checked
                                    Accessible.name: qsTr("Store recently used GIFs")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("GIF searches are sent directly to the "
                                               + "selected provider. Saved and recent "
                                               + "GIFs are stored locally on this device "
                                               + "and are not synchronized; search terms "
                                               + "are not saved.")
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Clear recent GIFs")
                                        enabled: app.gif.recent.count > 0
                                        onClicked: gifClearConfirm.open("recent")
                                    }
                                    AppButton {
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Clear saved provider GIFs")
                                        enabled: app.gif.favorites.count > 0
                                        onClicked: gifClearConfirm.open("favorites")
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.stormBorder
                                }

                                Label {
                                    text: qsTr("Images saved from chats")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }
                                Label {
                                    objectName: "starredGifsSummaryLabel"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("%1 image(s), %2 — kept on this "
                                               + "device only and removed "
                                               + "when you sign out of this "
                                               + "account.")
                                        .arg(app.gif.starredStore.count)
                                        .arg(root.formatBytes(
                                            app.gif.starredStore.totalBytes))
                                }
                                AppButton {
                                    objectName: "clearStarredGifsButton"
                                    storm: true
                                    kind: "danger"
                                    text: qsTr("Clear all images saved from chats")
                                    enabled: app.gif.starredStore.count > 0
                                    onClicked: starredGifsClearConfirm.open()
                                }

                                Label {
                                    text: qsTr("Hidden images")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }
                                Label {
                                    objectName: "hiddenMediaSummaryLabel"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: app.mediaVisibility.hiddenCount === 0
                                        ? qsTr("You have not hidden any images. Hiding one affects only what you see, and nothing is sent.")
                                        : (app.mediaVisibility.hiddenCount === 1
                                           ? qsTr("1 image is hidden on this device for this account.")
                                           : qsTr("%1 images are hidden on this device for this account.")
                                             .arg(app.mediaVisibility.hiddenCount))
                                }
                                AppButton {
                                    objectName: "showAllHiddenMediaButton"
                                    storm: true
                                    text: qsTr("Show all hidden images")
                                    enabled: app.mediaVisibility.hiddenCount > 0
                                    onClicked: app.mediaVisibility.clear()
                                }

                                Label {
                                    text: qsTr("Media kept on this device")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }
                                SettingsCheckBox {
                                    objectName: "keepMediaOnDeviceCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Keep downloaded media on this device")
                                    checked: app.settings.keepMediaOnDevice
                                    onToggled:
                                        app.settings.keepMediaOnDevice = checked
                                }
                                Label {
                                    objectName: "keepMediaOnDeviceState"
                                    readonly property string keepState:
                                        app.mediaKeepState
                                    visible: app.settings.keepMediaOnDevice
                                             && text !== ""
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    font.pixelSize: AppTheme.settingsHintSize
                                    color: keepState.indexOf("notKept") === 0
                                           ? AppTheme.stormDanger
                                           : AppTheme.stormTextSecondary
                                    text: {
                                        switch (keepState) {
                                        case "kept":
                                            return qsTr("Kept on this device, "
                                                        + "including media from "
                                                        + "encrypted rooms.")
                                        case "keptExceptEncrypted":
                                            return qsTr("Kept, except media from "
                                                        + "encrypted rooms: no "
                                                        + "secure system keyring "
                                                        + "holds the key.")
                                        case "keptExceptEncryptedPendingUpload":
                                            return qsTr("Kept this session, "
                                                        + "except media from "
                                                        + "encrypted rooms, while "
                                                        + "an attachment from an "
                                                        + "earlier session "
                                                        + "finishes uploading.")
                                        case "notKeptNewSignIn":
                                            return qsTr("Not kept this session: "
                                                        + "this account was just "
                                                        + "signed in. Media is kept "
                                                        + "from the next start.")
                                        case "notKeptKeyring":
                                            return qsTr("Not kept this session: "
                                                        + "the system keyring is "
                                                        + "locked or unavailable, "
                                                        + "or its key could not "
                                                        + "be read.")
                                        case "notKeptStore":
                                            return qsTr("Not kept this session: "
                                                        + "the media store on this "
                                                        + "device could not be "
                                                        + "opened.")
                                        }
                                        return ""
                                    }
                                }
                                Label {
                                    objectName: "keepMediaOnDeviceNote"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Pictures, videos and files you "
                                               + "open are kept in this account's "
                                               + "folder, so they open again "
                                               + "without downloading: up to about "
                                               + "2 GB, for 60 days after you "
                                               + "last open them, and removed when "
                                               + "you sign out. They are kept "
                                               + "encrypted, and the key is kept "
                                               + "in your system keyring. Media you "
                                               + "open in encrypted rooms is kept "
                                               + "only then; on macOS, in a "
                                               + "portable install and on a "
                                               + "desktop with no system keyring "
                                               + "the key is kept on this disk "
                                               + "instead, and media you open in "
                                               + "encrypted rooms is not kept. "
                                               + "While the keyring is locked, "
                                               + "and in an account's first "
                                               + "session, nothing is kept. Files "
                                               + "over 100 MB from unencrypted "
                                               + "rooms are kept unencrypted; "
                                               + "files over 100 MB from "
                                               + "encrypted rooms are downloaded "
                                               + "again each session. "
                                               + "Attachments you "
                                               + "send, from any room, are kept "
                                               + "the same way until they are "
                                               + "uploaded and then for up to 60 "
                                               + "days, whatever this setting "
                                               + "says. Turning it off stops "
                                               + "keeping media you open; Clear "
                                               + "removes what is already kept.")
                                }
                                AppButton {
                                    objectName: "clearStoredMediaButton"
                                    storm: true
                                    kind: "danger"
                                    text: qsTr("Clear kept media")
                                    enabled: !root.storedMediaClearing
                                    onClicked: {
                                        root.storedMediaStatus = ""
                                        root.storedMediaClearing =
                                            app.clearStoredMedia()
                                        if (!root.storedMediaClearing)
                                            root.storedMediaStatus = qsTr(
                                                "Nothing was cleared: no account "
                                                + "is signed in.")
                                    }
                                }
                                Label {
                                    objectName: "storedMediaStatusLabel"
                                    visible: root.storedMediaStatus !== ""
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: root.storedMediaStatus
                                }
                                Connections {
                                    target: app
                                    function onStoredMediaCleared(ok, files, bytes) {
                                        root.storedMediaClearing = false
                                        if (!ok)
                                            root.storedMediaStatus = qsTr(
                                                "Some kept media could not be "
                                                + "removed. Try again.")
                                        else if (files > 0)
                                            root.storedMediaStatus = qsTr(
                                                "Kept media removed, including "
                                                + "%1 of large files.")
                                                .arg(root.formatBytes(bytes))
                                        else
                                            root.storedMediaStatus = qsTr(
                                                "Kept media removed. It downloads "
                                                + "again when you open it.")
                                    }
                                }

                                Label {
                                    text: qsTr("Downloads")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing8
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        objectName: "downloadFolderLabel"
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: app.downloads
                                            ? qsTr("Save files to %1")
                                              .arg(app.downloads.folderDisplay)
                                            : ""
                                    }
                                    AppButton {
                                        objectName: "downloadFolderChangeButton"
                                        storm: true
                                        size: "sm"
                                        text: qsTr("Change…")
                                        enabled: !!app.downloads
                                                 && !app.downloads.sandboxRequiresAsking
                                        onClicked: app.downloads.chooseFolder()
                                    }
                                    AppButton {
                                        objectName: "downloadFolderResetButton"
                                        storm: true
                                        size: "sm"
                                        kind: "ghost"
                                        visible: !!app.downloads && app.downloads.customFolder
                                        text: qsTr("Use the Downloads folder")
                                        onClicked: app.downloads.resetFolder()
                                    }
                                }
                                SettingsCheckBox {
                                    objectName: "alwaysAskWhereToSaveCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Always ask where to save files")
                                    checked: !!app.downloads && app.downloads.asksWhereToSave
                                    enabled: !!app.downloads
                                             && !app.downloads.sandboxRequiresAsking
                                    onToggled:
                                        app.settings.alwaysAskWhereToSave = checked
                                }
                                Label {
                                    objectName: "alwaysAskWhereToSaveNote"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    textFormat: Text.PlainText
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: !!app.downloads && app.downloads.sandboxRequiresAsking
                                        ? qsTr("This copy of Lightning runs in "
                                               + "a sandbox that cannot write to "
                                               + "your Downloads folder, so it "
                                               + "asks where to save each file.")
                                        : qsTr("Download saves a file here "
                                               + "without asking and never "
                                               + "replaces one that is already "
                                               + "there: a second copy is saved "
                                               + "as \"name (1)\". Save as… "
                                               + "always asks. Lightning never "
                                               + "opens a downloaded file by "
                                               + "itself.")
                                }
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("notifications")
                        visible: root.section === "notifications"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Notifications")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "notificationsEnabledCheck"
                                    text: qsTr("Desktop notifications")
                                    checked: app.settings.notificationsEnabled
                                    onToggled: app.settings.notificationsEnabled = checked
                                }
                                Label {
                                    text: qsTr("Notification preview")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationPreviewCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    model: [
                                        qsTr("Sender and message"),
                                        qsTr("Sender only"),
                                        qsTr("Private")
                                    ]
                                    currentIndex: app.settings.notificationPreview
                                    onActivated: (index) =>
                                        app.settings.notificationPreview = index
                                }
                                Label {
                                    objectName: "notificationPreviewHelp"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Sender and message is the default: "
                                               + "a notification carries the message "
                                               + "text. Sender only shows who wrote "
                                               + "and never what they wrote, and "
                                               + "Private withholds the sender and "
                                               + "the room as well. "
                                               + "Encrypted messages that cannot be "
                                               + "decrypted always show a generic "
                                               + "notification. Notifications are "
                                               + "suppressed while the room is open, "
                                               + "focused, and at the latest message.")
                                }
                                // A separate level for encrypted rooms: a
                                // notification body is written to the desktop
                                // daemon and its log in plaintext, outside the
                                // room's encryption.
                                Label {
                                    text: qsTr("In encrypted rooms")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationPreviewEncryptedCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    model: [
                                        qsTr("Sender and message"),
                                        qsTr("Sender only"),
                                        qsTr("Private"),
                                        qsTr("Same as other rooms")
                                    ]
                                    currentIndex:
                                        app.settings.notificationPreviewEncrypted
                                    onActivated: (index) =>
                                        app.settings.notificationPreviewEncrypted = index
                                }
                                Label {
                                    text: qsTr("Notification sound")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationSoundCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    model: [
                                        qsTr("Off"),
                                        qsTr("Mentions and direct messages"),
                                        qsTr("All notifications")
                                    ]
                                    currentIndex: app.settings.notificationSound
                                    onActivated: (index) =>
                                        app.settings.notificationSound = index
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("The sound plays only when a "
                                               + "notification is shown, so muted and "
                                               + "active rooms stay silent. Bursts are "
                                               + "coalesced into a single alert.")
                                }
                                Label {
                                    text: qsTr("Notification sound style")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationSoundSourceCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                        && app.settings.notificationSound !== 0
                                    model: [
                                        qsTr("Lightning"),
                                        qsTr("System default")
                                    ]
                                    currentIndex:
                                        app.settings.notificationSoundSource
                                    onActivated: (index) =>
                                        app.settings.notificationSoundSource = index
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Notification volume")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightMedium
                                    }
                                    SettingsPercentBox {
                                        objectName: "notificationVolumePercentBox"
                                        slider: notificationSoundVolumeSlider
                                        follows: app.settings.notificationSoundVolume
                                    }
                                    AppButton {
                                        objectName: "notificationSoundPreviewButton"
                                        storm: true
                                        size: "sm"
                                        text: qsTr("Test")
                                        Accessible.name:
                                            qsTr("Play the message sound")
                                        onClicked:
                                            app.callSounds.previewNotification(false)
                                    }
                                    AppButton {
                                        objectName: "notificationMentionPreviewButton"
                                        storm: true
                                        size: "sm"
                                        text: qsTr("Test mention")
                                        Accessible.name:
                                            qsTr("Play the mention sound")
                                        onClicked:
                                            app.callSounds.previewNotification(true)
                                    }
                                }
                                SettingsSlider {
                                    id: notificationSoundVolumeSlider
                                    objectName: "notificationSoundVolumeSlider"
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    stepSize: 5
                                    enabled: app.settings.notificationsEnabled
                                        && app.settings.notificationSound !== 0
                                        && app.settings.notificationSoundSource === 0
                                    value: app.settings.notificationSoundVolume
                                    Accessible.name: qsTr("Notification volume")
                                    onMoved: app.settings.notificationSoundVolume =
                                             Math.round(value)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Lightning plays its own chime, and "
                                               + "mentions get a slightly more urgent "
                                               + "one; the desktop's notification sound "
                                               + "is turned off. It stays quiet during "
                                               + "calls. Choose System default to let the "
                                               + "desktop play its own, where the desktop "
                                               + "can; on macOS it cannot, and Lightning's "
                                               + "chime plays instead.")
                                }
                                SettingsCheckBox {
                                    objectName: "ringForCallsCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Ring for incoming voice calls")
                                    enabled: app.settings.notificationsEnabled
                                    checked: app.settings.ringForCalls
                                    onToggled:
                                        app.settings.ringForCalls = checked
                                    Accessible.description:
                                        qsTr("Repeat the call sound while an "
                                             + "incoming voice call is "
                                             + "ringing. This is independent of "
                                             + "Notification sound: the ring "
                                             + "follows this setting alone. "
                                             + "Turning it off still shows "
                                             + "the call — it only silences "
                                             + "the ring.")
                                    SettingsNeedsBadge { needs: qsTr("Desktop notifications") }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: (app.serverRoomNotificationModes
                                           ? qsTr("Per-room notification modes (set "
                                                  + "from Room information) are saved "
                                                  + "to your account's notification "
                                                  + "settings (server push rules). ")
                                           : qsTr("Per-room notification modes (set "
                                                  + "from Room information) apply to "
                                                  + "this device only — they are not "
                                                  + "server push rules. "))
                                          + qsTr("Lightning does not register for "
                                                 + "push, so its notifications arrive "
                                                 + "only while it is running, in the "
                                                 + "window or the tray.")
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    text: qsTr("Unread dot")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "unreadDotModeCombo"
                                    Layout.fillWidth: true
                                    model: [
                                        qsTr("All rooms"),
                                        qsTr("Only rooms with notifications on"),
                                        qsTr("Never")
                                    ]
                                    currentIndex: app.settings.unreadDotMode
                                    onActivated: (index) =>
                                        app.settings.unreadDotMode = index
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Muted rooms never show it. With "
                                               + "notifications on only, a room set to "
                                               + "mentions and keywords, or following "
                                               + "an account default of mentions only, "
                                               + "shows no dot. Mention badges are "
                                               + "always shown.")
                                }
                                Label {
                                    text: qsTr("Mark all rooms read automatically")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                RowLayout {
                                    id: autoReadRow
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    readonly property var presets: [0, 15, 30, 60]
                                    property bool custom: false
                                    readonly property int minutes:
                                        app.settings.autoMarkReadMinutes
                                    readonly property int presetIndex:
                                        presets.indexOf(minutes)
                                    AppComboBox {
                                        storm: true
                                        objectName: "autoMarkReadCombo"
                                        Layout.fillWidth: true
                                        model: [
                                            qsTr("Off"),
                                            qsTr("Every 15 minutes"),
                                            qsTr("Every 30 minutes"),
                                            qsTr("Every hour"),
                                            qsTr("Custom")
                                        ]
                                        currentIndex: autoReadRow.custom
                                                      || autoReadRow.presetIndex < 0
                                                      ? 4 : autoReadRow.presetIndex
                                        onActivated: (index) => {
                                            if (index === 4) {
                                                autoReadRow.custom = true
                                                if (autoReadRow.minutes === 0)
                                                    app.settings.autoMarkReadMinutes = 45
                                                return
                                            }
                                            autoReadRow.custom = false
                                            app.settings.autoMarkReadMinutes =
                                                autoReadRow.presets[index]
                                        }
                                    }
                                    AppTextField {
                                        objectName: "autoMarkReadMinutesField"
                                        storm: true
                                        visible: autoReadRow.custom
                                                 || autoReadRow.presetIndex < 0
                                        Layout.preferredWidth: 96
                                        text: String(autoReadRow.minutes)
                                        placeholderText: qsTr("Minutes")
                                        Accessible.name: qsTr("Minutes between automatic mark-read passes")
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        validator: IntValidator { bottom: 5; top: 1440 }
                                        function commitMinutes() {
                                            if (acceptableInput)
                                                app.settings.autoMarkReadMinutes =
                                                    parseInt(text, 10)
                                            text = Qt.binding(() => String(autoReadRow.minutes))
                                        }
                                        onEditingFinished: commitMinutes()
                                        // A validator suppresses editingFinished for "3".
                                        onActiveFocusChanged: if (!activeFocus) commitMinutes()
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Sends read receipts for every unread "
                                               + "room except the one you have open, "
                                               + "as Mark all as read does, following "
                                               + "your read receipt privacy. Custom "
                                               + "takes 5 to 1440 minutes.")
                                }
                            }
                        }
                        NotificationRulesSettings {
                            Layout.fillWidth: true
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("sound")
                        visible: root.section === "sound"
                        sourceComponent: ColumnLayout {
                        NoiseSuppressionSelector {
                            id: noiseTitles
                            objectName: "noiseSuppressionTitleSource"
                            visible: false
                        }
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Sound & video")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CallDeviceSettings {
                                    objectName: "callDeviceSettings"
                                    Layout.fillWidth: true
                                    activated: visible
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CallQualitySettings {
                                    objectName: "callQualitySettings"
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        SettingsCard {
                            objectName: "soundNoiseSuppressionRow"
                            RowLayout {
                                width: parent.width
                                spacing: AppTheme.spacing12
                                Icon {
                                    name: "science"
                                    size: 18
                                    color: AppTheme.stormTextSecondary
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Microphone noise suppression")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        objectName: "soundNoiseSuppressionValue"
                                        Layout.fillWidth: true
                                        text: qsTr("%1 · an experiment, set in Labs or "
                                                   + "from the call's microphone menu")
                                              .arg(noiseTitles.titleFor(
                                                  app.settings.noiseSuppressionMode))
                                        textFormat: Text.PlainText
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                        elide: Text.ElideRight
                                    }
                                }
                                AppButton {
                                    objectName: "soundNoiseSuppressionOpenLabs"
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Change…")
                                    Accessible.name: qsTr("Change noise suppression in Labs")
                                    onClicked: root.revealSearchResult(
                                        { section: "labs", anchor: "noiseSuppressionCard" })
                                }
                            }
                        }

                        Label {
                            text: qsTr("Media playback")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.settingsOptionSize
                            font.weight: AppTheme.weightBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: 4

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8

                                    Icon {
                                        name: mediaVolumeSettingSlider.value <= 0
                                              ? "volume_off" : "volume_up"
                                        size: 18
                                        color: AppTheme.stormTextSecondary
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Media playback volume")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightMedium
                                    }
                                    SettingsPercentBox {
                                        objectName: "mediaVolumeSettingReadout"
                                        slider: mediaVolumeSettingSlider
                                        follows: app.settings.mediaVolume
                                        valueScale: 100
                                    }
                                }
                                SettingsSlider {
                                    id: mediaVolumeSettingSlider
                                    objectName: "mediaVolumeSettingSlider"
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 1
                                    stepSize: 0.05
                                    value: app.settings.mediaVolume
                                    Accessible.name: qsTr("Media playback volume")
                                    // onMoved, not onValueChanged, which would
                                    // also fire for values coming from the
                                    // store and write them back.
                                    onMoved: app.settings.mediaVolume = value
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("The level voice messages, audio files and videos start at. Changing the volume on a player remembers it here too.")
                                }
                                SettingsCheckBox {
                                    objectName: "voicePlayerDockedCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Show the audio player as a strip under the header")
                                    checked: app.settings.voicePlayerDocked
                                    onToggled: app.settings.voicePlayerDocked = checked
                                    Accessible.description: qsTr("When off, the player for voice messages and audio files floats over the window, and you can move and resize it.")
                                }
                                Label {
                                    text: qsTr("Album art")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                }
                                AppComboBox {
                                    storm: true
                                    id: albumArtStyleCombo
                                    objectName: "albumArtStyleCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Album art")
                                    Accessible.description: qsTr("How the cover of an audio file is shown: a small tile beside the play button, a larger tile beside the title and controls, or a big square above the controls. A file with no cover, or a narrow message, always gets the small tile.")
                                    model: [
                                        { label: qsTr("Compact"), value: "compact" },
                                        { label: qsTr("Card"),    value: "card" },
                                        { label: qsTr("Large"),   value: "large" },
                                    ]
                                    function syncFromSettings() {
                                        syncToValue(app.settings.albumArtStyle)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onAlbumArtStyleChanged() {
                                            albumArtStyleCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.albumArtStyle = currentValue
                                }
                            }
                        }

                        Label {
                            text: qsTr("Call sounds")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.settingsOptionSize
                            font.weight: AppTheme.weightBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: 4

                                SettingsCheckBox {
                                    objectName: "callSoundsEnabledCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Play sounds during calls")
                                    checked: app.settings.callSoundsEnabled
                                    onToggled:
                                        app.settings.callSoundsEnabled = checked
                                    Accessible.description:
                                        qsTr("Short sounds when you join or "
                                             + "leave a call, when others "
                                             + "do, and when you mute or "
                                             + "share your screen.")
                                }
                                SettingsCheckBox {
                                    objectName: "callSoundsPresenceCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("People joining and leaving")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsPresence
                                    onToggled:
                                        app.settings.callSoundsPresence = checked
                                    SettingsNeedsBadge { needs: qsTr("Play sounds during calls") }
                                }
                                SettingsCheckBox {
                                    objectName: "callSoundsControlsCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Muting and deafening")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsControls
                                    onToggled:
                                        app.settings.callSoundsControls = checked
                                    SettingsNeedsBadge { needs: qsTr("Play sounds during calls") }
                                }
                                SettingsCheckBox {
                                    objectName: "callSoundsShareAndHandCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Screen shares and raised hands")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsShareAndHand
                                    onToggled:
                                        app.settings.callSoundsShareAndHand = checked
                                    SettingsNeedsBadge { needs: qsTr("Play sounds during calls") }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Call sound volume")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightMedium
                                    }
                                    SettingsPercentBox {
                                        objectName: "callSoundVolumePercentBox"
                                        slider: callSoundVolumeSlider
                                        follows: app.settings.callSoundVolume
                                    }
                                    AppButton {
                                        objectName: "callSoundPreviewButton"
                                        storm: true
                                        size: "sm"
                                        text: qsTr("Test")
                                        Accessible.name:
                                            qsTr("Play a call sound")
                                        onClicked: app.callSounds.preview("connected")
                                    }
                                }
                                SettingsSlider {
                                    id: callSoundVolumeSlider
                                    objectName: "callSoundVolumeSlider"
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    stepSize: 5
                                    enabled: app.settings.callSoundsEnabled
                                    value: app.settings.callSoundVolume
                                    Accessible.name: qsTr("Call sound volume")
                                    onMoved: app.settings.callSoundVolume =
                                             Math.round(value)
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Ringer volume")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightMedium
                                    }
                                    SettingsPercentBox {
                                        objectName: "ringerVolumePercentBox"
                                        slider: ringerVolumeSlider
                                        follows: app.settings.ringerVolume
                                    }
                                    AppButton {
                                        objectName: "ringerPreviewButton"
                                        storm: true
                                        size: "sm"
                                        text: qsTr("Test")
                                        Accessible.name:
                                            qsTr("Play the ringer")
                                        onClicked: app.callSounds.preview("ring")
                                    }
                                }
                                SettingsSlider {
                                    id: ringerVolumeSlider
                                    objectName: "ringerVolumeSlider"
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    stepSize: 5
                                    value: app.settings.ringerVolume
                                    Accessible.name: qsTr("Ringer volume")
                                    onMoved: app.settings.ringerVolume =
                                             Math.round(value)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("These sounds play only on this computer; nobody else in the call hears them. While you are deafened, only your own actions make a sound. Whether a call rings at all is set under Notifications.")
                                }
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("account")
                        visible: root.section === "account"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Account")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            id: accountIdentityCard
                            objectName: "accountIdentityCard"
                            property var accountRecord: ({})
                            function refreshAccountRecord() {
                                accountRecord =
                                    (app.accounts && app.accounts.activeUserId)
                                    ? app.accounts.account(app.accounts.activeUserId)
                                    : ({})
                            }
                            Component.onCompleted: refreshAccountRecord()
                            onEditingDisplayNameChanged: root.identityEditing = editingDisplayName
                            Component.onDestruction: root.identityEditing = false
                            Connections {
                                target: app.accounts
                                function onAccountsChanged() {
                                    accountIdentityCard.refreshAccountRecord()
                                }
                                function onActiveUserIdChanged() {
                                    accountIdentityCard.refreshAccountRecord()
                                }
                            }
                            readonly property string accountDisplayName:
                                accountRecord && accountRecord.displayName
                                ? accountRecord.displayName : ""

                            property bool editingDisplayName: false
                            function beginDisplayNameEdit() {
                                displayNameField.text =
                                    accountIdentityCard.accountDisplayName
                                app.dismissOwnDisplayNameError()
                                editingDisplayName = true
                                displayNameField.forceActiveFocus()
                                displayNameField.selectAll()
                            }
                            function cancelDisplayNameEdit() {
                                if (app.ownDisplayNameBusy) return
                                editingDisplayName = false
                                app.dismissOwnDisplayNameError()
                                displayNameField.text =
                                    accountIdentityCard.accountDisplayName
                            }
                            function commitDisplayName() {
                                if (app.ownDisplayNameBusy) return
                                var wanted = displayNameField.text.trim()
                                if (wanted === accountIdentityCard.accountDisplayName) {
                                    editingDisplayName = false
                                    app.dismissOwnDisplayNameError()
                                    return
                                }
                                app.submitOwnDisplayName(wanted)
                            }
                            Connections {
                                target: app
                                function onOwnDisplayNameSaved() {
                                    accountIdentityCard.editingDisplayName = false
                                }
                                function onLoggedInChanged() {
                                    accountIdentityCard.editingDisplayName = false
                                }
                            }
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Avatar {
                                        size: 48
                                        circle: false
                                        squareRadius: 14
                                        mxc: accountIdentityCard.accountRecord
                                             && accountIdentityCard.accountRecord.avatarUrl
                                             ? accountIdentityCard.accountRecord.avatarUrl : ""
                                        name: accountIdentityCard.accountDisplayName.length > 0
                                              ? accountIdentityCard.accountDisplayName
                                              : (app.accounts ? (app.accounts.activeUserId || "") : "")
                                        colorKey: app.accounts ? (app.accounts.activeUserId || "") : ""
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            Label {
                                                text: accountIdentityCard.accountDisplayName.length > 0
                                                      ? accountIdentityCard.accountDisplayName
                                                      : (app.accounts
                                                         ? (app.accounts.activeUserId || qsTr("(signed out)"))
                                                         : "")
                                                textFormat: Text.PlainText
                                                color: AppTheme.stormText
                                                font.pixelSize: AppTheme.textTitle
                                                font.weight: AppTheme.weightBold
                                                elide: Label.ElideRight
                                                Layout.maximumWidth: 300
                                            }
                                            StatusChip {
                                                storm: true
                                                visible: app.backendName === "rust"
                                                label: root.trustStateText(app.sessionTrustState)
                                                iconName: app.sessionTrustState === "Verified"
                                                          ? "verified_user" : ""
                                                tone: app.sessionTrustState === "Verified"
                                                      ? "success"
                                                      : app.sessionTrustState === "Not verified"
                                                        ? "danger" : "neutral"
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing4
                                            Label {
                                                Layout.fillWidth: true
                                                text: app.accounts ? (app.accounts.activeUserId || "") : ""
                                                color: AppTheme.stormTextMuted
                                                font.family: AppTheme.monoFont
                                                font.pixelSize: AppTheme.settingsHintSize
                                                elide: Label.ElideMiddle
                                            }
                                            IconButton {
                                                objectName: "copyMatrixIdButton"
                                                implicitWidth: 24
                                                implicitHeight: 24
                                                radius: AppTheme.radiusControl
                                                iconName: "content_copy"
                                                iconSize: 14
                                                visible: app.accounts
                                                         && app.accounts.activeUserId !== ""
                                                Accessible.name: qsTr("Copy Matrix ID")
                                                ToolTip.text: matrixIdCopied.running
                                                    ? qsTr("Copied") : qsTr("Copy Matrix ID")
                                                ToolTip.visible: hovered
                                                ToolTip.delay: 500
                                                onClicked: {
                                                    root.copyToClipboard(
                                                        app.accounts
                                                        ? app.accounts.activeUserId : "")
                                                    matrixIdCopied.restart()
                                                }
                                                Timer {
                                                    id: matrixIdCopied
                                                    interval: 1500
                                                }
                                            }
                                        }
                                        Label {
                                            visible: app.backendName === "rust" && app.sessionDeviceId !== ""
                                            text: qsTr("Device %1").arg(app.sessionDeviceId)
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.settingsHintSize
                                        }
                                    }
                                }
                                ColumnLayout {
                                    objectName: "ownDisplayNameSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: app.canEditOwnDisplayName
                                    Label {
                                        text: qsTr("Display name")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        visible: !accountIdentityCard.editingDisplayName
                                        Label {
                                            objectName: "ownDisplayNameValue"
                                            Layout.fillWidth: true
                                            elide: Label.ElideRight
                                            text: accountIdentityCard.accountDisplayName.length > 0
                                                  ? accountIdentityCard.accountDisplayName
                                                  : qsTr("Not set")
                                            textFormat: Text.PlainText
                                            color: accountIdentityCard.accountDisplayName.length > 0
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                        }
                                        AppButton {
                                            objectName: "editDisplayNameButton"
                                            storm: true
                                            text: qsTr("Edit")
                                            Accessible.name: qsTr("Edit display name")
                                            onClicked: accountIdentityCard.beginDisplayNameEdit()
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        visible: accountIdentityCard.editingDisplayName
                                        AppTextField {
                                            id: displayNameField
                                            objectName: "ownDisplayNameField"
                                            storm: true
                                            Layout.fillWidth: true
                                            enabled: !app.ownDisplayNameBusy
                                            placeholderText: qsTr("Your display name")
                                            Accessible.name: qsTr("Display name")
                                            onAccepted: accountIdentityCard.commitDisplayName()
                                            Keys.onEscapePressed: accountIdentityCard.cancelDisplayNameEdit()
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: AppTheme.settingsHintSize
                                            readonly property int used:
                                                app.displayNameLength(displayNameField.text.trim())
                                            visible: used > app.ownDisplayNameMaxLength() - 40
                                            color: used > app.ownDisplayNameMaxLength()
                                                   ? AppTheme.stormDanger
                                                   : AppTheme.stormTextMuted
                                            text: qsTr("%1 / %2 characters")
                                                  .arg(used)
                                                  .arg(app.ownDisplayNameMaxLength())
                                        }
                                        Label {
                                            objectName: "ownDisplayNameError"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            visible: app.ownDisplayNameError.length > 0
                                            color: AppTheme.stormDanger
                                            font.pixelSize: AppTheme.settingsHintSize
                                            text: app.ownDisplayNameError
                                            textFormat: Text.PlainText
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            AppButton {
                                                objectName: "saveDisplayNameButton"
                                                storm: true
                                                kind: "primary"
                                                text: app.ownDisplayNameBusy
                                                      ? qsTr("Saving…") : qsTr("Save")
                                                enabled: !app.ownDisplayNameBusy
                                                         && displayNameField.text.trim().length > 0
                                                         && displayNameField.text.trim()
                                                            !== accountIdentityCard.accountDisplayName
                                                         && app.displayNameLength(
                                                                displayNameField.text.trim())
                                                            <= app.ownDisplayNameMaxLength()
                                                onClicked: accountIdentityCard.commitDisplayName()
                                            }
                                            AppButton {
                                                objectName: "cancelDisplayNameButton"
                                                storm: true
                                                text: qsTr("Cancel")
                                                enabled: !app.ownDisplayNameBusy
                                                onClicked: accountIdentityCard.cancelDisplayNameEdit()
                                            }
                                            Item { Layout.fillWidth: true }
                                            AppButton {
                                                objectName: "clearDisplayNameButton"
                                                storm: true
                                                kind: "danger"
                                                text: qsTr("Clear")
                                                visible: accountIdentityCard.accountDisplayName.length > 0
                                                enabled: !app.ownDisplayNameBusy
                                                onClicked: displayNameClearConfirm.visible = true
                                            }
                                        }
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing16
                                    spacing: AppTheme.spacing8
                                    visible: app.nameColors.available
                                             && app.nameColors.supported
                                    Label {
                                        text: qsTr("Name colour")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        objectName: "nameColorHelpText"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        textFormat: Text.PlainText
                                        text: qsTr("Other Lightning users see "
                                            + "this colour on your name. It is "
                                            + "adjusted to stay readable on "
                                            + "whatever theme they use.")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    Flow {
                                        objectName: "nameColorSwatchFlow"
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Repeater {
                                            model: 9
                                            Rectangle {
                                                required property int index
                                                objectName: "nameColorSwatch"
                                                implicitWidth: 30
                                                implicitHeight: 30
                                                radius: 15
                                                readonly property string hex:
                                                    // String(): comparing a color to a hex
                                                    // string with === is never true.
                                                    String(AppTheme.nameInkForSlot(index)).toLowerCase()
                                                color: AppTheme.nameInkForSlot(index)
                                                border.width: app.nameColors.ownColor.toLowerCase()
                                                              === hex ? 3 : 1
                                                border.color: border.width > 1
                                                              ? AppTheme.stormText
                                                              : AppTheme.stormBorderStrong
                                                TapHandler {
                                                    onTapped: app.nameColors.setOwnColor(parent.hex)
                                                }
                                            }
                                        }
                                        Rectangle {
                                            id: nameColorCustomSwatch
                                            objectName: "nameColorCustomSwatch"
                                            implicitWidth: 30
                                            implicitHeight: 30
                                            radius: 15
                                            readonly property bool ownIsCustom: {
                                                const own = app.nameColors.ownColor.toLowerCase()
                                                if (own.length === 0) return false
                                                for (let i = 0; i < 9; ++i)
                                                    if (String(AppTheme.nameInkForSlot(i)).toLowerCase() === own)
                                                        return false
                                                return true
                                            }
                                            color: ownIsCustom ? app.nameColors.ownColor : AppTheme.stormInset
                                            border.width: ownIsCustom ? 3 : 1
                                            border.color: ownIsCustom ? AppTheme.stormText : AppTheme.stormBorderStrong
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "add"
                                                size: 16
                                                color: AppTheme.stormText
                                                visible: !nameColorCustomSwatch.ownIsCustom
                                            }
                                            Accessible.role: Accessible.Button
                                            Accessible.name: qsTr("Custom colour")
                                            ToolTip.text: qsTr("Custom colour…")
                                            ToolTip.visible: customHover.hovered
                                            ToolTip.delay: 600
                                            HoverHandler { id: customHover }
                                            TapHandler {
                                                onTapped: {
                                                    nameColorPicker.draft = app.nameColors.ownColor.length > 0
                                                        ? app.nameColors.ownColor
                                                        : String(AppTheme.nameInkForSlot(0))
                                                    nameColorPicker.selectedColor = nameColorPicker.draft
                                                    nameColorPicker.open = !nameColorPicker.open
                                                }
                                            }
                                        }
                                        Item {
                                            implicitWidth: clearNameColorButton.implicitWidth
                                            implicitHeight: 30
                                            AppButton {
                                                id: clearNameColorButton
                                                objectName: "clearNameColorButton"
                                                anchors.verticalCenter: parent.verticalCenter
                                                storm: true
                                                kind: "ghost"
                                                text: qsTr("Use theme colour")
                                                enabled: !app.nameColors.busy
                                                         && app.nameColors.ownColor.length > 0
                                                onClicked: app.nameColors.setOwnColor("")
                                            }
                                        }
                                    }
                                    ColorPickerPanel {
                                        id: nameColorPicker
                                        objectName: "nameColorPicker"
                                        Component.onCompleted: root.nameColorPickerRef = nameColorPicker
                                        Component.onDestruction: root.nameColorPickerRef = null
                                        property bool open: false
                                        property string draft: ""
                                        Layout.fillWidth: true
                                        visible: open
                                        title: qsTr("Custom name colour")
                                        subtitle: qsTr("Any colour. Other people see it adjusted to stay readable on their theme.")
                                        suggestions: {
                                            const out = []
                                            for (let i = 0; i < 9; ++i)
                                                out.push(String(AppTheme.nameInkForSlot(i)))
                                            return out
                                        }
                                        function toHex(c) {
                                            function two(v) {
                                                const s = Math.round(v * 255).toString(16).toUpperCase()
                                                return s.length < 2 ? "0" + s : s
                                            }
                                            return "#" + two(c.r) + two(c.g) + two(c.b)
                                        }
                                        onPicked: (value) => draft = toHex(value)
                                        onClosed: open = false
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: nameColorPicker.open
                                        spacing: AppTheme.spacing8
                                        Rectangle {
                                            implicitWidth: 30
                                            implicitHeight: 30
                                            radius: 15
                                            color: nameColorPicker.draft.length > 0 ? nameColorPicker.draft : "transparent"
                                            border.width: 1
                                            border.color: AppTheme.stormBorderStrong
                                        }
                                        Label {
                                            objectName: "nameColorDraftLabel"
                                            text: nameColorPicker.draft
                                            color: AppTheme.stormTextSecondary
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.settingsHintSize
                                        }
                                        Item { Layout.fillWidth: true }
                                        AppButton {
                                            objectName: "applyNameColorButton"
                                            storm: true
                                            text: qsTr("Apply")
                                            enabled: !app.nameColors.busy && nameColorPicker.draft.length > 0
                                                     && nameColorPicker.draft.toLowerCase() !== app.nameColors.ownColor.toLowerCase()
                                            onClicked: app.nameColors.setOwnColor(nameColorPicker.draft)
                                        }
                                        AppButton {
                                            storm: true
                                            text: qsTr("Cancel")
                                            onClicked: nameColorPicker.open = false
                                        }
                                    }
                                    Label {
                                        objectName: "nameColorError"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        textFormat: Text.PlainText
                                        visible: app.nameColors.lastError.length > 0
                                        color: AppTheme.stormDanger
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: app.nameColors.lastError
                                              === "unsupported"
                                            ? qsTr("Your homeserver cannot store "
                                                   + "a name colour.")
                                            : qsTr("That colour could not be saved.")
                                    }
                                }

                                ColumnLayout {
                                    objectName: "ownAvatarSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: app.canEditOwnAvatar()

                                    Label {
                                        text: qsTr("Profile picture")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing12
                                        Avatar {
                                            objectName: "ownAvatarPreview"
                                            size: AppTheme.scaled(64)
                                            mxc: accountIdentityCard.accountRecord
                                                 && accountIdentityCard.accountRecord.avatarUrl
                                                 ? accountIdentityCard.accountRecord.avatarUrl : ""
                                            name: accountIdentityCard.accountDisplayName
                                            colorKey: app.accounts
                                                ? (app.accounts.activeUserId || "") : ""
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            RowLayout {
                                                spacing: AppTheme.spacing8
                                                AppButton {
                                                    objectName: "chooseOwnAvatarButton"
                                                    storm: true
                                                    kind: "primary"
                                                    text: app.ownAvatarBusy
                                                        ? qsTr("Uploading…")
                                                        : qsTr("Change picture")
                                                    enabled: !app.ownAvatarBusy
                                                    onClicked: ownAvatarFileDialog.open()
                                                }
                                                AppButton {
                                                    objectName: "removeOwnAvatarButton"
                                                    storm: true
                                                    text: qsTr("Remove")
                                                    visible: !!(accountIdentityCard.accountRecord
                                                             && accountIdentityCard.accountRecord.avatarUrl
                                                             && accountIdentityCard.accountRecord.avatarUrl.length > 0)
                                                    enabled: !app.ownAvatarBusy
                                                    onClicked: app.clearOwnAvatar()
                                                }
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                                lineHeight: AppTheme.lineHeightBody
                                                lineHeightMode: Text.ProportionalHeight
                                                text: qsTr("You choose which part of the image to use.")
                                                color: AppTheme.stormTextMuted
                                                font.pixelSize: AppTheme.settingsHintSize
                                            }
                                        }
                                    }
                                    Label {
                                        objectName: "ownAvatarError"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        visible: app.ownAvatarError.length > 0
                                        text: app.ownAvatarError
                                        color: AppTheme.stormDanger
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    NativeFileDialog {
                                        id: ownAvatarFileDialog
                                        purpose: "image"
                                        currentFolder: app.defaultFileDialogFolder()
                                        title: qsTr("Choose a profile picture")
                                        nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.svg)")]
                                        onAccepted: ownAvatarCrop.openFor(selectedFile)
                                    }
                                    ImageCropDialog {
                                        id: ownAvatarCrop
                                        role: "avatar"
                                        // Pass the URL as-is; stripping "file://"
                                        // breaks Windows paths.
                                        onCropped: function (file) {
                                            app.submitOwnAvatar(file)
                                        }
                                    }
                                }

                                ColumnLayout {
                                    objectName: "ownBioSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: !!(app.bio && app.bio.available)

                                    Label {
                                        text: qsTr("About you")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        text: qsTr("A short note about yourself. Anyone who opens your profile can read it.")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: AppTheme.scaled(96)
                                        clip: true
                                        TextArea {
                                            id: ownBioField
                                            objectName: "ownBioField"
                                            property bool dirty: false
                                            text: app.bio ? app.bio.ownBio : ""
                                            onTextChanged: if (activeFocus) dirty = true
                                            enabled: !!(app.bio && app.bio.supported
                                                     && !app.bio.busy)
                                            wrapMode: TextArea.Wrap
                                            placeholderText: qsTr("Say something about yourself")
                                            placeholderTextColor: AppTheme.placeholderInk
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.scaled(13)
                                            background: Rectangle {
                                                radius: AppTheme.radiusSm
                                                color: AppTheme.stormInset
                                                border.width: 1
                                                border.color: ownBioField.activeFocus
                                                    ? AppTheme.bolt
                                                    : AppTheme.stormBorder
                                            }
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            objectName: "ownBioCounter"
                                            text: qsTr("%1 / %2")
                                                .arg(ownBioField.text.length)
                                                .arg(app.bio ? app.bio.maxLength : 0)
                                            color: (app.bio && ownBioField.text.length > app.bio.maxLength)
                                                ? AppTheme.stormDanger : AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.settingsHintSize
                                        }
                                        Item { Layout.fillWidth: true }
                                        AppButton {
                                            objectName: "saveOwnBioButton"
                                            storm: true
                                            kind: "primary"
                                            text: app.bio && app.bio.busy
                                                ? qsTr("Saving…") : qsTr("Save")
                                            enabled: !!(app.bio && app.bio.supported
                                                     && !app.bio.busy
                                                     && ownBioField.text.length <= app.bio.maxLength)
                                            onClicked: {
                                                app.bio.setOwnBio(ownBioField.text)
                                                ownBioField.dirty = false
                                            }
                                        }
                                        AppButton {
                                            objectName: "clearOwnBioButton"
                                            storm: true
                                            text: qsTr("Clear")
                                            visible: !!(app.bio && app.bio.ownBio.length > 0)
                                            enabled: !!(app.bio && app.bio.supported
                                                     && !app.bio.busy)
                                            onClicked: {
                                                app.bio.clearOwnBio()
                                                ownBioField.dirty = false
                                            }
                                        }
                                    }
                                    Label {
                                        objectName: "ownBioUnsupportedNote"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        visible: !!(app.bio && !app.bio.supported)
                                        text: qsTr("Your homeserver does not support profile bios yet.")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                    Label {
                                        objectName: "ownBioError"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        visible: !!(app.bio && app.bio.lastError.length > 0)
                                        text: app.bio ? app.bio.lastError : ""
                                        color: AppTheme.stormDanger
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                }
                                AppButton {
                                    storm: true
                                    text: qsTr("Open Privacy & security")
                                    onClicked: root.section = "privacy"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("To sign out, use the account menu at the "
                                               + "bottom of the sidebar.")
                                }
                            }
                        }
                        SettingsCard {
                            id: profileBannerCard
                            visible: app.banners && app.banners.available
                            readonly property bool serverSupports:
                                app.banners && app.banners.supported
                            readonly property string ownUserId:
                                app.accounts ? app.accounts.activeUserId : ""
                            readonly property bool shown: root.visible
                            onShownChanged: if (shown) profileBannerCard.ask()
                            Component.onCompleted: if (root.visible) profileBannerCard.ask()
                            onOwnUserIdChanged: profileBannerCard.ask()
                            function ask() {
                                if (app.banners && app.banners.available
                                        && profileBannerCard.ownUserId !== "")
                                    app.banners.request(profileBannerCard.ownUserId)
                            }
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Profile banner")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                Rectangle {
                                    objectName: "ownProfileBannerPreview"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.round(
                                        Math.max(60, width / 3))
                                    radius: AppTheme.radiusMd
                                    color: AppTheme.stormInset
                                    border.width: 1
                                    border.color: AppTheme.stormBorder
                                    clip: true
                                    Image {
                                        id: ownBannerImage
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        visible: status === Image.Ready
                                        opacity: ownBannerMotion.shown ? 0 : 1
                                        readonly property string mxc: {
                                            if (!app.banners)
                                                return ""
                                            var _dep = app.banners.revision
                                            return app.banners.ownBanner
                                        }
                                        // A counter, never an assignment to
                                        // `source`, which would destroy the
                                        // binding.
                                        property int resolveTick: 0
                                        source: {
                                            var _tick = resolveTick
                                            return mxc.length > 0
                                                && app.mediaBridge.supported
                                                ? app.mediaBridge.wideImageSource(mxc)
                                                : ""
                                        }
                                        Component.onCompleted: {
                                            if (mxc.length > 0
                                                    && source.toString().length === 0)
                                                resolveTick++
                                        }
                                        Connections {
                                            target: app.mediaBridge
                                            enabled: ownBannerImage.mxc.length > 0
                                            function onMediaCached(key) {
                                                if (key.endsWith(":" + ownBannerImage.mxc)
                                                    && ownBannerImage.source.toString().length === 0)
                                                    ownBannerImage.resolveTick++
                                            }
                                            function onMediaRetryable(key) {
                                                if (key.endsWith(":" + ownBannerImage.mxc)
                                                    && ownBannerImage.source.toString().length === 0)
                                                    ownBannerImage.resolveTick++
                                            }
                                        }
                                    }
                                    BannerMotion {
                                        id: ownBannerMotion
                                        objectName: "ownProfileBannerMotion"
                                        anchors.fill: parent
                                        mxc: ownBannerImage.mxc
                                        stillReady: ownBannerImage.status === Image.Ready
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible: !ownBannerImage.visible
                                        text: qsTr("No banner")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                    }
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "chooseProfileBannerButton"
                                        storm: true
                                        text: qsTr("Choose image…")
                                        enabled: !app.banners.busy
                                                 && profileBannerCard.serverSupports
                                        onClicked: bannerFileDialog.open()
                                    }
                                    AppButton {
                                        objectName: "removeProfileBannerButton"
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Remove")
                                        visible: app.banners.ownBanner.length > 0
                                        enabled: !app.banners.busy
                                                 && profileBannerCard.serverSupports
                                        onClicked: app.banners.clearOwnBanner()
                                    }
                                }
                                Label {
                                    objectName: "profileBannerError"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: {
                                        if (!profileBannerCard.serverSupports)
                                            return ""
                                        var e = app.banners.lastError
                                        if (e.length === 0)
                                            return ""
                                        if (e === "unsupported_image")
                                            return qsTr("That file is not an image Lightning can "
                                                        + "upload. PNG, JPEG, GIF, WebP and BMP "
                                                        + "work; the file's contents decide, not "
                                                        + "its name.")
                                        if (e === "forbidden")
                                            return qsTr("Your homeserver refused the banner.")
                                        return qsTr("The banner could not be saved (%1).").arg(e)
                                    }
                                }
                                Label {
                                    objectName: "profileBannerUnsupported"
                                    Layout.fillWidth: true
                                    visible: !profileBannerCard.serverSupports
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Your homeserver does not support profile banners "
                                               + "yet. They need extended profile fields "
                                               + "(MSC4133), which most servers have not enabled. "
                                               + "Nothing is wrong with your image.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("A wide image shown behind your profile card, "
                                               + "about 3:1. It is part of your public profile, "
                                               + "so anyone who can see your account can see it. "
                                               + "Saved under both the standard and the Commet "
                                               + "field names, so clients that already show "
                                               + "banners will show yours.")
                                }
                                NativeFileDialog {
                                    id: bannerFileDialog
                                    purpose: "image"
                                    currentFolder: app.defaultFileDialogFolder()
                                    title: qsTr("Choose a banner image")
                                    nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.svg)")]
                                    onAccepted: ownBannerCrop.openFor(
                                        selectedFile)
                                }
                                ImageCropDialog {
                                    id: ownBannerCrop
                                    role: "banner"
                                    onCropped: function (file) {
                                        app.banners.setOwnBanner(
                                            file.toString())
                                    }
                                }
                            }
                        }

                        SettingsCard {
                            objectName: "changePasswordCard"
                            visible: !!app.passwordChange
                                     && app.passwordChange.available
                            ChangePasswordSection {
                                width: parent.width
                            }
                        }

                        SettingsCard {
                            objectName: "networkRouteCard"
                            visible: !!app.networkRoute && app.networkRoute.available
                            NetworkRouteSettings {
                                width: parent.width
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Homeserver")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                AppTextField {
                                    storm: true
                                    objectName: "homeserverField"
                                    Layout.fillWidth: true
                                    text: app.settings.homeserverUrl
                                    placeholderText: "https://matrix.org"
                                    onEditingFinished: app.settings.homeserverUrl = text
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Changing the homeserver takes effect at the next sign-in.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Startup")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    font.weight: AppTheme.weightStrong
                                }
                                SettingsCheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "startMinimizedCheck"
                                    text: qsTr("Start minimized")
                                    checked: app.settings.startMinimized
                                    onToggled: app.settings.startMinimized = checked
                                }
                            }
                        }

                        SettingsBackupSection {
                            Layout.fillWidth: true
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("privacy")
                        visible: root.section === "privacy"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            objectName: "privacyStorageFactsCard"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: app.settings.secretsAreSecure
                                    color: AppTheme.stormSuccess
                                    text: qsTr("Access tokens are stored in the system credential store: %1. Logout clears them.")
                                              .arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: !app.settings.secretsAreSecure
                                    color: AppTheme.stormDanger
                                    text: app.settings.secretsArePortable
                                          ? qsTr("Portable mode: access tokens are encrypted inside this portable folder, with the key stored beside them, so anyone who copies the folder can use your session. Keep the folder private.")
                                          : (Qt.platform.os === "osx"
                                           || Qt.platform.os === "windows")
                                          ? qsTr("Insecure fallback active: access tokens are stored in the application's settings file (plaintext) because the system credential store could not be used. Restart to try again.")
                                          : qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Crypto backend: %1").arg(app.crypto.backendDescription)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: app.crypto.supportsE2ee ? AppTheme.stormSuccess : AppTheme.stormTextMuted
                                    text: qsTr("E2EE status: %1").arg(app.crypto.statusString)
                                }
                            }
                        }

                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Security status")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "cryptoHealthRefresh"
                                        text: qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.underline: true
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.refreshCryptoHealth()
                                        }
                                    }
                                }
                                Label {
                                    objectName: "cryptoHealthSummary"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormText
                                    text: app.cryptoHealth.statusSummary
                                }
                                RowLayout {
                                    objectName: "cryptoBootstrapStatus"
                                    visible: app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppBusyIndicator {
                                        size: 14
                                        color: AppTheme.bolt
                                        visible: running
                                        running: app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.WaitingForKeys
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.SecretsPending
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.SecretReceived
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.RestoringHistory
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: app.cryptoBootstrap.phase
                                                   === CryptoBootstrapModel.Ready
                                               ? AppTheme.stormSuccess
                                               : app.cryptoBootstrap.needsRecoveryKey
                                                 ? AppTheme.bolt
                                                 : AppTheme.stormText
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        text: app.cryptoBootstrap.statusMessage
                                        textFormat: Text.PlainText
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: text
                                    }
                                }
                                AppButton {
                                    storm: true
                                    objectName: "requestKeysAgain"
                                    visible: app.cryptoBootstrap.canRequestKeys
                                    enabled: app.loggedIn
                                    text: qsTr("Request keys again")
                                    Accessible.name: text
                                    onClicked: app.requestEncryptionKeys()
                                }
                                readonly property bool verifyAgainOffered:
                                    app.cryptoBootstrap.phase
                                        === CryptoBootstrapModel.IdentityIncomplete
                                    || app.cryptoBootstrap.phase
                                        === CryptoBootstrapModel.ManualRecoveryRequired
                                AppButton {
                                    storm: true
                                    objectName: "verifyAgainForKeys"
                                    visible: parent.verifyAgainOffered
                                    enabled: app.loggedIn
                                             && (!app.verificationActive
                                                 || app.verificationState === "done"
                                                 || app.verificationState === "cancelled"
                                                 || app.verificationState.indexOf("failed") === 0)
                                    text: app.cryptoBootstrap.phase
                                              === CryptoBootstrapModel.IdentityIncomplete
                                          ? qsTr("Verify this session again")
                                          : qsTr("Verify another session to request keys")
                                    Accessible.name: text
                                    onClicked: {
                                        root.section = "sessions"
                                        app.startOwnVerification()
                                    }
                                }
                                AppButton {
                                    storm: true
                                    objectName: "verifyAgainWithRecoveryKey"
                                    visible: parent.verifyAgainOffered
                                             && app.sessionTrustState !== "Verified"
                                    enabled: app.loggedIn
                                    text: qsTr("Use recovery key instead")
                                    Accessible.name: text
                                    onClicked: healthRecoveryEntry.visible
                                               = !healthRecoveryEntry.visible
                                }
                                RecoveryKeyEntry {
                                    id: healthRecoveryEntry
                                    objectName: "healthRecoveryEntry"
                                    Layout.fillWidth: true
                                    visible: false
                                    fieldObjectName: "healthRecoveryField"
                                    buttonText: qsTr("Use recovery key")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: {
                                        var device = app.cryptoHealth.currentDeviceVerified
                                        var deviceText = device === CryptoHealthModel.Yes
                                            ? qsTr("Yes")
                                            : device === CryptoHealthModel.No
                                              ? qsTr("No") : qsTr("Unknown")
                                        return qsTr("Current session verified: %1").arg(deviceText)
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.crossSigningReady
                                          ? qsTr("Cross-signing: ready")
                                          : app.cryptoHealth.crossSigningAvailable
                                            ? qsTr("Cross-signing: not complete on this session")
                                            : qsTr("Cross-signing: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.keyBackupUsable
                                          ? qsTr("Key backup: active on this session")
                                          : app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.Yes
                                            ? qsTr("Key backup: exists, but this session cannot use it yet")
                                            : app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.No
                                              ? qsTr("Key backup: none found")
                                              : qsTr("Key backup: checking…")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.recoveryAvailable
                                          ? qsTr("Recovery: set up")
                                          : app.cryptoHealth.recoveryRequired
                                            ? qsTr("Recovery: set up, but secrets are missing here")
                                            : qsTr("Recovery: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.cryptoSyncing
                                          ? qsTr("Encryption sync: active")
                                          : app.cryptoHealth.cryptoReady
                                            ? qsTr("Encryption sync: ready")
                                            : qsTr("Encryption sync: waiting")
                                }
                                AbstractButton {
                                    id: recoveryDiagnosticsToggle
                                    objectName: "recoveryDiagnosticsToggle"
                                    visible: app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    implicitHeight: 28
                                    hoverEnabled: true
                                    focusPolicy: Qt.TabFocus
                                    Accessible.role: Accessible.Button
                                    Accessible.name: root.showRecoveryDiagnostics
                                        ? qsTr("Hide recovery diagnostics")
                                        : qsTr("Recovery diagnostics")
                                    onClicked: root.showRecoveryDiagnostics
                                        = !root.showRecoveryDiagnostics
                                    contentItem: RowLayout {
                                        spacing: AppTheme.spacing4
                                        Label {
                                            text: root.showRecoveryDiagnostics
                                                  ? qsTr("Hide recovery diagnostics")
                                                  : qsTr("Recovery diagnostics")
                                            color: AppTheme.stormLink
                                            font.pixelSize: AppTheme.settingsOptionSize
                                        }
                                        Icon {
                                            name: "expand_more"
                                            size: 16
                                            color: AppTheme.stormLink
                                            rotation: root.showRecoveryDiagnostics ? 180 : 0
                                            Behavior on rotation {
                                                enabled: !AppTheme.reducedMotion
                                                NumberAnimation { duration: 120 }
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    background: Rectangle {
                                        radius: AppTheme.radiusMd
                                        color: (recoveryDiagnosticsToggle.hovered
                                                || recoveryDiagnosticsToggle.down)
                                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                               : "transparent"
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        radius: AppTheme.radiusMd + 4
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                        visible: recoveryDiagnosticsToggle.visualFocus
                                    }
                                }
                                ColumnLayout {
                                    objectName: "recoveryDiagnostics"
                                    visible: root.showRecoveryDiagnostics
                                             && app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing4
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        text: qsTr("Own identity: %1").arg(
                                            app.cryptoBootstrap.ownIdentity === "verified"
                                                ? qsTr("verified")
                                                : app.cryptoBootstrap.ownIdentity === "unverified"
                                                  ? qsTr("not verified on this session")
                                                  : qsTr("not checked yet"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        text: qsTr("Cross-signing private keys: %1").arg(
                                            app.cryptoBootstrap.crossSigningSecrets === "complete"
                                                ? qsTr("present")
                                                : app.cryptoBootstrap.crossSigningSecrets === "incomplete"
                                                  ? qsTr("missing on this session")
                                                  : qsTr("not checked yet"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        text: {
                                            var s = app.cryptoBootstrap.requestState
                                            var label = s === "requested"
                                                ? qsTr("sent")
                                                : s === "already_pending"
                                                  ? qsTr("pending")
                                                  : s === "none_missing"
                                                    ? qsTr("nothing missing")
                                                    : s === "identity_unverified"
                                                      ? qsTr("blocked — identity not verified")
                                                      : s === "no_eligible_devices"
                                                        ? qsTr("no verified session to ask")
                                                        : s === "unavailable"
                                                          ? qsTr("could not be created")
                                                          : qsTr("not sent yet")
                                            return qsTr("Secret request: %1 (%n attempt(s))",
                                                        "",
                                                        app.cryptoBootstrap.requestAttempts)
                                                .arg(label)
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        visible: app.cryptoBootstrap.requestAttempts > 0
                                        text: qsTr("Verified sessions available: %1")
                                            .arg(app.cryptoBootstrap.eligibleDevices)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        text: qsTr("Backup key usable: %1").arg(
                                            app.cryptoHealth.keyBackupUsable
                                                ? qsTr("yes") : qsTr("no"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        visible: app.cryptoBootstrap.keysReceived > 0
                                        text: qsTr("Room keys imported: %1")
                                            .arg(app.cryptoBootstrap.keysReceived)
                                    }
                                }
                            }
                        }

                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("sessions")
                        visible: root.section === "sessions"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        onVisibleChanged: {
                            if (visible && app.backendName === "rust")
                                app.refreshSessionDevices()
                        }

                        Label {
                            text: qsTr("Sessions")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            objectName: "sessionsHeading"
                            Layout.fillWidth: true
                            text: qsTr("This account's Matrix sessions and device "
                                       + "verification.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsCard {
                            visible: app.qrLogin && app.qrLogin.available
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                MenuSectionLabel {
                                    text: qsTr("Sign in another device")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Use a code instead of a "
                                               + "password. The new device is "
                                               + "signed in and verified in "
                                               + "one step, so it can read "
                                               + "your existing encrypted "
                                               + "conversations immediately.")
                                }
                                AppButton {
                                    objectName: "qrLoginOpenButton"
                                    text: qsTr("Sign in another device…")
                                    onClicked: qrLoginDialog.openDialog()
                                }
                            }
                        }

                        Rectangle {
                            objectName: "crossSigningDetailCard"
                            Layout.fillWidth: true
                            visible: app.cryptoHealth
                                     && app.cryptoHealth.cryptoSupported
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: crossSigningCol.implicitHeight
                                            + AppTheme.spacing16 * 2
                            ColumnLayout {
                                id: crossSigningCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing6
                                MenuSectionLabel { text: qsTr("Cross-signing") }
                                Repeater {
                                    model: [
                                        { label: qsTr("Master key"),
                                          ok: app.cryptoHealth.hasMasterKey },
                                        { label: qsTr("Self-signing key"),
                                          ok: app.cryptoHealth.hasSelfSigningKey },
                                        { label: qsTr("User-signing key"),
                                          ok: app.cryptoHealth.hasUserSigningKey },
                                        { label: qsTr("This session verified"),
                                          ok: app.cryptoHealth.currentDeviceVerified
                                              === CryptoHealthModel.Yes },
                                        { label: qsTr("Secrets recoverable (secret storage)"),
                                          ok: app.cryptoHealth.secretStorageAvailable }
                                    ]
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.label
                                            textFormat: Text.PlainText
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.settingsOptionSize
                                        }
                                        StatusChip {
                                            storm: true
                                            label: modelData.ok ? qsTr("Available")
                                                                : qsTr("Missing")
                                            tone: modelData.ok ? "success" : "neutral"
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: backupCard
                            objectName: "backupManagementCard"
                            Component.onDestruction: root.backupCardRef = null
                            Layout.fillWidth: true
                            visible: app.cryptoHealth
                                     && app.cryptoHealth.cryptoSupported
                                     && app.backup
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: backupCol.implicitHeight
                                            + AppTheme.spacing16 * 2
                            property string pendingConfirm: ""
                            readonly property bool shown: root.visible
                            onShownChanged: if (shown && app.backup) app.backup.requestProgress()
                            Component.onCompleted: {
                                root.backupCardRef = backupCard
                                if (root.visible && app.backup)
                                    app.backup.requestProgress()
                            }
                            ColumnLayout {
                                id: backupCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing8
                                MenuSectionLabel { text: qsTr("Key backup") }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: {
                                        var h = app.cryptoHealth
                                        if (h.keyBackupAvailable === CryptoHealthModel.Unknown)
                                            return qsTr("Lightning could not check whether this "
                                                        + "account has a key backup. Nothing has "
                                                        + "been ruled out.")
                                        if (h.keyBackupAvailable === CryptoHealthModel.No)
                                            return qsTr("No key backup exists for this account.")
                                        if (h.keyBackupUsable)
                                            return qsTr("Backup active — this session can use it (%1).")
                                                       .arg(root.backupStateText(h.keyBackupState))
                                        return qsTr("A backup exists but this session cannot use it yet (%1). Restore with your recovery key first.")
                                                   .arg(root.backupStateText(h.keyBackupState))
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: app.backup.uploadState.length > 0
                                             && app.backup.uploadState !== "unknown"
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: app.backup.uploadState === "uploading"
                                          ? qsTr("Uploading room keys: %1 of %2")
                                                .arg(app.backup.backedUp).arg(app.backup.total)
                                          : qsTr("Upload: %1 · backup: %2")
                                                .arg(root.backupStateText(app.backup.uploadState))
                                                .arg(root.backupStateText(app.backup.backupState))
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: app.backup.error.length > 0
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: app.backup.error
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: app.backup.recoveryKey.length > 0
                                    radius: AppTheme.radiusTile
                                    color: AppTheme.stormCanvas
                                    border.color: AppTheme.stormBorderStrong
                                    border.width: 1
                                    implicitHeight: keyCol.implicitHeight + AppTheme.spacing12 * 2
                                    ColumnLayout {
                                        id: keyCol
                                        anchors.fill: parent
                                        anchors.margins: AppTheme.spacing12
                                        spacing: AppTheme.spacing6
                                        Label {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            font.weight: AppTheme.weightStrong
                                            text: qsTr("Your new recovery key. Write it down now: Lightning does not keep it, and it will not be shown again once you leave this card.")
                                        }
                                        TextEdit {
                                            objectName: "backupRecoveryKeyText"
                                            Layout.fillWidth: true
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: TextEdit.Wrap
                                            color: AppTheme.stormText
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            text: app.backup.recoveryKey
                                        }
                                        RowLayout {
                                            Item { Layout.fillWidth: true }
                                            AppButton {
                                                storm: true
                                                kind: "primary"
                                                size: "sm"
                                                text: qsTr("I have saved it")
                                                onClicked: app.backup.dismissRecoveryKey()
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: backupCard.pendingConfirm.length > 0
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: backupCard.pendingConfirm === "reset_key"
                                          ? qsTr("Resetting creates a NEW recovery key and the old one stops working — anything currently recoverable only with the old key is gone. Press again to confirm.")
                                          : backupCard.pendingConfirm === "disable_and_delete"
                                            ? qsTr("Deleting the backup removes every room key stored on the server. Keys only in this session stay here; keys only in the backup are gone. Press again to confirm.")
                                            : qsTr("Disabling recovery removes secret storage and the backup from the server. Press again to confirm.")
                                }
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "backupEnableButton"
                                        visible: app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.No
                                                 && !app.cryptoHealth.secretStorageAvailable
                                        storm: true
                                        kind: "primary"
                                        size: "sm"
                                        enabled: !app.backup.busy
                                        // Recovery created while the account has no
                                        // cross-signing identity would leave no
                                        // identity keys in it, so a recovery key
                                        // could never verify a new session. That
                                        // case goes through the cross-signing
                                        // setup first (it also creates the
                                        // recovery key); its password prompt
                                        // lives on that page.
                                        readonly property bool needsCrossSigning:
                                            app.cryptoHealth.crossSigningSetup === "not_set_up"
                                            || app.cryptoHealth.crossSigningSetup === "unconfirmed"
                                        text: app.backup.busy && app.backup.lastAction === "enable"
                                              ? qsTr("Setting up…")
                                              : needsCrossSigning
                                                ? qsTr("Set up cross-signing and recovery…")
                                                : qsTr("Set up recovery and backup")
                                        onClicked: {
                                            if (needsCrossSigning) {
                                                root.section = "privacy"
                                                Qt.callLater(function () {
                                                    root.revealSearchResult({
                                                        section: "privacy",
                                                        anchor: "crossSigningSetupCard" })
                                                })
                                            }
                                            else
                                                app.backup.runAction("enable")
                                        }
                                    }
                                    AppButton {
                                        objectName: "backupCreateButton"
                                        visible: app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.No
                                                 && app.cryptoHealth.secretStorageAvailable
                                        storm: true
                                        kind: "primary"
                                        size: "sm"
                                        enabled: !app.backup.busy
                                        text: qsTr("Create backup")
                                        onClicked: app.backup.runAction("create_backup")
                                    }
                                    AppButton {
                                        objectName: "backupResetKeyButton"
                                        visible: app.cryptoHealth.secretStorageAvailable
                                                 && app.cryptoHealth.crossSigningReady
                                                 && app.cryptoHealth.currentDeviceVerified
                                                    === CryptoHealthModel.Yes
                                        storm: true
                                        kind: backupCard.pendingConfirm === "reset_key" ? "danger" : "secondary"
                                        size: "sm"
                                        enabled: !app.backup.busy
                                        text: qsTr("New recovery key")
                                        onClicked: {
                                            if (backupCard.pendingConfirm === "reset_key") {
                                                backupCard.pendingConfirm = ""
                                                app.backup.runAction("reset_key")
                                            } else {
                                                backupCard.pendingConfirm = "reset_key"
                                            }
                                        }
                                    }
                                    AppButton {
                                        objectName: "backupDeleteButton"
                                        visible: app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.Yes
                                        storm: true
                                        kind: "danger"
                                        size: "sm"
                                        enabled: !app.backup.busy
                                        text: qsTr("Delete backup")
                                        onClicked: {
                                            if (backupCard.pendingConfirm === "disable_and_delete") {
                                                backupCard.pendingConfirm = ""
                                                app.backup.runAction("disable_and_delete")
                                            } else {
                                                backupCard.pendingConfirm = "disable_and_delete"
                                            }
                                        }
                                    }
                                    AppButton {
                                        visible: backupCard.pendingConfirm.length > 0
                                        storm: true
                                        kind: "ghost"
                                        size: "sm"
                                        text: qsTr("Cancel")
                                        onClicked: backupCard.pendingConfirm = ""
                                    }
                                }
                            }
                        }

                        TrustCard {
                            id: sessionsTrustCard
                            objectName: "sessionsTrustCard"
                            Layout.fillWidth: true
                            visible: app.cryptoHealth
                                     && app.cryptoHealth.cryptoSupported
                            property var accountRecord: ({})
                            function refreshAccountRecord() {
                                accountRecord =
                                    (app.accounts && app.accounts.activeUserId)
                                    ? app.accounts.account(app.accounts.activeUserId)
                                    : ({})
                            }
                            Component.onCompleted: refreshAccountRecord()
                            Connections {
                                target: app.accounts
                                function onAccountsChanged() {
                                    sessionsTrustCard.refreshAccountRecord()
                                }
                                function onActiveUserIdChanged() {
                                    sessionsTrustCard.refreshAccountRecord()
                                }
                            }
                            readonly property bool devicesVerified:
                                app.sessionDevicesFailed
                                || app.sessionDevicesLoading
                                ? false
                                : app.sessionDevices.length > 0
                                  // `trusted` is the SDK's own answer per row
                                  // (crossSigned for this session, whose
                                  // is_verified() is a constant true; verified
                                  // for the others, whose signature alone is
                                  // not trust).
                                  ? app.sessionDevices.every(
                                        d => d.trusted === true)
                                  : app.cryptoHealth.currentDeviceVerified
                                    === CryptoHealthModel.Yes
                            readonly property var chainSteps: [
                                { label: qsTr("IDENTITY"), iconName: "person",
                                  complete: app.cryptoHealth.ownIdentityVerified
                                            === CryptoHealthModel.Yes },
                                { label: qsTr("%1 DEVICES")
                                        .arg(app.sessionDevices.length),
                                  iconName: "devices",
                                  complete: devicesVerified },
                                { label: qsTr("CROSS-SIGN"), iconName: "key",
                                  complete: app.cryptoHealth.crossSigningReady
                                            === true }
                            ]
                            displayName: accountRecord
                                         && accountRecord.displayName
                                         ? accountRecord.displayName
                                         : (app.accounts
                                            ? app.accounts.activeUserId : "")
                            userId: app.accounts ? app.accounts.activeUserId
                                                 : ""
                            avatarMxc: accountRecord && accountRecord.avatarUrl
                                       ? accountRecord.avatarUrl : ""
                            steps: chainSteps
                            statusText: {
                                var complete = 0
                                for (var i = 0; i < chainSteps.length; ++i) {
                                    if (chainSteps[i].complete)
                                        ++complete
                                }
                                return qsTr("%1 of 3 checks complete")
                                       .arg(complete)
                            }
                            readonly property bool identityAvailable:
                                app.cryptoHealth.crossSigningSetup !== "not_set_up"
                                && app.cryptoHealth.crossSigningSetup !== "unconfirmed"
                            showVerify: !app.verificationActive
                                        && app.verificationState === ""
                                        && app.sessionTrustState !== "Verified"
                                        && identityAvailable
                            showRecoveryOption: app.backendName === "rust"
                                                && app.sessionTrustState !== "Verified"
                                                && identityAvailable
                            onVerifyRequested: app.startOwnVerification()
                            onRecoveryRequested: sessionsRecoveryCard.open
                                                 = !sessionsRecoveryCard.open
                        }
                        SettingsCard {
                            id: sessionsRecoveryCard
                            objectName: "sessionsRecoveryCard"
                            property bool open: false
                            visible: open
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Enter your recovery key or passphrase. It "
                                               + "restores your message keys, and if "
                                               + "your account's cross-signing keys are "
                                               + "stored with it, this session becomes "
                                               + "verified without another device.")
                                }
                                RecoveryKeyEntry {
                                    id: sessionsRecoveryEntry
                                    objectName: "sessionsRecoveryEntry"
                                    Layout.fillWidth: true
                                    fieldObjectName: "sessionsRecoveryField"
                                    buttonText: qsTr("Use recovery key")
                                }
                            }
                        }

                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                id: sessionsListCard
                                width: parent.width
                                spacing: AppTheme.spacing8

                                property string actionNotice: ""
                                property bool actionNoticeError: false
                                function currentDeviceId() {
                                    for (var i = 0; i < app.sessionDevices.length; ++i) {
                                        if (app.sessionDevices[i].isCurrent === true)
                                            return app.sessionDevices[i].deviceId
                                    }
                                    return ""
                                }
                                function signOutOne(deviceId) {
                                    actionNotice = ""
                                    if (app.activeAccountIsOAuth())
                                        app.uia.requestManagementUrl(deviceId)
                                    else
                                        app.uia.signOutDevices(
                                            [deviceId], currentDeviceId())
                                }
                                function signOutAllOthers() {
                                    actionNotice = ""
                                    if (app.activeAccountIsOAuth()) {
                                        app.uia.requestManagementUrl("")
                                        return
                                    }
                                    var ids = []
                                    for (var i = 0; i < app.sessionDevices.length; ++i) {
                                        var d = app.sessionDevices[i]
                                        if (d.isCurrent !== true)
                                            ids.push(d.deviceId)
                                    }
                                    app.uia.signOutDevices(ids, currentDeviceId())
                                }
                                Connections {
                                    target: app.uia
                                    function onSignOutFinished(ok, message) {
                                        sessionsListCard.actionNotice = message
                                        sessionsListCard.actionNoticeError = !ok
                                    }
                                    function onManagementUrlReady(url) {
                                        app.media.openWebUrl(url)
                                        sessionsListCard.actionNotice = qsTr(
                                            "Manage this in the account "
                                            + "page that just opened, then "
                                            + "press Refresh here.")
                                        sessionsListCard.actionNoticeError = false
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    RowLayout {
                                        spacing: AppTheme.spacing6
                                        Icon {
                                            name: "devices"
                                            size: 13
                                            color: AppTheme.stormTextFaint
                                        }
                                        MenuSectionLabel { text: qsTr("Sessions") }
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "sessionDevicesRefresh"
                                        text: app.sessionDevicesLoading
                                              ? qsTr("Loading…") : qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        font.underline: !app.sessionDevicesLoading
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            enabled: !app.sessionDevicesLoading
                                            onClicked: app.refreshSessionDevices()
                                        }
                                    }
                                }
                                Label {
                                    visible: app.sessionDevicesFailed
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: qsTr("The session list could not be loaded.")
                                }
                                Label {
                                    visible: !app.sessionDevicesFailed
                                             && !app.sessionDevicesLoading
                                             && app.sessionDevices.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: qsTr("Press Refresh to load this account's sessions.")
                                }
                                SegmentedControl {
                                    id: sessionFilter
                                    objectName: "sessionFilter"
                                    storm: true
                                    dense: true
                                    model: [
                                        { value: "all", label: qsTr("All") },
                                        { value: "current", label: qsTr("Current") },
                                        { value: "verified", label: qsTr("Verified") },
                                        { value: "unverified", label: qsTr("Unverified") }
                                    ]
                                    current: "all"
                                    onActivated: (value) => current = value
                                }
                                Label {
                                    visible: app.sessionDeviceRenameError.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: app.sessionDeviceRenameError
                                }
                                Repeater {
                                    model: {
                                        var all = app.sessionDevices
                                        var f = sessionFilter.current
                                        if (f === "all")
                                            return all
                                        var out = []
                                        for (var i = 0; i < all.length; ++i) {
                                            var d = all[i]
                                            if (f === "current" && d.isCurrent === true)
                                                out.push(d)
                                            else if (f === "verified" && d.trusted === true)
                                                out.push(d)
                                            else if (f === "unverified"
                                                     && d.hasCryptoIdentity === true
                                                     && d.trusted !== true)
                                                out.push(d)
                                        }
                                        return out
                                    }
                                    delegate: Rectangle {
                                        Layout.fillWidth: true
                                        radius: AppTheme.radiusTile
                                        color: AppTheme.stormInset
                                        border.width: 1
                                        border.color: modelData.isCurrent === true
                                                      ? AppTheme.stormBorderStrong
                                                      : AppTheme.stormBorder
                                        implicitHeight: sessionTileRow.implicitHeight
                                                        + AppTheme.spacing8 * 2

                                        RowLayout {
                                            id: sessionTileRow
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: AppTheme.spacing8 + 2
                                            anchors.rightMargin: AppTheme.spacing8 + 2
                                            spacing: AppTheme.spacing8 + 2

                                            Rectangle {
                                                implicitWidth: 30
                                                implicitHeight: 30
                                                radius: AppTheme.radiusTile
                                                color: AppTheme.stormCanvas
                                                border.width: 1
                                                border.color: AppTheme.stormBorder
                                                Icon {
                                                    anchors.centerIn: parent
                                                    name: "devices"
                                                    size: 16
                                                    color: modelData.isCurrent === true
                                                           ? AppTheme.bolt
                                                           : AppTheme.stormTextSecondary
                                                }
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: AppTheme.spacing6
                                                    Label {
                                                        text: (modelData.displayName
                                                               && modelData.displayName.length > 0)
                                                              ? modelData.displayName
                                                              : modelData.deviceId
                                                        textFormat: Text.PlainText
                                                        color: AppTheme.stormText
                                                        font.pixelSize: AppTheme.settingsOptionSize
                                                        font.weight: AppTheme.weightStrong
                                                        elide: Label.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                    StatusChip {
                                                        storm: true
                                                        visible: modelData.isCurrent === true
                                                        label: qsTr("This session")
                                                        tone: "accent"
                                                    }
                                                    StatusChip {
                                                        storm: true
                                                        label: modelData.trusted === true
                                                              ? qsTr("Verified")
                                                              : modelData.hasCryptoIdentity === true
                                                                ? qsTr("Not verified")
                                                                : qsTr("No encryption")
                                                        iconName: modelData.trusted === true
                                                                  ? "verified_user" : ""
                                                        tone: modelData.trusted === true
                                                              ? "success" : "neutral"
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.settingsHintSize
                                                    elide: Label.ElideRight
                                                    text: {
                                                        var parts = [ modelData.deviceId ]
                                                        if (modelData.lastSeen
                                                            && !isNaN(modelData.lastSeen.getTime()))
                                                            parts.push(qsTr("last seen %1").arg(
                                                                Qt.locale(AppLocale.name).toString(modelData.lastSeen,
                                                                                  "d MMM yyyy hh:mm")))
                                                        if (modelData.lastSeenIp
                                                            && modelData.lastSeenIp.length > 0)
                                                            parts.push(modelData.lastSeenIp)
                                                        return parts.join(" · ")
                                                    }
                                                }
                                            }

                                            AppButton {
                                                objectName: "sessionRenameButton_"
                                                            + modelData.deviceId
                                                visible: !renameField.visible
                                                storm: true
                                                kind: "ghost"
                                                size: "sm"
                                                enabled: !app.sessionDeviceRenaming
                                                text: qsTr("Rename")
                                                Accessible.name:
                                                    qsTr("Rename session %1")
                                                        .arg(modelData.deviceId)
                                                onClicked: {
                                                    renameField.text =
                                                        modelData.displayName || ""
                                                    renameField.visible = true
                                                    renameField.forceActiveFocus()
                                                }
                                            }
                                            AppTextField {
                                                id: renameField
                                                objectName: "sessionRenameField_"
                                                            + modelData.deviceId
                                                visible: false
                                                storm: true
                                                Layout.preferredWidth: 160
                                                placeholderText: qsTr("Session name")
                                                onAccepted: {
                                                    if (text.trim().length > 0)
                                                        app.renameSessionDevice(
                                                            modelData.deviceId, text)
                                                    visible = false
                                                }
                                                onVisibleChanged: {
                                                    if (visible)
                                                        root.renamingSessionDeviceId = modelData.deviceId
                                                    else if (root.renamingSessionDeviceId === modelData.deviceId)
                                                        root.renamingSessionDeviceId = ""
                                                }
                                                Component.onDestruction: {
                                                    if (root.renamingSessionDeviceId === modelData.deviceId)
                                                        root.renamingSessionDeviceId = ""
                                                }
                                                Keys.onEscapePressed: function(event) {
                                                    event.accepted = true
                                                    visible = false
                                                }
                                            }
                                            AppButton {
                                                objectName: "sessionSignOutButton_"
                                                            + modelData.deviceId
                                                visible: modelData.isCurrent !== true
                                                         && app.uia.supported
                                                storm: true
                                                kind: "danger"
                                                implicitHeight: 26
                                                leftPadding: 10
                                                rightPadding: 10
                                                enabled: !app.uia.busy
                                                         && !app.uia.challengeActive
                                                text: qsTr("Sign out")
                                                Accessible.name:
                                                    qsTr("Sign out session %1")
                                                        .arg(modelData.deviceId)
                                                onClicked:
                                                    sessionsListCard.signOutOne(
                                                        modelData.deviceId)
                                            }
                                        }
                                    }
                                }
                                Label {
                                    visible: sessionsListCard.actionNotice.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: sessionsListCard.actionNoticeError
                                           ? AppTheme.stormDanger
                                           : AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: sessionsListCard.actionNotice
                                    Accessible.name: text
                                }
                                AppButton {
                                    objectName: "signOutOtherSessionsButton"
                                    visible: app.uia.supported
                                             && app.sessionDevices.length > 1
                                    storm: true
                                    kind: "danger"
                                    Layout.alignment: Qt.AlignLeft
                                    enabled: !app.uia.busy
                                             && !app.uia.challengeActive
                                    text: qsTr("Sign out all other sessions")
                                    Accessible.name: text
                                    onClicked: sessionsListCard.signOutAllOthers()
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsOptionSize
                                    text: qsTr("Signing out a session may require "
                                               + "your account password. "
                                               + "Verification below always "
                                               + "requires explicit confirmation "
                                               + "on both sessions.")
                                }
                            }
                        }

                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing6
                                    Icon {
                                        name: "key"
                                        size: 13
                                        color: AppTheme.stormTextFaint
                                    }
                                    MenuSectionLabel {
                                        objectName: "currentSessionHeading"
                                        text: qsTr("Current session")
                                    }
                                    Item { Layout.fillWidth: true }
                                    StatusChip {
                                        storm: true
                                        label: root.trustStateText(app.sessionTrustState)
                                        iconName: app.sessionTrustState === "Verified"
                                                  ? "verified_user" : ""
                                        tone: app.sessionTrustState === "Verified"
                                              ? "success"
                                              : app.sessionTrustState === "Not verified"
                                                ? "danger" : "neutral"
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Device ID: %1").arg(
                                        app.sessionDeviceId !== ""
                                            ? app.sessionDeviceId
                                            : (app.rustDeviceIdRedacted !== ""
                                                ? app.rustDeviceIdRedacted
                                                : qsTr("(not yet available)")))
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    visible: app.sessionTrustState !== "Verified"
                                    text: qsTr(
                                        "Verify this session using another session already " +
                                        "signed in to this Matrix account, or with your " +
                                        "recovery key. Verifying with another session does " +
                                        "not import room keys — key import is a separate " +
                                        "action below.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: !app.verificationActive
                                            && app.verificationState === ""
                                            && !(app.cryptoHealth
                                                 && app.cryptoHealth.cryptoSupported)
                                    AppButton {
                                        storm: true
                                        text: app.sessionTrustState === "Verified"
                                            ? qsTr("Verify again")
                                            : qsTr("Verify this session")
                                        enabled: app.loggedIn
                                        onClicked: app.startOwnVerification()
                                    }
                                    AppButton {
                                        storm: true
                                        objectName: "plainVerifyRecoveryButton"
                                        kind: "ghost"
                                        visible: app.backendName === "rust"
                                                 && app.sessionTrustState !== "Verified"
                                        enabled: app.loggedIn
                                        text: qsTr("Use recovery key instead")
                                        Accessible.name: text
                                        onClicked: plainVerifyRecoveryEntry.visible
                                                   = !plainVerifyRecoveryEntry.visible
                                    }
                                    Label {
                                        visible: app.sessionTrustState === "Verified"
                                        Layout.fillWidth: true
                                        color: AppTheme.stormSuccess
                                        text: qsTr("This Lightning session is verified through Matrix cross-signing.")
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                    }
                                    Item {
                                        visible: app.sessionTrustState !== "Verified"
                                        Layout.fillWidth: true
                                    }
                                }

                                RecoveryKeyEntry {
                                    id: plainVerifyRecoveryEntry
                                    objectName: "plainVerifyRecoveryEntry"
                                    Layout.fillWidth: true
                                    visible: false
                                    fieldObjectName: "plainVerifyRecoveryField"
                                    buttonText: qsTr("Use recovery key")
                                }

                                Pane {
                                    objectName: "verificationStatusCard"
                                    Layout.fillWidth: true
                                    visible: app.cryptoHealth
                                             && app.cryptoHealth.cryptoSupported
                                             && !app.verificationActive
                                             && app.verificationState === ""
                                    background: Rectangle {
                                        color: AppTheme.stormInset
                                        border.color:
                                            app.sessionVerificationNeeded
                                            ? AppTheme.stormDanger
                                            : AppTheme.stormBorder
                                        radius: AppTheme.radiusSm
                                    }
                                    RowLayout {
                                        width: parent.width
                                        spacing: AppTheme.spacing8
                                        Icon {
                                            name: app.sessionVerificationNeeded
                                                  ? "warning" : "verified_user"
                                            size: 18
                                            color: app.sessionVerificationNeeded
                                                   ? AppTheme.stormDanger
                                                   : AppTheme.stormSuccess
                                            visible: app.sessionTrustState === "Verified"
                                                     || app.sessionVerificationNeeded
                                        }
                                        Label {
                                            objectName: "verificationRestingStatus"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: app.sessionVerificationNeeded
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                            Accessible.role: Accessible.StaticText
                                            Accessible.name: text
                                            text: app.sessionVerificationNeeded
                                                ? qsTr("This session is not verified. Verify it "
                                                       + "to prove it is yours, so your other "
                                                       + "sessions share encryption keys with it.")
                                                : app.sessionTrustState === "Verified"
                                                  ? qsTr("This session is verified through Matrix "
                                                         + "cross-signing.")
                                                  : app.sessionTrustState === "Cross-signing unavailable"
                                                    ? qsTr("This account has no cross-signing identity "
                                                           + "yet, so there is nothing to verify "
                                                           + "against.")
                                                    : qsTr("Checking this session's verification "
                                                           + "state\u2026")
                                        }
                                        AppButton {
                                            storm: true
                                            objectName: "verificationDismissButton"
                                            text: qsTr("Stop reminding me")
                                            visible: app.sessionVerificationWarning
                                            onClicked: app.dismissVerificationWarning()
                                        }
                                    }
                                }
                            }
                        }

                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("privacy")
                        visible: root.section === "privacy"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        CrossSigningSetupCard {
                            Layout.fillWidth: true
                        }

                        SettingsCard {
                            objectName: "recoveryKeyCard"
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                MenuSectionLabel { text: qsTr("Recovery key") }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr(
                                        "Entering your recovery key or passphrase restores your " +
                                        "message keys and verifies this session, with no second " +
                                        "device needed, when your account's cross-signing keys " +
                                        "are stored with them. Without it, some old messages may show " +
                                        "\"[unable to decrypt yet]\" until another verified device " +
                                        "shares the room keys.")
                                }

                                Label {
                                    text: qsTr("Recovery key or passphrase")
                                    font.weight: AppTheme.weightStrong
                                    color: AppTheme.stormText
                                }
                                RecoveryKeyEntry {
                                    id: recoveryEntry
                                    objectName: "recoveryEntry"
                                    Layout.fillWidth: true
                                    fieldObjectName: "recoveryInputField"
                                    buttonText: qsTr("Restore keys")
                                }

                                Label {
                                    objectName: "importRoomKeysHeading"
                                    text: qsTr("Import room keys")
                                    font.weight: AppTheme.weightStrong
                                    color: AppTheme.stormText
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr(
                                        "Import an encrypted Matrix room-key export from another " +
                                        "session. Imported keys may unlock older encrypted messages, " +
                                        "but they do not verify this session.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        storm: true
                                        text: qsTr("Choose key export")
                                        enabled: app.loggedIn && !importPanel.running
                                        onClicked: importFileDialog.open()
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        elide: Text.ElideMiddle
                                        color: AppTheme.stormTextMuted
                                        text: importPanel.selectedFileName === ""
                                            ? qsTr("(no file selected)")
                                            : importPanel.selectedFileName
                                        textFormat: Text.PlainText
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: importPanel.selectedFileUrl.toString() !== ""
                                    AppTextField {
                                        storm: true
                                        id: importPassphraseField
                                        Layout.fillWidth: true
                                        echoMode: TextInput.Password
                                        placeholderText: qsTr("Export passphrase")
                                        enabled: !importPanel.running
                                        onAccepted: {
                                            if (text.length > 0)
                                                importStartButton.clicked()
                                        }
                                    }
                                    AppButton {
                                        storm: true
                                        id: importStartButton
                                        text: importPanel.running
                                            ? qsTr("Importing…")
                                            : qsTr("Import")
                                        enabled: !importPanel.running
                                            && importPassphraseField.text.length > 0
                                        onClicked: {
                                            app.importRoomKeys(
                                                importPanel.selectedFileUrl,
                                                importPassphraseField.text)
                                            importPassphraseField.text = ""
                                        }
                                    }
                                    AppButton {
                                        storm: true
                                        text: qsTr("Clear")
                                        enabled: !importPanel.running
                                        onClicked: {
                                            importPanel.selectedFileUrl = ""
                                            importPanel.selectedFileName = ""
                                            importPassphraseField.text = ""
                                        }
                                    }
                                }
                                ProgressBar {
                                    id: keyImportProgress
                                    Layout.fillWidth: true
                                    visible: importPanel.running
                                    indeterminate: app.roomKeyImportTotalCount === 0
                                    from: 0
                                    to: Math.max(1, app.roomKeyImportTotalCount)
                                    value: app.roomKeyImportImportedCount
                                    implicitHeight: 4
                                    background: Rectangle {
                                        implicitHeight: 4
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.stormInset
                                    }
                                    contentItem: Item {
                                        implicitHeight: 4
                                        clip: true
                                        Rectangle {
                                            height: parent.height
                                            radius: AppTheme.radiusPill
                                            color: AppTheme.bolt
                                            width: keyImportProgress.indeterminate
                                                   ? parent.width * 0.35
                                                   : parent.width
                                                     * keyImportProgress.position
                                            SequentialAnimation on x {
                                                running: keyImportProgress.indeterminate
                                                         && keyImportProgress.visible
                                                         && !AppTheme.reducedMotion
                                                loops: Animation.Infinite
                                                NumberAnimation {
                                                    from: -keyImportProgress.width * 0.35
                                                    to: keyImportProgress.width
                                                    duration: 1100
                                                    easing.type: Easing.InOutQuad
                                                }
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: importPanel.statusText !== ""
                                    color: importPanel.statusColor
                                    text: importPanel.statusText
                                    textFormat: Text.PlainText
                                }
                                QtObject {
                                    id: importPanel
                                    property url selectedFileUrl: ""
                                    property string selectedFileName: ""
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.stormTextMuted
                                }
                                NativeFileDialog {
                                    id: importFileDialog
                                    purpose: "keys"
                                    currentFolder: app.defaultFileDialogFolder()
                                    title: qsTr("Select encrypted Matrix room-key export")
                                    fileMode: "open"
                                    onAccepted: {
                                        importPanel.selectedFileUrl = selectedFile
                                        importPanel.selectedFileName =
                                            selectedFile.toString().split('/').pop()
                                        importPanel.statusText = ""
                                    }
                                }
                                Connections {
                                    target: app
                                    function onRoomKeyImportStateChanged() {
                                        var state = app.roomKeyImportState
                                        if (state === "importing") {
                                            importPanel.running = true
                                            importPanel.statusText =
                                                qsTr("Importing room keys…")
                                            importPanel.statusColor = AppTheme.stormTextMuted
                                        } else if (state === "done") {
                                            importPanel.running = false
                                            var doneText = qsTr(
                                                "Room-key import complete.\n" +
                                                "Imported sessions: %1\n" +
                                                "Affected rooms: %2\n" +
                                                "Note: importing keys does not verify this session.")
                                                .arg(app.roomKeyImportImportedCount)
                                                .arg(app.roomKeyImportAffectedRoomCount)
                                            if (app.roomKeyImportLastMessage !== "")
                                                doneText += "\n" + app.roomKeyImportLastMessage
                                            importPanel.statusText = doneText
                                            importPanel.statusColor = AppTheme.stormSuccess
                                            importPanel.selectedFileUrl = ""
                                            importPanel.selectedFileName = ""
                                            importPassphraseField.text = ""
                                        } else if (state === "failed") {
                                            importPanel.running = false
                                            importPanel.statusText =
                                                app.roomKeyImportLastMessage
                                            importPanel.statusColor = AppTheme.stormDanger
                                            importPassphraseField.text = ""
                                        }
                                    }
                                }

                                RoomKeyExportSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr(
                                        "Verification establishes trust in this session. " +
                                        "Secure Backup and room-key imports provide decryption keys " +
                                        "for message history. These are separate operations.")
                                }
                            }
                        }

                        SettingsCard {
                            visible: app.backendName === "rust"
                            background: Rectangle {
                                color: AppTheme.stormPanel
                                border.color: dangerZone.expanded ? AppTheme.stormDanger
                                                                  : AppTheme.stormBorder
                                radius: AppTheme.radiusMd
                            }
                            ColumnLayout {
                                id: dangerZone
                                objectName: "dangerZone"
                                property bool expanded: false
                                width: parent.width
                                spacing: AppTheme.spacing8

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Danger Zone")
                                        color: AppTheme.stormDanger
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Item { Layout.fillWidth: true }
                                    AppButton {
                                        storm: true
                                        text: dangerZone.expanded ? qsTr("Hide") : qsTr("Show")
                                        Accessible.name: qsTr("Toggle danger zone")
                                        onClicked: dangerZone.expanded = !dangerZone.expanded
                                    }
                                }
                                ColumnLayout {
                                    visible: dangerZone.expanded
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.settingsHintSize
                                        text: qsTr(
                                            "Reset deletes only Lightning's local Rust SDK store " +
                                            "for this account (also available from a terminal: " +
                                            "lightning-matrix --reset-crypto-store). It does not touch " +
                                            "server messages or Element data. You will need to sign " +
                                            "in again afterwards.")
                                    }
                                    AppButton {
                                        storm: true
                                        id: resetDangerButton
                                        kind: "danger"
                                        text: qsTr("Reset local Lightning session")
                                        onClicked: resetConfirmDialog.open()
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        visible: resetStatus.text !== ""
                                        color: resetStatus.ok ? AppTheme.stormSuccess : AppTheme.stormDanger
                                        text: resetStatus.text
                                    }
                                }
                                QtObject {
                                    id: resetStatus
                                    property bool ok: false
                                    property string text: ""
                                }
                                Connections {
                                    target: app
                                    function onLocalRustStoreResetResult(ok, message) {
                                        resetStatus.ok = ok
                                        resetStatus.text = message
                                    }
                                }
                                ConfirmDialog {
                                    id: resetConfirmDialog
                                    title: qsTr("Reset local Lightning session?")
                                    confirmText: qsTr("Reset")
                                    confirmKind: "dangerPrimary"
                                    width: 440
                                    Label {
                                        width: 380
                                        wrapMode: Text.WordWrap
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.settingsOptionSize
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        text: qsTr(
                                            "This deletes Lightning's local Matrix Rust SDK " +
                                            "store and any saved smoke session for this " +
                                            "account. Server messages, Element data, and " +
                                            "other accounts are untouched. You will need to " +
                                            "sign in again after this.")
                                    }
                                    onAccepted: app.resetLocalRustStore()
                                }
                            }
                        }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("labs")
                        visible: root.section === "labs"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Labs")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Experimental features, and diagnostics.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.settingsOptionSize
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }
                        SettingsGroupLabel {
                            objectName: "noiseSuppressionHeading"
                            text: qsTr("Microphone noise suppression")
                        }
                        SettingsCard {
                            objectName: "noiseSuppressionCard"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Filters background noise out of "
                                               + "your microphone in calls. One "
                                               + "method runs at a time, and a "
                                               + "change applies to a call in "
                                               + "progress.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                }
                                NoiseSuppressionSelector {
                                    id: noiseSelector
                                    Layout.fillWidth: true
                                    selected: app.settings.noiseSuppressionMode
                                    failedMode: app.groupCall
                                        ? app.groupCall.noiseSuppressionFailedMode
                                        : ""
                                    fellBackToWebrtc: app.groupCall
                                        ? app.groupCall.noiseSuppressionFallbackToWebrtc
                                        : false
                                    function refresh() {
                                        choices = app.groupCall
                                            ? app.groupCall.noiseSuppressionChoices()
                                            : []
                                    }
                                    Component.onCompleted: refresh()
                                    onVisibleChanged: if (visible) refresh()
                                    onChosen: (key) => {
                                        if (app.groupCall
                                                && key === app.groupCall.noiseSuppressionFailedMode
                                                && key === app.settings.noiseSuppressionMode)
                                            app.groupCall.retryNoiseSuppression()
                                        else
                                            app.settings.noiseSuppressionMode = key
                                    }
                                }
                            }
                        }
                        SettingsGroupLabel {
                            objectName: "gameActivityHeading"
                            text: qsTr("Game activity")
                        }
                        SettingsCard {
                            objectName: "gameActivityCard"
                            GameActivitySettings {
                                width: parent.width
                            }
                        }
                        SettingsGroupLabel {
                            objectName: "musicActivityHeading"
                            text: qsTr("Music activity")
                        }
                        SettingsCard {
                            objectName: "musicActivityCard"
                            MusicActivitySettings {
                                width: parent.width
                            }
                        }
                        LabsConcurrentAccountsCard {}
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    objectName: "labsBackendLine"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Backend: %1").arg(app.backendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    objectName: "labsSyncModeLine"
                                    visible: app.syncModeLabel !== ""
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Sync mode: %1").arg(app.syncModeLabel)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    objectName: "labsConnectionLine"
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Connection: %1").arg(app.connectionStatus)
                                }
                                AppButton {
                                    objectName: "labsRefreshRoomButton"
                                    storm: true
                                    text: qsTr("Refresh current room")
                                    enabled: app.currentRoomId !== ""
                                    onClicked: app.reloadCurrentRoomTimeline(50)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Rebuilds the open room's timeline from the "
                                               + "SDK. Safe at any time.")
                                }
                            }
                        }
                        }
                    }

                    UpdatesSettingsSection {
                        objectName: "updatesSection"
                        visible: root.section === "updates"
                        Layout.fillWidth: true
                    }

                    Loader {
                        Layout.fillWidth: true
                        active: root.sectionLive("about")
                        visible: root.section === "about"
                        sourceComponent: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("About")
                            color: AppTheme.settingsHeading
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Image {
                                        objectName: "aboutAppLogo"
                                        source: app.appIconSource
                                        sourceSize.width: 56
                                        sourceSize.height: 56
                                        Layout.preferredWidth: 56
                                        Layout.preferredHeight: 56
                                        fillMode: Image.PreserveAspectFit
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name: qsTr("Lightning logo")
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Lightning %1").arg(app.appVersion)
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.settingsOptionSize
                                            font.weight: AppTheme.weightStrong
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: AppTheme.stormTextMuted
                                            text: qsTr("A native C++/Qt Matrix desktop client. "
                                                       + "No Electron, no web view.")
                                        }
                                    }
                                }
                                Label {
                                    visible: app.backendName === "rust"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("Matrix engine: matrix-sdk 0.18.0 / matrix-sdk-ui 0.18.0 (Rust)")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.settingsHintSize
                                    text: qsTr("License: GPL-3.0-or-later")
                                }
                            }
                        }

                        AboutToolsSection { }
                        DeveloperToolsSection { }
                        LogExportCard {}

                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: AppTheme.spacing24
                        }
                        }
                    }
                }
            }

            }
        }
    }

    QrLoginDialog {
        id: qrLoginDialog
    }
}
