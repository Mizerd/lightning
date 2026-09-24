import QtQuick
import QtQuick.Controls
import MatrixClient

// A collapsible folder header in the Channels layout: a joined Space, or a
// non-Space group ("Invites", "Rooms"). A rotating chevron, the Space's
// avatar, and its name as given (not upper-cased).
//
// A collapsed folder shows the unread and mention totals of its rooms, so
// collapsing never silently mutes them.
ItemDelegate {
    id: root

    property string headerId: ""
    property string headerName: ""
    /// Empty for a group; a Space folder shows the Space's avatar.
    property string avatarUrl: ""
    property string identityColorKey: ""
    property bool showsAvatar: false
    property bool collapsed: false
    property int hiddenUnread: 0
    property int hiddenHighlight: 0

    // Loaded by the Channels presenter (five row kinds); the Loader takes its
    // height from this explicit value so rows stack instead of sitting at y=0.
    height: 32
    padding: 0
    hoverEnabled: true

    Accessible.role: Accessible.Button
    Accessible.name: {
        var base = root.headerName;
        if (root.hiddenHighlight > 0) {
            base = qsTr("%1, %2 mentions inside").arg(base).arg(root.hiddenHighlight);
        } else if (root.hiddenUnread > 0) {
            base = qsTr("%1, unread inside").arg(base);
        }
        return root.collapsed ? qsTr("%1, collapsed").arg(base) : qsTr("%1, expanded").arg(base);
    }
    // Tells a screen reader the row does something.
    Accessible.description: root.collapsed ? qsTr("Activate to expand") : qsTr("Activate to collapse")

    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: AppTheme.radiusSm
        color: root.hovered || root.activeFocus ? AppTheme.channelHover : "transparent"
        // An open folder keeps a quiet outline, so the column reads as groups.
        border.width: root.activeFocus ? 2 : (root.collapsed ? 0 : 1)
        border.color: root.activeFocus ? AppTheme.focusRing : AppTheme.border
    }

    contentItem: Item {
        anchors.fill: parent

        Icon {
            id: chevron
            anchors.verticalCenter: parent.verticalCenter
            x: 12
            name: "expand_more"
            size: 14
            color: AppTheme.channelCategoryText
            // Rotation rather than two glyphs, so it reads as one control
            // changing state.
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

        // Behind a Loader: an unresolved Space name is empty, the
        // ItemObservesViewport hazard.
        Loader {
            active: root.headerName.length > 0
            anchors.left: root.showsAvatar ? avatarLoader.right : chevron.right
            anchors.leftMargin: 6
            anchors.right: hiddenLoader.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.headerName
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightBold
                color: AppTheme.channelCategoryText
            }
        }

        // Only while collapsed, and only when there is something to say.
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

                // A mention gets the real count; plain unread gets a dot (a
                // summed unread count would be noise and the loudest thing in
                // the column).
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
