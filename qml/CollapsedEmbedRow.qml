import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One line in place of an embed, when "Collapse media and link embeds" is on
// (pictures, video, audio, files, stickers and link previews). It renders text
// and one icon glyph and never asks MediaBridge, LinkPreviewController or the
// network for anything; while collapsed the media component isn't
// instantiated at all, so collapsing removes fetches rather than deferring
// them.
//
// The affordance is a visible disclosure chevron on a clickable, focusable
// row: hover is invisible at rest and unreachable by keyboard/touch, and the
// timeline has no "current message" for a shortcut.
//
// The line still says what the embed is (kind plus filename, host, duration
// or size), in the accessible name too. Hosted behind a Loader, so the
// setting costs nothing when off.
Item {
    id: root

    /// What the embed is: "Image", "Video", "Voice message", "Link"…
    property string kindLabel: ""
    /// What it is of: a filename, a host, "1920×1080", "0:42". May be empty.
    property string detailText: ""
    /// An Icon.qml name. Unknown names render blank, not tofu, so use names in
    /// Icon.qml's map.
    property string iconName: "attach_file"
    /// Whether the embed is showing. The row exists in both states so expanding
    /// is reversible.
    property bool expanded: false
    /// Maximum width. 0 means not known yet (no bubble width on the first
    /// pass) and is treated as unbounded, since clamping to 0 never recovers.
    property real maximumWidth: 0
    /// False while the row must not react (multi-select mode).
    property bool interactive: true

    signal toggleRequested()

    objectName: "collapsedEmbedRow"

    readonly property string summaryText:
        root.detailText.length > 0
            ? root.kindLabel + " · " + root.detailText
            : root.kindLabel

    readonly property real naturalWidth:
        contentRow.implicitWidth + AppTheme.spacing8 * 2
    implicitWidth: root.maximumWidth > 0
                   ? Math.max(1, Math.min(root.maximumWidth, naturalWidth))
                   : Math.max(1, naturalWidth)
    implicitHeight: contentRow.implicitHeight + AppTheme.spacing4 * 2

    // A plate only under the pointer; at rest the row is muted text. The focus
    // ring below draws in every state.
    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: rowHover.hovered && root.interactive
               ? AppTheme.embedSurface : "transparent"
        border.width: 1
        border.color: rowHover.hovered && root.interactive
                      ? AppTheme.border : "transparent"
    }

    RowLayout {
        id: contentRow
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        spacing: AppTheme.spacing6

        // A square plate: `expand_more` and `chevron_right` have transposed ink
        // boxes, so a hugging plate would resize on toggle and shift the row.
        Item {
            objectName: "collapsedEmbedChevronPlate"
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            Layout.alignment: Qt.AlignVCenter
            Icon {
                objectName: "collapsedEmbedChevron"
                anchors.centerIn: parent
                name: root.expanded ? "expand_more" : "chevron_right"
                size: 16
                color: rowHover.hovered && root.interactive
                       ? AppTheme.text : AppTheme.textMuted
            }
        }

        Icon {
            objectName: "collapsedEmbedKindIcon"
            Layout.alignment: Qt.AlignVCenter
            name: root.iconName
            size: 14
            color: AppTheme.textMuted
        }

        Label {
            objectName: "collapsedEmbedLabel"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
            // The sender chose the filename and link host. Never markup.
            textFormat: Text.PlainText
            text: root.summaryText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            // ElideMiddle, as on the file card: a filename's tail distinguishes
            // it.
            elide: Label.ElideMiddle
            maximumLineCount: 1
        }
    }

    HoverHandler {
        id: rowHover
        enabled: root.interactive
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        objectName: "collapsedEmbedTap"
        enabled: root.interactive
        // WithinBounds: the default DragThreshold only grabs passively, so the
        // ancestor row's tap would also fire (toggling a selection or opening
        // the message).
        gesturePolicy: TapHandler.WithinBounds
        onTapped: root.toggleRequested()
    }

    // The only way to the embed while the setting is on, so it must work
    // without a pointer.
    activeFocusOnTab: root.interactive
    Accessible.role: Accessible.Button
    Accessible.name: root.expanded
        ? qsTr("Hide %1").arg(root.summaryText)
        : qsTr("Show %1").arg(root.summaryText)
    Accessible.description: root.expanded
        ? qsTr("This attachment is expanded. Activate to collapse it back to one line.")
        : qsTr("This attachment is collapsed to one line. Activate to show it.")
    Accessible.onPressAction: {
        if (root.interactive)
            root.toggleRequested()
    }
    Keys.onPressed: event => {
        if (!root.interactive)
            return
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
            || event.key === Qt.Key_Enter) {
            root.toggleRequested()
            event.accepted = true
            return
        }
        // Right/Left as for any disclosure control; directional, so pressing
        // Right twice doesn't close it again.
        if (event.key === Qt.Key_Right && !root.expanded) {
            root.toggleRequested()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left && root.expanded) {
            root.toggleRequested()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
