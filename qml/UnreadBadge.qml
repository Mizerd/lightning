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
    /// Show the DOT form: the room is unread but carries no honest number.
    ///
    /// Matrix computes notification_count only where a room's push rules say
    /// to, so a conversation can be genuinely unread with a count of 0. A
    /// pill that hides itself at 0 then makes that room look read, which is
    /// how messages went unnoticed until another client was opened. There is
    /// no number to show, so it shows that something is waiting instead.
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
    // A single digit renders as a circle rather than a squat lozenge.
    // `height`/`width`, not implicitHeight — Label derives its implicit size
    // from its text and refuses an assignment. The width is a FLOOR, not a
    // fixed size, so "128" still fits.
    readonly property bool dotOnly: root.dot && root.count <= 0
    height: dotOnly ? 10 : 18
    width: dotOnly ? 10 : Math.max(18, implicitWidth)
    Layout.preferredHeight: dotOnly ? 10 : 18
    Layout.minimumWidth: dotOnly ? 10 : 18
    leftPadding: dotOnly ? 0 : AppTheme.spacing6
    rightPadding: dotOnly ? 0 : AppTheme.spacing6
}
