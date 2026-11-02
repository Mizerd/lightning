import QtQuick
import QtQuick.Controls
import MatrixClient

ItemDelegate {
    id: root

    property string headerId: ""
    property string headerName: ""
    property string avatarUrl: ""
    property string identityColorKey: ""
    property bool showsAvatar: false
    property bool collapsed: false
    property int hiddenUnread: 0
    property int hiddenHighlight: 0

    height: Math.max(AppTheme.channelRowMinHeight,
                    2 * AppTheme.scaled(AppTheme.textMeta))
    padding: 0
    hoverEnabled: true
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()

    Accessible.role: Accessible.Button
    Accessible.name: {
        var base = root.headerName;
        if (root.hiddenHighlight > 0) {
            base = qsTr("%1, %n mention(s) inside", "", root.hiddenHighlight).arg(base);
        } else if (root.hiddenUnread > 0) {
            base = qsTr("%1, unread inside").arg(base);
        }
        return root.collapsed ? qsTr("%1, collapsed").arg(base) : qsTr("%1, expanded").arg(base);
    }
    Accessible.description: root.collapsed ? qsTr("Activate to expand") : qsTr("Activate to collapse")

    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: AppTheme.radiusSm
        color: root.hovered || root.activeFocus ? AppTheme.channelHover : "transparent"
        border.width: root.activeFocus ? 2 : (root.collapsed ? 0 : 1)
        border.color: root.activeFocus ? AppTheme.focusRing : AppTheme.channelFolderBorder
    }

    contentItem: Item {
        anchors.fill: parent

        Icon {
            id: chevron
            objectName: "channelCategoryChevron"
            anchors.verticalCenter: parent.verticalCenter
            // Anchored, not `x`, so it mirrors in right-to-left languages.
            anchors.left: parent.left
            anchors.leftMargin: 12
            name: "expand_more"
            size: 14
            color: AppTheme.channelChevron
            rotation: root.collapsed ? -90 : 0
            Behavior on rotation {
                NumberAnimation {
                    duration: 120
                    easing.type: Easing.OutCubic
                }
            }
        }

        Loader {
            id: avatarLoader
            active: root.showsAvatar
            visible: active
            anchors.left: chevron.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Avatar {
                size: 18
                circle: false
                squareRadius: 5
                labelSize: 8
                name: root.headerName
                colorKey: root.identityColorKey.length > 0 ? root.identityColorKey : root.headerId
                mxc: root.avatarUrl
            }
        }

        Loader {
            objectName: "channelCategoryName"
            active: root.headerName.length > 0
            anchors.left: root.showsAvatar ? avatarLoader.right : chevron.right
            anchors.leftMargin: 6
            anchors.right: hiddenLoader.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                textFormat: Text.PlainText
                text: root.headerName
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightBold
                color: AppTheme.channelCategoryText
            }
        }

        Loader {
            id: hiddenLoader
            active: root.collapsed && (root.hiddenHighlight > 0 || root.hiddenUnread > 0)
            visible: active
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Item {
                implicitWidth: root.hiddenHighlight > 0 ? pill.implicitWidth : 8
                implicitHeight: 18

                UnreadBadge {
                    id: pill
                    anchors.centerIn: parent
                    visible: root.hiddenHighlight > 0
                    count: root.hiddenHighlight
                    mention: true
                }
                Rectangle {
                    anchors.centerIn: parent
                    visible: root.hiddenHighlight === 0
                    width: 8
                    height: 8
                    radius: 4
                    color: AppTheme.channelUnreadMark
                }
            }
        }
    }
}
