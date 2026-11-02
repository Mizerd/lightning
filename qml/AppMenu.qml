import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import MatrixClient

Menu {
    id: root

    property int menuWidth: AppTheme.menuWidthDefault
    property string submenuIconName: ""
    property string contextLabel: ""
    property bool contextBolt: true

    readonly property int _headerHeight:
        contextLabel.length > 0 ? AppTheme.menuContextHeaderHeight : 0

    property int menuWidthMax: 320

    readonly property int _fitPadding: leftPadding + rightPadding
    property int fittedWidth: menuWidth
    function refitWidth() {
        var widest = 0
        for (var i = 0; i < root.count; ++i) {
            var row = root.itemAt(i)
            if (row && typeof row.highlighted === "boolean"
                    && row.implicitWidth > widest)
                widest = row.implicitWidth
        }
        if (widest > 0)
            root.fittedWidth =
                Math.max(root.menuWidth,
                         Math.min(root.menuWidthMax,
                                  Math.ceil(widest) + root._fitPadding))
    }
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
    // A Popup does not take active focus on its own, so a menu opened by
    // mouse left the keys with whatever was behind it (Return then pressed a
    // background button). With focus the menu drives Up/Down/Return itself;
    // no row is pre-highlighted, as in native menus.
    focus: true

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
            color: AppTheme.popoverSurface
            border.color: AppTheme.menuBorder
            border.width: 1
            radius: AppTheme.menuRadius

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
