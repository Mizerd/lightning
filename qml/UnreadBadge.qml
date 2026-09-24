import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Unread / mention count pill, shared by the Classic and Channels layouts so
// they cannot drift apart.
//
// A mention keeps its colour even when the room is muted. A muted plain count
// drops to an outline pill in muted ink.
Label {
    id: root

    /// The number shown. Zero renders nothing.
    property int count: 0
    /// This count is a mention, not just unread traffic.
    property bool mention: false
    /// The room is muted. Only affects a plain count.
    property bool muted: false
    /// Ink for the muted-plain case, tuned against the host's surface.
    property color mutedInk: AppTheme.textMuted
    /// Show the dot form: unread, but with no count to show. Matrix computes
    /// notification_count only where push rules say to, so a room can be unread
    /// with a count of 0.
    property bool dot: false

    visible: root.count > 0 || root.dot
    text: root.dot && root.count <= 0 ? "" : root.count

    color: root.mention ? AppTheme.dangerText : root.muted ? root.mutedInk : AppTheme.accentText
    background: Rectangle {
        color: root.mention ? AppTheme.mentionBadge : root.muted ? "transparent" : AppTheme.unreadBadge
        border.width: root.muted && !root.mention ? 1 : 0
        border.color: AppTheme.chipNeutralBorder
        radius: AppTheme.radiusPill
    }
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    font.pixelSize: AppTheme.textMicro
    font.weight: AppTheme.weightBold
    // A single digit renders as a circle. Set width/height rather than implicit
    // size (Label derives its implicit size from its text); the width is a floor.
    readonly property bool dotOnly: root.dot && root.count <= 0
    height: dotOnly ? 10 : 18
    width: dotOnly ? 10 : Math.max(18, implicitWidth)
    Layout.preferredHeight: dotOnly ? 10 : 18
    Layout.minimumWidth: dotOnly ? 10 : 18
    leftPadding: dotOnly ? 0 : AppTheme.spacing6
    rightPadding: dotOnly ? 0 : AppTheme.spacing6
}
