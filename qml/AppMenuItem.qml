import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

// v0.7: one menu row of the Lightning popover menu — Material Symbols
// icon slot, text label, legible disabled state, and a visually distinct
// danger variant for destructive actions. Rows stay keyboard-operable
// (arrow keys + Return through the Menu's own focus handling); the
// highlighted state renders for both pointer hover and keyboard focus.
//
// Storm skin (SPEC-storm-language §3.2): 32px rows, radius 8, constant 6px
// content inset, 17px muted icon, and the shared interactive-label recipe
// (the UI face at textBody/600 — menuFont resolves to uiFont since the
// 2026-08-21 type pass, so a menu no longer runs a second sans face). The
// highlighted row fills stormSelection, brightens icon (bolt) and label
// (stormText), flips its keycap to bolt/panel, and grows the signature
// edge-bolt caret overhanging the row's left edge. Danger rows ink
// stormDanger throughout with a 10% fill on hover. Radio rows (flyout
// submenus) use the shared StormNode states: bolt check circle = current,
// dashed ring = other.
MenuItem {
    id: root

    // Material Symbols name shown in the leading slot ("" keeps the slot
    // for alignment so labels line up across rows).
    property string iconName: ""
    // Destructive actions read in the danger ink and hover-tint 10% danger.
    property bool danger: false
    // Accelerator keycap: text part and/or icon part (MenuKeycap).
    property string accel: ""
    property string accelIconName: ""
    // Radio-row treatment for flyout submenus (SPEC 1d). radioSelected is
    // a plain property — never AbstractButton.checked — so the owner's
    // state binding cannot be destroyed by an internal toggle.
    property bool radio: false
    property bool radioSelected: false

    readonly property bool _active: root.highlighted || root.hovered
    readonly property color _ink: !root.enabled ? AppTheme.stormTextFaint
                                  : root.danger ? AppTheme.stormDanger
                                  : root._active ? AppTheme.stormText
                                  : AppTheme.stormTextSecondary

    implicitHeight: visible ? AppTheme.menuItemHeight : 0
    padding: AppTheme.menuItemPadding
    // §3.2: constant content inset clearing the edge-bolt caret gutter —
    // never active-only, so rows don't shift under the cursor.
    leftPadding: AppTheme.menuItemPadding + 6
    topPadding: 0
    bottomPadding: 0

    Accessible.description: radio ? (radioSelected ? qsTr("Selected")
                                                   : qsTr("Not selected"))
                                  : ""

    contentItem: RowLayout {
        spacing: AppTheme.menuIconGap
        StormNode {
            visible: root.radio
            size: 16
            complete: root.radioSelected
            iconName: root.radioSelected ? "check" : ""
        }
        Icon {
            visible: !root.radio && root.iconName.length > 0
            name: root.iconName
            size: AppTheme.menuIconSize
            color: !root.enabled ? AppTheme.stormTextFaint
                   : root.danger ? AppTheme.stormDanger
                   : root._active ? AppTheme.bolt
                   : AppTheme.stormTextMuted
        }
        Label {
            Layout.fillWidth: true
            text: root.text
            elide: Label.ElideRight
            // Exactly AppButton's label recipe. A menu row and a button can
            // sit in the same popover; they used to render the same face at
            // the same size in two different weights (DemiBold vs Bold).
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            color: root.radio && !root.radioSelected && root.enabled
                   && !root._active ? AppTheme.stormTextMuted : root._ink
        }
        MenuKeycap {
            visible: root.accel.length > 0 || root.accelIconName.length > 0
            keys: root.accel
            iconName: root.accelIconName
            // §3.2 danger group: icon+label+keycap ALL stormDanger — the
            // bolt flip never applies to destructive rows.
            active: root._active && root.enabled && !root.danger
            danger: root.danger
        }
        Icon {
            visible: root.subMenu !== null && root.subMenu !== undefined
            name: "chevron_right"
            size: AppTheme.menuIconSize - 2
            color: root._active ? AppTheme.stormTextSecondary
                                : AppTheme.stormTextMuted
        }
    }

    background: Rectangle {
        radius: AppTheme.menuItemRadius
        color: root._active && root.enabled
               ? (root.danger ? AppTheme.stormDangerSoft
                              : AppTheme.stormSelection)
               : root.radio && root.radioSelected ? AppTheme.stormSelection
               : "transparent"

        // §3.2 signature cursor: the bolt caret on the highlighted row.
        // Flush with the row edge, not overhanging: QQuickMenu clips its
        // content ListView, so anything left of x=0 would be scissored
        // (the unclipped Settings nav keeps the true overhang).
        Icon {
            visible: root._active && root.enabled && !root.danger
            name: "bolt"
            size: 12
            color: AppTheme.bolt
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 0
        }
    }
}
