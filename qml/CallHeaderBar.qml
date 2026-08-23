import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The in-call control bar, at the TOP of the conversation.
//
// Replaces the corner card as the place a live call is driven from: a call is
// the thing the user is doing in this room, so its controls belong with the
// room, directly under its header — not floating over a corner where they
// compete with passive prompts for the same space.
//
// Serves BOTH call lanes, because both are real:
//   * the legacy 1:1 lane (`app.calls`), which carries audio today
//   * the MatrixRTC group lane (`app.groupCall`)
// Whichever is live owns the bar. They cannot both be, and the properties
// below resolve to the active one rather than each control checking twice.
//
// Controls that pair a toggle with a device chooser use a split shape: the
// button toggles, the chevron opens the device menu. That is the arrangement
// the maintainer asked for, and it keeps "mute" one click away while making
// "which microphone" reachable without a trip to Settings.
Rectangle {
    id: root

    objectName: "callHeaderBar"

    /// Render as if a call were live, without one.
    ///
    /// A real seam, not test scaffolding: the theme editor has to be able to
    /// show this surface so a theme can be designed against it, and a
    /// screenshot harness needs the same thing. Both want the bar's
    /// APPEARANCE with no session behind it.
    ///
    /// Controls still bind to the real controllers, so nothing here can
    /// fabricate call STATE — a preview shows an idle-but-visible bar.
    property bool previewMode: false

    // ── Which lane is live ──
    readonly property bool legacyLive:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active
    readonly property bool groupLive: app.groupCall.active
    readonly property bool live: previewMode || legacyLive || groupLive

    /// The room the live call belongs to. The bar only shows in that room —
    /// the persistent Voice Connected strip is what follows the user
    /// elsewhere.
    readonly property string callRoomId: groupLive ? app.groupCall.roomId
                                                   : app.calls.activeRoomId

    readonly property bool micMuted: groupLive ? app.groupCall.microphoneMuted
                                               : app.calls.microphoneMuted
    readonly property bool deafened: groupLive ? app.groupCall.deafened
                                               : app.calls.deafened
    readonly property bool audioControlAvailable:
        previewMode || (groupLive ? true : app.calls.muteControlAvailable)
    /// Camera and screen share exist only on the SFU lane: the legacy 1:1
    /// lane is audio-only by design, so those controls are absent there
    /// rather than present and refusing.
    readonly property bool richMedia: previewMode || groupLive

    readonly property string stateText: {
        if (root.groupLive) {
            switch (app.groupCall.state) {
            case SfuCallController.Preparing:
            case SfuCallController.Authorizing:
            case SfuCallController.Connecting:
                return qsTr("Connecting…")
            case SfuCallController.Reconnecting:
                return qsTr("Reconnecting…")
            default:
                return qsTr("Voice call")
            }
        }
        if (app.calls.state === CallController.Inviting)
            return qsTr("Calling…")
        if (app.calls.state === CallController.Connecting)
            return qsTr("Connecting…")
        return qsTr("Voice call")
    }

    visible: previewMode || (live && callRoomId === app.currentRoomId)
    implicitHeight: visible ? bar.implicitHeight + AppTheme.spacing12 * 2 : 0
    height: implicitHeight
    color: AppTheme.stormInset
    // A hairline underneath rather than a floating card: the bar is part of
    // the room's chrome, continuous with the header above it.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.stormBorder
        visible: root.visible
    }

    RowLayout {
        id: bar
        anchors.centerIn: parent
        spacing: AppTheme.spacing8

        // State, on the leading side so the controls stay optically centred.
        RowLayout {
            spacing: 6
            Layout.rightMargin: AppTheme.spacing8
            Icon {
                name: "call"
                size: 16
                color: root.groupLive
                       && app.groupCall.state === SfuCallController.Reconnecting
                       ? AppTheme.warning : AppTheme.accent
            }
            Text {
                text: root.stateText
                color: AppTheme.stormText
                font.pixelSize: 13
                font.weight: Font.Medium
            }
        }

        // ── Camera + device chooser (SFU lane only) ──
        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: RowLayout {
                spacing: 0
                CallControlButton {
                    objectName: "callBarCameraButton"
                    iconName: app.groupCall.cameraOn ? "videocam"
                                                     : "videocam_off"
                    role: app.groupCall.cameraOn ? "active" : "neutral"
                    diameter: 40
                    glyphSize: 19
                    tooltip: app.groupCall.cameraOn ? qsTr("Turn off camera")
                                                    : qsTr("Turn on camera")
                    onClicked: app.groupCall.toggleCamera()
                }
                CallDeviceChevron {
                    objectName: "callBarCameraChevron"
                    kind: "camera"
                    accessibleName: qsTr("Choose camera")
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }
            }
        }

        // ── Screen share (SFU lane only) ──
        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarScreenShareButton"
                iconName: app.groupCall.screenSharing ? "stop_screen_share"
                                                      : "screen_share"
                role: app.groupCall.screenSharing ? "active" : "neutral"
                diameter: 40
                glyphSize: 19
                tooltip: app.groupCall.screenSharing
                         ? qsTr("Stop sharing your screen")
                         : qsTr("Share your screen")
                onClicked: {
                    if (app.groupCall.screenSharing)
                        app.groupCall.stopScreenShare()
                    else
                        app.groupCall.requestScreenShare()
                }
            }
        }

        // ── Microphone + device chooser ──
        RowLayout {
            spacing: 0
            CallControlButton {
                objectName: "callBarMicButton"
                // The icon states the CURRENT state: a struck-through mic
                // means "you are muted", as in every other call client.
                iconName: root.micMuted ? "mic_off" : "mic"
                role: root.micMuted ? "active" : "neutral"
                diameter: 40
                glyphSize: 19
                tooltip: root.micMuted ? qsTr("Unmute microphone")
                                       : qsTr("Mute microphone")
                enabled: root.audioControlAvailable
                onClicked: {
                    if (root.groupLive)
                        app.groupCall.toggleMicrophoneMuted()
                    else
                        app.calls.toggleMicrophoneMuted()
                }
            }
            CallDeviceChevron {
                objectName: "callBarMicChevron"
                kind: "microphone"
                accessibleName: qsTr("Choose microphone")
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
                // Marked when the chosen device is gone, so the reason audio
                // is coming from somewhere unexpected is visible here rather
                // than only in Settings.
                warn: app.callDevices.preferredMicrophoneMissing
            }
        }

        // ── Deafen + output chooser ──
        RowLayout {
            spacing: 0
            CallControlButton {
                objectName: "callBarDeafenButton"
                iconName: root.deafened ? "headset_off" : "headset_mic"
                role: root.deafened ? "active" : "neutral"
                diameter: 40
                glyphSize: 19
                tooltip: root.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                enabled: root.audioControlAvailable
                onClicked: {
                    if (root.groupLive)
                        app.groupCall.toggleDeafened()
                    else
                        app.calls.toggleDeafened()
                }
            }
            CallDeviceChevron {
                objectName: "callBarSpeakerChevron"
                kind: "speaker"
                accessibleName: qsTr("Choose output device")
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            color: AppTheme.stormBorder
        }

        // ── Leave, deliberately distinct ──
        CallControlButton {
            objectName: "callBarHangUpButton"
            iconName: "call_end"
            role: "danger"
            diameter: 40
            // Wider than the round controls: leaving is the one irreversible
            // action on this bar and must not be a same-shaped neighbour of
            // Mute.
            implicitWidth: 58
            glyphSize: 19
            tooltip: qsTr("Leave call")
            onClicked: {
                if (root.groupLive)
                    app.groupCall.leave()
                else
                    app.calls.hangup()
            }
        }
    }
}
