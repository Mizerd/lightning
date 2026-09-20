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

    // Hard ceiling on the fit below. Menu rows carry REMOTE text — room
    // names, member names, file names — so "widen to fit" without a stop is
    // a menu as wide as whatever someone called their room. 320 is the
    // account switcher's width, the widest popover this product draws;
    // past it a row elides as it always did.
    property int menuWidthMax: 320

    // THE DESIGN WIDTH IS A FLOOR, NOT A CLAMP — and until 2026-09-20 that
    // sentence stood here over a binding that could not deliver it. It read
    // `Math.max(menuWidth, implicitContentWidth + leftPadding + rightPadding)`,
    // and a Menu's `implicitContentWidth` is the implicitWidth of its
    // contentItem, which in QtQuick.Controls.Basic is a ListView declaring
    // `implicitHeight` and NO implicitWidth. Measured on Qt 6.11.1:
    // `implicitContentWidth = 0` and `contentWidth = 0` on an open menu, so
    // the expression returned `menuWidth` unconditionally and had done since
    // it was written. What the user saw was "Mentions & keywords" rendered
    // as "Mentions & …" in the 150 px notifications flyout.
    //
    // The rows are the only thing that knows how wide the menu must be, so
    // ask them — but only while the menu is SHOWING. A QtQuick Layout skips
    // items that are not effectively visible, and every row of a closed
    // popup is invisible because the popup is, so a closed menu's rows
    // report their padding alone. `refitWidth()` therefore keeps the last
    // answer it got (the `widest > 0` guard) instead of collapsing back to
    // the design width, and the recompute rides `opened`, which the Basic
    // style emits synchronously inside `open()` — before a frame is drawn,
    // so nothing flashes at the narrow width.
    readonly property int _fitPadding: leftPadding + rightPadding
    property int fittedWidth: menuWidth
    function refitWidth() {
        var widest = 0
        for (var i = 0; i < root.count; ++i) {
            var row = root.itemAt(i)
            // ROWS vote, and nothing else does. A separator's contentItem
            // declares a 200 px implicitWidth it has no use for, and a
            // wrapped Label hosted in a menu (the notifications disclaimer)
            // reports its whole unwrapped sentence — either one would size
            // the menu to something that is not a row. `highlighted` is the
            // MenuItem property neither of them has.
            if (row && typeof row.highlighted === "boolean"
                    && row.implicitWidth > widest)
                widest = row.implicitWidth
        }
        // The ceiling clamps the FIT, never the design width: a surface
        // whose menuWidth is itself above the ceiling must not be shrunk
        // by a guard that exists to stop a room name.
        if (widest > 0)
            root.fittedWidth =
                Math.max(root.menuWidth,
                         Math.min(root.menuWidthMax,
                                  Math.ceil(widest) + root._fitPadding))
    }
    // Declared through Connections, not as `onOpened:`/`onCountChanged:`
    // handlers, so that a menu built on AppMenu can still declare its own
    // handlers for those signals without either side wondering which won.
    Connections {
        target: root
        function onOpened() { root.refitWidth() }
        function onCountChanged() { root.refitWidth() }
    }

    width: fittedWidth
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
