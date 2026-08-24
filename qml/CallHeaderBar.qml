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
    /// The participant list was asked for. The host decides where it opens.
    signal participantsRequested()

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

    /// Where this bar is being shown: "header" (the strip under the room
    /// header, which is also the only placement the legacy 1:1 lane has) or
    /// "dock" (a floating pill at the bottom of the call stage, which is
    /// where a call client's controls belong and what was asked for).
    ///
    /// ONE definition of the control set, two placements. A second component
    /// for the dock would be two control bars to keep in step, and the last
    /// time this surface was split the result was two orphan buttons under
    /// the call UI.
    property string placement: "header"
    readonly property bool dock: root.placement === "dock"
    /// True when the call STAGE for this room is on screen. The stage carries
    /// its own dock, so the header must not draw a second copy of the same
    /// controls directly above it.
    readonly property bool stageOwnsControls:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId
    readonly property int controlDiameter: root.dock ? 48 : 40
    readonly property int controlGlyph: root.dock ? 22 : 19

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

    visible: previewMode
             || (live && callRoomId === app.currentRoomId
                 && (root.dock || !root.stageOwnsControls))
    implicitHeight: visible ? bar.implicitHeight + AppTheme.spacing12 * 2 : 0
    height: implicitHeight
    // The dock floats over the stage's canvas, so it paints no field of its
    // own — the pill behind the controls is the surface.
    color: root.dock ? "transparent" : AppTheme.stormInset
    // A hairline underneath rather than a floating card: the header bar is
    // part of the room's chrome, continuous with the header above it. The
    // dock has no edge to continue from.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.stormBorder
        visible: root.visible && !root.dock
    }

    // The dock's own surface: one rounded pill under the controls, which is
    // what makes a floating control row read as a single object rather than
    // a scatter of circles on the canvas.
    Rectangle {
        anchors.centerIn: bar
        width: bar.implicitWidth + AppTheme.spacing16 * 2
        height: bar.implicitHeight + AppTheme.spacing8 * 2
        radius: height / 2
        visible: root.dock && root.visible
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder
    }

    RowLayout {
        id: bar
        anchors.centerIn: parent
        spacing: AppTheme.spacing8

        // State, on the leading side so the controls stay optically centred.
        // Not in the dock: the stage's own header already names the call and
        // its state, and repeating it inside the control pill is noise.
        RowLayout {
            spacing: 6
            visible: !root.dock
            Layout.rightMargin: root.dock ? 0 : AppTheme.spacing8
            Layout.preferredWidth: visible ? implicitWidth : 0
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
                    diameter: root.controlDiameter
                    glyphSize: root.controlGlyph
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
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
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

        // ── Raise hand (SFU lane only) ──
        //
        // Here rather than on the call stage. The stage used to carry its own
        // control bar for raise-hand and the participant list, and once the
        // media controls moved up here that left two orphan buttons floating
        // under the call UI — reported exactly that way. One control surface,
        // at the top, which is what was asked for.
        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarHandButton"
                // front_hand, not back_hand: the icon map carries the
                // former and IconChromeTest refuses a name it cannot draw —
                // an unmapped glyph renders as tofu.
                iconName: "front_hand"
                role: app.groupCall.handRaised ? "active" : "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: app.groupCall.handRaised ? qsTr("Lower your hand")
                                                  : qsTr("Raise your hand")
                onClicked: app.groupCall.toggleHandRaised()
            }
        }

        // ── Participants (SFU lane only) ──
        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarParticipantsButton"
                iconName: "group"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                // The count goes in the TOOLTIP: CallControlButton has no
                // badge, and inventing one here would be a second styling
                // path for the same control.
                tooltip: app.groupCall.participantCount > 0
                         ? qsTr("Show who's in the call (%1)")
                           .arg(app.groupCall.participantCount)
                         : qsTr("Show who's in the call")
                onClicked: root.participantsRequested()
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
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
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
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
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
            diameter: root.controlDiameter
            // Wider than the round controls: leaving is the one irreversible
            // action on this bar and must not be a same-shaped neighbour of
            // Mute.
            implicitWidth: root.dock ? 70 : 58
            glyphSize: root.controlGlyph
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
