import QtQuick
import QtQuick.Controls
import MatrixClient

Rectangle {
    id: badge

    property string needs: ""
    readonly property Item control: parent

    objectName: "settingsNeedsBadge"
    visible: !!control && !control.enabled && needs.length > 0
    x: control ? control.width + AppTheme.spacing12 : 0
    y: control ? (control.height - height) / 2 : 0
    width: label.implicitWidth + AppTheme.spacing12
    height: label.implicitHeight + AppTheme.spacing4
    radius: AppTheme.radiusPill
    color: "transparent"
    border.width: 1
    border.color: AppTheme.stormBorderStrong

    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Needs “%1”").arg(needs)

    Label {
        id: label
        anchors.centerIn: parent
        text: qsTr("Needs “%1”").arg(badge.needs)
        textFormat: Text.PlainText
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        font.weight: AppTheme.weightMedium
    }
}
