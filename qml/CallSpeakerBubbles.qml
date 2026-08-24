import QtQuick
import QtQuick.Controls
import MatrixClient

// The row of participant bubbles above the call stage.
//
// Discord's arrangement, Lightning's tokens: one circular avatar per person
// in the call, side by side, with a ring that lights while they are speaking.
// It is the fastest read there is of "who is here and who is talking" — the
// stage answers "what are they showing", and at a glance those are different
// questions. No Discord asset, colour or wording is used; every value comes
// from AppTheme.
//
// The ring is driven by the SFU's own speaker updates (LiveKit computes audio
// levels server-side and pushes `SpeakersChanged`), NOT by anything measured
// locally. That matters twice: it works for remote participants whose audio we
// may not even be subscribed to, and it means no audio data is inspected here.
//
// Delegate discipline: this is instantiated per participant, so a Label whose
// text can legitimately be empty lives behind a Loader — a never-laid-out
// empty Text keeps ItemObservesViewport forever.
Item {
    id: root

    /// Participant rows, exactly as the stage receives them.
    property var people: []
    /// Bumped by the owner when the participant list changes, because
    /// `participants()` is a function call Qt cannot observe.
    property int refreshTick: 0
    /// The identity a click should spotlight, reported back to the owner.
    signal activated(string identity)

    readonly property int bubbleSize: 34
    readonly property int _count: root.people ? root.people.length : 0

    implicitHeight: bubbleSize + 10
    // Zero height when there is nobody yet, so the stage does not reserve a
    // strip of empty space while a call is still connecting.
    height: root._count > 0 ? implicitHeight : 0
    visible: height > 0

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
        model: root.people
        boundsBehavior: Flickable.StopAtBounds

        delegate: Item {
            id: bubble
            required property var modelData
            width: root.bubbleSize + 4
            height: strip.height

            readonly property bool speaking: bubble.modelData.speaking === true
            readonly property bool muted: bubble.modelData.micKnown === true
                                          && bubble.modelData.micMuted === true
            readonly property string personName: bubble.modelData.local === true
                ? qsTr("You")
                : (bubble.modelData.displayName
                   || bubble.modelData.userId || "")

            Accessible.role: Accessible.Button
            Accessible.name: bubble.personName
            Accessible.description: {
                var parts = [];
                if (bubble.speaking)
                    parts.push(qsTr("Speaking"));
                if (bubble.muted)
                    parts.push(qsTr("Microphone muted"));
                if (bubble.modelData.screenSharing === true)
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
                    mxc: bubble.modelData.avatarMxc || ""
                    // The REAL name, never the "You" label: initials of
                    // "You" render a Y for the local user and nothing
                    // recognisable.
                    name: bubble.modelData.displayName
                          || bubble.modelData.userId || ""
                    colorKey: bubble.modelData.userId || ""
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

                // Screen-share badge, top-right. Same honesty rule.
                Loader {
                    active: bubble.modelData.screenSharing === true
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
                onTapped: root.activated(bubble.modelData.identity || "")
            }
        }
    }
}
