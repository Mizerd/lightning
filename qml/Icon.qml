import QtQuick
import MatrixClient
import "IconGlyphs.js" as IconGlyphs

Text {
    textFormat: Text.PlainText
    id: root

    property string name: ""
    property int size: 18

    text: IconGlyphs.codepoints[name] || ""
    font.family: AppTheme.iconFont
    font.pixelSize: size
    color: AppTheme.textSecondary
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignHCenter
    renderType: Text.NativeRendering
}
