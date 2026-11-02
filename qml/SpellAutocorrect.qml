import QtQuick

QtObject {
    id: ac

    property Item editor: null
    property bool active: false
    property var textFn: null
    property var skipFn: null
    property var replaceFn: null

    property var pending: null

    readonly property int maxChars: 20000
    property int _lastLength: -1
    property bool _applying: false

    function _replace(start, length, text) {
        if (ac.replaceFn) {
            ac.replaceFn(start, length, text)
        } else {
            ac.editor.remove(start, start + length)
            ac.editor.insert(start, text)
        }
    }

    function reset() {
        ac.pending = null
        ac._lastLength = ac.editor ? ac.editor.length : -1
    }

    function edited() {
        if (ac._applying || !ac.editor)
            return
        var len = ac.editor.length
        var grewByOne = ac._lastLength >= 0 && len === ac._lastLength + 1
        ac._lastLength = len
        ac.pending = null
        if (!ac.active || !grewByOne || len > ac.maxChars)
            return
        var pos = ac.editor.cursorPosition
        if (pos < 2 || ac.editor.selectionStart !== ac.editor.selectionEnd)
            return
        var typed = ac.editor.getText(pos - 1, pos)
        if (typed.length !== 1 || !app.spell.isAutocorrectBoundaryText(typed))
            return
        var text = ac.textFn ? ac.textFn() : ac.editor.text
        var fix = app.spell.autocorrection(text, pos - 1,
                                           ac.skipFn ? ac.skipFn() : [])
        if (!fix || fix.replacement === undefined || fix.replacement === "")
            return
        ac._applying = true
        ac._replace(fix.start, fix.length, fix.replacement)
        var cursor = pos + fix.replacement.length - fix.length
        ac.editor.cursorPosition = cursor
        ac._applying = false
        ac._lastLength = ac.editor.length
        ac.pending = { start: fix.start, original: fix.original,
                       replacement: fix.replacement, cursor: cursor }
    }

    function cursorMoved() {
        if (!ac._applying && ac.pending !== null && ac.editor
                && ac.editor.cursorPosition !== ac.pending.cursor)
            ac.pending = null
    }

    function handleKey(event) {
        if (ac.pending === null || !ac.editor)
            return false
        var undo = (event.key === Qt.Key_Backspace
                    && event.modifiers === Qt.NoModifier)
                || event.matches(StandardKey.Undo)
        if (!undo)
            return false
        var p = ac.pending
        ac.pending = null
        if (ac.editor.cursorPosition !== p.cursor
                || ac.editor.selectionStart !== ac.editor.selectionEnd
                || ac.editor.getText(p.start, p.start + p.replacement.length)
                   !== p.replacement)
            return false
        ac._applying = true
        ac._replace(p.start, p.replacement.length, p.original)
        ac.editor.cursorPosition = p.cursor - p.replacement.length + p.original.length
        ac._applying = false
        ac._lastLength = ac.editor.length
        app.spell.rejectAutocorrection(p.original)
        event.accepted = true
        return true
    }
}
