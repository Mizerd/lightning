import QtQuick
import MatrixClient

// A text caret that stops blinking when the reader has gone quiet.
//
// A focused text field blinks its caret for as long as it has focus, and every
// blink is a frame: the composer takes focus when a room opens, so an idle
// window repainted about four times a second for as long as it was open
// (measured offscreen, 2026-10-26, and 2 Hz in a bare TextArea). After
// `idleMs` without typing, a cursor move or a focus change, the field's own
// blinking is switched off (`cursorVisible = false` stops its timer) and a
// SOLID caret is drawn instead, so nothing animates. Any of those events
// brings the normal caret back.
//
// Use: `cursorDelegate: idleCaret.delegate` and `IdleCaret { id: idleCaret;
// field: theTextArea }` inside the field.
Item {
    id: root

    required property Item field
    property int idleMs: 4000
    readonly property bool solid: _solid
    property bool _solid: false
    visible: false
    width: 0
    height: 0

    readonly property Component delegate: Component {
        Rectangle {
            width: Math.max(1, Math.round(AppTheme.scaled(1.5)))
            color: root.field.color
            // Drawn only in a focused field; blinking (or solid after idle).
            visible: root.field.activeFocus
                     && (root._solid || root.field.cursorVisible)
        }
    }

    function wake() {
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
            // Stops the field's own blink timer: no more frames.
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
