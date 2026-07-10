import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.background

    property var currentRoom: ({})

    function refreshCurrentRoom() {
        currentRoom = app.currentRoomId === ""
                      ? ({})
                      : app.roomList.findRoom(app.currentRoomId)
    }

    Connections {
        target: app
        function onCurrentRoomIdChanged() { refreshCurrentRoom() }
    }
    Connections {
        target: app.roomList
        function onDataChanged() { refreshCurrentRoom() }
        function onModelReset() { refreshCurrentRoom() }
    }
    Component.onCompleted: refreshCurrentRoom()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Room header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: header.implicitHeight + AppTheme.spacingM * 2
            color: AppTheme.surface
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.margins: AppTheme.spacingM
                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Label {
                            text: root.currentRoom.name
                                  ? root.currentRoom.name
                                  : (app.currentRoomId === ""
                                     ? qsTr("No room selected")
                                     : app.currentRoomId)
                            color: AppTheme.text
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Label {
                            visible: root.currentRoom.encrypted === true
                            text: "\u{1F512}"
                            color: AppTheme.textMuted
                            font.pixelSize: 12
                        }
                    }
                    Label {
                        text: root.currentRoom.topic || ""
                        color: AppTheme.textMuted
                        font.pixelSize: 12
                        visible: text.length > 0
                        Layout.fillWidth: true
                        elide: Label.ElideRight
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // Timeline
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: timeline
                anchors.fill: parent
                clip: true
                spacing: AppTheme.spacingS
                model: app.timeline
                verticalLayoutDirection: ListView.TopToBottom
                topMargin: AppTheme.spacingM
                bottomMargin: AppTheme.spacingM
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM

                delegate: MessageDelegate {
                    width: ListView.view.width - AppTheme.spacingM * 2
                }

                // Auto-scroll to end on new events when already near the bottom.
                property bool stickToBottom: true
                onContentYChanged: {
                    if (!moving) return
                    stickToBottom = (contentY + height >= contentHeight - 40)
                }
                onCountChanged: {
                    if (stickToBottom) positionViewAtEnd()
                    // Send read receipt for latest visible.
                    if (count > 0) app.timeline.markVisibleAsRead(0, count - 1)
                }
                Component.onCompleted: positionViewAtEnd()

                // Pagination trigger: scroll to top with backfill available.
                onAtYBeginningChanged: {
                    if (atYBeginning && app.timeline.canPaginate && !app.timeline.paginating)
                        app.timeline.requestOlder()
                }

                header: Item {
                    width: timeline.width
                    height: paginationHeader.visible ? paginationHeader.implicitHeight + 8
                                                     : 0
                    Row {
                        id: paginationHeader
                        anchors.centerIn: parent
                        spacing: 6
                        // v0.5.7: hidden entirely once the start of history
                        // is reached (no permanent placeholder).
                        visible: app.currentRoomId !== ""
                                 && (app.timeline.canPaginate
                                     || app.timeline.paginating
                                     || app.timeline.paginationFailed)
                        BusyIndicator {
                            width: 16; height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            running: app.timeline.paginating
                            visible: app.timeline.paginating
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: app.timeline.paginating
                                  ? qsTr("Loading older messages…")
                                  : (app.timeline.paginationFailed
                                     ? qsTr("Could not load older messages —")
                                     : qsTr("Scroll up to load more history"))
                            color: app.timeline.paginationFailed
                                   ? AppTheme.error : AppTheme.textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: app.timeline.paginationFailed
                                     && !app.timeline.paginating
                            text: qsTr("Retry")
                            color: AppTheme.accent
                            font.pixelSize: 11
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.timeline.requestOlder()
                            }
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Label {
                    anchors.centerIn: parent
                    visible: timeline.count === 0
                    text: app.currentRoomId === ""
                          ? qsTr("Select a room from the left")
                          : qsTr("No messages yet")
                    color: AppTheme.textMuted
                }
            }
        }

        // Typing indicator
        Rectangle {
            Layout.fillWidth: true
            visible: app.timeline.typingText && app.timeline.typingText.length > 0
            implicitHeight: typingLabel.implicitHeight + 6
            color: AppTheme.surface
            Label {
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                color: AppTheme.textMuted
                font.italic: true
                font.pixelSize: 11
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        MessageComposerBar { Layout.fillWidth: true }
    }
}
