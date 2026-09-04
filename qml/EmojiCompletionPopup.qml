import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

// MSC2545 shortcode completion: type `:blob` and the installed packs offer
// their matches. Same construction as SlashCommandPopup and MentionPopup — a
// surface floating above the composer that deliberately never takes focus,
// so the editor keeps the caret and forwards Up/Down/Tab/Return/Escape.
//
// The model is MessageComposer.emojiCompletionsAt(cursor), which is
// cursor-driven rather than a NOTIFY property: a slash command is always at
// position 0, while a shortcode can be anywhere, so the answer depends on
// where the caret is and only QML knows that.
Popup {
    id: root
    objectName: "emojiCompletionPopup"

    property var completions: []
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(string shortcode)

    parent: Overlay.overlay
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: completions ? completions.length : 0
    readonly property int rowH: AppTheme.scaled(34)
    readonly property int visibleRows: Math.max(1, Math.min(count, 7))

    width: Math.max(240, Math.min(anchorWidth, 360))
    height: visibleRows * rowH + padding * 2
    x: parent ? Math.max(AppTheme.spacing4,
                         Math.min(anchorInputTop.x,
                                  parent.width - width - AppTheme.spacing4))
              : 0
    y: Math.max(AppTheme.spacing4, anchorInputTop.y - height - AppTheme.spacing4)

    onCompletionsChanged: currentIndex = 0

    function move(delta) {
        if (count === 0)
            return
        currentIndex = (currentIndex + delta + count) % count
        list.positionViewAtIndex(currentIndex, ListView.Contain)
    }
    function accept() {
        if (currentIndex >= 0 && currentIndex < count)
            root.chosen(completions[currentIndex].shortcode)
    }
    function moveUp() { move(-1) }
    function moveDown() { move(1) }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        border.width: 1
        radius: AppTheme.radiusMd
    }

    contentItem: ListView {
        id: list
        model: root.completions
        clip: true
        interactive: count > root.visibleRows
        ScrollBar.vertical: AppScrollBar {}
        delegate: ItemDelegate {
            required property int index
            required property var modelData
            width: ListView.view.width
            height: root.rowH
            highlighted: index === root.currentIndex
            onClicked: root.chosen(modelData.shortcode)
            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                // The image itself, resolved through the authenticated media
                // path exactly as the timeline resolves one.
                Image {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    sourceSize.width: 40
                    source: app.mediaBridge.supported && modelData.url
                            ? app.mediaBridge.mxcImageSource(modelData.url, 40)
                            : ""
                }
                Label {
                    Layout.fillWidth: true
                    // A shortcode is remote text from a pack: never markup.
                    textFormat: Text.PlainText
                    text: ":" + modelData.shortcode + ":"
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                    elide: Label.ElideRight
                }
                Label {
                    textFormat: Text.PlainText
                    text: modelData.packName || ""
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                    elide: Label.ElideRight
                    Layout.maximumWidth: 110
                }
            }
        }
    }
}
