import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: card

    property int replyCount: -1
    property string latestSender: ""
    property string latestSenderId: ""
    property string latestPreview: ""
    property string latestKind: "text"
    property string latestAvatarMxc: ""
    property var latestTimestamp: undefined
    property bool unread: false

    // Facepile: ordered, MXID-deduplicated participants ({userId, displayName,
    // avatarUrl}) from ThreadManager. Empty means unknown, never "nobody": the
    // card falls back to the latest sender's avatar.
    property var participants: []
    readonly property int maxFaces: 4
    readonly property int faceSize: 18
    readonly property int faceOverlap: 6

    signal activated()

    readonly property bool hasReplies:
        replyCount > 0 || latestPreview.length > 0 || latestSender.length > 0

    visible: hasReplies
    implicitWidth: Math.min(420, content.implicitWidth + 2 * hPad)
    implicitHeight: hasReplies ? Math.max(30, content.implicitHeight + 2 * vPad) : 0

    readonly property int hPad: AppTheme.spacing8
    readonly property int vPad: AppTheme.spacing6

    function previewLabel() {
        switch (latestKind) {
        case "image":     return qsTr("Image")
        case "gif":       return qsTr("GIF")
        case "video":     return qsTr("Video")
        case "audio":     return qsTr("Audio")
        case "file":      return latestPreview.length > 0 ? latestPreview
                                                          : qsTr("File")
        case "redacted":  return qsTr("Message removed")
        case "encrypted": return qsTr("Encrypted reply")
        case "sticker":   return qsTr("Sticker")
        case "poll":      return qsTr("Poll")
        case "unsupported": return qsTr("New reply")
        default:
            return latestPreview.length > 0 ? latestPreview : qsTr("New reply")
        }
    }

    function countLabel() {
        return replyCount > 0 ? qsTr("%n reply(s)", "", replyCount)
                              : qsTr("Replies")
    }

    function timeLabel() {
        if (latestTimestamp === undefined || latestTimestamp === null)
            return ""
        var d = latestTimestamp
        if (isNaN(d.getTime && d.getTime()))
            return ""
        return Qt.formatDateTime(d, app.settings.clockTimeFormat)
    }

    Accessible.role: Accessible.Button
    Accessible.focusable: true
    Accessible.name: {
        var base = qsTr("Open thread")
        if (replyCount > 0)
            base = qsTr("%1, %n reply(s)", "thread card, reply count",
                        replyCount).arg(base)
        if (latestSender.length > 0)
            base = qsTr("%1, latest reply from %2").arg(base).arg(latestSender)
        var p = previewLabel()
        if (p.length > 0)
            base = qsTr("%1: %2", "thread card, name then message preview")
                .arg(base).arg(p)
        return base
    }
    Accessible.onPressAction: card.activated()

    activeFocusOnTab: hasReplies
    Keys.onReturnPressed: card.activated()
    Keys.onEnterPressed: card.activated()
    Keys.onSpacePressed: card.activated()

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusMd
        color: AppTheme.sidebar
        border.width: card.activeFocus ? 2 : 1
        border.color: card.activeFocus ? AppTheme.accent
                      : mouse.containsMouse ? AppTheme.accentBorder
                                            : AppTheme.border
        Behavior on border.color { ColorAnimation { duration: 90 } }

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.leftMargin: card.hPad
            anchors.rightMargin: card.hPad
            anchors.topMargin: card.vPad
            anchors.bottomMargin: card.vPad
            spacing: AppTheme.spacing8

            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "forum"
                size: 17
                color: card.unread ? AppTheme.accent : AppTheme.textMuted
            }

            Item {
                id: facepile
                readonly property var people: card.participants
                readonly property int shown:
                    Math.min(card.maxFaces, people.length)
                readonly property int step: card.faceSize - card.faceOverlap
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: card.faceSize
                // Width derives from the count and the card's inputs, never
                // from a child's `visible`: visible is effective visibility, so
                // a width gating a child it depends on would latch at 0.
                readonly property bool hasFallback:
                    shown === 0 && card.latestSender.length > 0
                Layout.preferredWidth: shown > 0
                    ? card.faceSize + (shown - 1) * step
                    : (hasFallback ? card.faceSize : 0)

                Repeater {
                    model: facepile.shown
                    delegate: Item {
                        required property int index
                        z: facepile.shown - index
                        x: index * facepile.step
                        width: card.faceSize
                        height: card.faceSize
                        readonly property var person:
                            index >= 0 && index < facepile.people.length
                                ? facepile.people[index] : null
                        Avatar {
                            anchors.fill: parent
                            size: card.faceSize
                            mxc: parent.person ? parent.person.avatarUrl : ""
                            name: parent.person ? parent.person.displayName : ""
                            colorKey: parent.person ? parent.person.userId : ""
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: "transparent"
                            border.color: AppTheme.surface
                            border.width: 1.5
                            visible: index > 0
                        }
                    }
                }

                Avatar {
                    id: fallbackAvatar
                    visible: facepile.hasFallback
                    width: card.faceSize
                    height: card.faceSize
                    size: card.faceSize
                    mxc: card.latestAvatarMxc
                    name: card.latestSender
                    colorKey: card.latestSenderId
                }
            }

            Label {
                text: card.countLabel()
                color: AppTheme.accent
                font.pixelSize: AppTheme.scaled(12)
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
            }

            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                font.pixelSize: AppTheme.scaled(11)
                color: card.unread ? AppTheme.textPrimary : AppTheme.textMuted
                text: card.latestSender.length > 0
                      ? card.latestSender + ": " + card.previewLabel()
                      : card.previewLabel()
            }

            Loader {
                active: card.timeLabel().length > 0
                visible: active
                Layout.alignment: Qt.AlignVCenter
                sourceComponent: Label {
                    text: card.timeLabel()
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(12)
                }
            }

            Rectangle {
                visible: card.unread
                Layout.preferredWidth: 7
                Layout.preferredHeight: 7
                Layout.alignment: Qt.AlignVCenter
                radius: 3.5
                color: AppTheme.accent
            }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        onClicked: {
            card.forceActiveFocus()
            card.activated()
        }
    }
}
