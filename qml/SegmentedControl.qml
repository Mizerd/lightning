import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One coherent segmented row for mutually exclusive choices (Room
// Information tabs, GIF provider/section tabs, message-layout selector).
// Segments are flat: the selected segment carries the accent-soft chip with
// accent text; unselected segments are transparent with a soft hover tint —
// never independent outlined rectangles, never native TabButtons.
//
// model: list of { label, value, enabled?, tip? } (or plain strings, where
// the string is both label and value). `current` is the selected value;
// clicking emits activated(value) — the owner updates `current`.
//
// A RowLayout rather than a Row so `fitWidth` can compact it (see below).
// With fitWidth off the two are equivalent: no segment fills, so each takes
// its natural width and the control's implicit width is their sum.
RowLayout {
    id: root

    property var model: []
    property var current
    // Compact variant for tight hosts (the 260px Settings-nav inline
    // results): smaller type and padding, same interaction and states.
    property bool dense: false
    // Fit the row into the width the host gives it instead of overflowing
    // past its edge. Opt-in, because a row wider than its host is harmless
    // in a host that sizes itself to this control (Room Information tabs, the
    // GIF provider tabs) and wrong in a host that clips — the room-list
    // column clips, so its four chips lost "Unreads" to the pane boundary.
    //
    // The compaction is the LAYOUT's, not arithmetic of ours: fillWidth plus
    // a maximumWidth of the segment's natural width makes QtQuickLayouts
    // shrink the segments proportionally. A hand-rolled scale factor cannot
    // work here — a plain Row derives its implicitWidth from its children's
    // ASSIGNED widths, so shrinking them shrinks the total the scale was
    // computed from, and the measured result is a polish() loop that settles
    // at less than half the available width. A RowLayout's implicitWidth is
    // the sum of the children's IMPLICIT widths and stays put, which is why
    // `overflowing` below can read it safely.
    //
    // The trailing filler is NOT decoration. A RowLayout given more width
    // than it needs SPREADS its children across it — as gaps, even when no
    // child can grow — where a Row leaves them packed at the start. Measured:
    // four chips in a 536px row landed at x = 0, 75, 221, 367 instead of
    // 0, 28, 81, 134. Every host that hands this control a fillWidth cell got
    // that, which is how the room-list chips ended up strewn across the
    // column. One filler that soaks up the surplus restores Row's packing in
    // every host, and it contributes nothing to the implicit width.
    property bool fitWidth: false
    readonly property real segmentSpacing: 2
    readonly property bool overflowing:
        fitWidth && width > 0 && implicitWidth > width
    // Storm surfaces (Settings, pickers): storm selection fill and inks;
    // themed hosts (Room Information tabs) keep the default treatment.
    property bool storm: false
    signal activated(var value)

    spacing: root.segmentSpacing

    Repeater {
        model: root.model
        delegate: AbstractButton {
            id: segment
            required property var modelData
            objectName: root.objectName.length > 0
                        ? root.objectName + "_" + String(segValue) : ""
            readonly property string segLabel:
                typeof modelData === "string" ? modelData
                                              : (modelData.label || "")
            readonly property var segValue:
                typeof modelData === "string" ? modelData : modelData.value
            readonly property bool selected: root.current === segValue

            enabled: typeof modelData === "string"
                     || modelData.enabled === undefined
                     || modelData.enabled === true
            implicitWidth: segText.implicitWidth + (root.dense ? 12 : 24)
            implicitHeight: root.dense ? AppTheme.buttonHeightSm
                                       : AppTheme.buttonHeight
            // Only while the row genuinely does not fit. fillWidth when it
            // DOES fit spreads the segments across the host's width, which
            // turns a compact chip row into four widely separated buttons.
            Layout.fillWidth: root.overflowing
            Layout.maximumWidth: implicitWidth
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            // A disabled segment is not a target: it must not offer a tip it
            // cannot act on, and AbstractButton keeps `hovered` true while
            // disabled, so the guard has to be explicit.
            opacity: enabled ? 1.0 : 0.55
            Accessible.role: Accessible.RadioButton
            Accessible.name: segLabel
            ToolTip.text: typeof modelData === "string"
                          ? "" : (modelData.tip || "")
            ToolTip.visible: enabled && hovered && ToolTip.text.length > 0
            ToolTip.delay: 400
            onClicked: root.activated(segValue)

            contentItem: Label {
                id: segText
                text: segment.segLabel
                color: {
                    if (root.storm)
                        return !segment.enabled ? AppTheme.stormTextFaint
                             : segment.selected ? AppTheme.stormText
                             : AppTheme.stormTextMuted
                    // The selected chip's background is accentSoft — a TINT
                    // of the surface, not a solid accent fill — so its ink
                    // must be a surface ink. accentText is the ink for a
                    // SOLID accent fill (it is white in every theme that does
                    // not override it), and pairing it with a tint was
                    // measured invisible: 1.00 on Deep Teal (#062A25 on
                    // #112928), 1.14 on Moss Light and 1.42 on Lightning
                    // Light, all white-on-near-white or dark-on-dark. The
                    // 2026-08-15 Storm report was the same defect on one
                    // theme and was patched for Storm alone; this is the
                    // general fix.
                    //
                    // selectedText is the ink meant for a selected row/chip
                    // and clears AA against accentSoft in every theme
                    // (lowest measured 5.45, Moss Light) —
                    // ThemeTokensTest pins that for all of them. Storm keeps
                    // its sanctioned solid-bolt treatment.
                    return !segment.enabled ? AppTheme.textDisabled
                         : segment.selected ? (AppTheme.storm
                                               ? AppTheme.stormText
                                               : AppTheme.selectedText)
                         : AppTheme.textSecondary
                }
                font.family: root.storm ? AppTheme.menuFont : AppTheme.uiFont
                font.pixelSize: root.dense ? AppTheme.textMeta
                                           : AppTheme.textBody
                font.weight: AppTheme.weightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                // Last resort once the padding has been squeezed out: a
                // clipped glyph reads as a rendering fault, an ellipsis
                // reads as a narrow column.
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: AppTheme.buttonRadius
                color: {
                    // A disabled segment used to fall through to exactly what
                    // an enabled, unselected, unhovered one renders —
                    // transparent with no border — so the ONLY cue was a
                    // one-step ink change that reads as a glitch rather than
                    // a state. It now carries a faint field of its own, on
                    // top of the 0.55 opacity above.
                    if (!segment.enabled)
                        return Qt.alpha(AppTheme.borderStrong,
                                        segment.selected ? 0.60 : 0.35)
                    if (root.storm)
                        return segment.selected ? AppTheme.stormSelection
                             : (segment.down || segment.hovered)
                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                               : "transparent"
                    if (segment.selected)
                        // accentSoft under Storm too, NOT a solid bolt fill.
                        //
                        // roomFilterMode defaults to 0 = "All", so a control
                        // sitting at its factory value carried a permanent
                        // solid block of the app's loudest colour in the
                        // navigation column. Bolt has to mean active/needs-you
                        // or it means nothing, and it was already spent seven
                        // times in one screenshot. The soft field still reads
                        // as selected (stormText on it measures 8.85-14.21
                        // over every ground) and the solid fill is reserved
                        // for the primary button.
                        return AppTheme.accentSoft
                    return segment.down ? AppTheme.buttonGhostPressed
                         : segment.hovered ? AppTheme.buttonGhostHover
                         : "transparent"
                }
                border.width: root.storm && segment.selected ? 1 : 0
                border.color: AppTheme.stormBorderStrong
            }
            // Inset focus ring — see the note in AppButton.qml. Segments sit
            // 2px apart, so an outset ring landed on the neighbour.
            Rectangle {
                anchors.fill: parent
                radius: AppTheme.buttonRadius
                color: "transparent"
                border.color: {
                    if (segment.selected && AppTheme.storm && !root.storm)
                        return AppTheme.boltInk
                    return root.storm ? AppTheme.bolt : AppTheme.focusRing
                }
                border.width: 2
                visible: segment.visualFocus
            }
        }
    }

    // See the fitWidth note: this exists so surplus width becomes trailing
    // space instead of gaps between the segments.
    Item {
        Layout.fillWidth: true
        implicitWidth: 0
        implicitHeight: 0
    }
}
