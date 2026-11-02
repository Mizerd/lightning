import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

Popup {
    id: root
    objectName: "emojiCompletionPopup"

    property var completions: []
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(var entry)

    parent: Overlay.overlay
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: completions ? completions.length : 0
    readonly property int rowH: AppTheme.scaled(34)
    readonly property int headerH: AppTheme.scaled(24)
    readonly property int visibleRows: Math.max(1, Math.min(count, 7))

    width: Math.max(240, Math.min(anchorWidth, 360))
    height: headerH + visibleRows * rowH + padding * 2
    x: parent ? Math.max(AppTheme.spacing4,
                         Math.min(anchorInputTop.x,
                                  parent.width - width - AppTheme.spacing4))
              : 0
    y: {
        var above = Math.max(AppTheme.spacing4,
                             anchorInputTop.y - height - AppTheme.spacing4)
        return parent ? Math.max(AppTheme.spacing4,
                                 Math.min(above, parent.height - height
                                                 - AppTheme.spacing4))
                      : above
    }

    onCompletionsChanged: currentIndex = 0

    // Popup-level on purpose: a list scrolled under a parked pointer hands the
    // hover to another row's handler, which must not look like fresh motion.
    property point lastHoverScenePos: Qt.point(-1, -1)

    function move(delta) {
        if (count === 0)
            return
        currentIndex = (currentIndex + delta + count) % count
        list.positionViewAtIndex(currentIndex, ListView.Contain)
    }
    function accept() {
        if (currentIndex >= 0 && currentIndex < count)
            root.chosen(completions[currentIndex])
    }
    function moveUp() { move(-1) }
    function moveDown() { move(1) }

    background: Rectangle {
        objectName: "emojiCompletionPopupSurface"
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        radius: AppTheme.menuRadius
    }

    contentItem: Column {
        spacing: 0

        Label {
            objectName: "emojiCompletionPopupHeader"
            width: parent.width
            height: root.headerH
            verticalAlignment: Text.AlignVCenter
            textFormat: Text.PlainText
            elide: Label.ElideRight
            text: qsTr("Emoji")
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontChip
            font.weight: Font.DemiBold
            font.letterSpacing: AppTheme.trackingStorm
            font.capitalization: Font.AllUppercase
            color: AppTheme.stormTextFaint
        }

    ListView {
        id: list
        objectName: "emojiCompletionList"
        width: parent.width
        height: root.visibleRows * root.rowH
        model: root.completions
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: count > root.visibleRows
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Emoji suggestions")
        ScrollBar.vertical: AppScrollBar {}
        SmoothWheelArea {}
        delegate: ItemDelegate {
            id: emojiRow
            required property int index
            required property var modelData
            width: ListView.view.width
            height: root.rowH
            highlighted: index === root.currentIndex
            readonly property bool isUnicode: modelData.kind === "unicode"
            hoverEnabled: true
            onClicked: root.chosen(modelData)
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
                onPointChanged: {
                    if (!hovered)
                        return
                    var p = point.scenePosition
                    if (Math.abs(p.x - root.lastHoverScenePos.x) < 1
                            && Math.abs(p.y - root.lastHoverScenePos.y) < 1)
                        return
                    root.lastHoverScenePos = p
                    root.currentIndex = emojiRow.index
                }
            }
            Accessible.role: Accessible.ListItem
            Accessible.name: ":" + modelData.shortcode + ": "
                             + (isUnicode ? (modelData.emoji || "") + " "
                                            + (modelData.name || "")
                                          : (modelData.packName || ""))
            Accessible.selected: index === root.currentIndex
            background: Rectangle {
                radius: AppTheme.menuItemRadius
                color: emojiRow.highlighted ? AppTheme.stormSelection
                                            : "transparent"
            }
            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                Image {
                    id: completionImage
                    objectName: "emojiCompletionImage"
                    visible: !isUnicode
                    Layout.preferredWidth: visible ? 20 : 0
                    Layout.preferredHeight: 20
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    sourceSize.width: 40
                    property int resolveTick: 0
                    source: {
                        var _tick = resolveTick
                        return !isUnicode && app.mediaBridge.supported && modelData.url
                            ? app.mediaBridge.mxcImageSource(modelData.url, 40)
                            : ""
                    }
                    Connections {
                        target: app.mediaBridge
                        function onMediaCached(cacheKey) {
                            if (!isUnicode && cacheKey.endsWith(":" + modelData.url))
                                completionImage.resolveTick++
                        }
                    }
                }
                Label {
                    objectName: "emojiCompletionGlyph"
                    visible: isUnicode
                    Layout.preferredWidth: visible ? 22 : 0
                    text: isUnicode ? modelData.emoji : ""
                    font.pixelSize: AppTheme.textTitle
                    horizontalAlignment: Text.AlignHCenter
                }
                Label {
                    objectName: "emojiCompletionShortcode"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: ":" + modelData.shortcode + ":"
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    elide: Label.ElideRight
                }
                Label {
                    objectName: "emojiCompletionMeta"
                    textFormat: Text.PlainText
                    text: isUnicode ? (modelData.name || "") : (modelData.packName || "")
                    color: AppTheme.legibleInkOn(
                        AppTheme.textMuted,
                        AppTheme.flatten(emojiRow.highlighted
                                         ? AppTheme.stormSelection
                                         : "transparent", AppTheme.stormPanel))
                    font.pixelSize: AppTheme.textMeta
                    elide: Label.ElideRight
                    Layout.maximumWidth: 110
                }
                MenuKeycap {
                    objectName: "emojiCompletionKeycap"
                    Layout.alignment: Qt.AlignVCenter
                    visible: emojiRow.highlighted
                    iconName: "keyboard_return"
                    active: true
                }
            }
        }
    }
    }
}
