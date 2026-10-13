import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A heading over a run of settings-navigation rows ("User", "Interface",
// "Behavior", "About"). It is chrome: not focusable, not a row, and it hides
// with its rows while a search narrows the list (the owner passes `shown`).
//
// Declared in the nav column between SettingsNavRow items, so the groups are
// just the order the rows are written in; a new section joins a group by being
// declared under its label.
Label {
    id: groupLabel

    // False while a search has hidden every row of the group.
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
