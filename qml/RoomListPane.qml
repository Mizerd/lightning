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
                // Bound to the model's `count` so the header follows rooms
                // arriving after the initial sync; empty before then rather
                // than a bogus "0".
                Label {
                    text: app.initialSyncDone ? list.count.toString() : ""
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
            }
        }

        // Space chip strip, visible only when the backend knows at least one
        // Space.
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
                            textFormat: Text.PlainText
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

            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

            // State-aware empty label, in precedence order:
            //   1. not signed in                → "Sign in to see rooms"
            //   2. no sync response yet         → "Loading rooms…"
            //   3. live, a real Space selected  → "No rooms in this Space"
            //   4. live, All rooms, still empty → "No joined rooms"
            Label {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacingXL * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: list.count === 0
                text: {
                    if (!app.loggedIn)
                        return qsTr("Sign in to see rooms")
                    if (!app.initialSyncDone)
                        return qsTr("Loading rooms…")
                    if (app.spaces && app.spaces.activeSpaceId
                        && app.spaces.activeSpaceId !== ""
                        && app.spaces.activeSpaceId !== "@orphans")
                        return qsTr("No rooms in this Space")
                    return qsTr("No joined rooms")
                }
                color: AppTheme.textMuted
            }
        }
    }
}
