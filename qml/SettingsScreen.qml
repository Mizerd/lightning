import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 4): two-pane Settings. Left navigation lists only
// categories with real, implemented controls; the right pane shows grouped
// cards with independent scrolling. Every 0.5.6–0.5.8 security control
// (SAS verification, recovery key, encrypted room-key import, local reset)
// is preserved under Security & Recovery, with the destructive reset in a
// separated Danger Zone. Sign out lives ONLY in the account menu.
//
// Category panes are toggled by visibility (never Loader), so in-flight
// verification/import state survives switching categories.
Item {
    id: root

    // Reusable settings card (grouped-controls surface).
    component SettingsCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusMd
        }
    }

    // "general" | "appearance" | "notifications" | "account" | "security"
    // | "advanced" | "about"
    property string section: "general"

    Component.onCompleted: {
        var requested = app.takeRequestedSettingsSection()
        if (requested.length > 0)
            section = requested
    }

    function goBack() {
        app.loggedIn ? app.showMain() : app.showLogin()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: root.goBack()
    }

    // Confirmation before clearing local GIF collections (Favorites are the
    // destructive one; Recents too for parity).
    Dialog {
        id: gifClearConfirm
        property string kind: ""
        function open(k) { kind = k; title = k === "favorites"
            ? qsTr("Clear GIF favorites?") : qsTr("Clear recent GIFs?"); visible = true }
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: 280
            wrapMode: Text.WordWrap
            text: gifClearConfirm.kind === "favorites"
                ? qsTr("Remove all saved GIF favorites from this device? This "
                       + "cannot be undone.")
                : qsTr("Clear the list of recently used GIFs on this device?")
        }
        onAccepted: {
            if (kind === "favorites") app.gif.favorites.clearAll()
            else if (kind === "recent") app.gif.recent.clearAll()
        }
    }

    Rectangle { anchors.fill: parent; color: AppTheme.background }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + AppTheme.spacing12 * 2
            color: AppTheme.surface
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing16
                anchors.rightMargin: AppTheme.spacing16
                spacing: AppTheme.spacing8
                Label {
                    text: qsTr("Settings")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontPageTitle
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Back")
                    Accessible.name: qsTr("Leave settings")
                    onClicked: root.goBack()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Left navigation ──────────────────────────────────────────
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                color: AppTheme.sidebar

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing8
                    spacing: 2

                    Repeater {
                        model: [
                            { key: "general",       label: qsTr("General") },
                            { key: "appearance",    label: qsTr("Appearance") },
                            { key: "notifications", label: qsTr("Notifications") },
                            { key: "account",       label: qsTr("Account") },
                            { key: "security",      label: qsTr("Security & Recovery") },
                            { key: "advanced",      label: qsTr("Advanced") },
                            { key: "about",         label: qsTr("About") }
                        ]
                        ItemDelegate {
                            required property var modelData
                            Layout.fillWidth: true
                            highlighted: root.section === modelData.key
                            Accessible.name: modelData.label
                            onClicked: root.section = modelData.key
                            contentItem: Label {
                                text: parent.modelData.label
                                color: parent.highlighted ? AppTheme.selectedText
                                                          : AppTheme.textPrimary
                                font.pixelSize: AppTheme.fontBody
                                elide: Label.ElideRight
                            }
                            background: Rectangle {
                                radius: AppTheme.radiusSm
                                color: parent.highlighted ? AppTheme.selected
                                     : parent.hovered ? AppTheme.hover
                                     : "transparent"
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: AppTheme.border }

            // ── Right content pane ───────────────────────────────────────
            Flickable {
                id: contentFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentHeight: contentColumn.implicitHeight + AppTheme.spacing24 * 2
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // Jump to the top when switching categories.
                Connections {
                    target: root
                    function onSectionChanged() { contentFlick.contentY = 0 }
                }

                ColumnLayout {
                    id: contentColumn
                    x: AppTheme.spacing24
                    y: AppTheme.spacing24
                    width: Math.min(640, contentFlick.width - AppTheme.spacing24 * 2)
                    spacing: AppTheme.spacing16

                    // ════════════ General ════════════
                    ColumnLayout {
                        visible: root.section === "general"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("General")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Startup and application behaviour.")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Startup")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                CheckBox {
                                    text: qsTr("Start minimized")
                                    checked: app.settings.startMinimized
                                    onToggled: app.settings.startMinimized = checked
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Timeline")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                CheckBox {
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
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Hide routine joins, leaves, profile changes, "
                                               + "and room setting updates. Messages and "
                                               + "decryption warnings remain visible.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Mouse-wheel speed")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                }
                                ComboBox {
                                    id: wheelSpeedCombo
                                    objectName: "timelineWheelSpeedCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    // Values map to TimelineScrollController::WheelSpeed.
                                    model: [
                                        { label: qsTr("Standard"),  value: 0 },
                                        { label: qsTr("Fast"),      value: 1 },
                                        { label: qsTr("Very fast"), value: 2 }
                                    ]
                                    // indexOfValue() only resolves once the
                                    // model is ready, so set it on completion
                                    // and whenever the persisted value changes
                                    // rather than in a one-shot binding.
                                    function syncFromSetting() {
                                        currentIndex = Math.max(0, indexOfValue(
                                            app.settings.timelineWheelSpeed))
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
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("How far one physical mouse-wheel notch moves "
                                               + "the timeline. Touchpad and precision scrolling "
                                               + "stay fine-grained regardless of this setting.")
                                }
                            }
                        }
                    }

                    // ════════════ Appearance ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Appearance")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Theme and language. Values persist across restarts.")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Theme")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                }
                                ComboBox {
                                    id: themeCombo
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    // Preset id values map to SettingsManager::Theme.
                                    model: [
                                        { label: qsTr("System"),        value: 0 },
                                        { label: qsTr("Light"),         value: 1 },
                                        { label: qsTr("Graphite"),      value: 3 },
                                        { label: qsTr("Midnight Blue"), value: 4 },
                                        { label: qsTr("Nord"),          value: 5 },
                                        { label: qsTr("Purple Dusk"),   value: 6 }
                                    ]
                                    // A stored legacy Dark (2) renders as the
                                    // Midnight Blue preset and is migrated to
                                    // its current id on the next selection.
                                    currentIndex: Math.max(0, indexOfValue(
                                        app.settings.theme === 2
                                        ? 4 : app.settings.theme))
                                    onActivated: app.settings.theme = currentValue
                                }
                                // Small live preview of the core palette.
                                RowLayout {
                                    spacing: AppTheme.spacing8
                                    Repeater {
                                        model: [
                                            { c: AppTheme.background,   n: qsTr("Background") },
                                            { c: AppTheme.surface,      n: qsTr("Surface") },
                                            { c: AppTheme.accent,       n: qsTr("Accent") },
                                            { c: AppTheme.selected,     n: qsTr("Selected") },
                                            { c: AppTheme.danger,       n: qsTr("Danger") }
                                        ]
                                        ColumnLayout {
                                            required property var modelData
                                            spacing: 2
                                            Rectangle {
                                                width: 40; height: 24
                                                radius: AppTheme.radiusSm
                                                color: parent.modelData.c
                                                border.color: AppTheme.border
                                            }
                                            Label {
                                                text: parent.modelData.n
                                                color: AppTheme.textMuted
                                                font.pixelSize: AppTheme.fontCaption
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
                                    text: qsTr("Language")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["en", "lt"]
                                    currentIndex: Math.max(0, model.indexOf(app.settings.language))
                                    onActivated: app.settings.language = model[currentIndex]
                                }
                                Label {
                                    text: qsTr("Language switching requires an app restart.")
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                }
                            }
                        }

                        // v0.5.11: link-preview and GIF policy.
                        Label {
                            text: qsTr("Link previews & media")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    id: autoPreviewCheck
                                    text: qsTr("Automatically load previews in unencrypted rooms")
                                    checked: app.settings.autoLoadLinkPreviews
                                    onToggled: app.settings.autoLoadLinkPreviews = checked
                                }
                                CheckBox {
                                    text: qsTr("Load previews in encrypted rooms")
                                    checked: app.settings.loadPreviewsInEncryptedRooms
                                    onToggled: app.settings.loadPreviewsInEncryptedRooms = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.warning
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Loading a preview contacts the linked website "
                                               + "directly and may reveal your IP address and "
                                               + "request timing. No JavaScript is executed. "
                                               + "Encrypted-room previews are off by "
                                               + "default; otherwise use each message's "
                                               + "“Load link preview” action.")
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.border
                                }

                                // ─────────── GIFs ───────────
                                Label {
                                    text: qsTr("GIFs")
                                    color: AppTheme.textPrimary
                                    font.pixelSize: AppTheme.fontBody
                                    font.weight: Font.DemiBold
                                    Layout.topMargin: AppTheme.spacing4
                                }

                                Label { text: qsTr("Autoplay GIFs"); color: AppTheme.textSecondary }
                                ComboBox {
                                    id: gifAutoplayCombo
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Autoplay GIFs")
                                    model: [
                                        { label: qsTr("Always"),   value: 0 },
                                        { label: qsTr("On hover"), value: 1 },
                                        { label: qsTr("Never"),    value: 2 },
                                    ]
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifAutoplay))
                                    onActivated: app.settings.gifAutoplay = currentValue
                                }

                                Label { text: qsTr("GIF safe search"); color: AppTheme.textSecondary }
                                ComboBox {
                                    id: gifRatingCombo
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
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifSafeSearch))
                                    onActivated: app.settings.gifSafeSearch = currentValue
                                }

                                Label { text: qsTr("Preferred GIF provider"); color: AppTheme.textSecondary }
                                ComboBox {
                                    id: gifProviderCombo
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Preferred GIF provider")
                                    model: [
                                        { label: "GIPHY", value: "giphy" },
                                        { label: "KLIPY", value: "klipy" },
                                    ]
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifPreferredProvider))
                                    onActivated: app.settings.gifPreferredProvider = currentValue
                                }
                                // Honest per-provider availability.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("GIPHY: %1 · KLIPY: %2")
                                        .arg(app.gif.providerConfigured("giphy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                        .arg(app.gif.providerConfigured("klipy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                }

                                CheckBox {
                                    text: qsTr("Store recently used GIFs")
                                    checked: app.settings.storeRecentGifs
                                    onToggled: app.settings.storeRecentGifs = checked
                                    Accessible.name: qsTr("Store recently used GIFs")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("GIF searches are sent directly to the "
                                               + "selected provider. Favorites and recent "
                                               + "GIFs are stored locally on this device "
                                               + "and are not synchronized; search terms "
                                               + "are not saved.")
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Button {
                                        text: qsTr("Clear recent GIFs")
                                        enabled: app.gif.recent.count > 0
                                        onClicked: gifClearConfirm.open("recent")
                                    }
                                    Button {
                                        text: qsTr("Clear GIF favorites")
                                        enabled: app.gif.favorites.count > 0
                                        onClicked: gifClearConfirm.open("favorites")
                                    }
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
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    text: qsTr("Desktop notifications")
                                    checked: app.settings.notificationsEnabled
                                    onToggled: app.settings.notificationsEnabled = checked
                                }
                                // v0.6.0 checkpoint 11: notification privacy.
                                Label {
                                    text: qsTr("Notification preview")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                ComboBox {
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
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Sender only (the default) never shows "
                                               + "message text in notifications. "
                                               + "Encrypted messages that cannot be "
                                               + "decrypted always show a generic "
                                               + "notification. Notifications are "
                                               + "suppressed while the room is open, "
                                               + "focused, and at the latest message.")
                                }
                                // v0.6.1: notification sound.
                                Label {
                                    text: qsTr("Notification sound")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                ComboBox {
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
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("The sound plays only when a "
                                               + "notification is shown, so muted and "
                                               + "active rooms stay silent. Bursts are "
                                               + "coalesced into a single alert.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Per-room notification modes (set from "
                                               + "Room information) apply to this "
                                               + "device only — they are not server "
                                               + "push rules. Push registration for "
                                               + "mobile-style notifications is not "
                                               + "implemented.")
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
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Rectangle {
                                        width: 48; height: 48
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.accent
                                        Label {
                                            anchors.centerIn: parent
                                            text: {
                                                var uid = app.accounts ? (app.accounts.activeUserId || "") : ""
                                                if (uid.startsWith("@")) uid = uid.slice(1)
                                                return uid.length > 0 ? uid[0].toUpperCase() : "?"
                                            }
                                            color: AppTheme.accentText
                                            font.pixelSize: AppTheme.fontRoomTitle
                                            font.weight: Font.DemiBold
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            Layout.fillWidth: true
                                            text: app.accounts ? (app.accounts.activeUserId || qsTr("(signed out)")) : ""
                                            color: AppTheme.textPrimary
                                            font.pixelSize: AppTheme.fontBody
                                            elide: Label.ElideMiddle
                                        }
                                        Label {
                                            visible: app.backendName === "rust" && app.sessionDeviceId !== ""
                                            text: qsTr("Device: %1").arg(app.sessionDeviceId)
                                            color: AppTheme.textMuted
                                            font.pixelSize: AppTheme.fontCaption
                                        }
                                        Label {
                                            visible: app.backendName === "rust"
                                            text: qsTr("Session: %1").arg(app.sessionTrustState)
                                            color: app.sessionTrustState === "Verified"
                                                   ? AppTheme.success : AppTheme.textMuted
                                            font.pixelSize: AppTheme.fontCaption
                                        }
                                    }
                                }
                                Button {
                                    text: qsTr("Open Security & Recovery")
                                    onClicked: root.section = "security"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("To sign out, use the account menu at the "
                                               + "bottom of the sidebar.")
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Homeserver")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    text: app.settings.homeserverUrl
                                    placeholderText: "https://matrix.org"
                                    onEditingFinished: app.settings.homeserverUrl = text
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Changing the homeserver takes effect at the next sign-in.")
                                }
                            }
                        }
                    }

                    // ════════════ Security & Recovery ════════════
                    ColumnLayout {
                        visible: root.section === "security"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Security & Recovery")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }

                        // Storage / crypto backend facts (all backends).
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: app.settings.secretsAreSecure
                                    color: AppTheme.success
                                    text: qsTr("Access tokens are stored via the system Secret Service. Logout clears them.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: !app.settings.secretsAreSecure
                                    color: AppTheme.danger
                                    text: qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: qsTr("Crypto backend: %1").arg(app.crypto.backendDescription)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: app.crypto.supportsE2ee ? AppTheme.success : AppTheme.textMuted
                                    text: qsTr("E2EE status: %1").arg(app.crypto.statusString)
                                }
                            }
                        }

                        // v0.6.0 checkpoint 7: read-only E2EE health from
                        // the Rust SDK (app.cryptoHealth). Unsupported
                        // capabilities show as informative state, never as
                        // errors.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Security status")
                                        color: AppTheme.textSecondary
                                        font.pixelSize: AppTheme.fontSecondary
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "cryptoHealthRefresh"
                                        text: qsTr("Refresh")
                                        color: AppTheme.accent
                                        font.pixelSize: AppTheme.fontSecondary
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
                                    color: AppTheme.text
                                    text: app.cryptoHealth.statusSummary
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
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
                                    color: AppTheme.textMuted
                                    text: app.cryptoHealth.crossSigningReady
                                          ? qsTr("Cross-signing: ready")
                                          : app.cryptoHealth.crossSigningAvailable
                                            ? qsTr("Cross-signing: not complete on this session")
                                            : qsTr("Cross-signing: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: app.cryptoHealth.keyBackupUsable
                                          ? qsTr("Key backup: active on this session")
                                          : app.cryptoHealth.keyBackupAvailable
                                            ? qsTr("Key backup: exists, but this session cannot use it yet")
                                            : qsTr("Key backup: none found")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: app.cryptoHealth.recoveryAvailable
                                          ? qsTr("Recovery: set up")
                                          : app.cryptoHealth.recoveryRequired
                                            ? qsTr("Recovery: set up, but secrets are missing here")
                                            : qsTr("Recovery: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: app.cryptoHealth.cryptoSyncing
                                          ? qsTr("Encryption sync: active")
                                          : app.cryptoHealth.cryptoReady
                                            ? qsTr("Encryption sync: ready")
                                            : qsTr("Encryption sync: waiting")
                                }
                            }
                        }

                        // v0.6.0 checkpoint 9: the account's Matrix
                        // devices/sessions — server metadata merged with SDK
                        // crypto trust. Read-only: removing other sessions
                        // requires interactive re-authentication, which
                        // Lightning does not implement yet (limitation shown
                        // honestly below).
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Sessions")
                                        color: AppTheme.textSecondary
                                        font.pixelSize: AppTheme.fontSecondary
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "sessionDevicesRefresh"
                                        text: app.sessionDevicesLoading
                                              ? qsTr("Loading…") : qsTr("Refresh")
                                        color: AppTheme.accent
                                        font.pixelSize: AppTheme.fontSecondary
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
                                    color: AppTheme.danger
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("The session list could not be loaded.")
                                }
                                Label {
                                    visible: !app.sessionDevicesFailed
                                             && !app.sessionDevicesLoading
                                             && app.sessionDevices.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("Press Refresh to load this account's sessions.")
                                }
                                Repeater {
                                    model: app.sessionDevices
                                    delegate: ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            Label {
                                                text: (modelData.displayName
                                                       && modelData.displayName.length > 0)
                                                      ? modelData.displayName
                                                      : modelData.deviceId
                                                color: AppTheme.text
                                                font.pixelSize: AppTheme.fontSecondary
                                                font.weight: Font.DemiBold
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Label {
                                                visible: modelData.isCurrent === true
                                                text: qsTr("This session")
                                                color: AppTheme.accent
                                                font.pixelSize: AppTheme.fontSecondary
                                            }
                                            Label {
                                                text: modelData.crossSigned === true
                                                      ? qsTr("Verified")
                                                      : modelData.hasCryptoIdentity === true
                                                        ? qsTr("Not verified")
                                                        : qsTr("No encryption")
                                                color: modelData.crossSigned === true
                                                       ? AppTheme.success
                                                       : AppTheme.textMuted
                                                font.pixelSize: AppTheme.fontSecondary
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            color: AppTheme.textMuted
                                            font.pixelSize: AppTheme.fontSecondary
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
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("Signing out other sessions from Lightning "
                                               + "is not supported yet — use another "
                                               + "client for that. Verification below "
                                               + "always requires explicit confirmation "
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

                                Label {
                                    text: qsTr("Current session")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
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
                                    color: app.sessionTrustState === "Verified"
                                        ? AppTheme.success
                                        : (app.sessionTrustState === "Not verified"
                                            ? AppTheme.warning
                                            : AppTheme.textMuted)
                                    text: qsTr("Status: %1").arg(app.sessionTrustState)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    visible: app.sessionTrustState !== "Verified"
                                    text: qsTr(
                                        "Verify this session using another session already " +
                                        "signed in to this Matrix account. This does not import " +
                                        "room keys — key import is a separate action below.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: !app.verificationActive
                                    Button {
                                        text: app.sessionTrustState === "Verified"
                                            ? qsTr("Verify again")
                                            : qsTr("Verify this session")
                                        enabled: app.loggedIn
                                        onClicked: app.startOwnVerification()
                                    }
                                    Label {
                                        visible: app.sessionTrustState === "Verified"
                                        Layout.fillWidth: true
                                        color: AppTheme.success
                                        text: qsTr("This Lightning session is verified through Matrix cross-signing.")
                                        wrapMode: Text.WordWrap
                                    }
                                    Item {
                                        visible: app.sessionTrustState !== "Verified"
                                        Layout.fillWidth: true
                                    }
                                }

                                // Active SAS flow card.
                                Pane {
                                    Layout.fillWidth: true
                                    visible: app.verificationActive
                                    background: Rectangle {
                                        color: AppTheme.surfaceAlt
                                        border.color: AppTheme.accent
                                        radius: AppTheme.radiusSm
                                    }
                                    ColumnLayout {
                                        width: parent.width
                                        spacing: AppTheme.spacing8

                                        Label {
                                            text: qsTr("Session verification")
                                            color: AppTheme.text
                                            font.weight: Font.DemiBold
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            color: AppTheme.textMuted
                                            text: {
                                                if (app.verificationState === "starting")
                                                    return qsTr("Sending verification request…")
                                                if (app.verificationState === "waiting_for_other_session")
                                                    return qsTr(
                                                        "Verification request sent. Accept it in " +
                                                        "another session, such as Element.")
                                                if (app.verificationState === "requested")
                                                    return qsTr("Incoming verification request from %1")
                                                        .arg(app.verificationOtherUser)
                                                if (app.verificationState === "sas_ready")
                                                    return qsTr(
                                                        "Compare all seven emojis with the other " +
                                                        "session. Confirm only if every emoji matches " +
                                                        "in the same order.")
                                                if (app.verificationState === "done")
                                                    return qsTr(
                                                        "Verification flow complete. Lightning is " +
                                                        "querying the SDK for updated trust state.")
                                                if (app.verificationState === "cancelled")
                                                    return qsTr("Verification cancelled.")
                                                if (app.verificationState.indexOf("failed") === 0)
                                                    return qsTr("Verification failed.")
                                                return qsTr("Waiting…")
                                            }
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            visible: app.verificationState === "sas_ready"
                                            Repeater {
                                                model: app.verificationEmojis
                                                delegate: Rectangle {
                                                    color: AppTheme.surface
                                                    border.color: AppTheme.border
                                                    radius: AppTheme.radiusSm
                                                    implicitWidth: 84
                                                    implicitHeight: 78
                                                    ColumnLayout {
                                                        anchors.centerIn: parent
                                                        spacing: 2
                                                        Label {
                                                            text: modelData.symbol || ""
                                                            font.pixelSize: 28
                                                            horizontalAlignment: Text.AlignHCenter
                                                            Layout.alignment: Qt.AlignHCenter
                                                        }
                                                        Label {
                                                            text: modelData.description || ""
                                                            font.pixelSize: AppTheme.fontCaption
                                                            color: AppTheme.textMuted
                                                            horizontalAlignment: Text.AlignHCenter
                                                            Layout.alignment: Qt.AlignHCenter
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            Button {
                                                text: qsTr("Accept")
                                                visible: app.verificationState === "requested"
                                                highlighted: true
                                                onClicked: app.acceptVerification()
                                            }
                                            Button {
                                                text: qsTr("They match")
                                                visible: app.verificationState === "sas_ready"
                                                highlighted: true
                                                onClicked: app.confirmVerification()
                                            }
                                            Button {
                                                text: qsTr("They do not match")
                                                visible: app.verificationState === "sas_ready"
                                                onClicked: app.mismatchVerification()
                                            }
                                            Button {
                                                text: qsTr("Cancel verification")
                                                visible: app.verificationState === "requested"
                                                        || app.verificationState === "sas_ready"
                                                        || app.verificationState === "waiting_for_other_session"
                                                        || app.verificationState === "starting"
                                                onClicked: app.cancelVerification()
                                            }
                                            Button {
                                                text: qsTr("Dismiss")
                                                visible: app.verificationState === "done"
                                                        || app.verificationState === "cancelled"
                                                        || app.verificationState.indexOf("failed") === 0
                                                onClicked: app.cancelVerification()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Rust-only: recovery key/passphrase + room-key
                        // import. Restoring recovery also restores
                        // cross-signing secrets where they are stored in 4S
                        // (the SDK imports them and can sign this session).
                        // Honest limitations: SETTING UP new cross-signing or
                        // a new key backup requires interactive
                        // re-authentication / full 4S bootstrap, which
                        // Lightning does not implement in 0.6.0; secrets from
                        // other sessions arrive via the SDK's automatic
                        // secret gossip after verification (no manual
                        // request button is faked).
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Some old messages may show \"[unable to decrypt yet]\" until " +
                                        "you restore your recovery key here, or until another " +
                                        "verified device shares the room keys.")
                                }

                                Label {
                                    text: qsTr("Recovery key or passphrase")
                                    font.weight: Font.DemiBold
                                    color: AppTheme.text
                                }
                                GridLayout {
                                    id: recoveryRow
                                    Layout.fillWidth: true
                                    columnSpacing: AppTheme.spacing8
                                    rowSpacing: AppTheme.spacing8
                                    columns: width < 360 ? 1 : 2
                                    TextField {
                                        id: recoveryField
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 160
                                        objectName: "recoveryInputField"
                                        echoMode: TextInput.Password
                                        // The SDK's recover() accepts both a
                                        // recovery key and a passphrase.
                                        placeholderText: qsTr("Recovery key or passphrase")
                                        enabled: !recoveryPanel.running
                                    }
                                    Button {
                                        text: recoveryPanel.running
                                            ? qsTr("Restoring…")
                                            : qsTr("Restore keys")
                                        enabled: !recoveryPanel.running
                                            && recoveryField.text.length > 0
                                        onClicked: {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.textMuted
                                            app.requestRecoverFromBackup(recoveryField.text)
                                            // Wipe local copy immediately — the recovery
                                            // key never sits in a QML property beyond
                                            // this call.
                                            recoveryField.text = ""
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: recoveryPanel.statusText !== ""
                                    color: recoveryPanel.statusColor
                                    text: recoveryPanel.statusText
                                }
                                QtObject {
                                    id: recoveryPanel
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.textMuted
                                }
                                Connections {
                                    target: app
                                    function onRecoveryStateChanged(state, message) {
                                        if (state === "attempted") {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.textMuted
                                        } else if (state === "ok") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery complete. New messages should " +
                                                "decrypt as keys arrive. Some old messages may " +
                                                "still require another verified device to share " +
                                                "keys.")
                                            // v0.6.0 checkpoint 10: recovered
                                            // secrets change trust/backup
                                            // state — re-read it from the SDK.
                                            app.refreshCryptoHealth()
                                            app.refreshSessionTrustState()
                                            recoveryPanel.statusColor = AppTheme.success
                                        } else if (state === "failed") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery failed: %1").arg(message)
                                            recoveryPanel.statusColor = AppTheme.danger
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Import room keys")
                                    font.weight: Font.DemiBold
                                    color: AppTheme.text
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Import an encrypted Matrix room-key export from another " +
                                        "session. Imported keys may unlock older encrypted messages, " +
                                        "but they do not verify this session.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Button {
                                        text: qsTr("Choose key export")
                                        enabled: app.loggedIn && !importPanel.running
                                        onClicked: importFileDialog.open()
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        elide: Text.ElideMiddle
                                        color: AppTheme.textMuted
                                        text: importPanel.selectedFileName === ""
                                            ? qsTr("(no file selected)")
                                            : importPanel.selectedFileName
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: importPanel.selectedFileUrl.toString() !== ""
                                    TextField {
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
                                    Button {
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
                                            // Wipe the passphrase from the QML field
                                            // immediately — never keep it beyond the
                                            // dispatch.
                                            importPassphraseField.text = ""
                                        }
                                    }
                                    Button {
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
                                    Layout.fillWidth: true
                                    visible: importPanel.running
                                    indeterminate: app.roomKeyImportTotalCount === 0
                                    from: 0
                                    to: Math.max(1, app.roomKeyImportTotalCount)
                                    value: app.roomKeyImportImportedCount
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: importPanel.statusText !== ""
                                    color: importPanel.statusColor
                                    text: importPanel.statusText
                                }
                                QtObject {
                                    id: importPanel
                                    property url selectedFileUrl: ""
                                    property string selectedFileName: ""
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.textMuted
                                }
                                FileDialog {
                                    id: importFileDialog
                                    title: qsTr("Select encrypted Matrix room-key export")
                                    fileMode: FileDialog.OpenFile
                                    // Deliberately no nameFilters — Element writes
                                    // .txt exports; users may rename.
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
                                            importPanel.statusColor = AppTheme.textMuted
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
                                            importPanel.statusColor = AppTheme.success
                                            importPanel.selectedFileUrl = ""
                                            importPanel.selectedFileName = ""
                                            importPassphraseField.text = ""
                                        } else if (state === "failed") {
                                            importPanel.running = false
                                            importPanel.statusText =
                                                app.roomKeyImportLastMessage
                                            importPanel.statusColor = AppTheme.danger
                                            importPassphraseField.text = ""
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Verification establishes trust in this session. " +
                                        "Secure Backup and room-key imports provide decryption keys " +
                                        "for message history. These are separate operations.")
                                }
                            }
                        }

                        // Danger Zone — collapsed by default, clearly apart.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            background: Rectangle {
                                color: AppTheme.surface
                                border.color: dangerZone.expanded ? AppTheme.danger
                                                                  : AppTheme.border
                                radius: AppTheme.radiusMd
                            }
                            ColumnLayout {
                                id: dangerZone
                                property bool expanded: false
                                width: parent.width
                                spacing: AppTheme.spacing8

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Danger Zone")
                                        color: AppTheme.danger
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    Button {
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
                                        color: AppTheme.textMuted
                                        font.pixelSize: AppTheme.fontCaption
                                        text: qsTr(
                                            "Reset deletes only Lightning's local Rust SDK store " +
                                            "for this account (also available from a terminal: " +
                                            "matrix-client --reset-crypto-store). It does not touch " +
                                            "server messages or Element data. You will need to sign " +
                                            "in again afterwards.")
                                    }
                                    Button {
                                        id: resetDangerButton
                                        text: qsTr("Reset local Lightning session")
                                        onClicked: resetConfirmDialog.open()
                                        contentItem: Label {
                                            text: resetDangerButton.text
                                            color: AppTheme.dangerText
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            font.weight: Font.DemiBold
                                            font.pixelSize: AppTheme.fontSecondary
                                        }
                                        background: Rectangle {
                                            radius: AppTheme.radiusSm
                                            color: resetDangerButton.pressed
                                                    ? Qt.darker(AppTheme.danger, 1.2)
                                                    : (resetDangerButton.hovered
                                                        ? Qt.darker(AppTheme.danger, 1.1)
                                                        : AppTheme.danger)
                                            border.color: Qt.darker(AppTheme.danger, 1.3)
                                            border.width: 1
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: resetStatus.text !== ""
                                        color: resetStatus.ok ? AppTheme.success : AppTheme.danger
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
                                Dialog {
                                    id: resetConfirmDialog
                                    title: qsTr("Reset local Lightning session?")
                                    standardButtons: Dialog.Ok | Dialog.Cancel
                                    modal: true
                                    Label {
                                        width: 380
                                        wrapMode: Text.WordWrap
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

                    // ════════════ Advanced ════════════
                    ColumnLayout {
                        visible: root.section === "advanced"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Advanced")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: qsTr("Backend: %1").arg(app.backendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: app.syncModeLabel !== ""
                                    color: AppTheme.textMuted
                                    text: qsTr("Sync mode: %1").arg(app.syncModeLabel)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: qsTr("Connection: %1").arg(app.connectionStatus)
                                }
                                Button {
                                    text: qsTr("Refresh current room")
                                    enabled: app.currentRoomId !== ""
                                    onClicked: app.reloadCurrentRoomTimeline(50)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Rebuilds the open room's timeline from the "
                                               + "SDK. Safe at any time.")
                                }
                            }
                        }
                    }

                    // ════════════ About ════════════
                    ColumnLayout {
                        visible: root.section === "about"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("About")
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Lightning %1").arg(app.appVersion)
                                    color: AppTheme.textPrimary
                                    font.pixelSize: AppTheme.fontBody
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    text: qsTr("A native C++/Qt Matrix desktop client. "
                                               + "No Electron, no web view.")
                                }
                                Label {
                                    visible: app.backendName === "rust"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    // Pinned in rust/Cargo.toml; update together.
                                    text: qsTr("Matrix engine: matrix-sdk 0.18.0 / matrix-sdk-ui 0.18.0 (Rust)")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("License: MIT OR Apache-2.0")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
