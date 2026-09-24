import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

// One row of the Lightning popover menu: a Material Symbols icon slot, a
// label, a legible disabled state and a distinct danger variant. Keyboard
// operable through the Menu's own focus handling; the highlight shows for
// both hover and keyboard focus.
//
// 32px rows, radius 8, a constant content inset, a 17px muted icon and the
// shared interactive-label style. The highlighted row fills stormSelection,
// brightens icon (bolt) and label, flips its keycap and shows the edge bolt
// caret. Danger rows use stormDanger throughout with a 10% hover fill. Radio
// rows (flyout submenus) use StormNode: bolt check circle = current, dashed
// ring = other.
MenuItem {
    id: root

    // Material Symbols name for the leading slot ("" keeps the slot so labels
    // align).
    property string iconName: ""
    // Destructive actions use the danger ink and a 10% danger hover tint.
    property bool danger: false
    // Accelerator keycap: text and/or icon (MenuKeycap).
    property string accel: ""
    property string accelIconName: ""
    // Radio-row treatment for flyout submenus. radioSelected is a plain
    // property, not AbstractButton.checked, so an internal toggle can't break
    // the owner's binding.
    property bool radio: false
    property bool radioSelected: false

    readonly property bool _active: root.highlighted || root.hovered
    readonly property color _ink: !root.enabled ? AppTheme.stormTextFaint
                                  : root.danger ? AppTheme.stormDanger
                                  : root._active ? AppTheme.stormText
                                  : AppTheme.stormTextSecondary

    implicitHeight: visible ? AppTheme.menuItemHeight : 0
    padding: AppTheme.menuItemPadding
    // Constant content inset clearing the caret gutter, so rows don't shift
    // under the cursor.
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
            // Menu rows carry remote text (room, member, file names) and
            // MenuItem has no textFormat, so this label pins plain text.
            textFormat: Text.PlainText
            elide: Label.ElideRight
            // Exactly AppButton's label style; they can share a popover.
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
            // Danger rows: icon, label and keycap all stormDanger; never the
            // bolt flip.
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

        // The bolt caret on the highlighted row, flush with the edge:
        // QQuickMenu clips its ListView, so an overhang would be cut off.
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
