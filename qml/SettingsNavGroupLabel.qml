import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Label {
    id: groupLabel

    property bool shown: true

    visible: shown
    Layout.fillWidth: true
    Layout.leftMargin: AppTheme.spacing8
    Layout.topMargin: AppTheme.spacing8
    Layout.bottomMargin: AppTheme.spacing2
    elide: Label.ElideRight
    textFormat: Text.PlainText
    color: AppTheme.stormTextMuted
    font.family: AppTheme.menuSectionFont
    font.pixelSize: AppTheme.menuSectionSize
    font.weight: AppTheme.menuSectionWeight
    font.letterSpacing: AppTheme.menuSectionTracking
    Accessible.role: Accessible.Heading
    Accessible.name: text
}
