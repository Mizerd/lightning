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
    property string latestSenderId: ""          // MXID for the stable fallback colour
    property string latestPreview: ""           // sanitized latest-reply preview
    property string latestKind: "text"          // semantic kind for the label
    property string latestAvatarMxc: ""         // mxc:// via the safe avatar path
    property var latestTimestamp: undefined      // QDateTime or undefined
    property bool unread: false

    // v0.7 facepile. Ordered, MXID-deduplicated participant rows
    // ({userId, displayName, avatarUrl}) from ThreadManager — real thread
    // senders, never inferred from rendered delegates. Empty means UNKNOWN
    // (not yet fetched, or the lookup failed), never "nobody": the card
    // falls back to the latest sender's avatar rather than showing an empty
    // facepile.
    property var participants: []
    // The design shows a small stack; more than this is not more legible.
    readonly property int maxFaces: 4
    readonly property int faceSize: 18
    readonly property int faceOverlap: 6

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

            // Participant facepile — REAL thread participants (root sender
            // first, then first-appearance order, deduplicated by MXID on
            // the Rust side). Falls back to the latest reply's sender alone
            // while participants are still unknown, so the card never loses
            // the avatar it used to show.
            //
            // Geometry is fixed by the COUNT, which is known synchronously;
            // each Avatar has a fixed size. So resolving an avatar image
            // later cannot move anything — no layout jump.
            Item {
                id: facepile
                readonly property var people: card.participants
                readonly property int shown:
                    Math.min(card.maxFaces, people.length)
                readonly property int step: card.faceSize - card.faceOverlap
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: card.faceSize
                // Width derives from the participant COUNT and the card's
                // own inputs — never from a child's `visible`. Item.visible
                // reads EFFECTIVE (ancestor-gated) visibility, so a width
                // that depended on fallbackAvatar.visible while also gating
                // it would latch at 0: once hidden, the child reads hidden
                // regardless of its own binding and nothing re-evaluates.
                // That permanently hid the fallback avatar on cards created
                // before their latest-sender metadata hydrated.
                readonly property bool hasFallback:
                    shown === 0 && card.latestSender.length > 0
                Layout.preferredWidth: shown > 0
                    ? card.faceSize + (shown - 1) * step
                    : (hasFallback ? card.faceSize : 0)

                Repeater {
                    model: facepile.shown
                    delegate: Item {
                        required property int index
                        // Leftmost face on top reads as a stack rather than
                        // a row of discs.
                        z: facepile.shown - index
                        x: index * facepile.step
                        width: card.faceSize
                        height: card.faceSize
                        // Guarded: on a shrink, a surviving delegate can
                        // re-evaluate before the Repeater model shrinks and
                        // would otherwise dereference undefined. Several
                        // suites assert zero engine warnings.
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
                        // Drawn OVER the avatar: an inner ring in the card's
                        // own colour separates overlapping faces without
                        // changing the pile's geometry.
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

                // Pre-participants fallback: exactly what the card showed
                // before facepiles existed.
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
