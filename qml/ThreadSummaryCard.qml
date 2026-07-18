import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.1: Element-style compact thread summary rendered under a thread root
// in the main timeline. Shows a speech-bubble icon, the latest reply's
// sender + a safe preview, the authoritative reply count, an optional
// timestamp and an unread indicator. Presentation only — every value is a
// safe semantic field supplied by the TimelineModel thread-summary roles;
// this component never parses event JSON, ciphertext, or media URLs.
//
// Clicking (or Enter/Space while focused) emits activated(); the caller opens
// the correct thread panel with the real room + root ids.
Item {
    id: card

    // ── Inputs (bound from the delegate's model roles) ───────────────
    property int replyCount: -1                 // SDK num_replies, -1 unknown
    property string latestSender: ""            // already-resolved display name
    property string latestPreview: ""           // sanitized latest-reply preview
    property string latestKind: "text"          // semantic kind for the label
    property string latestAvatarMxc: ""         // mxc:// via the safe avatar path
    property var latestTimestamp: undefined      // QDateTime or undefined
    property bool unread: false

    signal activated()

    // A root only shows the card once the SDK reports thread activity. Using
    // the count OR any latest-reply metadata keeps it visible even before the
    // exact count resolves, without inventing a number.
    readonly property bool hasReplies:
        replyCount > 0 || latestPreview.length > 0 || latestSender.length > 0

    visible: hasReplies
    implicitWidth: Math.min(420, content.implicitWidth + 2 * hPad)
    implicitHeight: hasReplies ? Math.max(30, content.implicitHeight + 2 * vPad) : 0

    readonly property int hPad: AppTheme.spacing8
    readonly property int vPad: AppTheme.spacing6

    // ── Safe label helpers ───────────────────────────────────────────
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
            // text / notice / emote — show the sanitized preview, or a neutral
            // fallback when it is empty.
            return latestPreview.length > 0 ? latestPreview : qsTr("New reply")
        }
    }

    function countLabel() {
        // Only the SDK's authoritative count is shown as a number; otherwise a
        // count-free label, never an invented number.
        return replyCount > 0 ? qsTr("%n reply(s)", "", replyCount)
                              : qsTr("Replies")
    }

    function timeLabel() {
        if (latestTimestamp === undefined || latestTimestamp === null)
            return ""
        var d = latestTimestamp
        if (isNaN(d.getTime && d.getTime()))
            return ""
        return Qt.formatDateTime(d, "hh:mm")
    }

    Accessible.role: Accessible.Button
    Accessible.focusable: true
    Accessible.name: {
        var base = qsTr("Open thread")
        if (replyCount > 0)
            base += ", " + qsTr("%n reply(s)", "", replyCount)
        if (latestSender.length > 0)
            base += ", " + qsTr("latest reply from %1").arg(latestSender)
        var p = previewLabel()
        if (p.length > 0)
            base += ": " + p
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
        color: mouse.containsMouse || card.activeFocus
               ? AppTheme.hover : AppTheme.surfaceElevated
        border.width: card.activeFocus ? 2 : 1
        border.color: card.activeFocus ? AppTheme.accent : AppTheme.border
        Behavior on color { ColorAnimation { duration: 90 } }

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.leftMargin: card.hPad
            anchors.rightMargin: card.hPad
            anchors.topMargin: card.vPad
            anchors.bottomMargin: card.vPad
            spacing: AppTheme.spacing8

            // Speech-bubble icon (self-contained vector, theme-aware).
            Canvas {
                id: bubble
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                Layout.alignment: Qt.AlignVCenter
                readonly property color glyph: card.unread ? AppTheme.accent
                                                           : AppTheme.textMuted
                onGlyphChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = glyph
                    ctx.lineWidth = 1.5
                    ctx.lineJoin = "round"
                    var x = 1.5, y = 1.5, w = 13, h = 9, r = 2.5
                    ctx.beginPath()
                    ctx.moveTo(x + r, y)
                    ctx.lineTo(x + w - r, y)
                    ctx.quadraticCurveTo(x + w, y, x + w, y + r)
                    ctx.lineTo(x + w, y + h - r)
                    ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
                    ctx.lineTo(x + 5, y + h)
                    ctx.lineTo(x + 3, y + h + 3)      // little tail
                    ctx.lineTo(x + 3, y + h)
                    ctx.lineTo(x + r, y + h)
                    ctx.quadraticCurveTo(x, y + h, x, y + h - r)
                    ctx.lineTo(x, y + r)
                    ctx.quadraticCurveTo(x, y, x + r, y)
                    ctx.closePath()
                    ctx.stroke()
                }
            }

            // Latest reply sender avatar (existing safe avatar path).
            Avatar {
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                Layout.alignment: Qt.AlignVCenter
                size: 18
                mxc: card.latestAvatarMxc
                name: card.latestSender
                visible: card.latestSender.length > 0
            }

            Label {
                text: card.countLabel()
                color: AppTheme.accent
                font.pixelSize: 11
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
            }

            // "Sender: preview" — elides in narrow windows, single line, never
            // grows to a full message height.
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

            Label {
                text: card.timeLabel()
                visible: text.length > 0
                color: AppTheme.textMuted
                font.pixelSize: 10
                Layout.alignment: Qt.AlignVCenter
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
