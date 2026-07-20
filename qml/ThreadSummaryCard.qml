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
        // Design thread chip: panel-tier background, 1px border that warms
        // toward the accent on hover; keyboard focus gets the accent stroke.
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

            // Thread indicator glyph — Material Symbols "forum" per the
            // handoff (interface chrome never draws inline vector icons).
            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "forum"
                size: 17
                color: card.unread ? AppTheme.accent : AppTheme.textMuted
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
                font.pixelSize: 12
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
                font.pixelSize: 12
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
