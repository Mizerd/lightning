import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.1: Home / no-room surface. Shown in the timeline region when no room
// is selected (app.currentRoomId === ""), replacing the bare
// "Select a room from the left" placeholder; TimelinePane hides the composer
// in this state. Deliberately uncluttered (design language, current theme
// and fonts): a welcome, the primary create/find actions, and a short
// "jump back in" list of recent conversations. Keyboard accessible; the
// actions route up to the room list's shared new-conversation dialog.
Item {
    id: root
    objectName: "homePane"

    // Routed to the room list's new-conversation dialog by MainScreen.
    signal newMessageRequested()
    signal createRoomRequested()
    signal createSpaceRequested()

    readonly property string activeUserId: app.accounts ? app.accounts.activeUserId : ""
    readonly property var activeAccount: app.accounts && activeUserId.length > 0
                                         ? app.accounts.account(activeUserId) : null
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

    // Recent conversations are recomputed from the model rather than bound
    // through a Repeater over the whole room list, so hidden rows never spin
    // up avatar fetches.
    property var recentModel: []
    function refreshRecent() {
        recentModel = app.roomList ? app.roomList.recentRooms(6) : []
    }
    Component.onCompleted: refreshRecent()
    onVisibleChanged: if (visible) refreshRecent()
    Connections {
        target: app.roomList
        enabled: root.visible
        function onModelReset() { root.refreshRecent() }
        function onRowsInserted() { root.refreshRecent() }
        function onRowsRemoved() { root.refreshRecent() }
        function onDataChanged() { root.refreshRecent() }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: card.implicitHeight + AppTheme.spacing24 * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true

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
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    Label {
                        objectName: "homeWelcomeName"
                        text: root.displayName
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: 22
                        font.weight: Font.ExtraBold
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Pick up a conversation, or start something new.")
                color: AppTheme.textSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: 13
                wrapMode: Text.WordWrap
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
                    font.pixelSize: 12
                }
                Rectangle {
                    radius: 5
                    color: AppTheme.cardElevated
                    border.color: AppTheme.border
                    border.width: 1
                    implicitWidth: kbd.implicitWidth + 12
                    implicitHeight: kbd.implicitHeight + 4
                    Label {
                        id: kbd
                        anchors.centerIn: parent
                        text: "Ctrl+K"
                        color: AppTheme.textSecondary
                        font.family: AppTheme.monoFont
                        font.pixelSize: 11
                    }
                }
            }

            // Jump back in — recent conversations.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacingS
                spacing: AppTheme.spacingXS
                visible: root.recentModel.length > 0
                Label {
                    text: qsTr("JUMP BACK IN")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: 11
                    font.weight: Font.ExtraBold
                    font.letterSpacing: 0.8
                }
                Repeater {
                    model: root.recentModel
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 44
                        radius: AppTheme.radiusMd
                        color: recentHover.hovered ? AppTheme.hover : "transparent"
                        HoverHandler { id: recentHover }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacingS
                            anchors.rightMargin: AppTheme.spacingS
                            spacing: AppTheme.spacingS
                            Avatar {
                                size: 30
                                name: modelData.name || ""
                                mxc: modelData.avatarUrl || ""
                                colorKey: modelData.roomId || ""
                                circle: modelData.isDirect === true
                                roomGlyph: modelData.isDirect !== true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.name || qsTr("Conversation")
                                color: AppTheme.text
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.scaled(AppTheme.fontBody)
                                font.weight: modelData.hasUnread ? Font.Bold : Font.Medium
                                elide: Label.ElideRight
                            }
                            // Unread dot / count.
                            Rectangle {
                                visible: modelData.hasUnread === true
                                radius: height / 2
                                color: AppTheme.accent
                                implicitHeight: 18
                                implicitWidth: Math.max(18, countLabel.implicitWidth + 10)
                                Label {
                                    id: countLabel
                                    anchors.centerIn: parent
                                    visible: (modelData.unreadCount || 0) > 0
                                    text: modelData.unreadCount > 99 ? "99+"
                                          : modelData.unreadCount
                                    color: AppTheme.accentText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: 11
                                    font.weight: Font.ExtraBold
                                }
                            }
                        }
                        TapHandler {
                            onTapped: if (modelData.roomId)
                                          app.openRoom(modelData.roomId)
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open %1").arg(modelData.name || "")
                    }
                }
            }
        }
    }
}
