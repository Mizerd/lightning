import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "This room is still unread because of a thread." A room's badge is cleared by
// reading the MAIN timeline, but a thread reply is cleared only by opening that
// thread, so without this a room stays unread with nothing in the main
// timeline to explain it. Element draws the same thing: a thread glyph beside
// the room instead of a plain unread count.
//
// Shared by the Classic and Channels layouts, in the same colours as
// UnreadBadge so a mention keeps its meaning. There is deliberately NO number:
// the bridge only knows that threads hold the remainder, never how many
// replies per thread, and an invented count would be wrong.
Rectangle {
    id: root
    objectName: "threadUnreadBadge"

    /// 0 nothing (renders nothing), 1 unread in a thread, 2 mention in a thread.
    property int kind: 0
    /// Ink for the glyph's pill when the host row is not the ordinary surface.
    readonly property bool mention: kind === 2

    readonly property string label: mention ? qsTr("Mention in a thread")
                                            : qsTr("Unread in a thread")

    visible: kind > 0
    implicitWidth: 18
    implicitHeight: 18
    width: implicitWidth
    height: implicitHeight
    Layout.preferredWidth: 18
    Layout.preferredHeight: 18
    radius: AppTheme.radiusPill
    // The fills every count badge uses, each with the ink that reads on it
    // at 4.5:1 (7:1 high contrast).
    color: mention ? AppTheme.mentionBadgeFill : AppTheme.unreadCountFill

    Accessible.role: Accessible.StaticText
    Accessible.name: root.label

    Icon {
        anchors.centerIn: parent
        name: "forum"
        size: 12
        color: root.mention ? AppTheme.mentionBadgeInk : AppTheme.unreadCountInk
    }

    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered && root.visible
    ToolTip.delay: 500
    ToolTip.text: root.label
}
