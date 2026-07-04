import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.surface

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: header.implicitHeight + AppTheme.spacingM * 2
            color: AppTheme.surface
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.margins: AppTheme.spacingM
                Label {
                    text: qsTr("Rooms")
                    color: AppTheme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: app.roomList.rowCount ? app.roomList.rowCount() : ""
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
            }
        }

        // v0.4.1: Space chip strip. Only visible when the current backend
        // knows about at least one Space (SpaceManager surfaces an "All
        // rooms" pseudo-row plus real Spaces plus optional "Other rooms").
        Rectangle {
            Layout.fillWidth: true
            visible: app.spaces && app.spaces.hasSpaces
            implicitHeight: spaceStrip.height + AppTheme.spacingS * 2
            color: AppTheme.surfaceAlt

            ListView {
                id: spaceStrip
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 32
                orientation: ListView.Horizontal
                clip: true
                spacing: AppTheme.spacingS
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM
                model: app.spaces
                delegate: Rectangle {
                    property bool selected: model.spaceId === app.spaces.activeSpaceId
                    height: 28
                    radius: 14
                    color: selected ? AppTheme.accent : AppTheme.surface
                    border.color: selected ? AppTheme.accent : AppTheme.border
                    border.width: 1
                    width: chipRow.implicitWidth + AppTheme.spacingM * 2

                    RowLayout {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: AppTheme.spacingXS
                        Label {
                            text: model.name
                            color: parent.parent.selected ? AppTheme.accentText : AppTheme.text
                            font.pixelSize: 12
                        }
                        Label {
                            visible: model.childCount > 0
                            text: "· " + model.childCount
                            color: parent.parent.selected ? AppTheme.accentText : AppTheme.textMuted
                            font.pixelSize: 12
                        }
                    }

                    TapHandler {
                        onTapped: app.spaces.activeSpaceId = model.spaceId
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.roomList
            currentIndex: -1
            spacing: 0

            delegate: RoomDelegate {
                width: ListView.view.width
                selected: model.roomId === app.currentRoomId
                onClicked: app.openRoom(model.roomId)
            }

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: app.loggedIn ? qsTr("No rooms in this Space") : qsTr("Sign in to see rooms")
                color: AppTheme.textMuted
            }
        }
    }
}
