import QtQuick
import QtQuick.Controls
import MatrixClient

// A plain group label in the Channels layout — "Favourites", "Direct
// messages", "Channels".
//
// Deliberately NOT ChannelCategoryHeader. A category is a child Space: it
// collapses, and it can be opened as a room in its own right. These are
// headings over rows that came from somewhere else, so they do neither —
// giving them a chevron would offer to collapse something that has no
// Matrix identity, and giving them a tap would offer to navigate into a
// label.
Item {
    id: root

    property string label: ""

    height: 26

    // Announced as a heading so a screen reader can skip by group, which is
    // the entire point of these rows.
    Accessible.role: Accessible.Heading
    Accessible.name: root.label

    // Behind a Loader: the label is empty in the state this is created in.
    Loader {
        active: root.label.length > 0
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        sourceComponent: Label {
            // Upper-cased in the VIEW: a locale-aware upper case belongs
            // next to the font that renders it, and the real string is what
            // a screen reader should say.
            text: root.label.toUpperCase()
            elide: Text.ElideRight
            maximumLineCount: 1
            font.pixelSize: AppTheme.textMicro
            font.weight: AppTheme.weightBold
            font.letterSpacing: 0.6
            color: AppTheme.channelCategoryText
        }
    }
}
