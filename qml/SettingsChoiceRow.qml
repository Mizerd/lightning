import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

GridLayout {
    id: row

    property alias label: title.text
    default property alias content: slot.data
    property int labelWidth: 176
    property int stackBelow: 560

    Layout.fillWidth: true
    columns: width >= row.stackBelow ? 2 : 1
    columnSpacing: AppTheme.spacing16
    rowSpacing: AppTheme.spacing4

    Label {
        id: title
        Layout.preferredWidth: row.columns === 2 ? row.labelWidth : -1
        Layout.fillWidth: row.columns === 1
        Layout.alignment: Qt.AlignTop
        topPadding: row.columns === 2 ? AppTheme.spacing8 : 0
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: AppTheme.settingsText
        font.pixelSize: AppTheme.settingsOptionSize
        font.weight: AppTheme.weightStrong
    }
    ColumnLayout {
        id: slot
        Layout.fillWidth: true
        spacing: AppTheme.spacing4
    }
}
