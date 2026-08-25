import QtQuick
import QtQuick.Controls
import MatrixClient

// A collapsible folder header in the Channels layout — a joined Space, or one
// of the non-Space groups ("Invites", "Rooms").
//
// Sable-first, as directed: a chevron that rotates, the Space's own avatar,
// and its name in the case its admin actually gave it. It used to upper-case
// the label, which is right for a generic "CHANNELS" heading and wrong the
// moment the heading IS a Space someone named — nobody calls their space
// TRADEMARK TRAILWAYS.
//
// One thing it must never do is hide activity. A collapsed folder carries the
// unread and mention totals of the rooms inside it, because otherwise
// collapsing silently mutes them — and the user collapsed it to save space,
// not to stop being told.
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

    // A Loader-hosted row: the Channels presenter picks between five row
    // kinds, so this is loaded rather than declared inline. The Loader takes
    // its height from this value, which is what makes the rows lay out one
    // below another instead of stacking at y=0.
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
    // A screen reader needs to know this row DOES something, and "category"
    // alone does not say that.
    Accessible.description: root.collapsed ? qsTr("Activate to expand") : qsTr("Activate to collapse")

    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: AppTheme.radiusSm
        color: root.hovered || root.activeFocus ? AppTheme.channelHover : "transparent"
        // An OPEN folder keeps a quiet outline, so a long column reads as a
        // set of groups rather than as one run of rows with occasional bold
        // text in it. Sable draws the same pill.
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
            // Rotation rather than two glyphs, so the transition reads as one
            // control changing state.
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

        // Behind a Loader: a Space whose name has not resolved yet renders
        // this row with an empty string, which is the ItemObservesViewport
        // hazard.
        Loader {
            active: root.headerName.length > 0
            anchors.left: root.showsAvatar ? avatarLoader.right : chevron.right
            anchors.leftMargin: 6
            anchors.right: hiddenLoader.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
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

                // A mention inside gets the real count; plain unread gets a
                // dot, because the SUM of unread counts across a collapsed
                // group is a number nobody asked for and it would be the
                // loudest thing in the column.
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
