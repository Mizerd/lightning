import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A one-to-two line note directly under a toggle that sends something
// somewhere, or tells a server something. Longer reasoning belongs in the
// section's own paragraph; this says what the switch does to you, in the place
// you are looking. Muted body ink, never a warning colour: it informs.
Label {
    Layout.fillWidth: true
    Layout.leftMargin: AppTheme.spacing4
    wrapMode: Text.WordWrap
    lineHeight: AppTheme.lineHeightBody
    lineHeightMode: Text.ProportionalHeight
    color: AppTheme.stormTextMuted
    font.pixelSize: AppTheme.textMeta
    textFormat: Text.PlainText
}
