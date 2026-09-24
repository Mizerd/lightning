import QtQuick
import QtQuick.Controls
import MatrixClient

// Participant bubbles for a collapsed call (and beside a spotlight): one
// circular avatar per person with a ring that lights while they speak. Not
// shown above the expanded stage, which already draws everyone.
//
// The ring is driven by the SFU's speaker updates (LiveKit's server-side
// SpeakersChanged), not local measurement, so it works for unsubscribed
// remote audio and inspects no audio here. It stays binary: at 34 px an
// amplitude ring is a sub-pixel wobble.
//
// Per-participant delegate: a Label whose text can be empty lives behind a
// Loader (an empty Text keeps ItemObservesViewport).
Item {
    id: root

    /// The participant model, bound directly (a copied array would reset the
    /// model and rebuild every bubble on each speaker update). Overridable for
    /// test fixtures.
    property var model: app.groupCall.participantModel

    /// The identity a click should spotlight, reported back to the owner.
    signal activated(string identity)

    readonly property int bubbleSize: 34
    readonly property int _count: root.model ? root.model.count : 0

    // Zero implicit height with nobody present, so a connecting call reserves
    // no band. Implicit rather than an explicit `height:` because the host
    // RowLayout writes width/height onto children, destroying such bindings.
    implicitHeight: root._count > 0 ? root.bubbleSize + 10 : 0
    // An implicit width too, for hosts that don't fill (beside a spotlight),
    // or the strip would get width 0.
    /// One bubble's cell, wider than the avatar so the centred speaking ring
    /// (`bubbleSize + 8`) isn't clipped. Shared by the delegate and
    /// implicitWidth so the two can't disagree.
    readonly property int cellWidth: root.bubbleSize + 8

    // Content width, capped so a large call doesn't push the title and controls
    // out of the header; past the cap the strip scrolls. N cells plus N-1 gaps
    // (no trailing gap).
    implicitWidth: root._count > 0
        ? Math.min(root._count * root.cellWidth
                   + (root._count - 1) * AppTheme.spacing6,
                   root.maxImplicitWidth)
        : 0
    /// Width at which the strip starts scrolling. Filling hosts ignore it.
    property int maxImplicitWidth: 220
    visible: root._count > 0

    ListView {
        id: strip
        objectName: "callSpeakerBubbles"
        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: AppTheme.spacing6
        clip: true
        // More people than fit scroll; an "+N" badge would be a second count
        // that could disagree with the header's.
        model: root.model
        boundsBehavior: Flickable.StopAtBounds

        delegate: Item {
            id: bubble
            required property string identity
            required property string userId
            required property string displayName
            required property string avatarMxc
            required property bool local
            required property bool speaking
            required property bool micKnown
            required property bool micMuted
            required property bool screenSharing

            // Wide enough for the speaking ring; `root.cellWidth` so it matches
            // the strip's implicitWidth.
            width: root.cellWidth
            height: strip.height

            readonly property bool muted: bubble.micKnown && bubble.micMuted
            readonly property string personName: bubble.local
                ? qsTr("You")
                : (bubble.displayName || bubble.userId || "")

            Accessible.role: Accessible.Button
            Accessible.name: bubble.personName
            Accessible.description: {
                var parts = [];
                if (bubble.speaking)
                    parts.push(qsTr("Speaking"));
                if (bubble.muted)
                    parts.push(qsTr("Microphone muted"));
                if (bubble.screenSharing)
                    parts.push(qsTr("Sharing their screen"));
                return parts.join(", ");
            }

            // A ring rather than a growing avatar, so the row never reflows
            // while people talk.
            Rectangle {
                anchors.centerIn: avatarHolder
                width: root.bubbleSize + 8
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: AppTheme.success
                opacity: bubble.speaking ? 1 : 0
                visible: opacity > 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 110
                    }
                }
            }

            Item {
                id: avatarHolder
                anchors.centerIn: parent
                width: root.bubbleSize
                height: root.bubbleSize

                Avatar {
                    anchors.fill: parent
                    mxc: bubble.avatarMxc
                    // The real name, not "You" (which would render a Y).
                    name: bubble.displayName || bubble.userId || ""
                    colorKey: bubble.userId
                    size: root.bubbleSize
                }

                // Muted badge, bottom-right, only when the SFU said so; unknown
                // renders nothing.
                Loader {
                    active: bubble.muted
                    visible: active
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    sourceComponent: Rectangle {
                        width: 14
                        height: 14
                        radius: 7
                        color: AppTheme.stormCanvas
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        Icon {
                            anchors.centerIn: parent
                            name: "mic_off"
                            size: 10
                            color: AppTheme.danger
                        }
                    }
                }

                // Screen-share badge, top-right. Not hover-gated: in a
                // collapsed call it's the only sign a share is running.
                Loader {
                    active: bubble.screenSharing
                    visible: active
                    anchors.right: parent.right
                    anchors.top: parent.top
                    sourceComponent: Rectangle {
                        width: 14
                        height: 14
                        radius: 7
                        color: AppTheme.stormCanvas
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        Icon {
                            anchors.centerIn: parent
                            name: "screen_share"
                            size: 10
                            color: AppTheme.accent
                        }
                    }
                }
            }

            HoverHandler {
                id: bubbleHover
                cursorShape: Qt.PointingHandCursor
            }
            ToolTip.visible: bubbleHover.hovered
                             && bubble.personName.length > 0
            ToolTip.text: bubble.personName
            ToolTip.delay: 300

            TapHandler {
                onTapped: root.activated(bubble.identity)
            }
        }
    }
}
