import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// In-shell Settings pane: replaces only the timeline region; the spaces rail
// and room list stay visible. A 60px header, a 260px navigation column and the
// per-category panes. Destructive reset lives in a separate Danger Zone; sign
// out lives only in the account menu.
//
// Category panes toggle by visibility (never a Loader) so in-flight
// verification/import state survives switching categories.
Item {
    id: root
    // Named so a suite can reach searchIndex and the reveal helpers.
    objectName: "settingsScreenRoot"
    // A minted recovery key must not stay in memory once the user leaves the
    // card, including on a section change: this screen is a warm Loader kept
    // alive across opens. An armed destructive confirmation is cleared too, or
    // coming back later would turn a two-press action into one.
    function forgetTransientBackupState() {
        if (app.backup)
            app.backup.dismissRecoveryKey()
        if (typeof backupCard !== "undefined" && backupCard)
            backupCard.pendingConfirm = ""
    }
    onVisibleChanged: {
        if (!visible) {
            root.forgetTransientBackupState()
            // Clear the search query as well. Otherwise the nav reopens
            // narrowed to the previous query's matches, possibly empty. Done on
            // hide, not show: Ctrl+, while open focuses the field, and the
            // screenshot demo sets a query after showing.
            if (settingsSearchField)
                settingsSearchField.text = ""
        }
    }
    onSectionChanged: root.forgetTransientBackupState()

    // Whether the sanitized E2EE recovery diagnostics are expanded.
    property bool showRecoveryDiagnostics: false

    // FontManager, or null. `fonts` is a context property from main.cpp that
    // several suites do not install, and `visible: false` does not stop
    // bindings being created, so every reference goes through this guarded
    // alias.
    readonly property var fontManager:
        (typeof fonts !== "undefined") ? fonts : null

    // Client-side settings search. Entries are {title, keywords, section,
    // breadcrumb, control?, anchor}, indexed by section key (Privacy & security
    // spans several blocks in this file). `control` names one of the settings
    // with an inline live control in the results, bound two-way to the same
    // SettingsManager property as its real control.
    property string settingsSearchQuery: ""
    readonly property var searchIndex: [
        { title: qsTr("Account"), keywords: qsTr("account profile"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "accountIdentityCard" },
        { title: qsTr("Homeserver"), keywords: qsTr("homeserver server url"),
          section: "account", breadcrumb: qsTr("Account"),
          anchor: "homeserverField" },
        { title: qsTr("Start minimized"), keywords: qsTr("startup minimized"),
          section: "account", breadcrumb: qsTr("Account · Startup"),
          anchor: "startMinimizedCheck" },

        { title: qsTr("Theme"),
          keywords: qsTr("theme moss indigo teal light dark graphite midnight nordic purple warm"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "featuredThemeFlow" },
        { title: qsTr("Match system light/dark"),
          keywords: qsTr("match system auto theme"), section: "appearance",
          breadcrumb: qsTr("Appearance · Theme"), control: "matchSystem",
          anchor: "matchSystemSwitch" },
        { title: qsTr("Message layout"),
          keywords: qsTr("message layout modern bubbles compact"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          control: "messageLayout",
          anchor: "messageLayoutControl" },
        // Indexed under words people use for the rail, including "space bar".
        { title: qsTr("Spaces rail depth"),
          keywords: qsTr("spaces rail depth space bar sidebar nesting regions "
                         + "classic old style flat tint indent"),
          section: "appearance", breadcrumb: qsTr("Appearance · Panels"),
          control: "spacesRailDepth",
          anchor: "spacesRailDepthControl" },
        { title: qsTr("Text size"), keywords: qsTr("text size font scale"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "textScaleSlider" },
        { title: qsTr("Interface zoom"),
          keywords: qsTr("interface zoom scale bigger ui size"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "interfaceZoomSlider" },
        { title: qsTr("Font"), keywords: qsTr("font family typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "uiFontSelector" },
        { title: qsTr("Code font"),
          keywords: qsTr("code font monospace mono fixed width typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance · Font"),
          anchor: "monoFontCombo" },
        { title: qsTr("Your own fonts"),
          keywords: qsTr("import font file ttf otf install custom typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance · Font"),
          anchor: "importedFontCard" },
        { title: qsTr("Language"), keywords: qsTr("language locale"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          anchor: "languageCombo" },
        { title: qsTr("Show room activity"),
          keywords: qsTr("room activity membership joins leaves profile"),
          section: "appearance", breadcrumb: qsTr("Appearance · Timeline"),
          control: "showRoomActivity",
          anchor: "showRoomActivityCheck" },
        { title: qsTr("Mouse-wheel speed"),
          keywords: qsTr("wheel speed scroll timeline"), section: "appearance",
          breadcrumb: qsTr("Appearance · Timeline"),
          anchor: "timelineWheelSpeedCombo" },
        { title: qsTr("Joins, leaves and invites"),
          keywords: qsTr("membership join leave invite kick ban activity hide"),
          section: "appearance", breadcrumb: qsTr("Appearance · Timeline"),
          control: "showMembershipEvents",
          anchor: "showMembershipEventsCheck" },
        { title: qsTr("Display name and avatar changes"),
          keywords: qsTr("profile change display name avatar activity hide"),
          section: "appearance", breadcrumb: qsTr("Appearance · Timeline"),
          control: "showProfileChangeEvents",
          anchor: "showProfileChangeEventsCheck" },
        { title: qsTr("Collapse media and link embeds"),
          keywords: qsTr("embed embeds collapse collapsed compact single line "
                         + "clutter declutter media image picture gif sticker "
                         + "video audio voice file attachment link preview "
                         + "expand arrow"),
          section: "appearance", breadcrumb: qsTr("Appearance · Timeline"),
          control: "collapseEmbeds",
          anchor: "collapseEmbedsCheck" },
        { title: qsTr("Reduce motion"),
          keywords: qsTr("reduced motion animation accessibility vestibular"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          control: "reducedMotion",
          anchor: "reducedMotionCheck" },
        { title: qsTr("Smooth scrolling"),
          keywords: qsTr("smooth scrolling scroll wheel glide animation instant jumpy mouse"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          control: "smoothScrolling",
          anchor: "smoothScrollingCheck" },
        { title: qsTr("Clock"),
          keywords: qsTr("clock 24 hour time format am pm timestamp"),
          section: "appearance",
          breadcrumb: qsTr("Appearance · Motion and time"),
          anchor: "clockFormatCombo" },
        { title: qsTr("Show Space banners"),
          keywords: qsTr("space banner header image hide show"),
          section: "appearance", breadcrumb: qsTr("Appearance · Panels"),
          control: "spaceBannersVisible",
          anchor: "spaceBannersVisibleCheck" },
        { title: qsTr("Conversation list width"),
          keywords: qsTr("room list width panel size sidebar"),
          section: "appearance", breadcrumb: qsTr("Appearance · Panels"),
          anchor: "roomListWidthSlider" },
        { title: qsTr("Side panel width"),
          keywords: qsTr("side panel width members threads size"),
          section: "appearance", breadcrumb: qsTr("Appearance · Panels"),
          anchor: "sidePanelWidthSlider" },
        { title: qsTr("Enter starts a new line"),
          keywords: qsTr("enter newline send composer message box return"),
          section: "appearance", breadcrumb: qsTr("Appearance · Message box"),
          control: "enterInsertsNewline",
          anchor: "enterInsertsNewlineCheck" },
        { title: qsTr("Send text with an attachment as its caption"),
          keywords: qsTr("caption attachment upload text description"),
          section: "appearance", breadcrumb: qsTr("Appearance · Message box"),
          control: "sendTextAsCaption",
          anchor: "sendTextAsCaptionCheck" },
        { title: qsTr("Message box buttons"),
          keywords: qsTr("composer buttons hide show emoji gif sticker stickers "
                         + "voice microphone formatting schedule send later "
                         + "declutter simplify"),
          section: "appearance", breadcrumb: qsTr("Appearance · Message box"),
          anchor: "composerButtonsHeading" },
        { title: qsTr("Check spelling as you type"),
          keywords: qsTr("spell spelling checker dictionary typo underline language"),
          section: "appearance", breadcrumb: qsTr("Appearance · Message box"),
          anchor: "spellCheckEnabledCheck" },
        { title: qsTr("Spelling language"),
          keywords: qsTr("spell spelling language dictionary automatic system"),
          section: "appearance", breadcrumb: qsTr("Appearance · Message box"),
          anchor: "spellLanguageCombo" },

        { title: qsTr("Keyboard shortcuts"),
          keywords: qsTr("keyboard shortcut shortcuts key keys binding rebind hotkey"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "shortcutsHeading" },
        { title: qsTr("Reset all shortcuts"),
          keywords: qsTr("reset shortcuts default keys"),
          section: "shortcuts", breadcrumb: qsTr("Keyboard shortcuts"),
          anchor: "shortcutResetAllButton" },
        { title: qsTr("Bold, italic and code keys"),
          keywords: qsTr("bold italic strikethrough code quote list formatting keys"),
          section: "shortcuts",
          breadcrumb: qsTr("Keyboard shortcuts · Message formatting"),
          anchor: "shortcutRow_composer.bold" },

        { title: qsTr("Microphone"),
          keywords: qsTr("microphone mic input device voice call audio sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          control: "callDevice_microphone",
          anchor: "callDevice_microphone" },
        { title: qsTr("Microphone volume"),
          keywords: qsTr("microphone volume gain mic input level loud quiet boost amplify sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Microphone"),
          anchor: "microphoneGainSlider" },
        { title: qsTr("Output device"),
          keywords: qsTr("speaker output headphones headset device voice call audio sound"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Output"),
          control: "callDevice_speaker",
          anchor: "callDevice_speaker" },
        { title: qsTr("Media playback volume"),
          keywords: qsTr("volume sound audio video voice message playback level media loud"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Media"),
          anchor: "mediaVolumeSettingSlider" },
        { title: qsTr("Camera"),
          keywords: qsTr("camera webcam video device call"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Camera"),
          control: "callDevice_camera",
          anchor: "callDevice_camera" },
        { title: qsTr("Call sounds"),
          keywords: qsTr("call sounds join leave mute deafen unmute screen share hand chime beep effects"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Call sounds"),
          anchor: "callSoundsEnabledCheck" },
        { title: qsTr("Ringer volume"),
          keywords: qsTr("ringer ringtone ring volume incoming call loud"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Call sounds"),
          anchor: "ringerVolumeSlider" },
        { title: qsTr("Float the call when Lightning is minimised"),
          keywords: qsTr("float picture in picture pip call window minimised"),
          section: "sound",
          breadcrumb: qsTr("Sound & video · Calls"),
          anchor: "callPictureInPictureCheck" },
        { title: qsTr("Ring for incoming voice calls"),
          keywords: qsTr("ring ringer ringtone sound incoming call alert"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "ringForCallsCheck" },
        { title: qsTr("Desktop notifications"),
          keywords: qsTr("notifications desktop enable"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          control: "notificationsEnabled",
          anchor: "notificationsEnabledCheck" },
        { title: qsTr("Notification preview"),
          keywords: qsTr("notification preview privacy sender message"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          anchor: "notificationPreviewCombo" },

        { title: qsTr("Notification preview in encrypted rooms"),
          keywords: qsTr("notification preview encrypted body privacy hide message text"),
          section: "notifications",
          breadcrumb: qsTr("Notifications"),
          anchor: "notificationPreviewEncryptedCombo" },
        { title: qsTr("Notification sound"),
          keywords: qsTr("notification sound mute"), section: "notifications",
          breadcrumb: qsTr("Notifications"),
          anchor: "notificationSoundCombo" },

        { title: qsTr("Only exchange messages with verified devices"),
          keywords: qsTr("invisible crypto msc4153 cross-signed verified "
                         + "device trust insecure exclude encryption"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Device trust"),
          anchor: "strictDeviceTrustCheck" },

        { title: qsTr("Read receipts"),
          keywords: qsTr("read receipt receipts private seen ticks blue "
                         + "m.read.private privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Reading and typing"),
          anchor: "readReceiptModeCombo" },

        { title: qsTr("Let others see when I am typing"),
          keywords: qsTr("typing notice notification composing indicator "
                         + "privacy"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Reading and typing"),
          anchor: "sendTypingCheck" },

        { title: qsTr("Share my online status"),
          keywords: qsTr("presence online idle offline status share"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Presence"),
          anchor: "sharePresenceCheck" },

        { title: qsTr("Ignored users"),
          keywords: qsTr("ignore ignored block user mute person hide"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Ignored users"),
          anchor: "ignoredUsersCard" },
        { title: qsTr("Sign out other sessions"),
          keywords: qsTr("sessions devices sign out remove device delete"),
          section: "sessions",
          breadcrumb: qsTr("Sessions"),
          anchor: "signOutOtherSessionsButton" },

        { title: qsTr("Automatically load previews in unencrypted rooms"),
          keywords: qsTr("link preview privacy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          control: "autoLoadLinkPreviews",
          anchor: "autoPreviewCheck" },
        { title: qsTr("Load previews in encrypted rooms"),
          keywords: qsTr("link preview encrypted"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          anchor: "encryptedPreviewCheck" },
        { title: qsTr("Autoplay and prefetch media"),
          keywords: qsTr("gif autoplay prefetch video audio media"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Media"),
          anchor: "gifAutoplayCombo" },
        { title: qsTr("GIF safe search"),
          keywords: qsTr("gif safe search rating"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "gifRatingCombo" },
        { title: qsTr("Preferred GIF provider"),
          keywords: qsTr("gif provider giphy klipy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "gifProviderCombo" },
        { title: qsTr("Store recently used GIFs"),
          keywords: qsTr("gif recents store"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs"),
          anchor: "starredGifsSummaryLabel" },
        { title: qsTr("Security status"),
          keywords: qsTr("e2ee encryption status cross-signing backup"),
          section: "privacy", breadcrumb: qsTr("Privacy & security"),
          anchor: "cryptoHealthSummary" },
        { title: qsTr("Recovery key or passphrase"),
          keywords: qsTr("recovery key passphrase backup restore"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "recoveryInputField" },
        { title: qsTr("Import room keys"),
          keywords: qsTr("import room keys export"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "importRoomKeysHeading" },
        { title: qsTr("Danger Zone"),
          keywords: qsTr("reset danger local session"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery"),
          anchor: "dangerZone" },

        { title: qsTr("Sessions"), keywords: qsTr("sessions devices"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "sessionsHeading" },
        { title: qsTr("Current session"),
          keywords: qsTr("device id session status"), section: "sessions",
          breadcrumb: qsTr("Sessions"),
          anchor: "currentSessionHeading" },
        { title: qsTr("Sign in another device"),
          keywords: qsTr("qr code scan sign in another device phone link "
                         + "msc4108 login"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "qrLoginOpenButton" },

        { title: qsTr("Verify this session"),
          keywords: qsTr("verify verification sas cross-signing"),
          section: "sessions", breadcrumb: qsTr("Sessions"),
          anchor: "verificationStatusCard" },

        { title: qsTr("Backend"), keywords: qsTr("backend rust http mock"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsBackendLine" },
        { title: qsTr("Sync mode"), keywords: qsTr("sync sliding"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsSyncModeLine" },
        { title: qsTr("Connection"), keywords: qsTr("connection status"),
          section: "labs", breadcrumb: qsTr("Labs"),
          anchor: "labsConnectionLine" },
        { title: qsTr("Refresh current room"),
          keywords: qsTr("refresh reload timeline"), section: "labs",
          breadcrumb: qsTr("Labs"),
          anchor: "labsRefreshRoomButton" },

        { title: qsTr("About"), keywords: qsTr("about version license"),
          section: "about", breadcrumb: qsTr("About"),
          anchor: "aboutAppLogo" },

        { title: qsTr("Updates"),
          keywords: qsTr("update version upgrade check download install"),
          section: "updates", breadcrumb: qsTr("Updates"),
          anchor: "updatesSection" },
        { title: qsTr("Automatically check for updates"),
          keywords: qsTr("update automatic check background"),
          section: "updates", breadcrumb: qsTr("Updates · Automatic checks"),
          anchor: "updatesSection" },
    ]
    readonly property var matchedSearchResults: {
        var q = root.settingsSearchQuery.trim().toLowerCase()
        if (q.length === 0) return []
        return root.searchIndex.filter(function(e) {
            return (e.title + " " + e.keywords).toLowerCase().indexOf(q) !== -1
        })
    }
    readonly property var matchedSearchSections: {
        var s = {}
        for (var i = 0; i < root.matchedSearchResults.length; ++i)
            s[root.matchedSearchResults[i].section] = true
        return s
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
        if (idx === -1) return safe
        return safe.slice(0, idx) + "<font color=\"" + AppTheme.bolt + "\">"
             + safe.slice(idx, idx + lowerQ.length) + "</font>"
             + safe.slice(idx + lowerQ.length)
    }

    // Each entry's `anchor` is the objectName of the control it names. A click
    // scrolls to it and flashes a halo, which answers the click even when the
    // section does not change and lands on sub-groups deep in a long page.
    // Anchors are explicit, not derived from translated text;
    // everySearchIndexEntryResolvesItsAnchor checks every entry.
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

    // Returns the target's y in content coordinates, or -1 when there is
    // nothing to reveal.
    function applySearchReveal(flash) {
        if (root.searchRevealAnchor.length === 0)
            return -1
        var target = root.findInPane(contentColumn, root.searchRevealAnchor)
        if (!target)
            return -1
        var top = target.mapToItem(contentFlick.contentItem, 0, 0).y
        var maxY = Math.max(0, contentFlick.contentHeight - contentFlick.height)
        // Land the control slightly below the top so its heading stays visible.
        settingsWheelArea.stopGlide()
        contentFlick.contentY =
            Math.max(0, Math.min(top - AppTheme.spacing24 * 2, maxY))
        if (flash)
            searchRevealHalo.flashOver(target)
        else
            searchRevealHalo.placeOver(target)
        return top
    }

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
                running = false
                ticks = 0
                lastTop = -1
                return
            }
            lastTop = top
        }
    }

    // Indeterminate spinner in the accent. Basic's BusyIndicator inks
    // palette.dark (textSecondary), which never reads as active.
    component StormSpinner: Item {
        id: spinner
        property color ink: AppTheme.bolt
        property bool running: true
        property int diameter: 16
        implicitWidth: diameter
        implicitHeight: diameter
        visible: running
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: Qt.alpha(spinner.ink, 0.22)
        }
        Item {
            anchors.fill: parent
            transformOrigin: Item.Center
            Rectangle {
                // Centred on the 2px ring stroke.
                width: 6; height: 6; radius: 3
                color: spinner.ink
                anchors.horizontalCenter: parent.horizontalCenter
                y: -2
            }
            RotationAnimator on rotation {
                // Reduced motion keeps the ring and its head, static.
                running: spinner.running && spinner.visible
                         && !AppTheme.reducedMotion
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }
        }
    }

    // Group label above a cluster of controls. Uses the menuSection* tokens (UI
    // face, 12px, 600, sentence case) and the muted ink; faint is the disabled
    // ink and falls below AA on several themes.
    component SettingsGroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }

    // Reusable confirmation dialog with themed chrome and AppButton footer. The
    // Basic style's Dialog is square, matches this screen's background under
    // Storm, and its buttons draw an invisible focus border.
    component ConfirmDialog: Dialog {
        id: confirmDialog
        property string confirmText: qsTr("Confirm")
        // "primary" for a benign commit, "dangerPrimary" for a destructive one;
        // the quiet "danger" outline is not a confirm button.
        property string confirmKind: "primary"
        anchors.centerIn: parent
        modal: true
        // Explicit width: sizing a Dialog from fixed-width content feeds
        // implicitWidth back into itself.
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
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: confirmDialog.reject()
                }
                AppButton {
                    storm: true
                    kind: confirmDialog.confirmKind
                    text: confirmDialog.confirmText
                    onClicked: confirmDialog.accept()
                }
            }
        }
    }

    // Settings card. stormPanel (the palette's `surface`), not stormCanvas: on
    // every theme but Storm the canvas routes to `background`, so the card was
    // the same colour as the page. Anything nested that painted stormPanel now
    // uses stormInset. theSettingsCardIsVisibleAgainstThePageOnEveryTheme holds
    // the floor.
    component SettingsCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            objectName: "settingsCardSurface"
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    // Storm-skinned slider, so new sliders do not arrive with the Basic groove.
    // Geometry and white thumb match textScaleSlider (the thumb rides the fill
    // boundary, so a dark disc would read as disabled).
    component SettingsSlider: Slider {
        id: styledSlider
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
            border.width: styledSlider.visualFocus ? 2 : 0
            border.color: AppTheme.bolt
        }
    }

    // Nav row: 32px, radiusTile; the active row fills selectedHover, brightens
    // icon and label, and carries the bolt caret.
    component SettingsNavRow: ItemDelegate {
        id: navRow
        property string sectionKey: ""
        property string iconName: ""
        property string navLabel: ""
        // Attention dot on the row leading to something that needs attention.
        // Dismissible (see sessionVerificationWarning).
        property bool alert: false
        objectName: "settingsNavRow_" + sectionKey
        Layout.fillWidth: true
        implicitHeight: 32
        // Constant content inset for the caret gutter, so rows do not shift as
        // the selection moves. The caret sits inside the pill (4px edge, 11px
        // caret, then content); overhanging the rounded corner made it look
        // broken.
        readonly property int caretGutter: 4 + 11 + AppTheme.spacing4
        leftPadding: padding + caretGutter
        // The Basic ItemDelegate's 12px padding survives the forced 32px height
        // and leaves contentItem too little room to centre; same fix as
        // AppMenuItem.
        topPadding: 0
        bottomPadding: 0
        // Typing narrows the nav to sections with a match, but never empties
        // it: with zero matches every row stays so the reader has a way on.
        visible: root.settingsSearchQuery.trim().length === 0
                 || root.matchedSearchResults.length === 0
                 || root.matchedSearchSections[sectionKey] === true
        highlighted: root.section === sectionKey
        Accessible.name: navLabel
        Accessible.selected: navRow.highlighted
        onClicked: root.section = sectionKey
        contentItem: RowLayout {
            spacing: AppTheme.spacing8
            // Pin the icon's height to the label's implicitHeight: the icon
            // font and UI font have different metrics, so RowLayout would
            // centre their ink at slightly different y (visible at fractional
            // DPRs).
            Icon {
                name: navRow.iconName
                size: 16
                Layout.alignment: Qt.AlignVCenter
                // navRowText, not navLabel: that name is the row's string
                // property.
                Layout.preferredHeight: navRowText.implicitHeight
                color: navRow.highlighted ? AppTheme.bolt
                                          : AppTheme.stormTextMuted
            }
            Label {
                id: navRowText
                text: navRow.navLabel
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                color: navRow.highlighted ? AppTheme.stormText
                                          : AppTheme.stormTextSecondary
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textBody
                font.weight: navRow.highlighted ? AppTheme.weightBold : AppTheme.weightStrong
                elide: Label.ElideRight
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
            // selectedHover, not stormSelection: outside Storm stormSelection
            // routes to `hover`, which is nearly invisible on this column in
            // the light palettes, and `selected` equals accentSoft on Moss
            // Light. The storm namespace has no strong selection role; adding
            // stormSelectionStrong to AppTheme is the tidier follow-up.
            color: navRow.highlighted ? AppTheme.selectedHover
                 : navRow.hovered ? Qt.alpha(AppTheme.selectedHover, 0.55)
                 : "transparent"
            Icon {
                objectName: "settingsNavCaret"
                visible: navRow.highlighted
                name: "bolt"
                size: 11
                color: AppTheme.bolt
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                // Inside the pill, clear of its rounded corner (see
                // caretGutter).
                anchors.leftMargin: 4
            }
        }
    }

    // "account" | "appearance" | "shortcuts" | "notifications" | "sound" |
    // "privacy" | "sessions" | "labs" | "about". Opens on Appearance.
    property string section: "appearance"

    function sectionTitle(key) {
        if (key === "account") return qsTr("Account")
        if (key === "appearance") return qsTr("Appearance")
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
        // The icon font is a subset with no plain "keyboard" glyph; an unmapped
        // name renders as tofu and fails IconChromeTest.
        if (key === "shortcuts") return "keyboard_return"
        if (key === "notifications") return "notifications"
        // volume_up is mapped (Icon.qml); an unmapped name fails
        // IconChromeTest.
        if (key === "sound") return "volume_up"
        if (key === "privacy") return "verified_user"
        if (key === "sessions") return "devices"
        if (key === "labs") return "science"
        // Reuses the verified "download" glyph.
        if (key === "updates") return "download"
        if (key === "about") return "info"
        return "settings"
    }

    // Mirrors GifPicker.qml's formatBytes(); kept local deliberately.
    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    // Deep links from older call sites (e.g. "security") keep working.
    function mapLegacySection(key) {
        if (key === "general") return "appearance"
        if (key === "security") return "privacy"
        if (key === "advanced") return "labs"
        return key
    }

    Component.onCompleted: {
        var requested = app.takeRequestedSettingsSection()
        if (requested.length > 0)
            section = mapLegacySection(requested)
    }
    // The screen is built once and kept (Main.qml), so a section request while
    // alive arrives here; on a cold build both paths fire with the same value.
    Connections {
        target: app
        function onSettingsSectionRequested(requested) {
            if (requested.length > 0)
                section = mapLegacySection(requested)
            // Consume the request so a rebuild does not replay it. If still
            // incubating, Component.onCompleted takes it instead, which is why
            // C++ must not clear it.
            app.takeRequestedSettingsSection()
        }
    }

    function goBack() {
        app.loggedIn ? app.showMain() : app.showLogin()
    }

    // Off-screen TextEdit, the same mechanism MessageDelegate uses for Copy.
    // Cleared straight after so nothing lingers.
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
        enabled: root.visible && !accountIdentityCard.editingDisplayName
        onActivated: root.goBack()
    }
    // Focuses the settings search field when Settings is already open.
    // MainScreen declares the same action for when it is closed; the gates must
    // be exclusive (root.visible here is app.currentScreen === 2) or Qt fires
    // neither. The sequence comes from ShortcutRegistry. `bindingRevision` is
    // read inside the binding because sequenceFor() is a plain call with no
    // dependency.
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.openSettings")]
        }
        enabled: root.visible
        onActivated: settingsSearchField.forceActiveFocus()
    }

    // Screenshot-demo hooks; inert in a non-demo build. The trust card needs no
    // handler (it renders whenever the section is "sessions"). See
    // docs/screenshot-demo.md.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoFocusSettingsSearch(query) {
            settingsSearchField.text = query
            settingsSearchField.forceActiveFocus()
        }
    }

    // Confirmation before clearing local GIF collections. `kind` keeps the
    // store token ("favorites" is app.gif.favorites); only the prose says
    // "saved". Also used for clearing the local message index.
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
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            // Says what is and is not lost: the index is a derived copy.
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
            font.pixelSize: AppTheme.textBody
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

    // Clearing the starred-GIF store deletes real decrypted files on disk, so
    // it has its own confirmation.
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
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("Delete every image you saved out of a chat from this "
                       + "device? Saved provider GIFs are unaffected — they "
                       + "are only links. This cannot be undone.")
        }
        onAccepted: app.gif.starredStore.clearAll()
    }

    // Clearing the display name only happens through this confirmation;
    // AppController refuses an emptied editor.
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
            font.pixelSize: AppTheme.textBody
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
        color: AppTheme.stormDeep
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Settings header: section icon, "Settings — <section>", close X. Same
        // height as the main screen's header band (AppTheme.headerBandHeight),
        // so opening Settings does not shift the top edge.
        Rectangle {
            objectName: "settingsHeaderBar"
            Layout.fillWidth: true
            implicitHeight: AppTheme.headerBandHeight
            color: AppTheme.stormDeep
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
                    text: qsTr("Settings — %1").arg(root.sectionTitle(root.section))
                    color: AppTheme.stormText
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
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.stormBorder }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Left navigation: 260px, icon rows, About pinned at the bottom.
            Rectangle {
                objectName: "settingsNavColumn"
                Layout.fillHeight: true
                Layout.preferredWidth: 260
                Layout.minimumWidth: 200
                color: AppTheme.stormCanvas
                // Nothing in this column may paint across the divider into the
                // content pane.
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: 2

                    // Pane title, sitting tight against the search field it
                    // labels.
                    RowLayout {
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
                            color: AppTheme.stormText
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                    }

                    // Search field with a trailing keycap. The margin below it
                    // is the one deliberate gap in this column: it separates
                    // search from the section list.
                    RowLayout {
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
                            onTextChanged: root.settingsSearchQuery = text
                            // Enter takes the first match, landing on the
                            // control like a click.
                            onAccepted: {
                                if (root.matchedSearchResults.length > 0)
                                    root.revealSearchResult(
                                        root.matchedSearchResults[0])
                            }
                        }
                        // From the registry, since the shortcut is rebindable.
                        // bindingRevision is read inside the binding because
                        // sequenceFor() creates no dependency.
                        MenuKeycap {
                            keys: {
                                var _rev = app.shortcuts.bindingRevision
                                return app.shortcuts.sequenceFor(
                                    "app.openSettings")
                            }
                        }
                    }

                    // Search results panel: replaces the nav list while
                    // searching.
                    ColumnLayout {
                        objectName: "settingsSearchResults"
                        visible: root.settingsSearchQuery.trim().length > 0
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing4

                        Label {
                            objectName: "settingsSearchNoResults"
                            visible: root.matchedSearchResults.length === 0
                            Layout.fillWidth: true
                            text: qsTr("No matching settings")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
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
                                // selectedHover at full strength, as for the
                                // nav row: stormSelection routes to `hover`
                                // outside Storm and is nearly invisible on this
                                // column in the light themes, and hover is a
                                // result's only state. `selected` is not usable
                                // either (it equals accentSoft on Moss Light).
                                // The quick-filter chips keep stormSelection
                                // because they hover on stormInset, where
                                // `hover` works.
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
                                            font.pixelSize: AppTheme.textBody
                                            font.weight: AppTheme.weightStrong
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: resultRow.modelData.breadcrumb
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.textMeta
                                            elide: Label.ElideRight
                                        }
                                        // A click anywhere in the text column
                                        // navigates; scoped away from the inline
                                        // controls below.
                                        TapHandler {
                                            onTapped: root.revealSearchResult(
                                                resultRow.modelData)
                                        }

                                        // Too wide for the space beside the title,
                                        // so it sits below it.
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

                                        // Below the title too, so every result row
                                        // has one shape.
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

                                    // Inline live controls, each bound two-way
                                    // to the same SettingsManager property as
                                    // its real control.
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
                                    // Every entry that names a `control` gets
                                    // one here, or it would advertise an inline
                                    // control it lacks.
                                    AppSwitch {
                                        objectName: "settingsSearchInlineShowMembership_" + resultRow.index
                                        visible: resultRow.modelData.control === "showMembershipEvents"
                                        // Follows the master switch, like the real
                                        // control.
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
                                }
                                HoverHandler { id: resultHover }
                                Accessible.role: Accessible.Button
                                Accessible.name: resultRow.modelData.title + " "
                                    + resultRow.modelData.breadcrumb
                            }
                        }

                        // Quick-filter chips seeding example queries.
                        Flow {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing4
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: [
                                    qsTr("theme"), qsTr("notifications"),
                                    qsTr("privacy"), qsTr("sessions"),
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
                                        // Ordinary UI text; mono is for code,
                                        // keycaps and Matrix IDs.
                                        font.pixelSize: AppTheme.textMeta
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

                    SettingsNavRow {
                        sectionKey: "account"
                        iconName: "account_circle"
                        navLabel: qsTr("Account")
                    }
                    SettingsNavRow {
                        sectionKey: "appearance"
                        iconName: "palette"
                        navLabel: qsTr("Appearance")
                    }
                    SettingsNavRow {
                        sectionKey: "shortcuts"
                        iconName: "keyboard_return"
                        navLabel: qsTr("Keyboard shortcuts")
                    }
                    SettingsNavRow {
                        sectionKey: "notifications"
                        iconName: "notifications"
                        navLabel: qsTr("Notifications")
                    }
                    SettingsNavRow {
                        sectionKey: "sound"
                        iconName: "volume_up"
                        navLabel: qsTr("Sound & video")
                    }
                    SettingsNavRow {
                        sectionKey: "privacy"
                        iconName: "verified_user"
                        navLabel: qsTr("Privacy & security")
                    }
                    SettingsNavRow {
                        sectionKey: "sessions"
                        iconName: "devices"
                        navLabel: qsTr("Sessions")
                        // Verification lives under Sessions, which the cog's
                        // badge points at.
                        alert: app.sessionVerificationWarning
                    }
                    SettingsNavRow {
                        sectionKey: "labs"
                        iconName: "science"
                        navLabel: qsTr("Labs")
                    }
                    SettingsNavRow {
                        sectionKey: "updates"
                        iconName: "download"
                        navLabel: qsTr("Updates")
                    }
                    Item { Layout.fillHeight: true }

                    // Match counter pinned at the nav bottom while searching.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        visible: root.settingsSearchQuery.trim().length > 0
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
                                return qsTr("Matches · %1 sections · %2 settings")
                                       .arg(sections)
                                       .arg(root.matchedSearchResults.length)
                            }
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightMedium
                            color: AppTheme.stormTextMuted
                            elide: Label.ElideRight
                        }
                    }

                    SettingsNavRow {
                        sectionKey: "about"
                        iconName: "info"
                        navLabel: qsTr("About")
                    }
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: AppTheme.stormBorder }

            // Right content pane, wrapped in an Item so the About page's Storm
            // Band can overlay the pane's bottom edge without changing the
            // Flickable's geometry.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
            Flickable {
                id: contentFlick
                objectName: "settingsContentFlick"
                anchors.fill: parent
                contentHeight: contentColumn.implicitHeight + AppTheme.spacing24 * 2
                clip: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the timeline (see
                // SmoothWheelArea.qml).
                SmoothWheelArea { id: settingsWheelArea }
                // Jump to the top when switching categories, stopping any glide
                // first.
                Connections {
                    target: root
                    function onSectionChanged() {
                        settingsWheelArea.stopGlide()
                        contentFlick.contentY = 0
                    }
                }

                // Search-reveal halo: rings the named control for about a
                // second so the click is acknowledged even when nothing needs
                // scrolling. One item for all anchors, drawn on top with
                // `enabled: false` so it never takes a press.
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

                    // ════════════ Appearance ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Appearance")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("Theme, message layout and text size — per account.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsGroupLabel { text: qsTr("Theme") }
                        // Four featured themes. Every swatch is read live from
                        // AppTheme.paletteForTheme(id), so the previews cannot
                        // drift from the real palettes: frame = background,
                        // rail = sidebar, bar1 = border, bar2 = surface, accent
                        // = accent.
                        Flow {
                            id: featuredThemeFlow
                            objectName: "featuredThemeFlow"
                            Layout.fillWidth: true
                            spacing: 14
                            // paletteForTheme() returns each theme's own
                            // literals, so a card previews its own theme, not
                            // the active one.
                            function previewFor(id, name) {
                                var p = AppTheme.paletteForTheme(id)
                                return { id: id, name: name,
                                         frame: p.background, rail: p.sidebar,
                                         bar1: p.border, bar2: p.surface,
                                         accent: p.accent }
                            }
                            Repeater {
                                // Indigo Night leads as the flagship; Storm
                                // stays featured as the brand theme.
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
                                    // Deliberately unguarded: with a typeof
                                    // guard, an undefined `app` at first
                                    // evaluation would register no dependency
                                    // on app.settings.theme and the ring would
                                    // stay wrong forever. The model is a
                                    // literal and delegates are built during
                                    // ordinary creation, so the failed-lookup
                                    // case does not apply.
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    // "Match system" is theme 0, which is no
                                    // card's id, yet it resolves to Moss Light
                                    // or Indigo Night
                                    // (AppTheme.effectiveTheme). Mark that card
                                    // as a third state: in effect -> bolt edge
                                    // + bolt ring; chosen -> bolt edge + filled
                                    // radio; neither -> quiet edge.
                                    readonly property bool inEffect:
                                        app.settings.theme === 0
                                        && AppTheme.effectiveTheme === modelData.id
                                    readonly property bool cardIsLive:
                                        selectedTheme || inEffect
                                    // Three 150px preview cards.
                                    implicitWidth: 150
                                    // Integral height keeps the card edge on
                                    // device pixels.
                                    implicitHeight: previewTop.height
                                                    + Math.ceil(cardFoot.height)
                                    radius: AppTheme.radiusLg
                                    // No clip: the selection glow and focus
                                    // ring are drawn outside the card, and a
                                    // rectangular scissor cannot round corners
                                    // anyway. stormPanel, not stormCanvas, for
                                    // the same reason as SettingsCard: outside
                                    // Storm the canvas equals the page
                                    // background.
                                    color: AppTheme.stormPanel
                                    // The outline is an overlay sibling below,
                                    // since the edge-filling children would
                                    // cover a border on this rectangle.
                                    border.width: 0
                                    Accessible.role: Accessible.RadioButton
                                    // An in-effect but unchosen card is marked
                                    // only by a ring, so the state goes in the
                                    // accessible name.
                                    Accessible.name: themeCard.inEffect
                                        ? qsTr("%1 (in effect)").arg(modelData.name)
                                        : modelData.name
                                    Accessible.checked: themeCard.selectedTheme
                                    Accessible.focusable: true
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: app.settings.theme = modelData.id
                                    Keys.onSpacePressed: app.settings.theme = modelData.id

                                    // Selected: a 3px accent glow at 18% alpha.
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
                                    // Keyboard focus ring.
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

                                    // Preview top: 96px in the previewed
                                    // theme's colours.
                                    Rectangle {
                                        id: previewTop
                                        objectName: "themeCardPreview_" + themeCard.modelData.id
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: 96
                                        color: themeCard.modelData.frame
                                        // Follow the card's rounded top corners.
                                        topLeftRadius: AppTheme.radiusLg - 1
                                        topRightRadius: AppTheme.radiusLg - 1
                                        // 10px padding, 26px mini rail, three
                                        // rounded bars at 70/50/60% width, the
                                        // last in the theme's accent.
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

                                    // Card bottom: raised background, radio,
                                    // theme name.
                                    Rectangle {
                                        id: cardFoot
                                        objectName: "themeCardFoot_"
                                                    + themeCard.modelData.id
                                        anchors.top: previewTop.bottom
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: footRow.implicitHeight + 20
                                        // Same plane as the card.
                                        color: AppTheme.stormPanel
                                        // Follow the card's rounded bottom
                                        // corners.
                                        bottomLeftRadius: AppTheme.radiusLg - 1
                                        bottomRightRadius: AppTheme.radiusLg - 1
                                        RowLayout {
                                            id: footRow
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            spacing: 8
                                            // A radio's resting ring is a control
                                            // boundary and needs 3:1 (WCAG 1.4.11).
                                            // stormTextFaint is the disabled ink and
                                            // fails on the light themes;
                                            // stormTextMuted clears 3:1 on every
                                            // theme.
                                            Rectangle {
                                                objectName: "themeCardRadio_"
                                                            + themeCard.modelData.id
                                                implicitWidth: 14
                                                implicitHeight: 14
                                                radius: 7
                                                // Filled only for a theme the user chose;
                                                // in effect gets the ring.
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
                                                font.pixelSize: AppTheme.textBody
                                                font.weight: AppTheme.weightStrong
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }

                                    // Card outline, above the edge-filling
                                    // children. Integer weights only: 1.5px
                                    // antialiases into two half-covered rows.
                                    Rectangle {
                                        objectName: "themeCardOutline_"
                                                    + themeCard.modelData.id
                                        anchors.fill: parent
                                        z: 5
                                        radius: AppTheme.radiusLg
                                        color: "transparent"
                                        // The resting edge must clear 3:1 against
                                        // arbitrary theme backgrounds. stormBorder
                                        // fails on every theme; stormTextMuted,
                                        // the same ink as the radio ring, clears
                                        // on all eleven.
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

                        // Compact row for the remaining presets.
                        SettingsGroupLabel { text: qsTr("More themes") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Repeater {
                                // 8-11 are the featured cards; 12 has its own
                                // row with the editor.
                                model: AppTheme.themeList.filter(
                                    (t) => t.id !== 8 && t.id !== 9 && t.id !== 10
                                           && t.id !== 11 && t.id !== 12)
                                delegate: Rectangle {
                                    id: miniThemeCard
                                    required property var modelData
                                    objectName: "miniThemeCard_" + modelData.id
                                    readonly property var pal:
                                        AppTheme.paletteForTheme(modelData.id)
                                    // Deliberately unguarded; see the featured
                                    // cards' selectedTheme.
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    implicitWidth: miniRow.implicitWidth + 24
                                    implicitHeight: 34
                                    radius: AppTheme.radiusTile
                                    // Hover and selected must differ.
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
                                            font.pixelSize: AppTheme.textMeta
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

                        // Custom theme: its own row rather than a card, since
                        // there is no palette to preview until one is made.
                        SettingsGroupLabel { text: qsTr("Custom theme") }
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

                                // Live swatch strip of the shell regions in
                                // window order.
                                Row {
                                    spacing: 2
                                    Repeater {
                                        model: app.customTheme.exists
                                               ? ["rail", "sidebar", "background",
                                                  "surface", "accent"]
                                               : []
                                        delegate: Rectangle {
                                            required property string modelData
                                            width: 10
                                            height: 28
                                            radius: 2
                                            color: {
                                                var o = app.customTheme.colors
                                                if (o && o[modelData] !== undefined)
                                                    return o[modelData]
                                                var pal = AppTheme.paletteForTheme(
                                                    app.customTheme.baseTheme)
                                                return pal[modelData] !== undefined
                                                       ? pal[modelData]
                                                       : AppTheme.stormTextMuted
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Label {
                                        // The active custom theme's name once it
                                        // has one.
                                        text: !app.customTheme.exists
                                              ? qsTr("Build your own theme")
                                              : app.customTheme.name.length > 0
                                                ? app.customTheme.name
                                                : qsTr("Your theme")
                                        textFormat: Text.PlainText
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        text: !app.customTheme.exists
                                              ? qsTr("Pick a colour for any part of the window and watch a sample room repaint.")
                                              : app.customTheme.themes.length > 1
                                                ? qsTr("%n colour(s) changed. %1 themes saved.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                                      .arg(app.customTheme.themes.length)
                                                : qsTr("%n colour(s) changed. Pick a colour for any part of the window.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                }

                                AppButton {
                                    objectName: "customThemeEditButton"
                                    kind: app.customTheme.exists ? "secondary"
                                                                 : "primary"
                                    storm: true
                                    text: app.customTheme.exists ? qsTr("Edit")
                                                                 : qsTr("Create")
                                    // Opening the editor does not select the
                                    // theme: its preview paints the custom
                                    // palette by id, and the editor has its own
                                    // "Use this theme" button.
                                    onClicked: themeEditorLoader.active = true
                                }
                            }
                        }

                        // Loaded on demand: the editor carries a full preview
                        // shell and a colour dialog.
                        Loader {
                            id: themeEditorLoader
                            objectName: "themeEditorLoader"
                            active: false
                            sourceComponent: ThemeEditorDialog {}
                            onLoaded: item.open()
                            Connections {
                                target: themeEditorLoader.item
                                function onClosed() {
                                    themeEditorLoader.active = false
                                }
                            }
                        }

                        // Match-system row: 36×20 switch, 16px white thumb,
                        // 150ms travel; the whole row is clickable.
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
                                // Drawn like AppSwitch but cannot be one:
                                // AppSwitch has its own TapHandler, and nested
                                // inside this AbstractButton it would toggle
                                // twice per click. Its colours must match
                                // AppSwitch's.
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
                                            // accentHover: `bolt` is the routed
                                            // accent, so this is the hovered bolt on
                                            // Storm and each theme's own hover
                                            // elsewhere.
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
                                        // The checked track is bolt, so the thumb
                                        // uses boltInk (designed to sit on bolt);
                                        // the unchecked track keeps a white thumb.
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
                                    font.pixelSize: AppTheme.textBody
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
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("When on, Lightning follows the system scheme: "
                                       + "Moss Light in light mode, Indigo Night in dark mode.")
                        }

                        SettingsGroupLabel { text: qsTr("Conversation list") }
                        RowLayout {
                            objectName: "roomNavigationLayoutCards"
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12

                            NavigationLayoutCard {
                                objectName: "navLayoutClassicCard"
                                Layout.fillWidth: true
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
                            font.pixelSize: AppTheme.textMeta
                            // Describes what the layout is.
                            text: qsTr("Channels lists every space you are in as "
                                       + "a folder, with the rooms it contains "
                                       + "underneath. Rooms in no space, and your "
                                       + "direct messages, stay together in Rooms.")
                        }

                        SettingsGroupLabel { text: qsTr("Message layout") }
                        SegmentedControl {
                            storm: true
                            objectName: "messageLayoutControl"
                            model: [
                                { label: qsTr("Modern"), value: 0 },
                                { label: qsTr("Bubbles"), value: 1 },
                                { label: qsTr("Compact"), value: 2 },
                            ]
                            current: app.settings.messageLayout
                            onActivated: (value) =>
                                app.settings.messageLayout = value
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Bubbles applies to direct messages; rooms keep "
                                       + "the Modern rows. Compact tightens every timeline.")
                        }

                        SettingsGroupLabel { text: qsTr("Text size") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            // The small/large A caps illustrate the range; they
                            // are not scale sizes and must not be tokenised.
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 12
                            }
                            Slider {
                                id: textScaleSlider
                                objectName: "textScaleSlider"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 90
                                to: 140
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.textScale
                                onMoved: app.settings.textScale = Math.round(value)
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
                                    // Always white: a slider thumb rides the
                                    // fill boundary, so a dark boltInk disc
                                    // would read as disabled. White reads on
                                    // the groove, the fill edge and every
                                    // palette.
                                    color: "#FFFFFF"
                                    // One of the four shadows the design
                                    // allows.
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.topMargin: 1
                                        anchors.bottomMargin: -1
                                        radius: 8
                                        z: -1
                                        color: "#40000000"
                                    }
                                    border.width: textScaleSlider.visualFocus ? 2 : 0
                                    border.color: AppTheme.bolt
                                }
                            }
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 18
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Scales message and list text, and the "
                                       + "Spaces rail with it, so its nesting "
                                       + "levels stay readable at any size. "
                                       + "Other chrome keeps its size — "
                                       + "Interface zoom below scales the "
                                       + "whole window.")
                        }

                        // Interface zoom: whole-UI scale via QT_SCALE_FACTOR,
                        // which Qt reads once at startup, hence the restart
                        // caption.
                        SettingsGroupLabel { text: qsTr("Interface zoom") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Slider {
                                id: interfaceZoomSlider
                                objectName: "interfaceZoomSlider"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 75
                                to: 150
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.interfaceZoom
                                onMoved: app.settings.interfaceZoom
                                         = Math.round(value)
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
                                    // Always white, like the text-size thumb.
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
                                                  ? 2 : 0
                                    border.color: AppTheme.bolt
                                }
                            }
                            Label {
                                text: app.settings.interfaceZoom + "%"
                                color: AppTheme.stormTextMuted
                                // Mono for a live numeric readout that must not
                                // reflow.
                                font.pixelSize: AppTheme.textMeta
                                font.family: AppTheme.monoFont
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Scales the entire interface — text, "
                                       + "icons and layout. Ctrl+= and Ctrl+- "
                                       + "adjust it anywhere. Takes effect the "
                                       + "next time Lightning starts.")
                        }

                        // UI font (bundled OFL families).
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
                                    // Same forced-height padding fix as
                                    // SettingsNavRow.
                                    topPadding: 0
                                    bottomPadding: 0
                                    Accessible.name:
                                        qsTr("Use the %1 font").arg(modelData)
                                    onClicked:
                                        app.settings.uiFont = modelData
                                    background: Rectangle {
                                        radius: AppTheme.radiusMd
                                        // Selection at full strength, hover at 55%
                                        // of it (as AppComboBox), so the selection
                                        // does not appear to follow the pointer.
                                        color: fontRow.selected
                                               ? AppTheme.stormSelection
                                               : (fontRow.hovered || fontRow.down)
                                                 ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                                 : AppTheme.stormInset
                                        border.width: 1
                                        // Selection also carries a bolt edge, not
                                        // just a darker grey.
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
                                                font.pixelSize: AppTheme.textBody
                                                font.weight: AppTheme.weightStrong
                                                color: AppTheme.stormText
                                            }
                                            // The sample previews the actual family.
                                            Label {
                                                text: qsTr("Messages, rooms and settings")
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.textBody
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
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Applies to the whole interface. Code, Matrix "
                                       + "IDs, icons, and emoji keep their own fonts.")
                        }

                        // Any installed family, the fixed-pitch subset for
                        // code, and hand-imported fonts. The combos show the
                        // stored choice, not the resolved font: an uninstalled
                        // font keeps showing as selected with a "missing" line,
                        // rather than looking like the setting reset itself.
                        SettingsCard {
                            objectName: "systemFontCard"
                            visible: root.fontManager !== null
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    text: qsTr("Interface font")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    id: systemUiFontCombo
                                    objectName: "systemUiFontCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    model: root.fontManager ? root.fontManager.uiFamilies : []
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Says the choice is kept.
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    id: monoFontCombo
                                    objectName: "monoFontCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    model: root.fontManager ? root.fontManager.monospaceFamilies : []
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
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    // Says plainly that the file is copied.
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
                                            font.pixelSize: AppTheme.textBody
                                            // Show family names; the stored file name
                                            // is a hash.
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
                                    font.pixelSize: AppTheme.textMeta
                                    // One sentence per refusal category (a C++
                                    // constant). The rejected file name is not
                                    // echoed back.
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
                                FileDialog {
                                    id: fontFileDialog
                                    title: qsTr("Choose a font file")
                                    fileMode: FileDialog.OpenFile
                                    nameFilters: [
                                        qsTr("Fonts (*.ttf *.otf)")
                                    ]
                                    onAccepted: root.fontManager.importFontFile(selectedFile)
                                }
                            }
                        }

                        // Panel visibility, mirroring Ctrl+B / Ctrl+Shift+B so
                        // a panel hidden by shortcut can be found again here.
                        SettingsGroupLabel { text: qsTr("Panels") }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showSpacesRailCheck"
                                    text: qsTr("Show the Spaces rail (Ctrl+Shift+B)")
                                    checked: app.settings.spacesRailVisible
                                    onToggled: app.settings.spacesRailVisible = checked
                                    Accessible.description: qsTr(
                                        "Show the narrow strip of Spaces down the far edge")
                                }
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Drag the line between two panels to "
                                               + "resize them. Widths are remembered.")
                                }

                                // How the Spaces rail shows nesting, next to
                                // the rail's own settings. Enabled only while
                                // the rail is shown.
                                Label {
                                    objectName: "spacesRailDepthLabel"
                                    Layout.topMargin: AppTheme.spacing4
                                    Layout.leftMargin: AppTheme.spacing4
                                    text: qsTr("Spaces rail depth")
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                }
                                SegmentedControl {
                                    storm: true
                                    objectName: "spacesRailDepthControl"
                                    // Align the ink, not the box: a segment's
                                    // first glyph sits 12px inside its edge
                                    // (SegmentedControl pads its text by 12),
                                    // so pull the box back by that.
                                    // theRailDepthControlLinesUpWithItsOwnLabel
                                    // checks real ink positions.
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
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Regions tint the rail behind a Space "
                                               + "and everything inside it. Classic is "
                                               + "the flat rail from earlier versions, "
                                               + "where a nested Space steps in instead. "
                                               + "Both show the same Spaces.")
                                }
                            }
                        }

                        // System tray. Hidden where the platform has none,
                        // since "close to tray" would close the window into
                        // nothing.
                        SettingsGroupLabel {
                            visible: app.trayAvailable
                            text: qsTr("System tray")
                        }
                        SettingsCard {
                            visible: app.trayAvailable
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "closeToTrayCheck"
                                    text: qsTr("Keep running in the tray when the window is closed")
                                    checked: app.settings.closeToTray
                                    onToggled: app.settings.closeToTray = checked
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "startInTrayCheck"
                                    text: qsTr("Start in the tray")
                                    enabled: app.settings.closeToTray
                                    checked: app.settings.startInTray
                                    onToggled: app.settings.startInTray = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Click the tray icon to bring the window "
                                               + "back. Ctrl+Q quits.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Timeline")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Hide routine joins, leaves, profile changes, "
                                               + "and room setting updates. Messages and "
                                               + "decryption warnings remain visible.")
                                }
                                // The two halves of "room activity", indented
                                // and disabled with the master.
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showMembershipEventsCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.showRoomActivity
                                    text: qsTr("Joins, leaves and invites")
                                    checked: app.settings.showMembershipEvents
                                    onToggled: app.settings.showMembershipEvents = checked
                                    Accessible.description: qsTr(
                                        "Show membership changes in timelines")
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showProfileChangeEventsCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.showRoomActivity
                                    text: qsTr("Display name and avatar changes")
                                    checked: app.settings.showProfileChangeEvents
                                    onToggled: app.settings.showProfileChangeEvents = checked
                                    Accessible.description: qsTr(
                                        "Show profile changes in timelines")
                                }
                                // In the Timeline card rather than Privacy: it
                                // changes nothing fetched or sent, only
                                // density.
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Names what it covers and what it does not
                                    // (reply quotes stay).
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
                                    text: qsTr("Mouse-wheel speed")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                }
                                AppComboBox {
                                    storm: true
                                    id: wheelSpeedCombo
                                    objectName: "timelineWheelSpeedCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    // Values map to
                                    // TimelineScrollController::WheelSpeed.
                                    model: [
                                        { label: qsTr("Standard"),  value: 0 },
                                        { label: qsTr("Fast"),      value: 1 },
                                        { label: qsTr("Very fast"), value: 2 }
                                    ]
                                    // indexOfValue() only resolves once the
                                    // model is ready; sync on completion and on
                                    // change.
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("How far one physical mouse-wheel notch moves "
                                               + "the timeline. Touchpad and precision scrolling "
                                               + "stay fine-grained regardless of this setting.")
                                }
                            }
                        }

                        // Custom application icon: validated raster, normalized
                        // to a circle, applied immediately and restored at
                        // startup. Device-global.
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Application icon")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
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
                                FileDialog {
                                    id: appIconDialog
                                    title: qsTr("Choose an application icon image")
                                    fileMode: FileDialog.OpenFile
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

                    // ════════════ Appearance (continued: language)
                    // ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Language")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                }
                                AppComboBox {
                                    id: languageCombo
                                    objectName: "languageCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    model: app.localization.languages
                                    // The language's own name, so someone who
                                    // cannot read the current UI language can
                                    // still find theirs.
                                    textRole: "endonym"
                                    valueRole: "code"
                                    enabled: app.localization.translationsAvailable

                                    // indexOfValue() is -1 at creation; sync
                                    // explicitly here and on change.
                                    function syncIndex() {
                                        syncToValue(app.localization.language)
                                    }
                                    Component.onCompleted: syncIndex()
                                    onModelChanged: Qt.callLater(syncIndex)
                                    Connections {
                                        target: app.localization
                                        function onLanguageChanged() {
                                            Qt.callLater(languageCombo.syncIndex)
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
                                    // Three distinct cases: no catalogs in this
                                    // build, "System default" resolved to
                                    // something, or an explicit choice.
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
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                        }

                    }

                    // ════════════ Appearance (continued: motion, time, panels,
                    // composer) ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Motion and time")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "reducedMotionCheck"
                                    text: qsTr("Reduce motion")
                                    checked: app.settings.reducedMotion
                                    onToggled: app.settings.reducedMotion = checked
                                    Accessible.description: qsTr(
                                        "Shorten or remove interface animations")
                                }
                                // Separate from Reduce motion, which is an
                                // accessibility setting over every animation.
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Does not claim "no animation anywhere":
                                    // video in the timeline is content.
                                    text: qsTr("Shortens or removes interface "
                                               + "animations — panel slides, "
                                               + "fades and list reorders. "
                                               + "Media you play still plays.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Clock")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                }
                                AppComboBox {
                                    id: clockFormatCombo
                                    objectName: "clockFormatCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    // Values match SettingsManager's
                                    // kClockFormat* constants.
                                    model: [
                                        { label: qsTr("Follow the system"), value: 0 },
                                        { label: qsTr("12-hour (1:05 PM)"),  value: 1 },
                                        { label: qsTr("24-hour (13:05)"),    value: 2 }
                                    ]
                                    // indexOfValue() is -1 at creation; sync
                                    // explicitly.
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

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Panels")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                // These settings could otherwise only be
                                // changed from the banners themselves.
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spaceBannersVisibleCheck"
                                    text: qsTr("Show Space banners")
                                    checked: app.settings.spaceBannersVisible
                                    onToggled: app.settings.spaceBannersVisible = checked
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spaceBannerExpandedCheck"
                                    Layout.leftMargin: AppTheme.spacing16
                                    enabled: app.settings.spaceBannersVisible
                                    text: qsTr("Show the whole banner instead of a strip")
                                    checked: app.settings.spaceBannerExpanded
                                    onToggled: app.settings.spaceBannerExpanded = checked
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Conversation list width: %1 px")
                                        .arg(app.settings.roomListWidth)
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                }
                                SettingsSlider {
                                    objectName: "roomListWidthSlider"
                                    Layout.fillWidth: true
                                    // Bounds come from SettingsManager: a
                                    // narrower range would silently forbid
                                    // supported widths and misplace the handle
                                    // for a stored value.
                                    from: app.settings.roomListMinWidth
                                    to: app.settings.roomListMaxWidth
                                    stepSize: 10
                                    value: app.settings.roomListWidth
                                    onMoved: app.settings.roomListWidth = Math.round(value)
                                    Accessible.name: qsTr("Conversation list width")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Side panel width: %1 px")
                                        .arg(app.settings.sidePanelWidth)
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Message box")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    // States both keys in both states.
                                    text: app.settings.enterInsertsNewline
                                        ? qsTr("Enter starts a new line; Ctrl+Enter sends.")
                                        : qsTr("Enter sends; Shift+Enter starts a new line.")
                                }
                                // Spell checking: an app preference independent
                                // of the UI language. Uses the OS engine; says
                                // so when it has no dictionary.
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "spellCheckEnabledCheck"
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Check spelling as you type")
                                    checked: app.settings.spellCheckEnabled
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
                                        font.pixelSize: AppTheme.textBody
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
                                        Accessible.name: qsTr("Spelling language")
                                        // Never bind currentIndex; see
                                        // AppComboBox.
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: {
                                        if (!app.spell)
                                            return ""
                                        if (app.spell.available) {
                                            var engine = app.spell.backendName === "windows"
                                                ? qsTr("Windows spell checker")
                                                : app.spell.backendName === "macos"
                                                  ? qsTr("macOS spell checker")
                                                  : qsTr("Enchant (system dictionaries)")
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
                                            return qsTr("Spell checking is unavailable because the enchant-2 spelling library is not installed on this system.")
                                        }
                                        var wanted = app.settings.spellCheckLanguage.length > 0
                                            ? app.settings.spellCheckLanguage : qsTr("your system language")
                                        var how = os === "windows"
                                            ? qsTr("Add the language under Windows Settings › Time & language › Language to install its spelling.")
                                            : os === "osx"
                                              ? qsTr("Add the language under System Settings › Keyboard › Text Input.")
                                              : qsTr("Install your distribution's spelling dictionary for that language (usually a hunspell package). Inside Flatpak or Snap only the runtime's own dictionaries are visible.")
                                        return qsTr("Spell checking is unavailable because no system dictionary is installed for %1.").arg(wanted) + " " + how
                                    }
                                }
                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Honest about interop: clients that ignore
                                    // the caption field show the file without
                                    // text.
                                    text: qsTr("One event instead of two. Clients "
                                               + "that do not understand captions "
                                               + "show the attachment without the "
                                               + "text.")
                                }

                                // Message box buttons. Presented as "show",
                                // stored as "hidden"
                                // (SettingsManager::hiddenComposerButtons) so
                                // buttons added later appear for everyone.
                                // Attach is not hideable: in a narrow window
                                // other actions move into its menu.
                                Label {
                                    objectName: "composerButtonsHeading"
                                    Layout.topMargin: AppTheme.spacing16
                                    Layout.leftMargin: AppTheme.spacing4
                                    text: qsTr("Message box buttons")
                                    // Must not out-rank the card's "Message
                                    // box" heading.
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                    CheckBox {
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
                    }

                    // ════════════ Keyboard shortcuts ════════════ Every
                    // rebindable action from ShortcutRegistry, grouped by
                    // category. A Repeater over the model, so actions added in
                    // C++ appear without QML changes.
                    ColumnLayout {
                        visible: root.section === "shortcuts"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            objectName: "shortcutsHeading"
                            text: qsTr("Keyboard shortcuts")
                            color: AppTheme.stormText
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
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                        }

                        // Only when something is wrong. setBinding refuses to
                        // create a conflict, so a non-zero count means a
                        // hand-edited or newer settings file.
                        Loader {
                            Layout.fillWidth: true
                            active: app.shortcuts.conflictCount > 0
                            visible: active
                            sourceComponent: Rectangle {
                                objectName: "shortcutConflictBanner"
                                implicitHeight: conflictBannerLabel.implicitHeight
                                                + AppTheme.spacing12 * 2
                                radius: AppTheme.radiusMd
                                // Storm danger tokens: this file uses only the
                                // storm vocabulary (enforced by
                                // ThemeTokensTest).
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Some shortcuts share a key. Qt fires "
                                               + "neither of two shortcuts on the same "
                                               + "combination, so both actions are "
                                               + "currently doing nothing. Change one "
                                               + "of each pair, or reset everything.")
                                }
                            }
                        }

                        Repeater {
                            model: app.shortcuts.categories()
                            delegate: SettingsCard {
                                id: shortcutCategoryCard
                                required property string modelData
                                ColumnLayout {
                                    width: parent.width
                                    spacing: AppTheme.spacing8
                                    Label {
                                        text: shortcutCategoryCard.modelData
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Repeater {
                                        model: app.shortcuts
                                        delegate: ShortcutRow {
                                            // ShortcutRegistry::roleNames prefixes its
                                            // roles so they can be assigned to this
                                            // component's like-named properties.
                                            actionId: model.shortcutId
                                            description: model.shortcutDescription
                                            currentSequence: model.shortcutCurrent
                                            defaultSequence: model.shortcutDefault
                                            isDefault: model.shortcutIsDefault
                                            conflictsWith: model.shortcutConflict
                                            shadowNote: model.shortcutShadow
                                            // Each category card renders the whole
                                            // model and hides other categories' rows;
                                            // invisible items take no space in the
                                            // layout.
                                            visible: model.shortcutCategory
                                                     === shortcutCategoryCard.modelData
                                            Layout.fillWidth: true
                                            // Stack the name above the keycap when the
                                            // pane is too narrow. Uses the pane's
                                            // width, not the row's (see ShortcutRow).
                                            // 560 fits the longest description beside
                                            // a 132px keycap and two buttons.
                                            compact: contentColumn.width < 560
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Explains why a bare letter is refused and
                                    // why Ctrl+B does two things.
                                    text: qsTr("A shortcut needs Ctrl, Alt or Super: "
                                               + "Lightning takes the key before any "
                                               + "text box sees it, so a plain letter "
                                               + "would stop you typing that letter "
                                               + "anywhere.\n\n"
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

                    // ════════════ Privacy & security ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Privacy & security")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Network privacy, encryption health, and recovery.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        // MSC4153. Under Privacy because it is a choice about
                        // who you talk to, and can make messages unreadable.
                        Label {
                            text: qsTr("Device trust")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    objectName: "strictDeviceTrustCheck"
                                    palette.windowText: AppTheme.stormText
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
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    // The SDK reads this when building a client
                                    // and cannot change it afterwards.
                                    text: qsTr("Takes effect the next time "
                                               + "Lightning starts.")
                                }
                            }
                        }

                        // What this device discloses while reading and typing.
                        Label {
                            text: qsTr("Reading and typing")
                            color: AppTheme.stormText
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "readReceiptModeCombo"
                                    Layout.fillWidth: true
                                    model: [
                                        qsTr("Send read receipts"),
                                        qsTr("Private — only my own devices"),
                                        qsTr("Do not send read receipts")
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
                                    font.pixelSize: AppTheme.textMeta
                                    // The fully-read marker is account data and
                                    // still syncs; only the receipt stops, so
                                    // badges on other devices stop clearing.
                                    text: qsTr("Private receipts still clear the "
                                               + "unread badge on your own other "
                                               + "devices; nobody else sees them. "
                                               + "Not sending them at all means your "
                                               + "other devices stop clearing it too, "
                                               + "though your place in a conversation "
                                               + "is still saved. Either way, receipts "
                                               + "you have already sent cannot be "
                                               + "taken back, and other people's "
                                               + "receipts are still shown to you.")
                                }

                                CheckBox {
                                    objectName: "sendTypingCheck"
                                    palette.windowText: AppTheme.stormText
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Typing notices are the most frequent "
                                               + "thing a chat client discloses — "
                                               + "every few keystrokes, to everyone "
                                               + "in the room. Turning this off does "
                                               + "not hide other people's.")
                                }
                            }
                        }

                        // Own presence publication, offered when the backend
                        // supports presence. Gated on capability, not on the
                        // read-refusal latch: publication continues when reads
                        // are refused, so its off switch must never disappear.
                        Label {
                            visible: app.presence && app.presence.supported
                            text: qsTr("Presence")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            visible: app.presence && app.presence.supported
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "sharePresenceCheck"
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Tells your homeserver when you are online or "
                                               + "idle, so people you share rooms with can see "
                                               + "it. Turning this off publishes offline once "
                                               + "and stops updates; whether others' status is "
                                               + "visible to you is decided by their servers, "
                                               + "not by this switch.")
                                }
                            }
                        }

                        // Ignored users (m.ignored_user_list, shared with every
                        // client).
                        Label {
                            visible: app.moderation.supported
                            text: qsTr("Ignored users")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            id: ignoredUsersCard
                            objectName: "ignoredUsersCard"
                            visible: app.moderation.supported
                            // ModerationController reports outcomes on
                            // ignoreActionFinished, whose only other consumer
                            // (MemberProfilePopover) filters by its own user,
                            // so show refusals here.
                            property string unignoreError: ""
                            // The user this card asked about, so failures from
                            // the profile popover do not surface here too.
                            property string unignoreUserId: ""
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Connections {
                                    target: app.moderation
                                    function onIgnoreActionFinished(
                                        userId, ignored, ok, message) {
                                        if (userId !== ignoredUsersCard.unignoreUserId)
                                            return
                                        ignoredUsersCard.unignoreUserId = ""
                                        ignoredUsersCard.unignoreError =
                                            ok ? "" : message
                                    }
                                }

                                Label {
                                    visible: app.moderation.ignoredUsers.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("Nobody is ignored. Ignore a "
                                               + "person from their profile "
                                               + "to hide their messages "
                                               + "everywhere, on every "
                                               + "device.")
                                }
                                Repeater {
                                    model: app.moderation.ignoredUsers
                                    delegate: RowLayout {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData
                                            color: AppTheme.stormText
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.textBody
                                            elide: Label.ElideRight
                                        }
                                        AppButton {
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: ignoredUsersCard.unignoreError
                                }
                                Label {
                                    visible: app.moderation.ignoredUsers.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                            color: AppTheme.stormText
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
                                    // Body leading, matching the other
                                    // paragraphs on this page.
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
                                    font.pixelSize: AppTheme.textMeta
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
                                        font.pixelSize: AppTheme.textBody
                                    }
                                    AppButton {
                                        objectName: "clearSearchIndexButton"
                                        text: qsTr("Clear index")
                                        kind: "danger"
                                        enabled: app.messageSearch.indexedMessages > 0
                                        onClicked: clearIndexConfirm.visible = true
                                    }
                                }
                            }
                        }
                        // Link-preview and GIF policy.
                        Label {
                            text: qsTr("Link previews & media")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    id: autoPreviewCheck
                                    objectName: "autoPreviewCheck"
                                    text: qsTr("Automatically load previews in unencrypted rooms")
                                    checked: app.settings.autoLoadLinkPreviews
                                    onToggled: app.settings.autoLoadLinkPreviews = checked
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "encryptedPreviewCheck"
                                    text: qsTr("Load previews in encrypted rooms")
                                    checked: app.settings.loadPreviewsInEncryptedRooms
                                    onToggled: app.settings.loadPreviewsInEncryptedRooms = checked
                                }
                                // Privacy caution: the danger rule carries the
                                // caution so the copy stays readable body ink.
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
                                        font.pixelSize: AppTheme.textMeta
                                        // Previews go through the homeserver first
                                        // and fall back to a direct fetch only
                                        // when it cannot supply one (Synapse
                                        // disables previews by default), so the
                                        // text still describes the direct case and
                                        // the switches stay off by default. Names
                                        // the real control, the link card's Show
                                        // button.
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

                                // ─────────── GIFs ───────────
                                Label {
                                    text: qsTr("GIFs")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }

                                Label { text: qsTr("Autoplay and prefetch media"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifAutoplayCombo
                                    objectName: "gifAutoplayCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Autoplay and prefetch media")
                                    model: [
                                        { label: qsTr("Always"),   value: 0 },
                                        { label: qsTr("On hover"), value: 1 },
                                        { label: qsTr("Never"),    value: 2 },
                                    ]
                                    // Not a currentIndex binding and not
                                    // Math.max(0, indexOfValue(...)): at
                                    // creation indexOfValue() is -1, which
                                    // clamps to row 0 and displays the wrong
                                    // value. syncToValue retries instead.
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
                                    text: qsTr("Also governs passive downloads: GIF, video and audio prefetching. \"Never\" disables all of them.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    Layout.fillWidth: true
                                }

                                Label { text: qsTr("GIF safe search"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifRatingCombo
                                    objectName: "gifRatingCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("GIF safe search rating")
                                    // Values map to gif::Rating (0=g … 3=r).
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
                                // Per-provider availability.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("GIPHY: %1 · KLIPY: %2")
                                        .arg(app.gif.providerConfigured("giphy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                        .arg(app.gif.providerConfigured("klipy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                }

                                CheckBox {
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
                                    font.pixelSize: AppTheme.textMeta
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

                                // Client-local GIF starring stores actual
                                // decrypted file bytes (see GifStarredStore),
                                // so it gets its own count, size and confirmed
                                // Clear All.
                                Label {
                                    text: qsTr("Images saved from chats")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
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

                                // Hidden images persist across sessions, so the
                                // count and a Show-all must be discoverable
                                // here.
                                Label {
                                    text: qsTr("Hidden images")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Branched rather than a %n plural: without
                                    // a loaded translation "(s)" renders
                                    // literally.
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
                            }
                        }
                    }

                    // ════════════ Notifications ════════════
                    ColumnLayout {
                        visible: root.section === "notifications"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Notifications")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "notificationsEnabledCheck"
                                    text: qsTr("Desktop notifications")
                                    checked: app.settings.notificationsEnabled
                                    onToggled: app.settings.notificationsEnabled = checked
                                }
                                // Notification privacy.
                                Label {
                                    text: qsTr("Notification preview")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
                                    // States what each mode does and which is
                                    // the default:
                                    // SettingsManager::notificationPreview()
                                    // defaults to 0 (Sender and message).
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationPreviewEncryptedCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    // Index 3, "same as above", is the default,
                                    // so upgrades change nothing.
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
                                // Notification sound.
                                Label {
                                    text: qsTr("Notification sound")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("The sound plays only when a "
                                               + "notification is shown, so muted and "
                                               + "active rooms stay silent. Bursts are "
                                               + "coalesced into a single alert.")
                                }
                                // Call devices live under "Sound & video"; the
                                // ring toggle stays here, gated on desktop
                                // notifications.
                                CheckBox {
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
                                             + "ringing. Turning this off "
                                             + "still shows the call — it "
                                             + "only silences the ring.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    // The Rust backend saves per-room modes to
                                    // the server's push rules; other backends
                                    // keep them device-local.
                                    text: (app.serverRoomNotificationModes
                                           ? qsTr("Per-room notification modes (set "
                                                  + "from Room information) are saved "
                                                  + "to your account's notification "
                                                  + "settings (server push rules). ")
                                           : qsTr("Per-room notification modes (set "
                                                  + "from Room information) apply to "
                                                  + "this device only — they are not "
                                                  + "server push rules. "))
                                          + qsTr("Push registration for "
                                                 + "mobile-style notifications is not "
                                                 + "implemented.")
                                }
                            }
                        }
                    }

                    // ════════════ Sound & video ════════════ Capture, playback
                    // and levels. There is exactly one callDeviceSettings in
                    // this file. Named "Sound & video" because the same
                    // component owns the camera picker.
                    ColumnLayout {
                        visible: root.section === "sound"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Sound & video")
                            color: AppTheme.stormText
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
                                    // Enumeration initialises Qt Multimedia,
                                    // which is slow on PipeWire, so wait until
                                    // this section is on screen.
                                    activated: visible
                                }
                            }
                        }

                        Label {
                            text: qsTr("Media playback")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: 4

                                // The starting level for voice messages, audio
                                // and video; the same two-way SettingsManager
                                // binding as the other controls here.
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
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                    Label {
                                        objectName: "mediaVolumeSettingReadout"
                                        text: Math.round(
                                            mediaVolumeSettingSlider.value * 100) + "%"
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                }
                                SettingsSlider {
                                    id: mediaVolumeSettingSlider
                                    objectName: "mediaVolumeSettingSlider"
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 1
                                    stepSize: 0.05
                                    // A plain binding, so a level chosen on a
                                    // media card shows here. Qt breaks it on
                                    // the first drag, as with the other
                                    // sliders.
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("The level voice messages, audio files and videos start at. Changing the volume on a player remembers it here too.")
                                }
                            }
                        }

                        // Call sounds. The policy is CallSoundPolicy in C++;
                        // this is only the switches and two volumes.
                        Label {
                            text: qsTr("Call sounds")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: 4

                                CheckBox {
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
                                CheckBox {
                                    objectName: "callSoundsPresenceCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("People joining and leaving")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsPresence
                                    onToggled:
                                        app.settings.callSoundsPresence = checked
                                }
                                CheckBox {
                                    objectName: "callSoundsControlsCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Muting and deafening")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsControls
                                    onToggled:
                                        app.settings.callSoundsControls = checked
                                }
                                CheckBox {
                                    objectName: "callSoundsShareAndHandCheck"
                                    Layout.leftMargin: AppTheme.spacing24
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Screen shares and raised hands")
                                    enabled: app.settings.callSoundsEnabled
                                    checked: app.settings.callSoundsShareAndHand
                                    onToggled:
                                        app.settings.callSoundsShareAndHand = checked
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Call sound volume")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                    Label {
                                        text: Math.round(
                                            callSoundVolumeSlider.value) + "%"
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                    AppButton {
                                        objectName: "callSoundPreviewButton"
                                        storm: true
                                        kind: "ghost"
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
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                    Label {
                                        text: Math.round(
                                            ringerVolumeSlider.value) + "%"
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightMedium
                                    }
                                    AppButton {
                                        objectName: "ringerPreviewButton"
                                        storm: true
                                        kind: "ghost"
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("These sounds play only on this computer; nobody else in the call hears them. While you are deafened, only your own actions make a sound. Whether a call rings at all is set under Notifications.")
                                }
                            }
                        }
                    }

                    // ════════════ Account ════════════
                    ColumnLayout {
                        visible: root.section === "account"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Account")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        // Account header in the identity-card idiom (avatar,
                        // display name, mono MXID, status chip) in the active
                        // theme.
                        SettingsCard {
                            id: accountIdentityCard
                            objectName: "accountIdentityCard"
                            // Invokable results do not re-evaluate on signals;
                            // refresh the record when the registry or selection
                            // changes, so a late profile does not leave the
                            // MXID as the name.
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
                                    accountIdentityCard.refreshAccountRecord()
                                }
                                function onActiveUserIdChanged() {
                                    accountIdentityCard.refreshAccountRecord()
                                }
                            }
                            readonly property string accountDisplayName:
                                accountRecord && accountRecord.displayName
                                ? accountRecord.displayName : ""

                            // Own display name
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
                                // Unchanged is not a save: the server would
                                // answer successfully, the registry would emit
                                // nothing, and a UI waiting for accountsChanged
                                // would hang.
                                if (wanted === accountIdentityCard.accountDisplayName) {
                                    editingDisplayName = false
                                    app.dismissOwnDisplayNameError()
                                    return
                                }
                                // A refusal leaves the editor open with the
                                // reason in app.ownDisplayNameError.
                                app.submitOwnDisplayName(wanted)
                            }
                            Connections {
                                target: app
                                // Server-confirmed only; nothing else closes
                                // the editor.
                                function onOwnDisplayNameSaved() {
                                    accountIdentityCard.editingDisplayName = false
                                }
                                // Session teardown closes the editor.
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
                                                label: app.sessionTrustState
                                                iconName: app.sessionTrustState === "Verified"
                                                          ? "verified_user" : ""
                                                // Same mapping as the Sessions "Current
                                                // session" chip.
                                                tone: app.sessionTrustState === "Verified"
                                                      ? "success"
                                                      : app.sessionTrustState === "Not verified"
                                                        ? "danger" : "neutral"
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        // Copyable: the MXID is elided and is the
                                        // string people share to be found.
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing4
                                            Label {
                                                Layout.fillWidth: true
                                                text: app.accounts ? (app.accounts.activeUserId || "") : ""
                                                color: AppTheme.stormTextMuted
                                                font.family: AppTheme.monoFont
                                                font.pixelSize: AppTheme.textMeta
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
                                                // Feedback in the tooltip: the icon font
                                                // subset has no suitable check glyph.
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
                                            font.pixelSize: AppTheme.textMeta
                                        }
                                    }
                                }
                                // Own display name, edited in place. Hidden on
                                // backends that cannot write a profile.
                                ColumnLayout {
                                    objectName: "ownDisplayNameSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: app.canEditOwnDisplayName
                                    Label {
                                        text: qsTr("Display name")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
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
                                            // "Not set" rather than the localpart, so
                                            // a cleared name does not look set.
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
                                            // No maximumLength: it counts UTF-16 units
                                            // and could split an emoji. The ceiling is
                                            // enforced by code point in AppController
                                            // and shown by the counter.
                                            onAccepted: accountIdentityCard.commitDisplayName()
                                            Keys.onEscapePressed: accountIdentityCard.cancelDisplayNameEdit()
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: AppTheme.textMeta
                                            readonly property int used:
                                                app.displayNameLength(displayNameField.text.trim())
                                            // Only near the ceiling.
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
                                            font.pixelSize: AppTheme.textMeta
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
                                                // AppController also refuses duplicates.
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
                                // Name colour, carried in the Matrix profile
                                // (org.lightning.name_color, MSC4133) so other
                                // Lightning clients see it. Absent, names use
                                // the reader's theme colour. The swatches are
                                // the theme's own ladder, which works in every
                                // theme.
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing16
                                    spacing: AppTheme.spacing8
                                    visible: app.nameColors.available
                                             && app.nameColors.supported
                                    Label {
                                        text: qsTr("Name colour")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                    // A Flow, not a RowLayout: a RowLayout's
                                    // minimum is the sum of its children, which
                                    // forced the column wider than the card and
                                    // pushed controls off-screen at the minimum
                                    // window width. A Flow's minimum is its
                                    // widest child. The reset button is the
                                    // group's last item.
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
                                        // A tenth swatch for any colour. It opens
                                        // the picker below; nothing is sent until
                                        // Apply, since each set is a server write.
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
                                            // stormInset: the card is stormPanel, so
                                            // an empty slot is a recessed well.
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
                                        // A Flow top-aligns a line; wrap the 32px
                                        // button at the discs' 30px height to
                                        // centre it.
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
                                    // Inline custom picker, hidden until the
                                    // tenth swatch opens it. `draft` is what it
                                    // shows; Apply is the one write.
                                    ColorPickerPanel {
                                        id: nameColorPicker
                                        objectName: "nameColorPicker"
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
                                            font.pixelSize: AppTheme.textMeta
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
                                        font.pixelSize: AppTheme.textMeta
                                        text: app.nameColors.lastError
                                              === "unsupported"
                                            ? qsTr("Your homeserver cannot store "
                                                   + "a name colour.")
                                            : qsTr("That colour could not be saved.")
                                    }
                                }

                                // Own profile picture, through the crop dialog
                                // like every display image.
                                ColumnLayout {
                                    objectName: "ownAvatarSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: app.canEditOwnAvatar()

                                    Label {
                                        text: qsTr("Profile picture")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing12
                                        Avatar {
                                            objectName: "ownAvatarPreview"
                                            size: AppTheme.scaled(64)
                                            // From the same account record as the
                                            // identity card; the write path re-fetches
                                            // the profile, so both update together.
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
                                                    // !! so the result is a bool; `a && b`
                                                    // yields null when a is null.
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
                                                font.pixelSize: AppTheme.textMeta
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
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                    FileDialog {
                                        id: ownAvatarFileDialog
                                        title: qsTr("Choose a profile picture")
                                        nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)")]
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

                                // Own bio (MSC4133 extended profile). Hidden
                                // when the backend cannot write it, disclosed
                                // but disabled when the homeserver cannot.
                                ColumnLayout {
                                    objectName: "ownBioSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: !!(app.bio && app.bio.available)

                                    Label {
                                        text: qsTr("About you")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        text: qsTr("A short note about yourself. Anyone who opens your profile can read it.")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: AppTheme.scaled(96)
                                        clip: true
                                        TextArea {
                                            id: ownBioField
                                            objectName: "ownBioField"
                                            // The stored value is the source of truth;
                                            // the field follows it unless being
                                            // edited. Assigning `text` imperatively
                                            // would destroy that binding.
                                            property bool dirty: false
                                            text: app.bio ? app.bio.ownBio : ""
                                            onTextChanged: if (activeFocus) dirty = true
                                            enabled: !!(app.bio && app.bio.supported
                                                     && !app.bio.busy)
                                            wrapMode: TextArea.Wrap
                                            placeholderText: qsTr("Say something about yourself")
                                            placeholderTextColor: AppTheme.stormTextMuted
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
                                            // Counted in characters, matching the Rust
                                            // bound.
                                            text: qsTr("%1 / %2")
                                                .arg(ownBioField.text.length)
                                                .arg(app.bio ? app.bio.maxLength : 0)
                                            color: (app.bio && ownBioField.text.length > app.bio.maxLength)
                                                ? AppTheme.stormDanger : AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.textMeta
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
                                        // Disclosed rather than hidden: a server
                                        // capability.
                                        visible: !!(app.bio && !app.bio.supported)
                                        text: qsTr("Your homeserver does not support profile bios yet.")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
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
                                        font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("To sign out, use the account menu at the "
                                               + "bottom of the sidebar.")
                                }
                            }
                        }
                        // Profile banner (MSC4427 over MSC4133 extended profile
                        // fields). Hidden when the backend cannot read them;
                        // disclosed when the homeserver lacks extended
                        // profiles. The account is asked on open so the answer
                        // is known before a file is picked.
                        SettingsCard {
                            id: profileBannerCard
                            visible: app.banners && app.banners.available
                            readonly property bool serverSupports:
                                app.banners && app.banners.supported
                            readonly property string ownUserId:
                                app.accounts ? app.accounts.activeUserId : ""
                            // On every open: the screen is built ahead of time
                            // and kept.
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
                                    font.pixelSize: AppTheme.textBody
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
                                        Connections {
                                            target: app.mediaBridge
                                            enabled: ownBannerImage.mxc.length > 0
                                            function onMediaCached(key) {
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
                                        font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Named causes get an actionable sentence;
                                    // a raw category read like a rejected
                                    // image.
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
                                // The homeserver's answer, stated where the
                                // control would be.
                                Label {
                                    objectName: "profileBannerUnsupported"
                                    Layout.fillWidth: true
                                    visible: !profileBannerCard.serverSupports
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
                                    // A banner is public profile data.
                                    text: qsTr("A wide image shown behind your profile card, "
                                               + "about 3:1. It is part of your public profile, "
                                               + "so anyone who can see your account can see it. "
                                               + "Saved under both the standard and the Commet "
                                               + "field names, so clients that already show "
                                               + "banners will show yours.")
                                }
                                FileDialog {
                                    id: bannerFileDialog
                                    title: qsTr("Choose a banner image")
                                    nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)")]
                                    // The crop dialog decides what is published
                                    // and refuses anything but the five raster
                                    // formats before rendering.
                                    onAccepted: ownBannerCrop.openFor(
                                        selectedFile)
                                }
                                ImageCropDialog {
                                    id: ownBannerCrop
                                    role: "banner"
                                    // Pass the URL as-is; stripping "file://"
                                    // breaks Windows paths.
                                    onCropped: function (file) {
                                        app.banners.setOwnBanner(
                                            file.toString())
                                    }
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Homeserver")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "startMinimizedCheck"
                                    text: qsTr("Start minimized")
                                    checked: app.settings.startMinimized
                                    onToggled: app.settings.startMinimized = checked
                                }
                            }
                        }
                    }

                    // ════════════ Privacy & security (encryption) ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        // Storage / crypto backend facts.
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
                                    text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: app.settings.secretsAreSecure
                                    color: AppTheme.stormSuccess
                                    text: qsTr("Access tokens are stored via the system Secret Service. Logout clears them.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: !app.settings.secretsAreSecure
                                    color: AppTheme.stormDanger
                                    text: qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
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

                        // Read-only E2EE health from the Rust SDK
                        // (app.cryptoHealth). Unsupported capabilities are
                        // informative state, not errors.
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
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "cryptoHealthRefresh"
                                        text: qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.textBody
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
                                // Live bootstrap status: the SDK's secret
                                // request / backup restore after this session
                                // is verified. Manual recovery-key entry below
                                // is the fallback.
                                RowLayout {
                                    objectName: "cryptoBootstrapStatus"
                                    visible: app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    StormSpinner {
                                        diameter: 14
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
                                        font.pixelSize: AppTheme.textBody
                                        text: app.cryptoBootstrap.statusMessage
                                        textFormat: Text.PlainText
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: text
                                    }
                                }
                                // Key re-request: a fresh m.secret.request
                                // round through the SDK's gossip machinery,
                                // retried on a bounded ladder. Shown only when
                                // useful (session verified, identity trusted,
                                // secrets missing).
                                AppButton {
                                    storm: true
                                    objectName: "requestKeysAgain"
                                    visible: app.cryptoBootstrap.canRequestKeys
                                    enabled: app.loggedIn
                                    text: qsTr("Request keys again")
                                    Accessible.name: text
                                    onClicked: app.requestEncryptionKeys()
                                }
                                // If this session does not trust the account
                                // identity, a gossiped answer cannot be
                                // accepted; only verification or the recovery
                                // key completes the chain. Uses the existing
                                // startOwnVerification path.
                                AppButton {
                                    storm: true
                                    objectName: "verifyAgainForKeys"
                                    visible: app.cryptoBootstrap.phase
                                                 === CryptoBootstrapModel.IdentityIncomplete
                                             || app.cryptoBootstrap.phase
                                                 === CryptoBootstrapModel.ManualRecoveryRequired
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
                                        // The flow card renders in Sessions; bring
                                        // it into view.
                                        root.section = "sessions"
                                        app.startOwnVerification()
                                    }
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
                                    // "Not yet known" is distinct from "none
                                    // found", which could make someone abandon
                                    // a real recovery key.
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
                                // Sanitized recovery diagnostics (fixed tokens
                                // and counts only), expandable. A real
                                // disclosure row with a rotating chevron;
                                // implicitHeight is constant.
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
                                            font.pixelSize: AppTheme.textBody
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
                                    // Focus ring overlay drawn outside the row
                                    // (anchors.fill with negative margins).
                                    // visualFocus: keyboard focus only.
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
                                        visible: app.cryptoBootstrap.requestAttempts > 0
                                        text: qsTr("Verified sessions available: %1")
                                            .arg(app.cryptoBootstrap.eligibleDevices)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textBody
                                        text: qsTr("Backup key usable: %1").arg(
                                            app.cryptoHealth.keyBackupUsable
                                                ? qsTr("yes") : qsTr("no"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textBody
                                        visible: app.cryptoBootstrap.keysReceived > 0
                                        text: qsTr("Room keys imported: %1")
                                            .arg(app.cryptoBootstrap.keysReceived)
                                    }
                                }
                            }
                        }

                    }

                    // ════════════ Sessions ════════════
                    ColumnLayout {
                        visible: root.section === "sessions"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        // The trust chain's devices step reads the session
                        // list; load it when the section opens.
                        onVisibleChanged: {
                            if (visible && app.backendName === "rust")
                                app.refreshSessionDevices()
                        }

                        Label {
                            text: qsTr("Sessions")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            objectName: "sessionsHeading"
                            Layout.fillWidth: true
                            text: qsTr("This account's Matrix sessions and device "
                                       + "verification.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        // MSC4108: sign another device in from this one. Under
                        // Sessions because it produces a new, already verified
                        // session.
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
                                    font.pixelSize: AppTheme.textMeta
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

                        // Own-account trust chain, from real crypto state only.
                        // Brand-fixed colours by design. Verify routes to the
                        // SAS flow; its visibility mirrors the start row below
                        // so both never show. Cross-signing detail: the three
                        // keys, whether this session is verified, and whether
                        // secrets are recoverable (booleans only, never key
                        // material).
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
                                            font.pixelSize: AppTheme.textBody
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

                        // Key-backup management. Health comes from the SDK's
                        // usable/state line, not from a version existing; every
                        // action is the SDK's own flow, with a warning where
                        // destructive.
                        Rectangle {
                            id: backupCard
                            objectName: "backupManagementCard"
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
                            // Re-asked on every open, since the screen is kept
                            // alive.
                            readonly property bool shown: root.visible
                            onShownChanged: if (shown && app.backup) app.backup.requestProgress()
                            Component.onCompleted: if (root.visible && app.backup) app.backup.requestProgress()
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
                                    font.pixelSize: AppTheme.textBody
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
                                                       .arg(h.keyBackupState)
                                        return qsTr("A backup exists but this session cannot use it yet (%1). Restore with your recovery key first.")
                                                   .arg(h.keyBackupState)
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: app.backup.uploadState.length > 0
                                             && app.backup.uploadState !== "unknown"
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                    text: app.backup.uploadState === "uploading"
                                          ? qsTr("Uploading room keys: %1 of %2")
                                                .arg(app.backup.backedUp).arg(app.backup.total)
                                          : qsTr("Upload: %1 · backup: %2")
                                                .arg(app.backup.uploadState)
                                                .arg(app.backup.backupState)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: app.backup.error.length > 0
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.textBody
                                    text: app.backup.error
                                }
                                // One-time recovery key display. Cleared on
                                // Done; never logged or persisted.
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
                                            font.pixelSize: AppTheme.textBody
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
                                            font.pixelSize: AppTheme.textBody
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
                                // Destructive actions arm on the first click
                                // and confirm on the second; the copy says what
                                // is lost.
                                Label {
                                    Layout.fillWidth: true
                                    visible: backupCard.pendingConfirm.length > 0
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.textBody
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
                                        // A definite No, never "not known": enable
                                        // reaches Recovery::enable(), which on a
                                        // store that already holds a backup key
                                        // mints a new 4S key and silently
                                        // invalidates the user's existing recovery
                                        // key.
                                        visible: app.cryptoHealth.keyBackupAvailable === CryptoHealthModel.No
                                                 && !app.cryptoHealth.secretStorageAvailable
                                        storm: true
                                        kind: "primary"
                                        size: "sm"
                                        enabled: !app.backup.busy
                                        text: app.backup.busy && app.backup.lastAction === "enable"
                                              ? qsTr("Setting up…") : qsTr("Set up recovery and backup")
                                        onClicked: app.backup.runAction("enable")
                                    }
                                    AppButton {
                                        objectName: "backupCreateButton"
                                        // Same rule as backupEnableButton.
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
                                        // Not secretStorageAvailable alone, which
                                        // is server truth and is true on a session
                                        // holding none of the secrets. Reset calls
                                        // create_secret_store(), filled from what
                                        // this session can export, so on an
                                        // unverified session it would repoint
                                        // m.secret_storage.default_key at an empty
                                        // store: the old recovery key opens
                                        // nothing and the identity is no longer
                                        // recoverable. Gated on this session
                                        // holding the secrets.
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
                            // Same invokable-staleness guard as the Account
                            // header.
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
                            // An empty list is not an answer: it is empty
                            // before the fetch returns and when it fails.
                            // Consult sessionDevicesFailed; trust labels come
                            // from SDK state, not from a missing list.
                            readonly property bool devicesVerified:
                                app.sessionDevicesFailed
                                || app.sessionDevicesLoading
                                ? false
                                : app.sessionDevices.length > 0
                                  // Not d.verified: is_verified() is always
                                  // true for our own device (the SDK marks it
                                  // locally trusted), so it would report an
                                  // unverified account as trusted.
                                  ? app.sessionDevices.every(
                                        d => d.crossSigned === true)
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
                            showVerify: !app.verificationActive
                                        && app.verificationState === ""
                            onVerifyRequested: app.startOwnVerification()
                        }

                        // The account's devices: server metadata merged with
                        // SDK trust. Other sessions can be signed out via UIA
                        // (password accounts) or the account console
                        // (OAuth/MAS). A tile disappears only when the refetch
                        // confirms the deletion.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                id: sessionsListCard
                                width: parent.width
                                spacing: AppTheme.spacing8

                                // Sign-out notice and OAuth console routing.
                                // The card is visibility-toggled, so a result
                                // arriving while another section is shown still
                                // lands here.
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
                                        // The account console owns OAuth session
                                        // management; Refresh picks up the result.
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
                                    // Mono-caption module header, as on the
                                    // trust card.
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
                                        font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("Press Refresh to load this account's sessions.")
                                }
                                // Filter over the same snapshot, no refetch.
                                // "Unverified" means a session with a crypto
                                // identity that is not cross-signed; one with
                                // no encryption is neither.
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
                                    font.pixelSize: AppTheme.textBody
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
                                            // Same fact as the chip, so the filter
                                            // matches the badge.
                                            else if (f === "verified" && d.crossSigned === true)
                                                out.push(d)
                                            else if (f === "unverified"
                                                     && d.hasCryptoIdentity === true
                                                     && d.crossSigned !== true)
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
                                                        font.pixelSize: AppTheme.textBody
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
                                                        // crossSigned, not verified: matrix-sdk
                                                        // marks our own device locally trusted at
                                                        // creation, so is_verified() is always
                                                        // true for "This session". Follow-up: for
                                                        // other rows is_verified() is better (it
                                                        // also covers SAS without cross-signing),
                                                        // i.e. `isCurrent ? crossSigned :
                                                        // verified`.
                                                        label: modelData.crossSigned === true
                                                              ? qsTr("Verified")
                                                              : modelData.hasCryptoIdentity === true
                                                                ? qsTr("Not verified")
                                                                : qsTr("No encryption")
                                                        iconName: modelData.crossSigned === true
                                                                  ? "verified_user" : ""
                                                        tone: modelData.crossSigned === true
                                                              ? "success" : "neutral"
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMeta
                                                    elide: Label.ElideRight
                                                    text: {
                                                        var parts = [ modelData.deviceId ]
                                                        if (modelData.lastSeen
                                                            && !isNaN(modelData.lastSeen.getTime()))
                                                            parts.push(qsTr("last seen %1").arg(
                                                                Qt.formatDateTime(modelData.lastSeen,
                                                                                  "d MMM yyyy hh:mm")))
                                                        if (modelData.lastSeenIp
                                                            && modelData.lastSeenIp.length > 0)
                                                            parts.push(modelData.lastSeenIp)
                                                        return parts.join(" · ")
                                                    }
                                                }
                                            }

                                            // Sign out another session; never offered
                                            // for the current one (that is the normal
                                            // Sign out with its store cleanup). Rename
                                            // uses an inline field and the standard
                                            // device endpoint; the list refetches on
                                            // success.
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
                                                Keys.onEscapePressed: visible = false
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
                                    font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("Signing out a session may require "
                                               + "your account password. "
                                               + "Verification below always "
                                               + "requires explicit confirmation "
                                               + "on both sessions.")
                                }
                            }
                        }

                        // Rust-only: session verification.
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
                                        label: app.sessionTrustState
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
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textMeta
                                    visible: app.sessionTrustState !== "Verified"
                                    text: qsTr(
                                        "Verify this session using another session already " +
                                        "signed in to this Matrix account. This does not import " +
                                        "room keys — key import is a separate action below.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    // Mirror of the active-flow card's
                                    // condition. Where the trust card renders,
                                    // its Verify button is the only start
                                    // affordance; this row covers backends
                                    // without it.
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

                                // The SAS/QR flow runs in the centred
                                // VerificationDialog (Main.qml), which follows
                                // AppController's verification state; this page
                                // only starts a flow and states this session's
                                // resting status.
                                Pane {
                                    objectName: "verificationStatusCard"
                                    Layout.fillWidth: true
                                    visible: app.cryptoHealth
                                             && app.cryptoHealth.cryptoSupported
                                             && !app.verificationActive
                                             && app.verificationState === ""
                                    // stormInset: nested inside a SettingsCard
                                    // (stormPanel).
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
                                        // No Verify button: the TrustCard owns the
                                        // single start affordance. Dismiss
                                        // silences the badges (rail cog, Sessions
                                        // dot), never this card, and is cleared
                                        // automatically once the session verifies.
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

                    // ════════════ Privacy & security (recovery) ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        // Rust-only: recovery key/passphrase and room-key
                        // import. Restoring recovery also restores
                        // cross-signing secrets stored in 4S. Setting up new
                        // cross-signing or a new backup needs a full 4S
                        // bootstrap, which is not implemented; secrets from
                        // other sessions arrive via SDK gossip after
                        // verification.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr(
                                        "Some old messages may show \"[unable to decrypt yet]\" until " +
                                        "you restore your recovery key here, or until another " +
                                        "verified device shares the room keys.")
                                }

                                Label {
                                    text: qsTr("Recovery key or passphrase")
                                    font.weight: AppTheme.weightStrong
                                    color: AppTheme.stormText
                                }
                                GridLayout {
                                    id: recoveryRow
                                    Layout.fillWidth: true
                                    columnSpacing: AppTheme.spacing8
                                    rowSpacing: AppTheme.spacing8
                                    columns: width < 360 ? 1 : 2
                                    AppTextField {
                                        storm: true
                                        id: recoveryField
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 160
                                        objectName: "recoveryInputField"
                                        echoMode: TextInput.Password
                                        // recover() accepts a recovery key or a
                                        // passphrase.
                                        placeholderText: qsTr("Recovery key or passphrase")
                                        enabled: !recoveryPanel.running
                                    }
                                    AppButton {
                                        storm: true
                                        text: recoveryPanel.running
                                            ? qsTr("Restoring…")
                                            : qsTr("Restore keys")
                                        enabled: !recoveryPanel.running
                                            && recoveryField.text.length > 0
                                        onClicked: {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.stormTextMuted
                                            app.requestRecoverFromBackup(recoveryField.text)
                                            // Wipe the field immediately; the key
                                            // never stays in a QML property.
                                            recoveryField.text = ""
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: recoveryPanel.statusText !== ""
                                    color: recoveryPanel.statusColor
                                    text: recoveryPanel.statusText
                                    textFormat: Text.PlainText
                                }
                                QtObject {
                                    id: recoveryPanel
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.stormTextMuted
                                }
                                Connections {
                                    target: app
                                    function onRecoveryStateChanged(state, message) {
                                        if (state === "attempted") {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.stormTextMuted
                                        } else if (state === "ok") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery complete. New messages should " +
                                                "decrypt as keys arrive. Some old messages may " +
                                                "still require another verified device to share " +
                                                "keys.")
                                            // Recovered secrets change trust/backup
                                            // state; re-read it.
                                            app.refreshCryptoHealth()
                                            app.refreshSessionTrustState()
                                            recoveryPanel.statusColor = AppTheme.stormSuccess
                                        } else if (state === "failed") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery failed: %1").arg(message)
                                            recoveryPanel.statusColor = AppTheme.stormDanger
                                        }
                                    }
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
                                    font.pixelSize: AppTheme.textMeta
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
                                            // Wipe the passphrase field immediately.
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
                                // Same treatment as the text-size slider: 4px,
                                // pill ends, bolt on stormInset.
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
                                FileDialog {
                                    id: importFileDialog
                                    title: qsTr("Select encrypted Matrix room-key export")
                                    fileMode: FileDialog.OpenFile
                                    // No nameFilters: Element writes .txt
                                    // exports and users may rename them.
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

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr(
                                        "Verification establishes trust in this session. " +
                                        "Secure Backup and room-key imports provide decryption keys " +
                                        "for message history. These are separate operations.")
                                }
                            }
                        }

                        // Danger Zone, collapsed by default.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            // Only the border changes; the fill stays the
                            // SettingsCard plane.
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
                                        font.pixelSize: AppTheme.textMeta
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
                                    // Wider than the default 340 for five lines
                                    // of consequence copy.
                                    width: 440
                                    Label {
                                        width: 380
                                        wrapMode: Text.WordWrap
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
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

                    // ════════════ Labs ════════════
                    ColumnLayout {
                        visible: root.section === "labs"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Labs")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("No experimental features are available in "
                                       + "this build. Diagnostics live here.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }
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
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Rebuilds the open room's timeline from the "
                                               + "SDK. Safe at any time.")
                                }
                            }
                        }
                    }

                    // ════════════ Updates ════════════ Visibility-toggled like
                    // the other panes, so its local state survives category
                    // switches.
                    UpdatesSettingsSection {
                        objectName: "updatesSection"
                        visible: root.section === "updates"
                        Layout.fillWidth: true
                    }

                    // ════════════ About ════════════
                    ColumnLayout {
                        visible: root.section === "about"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("About")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    // The application logo, or the custom icon
                                    // when set.
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
                                            font.pixelSize: AppTheme.textBody
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
                                    font.pixelSize: AppTheme.textMeta
                                    // Pinned in rust/Cargo.toml; update together.
                                    text: qsTr("Matrix engine: matrix-sdk 0.18.0 / matrix-sdk-ui 0.18.0 (Rust)")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("License: GPL-3.0-or-later")
                                }
                            }
                        }

                        // Reserve the Storm Band's height so content can scroll
                        // clear of its opaque lower part.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 190
                        }
                    }
                }
            }

            // Storm Band pinned to the bottom of the content pane, dissolving
            // upward. Input-transparent; About only. backdropColor must stay
            // stormDeep (ThemeTokensTest bans the raw background token in this
            // file).
            StormBand {
                objectName: "aboutStormBand"
                visible: root.section === "about"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 190
                backdropColor: AppTheme.stormDeep
            }
            }
        }
    }

    QrLoginDialog {
        id: qrLoginDialog
    }
}
