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
    // A mention count carries "@" so it is not told apart by colour alone
    // (WCAG 1.4.1), as Home's badge already does; the accessible name stays
    // the number.
    text: root.dot && root.count <= 0 ? "" : (root.mention ? "@" + root.count : root.count)
    Accessible.name: root.dot && root.count <= 0 ? "" : String(root.count)

    // The derived badge inks (AppTheme.mentionBadgeInk / unreadCountInk): the
    // theme's own text colour, or the opposite ink where keeping it would
    // move the badge colour too far.
    color: root.mention ? AppTheme.mentionBadgeInk : root.muted ? root.mutedInk : AppTheme.unreadCountInk
    background: Rectangle {
        // The dot keeps the theme's unread colour and carries its contrast
        // in a thin ring (unreadDotRing); the count pill has the fill its
        // number reads on (unreadCountFill).
        readonly property bool ringed: root.dotOnly && !root.mention && !root.muted
        color: root.mention ? AppTheme.roomMentionBadge : root.muted ? "transparent"
             : root.dotOnly ? AppTheme.unreadDot : AppTheme.unreadCountFill
        border.width: ringed ? AppTheme.badgeDotRingWidth
                             : root.muted && !root.mention ? 1 : 0
        border.color: ringed ? AppTheme.unreadDotRing : AppTheme.chipNeutralBorder
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
