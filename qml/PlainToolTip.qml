import QtQuick
import QtQuick.Controls

// An explicit ToolTip whose text is drawn as plain text. Main.qml's
// SharedToolTipGuard covers every ATTACHED ToolTip.text; an explicit
// ToolTip { } has its own contentItem, and the Basic style's is a Text in
// AutoText, so text someone else chose (a name, an emoji shortcode, a file
// name) would be rendered as markup and an <img> in it fetched on hover.
ToolTip {
    id: control
    contentItem: Text {
        objectName: "plainToolTipText"
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.toolTipText
    }
}
