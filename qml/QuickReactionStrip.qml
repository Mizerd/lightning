import QtQuick
import QtQuick.Controls
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
    // Tab/Backtab walk the VISIBLE cells and then leave the strip. The cells
    // are not tab stops of their own and the root stops being one while a cell
    // holds focus: otherwise Qt's traversal from a cell finds the root as the
    // neighbouring stop and the root hands focus straight back to the same
    // cell (at either end of the strip, so Tab/Backtab were a trap).
    property bool cellFocused: false
    activeFocusOnTab: !root.cellFocused
    function cellVisibleAt(i) {
        var c = cellRow.children[i]
        return !!c && c.visible
    }
    function stepFocus(delta) {
        var next = root.focusIndex + delta
        while (next >= 0 && next <= root.columns - 1
               && !root.cellVisibleAt(next))
            next += delta
        if (next < 0 || next > root.columns - 1)
            return false
        root.focusIndex = next
        root.focusCell(next)
        return true
    }
    function edgeFocus(first) {
        var i = first ? 0 : root.columns - 1
        while (i >= 0 && i <= root.columns - 1 && !root.cellVisibleAt(i))
            i += first ? 1 : -1
        if (i < 0 || i > root.columns - 1)
            return
        root.focusIndex = i
        root.focusCell(i)
    }
    Keys.onLeftPressed: root.stepFocus(-1)
    Keys.onRightPressed: root.stepFocus(1)
    Keys.onTabPressed: (event) => { event.accepted = root.stepFocus(1) }
    Keys.onBacktabPressed: (event) => { event.accepted = root.stepFocus(-1) }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Home) {
            root.edgeFocus(true)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            root.edgeFocus(false)
            event.accepted = true
        }
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
                objectName: "quickReactionCell"
                required property int index
                readonly property bool tipVisible: tip.visible
                readonly property string tipText: tip.text
                readonly property bool isMore: index === root.columns - 1
                readonly property string emojiValue:
                    isMore ? "" : (root._effective[index] || "")
                readonly property bool cellVisible: isMore || emojiValue.length > 0

                visible: cellVisible
                width: root._cellSize
                height: AppTheme.emojiCellSize

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
                    if (activeFocus) {
                        root.focusIndex = index
                        root.cellFocused = true
                    } else if (root.focusIndex === index) {
                        root.cellFocused = false
                    }
                }
                HoverHandler { id: hover }
                TapHandler { onTapped: cell.activate() }
                Keys.onReturnPressed: cell.activate()
                Keys.onEnterPressed: cell.activate()
                Keys.onSpacePressed: cell.activate()
                // Key events a cell does not accept bubble to the root's
                // handlers above (Left/Right/Tab/Backtab/Home/End).
                ToolTip {
                    id: tip
                    visible: hover.hovered || cell.activeFocus
                    delay: hover.hovered ? 500 : 0
                    text: cell.Accessible.name
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
