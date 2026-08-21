import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import MatrixClient

// v0.7: the Lightning popover menu. One flat raised surface for every
// context/action menu in the application. Items are AppMenuItem; separators
// are AppMenuSeparator. Placement clamps to the window automatically (Popup
// behaviour) and Escape/outside-click close as standard.
//
// Storm skin (SPEC-storm-language §3.1): stormPanel fill, 1px border,
// radius 12, padding 6 — theme-invariant navy in every user theme; the menu
// system is the brand moment.
//
// Elevation: the recorded deviation used to be "no shadow at all, because a
// drop shadow would inflate the popup geometry that delegate anchor maths
// depends on". That is only true of a shadow drawn ON the background item.
// The MultiEffect below is a SIBLING behind the panel, sourced from it and
// sized to it, so the popup's implicitWidth/implicitHeight are exactly what
// they were — while a menu over the emoji picker finally reads as being on
// top of it instead of pasted onto it. The border also steps up to
// stormBorderStrong, which carries the same depth cue if a platform ever
// renders this popup in its own window and clips the shadow at its edge.
//
// Optional context header (§3.1): `contextLabel` renders a bolt glyph plus a
// short wayfinding line above the first row ("Message · Sam · 13:04",
// "#design-lounge", "Notify mode").
Menu {
    id: root

    // Per-surface design width (SPEC: message 252, room 196, flyout 150).
    property int menuWidth: AppTheme.menuWidthDefault
    // Material Symbols glyph shown on the parent row when this menu is
    // nested inside another AppMenu as a flyout submenu.
    property string submenuIconName: ""
    // Context-header text; empty hides the header entirely.
    property string contextLabel: ""
    // Headers like Notify mode carry no bolt glyph (§4 2b flyout).
    property bool contextBolt: true

    readonly property int _headerHeight:
        contextLabel.length > 0 ? AppTheme.menuContextHeaderHeight : 0

    // The design width is a floor, not a clamp: menus whose rows outgrow it
    // (long labels on surfaces outside this round, translations) widen to
    // fit rather than eliding at a fixed pin.
    width: Math.max(menuWidth,
                    implicitContentWidth + leftPadding + rightPadding)
    padding: AppTheme.menuPadding
    topPadding: AppTheme.menuPadding + _headerHeight
    overlap: 0
    cascade: true

    // Rows the menu creates itself (nested Menu children) use the same
    // AppMenuItem language as explicitly declared rows.
    delegate: AppMenuItem {
        iconName: subMenu && subMenu.submenuIconName !== undefined
                  ? subMenu.submenuIconName : ""
    }

    background: Item {
        implicitWidth: root.menuWidth

        MultiEffect {
            source: menuPanel
            anchors.fill: menuPanel
            z: -1
            shadowEnabled: !AppTheme.reducedMotion
            shadowColor: AppTheme.shadowStrong
            shadowBlur: 0.8
            shadowVerticalOffset: AppTheme.elevationPopoverY
            shadowHorizontalOffset: 0
        }

        Rectangle {
            id: menuPanel
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            radius: AppTheme.menuRadius

            // The header lives on the background so the Menu's own item list
            // stays pure MenuItem content (focus and arrow keys never visit
            // it). Width-bound with a middle elide: producers pass user/room
            // controlled strings (sender names, MXID fallbacks, room names),
            // and the menu's width binding measures only its MenuItems — an
            // unbound header would paint past the panel border. Decorative
            // for a11y: every action row carries its own accessible name.
            Item {
                visible: root.contextLabel.length > 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: AppTheme.spacing8
                anchors.leftMargin: AppTheme.spacing8 + 2
                anchors.rightMargin: AppTheme.spacing8 + 2
                height: AppTheme.menuContextHeaderHeight - AppTheme.spacing8
                Accessible.ignored: true
                Icon {
                    id: contextBoltIcon
                    visible: root.contextBolt
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    name: "bolt"
                    size: 13
                    color: AppTheme.bolt
                }
                Text {
                    anchors.left: root.contextBolt ? contextBoltIcon.right
                                                   : parent.left
                    anchors.leftMargin: root.contextBolt ? AppTheme.spacing6 : 0
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.contextLabel
                    elide: Text.ElideMiddle
                    // Same treatment as MenuSectionLabel: the UI face in
                    // sentence case, not tracked mono caps. See that file for
                    // why the mono recipe left the menu head.
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                    color: AppTheme.stormTextMuted
                }
            }
        }
    }
}
