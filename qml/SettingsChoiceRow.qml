import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One setting on one line: its name in a fixed left column, the control and
// any hint stacked on the right. Replaces a heading row above each control
// (Settings > Layout), which spent a whole line on a two-word label.
// Below `stackBelow` the label goes back above the control, so a narrow
// window never squeezes the control into the leftover width.
GridLayout {
    id: row

    property alias label: title.text
    /// The control and its hints.
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
        // Level with the text inside the control beside it.
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
