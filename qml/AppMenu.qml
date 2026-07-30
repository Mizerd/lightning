import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the Lightning popover menu. One flat raised surface for every
// context/action menu in the application: theme-aware background, 1px
// structural border, design radius, compact padding — no native styling,
// no bevels, no gradients. Items are AppMenuItem; separators are
// AppMenuSeparator. Placement clamps to the window automatically (Popup
// behaviour) and Escape/outside-click close as standard.
//
// v0.6.5 (SPEC §0): radius-12 container, padding 6, per-surface design
// widths, and cascading flyout submenus — a nested AppMenu renders as a
// row with a trailing chevron and opens beside its parent item. Context
// menus stay border-only by design: a drop shadow would inflate the popup
// geometry that delegate anchor maths depends on.
Menu {
    id: root

    // Per-surface design width (SPEC: message 252, room 196, flyout 150).
    property int menuWidth: AppTheme.menuWidthDefault
    // Material Symbols glyph shown on the parent row when this menu is
    // nested inside another AppMenu as a flyout submenu.
    property string submenuIconName: ""

    // The design width is a floor, not a clamp: menus whose rows outgrow it
    // (long labels on surfaces outside this round, translations) widen to
    // fit rather than eliding at a fixed pin.
    width: Math.max(menuWidth,
                    implicitContentWidth + leftPadding + rightPadding)
    padding: AppTheme.menuPadding
    overlap: 0
    cascade: true

    // Rows the menu creates itself (nested Menu children) use the same
    // AppMenuItem language as explicitly declared rows.
    delegate: AppMenuItem {
        iconName: subMenu && subMenu.submenuIconName !== undefined
                  ? subMenu.submenuIconName : ""
    }

    background: Rectangle {
        implicitWidth: root.menuWidth
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
        border.width: 1
        radius: AppTheme.menuRadius
    }
}
