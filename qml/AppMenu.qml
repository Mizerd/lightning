import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the Lightning popover menu. One flat raised surface for every
// context/action menu in the application. Items are AppMenuItem; separators
// are AppMenuSeparator. Placement clamps to the window automatically (Popup
// behaviour) and Escape/outside-click close as standard.
//
// Storm skin (SPEC-storm-language §3.1): stormPanel fill, 1px stormBorder,
// radius 12, padding 6 — theme-invariant navy in every user theme; the menu
// system is the brand moment. Context menus stay border-only by design: a
// drop shadow would inflate the popup geometry that delegate anchor maths
// depends on (recorded deviation from the mock's 0 24px 60px shadow).
//
// Optional mono context header (§3.1): `contextLabel` renders an
// outline-bolt + JetBrains Mono UPPERCASE line above the first row
// ("MESSAGE · SAM · 13:04", "#DESIGN-LOUNGE", "NOTIFY MODE").
Menu {
    id: root

    // Per-surface design width (SPEC: message 252, room 196, flyout 150).
    property int menuWidth: AppTheme.menuWidthDefault
    // Material Symbols glyph shown on the parent row when this menu is
    // nested inside another AppMenu as a flyout submenu.
    property string submenuIconName: ""
    // Mono context-header text; empty hides the header entirely.
    property string contextLabel: ""
    // Headers like NOTIFY MODE carry no bolt glyph (§4 2b flyout).
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

    background: Rectangle {
        implicitWidth: root.menuWidth
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        radius: AppTheme.menuRadius

        // The header lives on the background so the Menu's own item list
        // stays pure MenuItem content (focus and arrow keys never visit it).
        // Width-bound with a middle elide: producers pass user/room
        // controlled strings (sender names, MXID fallbacks, room names), and
        // the menu's width binding measures only its MenuItems — an unbound
        // header would paint past the panel border. Decorative for a11y:
        // every action row carries its own accessible name.
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
                anchors.leftMargin: root.contextBolt ? AppTheme.spacing8 : 0
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.contextLabel
                elide: Text.ElideMiddle
                font.family: AppTheme.monoFont
                // font.pixelSize is int — the mock's 9.5 rounds up.
                font.pixelSize: AppTheme.fontChip
                font.weight: Font.DemiBold
                font.letterSpacing: AppTheme.trackingStorm
                font.capitalization: Font.AllUppercase
                color: AppTheme.stormTextMuted
            }
        }
    }
}
