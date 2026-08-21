import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// Section label inside popovers and pickers (Rooms, People, Frequently used,
// Shared — 3 rooms, …).
//
// It used to be JetBrains Mono at 10px/600, ALL CAPS, 1.6px tracking. That is
// HUD typography carrying wayfinding text: slow to read at 10px, the loudest
// thing in a menu whose actual content ("Reply", "Copy text") was set quieter
// in a different face, and applied on the light themes too where the Storm
// language was never meant to go. Three faces inside one 60px menu head is
// what the 2026-08-21 report means by "the font looks out of place".
//
// It is now the UI face at 12/600 in sentence case (AppTheme.menuSection*).
// Mono is kept for what is genuinely monospaced: keycaps (MenuKeycap), code
// (CodeBlock) and Matrix identifiers. The brand moment in the menu system is
// the bolt glyph and the navy panel, not the letterspacing.
Label {
    font.family: AppTheme.menuSectionFont
    font.pixelSize: AppTheme.menuSectionSize
    font.weight: AppTheme.menuSectionWeight
    font.letterSpacing: AppTheme.menuSectionTracking
    // Muted, not faint: at 12px sentence case this is a readable heading
    // rather than a decorative rule, so it takes the ink that clears AA.
    color: AppTheme.stormTextMuted
    elide: Label.ElideRight
}
