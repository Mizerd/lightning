import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7: the hairline between logical groups of AppMenu rows.
MenuSeparator {
    topPadding: AppTheme.spacing4
    bottomPadding: AppTheme.spacing4
    contentItem: Rectangle {
        implicitWidth: 200
        implicitHeight: 1
        color: AppTheme.separator
    }
}
