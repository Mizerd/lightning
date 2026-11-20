import QtQuick
import QtQuick.Controls
import MatrixClient

// An explicit ToolTip whose text is drawn as plain text. Main.qml's
// SharedToolTipGuard covers every ATTACHED ToolTip.text; an explicit
// ToolTip { } has its own contentItem, and the Basic style's is a Text in
// AutoText, so text someone else chose (a name, an emoji shortcode, a file
// name) would be rendered as markup and an <img> in it fetched on hover.
ToolTip {
    id: control
    // The same chrome SharedToolTipGuard gives the attached tooltips.
    Component.onCompleted: {
        if (background && background.radius !== undefined)
            background.radius = Qt.binding(() => AppTheme.radiusMd)
        if (background && background.border !== undefined)
            background.border.color = Qt.binding(() => AppTheme.border)
    }
    contentItem: Text {
        objectName: "plainToolTipText"
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.toolTipText
    }
}
