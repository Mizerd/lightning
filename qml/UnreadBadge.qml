import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The unread / mention count pill.
//
// Extracted so the Classic and Channels layouts cannot drift apart on what a
// count looks like — two hand-rolled pills is how one layout ends up saying
// "3" in danger ink and the other in accent ink for the same room.
//
// Policy, unchanged from what RoomDelegate already did:
//   * A mention keeps its colour even when the room is MUTED. Muting silences
//     the noise; it does not hide that somebody named you.
//   * A muted plain count drops to an outline pill in muted ink, which is
//     what Element does and what makes the mute legible at a glance.
Label {
    id: root

    /// The number shown. A zero renders nothing rather than a "0" pill.
    property int count: 0
    /// This count is a mention, not just unread traffic.
    property bool mention: false
    /// The room is muted. Only changes a PLAIN count's presentation.
    property bool muted: false
    /// Ink for the muted-plain case, which is tuned against the host's
    /// surface rather than globally (see RoomDelegate's metaInk note).
    property color mutedInk: AppTheme.textMuted

    visible: root.count > 0
    text: root.count

    color: root.mention ? AppTheme.dangerText : root.muted ? root.mutedInk : AppTheme.accentText
    background: Rectangle {
        color: root.mention ? AppTheme.mentionBadge : root.muted ? "transparent" : AppTheme.unreadBadge
        border.width: root.muted && !root.mention ? 1 : 0
        border.color: AppTheme.chipNeutralBorder
        radius: AppTheme.radiusPill
    }
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    leftPadding: AppTheme.spacing6
    rightPadding: AppTheme.spacing6
    font.pixelSize: AppTheme.textMicro
    font.weight: AppTheme.weightBold
    // A single digit renders as a circle rather than a squat lozenge.
    // `height`/`width`, not implicitHeight — Label derives its implicit size
    // from its text and refuses an assignment. The width is a FLOOR, not a
    // fixed size, so "128" still fits.
    height: 18
    width: Math.max(18, implicitWidth)
    Layout.preferredHeight: 18
    Layout.minimumWidth: 18
}
