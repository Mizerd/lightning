import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the hairline between logical groups of AppMenu rows.
// v0.6.5 (SPEC §0): 6px vertical / 4px horizontal margins.
// Storm skin: stormBorder ink (§3.2 divider).
MenuSeparator {
    // A HIDDEN separator must take no space. QQuickMenu lays its rows out in
    // a ListView that honours each item's own height, and MenuSeparator's
    // height comes from its contentItem plus padding whether it is visible or
    // not — so a group divider that belongs to rows this menu is not showing
    // left a 13px band behind. That is the "empty space at the top" of the
    // rail's Space menu: its first divider belongs to the folder-only rows,
    // which a real Space never shows. AppMenuItem already does exactly this.
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
