import QtQuick
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// One participant on the call stage — Discord-style layout, Lightning tokens.
//
// A voice tile is an avatar on a calm surface with a speaking ring; a name
// strip sits along the bottom and state badges ride the top-right corner.
// Nothing here is Discord artwork or Discord colour: every value comes from
// AppTheme, so the tile follows all eleven themes and the text scale.
//
// HONESTY RULE, and the whole reason `micKnown`/`cameraKnown` exist: the SFU
// reports a track's muted state only for tracks it knows about. Before a
// participant publishes, or for a device that never will, the state is
// genuinely UNKNOWN — and a boolean cannot say that. So a badge renders only
// when something authoritative said so, and unknown renders NOTHING rather
// than a confident, wrong "not muted".
//
// Delegate discipline: this is instantiated per participant, so every Label
// whose text can legitimately be empty lives behind a Loader. A never
// laid-out empty Text keeps ItemObservesViewport forever and makes Qt walk
// the whole instantiated tree on every scroll frame — the single most
// expensive QML mistake recorded in this repo.
Item {
    id: root

    property string userId: ""
    property string displayName: ""
    property string avatarMxc: ""

    property bool micKnown: false
    property bool micMuted: false
    property bool cameraKnown: false
    property bool cameraOn: false
    /// The SFU participant identity this tile shows. Routes video, and it
    /// is the only identifier that works for BOTH membership formats — the
    /// sticky form's identity is a hash, so it cannot be rebuilt from a user
    /// and device id.
    property string identity: ""
    property bool screenSharing: false
    property bool handRaised: false

    /// Voice-activity ring, driven by the SFU's speaker updates.
    property bool speaking: false
    /// This participant is the local device.
    property bool local: false
    /// Manually spotlighted.
    property bool focused: false
    /// Compact form for the strip beside a screen share.
    property bool compact: false

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    readonly property int _avatarSize: {
        // Fit the avatar to the tile rather than to a fixed ladder, so the
        // grid stays sane from a 2-up 1:1 layout to a 12-up group.
        var box = Math.min(width, height - (compact ? 18 : 26))
        var size = Math.round(box * 0.52)
        return Math.max(compact ? 28 : 40, Math.min(size, 96))
    }

    // "You" for the local device, but the AVATAR and colour key still come
    // from the real account — reported as: "I came in as You, should show my
    // avatar and display name". The label says who the tile is; it is not a
    // reason to draw a blank circle.
    readonly property string _label: root.local
                                     ? qsTr("You")
                                     : (root.displayName.length > 0
                                        ? root.displayName : root.userId)
    /// What the avatar draws initials from when there is no image: the real
    /// name, never the word "You" (which would render "Y" for everyone).
    readonly property string _avatarName: root.displayName.length > 0
                                          ? root.displayName
                                          : root.userId

    Accessible.role: Accessible.Button
    Accessible.name: root._label.length > 0 ? root._label : qsTr("Participant")
    // Carries the same facts the badges show, so a screen-reader user learns
    // what a sighted one does — and is told nothing when the state is
    // unknown.
    Accessible.description: {
        var parts = []
        if (root.micKnown && root.micMuted)
            parts.push(qsTr("Microphone muted"))
        if (root.screenSharing)
            parts.push(qsTr("Sharing their screen"))
        if (root.handRaised)
            parts.push(qsTr("Hand raised"))
        if (root.speaking)
            parts.push(qsTr("Speaking"))
        return parts.join(", ")
    }
    Accessible.focusable: true
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusTile
        color: root.focused ? AppTheme.selected : AppTheme.cardElevated
        border.width: root.focused || root.activeFocus ? 2 : 1
        border.color: root.activeFocus
                      ? AppTheme.focusRing
                      : (root.focused ? AppTheme.accentBorder : AppTheme.borderSubtle)

        // Live video, when there is any.
        //
        // Behind a Loader so a voice-only tile builds no VideoOutput at all
        // — this is a per-participant delegate, and a grid of idle video
        // surfaces costs real GPU memory for nothing.
        //
        // `cameraOn` is only ever true when something authoritative said so
        // (see the honesty rule above), so an unknown camera shows the
        // avatar rather than a black rectangle.
        Loader {
            id: videoLoader
            anchors.fill: parent
            // A screen share is video as much as a camera is. Gating on
            // `cameraOn` alone meant a shared screen never rendered at all —
            // reported as "I did not see their screenshare".
            //
            // `local` is excluded because the engine publishes our own media
            // rather than receiving it: there is no remote stream for this
            // device, so a self-view would be a permanently black rectangle.
            active: root.identity.length > 0 && !root.local
                    && ((root.cameraKnown && root.cameraOn)
                        || root.screenSharing)
            visible: active && item && item.hasFrame
            sourceComponent: Item {
                /// Nothing has arrived yet: the tile keeps showing the
                /// avatar instead of a black hole while the first frame is
                /// in flight.
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectCrop
                }

                // Attach on creation, DETACH on destruction. The router
                // holds a QPointer so a missed detach cannot crash, but it
                // would keep routing frames at a dead tile for one frame and
                // keep the entry alive until then.
                Component.onCompleted: app.groupCall.attachVideoSink(
                                           root.identity, output.videoSink)
                Component.onDestruction: app.groupCall.detachVideoSink(
                                             root.identity)
            }
        }

        // Speaking ring around the AVATAR, not a moving avatar: Discord's
        // cue reads as a halo, and shifting the avatar on every syllable is
        // what makes a grid feel unstable.
        Item {
            id: avatarBlock
            // Hidden, not destroyed, while video is live: the camera can go
            // off at any moment and rebuilding the avatar block then would
            // flash an empty tile.
            visible: !videoLoader.visible
            anchors.centerIn: parent
            anchors.verticalCenterOffset: root.compact ? -6 : -8
            width: root._avatarSize
            height: root._avatarSize

            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 12
                height: parent.height + 12
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: AppTheme.success
                opacity: root.speaking ? 1 : 0
                scale: root.speaking ? 1 : 0.94
                visible: opacity > 0
                Behavior on opacity {
                    NumberAnimation { duration: 110 }
                }
                Behavior on scale {
                    NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
                }
            }

            Avatar {
                anchors.fill: parent
                mxc: root.avatarMxc
                // The real name, not the "You" label: initials of "You"
                // would be a Y on the local tile and nothing recognisable.
                name: root._avatarName
                colorKey: root.userId
                size: root._avatarSize
            }
        }

        // Name strip. Behind a Loader: the label is empty until a profile
        // resolves, which is the state it is created in.
        Loader {
            active: root._label.length > 0
            visible: active
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 10
            sourceComponent: RowLayout {
                spacing: 4
                Loader {
                    active: root.micKnown && root.micMuted
                    visible: active
                    Layout.alignment: Qt.AlignVCenter
                    sourceComponent: Icon {
                        name: "mic_off"
                        size: root.compact ? 12 : 14
                        color: AppTheme.dangerInk
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root._label
                    color: AppTheme.textPrimary
                    font.pixelSize: root.compact ? 11 : 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }

        // State badges, top-right. Each in its own Loader so an inactive
        // badge costs nothing and contributes no empty Text.
        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: root.compact ? 6 : 8
            spacing: 4

            Loader {
                active: root.handRaised
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "front_hand"
                    tone: "accent"
                }
            }
            Loader {
                active: root.screenSharing
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "screen_share"
                    tone: "accent"
                }
            }
            Loader {
                // Only an authoritative "camera is off" earns a badge.
                active: root.cameraKnown && !root.cameraOn
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "videocam_off"
                    tone: "muted"
                }
            }
        }

        TapHandler {
            // Left button only: TapHandlers are non-exclusive across
            // subtrees, so grabbing every button here would also swallow
            // presses meant for the stage beneath.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }
    }
}
