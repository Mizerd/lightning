import QtQuick
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    objectName: "voiceConnectedBar"
    signal returnToCallRequested

    visible: app.groupCall.active && !(app.currentScreen === 1 && app.groupCall.roomId === app.currentRoomId)
    implicitHeight: visible ? content.implicitHeight + AppTheme.spacing8 * 2 : 0
    height: implicitHeight
    color: AppTheme.stormInset
    border.width: 1
    border.color: AppTheme.stormBorder
    radius: AppTheme.radiusMd

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: AppTheme.spacing8
        spacing: AppTheme.spacing8

        ColumnLayout {
            Layout.fillWidth: true
            // Shrinkable: a non-fill Text keeps its width in a Layout and would
            // push the hang-up button out of the bar. The text elides; the
            // buttons never move.
            Layout.minimumWidth: 0
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 4
                Icon {
                    name: "call"
                    size: 14
                    color: app.groupCall.state === SfuCallController.Connected ? AppTheme.success : AppTheme.warning
                }
                Text {
                    text: app.groupCall.state === SfuCallController.Reconnecting
                          ? qsTr("Reconnecting…")
                          : app.groupCall.state === SfuCallController.Connected
                            ? qsTr("Voice connected") : qsTr("Connecting…")
                    color: app.groupCall.state === SfuCallController.Connected ? AppTheme.success : AppTheme.warning
                    font.pixelSize: AppTheme.scaled(11)
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: {
                    if (!app.roomList || app.groupCall.roomId.length === 0)
                        return "";
                    var room = app.roomList.findRoom(app.groupCall.roomId);
                    return room && room.name ? room.name : "";
                }
                visible: text.length > 0
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.scaled(11)
                elide: Text.ElideRight
            }
        }

        CallControlButton {
            objectName: "voiceBarMuteButton"
            iconName: app.groupCall.microphoneMuted ? "mic_off" : "mic"
            role: app.groupCall.microphoneMuted ? "active" : "neutral"
            diameter: 28
            glyphSize: 15
            tooltip: app.groupCall.microphoneMuted ? qsTr("Unmute microphone") : qsTr("Mute microphone")
            onClicked: app.groupCall.toggleMicrophoneMuted()
        }
        CallControlButton {
            objectName: "voiceBarDeafenButton"
            iconName: app.groupCall.deafened ? "headset_off" : "headset_mic"
            role: app.groupCall.deafened ? "active" : "neutral"
            diameter: 28
            glyphSize: 15
            tooltip: app.groupCall.deafened ? qsTr("Undeafen") : qsTr("Deafen")
            onClicked: app.groupCall.toggleDeafened()
        }
        CallControlButton {
            objectName: "voiceBarReturnButton"
            iconName: "open_in_full"
            diameter: 28
            glyphSize: 15
            tooltip: qsTr("Return to call")
            onClicked: root.returnToCallRequested()
        }
        CallControlButton {
            objectName: "voiceBarLeaveButton"
            iconName: "call_end"
            role: "danger"
            diameter: 28
            glyphSize: 15
            tooltip: qsTr("Leave call")
            onClicked: app.groupCall.leave()
        }
    }
}
