import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// The hairline between groups of AppMenu rows (stormBorder, 6px vertical /
// 4px horizontal margins).
MenuSeparator {
    // A hidden separator must take no space: QQuickMenu's ListView honours
    // each item's height, and MenuSeparator's height doesn't depend on
    // visibility. Same as AppMenuItem.
    implicitHeight: visible ? AppTheme.menuDividerVMargin * 2 + 1 : 0
    topPadding: AppTheme.menuDividerVMargin
    bottomPadding: AppTheme.menuDividerVMargin
    leftPadding: AppTheme.menuDividerHMargin
    rightPadding: AppTheme.menuDividerHMargin
    contentItem: Rectangle {
        implicitWidth: 200
        implicitHeight: 1
        color: AppTheme.stormBorder
    }
}
