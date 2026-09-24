import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Home / no-room surface, shown in the timeline region when no room is
// selected; TimelinePane hides the composer here. A welcome, the primary
// create/find actions and a short list of recent conversations. The actions
// route to the room list's shared new-conversation dialog.
Item {
    id: root
    objectName: "homePane"

    // Routed to the room list's new-conversation dialog by MainScreen.
    signal newMessageRequested()
    signal createRoomRequested()
    signal createSpaceRequested()

    // `account()` is a Q_INVOKABLE, so a binding on it would miss renames and
    // avatar changes. Both fields are refreshed by one function driven by the
    // manager's signals (as the rail's account tile and Settings identity card
    // do). A local binding with a change handler would run that handler inside
    // `displayName`'s first evaluation and cause a binding loop.
    property string activeUserId: ""
    property var activeAccount: null
    function refreshActiveAccount() {
        var uid = app.accounts ? app.accounts.activeUserId : ""
        // The account first, so the one intermediate state between these writes
        // already greets correctly.
        root.activeAccount = (app.accounts && uid.length > 0)
            ? app.accounts.account(uid) : null
        root.activeUserId = uid
    }
    Connections {
        target: app.accounts
        // Both the selection moving and the active record changing.
        function onAccountsChanged() { root.refreshActiveAccount() }
        function onActiveUserIdChanged() { root.refreshActiveAccount() }
    }
    readonly property string displayName: {
        var n = activeAccount && activeAccount.displayName
                ? activeAccount.displayName : ""
        if (n.length > 0)
            return n
        // Localpart fallback (@user:hs -> user), never a bare MXID.
        var id = activeUserId
        if (id.length === 0)
            return qsTr("there")
        if (id.charAt(0) === "@")
            id = id.substring(1)
        var colon = id.indexOf(":")
        return colon > 0 ? id.substring(0, colon) : id
    }

    // Recomputed from the model rather than a Repeater over the whole room
    // list, so hidden rows never fetch avatars.
    property var recentModel: []
    property var spacesModel: []
    function refreshRecent() {
        recentModel = app.roomList ? app.roomList.recentRooms(6) : []
        spacesModel = app.roomList ? app.roomList.spacesSummary(8) : []
    }
    // Only one Component.onCompleted per object; a second is a load error.
    Component.onCompleted: {
        root.refreshActiveAccount()
        refreshRecent()
    }
    onVisibleChanged: if (visible) refreshRecent()
    // Coalesce bursty room-list updates so each sync tick doesn't rebuild the
    // sections and their avatars.
    Timer {
        id: refreshCoalesce
        interval: 250
        repeat: false
        onTriggered: root.refreshRecent()
    }
    Connections {
        target: app.roomList
        enabled: root.visible
        function onModelReset() { refreshCoalesce.restart() }
        function onRowsInserted() { refreshCoalesce.restart() }
        function onRowsRemoved() { refreshCoalesce.restart() }
        function onDataChanged() { refreshCoalesce.restart() }
    }

    // Compact "3:24 PM / Yesterday / Mon / 12 Jun" recency label.
    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            // The app-wide clock setting, as a Qt format string from C++. Read
            // as a property so bindings depend on it; this label re-renders
            // when its caller's binding next does.
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.formatDate(when, "ddd")
        return Qt.formatDate(when, "d MMM")
    }

    readonly property bool offline:
        app.connectionStatus === qsTr("Error")
        || app.connectionStatus === qsTr("Offline — retrying")
        || app.connectionStatus === qsTr("Not connected")

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: card.implicitHeight + AppTheme.spacing24 * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        // Same wheel/touchpad feel as the room timeline; see
        // qml/SmoothWheelArea.qml.
        SmoothWheelArea {}

        ColumnLayout {
            id: card
            width: Math.min(560, parent.width - AppTheme.spacing24 * 2)
            x: (parent.width - width) / 2
            y: Math.max(AppTheme.spacing24, (root.height - implicitHeight) / 2)
            spacing: AppTheme.spacing20

            // Brand + welcome
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: AppTheme.spacing12
                Avatar {
                    size: 56
                    circle: true
                    name: root.displayName
                    mxc: root.activeAccount ? (root.activeAccount.avatarUrl || "") : ""
                    colorKey: root.activeUserId
                }
                ColumnLayout {
                    spacing: 2
                    Label {
                        text: qsTr("Welcome back")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    Label {
                        objectName: "homeWelcomeName"
                        text: root.displayName
                        textFormat: Text.PlainText
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textDisplay
                        font.weight: AppTheme.weightDisplay
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Pick up a conversation, or start something new.")
                color: AppTheme.textSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
            }

            // Offline notice, informational only: cached rooms stay reachable
            // and the status bar carries the detail.
            Rectangle {
                objectName: "homeOfflineNotice"
                visible: root.offline
                Layout.fillWidth: true
                radius: AppTheme.radiusMd
                // The warning chip family, so fill, border and icon share one
                // ink.
                color: AppTheme.chipWarningFill
                border.color: AppTheme.chipWarningBorder
                border.width: 1
                implicitHeight: offlineRow.implicitHeight + AppTheme.spacing12 * 2
                RowLayout {
                    id: offlineRow
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing10
                    Icon { name: "warning"; size: 18; color: AppTheme.warning }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("You appear to be offline. Reconnecting — "
                                   + "your rooms stay available.")
                        // textPrimary: secondary ink on the 14% status tint is
                        // below AA on the light themes.
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                }
            }

            // Actionable security state only; silent when all is fine.
            Rectangle {
                objectName: "homeSecurityCard"
                visible: app.cryptoBootstrap
                         && app.cryptoBootstrap.needsRecoveryKey === true
                Layout.fillWidth: true
                radius: AppTheme.radiusMd
                color: AppTheme.accentSoft
                border.color: AppTheme.accentBorder
                border.width: 1
                implicitHeight: securityRow.implicitHeight + AppTheme.spacing12 * 2
                RowLayout {
                    id: securityRow
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing10
                    Icon { name: "key"; size: 18; color: AppTheme.accent }
                    Label {
                        Layout.fillWidth: true
                        text: app.cryptoBootstrap
                              ? app.cryptoBootstrap.statusMessage : ""
                        textFormat: Text.PlainText
                        // As above: secondary ink on accentSoft is below AA.
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                    AppButton {
                        objectName: "homeSecurityButton"
                        // The destination depends on the phase:
                        // NoBackupAvailable needs to create a backup
                        // (Sessions); ManualRecoveryRequired and
                        // IdentityIncomplete need the recovery key field
                        // (Privacy & security).
                        readonly property bool wantsRecoveryInput:
                            app.cryptoBootstrap
                            && (app.cryptoBootstrap.phase
                                    === CryptoBootstrapModel.ManualRecoveryRequired
                                || app.cryptoBootstrap.phase
                                    === CryptoBootstrapModel.IdentityIncomplete)
                        text: wantsRecoveryInput ? qsTr("Enter recovery key")
                                                 : qsTr("Set up backup")
                        onClicked: app.showSettingsSection(
                                       wantsRecoveryInput ? "privacy" : "sessions")
                    }
                }
            }

            // Primary actions (shared Lightning buttons).
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: AppTheme.spacingS
                AppButton {
                    objectName: "homeNewMessageButton"
                    kind: "primary"
                    text: qsTr("New message")
                    visible: app.conversations && app.conversations.supported
                    onClicked: root.newMessageRequested()
                }
                AppButton {
                    objectName: "homeCreateRoomButton"
                    text: qsTr("Create room")
                    visible: app.conversations && app.conversations.supported
                    onClicked: root.createRoomRequested()
                }
                AppButton {
                    objectName: "homeCreateSpaceButton"
                    text: qsTr("Create Space")
                    visible: app.conversations && app.conversations.supported
                    onClicked: root.createSpaceRequested()
                }
                AppButton {
                    objectName: "homeSettingsButton"
                    text: qsTr("Settings")
                    onClicked: app.showSettings()
                }
            }

            // Quick-switcher hint.
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: AppTheme.spacingXS
                Label {
                    text: qsTr("Jump to anything with")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                // The shared keycap chip, as in the room-list search field.
                // `storm: false`: this surface uses the user's theme.
                MenuKeycap {
                    keys: "Ctrl+K"
                    storm: false
                }
            }

            // Jump back in: recent conversations.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                spacing: AppTheme.spacingXS
                visible: root.recentModel.length > 0
                Label {
                    text: qsTr("Jump back in")
                    color: AppTheme.sectionLabelColor
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                }
                Repeater {
                    model: root.recentModel
                    // AbstractButton for real keyboard focus and press states.
                    delegate: AbstractButton {
                        id: recentRow
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 48
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        padding: 0
                        leftPadding: AppTheme.spacingS
                        rightPadding: AppTheme.spacingS
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open %1")
                                             .arg(modelData.name || "")
                        onClicked: if (modelData.roomId)
                                       app.openRoom(modelData.roomId)

                        background: Rectangle {
                            radius: AppTheme.radiusMd
                            color: recentRow.down ? AppTheme.buttonGhostPressed
                                 : recentRow.hovered ? AppTheme.buttonGhostHover
                                                     : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                radius: AppTheme.radiusMd + 2
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.focusRing
                                visible: recentRow.visualFocus
                            }
                        }

                        contentItem: RowLayout {
                            spacing: AppTheme.spacing10
                            Avatar {
                                size: 32
                                name: recentRow.modelData.name || ""
                                mxc: recentRow.modelData.avatarUrl || ""
                                colorKey: recentRow.modelData.identityColorKey
                                          || recentRow.modelData.roomId || ""
                                circle: recentRow.modelData.isDirect === true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: recentRow.modelData.name
                                      || qsTr("Conversation")
                                textFormat: Text.PlainText
                                color: AppTheme.text
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                font.weight: recentRow.modelData.hasUnread
                                             ? AppTheme.weightBold
                                             : AppTheme.weightMedium
                                elide: Label.ElideRight
                            }
                            // Last-activity recency.
                            Label {
                                text: root.activityLabel(
                                          recentRow.modelData.lastActivity)
                                visible: text.length > 0
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                            // Mention badge, with the same tokens as the room
                            // list (mentionBadge / unreadBadge).
                            Rectangle {
                                visible: (recentRow.modelData.highlightCount || 0) > 0
                                radius: height / 2
                                color: AppTheme.mentionBadge
                                implicitHeight: 18
                                implicitWidth: Math.max(
                                    18, mentionLabel.implicitWidth + 10)
                                Label {
                                    id: mentionLabel
                                    anchors.centerIn: parent
                                    text: "@" + (recentRow.modelData.highlightCount > 99
                                                 ? "99+"
                                                 : recentRow.modelData.highlightCount)
                                    color: AppTheme.dangerText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightBold
                                }
                            }
                            // Unread dot / count.
                            Rectangle {
                                visible: recentRow.modelData.hasUnread === true
                                radius: height / 2
                                color: AppTheme.unreadBadge
                                implicitHeight: 18
                                implicitWidth: Math.max(
                                    18, countLabel.implicitWidth + 10)
                                Label {
                                    id: countLabel
                                    anchors.centerIn: parent
                                    visible: (recentRow.modelData.unreadCount || 0) > 0
                                    text: recentRow.modelData.unreadCount > 99
                                          ? "99+"
                                          : recentRow.modelData.unreadCount
                                    color: AppTheme.accentText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightBold
                                }
                            }
                        }
                    }
                }
            }

            // Spaces shortcut strip (the rail stays the main navigation).
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                spacing: AppTheme.spacingXS
                visible: root.spacesModel.length > 0
                Label {
                    text: qsTr("Your spaces")
                    color: AppTheme.sectionLabelColor
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    Repeater {
                        model: root.spacesModel
                        // A real button, as with the recent rows.
                        delegate: AbstractButton {
                            id: spacePill
                            required property var modelData
                            readonly property bool current:
                                app.spaces && modelData.roomId
                                && app.spaces.activeSpaceId === modelData.roomId
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            padding: 0
                            leftPadding: AppTheme.spacing10
                            rightPadding: AppTheme.spacing10
                            implicitHeight: 32
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Open Space %1")
                                .arg(modelData.name || "")
                            // Select and reveal: a nested Space has no rail row
                            // unless its ancestors are expanded, so revealSpace
                            // opens the chain and scrolls the rail to it.
                            onClicked: {
                                if (!modelData.roomId)
                                    return
                                if (app.spaces)
                                    app.spaces.activeSpaceId = modelData.roomId
                                if (app.railEntries)
                                    app.railEntries.revealSpace(
                                        modelData.roomId)
                            }

                            background: Rectangle {
                                radius: AppTheme.radiusPill
                                // Marks the Space you're already in.
                                color: spacePill.current
                                       ? AppTheme.accentSoft
                                     : spacePill.down
                                       ? AppTheme.buttonNeutralPressed
                                     : spacePill.hovered
                                       ? AppTheme.buttonNeutralHover
                                       : AppTheme.buttonNeutralFill
                                border.width: 1
                                border.color: spacePill.current
                                              ? AppTheme.accentBorder
                                              : AppTheme.buttonNeutralBorder
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    radius: AppTheme.radiusPill
                                    color: "transparent"
                                    border.width: 2
                                    border.color: AppTheme.focusRing
                                    visible: spacePill.visualFocus
                                }
                            }

                            contentItem: RowLayout {
                                spacing: AppTheme.spacing6
                                Avatar {
                                    size: 20
                                    name: spacePill.modelData.name || ""
                                    mxc: spacePill.modelData.avatarUrl || ""
                                    colorKey: spacePill.modelData.roomId || ""
                                }
                                Label {
                                    text: spacePill.modelData.name
                                          || qsTr("Space")
                                    textFormat: Text.PlainText
                                    color: AppTheme.text
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightMedium
                                }
                            }
                        }
                    }
                }
            }

            // Empty-account onboarding, explaining the primary actions above.
            Rectangle {
                objectName: "homeOnboarding"
                visible: root.recentModel.length === 0
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                radius: AppTheme.radiusMd
                color: AppTheme.cardElevated
                border.color: AppTheme.border
                border.width: 1
                implicitHeight: onboardingCol.implicitHeight
                                + AppTheme.spacing16 * 2
                ColumnLayout {
                    id: onboardingCol
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing16
                    spacing: AppTheme.spacingXS
                    Label {
                        text: qsTr("Nothing here yet")
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        Layout.fillWidth: true
                        // No join-by-address flow exists, so the copy mustn't
                        // promise one.
                        text: qsTr("Start a direct message to talk to someone, "
                                   + "create a room for a group, or organise "
                                   + "rooms into a Space. Invitations you "
                                   + "receive appear in the room list.")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                }
            }
        }
    }
}
