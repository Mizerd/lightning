import QtQuick
import QtQuick.Controls
import MatrixClient

// One icon-sized row shared by the Classic and Channels compact presenters.
ItemDelegate {
    id: root
    objectName: "compactRoomRow"
    property string label: ""
    property string rowId: ""
    property string avatarUrl: ""
    property string identityColorKey: ""
    property string iconName: ""
    property bool direct: false
    property bool selected: false
    property bool folder: false
    property bool folderCollapsed: false
    property int unreadCount: 0
    property int highlightCount: 0
    property bool hasUnread: false
    property bool muted: false
    property int threadUnreadKind: 0
    readonly property string tooltipText: label

    width: ListView.view ? ListView.view.width : 64
    height: 48
    padding: 0
    hoverEnabled: true
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()
    Accessible.role: Accessible.Button
    Accessible.name: muted ? qsTr("%1, muted").arg(label)
                     : threadUnreadKind === 2 && highlightCount === 0
                     ? qsTr("%1, mention in a thread", "compact room row").arg(label)
                     : highlightCount > 0
                     ? qsTr("%1, %n mention(s)", "compact room row", highlightCount).arg(label)
                     : hasUnread || unreadCount > 0 || threadUnreadKind > 0
                       ? qsTr("%1, unread", "compact room row").arg(label)
                       : label
    Accessible.description: folder
                            ? (folderCollapsed ? qsTr("Activate to expand")
                                               : qsTr("Activate to collapse")) : ""
    ToolTip.text: tooltipText
    ToolTip.visible: hovered && label.length > 0
    ToolTip.delay: 500

    background: Rectangle {
        x: 4; y: 2; width: root.width - 8; height: root.height - 4
        radius: AppTheme.radiusMd
        color: root.selected ? AppTheme.roomSelected
             : root.hovered ? AppTheme.roomHover : "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: AppTheme.focusRing
    }
    contentItem: Item {
        Avatar {
            id: avatar
            objectName: "compactRoomAvatar"
            anchors.centerIn: parent
            visible: root.iconName.length === 0
            size: 32
            circle: root.direct
            name: root.label
            mxc: root.avatarUrl
            colorKey: root.identityColorKey || root.rowId
        }
        Icon {
            anchors.centerIn: parent
            visible: root.iconName.length > 0
            name: root.iconName
            size: 22
            color: AppTheme.channelText
        }
        UnreadBadge {
            id: badge
            objectName: "compactRoomBadge"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 2
            anchors.bottomMargin: 2
            count: root.muted ? 0 : root.highlightCount > 0
                   ? root.highlightCount : root.unreadCount
            mention: root.highlightCount > 0 || root.threadUnreadKind === 2
            dot: !root.muted && (root.hasUnread || root.threadUnreadKind > 0)
        }
    }
}
