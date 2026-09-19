import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// ONE LINE WHERE AN EMBED WAS.
//
// Settings → Appearance → Timeline → "Collapse media and link embeds" turns
// every picture, video, audio card, file card, sticker and loaded link
// preview in the timeline into this row. It is the disclosure control for
// that embed and nothing else: it renders text and one icon-font glyph, and
// it never asks MediaBridge, LinkPreviewController or the network for
// anything. That is not incidental — while a row is collapsed the media
// component is not INSTANTIATED at all, so collapsing strictly removes
// fetches rather than moving them.
//
// WHY A VISIBLE CHEVRON AND NOT HOVER OR A KEY. The maintainer named three
// possible affordances and picked none. Hover is invisible at rest, so a
// reader who does not already know the row is expandable never finds out,
// and it is unreachable by keyboard and by touch. A keyboard shortcut needs
// a "current message" the room timeline does not have (its rows are not a
// focus ring and nothing owns a selection outside forwarding mode). A
// disclosure chevron on a row that is itself the click target is visible at
// rest, is what Element and Discord both use for a collapsed embed, and is
// reachable by Tab because this item takes focus. The other two are then
// additions rather than the only way in.
//
// WHAT THE LINE MUST STILL SAY. A collapsed embed that reads "Attachment"
// has replaced clutter with a mystery. The host supplies the kind and
// whatever that surface already knows — a filename, a host, a duration, a
// pixel size — and both halves are in the accessible name too, because a
// screen reader gets no benefit from a glyph.
//
// Behind a Loader in its host, so a timeline with the setting off pays
// nothing at all for it.
Item {
    id: root

    /// What the embed is: "Image", "Video", "Voice message", "Link"…
    property string kindLabel: ""
    /// What it is OF: a filename, a host, "1920×1080", "0:42". May be empty
    /// when the surface genuinely knows nothing else about itself.
    property string detailText: ""
    /// An Icon.qml name. Icon answers an unknown name with an EMPTY STRING
    /// rather than tofu, so a typo here is a silently blank glyph — every
    /// name this file can be given is in the map at the top of Icon.qml.
    property string iconName: "attach_file"
    /// Whether the embed below is currently showing. Drives the chevron and
    /// the wording; the row exists in both states so the expansion is
    /// reversible. A one-way expand would mean the setting silently stopped
    /// applying to every row the reader had ever opened.
    property bool expanded: false
    /// The column this row may not exceed. 0 means "not known yet" (the
    /// bubble has no width during the first binding pass), and is treated as
    /// unbounded rather than as zero — clamping to 0 would collapse the row
    /// to nothing and never recover.
    property real maximumWidth: 0
    /// False while the row must not react — multi-select mode, where every
    /// other surface in the delegate is suspended too.
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

    // A quiet plate that only appears under the pointer. At rest the row is
    // a line of muted text, which is the entire point of the setting: a
    // permanent bordered card per embed would be clutter of a different
    // shape. The focus ring below is drawn in every state, because a
    // keyboard user has no pointer to reveal anything with.
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

        // A SQUARE PLATE, and that is not a detail. `expand_more`'s ink is
        // 12×6 device px and `chevron_right`'s is 7×12 — transposes of each
        // other — so a plate that hugged either glyph would change size when
        // the row toggled and shift everything after it. Place the ink, not
        // the box (§16).
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
            // The sender chose the filename and the link host. Never markup.
            textFormat: Text.PlainText
            text: root.summaryText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            // ElideMiddle, matching the file card: the tail of a filename
            // (".tar.gz", "-final-v3.png") is the half that distinguishes it.
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
        // WithinBounds, not the default DragThreshold. On the default a
        // TapHandler takes only a PASSIVE grab and the ancestor's handler
        // fires on the same press — which here is the row's own tap, so a
        // click meant to expand a picture would also toggle a selection or
        // open the message. §16 records the same fix on the image viewer's
        // thumbnail strip.
        gesturePolicy: TapHandler.WithinBounds
        onTapped: root.toggleRequested()
    }

    // Keyboard reach. This row is the ONLY way to the embed while the
    // setting is on, so it has to be operable without a pointer — the same
    // argument MediaHiddenPlaceholder makes for "Show image".
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
        // Right/left as well, because this is a disclosure control and that
        // is what a disclosure control answers to everywhere else. They are
        // directional rather than toggling on purpose: pressing Right twice
        // must not close what the first press opened.
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
