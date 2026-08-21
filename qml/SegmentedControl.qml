import QtQuick
import QtQuick.Controls
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
Row {
    id: root

    property var model: []
    property var current
    // Compact variant for tight hosts (the 260px Settings-nav inline
    // results): smaller type and padding, same interaction and states.
    property bool dense: false
    // Storm surfaces (Settings, pickers): storm selection fill and inks;
    // themed hosts (Room Information tabs) keep the default treatment.
    property bool storm: false
    signal activated(var value)

    spacing: 2

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
}
