import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the hairline between logical groups of AppMenu rows.
// v0.6.5 (SPEC §0): 6px vertical / 4px horizontal margins.
// Storm skin: stormBorder ink (§3.2 divider).
MenuSeparator {
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
