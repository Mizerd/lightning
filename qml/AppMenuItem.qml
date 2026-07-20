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
MenuItem {
    id: root

    // Material Symbols name shown in the leading slot ("" keeps the slot
    // for alignment so labels line up across rows).
    property string iconName: ""
    // Destructive actions read in the danger colour and hover-tint red.
    property bool danger: false

    implicitHeight: visible ? 34 : 0
    padding: AppTheme.spacing8

    contentItem: RowLayout {
        spacing: AppTheme.spacing8
        Icon {
            name: root.iconName
            size: 16
            visible: root.iconName.length > 0
            color: !root.enabled ? AppTheme.textMuted
                   : root.danger ? AppTheme.danger
                   : AppTheme.textSecondary
        }
        Label {
            Layout.fillWidth: true
            text: root.text
            elide: Label.ElideRight
            font.pixelSize: AppTheme.fontSecondary
            color: !root.enabled ? AppTheme.textMuted
                   : root.danger ? AppTheme.danger
                   : AppTheme.textPrimary
        }
    }

    background: Rectangle {
        radius: AppTheme.radiusSm
        color: root.highlighted || root.hovered
               ? (root.danger ? Qt.alpha(AppTheme.danger, 0.12)
                              : AppTheme.hover)
               : "transparent"
    }
}
