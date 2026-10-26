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
// The blink itself is ours: a field with a `cursorDelegate` never blinks it
// (Qt's own blink only drives the cursor it paints), so a delegate bound to
// `cursorVisible` alone is a caret that never blinks at all -- measured on a
// private Xvfb, 2026-10-26: the composer's caret was identical in every frame
// while the search field beside it blinked. The delegate toggles at the
// platform's cursor flash rate while the field is active and not idle.
//
// That rate is also the app's "stop the cursor blinking" preference:
// AppController sets Qt.styleHints.cursorFlashTime to 0 while it is on (and
// back to the platform's value when it is off), so reading the hint honours
// it with no second setting -- preference on: one steady caret, always;
// off: blink while active, then the solid caret after `idleMs`. The hint is
// NOTIFYable, so a change applies at once.
//
// Use: `cursorDelegate: idleCaret.delegate` and `IdleCaret { id: idleCaret;
// field: theTextArea }` inside the field.
Item {
    id: root

    required property Item field
    property int idleMs: 4000
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
            width: Math.max(1, Math.round(AppTheme.scaled(1.5)))
            color: root.field.color
            property bool blinkOn: true
            // Drawn only in a focused field; blinking (or solid after idle).
            visible: root.field.activeFocus
                     && (root._solid || (root.field.cursorVisible && blinkOn))
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
                // Activity shows the caret at once and restarts the cycle, as
                // a field's own caret does on a keystroke.
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
