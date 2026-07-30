import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.6.5 menu language (SPEC §0): section label inside popovers and
// pickers — 10px extra-bold uppercase with .08em tracking in the
// disabled ink (ROOMS, PEOPLE, FREQUENTLY USED, SHARED — N ROOMS, …).
Label {
    font.pixelSize: AppTheme.fontChip
    font.weight: Font.ExtraBold
    font.letterSpacing: AppTheme.trackingSection
    font.capitalization: Font.AllUppercase
    color: AppTheme.sectionLabelColor
    elide: Label.ElideRight
}
