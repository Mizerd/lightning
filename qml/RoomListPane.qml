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
                text: app.loggedIn ? qsTr("No rooms yet") : qsTr("Sign in to see rooms")
                color: AppTheme.textMuted
            }
        }
    }
}
