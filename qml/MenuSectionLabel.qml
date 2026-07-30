import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.6.5 menu language (SPEC §0): section label inside popovers and
// pickers (ROOMS, PEOPLE, FREQUENTLY USED, SHARED — N ROOMS, …).
// Storm skin (SPEC-storm-language §2): JetBrains Mono ~9.5px/600 UPPERCASE
// at .16em tracking in the faint storm ink — deliberately dim decorative
// mono, never sentence text.
Label {
    font.family: AppTheme.monoFont
    font.pixelSize: AppTheme.fontChip
    font.weight: Font.DemiBold
    font.letterSpacing: AppTheme.trackingStorm
    font.capitalization: Font.AllUppercase
    color: AppTheme.stormTextFaint
    elide: Label.ElideRight
}
