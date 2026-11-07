import QtQuick
import MatrixClient

Item {
    id: root

    required property Item field
    property int idleMs: 4000
    // A non-blinking block over the character at the caret (Vim Normal mode).
    property bool block: false
    readonly property real blockWidth: {
        if (!root.block)
            return 0
        var _len = root.field.length
        var p = root.field.cursorPosition
        var a = root.field.positionToRectangle(p)
        var b = root.field.positionToRectangle(p + 1)
        var w = (p < _len && b.y === a.y && b.x > a.x)
            ? b.x - a.x : root.field.font.pixelSize * 0.55
        return Math.max(2, w)
    }
    readonly property bool solid: _solid
    property bool _solid: false
    signal woke()
    objectName: "idleCaret"
    visible: false
    width: 0
    height: 0

    readonly property Component delegate: Component {
        Rectangle {
            id: caret
            objectName: "idleCaretCursor"
            width: root.block ? root.blockWidth
                              : Math.max(1, Math.round(AppTheme.scaled(1.5)))
            color: root.field.color
            opacity: root.block ? 0.45 : 1
            property bool blinkOn: true
            visible: root.field.activeFocus
                     && (root.block || root._solid
                         || (root.field.cursorVisible && blinkOn))
            Timer {
                id: blink
                // cursorFlashTime is a whole on+off cycle; 0 means never blink.
                interval: Math.max(50, Qt.styleHints.cursorFlashTime / 2)
                repeat: true
                running: Qt.styleHints.cursorFlashTime > 0
                         && root.field.activeFocus
                         && root.field.cursorVisible && !root._solid
                onRunningChanged: caret.blinkOn = true
                onTriggered: caret.blinkOn = !caret.blinkOn
            }
            Connections {
                target: root
                function onWoke() {
                    caret.blinkOn = true
                    if (blink.running)
                        blink.restart()
                }
            }
        }
    }

    function wake() {
        root.woke()
        if (root._solid) {
            root._solid = false
            root.field.cursorVisible = true
        }
        if (root.field.activeFocus)
            idleTimer.restart()
    }

    Timer {
        id: idleTimer
        interval: root.idleMs
        onTriggered: {
            if (!root.field.activeFocus)
                return
            root._solid = true
            root.field.cursorVisible = false
        }
    }

    Connections {
        target: root.field
        function onTextChanged() { root.wake() }
        function onCursorPositionChanged() { root.wake() }
        function onActiveFocusChanged() {
            if (root.field.activeFocus) {
                root.wake()
            } else {
                idleTimer.stop()
                root._solid = false
            }
        }
    }
    Component.onCompleted: if (root.field.activeFocus) idleTimer.restart()
}
