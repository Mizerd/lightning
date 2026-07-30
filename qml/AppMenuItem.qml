import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

// v0.7: one menu row of the Lightning popover menu — Material Symbols
// icon slot, text label, soft hover/focus tint, legible disabled state,
// and a visually distinct danger variant for destructive actions. Rows
// stay keyboard-operable (arrow keys + Return through the Menu's own
// focus handling); the highlighted state renders for both pointer hover
// and keyboard focus.
//
// v0.6.5 (SPEC §0): 32px rows, radius 8, 18px muted icon, accentSoft
// highlight with selectedText ink, an accelerator keycap slot, radio
// rows for flyout submenus, and a trailing chevron on submenu parents.
MenuItem {
    id: root

    // Material Symbols name shown in the leading slot ("" keeps the slot
    // for alignment so labels line up across rows).
    property string iconName: ""
    // Destructive actions read in the danger ink and hover-tint dangerSoft.
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
    readonly property color _ink: !root.enabled ? AppTheme.textDisabled
                                  : root.danger ? AppTheme.dangerInk
                                  : root._active ? AppTheme.selectedText
                                  : AppTheme.textPrimary

    implicitHeight: visible ? AppTheme.menuItemHeight : 0
    padding: AppTheme.menuItemPadding
    topPadding: 0
    bottomPadding: 0

    Accessible.description: radio ? (radioSelected ? qsTr("Selected")
                                                   : qsTr("Not selected"))
                                  : ""

    contentItem: RowLayout {
        spacing: AppTheme.menuIconGap
        Icon {
            name: root.radio ? (root.radioSelected ? "radio_button_checked"
                                                   : "radio_button_unchecked")
                             : root.iconName
            size: AppTheme.menuIconSize
            visible: root.radio || root.iconName.length > 0
            color: !root.enabled ? AppTheme.textDisabled
                   : root.danger ? AppTheme.dangerInk
                   : root.radio && root.radioSelected ? AppTheme.accent
                   : root._active ? AppTheme.selectedText
                   : AppTheme.icon
        }
        Label {
            Layout.fillWidth: true
            text: root.text
            elide: Label.ElideRight
            font.pixelSize: AppTheme.fontSecondary
            font.weight: Font.DemiBold
            color: root.radio && !root.radioSelected && root.enabled
                   && !root._active ? AppTheme.textSecondary : root._ink
        }
        MenuKeycap {
            visible: root.accel.length > 0 || root.accelIconName.length > 0
            keys: root.accel
            iconName: root.accelIconName
        }
        Icon {
            visible: root.subMenu !== null && root.subMenu !== undefined
            name: "chevron_right"
            size: AppTheme.menuIconSize - 2
            color: root._active ? AppTheme.selectedText : AppTheme.textMuted
        }
    }

    background: Rectangle {
        radius: AppTheme.menuItemRadius
        color: root._active && root.enabled
               ? (root.danger ? AppTheme.dangerSoft : AppTheme.accentSoft)
               : root.radio && root.radioSelected ? AppTheme.accentSoft
               : "transparent"
    }
}
