import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The in-call control bar, at the top of the conversation.
//
// Serves both call lanes: the legacy 1:1 lane (`app.calls`, audio only) and
// the MatrixRTC group lane (`app.groupCall`). At most one is live, and the
// properties below resolve to it so controls don't check twice.
//
// Toggle-plus-device controls use a split shape: the button toggles, the
// chevron opens the device menu.
Rectangle {
    id: root

    objectName: "callHeaderBar"
    /// The participant list was requested; the host decides where it opens.
    signal participantsRequested()

    /// Render as if a call were live, for the theme editor and screenshot
    /// harness. Controls still bind to the real controllers, so no call state
    /// is fabricated.
    property bool previewMode: false

    /// Placement: "header" (the strip under the room header; the legacy lane's
    /// only placement) or "dock" (a floating pill on the call stage). One
    /// definition of the control set, several placements.
    property string placement: "header"
    readonly property bool dock: root.placement === "dock"
    /// The collapsed call strip's form of the dock: smaller controls, no pill,
    /// only what fits on one line. Needed because the header instance stands
    /// down while the stage is on screen.
    property bool compact: false
    /// True when this room's call stage is on screen; it carries its own
    /// controls, so the header must not duplicate them.
    readonly property bool stageOwnsControls:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId
    readonly property int controlDiameter:
        root.dock ? (root.compact ? 32 : 48) : 40
    readonly property int controlGlyph:
        root.dock ? (root.compact ? 17 : 22) : 19

    // ── Which lane is live ──
    readonly property bool legacyLive:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active
    readonly property bool groupLive: app.groupCall.active
    readonly property bool live: previewMode || legacyLive || groupLive

    /// The live call's room. The bar only shows there; the Voice Connected
    /// strip follows the user elsewhere.
    readonly property string callRoomId: groupLive ? app.groupCall.roomId
                                                   : app.calls.activeRoomId

    readonly property bool micMuted: groupLive ? app.groupCall.microphoneMuted
                                               : app.calls.microphoneMuted
    readonly property bool deafened: groupLive ? app.groupCall.deafened
                                               : app.calls.deafened
    readonly property bool audioControlAvailable:
        previewMode || (groupLive ? true : app.calls.muteControlAvailable)
    /// Camera and screen share exist only on the SFU lane; the legacy lane is
    /// audio-only, so they are absent there rather than refusing.
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
    // One-line padding for the compact strip.
    implicitHeight: visible
                    ? bar.implicitHeight
                      + (root.compact ? AppTheme.spacing4 : AppTheme.spacing12) * 2
                    : 0
    height: implicitHeight
    // Needed: the collapsed strip and the full-screen window host this bar in
    // Loaders that adopt its implicit size. With a 0 implicit width the next
    // row item would be placed over the centred controls.
    implicitWidth: visible
                   ? bar.implicitWidth
                     + (root.compact ? 0 : AppTheme.spacing16 * 2)
                   : 0
    // The dock floats on the stage canvas; the pill is its surface.
    color: root.dock ? "transparent" : AppTheme.stormInset
    // A hairline beneath: the header bar continues the room chrome above it.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.stormBorder
        visible: root.visible && !root.dock
    }

    // The dock's pill, so the controls read as one object.
    Rectangle {
        anchors.centerIn: bar
        width: bar.implicitWidth + AppTheme.spacing16 * 2
        height: bar.implicitHeight + AppTheme.spacing8 * 2
        radius: height / 2
        // Not in the compact strip, which already sits on the panel's field.
        visible: root.dock && root.visible && !root.compact
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder
    }

    RowLayout {
        id: bar
        // Centred while it fits, but never past the parent's right edge.
        // A Layout lays out children by their own `visible`, not their
        // ancestors', so with no live call this row is still 199 px wide inside
        // a zero-width cell, and centerIn would hang it off both sides. When
        // the content doesn't fit it spills left, so the camera button leaves
        // the panel first, never Leave (see CallStage.qml).
        anchors.verticalCenter: parent.verticalCenter
        x: Math.min(Math.round((parent.width - width) / 2),
                    parent.width - width)
        spacing: AppTheme.spacing8

        // State, leading so the controls stay optically centred. Not in the
        // dock: the stage header already names the call.
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
                    visible: !root.compact
                    // Zero width when hidden, or the RowLayout keeps its slot.
                    Layout.preferredWidth: visible ? implicitWidth : 0
                    kind: "camera"
                    accessibleName: qsTr("Choose camera")
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }
            }
        }

        // ── Screen share + options (SFU lane only) ──
        // One inner RowLayout with spacing 0, like the camera and mic pairs.
        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: RowLayout {
                spacing: 0
                CallControlButton {
                    objectName: "callBarScreenShareButton"
                    iconName: app.groupCall.screenSharing
                              ? "stop_screen_share" : "screen_share"
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
                // Share options (sound, resolution, frame rate) on a chevron,
                // the same split shape as mic and camera. Also needed because
                // the portal draws the share picker on Wayland, so options
                // there would be invisible.
                AbstractButton {
                    id: shareChevron
                    objectName: "callBarShareOptionsChevron"
                    visible: !root.compact
                    // Zero width when hidden, or the RowLayout keeps its slot.
                    Layout.preferredWidth: visible ? implicitWidth : 0
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                    implicitWidth: 20
                    implicitHeight: 26
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.ButtonMenu
                    Accessible.name: qsTr("Screen share options")
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: shareChevron.pressed
                               ? AppTheme.selectedHover
                               : (shareChevron.hovered
                                  || shareChevron.activeFocus
                                  ? AppTheme.hover
                                  : AppTheme.surfaceElevated)
                        border.width: shareChevron.activeFocus ? 2 : 1
                        border.color: shareChevron.activeFocus
                                      ? AppTheme.focusRing
                                      : AppTheme.borderSubtle
                        Behavior on color { ColorAnimation { duration: 90 } }
                    }
                    contentItem: Icon {
                        name: "expand_more"
                        size: 14
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: shareChevron.hovered
                                     || shareChevron.activeFocus
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Sound, resolution and frame rate")
                    onClicked: shareOptions.popup()
                    CallShareOptionsMenu { id: shareOptions }
                }
            }
        }

        // ── Raise hand (SFU lane only) ──
        // element-call's format: an m.reaction (U+1F590 U+FE0F) annotating the
        // sender's own membership state event, lowered by redaction.
        Loader {
            active: root.richMedia && !root.compact
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarHandButton"
                // front_hand: the icon map carries it, and IconChromeTest
                // refuses names it can't draw.
                iconName: "front_hand"
                role: app.groupCall.handRaised ? "active" : "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                // The hand is on the wire in element-call's format, so no
                // "only on this device" disclaimer.
                tooltip: app.groupCall.handRaised ? qsTr("Lower your hand")
                                                  : qsTr("Raise your hand")
                onClicked: app.groupCall.toggleHandRaised()
            }
        }

        // ── React (SFU lane only) ── element-call sends a transient
        // `io.element.call.reaction` referencing the sender's membership, read
        // by the same ReactionsReader as a raised hand. The set is the first
        // row (5) of element-call's ReactionSet (src/reactions/index.ts); each
        // `name` is what Element clients look the sound up by, so the pairs
        // must match. rust/src/rtc.rs holds the same table and refuses pairs
        // not in it.
        Loader {
            active: root.richMedia && !root.compact
            visible: active
            sourceComponent: CallControlButton {
                id: reactButton
                objectName: "callBarReactButton"
                iconName: "add_reaction"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: qsTr("Send a reaction")
                onClicked: reactionPopup.opened ? reactionPopup.close()
                                                : reactionPopup.open()

                Popup {
                    id: reactionPopup
                    objectName: "callBarReactionPopup"
                    // Above the bar in the dock (at the bottom of the stage),
                    // below it in the header.
                    x: (reactButton.width - width) / 2
                    y: root.dock ? -height - 8 : reactButton.height + 8
                    padding: 6
                    // Modal without dim, so handlers beneath the popup stay
                    // inactive.
                    modal: true
                    // Required for CloseOnEscape: Qt routes Escape only to a
                    // popup with active focus.
                    focus: true
                    dim: false
                    closePolicy: Popup.CloseOnEscape
                                 | Popup.CloseOnPressOutside
                    background: Rectangle {
                        radius: AppTheme.radiusMd
                        color: AppTheme.surfaceElevated
                        border.width: 1
                        border.color: AppTheme.borderSubtle
                    }

                    contentItem: Row {
                        spacing: 2
                        Repeater {
                            // element-call's ReactionSet, first row. Literal
                            // emoji: a contract test compares these bytes with
                            // rust/src/rtc.rs, and a mistyped escape would
                            // break loading.
                            model: [
                                { emoji: "👍", name: "thumbsup",
                                  label: qsTr("Thumbs up") },
                                { emoji: "🎉", name: "party",
                                  label: qsTr("Party") },
                                { emoji: "👏", name: "clapping",
                                  label: qsTr("Applause") },
                                { emoji: "🐶", name: "dog",
                                  label: qsTr("Dog") },
                                { emoji: "🐱", name: "cat",
                                  label: qsTr("Cat") }
                            ]
                            delegate: AbstractButton {
                                id: reactionChoice
                                required property var modelData
                                objectName: "callReactionChoice"
                                implicitWidth: 36
                                implicitHeight: 36
                                hoverEnabled: true
                                focusPolicy: Qt.StrongFocus
                                Accessible.role: Accessible.Button
                                Accessible.name: reactionChoice.modelData.label
                                background: Rectangle {
                                    radius: AppTheme.radiusSm
                                    color: reactionChoice.pressed
                                           ? AppTheme.selectedHover
                                           : (reactionChoice.hovered
                                              || reactionChoice.activeFocus
                                              ? AppTheme.hover
                                              : "transparent")
                                    border.width: reactionChoice.activeFocus
                                                  ? 2 : 0
                                    border.color: AppTheme.focusRing
                                }
                                contentItem: Text {
                                    // Emoji text. The family is resolved in C++
                                    // (QML has no `font.families`; Qt's
                                    // fallback may pick a monochrome face).
                                    textFormat: Text.PlainText
                                    text: reactionChoice.modelData.emoji
                                    font.pixelSize: 20
                                    font.family:
                                        (typeof app !== "undefined" && app
                                         && app.emojiFontFamily) || ""
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                ToolTip.visible: reactionChoice.hovered
                                ToolTip.delay: 400
                                ToolTip.text: reactionChoice.modelData.label
                                onClicked: {
                                    app.groupCall.sendCallReaction(
                                        reactionChoice.modelData.emoji,
                                        reactionChoice.modelData.name)
                                    reactionPopup.close()
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Participants (SFU lane only) ──
        Loader {
            active: root.richMedia && !root.compact
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarParticipantsButton"
                iconName: "group"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                // The count goes in the tooltip; CallControlButton has no
                // badge.
                tooltip: app.groupCall.participantCount > 0
                         ? qsTr("Show who's in the call (%1)")
                           .arg(app.groupCall.participantCount)
                         : qsTr("Show who's in the call")
                onClicked: root.participantsRequested()
            }
        }

        // ── Pop out (picture-in-picture) ── Manual entry to the floating
        // window Main.qml opens on minimise; Qt can't tell an unminimised
        // window behind another app from one in front.
        Loader {
            active: root.live && !root.compact
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarPipButton"
                // close_fullscreen: the icon font is a subset (see qml/Icon.qml
                // and scripts/generate-icon-font.sh), and a missing name
                // renders as tofu in packaged builds.
                iconName: "close_fullscreen"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: qsTr("Pop the call out into a floating window")
                onClicked: {
                    if (app.groupCall && app.groupCall.stageState)
                        app.groupCall.stageState.setPictureInPicture(true)
                }
            }
        }

        // ── Microphone + device chooser ──
        RowLayout {
            spacing: 0
            CallControlButton {
                objectName: "callBarMicButton"
                // The icon shows the current state (struck-through = muted).
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
                visible: !root.compact
                Layout.preferredWidth: visible ? implicitWidth : 0
                kind: "microphone"
                accessibleName: qsTr("Choose microphone")
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
                // Flags a missing chosen device here as well as in Settings.
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
                visible: !root.compact
                Layout.preferredWidth: visible ? implicitWidth : 0
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
            // Wider than the round controls: the one irreversible action
            // shouldn't look like Mute.
            implicitWidth: root.dock ? (root.compact ? 46 : 70) : 58
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
