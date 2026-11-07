import QtQuick
import QtQuick.Controls
import MatrixClient

// The composer's Vim mode, with any pending count or operator.
Rectangle {
    id: badge
    property bool normal: false
    property string pending: ""
    property string labelObjectName: ""
    readonly property string modeText: badge.normal
        ? qsTr("NORMAL", "Vim editing mode") : qsTr("INSERT", "Vim editing mode")
    implicitWidth: label.implicitWidth + AppTheme.spacing8
    implicitHeight: label.implicitHeight + AppTheme.spacing4
    radius: AppTheme.radiusControl
    color: badge.normal ? AppTheme.accent : "transparent"
    border.width: badge.normal ? 0 : 1
    border.color: AppTheme.border
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Editing mode: %1").arg(badge.modeText)
    Label {
        id: label
        objectName: badge.labelObjectName
        anchors.centerIn: parent
        text: badge.pending.length > 0 ? badge.modeText + " " + badge.pending
                                       : badge.modeText
        textFormat: Text.PlainText
        font.family: AppTheme.monoFont
        font.pixelSize: AppTheme.scaled(10)
        font.weight: AppTheme.weightStrong
        color: badge.normal ? AppTheme.accentText : AppTheme.textMuted
    }
}
