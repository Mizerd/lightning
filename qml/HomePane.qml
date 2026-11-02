import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    objectName: "homePane"

    signal newMessageRequested()
    signal createRoomRequested()
    signal createSpaceRequested()
    signal discoverRequested()

    // `account()` is a Q_INVOKABLE, so a binding on it would miss renames and
    // avatar changes. Both fields are refreshed by one function driven by the
    // manager's signals (as the rail's account tile and Settings identity card
    // do). A local binding with a change handler would run that handler inside
    // `displayName`'s first evaluation and cause a binding loop.
    property string activeUserId: ""
    property var activeAccount: null
    function refreshActiveAccount() {
        var uid = app.accounts ? app.accounts.activeUserId : ""
        root.activeAccount = (app.accounts && uid.length > 0)
            ? app.accounts.account(uid) : null
        root.activeUserId = uid
    }
    Connections {
        target: app.accounts
        function onAccountsChanged() { root.refreshActiveAccount() }
        function onActiveUserIdChanged() { root.refreshActiveAccount() }
    }
    // Something to greet: an account id or a name. Without one (a mock or a
    // half-restored session) the greeting is plain "Welcome back" with no
    // avatar, never a placeholder word drawn as somebody's initials.
    readonly property bool hasIdentity:
        activeUserId.length > 0
        || !!(activeAccount && activeAccount.displayName)
    readonly property string displayName: {
        var n = activeAccount && activeAccount.displayName
                ? activeAccount.displayName : ""
        if (n.length > 0)
            return n
        var id = activeUserId
        if (id.length === 0)
            return qsTr("there")
        if (id.charAt(0) === "@")
            id = id.substring(1)
        var colon = id.indexOf(":")
        return colon > 0 ? id.substring(0, colon) : id
    }

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

    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.locale(AppLocale.name).toString(when, "ddd")
        return Qt.locale(AppLocale.name).toString(when, "d MMM")
    }

    readonly property bool offline:
        app.connectionStatus === qsTr("Offline — retrying")
        || app.connectionStatus === qsTr("Not connected")
    readonly property bool syncFailed: app.connectionStatus === qsTr("Error")
    readonly property string connectionNotice: syncFailed
        ? qsTr("Lightning can't sync with your server. Your rooms stay "
               + "available, but new messages may not arrive.")
        : (offline ? (routed
                      ? qsTr("Offline: this account's proxy or homeserver can't be "
                             + "reached. Lightning keeps trying through the proxy, "
                             + "never directly — your rooms stay available.")
                      : qsTr("You appear to be offline. Reconnecting — "
                             + "your rooms stay available."))
                   : "")
    readonly property bool routed: !!app.networkRoute && !!app.networkRoute.route
        && (app.networkRoute.route.kind || "none") !== "none"

    ChatBackgroundDialog {
        id: homeBackgroundDialog
        objectName: "homeChatBackgroundDialog"
    }

    readonly property int cornerPromptReserve:
        (app.messageSearch && app.messageSearch.indexAllOffered) ? 232 : 0

    Flickable {
        id: homeFlick
        objectName: "homeFlick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: card.implicitHeight + AppTheme.spacing24 * 2
                       + root.cornerPromptReserve
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        SmoothWheelArea { id: homeWheel }
        ScrollBar.vertical: AppScrollBar {
            objectName: "homeScrollBar"
            policy: ScrollBar.AsNeeded
        }
        function scrollKey(key) {
            var max = Math.max(0, contentHeight - height)
            var y = contentY
            switch (key) {
            case Qt.Key_Home: y = 0; break
            case Qt.Key_End: y = max; break
            case Qt.Key_PageUp: y = contentY - height * 0.9; break
            case Qt.Key_PageDown: y = contentY + height * 0.9; break
            case Qt.Key_Up: y = contentY - 48; break
            case Qt.Key_Down: y = contentY + 48; break
            default: return false
            }
            homeWheel.stopGlide()
            contentY = Math.max(0, Math.min(max, y))
            return true
        }
        Keys.onPressed: function(event) {
            if (homeFlick.scrollKey(event.key))
                event.accepted = true
        }
        TapHandler { onTapped: homeFlick.forceActiveFocus() }

        ColumnLayout {
            id: card
            objectName: "homeCard"
            width: Math.min(560, parent.width - AppTheme.spacing24 * 2)
            x: (parent.width - width) / 2
            y: Math.max(AppTheme.spacing24,
                        (root.height - root.cornerPromptReserve
                         - implicitHeight) / 2)
            spacing: AppTheme.spacing20

            RowLayout {
                objectName: "homeWelcomeRow"
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: card.width
                spacing: AppTheme.spacing12
                Avatar {
                    objectName: "homeWelcomeAvatar"
                    visible: root.hasIdentity
                    size: 56
                    circle: true
                    name: root.displayName
                    mxc: root.activeAccount ? (root.activeAccount.avatarUrl || "") : ""
                    colorKey: root.activeUserId
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        visible: root.hasIdentity
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: qsTr("Welcome back")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    Label {
                        objectName: "homeWelcomeName"
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: root.hasIdentity ? root.displayName
                                               : qsTr("Welcome back")
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

            Rectangle {
                objectName: "homeOfflineNotice"
                visible: root.connectionNotice !== ""
                Layout.fillWidth: true
                radius: AppTheme.radiusMd
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
                        objectName: "homeConnectionNoticeText"
                        Layout.fillWidth: true
                        text: root.connectionNotice
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                }
            }

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
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                    AppButton {
                        objectName: "homeSecurityButton"
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

            Item {
                id: actionsHolder
                objectName: "homeActionsHolder"
                Layout.fillWidth: true
                implicitHeight: actionsFlow.height
                Flow {
                    id: actionsFlow
                    objectName: "homeActionsFlow"
                    anchors.horizontalCenter: parent.horizontalCenter
                    readonly property real oneRow: {
                        var w = 0, n = 0
                        for (var i = 0; i < children.length; ++i) {
                            var c = children[i]
                            if (c.visible) { w += c.implicitWidth; ++n }
                        }
                        return w + Math.max(0, n - 1) * spacing
                    }
                    width: Math.min(actionsHolder.width, oneRow)
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
                        objectName: "homeExploreButton"
                        iconName: "explore"
                        text: qsTr("Explore rooms")
                        visible: app.discovery && app.discovery.supported
                        onClicked: root.discoverRequested()
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: AppTheme.spacingXS
                Label {
                    text: qsTr("Jump to anything with")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                MenuKeycap {
                    keys: "Ctrl+K"
                    storm: false
                }
            }

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
                    delegate: AbstractButton {
                        id: recentRow
                        required property var modelData
                        property int notificationMode: 0
                        readonly property bool muted: notificationMode === 2
                        function refreshNotificationMode() {
                            if (typeof app === "undefined" || !app || !app.settings)
                                return
                            var id = modelData.roomId || ""
                            notificationMode = id.length > 0
                                               ? app.settings.roomNotificationMode(id) : 0
                        }
                        Component.onCompleted: refreshNotificationMode()
                        onModelDataChanged: refreshNotificationMode()
                        Connections {
                            target: (typeof app !== "undefined" && app) ? app.settings : null
                            function onRoomNotificationModeChanged(changedRoomId) {
                                if (changedRoomId === recentRow.modelData.roomId)
                                    recentRow.refreshNotificationMode()
                            }
                        }
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
                                font.weight: (recentRow.modelData.hasUnread && !recentRow.muted)
                                             ? AppTheme.weightBold
                                             : AppTheme.weightMedium
                                elide: Label.ElideRight
                            }
                            Label {
                                text: root.activityLabel(
                                          recentRow.modelData.lastActivity)
                                visible: text.length > 0
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                            Rectangle {
                                objectName: "homeMentionBadge"
                                visible: !recentRow.muted
                                         && (recentRow.modelData.highlightCount || 0) > 0
                                radius: height / 2
                                color: AppTheme.mentionBadgeFill
                                implicitHeight: 18
                                implicitWidth: Math.max(
                                    18, mentionLabel.implicitWidth + 10)
                                Label {
                                    id: mentionLabel
                                    anchors.centerIn: parent
                                    text: "@" + (recentRow.modelData.highlightCount > 99
                                                 ? "99+"
                                                 : recentRow.modelData.highlightCount)
                                    color: AppTheme.mentionBadgeInk
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightBold
                                }
                            }
                            Rectangle {
                                objectName: "homeUnreadBadge"
                                visible: !recentRow.muted
                                         && recentRow.modelData.hasUnread === true
                                radius: height / 2
                                color: (recentRow.modelData.unreadCount || 0) > 0
                                       ? AppTheme.unreadCountFill : AppTheme.unreadDot
                                border.width: (recentRow.modelData.unreadCount || 0) > 0
                                              ? 0 : AppTheme.badgeDotRingWidth
                                border.color: AppTheme.unreadDotRing
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
                                    color: AppTheme.unreadCountInk
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightBold
                                }
                            }
                        }
                    }
                }
            }

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
                            width: Math.min(implicitWidth, parent ? parent.width
                                                                  : implicitWidth)
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Open Space %1")
                                .arg(modelData.name || "")
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

                            ToolTip.text: spacePill.modelData.name || ""
                            ToolTip.visible: spacePill.hovered
                                             && spacePillLabel.truncated
                            ToolTip.delay: 500

                            contentItem: RowLayout {
                                spacing: AppTheme.spacing6
                                Avatar {
                                    size: 20
                                    name: spacePill.modelData.name || ""
                                    mxc: spacePill.modelData.avatarUrl || ""
                                    colorKey: spacePill.modelData.roomId || ""
                                }
                                Label {
                                    id: spacePillLabel
                                    objectName: "homeSpacePillLabel"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
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

            ColumnLayout {
                objectName: "homePersonalise"
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                spacing: AppTheme.spacingXS
                Label {
                    text: qsTr("Make it yours")
                    color: AppTheme.sectionLabelColor
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    AppButton {
                        objectName: "homeThemeButton"
                        size: "sm"
                        iconName: "palette"
                        text: qsTr("Theme and colours")
                        Accessible.description:
                            qsTr("Themes, custom colours and gradients")
                        ToolTip.text: qsTr("Themes, custom colours and gradients")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: app.showSettingsSection("appearance")
                    }
                    AppButton {
                        objectName: "homeChatBackgroundButton"
                        visible: !!app.backdrops
                        size: "sm"
                        iconName: "image"
                        text: qsTr("Chat background")
                        onClicked: homeBackgroundDialog.openForDefault()
                    }
                    AppButton {
                        objectName: "homeProfileButton"
                        size: "sm"
                        iconName: "account_circle"
                        text: qsTr("Name and picture")
                        onClicked: app.showSettingsSection("account")
                    }
                    AppButton {
                        objectName: "homeSettingsButton"
                        size: "sm"
                        kind: "ghost"
                        iconName: "settings"
                        text: qsTr("All settings")
                        onClicked: app.showSettings()
                    }
                }
            }

            Rectangle {
                id: homeOnboarding
                objectName: "homeOnboarding"
                visible: root.recentModel.length === 0
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                radius: AppTheme.radiusMd
                readonly property bool filtered:
                    !!app.roomList && (app.roomList.searchQuery || "").length > 0
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
                        objectName: "homeOnboardingTitle"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: homeOnboarding.filtered
                              ? qsTr("No rooms match") : qsTr("Nothing here yet")
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        objectName: "homeOnboardingBody"
                        Layout.fillWidth: true
                        text: homeOnboarding.filtered
                              ? qsTr("Nothing in your room list matches the "
                                     + "filter. Clear it to see your rooms.")
                              : qsTr("Start a direct message to talk to someone, "
                                   + "create a room for a group, or organise "
                                   + "rooms into a Space. Explore rooms finds "
                                   + "public ones to join. Invitations you "
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

    MiddleClickScroller {
        objectName: "homeMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: homeFlick
    }
}
