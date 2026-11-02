import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Label {
    id: root

    property int count: 0
    property bool mention: false
    property bool muted: false
    property color mutedInk: AppTheme.textMuted
    property bool dot: false

    visible: root.count > 0 || root.dot
    // A mention count carries "@" so it is not told apart by colour alone
    // (WCAG 1.4.1), as Home's badge already does; the accessible name stays
    // the number.
    text: root.dot && root.count <= 0 ? "" : (root.mention ? "@" + root.count : root.count)
    Accessible.name: root.dot && root.count <= 0 ? "" : String(root.count)

    color: root.mention ? AppTheme.mentionBadgeInk : root.muted ? root.mutedInk : AppTheme.unreadCountInk
    background: Rectangle {
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
    readonly property bool dotOnly: root.dot && root.count <= 0
    height: dotOnly ? 10 : 18
    width: dotOnly ? 10 : Math.max(18, implicitWidth)
    Layout.preferredHeight: dotOnly ? 10 : 18
    Layout.minimumWidth: dotOnly ? 10 : 18
    leftPadding: dotOnly ? 0 : AppTheme.spacing6
    rightPadding: dotOnly ? 0 : AppTheme.spacing6
}
