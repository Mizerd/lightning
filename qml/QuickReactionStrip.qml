import QtQuick
import MatrixClient

// v0.6.5 (SPEC 1a): the quick-react row at the top of the message context
// menu — a 6-column grid of 32px emoji cells (the first 5 recently used
// emoji, plus a trailing "more" cell that opens the full shared picker).
//
// This is a plain Item, not an AppMenuItem: it is added directly as a child
// of an AppMenu, and QQuickMenu resizes every content row (MenuItem,
// MenuSeparator, or a plain Item like this one) to the menu's own content
// width, so this component only lays out its own cells across whatever
// width it is given — it never sets its own `width`.
//
// Emoji literals are banned in MessageDelegate.qml/RoomDelegate.qml by
// contract tests (IconChromeTest's chrome scan, EmojiUiContractTest); this
// component is exactly the emoji-specific exception those tests carve out.
Item {
    id: root

    // Recent-emoji source in MRU order (callers pass
    // app.emojiCatalog.recentEmoji, which may be absent or empty — this
    // component supplies its own default set in that case). Only the first
    // 5 are shown; the trailing cell always opens the full picker.
    property var emojis: []
    property int columns: 6
    signal picked(string emoji)
    signal morePressed()

    // Keyboard contract: the strip replaced the arrow-reachable "React"
    // MenuItem, so the ROOT must be a focus stop the menu's own Tab/arrow
    // machinery can land on. Focus on the root forwards to the current
    // cell; Left/Right move it; Return/Space/Enter activate it.
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

                // Storm §4 2a: the emphasized cell fills stormSelection with
                // a stormBorderStrong border — the same selected-cell
                // treatment as the mock's 🔥 cell.
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
                    font.families: AppTheme.emojiFontFamilies
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

    // 1px hairline under the row, 6px gap before the next menu row.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: cellRow.bottom
        height: 1
        color: AppTheme.stormBorder
    }
}
