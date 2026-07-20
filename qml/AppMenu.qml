import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the Lightning popover menu. One flat raised surface for every
// context/action menu in the application: theme-aware background, 1px
// structural border, design radius, compact padding — no native styling,
// no bevels, no gradients. Items are AppMenuItem; separators are
// AppMenuSeparator. Placement clamps to the window automatically (Popup
// behaviour) and Escape/outside-click close as standard.
Menu {
    id: root

    padding: AppTheme.spacing4
    overlap: 0

    background: Rectangle {
        implicitWidth: 220
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
        border.width: 1
        radius: AppTheme.radiusMd
    }
}
