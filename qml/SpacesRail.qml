import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.5.4: narrow far-left Spaces rail (~56 px wide).
// Visible only when there are real Matrix Spaces (app.spaces.hasSpaces).
// Each item is a 40×40 circle avatar with the first letter of the space name
// (or a symbol for pseudo-rows). Tapping sets app.spaces.activeSpaceId.
// A 3 px coloured left-edge bar marks the active selection.
Rectangle {
    id: root
    color: AppTheme.sidebar
    visible: true

    ListView {
        id: list
        anchors.fill: parent
        model: app.spaces
        clip: true
        spacing: 0

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Item {
            id: spaceItem
            width: list.width
            height: 56

            property bool isActive: app.spaces && app.spaces.activeSpaceId === model.spaceId
            property bool isPseudo: model.spaceId === "" || model.spaceId === "@orphans"

            // Left selection indicator bar
            Rectangle {
                width: 3
                height: parent.height * 0.6
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                radius: 2
                color: AppTheme.accent
                visible: spaceItem.isActive
            }

            // Avatar circle: pill when inactive, rounded-square when active
            Rectangle {
                id: spaceCircle
                width: 40; height: 40
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: model.level > 0 ? 5 : 0
                radius: spaceItem.isActive ? AppTheme.radiusSm : AppTheme.radiusPill
                color: spaceItem.isActive ? AppTheme.accent : AppTheme.cardElevated

                Behavior on radius { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                Behavior on color  { ColorAnimation  { duration: 120 } }

                // Symbol or first letter
                Label {
                    anchors.centerIn: parent
                    text: model.spaceId === ""
                          ? "⊞"
                          : model.spaceId === "@orphans"
                            ? "◦"
                            : (model.name && model.name.length > 0
                               ? model.name[0].toUpperCase() : "#")
                    font.pixelSize: spaceItem.isPseudo ? 20 : 16
                    font.weight: Font.DemiBold
                    color: spaceItem.isActive ? AppTheme.accentText : AppTheme.textSecondary
                }

                // Unread count badge
                Rectangle {
                    visible: model.unreadTotal > 0 && !spaceItem.isActive
                    width: Math.max(18, badgeLabel.implicitWidth + 6)
                    height: 18
                    radius: 9
                    color: model.highlightTotal > 0 ? AppTheme.error : AppTheme.accent
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: -3
                    anchors.rightMargin: -4

                    Label {
                        id: badgeLabel
                        anchors.centerIn: parent
                        text: model.unreadTotal > 99 ? "99+" : model.unreadTotal.toString()
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        color: "#FFFFFF"
                    }
                }
            }

            // Hover highlight ring around the circle
            HoverHandler { id: spaceHover }
            Rectangle {
                anchors.fill: spaceCircle
                anchors.margins: -3
                radius: spaceCircle.radius + 3
                color: AppTheme.hover
                visible: spaceHover.hovered && !spaceItem.isActive
                z: -1
            }

            TapHandler {
                onTapped: if (app.spaces) app.spaces.activeSpaceId = model.spaceId
            }

            ToolTip {
                visible: spaceHover.hovered
                text: model.spaceId === ""
                      ? qsTr("All rooms")
                      : model.spaceId === "@orphans"
                        ? qsTr("Other rooms")
                        : (model.name || "")
                delay: 500
            }
        }
    }
}
