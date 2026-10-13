import QtQuick
import QtQuick.Controls
import MatrixClient

// "Needs <setting>" pill for a dependent settings row. Declare it as a CHILD of
// the control it explains; it shows only while that control is disabled, sits
// just after the control, and takes no layout space, so switching the setting
// it needs on or off never reflows the card.
//
//     CheckBox {
//         enabled: app.settings.closeToTray
//         SettingsNeedsBadge { needs: qsTr("Keep running in the tray") }
//     }
//
// `needs` is the visible NAME of the controlling setting, as the user reads it.
Rectangle {
    id: badge

    // The controlling setting's name, translated by the caller.
    property string needs: ""
    // The control being explained. Defaults to the parent.
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
