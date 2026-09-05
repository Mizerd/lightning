import QtQuick
import QtQuick.Layouts
import MatrixClient

// The persistent "Voice Connected" strip — Discord's idea, Lightning's
// tokens. Lives in the navigation column so the user can browse other rooms
// while staying in the call: the call does NOT end because they opened
// another room, and this is how they get back to it.
Rectangle {
    id: root

    objectName: "voiceConnectedBar"
    /// Emitted when the user asks to return to the call surface.
    signal returnToCallRequested

    // Only while the call is somewhere ELSE. This bar exists so a call
    // survives browsing away from its room; inside that room the call
    // controls are already at the top of the conversation, and showing both
    // put three copies of the same call on screen at once.
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
            // SHRINKABLE, or the buttons cannot fit. A non-fill Text is fixed
            // at its own width inside a Layout, so at the column's narrow
            // floor the "Voice connected" line kept its width and pushed the
            // hang-up button out through the bar's edge (2026-09-05
            // screenshot). The text yields and elides; the buttons never move.
            Layout.minimumWidth: 0
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 4
                Icon {
                    name: "call"
                    size: 14
                    // Green while connected, warning while reconnecting —
                    // the state is shown, never left as a frozen picture.
                    color: app.groupCall.state === SfuCallController.Connected ? AppTheme.success : AppTheme.warning
                }
                Text {
                    text: app.groupCall.state === SfuCallController.Connected ? qsTr("Voice connected") : qsTr("Connecting…")
                    color: app.groupCall.state === SfuCallController.Connected ? AppTheme.success : AppTheme.warning
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                }
            }
            Text {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: {
                    // findRoom is a plain C++ call Qt cannot observe, so
                    // this re-reads whenever the call's room changes —
                    // which is the only thing that can change it here.
                    if (!app.roomList || app.groupCall.roomId.length === 0)
                        return "";
                    var room = app.roomList.findRoom(app.groupCall.roomId);
                    return room && room.name ? room.name : "";
                }
                visible: text.length > 0
                color: AppTheme.stormTextSecondary
                font.pixelSize: 11
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
