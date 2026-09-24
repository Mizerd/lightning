import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// Section label inside popovers and pickers ("Rooms", "People", "Frequently
// used"): the UI face at 12/600, sentence case (AppTheme.menuSection*). Mono is
// for keycaps, code and Matrix identifiers.
Label {
    font.family: AppTheme.menuSectionFont
    font.pixelSize: AppTheme.menuSectionSize
    font.weight: AppTheme.menuSectionWeight
    font.letterSpacing: AppTheme.menuSectionTracking
    // Muted, not faint: a readable heading, so it takes an ink that clears AA.
    color: AppTheme.stormTextMuted
    elide: Label.ElideRight
}
