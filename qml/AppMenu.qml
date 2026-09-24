import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import MatrixClient

// The Lightning popover menu, used for every context/action menu. Items are
// AppMenuItem, separators AppMenuSeparator. Popup clamps placement to the
// window; Escape and outside clicks close.
//
// The shadow is a MultiEffect sibling behind the panel, so it doesn't change
// the popup's implicit size (delegate anchor maths depend on it).
//
// Optional context header: `contextLabel` shows a bolt glyph and a short
// wayfinding line above the first row ("Message · Sam · 13:04").
Menu {
    id: root

    // Per-surface design width (message 252, room 196, flyout 150).
    property int menuWidth: AppTheme.menuWidthDefault
    // Glyph shown on the parent row when nested as a flyout submenu.
    property string submenuIconName: ""
    // Context-header text; empty hides the header.
    property string contextLabel: ""
    // Some headers (e.g. Notify mode) carry no bolt glyph.
    property bool contextBolt: true

    readonly property int _headerHeight:
        contextLabel.length > 0 ? AppTheme.menuContextHeaderHeight : 0

    // Ceiling on the fit below: rows carry remote text (room, member and file
    // names), so widening without a stop is unbounded. Past it rows elide.
    property int menuWidthMax: 320

    // The design width is a floor, not a clamp. Basic's Menu reports no
    // implicit content width (its ListView has none), so the rows are measured
    // directly. Only while showing: a closed popup's rows are invisible and
    // report padding only, so the last answer is kept (`widest > 0`). `opened`
    // fires synchronously inside open(), before a frame, so nothing flashes.
    readonly property int _fitPadding: leftPadding + rightPadding
    property int fittedWidth: menuWidth
    function refitWidth() {
        var widest = 0
        for (var i = 0; i < root.count; ++i) {
            var row = root.itemAt(i)
            // Only rows vote (`highlighted` is a MenuItem property): separators
            // and hosted wrapping labels report widths they don't need.
            if (row && typeof row.highlighted === "boolean"
                    && row.implicitWidth > widest)
                widest = row.implicitWidth
        }
        // The ceiling clamps the fit, never the design width.
        if (widest > 0)
            root.fittedWidth =
                Math.max(root.menuWidth,
                         Math.min(root.menuWidthMax,
                                  Math.ceil(widest) + root._fitPadding))
    }
    // Via Connections, so menus built on AppMenu can still declare their
    // own onOpened/onCountChanged handlers.
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

    // Rows the menu creates itself (nested Menus) use AppMenuItem too.
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

            // The header lives on the background so focus and arrow keys never
            // visit it. Bound to the panel width with a middle elide, since it
            // shows user/room-controlled strings and the width fit only
            // measures rows. Ignored by accessibility; every row has its own
            // name.
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
                    // Same treatment as MenuSectionLabel.
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
