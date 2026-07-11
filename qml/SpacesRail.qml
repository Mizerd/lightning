import QtQuick
import QtQuick.Controls
import QtQuick.Effects
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
                    visible: spaceImage.status !== Image.Ready
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

                // Real Space avatar (a Space is a room). Fetched through the
                // shared media bridge; only shown once fully loaded so the
                // letter placeholder never flickers to a broken glyph. Masked
                // to the (animated) circle/rounded-square shape.
                Image {
                    id: spaceImage
                    anchors.fill: parent
                    visible: false
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    property string mxc: spaceItem.isPseudo
                                         ? "" : (model.avatarUrl || "")
                    source: mxc.length > 0 && app.mediaBridge.supported
                            ? app.mediaBridge.avatarSource(mxc, 80) : ""
                    Connections {
                        target: app.mediaBridge
                        enabled: spaceImage.mxc.length > 0
                        function onMediaCached(cacheKey) {
                            spaceImage.source = app.mediaBridge.avatarSource(
                                spaceImage.mxc, 80)
                        }
                    }
                }
                Item {
                    id: spaceMask
                    anchors.fill: parent
                    visible: false
                    layer.enabled: true
                    Rectangle {
                        anchors.fill: parent
                        radius: spaceCircle.radius
                        color: "black"
                    }
                }
                MultiEffect {
                    anchors.fill: parent
                    source: spaceImage
                    maskEnabled: true
                    maskSource: spaceMask
                    visible: spaceImage.status === Image.Ready
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
