import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Label {
    Layout.fillWidth: true
    Layout.leftMargin: AppTheme.spacing4
    wrapMode: Text.WordWrap
    lineHeight: AppTheme.lineHeightBody
    lineHeightMode: Text.ProportionalHeight
    color: AppTheme.stormTextMuted
    font.pixelSize: AppTheme.settingsHintSize
    textFormat: Text.PlainText
}
