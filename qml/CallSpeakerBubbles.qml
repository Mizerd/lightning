import QtQuick
import QtQuick.Controls
import MatrixClient

// The row of participant bubbles for a COLLAPSED call.
//
// One circular avatar per person, side by side, with a ring that lights while
// they are speaking. It is the fastest read there is of "who is here and who
// is talking", which is the only question a one-line strip has room to answer.
//
// WHERE IT IS NOT: above the expanded stage. It used to render there
// permanently, as a second, smaller copy of the same faces the stage was
// already drawing underneath — a whole line of the message column spent
// repeating the tiles below it. Expanded, the stage IS the participant view.
//
// The ring is driven by the SFU's own speaker updates (LiveKit computes audio
// levels server-side and pushes `SpeakersChanged`), NOT by anything measured
// locally. That matters twice: it works for remote participants whose audio we
// may not even be subscribed to, and it means no audio data is inspected here.
// The bubble ring stays BINARY on purpose — at 34 px an amplitude ring is a
// sub-pixel wobble, and the stage's own tiles are where the level is visible.
//
// Delegate discipline: this is instantiated per participant, so a Label whose
// text can legitimately be empty lives behind a Loader — a never-laid-out
// empty Text keeps ItemObservesViewport forever.
Item {
    id: root

    /// The REAL participant model, bound directly.
    ///
    /// It used to be a JS array copied out of `participants()` behind a
    /// hand-bumped tick, which is a MODEL RESET on every speaker update: every
    /// bubble destroyed and rebuilt on every syllable. Overridable so a test
    /// can hand it a fixture model.
    property var model: app.groupCall.participantModel

    /// The identity a click should spotlight, reported back to the owner.
    signal activated(string identity)

    readonly property int bubbleSize: 34
    readonly property int _count: root.model ? root.model.count : 0

    // Zero IMPLICIT height when there is nobody yet, so the strip reserves no
    // band of empty space while a call is still connecting.
    //
    // The emptiness lives in `implicitHeight` rather than in an explicit
    // `height:` binding on purpose: this is hosted inside a RowLayout, and a
    // layout WRITES width and height onto its children — which destroys any
    // binding they had on those properties. An implicit size is what a layout
    // reads instead of overwrites. Outside a layout, an Item with no explicit
    // height still takes its implicit one, so the standalone form is
    // unchanged.
    implicitHeight: root._count > 0 ? root.bubbleSize + 10 : 0
    visible: root._count > 0

    ListView {
        id: strip
        objectName: "callSpeakerBubbles"
        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: AppTheme.spacing6
        clip: true
        // More people than fit is a scroll, never a silent truncation: an
        // "+N" badge here would be a second count next to the header's, and
        // the two would disagree the moment one of them was capped.
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

            width: root.bubbleSize + 4
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

            // The speaking ring. A ring rather than a growing avatar: the row
            // must not change size on every syllable, or a call with four
            // people talking reflows continuously.
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
                    // The REAL name, never the "You" label: initials of
                    // "You" render a Y for the local user and nothing
                    // recognisable.
                    name: bubble.displayName || bubble.userId || ""
                    colorKey: bubble.userId
                    size: root.bubbleSize
                }

                // Muted badge, bottom-right, only when the SFU actually said
                // so: unknown renders nothing rather than a confident and
                // possibly wrong "not muted".
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

                // Screen-share badge, top-right. Same honesty rule. In a
                // collapsed call this is the only sign a share is running,
                // which is why it is not hover-gated.
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
