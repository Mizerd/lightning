import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Compact thread summary under a thread root in the main timeline: icon, latest
// reply's sender and safe preview, reply count, optional timestamp and unread
// indicator. Presentation only: every value is a safe semantic field from the
// TimelineModel thread-summary roles; nothing parses event JSON, ciphertext or
// media URLs. activated() on click or Enter/Space; the caller opens the thread
// with the real room and root ids.
Item {
    id: card

    // Inputs, bound from the delegate's model roles
    property int replyCount: -1                 // SDK num_replies, -1 unknown
    property string latestSender: ""            // already-resolved display name
    property string latestSenderId: ""          // MXID for the stable fallback colour
    property string latestPreview: ""           // sanitized latest-reply preview
    property string latestKind: "text"          // semantic kind for the label
    property string latestAvatarMxc: ""         // mxc:// via the safe avatar path
    property var latestTimestamp: undefined      // QDateTime or undefined
    property bool unread: false

    // Facepile: ordered, MXID-deduplicated participants ({userId, displayName,
    // avatarUrl}) from ThreadManager. Empty means unknown, never "nobody": the
    // card falls back to the latest sender's avatar.
    property var participants: []
    // A small stack; more is not more legible.
    readonly property int maxFaces: 4
    readonly property int faceSize: 18
    readonly property int faceOverlap: 6

    signal activated()

    // Shown once the SDK reports thread activity: the count or any latest-reply
    // metadata, without inventing a number.
    readonly property bool hasReplies:
        replyCount > 0 || latestPreview.length > 0 || latestSender.length > 0

    visible: hasReplies
    implicitWidth: Math.min(420, content.implicitWidth + 2 * hPad)
    implicitHeight: hasReplies ? Math.max(30, content.implicitHeight + 2 * vPad) : 0

    readonly property int hPad: AppTheme.spacing8
    readonly property int vPad: AppTheme.spacing6

    // Safe label helpers
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
            // The sanitized preview, or a neutral fallback.
            return latestPreview.length > 0 ? latestPreview : qsTr("New reply")
        }
    }

    function countLabel() {
        // Only the SDK's count is shown as a number.
        return replyCount > 0 ? qsTr("%n reply(s)", "", replyCount)
                              : qsTr("Replies")
    }

    function timeLabel() {
        if (latestTimestamp === undefined || latestTimestamp === null)
            return ""
        var d = latestTimestamp
        if (isNaN(d.getTime && d.getTime()))
            return ""
        // The app-wide clock format (Settings -> Appearance).
        return Qt.formatDateTime(d, app.settings.clockTimeFormat)
    }

    Accessible.role: Accessible.Button
    Accessible.focusable: true
    Accessible.name: {
        // Each augmentation is its own template, never concatenated, so
        // translators can move the clause (and RTL punctuation is right).
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
        // Panel-tier background with a 1px border that warms toward the accent
        // on hover; keyboard focus gets the accent stroke.
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

            // Thread glyph (Material Symbols "forum").
            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "forum"
                size: 17
                color: card.unread ? AppTheme.accent : AppTheme.textMuted
            }

            // Participant facepile (root sender first, then first appearance,
            // deduplicated in Rust), falling back to the latest reply's sender
            // while unknown. Geometry depends only on the count, so avatar
            // images loading later never move anything.
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
                        // Leftmost face on top, so it reads as a stack.
                        z: facepile.shown - index
                        x: index * facepile.step
                        width: card.faceSize
                        height: card.faceSize
                        // Guarded: a surviving delegate can re-evaluate before
                        // the model shrinks.
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
                        // An inner ring in the card's colour separates
                        // overlapping faces.
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

                // Fallback before participants are known.
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
                font.pixelSize: 12
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
            }

            // "Sender: preview", single line, elided.
            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                font.pixelSize: 11
                color: card.unread ? AppTheme.textPrimary : AppTheme.textMuted
                text: card.latestSender.length > 0
                      ? card.latestSender + ": " + card.previewLabel()
                      : card.previewLabel()
            }

            // A Loader: timeLabel() returns "" without a timestamp, and a Text
            // holding "" from creation stays a viewport observer (see
            // MessageDelegate.qml).
            Loader {
                active: card.timeLabel().length > 0
                visible: active
                Layout.alignment: Qt.AlignVCenter
                sourceComponent: Label {
                    text: card.timeLabel()
                    color: AppTheme.textMuted
                    font.pixelSize: 12
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
