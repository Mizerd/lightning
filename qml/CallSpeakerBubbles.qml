import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property var model: app.groupCall.participantModel

    signal activated(string identity)

    readonly property int bubbleSize: 34
    readonly property int _count: root.model ? root.model.count : 0

    // Zero implicit height with nobody present, so a connecting call reserves
    // no band. Implicit rather than an explicit `height:` because the host
    // RowLayout writes width/height onto children, destroying such bindings.
    implicitHeight: root._count > 0 ? root.bubbleSize + 10 : 0
    readonly property int cellWidth: root.bubbleSize + 8

    implicitWidth: root._count > 0
        ? Math.min(root._count * root.cellWidth
                   + (root._count - 1) * AppTheme.spacing6,
                   root.maxImplicitWidth)
        : 0
    property int maxImplicitWidth: 220
    visible: root._count > 0

    ListView {
        id: strip
        objectName: "callSpeakerBubbles"
        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: AppTheme.spacing6
        clip: true
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
                    name: bubble.displayName || bubble.userId || ""
                    colorKey: bubble.userId
                    size: root.bubbleSize
                }

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
