import QtQuick
import MatrixClient

// Quick-react row at the top of the message context menu: 32px cells with the
// five most recent emoji and a "more" cell that opens the shared picker. A
// plain Item child of an AppMenu; QQuickMenu sizes every content row to its
// width, so this only lays out its cells and never sets its own `width`. Emoji
// literals are banned in MessageDelegate.qml/RoomDelegate.qml by IconChromeTest
// and EmojiUiContractTest; this component is their carved-out exception.
Item {
    id: root

    // Recent emoji in MRU order (app.emojiCatalog.recentEmoji, possibly empty,
    // in which case a default set is used). The first five are shown.
    property var emojis: []
    property int columns: 6
    signal picked(string emoji)
    signal morePressed()

    // The root is a focus stop the menu's Tab/arrow navigation can reach; it
    // forwards to the current cell. Left/Right move; Return/Space/Enter
    // activate.
    property int focusIndex: 0
    activeFocusOnTab: true
    onActiveFocusChanged: {
        if (activeFocus)
            root.focusCell(root.focusIndex)
    }
    function focusCell(index) {
        var item = cellRow.children[Math.max(0, Math.min(index,
                                                         root.columns - 1))]
        if (item && item.forceActiveFocus)
            item.forceActiveFocus()
    }
    Keys.onLeftPressed: {
        root.focusIndex = Math.max(0, root.focusIndex - 1)
        root.focusCell(root.focusIndex)
    }
    Keys.onRightPressed: {
        root.focusIndex = Math.min(root.columns - 1, root.focusIndex + 1)
        root.focusCell(root.focusIndex)
    }

    readonly property var _defaults: ["👍", "🔥", "❤️", "😂", "🎉"]
    readonly property var _effective:
        (root.emojis && root.emojis.length > 0)
        ? root.emojis.slice(0, 5) : root._defaults

    readonly property real _cellSize:
        root.width > 0 ? root.width / root.columns : AppTheme.emojiCellSize

    implicitWidth: root.columns * AppTheme.emojiCellSize
    implicitHeight: AppTheme.emojiCellSize + AppTheme.spacing6 + 1

    Row {
        id: cellRow
        x: 0
        y: 0
        width: root.width
        height: AppTheme.emojiCellSize

        Repeater {
            model: root.columns

            delegate: Item {
                id: cell
                required property int index
                readonly property bool isMore: index === root.columns - 1
                readonly property string emojiValue:
                    isMore ? "" : (root._effective[index] || "")
                readonly property bool cellVisible: isMore || emojiValue.length > 0

                visible: cellVisible
                width: root._cellSize
                height: AppTheme.emojiCellSize
                activeFocusOnTab: cellVisible

                Accessible.role: Accessible.Button
                Accessible.name: isMore ? qsTr("More reactions")
                                        : qsTr("React with %1").arg(emojiValue)
                Accessible.onPressAction: activate()

                function activate() {
                    if (isMore) root.morePressed()
                    else if (emojiValue.length > 0) root.picked(emojiValue)
                }

                // The emphasized cell: stormSelection fill with a
                // stormBorderStrong border.
                Rectangle {
                    anchors.fill: parent
                    radius: AppTheme.menuItemRadius
                    readonly property bool emphasized:
                        hover.hovered || cell.activeFocus
                    color: emphasized ? AppTheme.stormSelection : "transparent"
                    border.width: emphasized ? 1 : 0
                    border.color: AppTheme.stormBorderStrong
                }

                Text {
                    anchors.centerIn: parent
                    visible: !cell.isMore
                    text: cell.emojiValue
                    font.family: app.emojiFontFamily || ""
                    font.pixelSize: AppTheme.emojiGlyphSize
                }
                Icon {
                    anchors.centerIn: parent
                    visible: cell.isMore
                    name: "add"
                    size: AppTheme.menuIconSize
                    color: AppTheme.stormTextMuted
                }

                onActiveFocusChanged: {
                    if (activeFocus)
                        root.focusIndex = index
                }
                HoverHandler { id: hover }
                TapHandler { onTapped: cell.activate() }
                Keys.onReturnPressed: cell.activate()
                Keys.onEnterPressed: cell.activate()
                Keys.onSpacePressed: cell.activate()
                Keys.onLeftPressed: {
                    root.focusIndex = Math.max(0, cell.index - 1)
                    root.focusCell(root.focusIndex)
                }
                Keys.onRightPressed: {
                    root.focusIndex = Math.min(root.columns - 1,
                                               cell.index + 1)
                    root.focusCell(root.focusIndex)
                }
            }
        }
    }

    // 1px hairline under the row, 6px before the next menu row.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: cellRow.bottom
        height: 1
        color: AppTheme.stormBorder
    }
}
