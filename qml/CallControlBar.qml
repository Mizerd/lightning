import QtQuick
import QtQuick.Layouts
import MatrixClient

// The call control bar — a centred pill of circular controls, Discord's
// interaction shape rendered entirely in Lightning tokens.
//
// The governing rule: a control whose backend does not exist is NOT SHOWN.
// It is not shown disabled with a tooltip either, because a disabled
// AbstractButton receives no hover in Qt Quick and so cannot explain itself.
// Every control here reaches something real.
Item {
    id: root

    property int participantCount: 0
    /// Whether this host is responsible for mute / deafen / camera / hang up.
    ///
    /// False on the call STAGE, where CallHeaderBar already owns them at the
    /// top of the conversation. Without this the same four controls were
    /// drawn twice on one screen for one call.
    property bool showMediaControls: true
    property bool participantsOpen: false

    signal hangUpRequested
    signal participantsToggled
    signal layoutCycleRequested

    implicitWidth: bar.implicitWidth
    implicitHeight: bar.implicitHeight

    Rectangle {
        id: bar
        anchors.centerIn: parent
        implicitWidth: row.implicitWidth + AppTheme.spacing16 * 2
        implicitHeight: row.implicitHeight + AppTheme.spacing12 * 2
        radius: AppTheme.radiusPill
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder

        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: AppTheme.spacing8

            CallControlButton {
                objectName: "callMicButton"
                visible: root.showMediaControls
                // The icon states the CURRENT state, which is what a muted
                // user needs to see at a glance.
                iconName: app.groupCall.microphoneMuted ? "mic_off" : "mic"
                role: app.groupCall.microphoneMuted ? "active" : "neutral"
                tooltip: app.groupCall.microphoneMuted ? qsTr("Unmute microphone") : qsTr("Mute microphone")
                onClicked: app.groupCall.toggleMicrophoneMuted()
            }

            CallControlButton {
                objectName: "callDeafenButton"
                visible: root.showMediaControls
                iconName: app.groupCall.deafened ? "headset_off" : "headset_mic"
                role: app.groupCall.deafened ? "active" : "neutral"
                tooltip: app.groupCall.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                onClicked: app.groupCall.toggleDeafened()
            }

            CallControlButton {
                objectName: "callCameraButton"
                visible: root.showMediaControls
                iconName: app.groupCall.cameraOn ? "videocam" : "videocam_off"
                role: app.groupCall.cameraOn ? "active" : "neutral"
                tooltip: app.groupCall.cameraOn ? qsTr("Turn off camera") : qsTr("Turn on camera")
                onClicked: app.groupCall.toggleCamera()
            }

            CallControlButton {
                objectName: "callHandButton"
                iconName: "front_hand"
                role: app.groupCall.handRaised ? "active" : "neutral"
                tooltip: app.groupCall.handRaised ? qsTr("Lower hand") : qsTr("Raise hand")
                onClicked: app.groupCall.toggleHandRaised()
            }

            // Layout switching only means something above two people; below
            // that there is one sensible arrangement and a control that
            // cycles nothing is noise.
            Loader {
                active: root.participantCount > 2
                visible: active
                sourceComponent: CallControlButton {
                    objectName: "callLayoutButton"
                    iconName: "grid_view"
                    tooltip: qsTr("Change layout")
                    onClicked: root.layoutCycleRequested()
                }
            }

            CallControlButton {
                objectName: "callParticipantsButton"
                iconName: "group"
                role: root.participantsOpen ? "active" : "neutral"
                tooltip: qsTr("Participants")
                onClicked: root.participantsToggled()
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 24
                Layout.alignment: Qt.AlignVCenter
                color: AppTheme.stormBorder
            }

            CallControlButton {
                objectName: "callHangUpButton"
                visible: root.showMediaControls
                iconName: "call_end"
                role: "danger"
                // Wider than the round controls: leaving is the one
                // irreversible action here and must not be a same-shaped
                // neighbour of Mute.
                diameter: 44
                implicitWidth: 62
                tooltip: qsTr("Leave call")
                onClicked: root.hangUpRequested()
            }
        }
    }
}
