import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    objectName: "identityCard"

    // No property named `name` (IconChromeTest's repo-wide scan).
    property bool active: false
    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    property int unreadCount: 0
    property int mentionCount: 0
    property bool activityDot: false
    property bool mentionsLowerBound: false
    property int liveState: 0
    property bool needsSignIn: false
    property bool healthWarning: false

    property int rowHeight: AppTheme.scaled(44)

    signal activated()
    signal removeRequested()

    readonly property bool focusWithin: root.activeFocus || removeButton.activeFocus

    readonly property bool pointerWithin: cardHover.hovered
                                          || cardMouse.containsMouse
                                          || removeButton.hovered
    HoverHandler { id: cardHover }

    readonly property string localpart: {
        var uid = root.userId
        if (uid.indexOf("@") === 0) uid = uid.slice(1)
        var colon = uid.indexOf(":")
        return colon > 0 ? uid.slice(0, colon) : uid
    }
    readonly property string visibleName:
        root.displayName.length > 0 ? root.displayName : root.localpart

    readonly property alias identityLabel: idLabel

    implicitWidth: 280
    implicitHeight: rowHeight
    readonly property real textColumnHeight: textColumn.implicitHeight

    Accessible.role: Accessible.Button
    Accessible.focusable: true
    Accessible.name: root.active
        ? qsTr("Active account, %1, %2").arg(root.visibleName).arg(root.userId)
        : qsTr("Switch to %1, %2").arg(root.visibleName).arg(root.userId)
    Accessible.description: {
        const base = root.active ? qsTr("This is the active account")
            : (root.needsSignIn ? qsTr("Needs sign-in")
               : (root.healthWarning ? qsTr("Encryption needs attention") : ""))
        if (root.active || root.liveState === 0)
            return base
        const parts = []
        if (base.length > 0) parts.push(base)
        if (activityBadge.stateLabel.length > 0) parts.push(activityBadge.stateLabel)
        if (activityBadge.activityLabel.length > 0) parts.push(activityBadge.activityLabel)
        return parts.join(", ")
    }
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onReturnPressed: root.activated()
    Keys.onEnterPressed: root.activated()
    Keys.onSpacePressed: root.activated()

    property color hostSurface: AppTheme.stormCanvas

    readonly property color rowFill: AppTheme.flatten(rowChip.color,
                                                      root.hostSurface)

    Rectangle {
        id: rowChip
        objectName: "identityCardRowChip"
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        radius: AppTheme.radiusMd
        color: root.active
               ? (root.pointerWithin ? AppTheme.selectedHover
                                     : AppTheme.selected)
               : (root.pointerWithin ? AppTheme.hover : "transparent")
    }

    Rectangle {
        objectName: "identityCardActiveEdge"
        visible: root.active
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.max(16, parent.height - AppTheme.spacing12)
        radius: width / 2
        color: AppTheme.bolt
    }

    Rectangle {
        visible: root.activeFocus
        anchors.fill: rowChip
        radius: rowChip.radius
        color: "transparent"
        border.width: 2
        border.color: AppTheme.bolt
    }

    MouseArea {
        id: cardMouse
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        onClicked: {
            root.forceActiveFocus()
            root.activated()
        }
    }

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing12
        anchors.rightMargin: AppTheme.spacing6
        spacing: AppTheme.spacing8

        Item {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: AppTheme.scaled(28)
            implicitHeight: AppTheme.scaled(28)
            Avatar {
                anchors.centerIn: parent
                size: AppTheme.scaled(28)
                circle: true
                name: root.visibleName
                mxc: root.avatarMxc
                fallbackSource: (typeof app !== "undefined" && app
                                 && app.accountAvatars && root.userId.length > 0)
                                ? app.accountAvatars.avatarUrlFor(root.userId)
                                : ""
                colorKey: root.userId
            }
            Rectangle {
                objectName: "identityCardAvatarRing"
                visible: root.active
                anchors.fill: parent
                anchors.margins: -3
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: AppTheme.bolt
            }
        }

        ColumnLayout {
            id: textColumn
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            Label {
                objectName: "identityCardName"
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.visibleName
                color: AppTheme.legibleInkOn(AppTheme.stormText, root.rowFill)
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                font.weight: root.active ? AppTheme.weightBold
                                         : AppTheme.weightStrong
                elide: Label.ElideRight
            }
            Label {
                id: idLabel
                objectName: "identityCardUserId"
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.userId
                color: AppTheme.legibleInkOn(root.active
                                             ? AppTheme.stormTextSecondary
                                             : AppTheme.stormTextMuted,
                                             root.rowFill)
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.scaled(AppTheme.fontMonoXS)
                elide: Label.ElideMiddle
            }
        }

        Icon {
            objectName: "identityCardHealthIcon"
            visible: root.needsSignIn || root.healthWarning
            name: root.needsSignIn ? "error" : "warning"
            size: AppTheme.scaled(15)
            color: AppTheme.stormDanger
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }
        AccountActivityBadge {
            id: activityBadge
            objectName: "identityCardActivityBadge"
            Layout.alignment: Qt.AlignVCenter
            activeAccount: root.active
            liveState: root.liveState
            mentionCount: root.mentionCount
            mentionsLowerBound: root.mentionsLowerBound
            hasUnread: root.activityDot
            visible: root.liveState !== 0 && activityBadge.shown
            opacity: root.pointerWithin ? 0 : 1
            Accessible.ignored: true
        }
        Icon {
            objectName: "identityCardActiveTick"
            visible: root.active
            name: "check_circle"
            size: AppTheme.scaled(17)
            color: AppTheme.bolt
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }
        ToolButton {
            id: removeButton
            objectName: "identityCardRemoveButton"
            readonly property bool revealed:
                !root.active && (root.pointerWithin || root.activeFocus
                                 || removeButton.activeFocus)
            visible: !root.active
            opacity: revealed ? 1 : 0
            enabled: revealed
            activeFocusOnTab: !root.active
            Accessible.ignored: !revealed
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 28
            implicitHeight: 30
            Accessible.name: qsTr("Remove account %1").arg(root.userId)
            ToolTip.text: qsTr("Remove from this device")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            contentItem: Icon {
                name: "close"
                size: 15
                color: AppTheme.stormDanger
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: removeButton.hovered ? AppTheme.stormSelection
                                            : "transparent"
                Rectangle {
                    objectName: "identityCardRemoveFocusRing"
                    visible: removeButton.visualFocus
                    anchors.fill: parent
                    radius: AppTheme.radiusSm
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.bolt
                }
            }
            onClicked: root.removeRequested()
        }
    }
}
