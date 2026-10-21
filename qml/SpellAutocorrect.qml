import QtQuick

// Autocorrect for one text editor (the room composer and the thread composer,
// each in plain and rich mode). The policy -- what counts as a clear typo,
// and what is never touched (code, URLs, mentions, shortcodes, commands) -- is
// SpellChecker::autocorrection in C++; this item only watches the editor:
//
//  * it asks only when exactly ONE character was added and that character is
//    an autocorrect boundary (whitespace or , ; ! ?), so a paste, a draft
//    restore or an IME commit of several characters is never corrected;
//  * a correction can be undone with Backspace or Ctrl+Z as the very next
//    key, which restores the typed word, keeps the boundary character and
//    tells the checker not to correct that word again this session;
//  * any other edit or caret move ends the undo window.
//
// The owner calls edited() from onTextChanged, cursorMoved() from
// onCursorPositionChanged and handleKey(event) first thing in Keys.onPressed.
QtObject {
    id: ac

    // The TextArea this instance watches.
    property Item editor: null
    // Bound by the owner: spell checking available and on, autocorrect on,
    // and this editor the one in use.
    property bool active: false
    // () => the text the checker sees (rich: the document's plain text).
    property var textFn: null
    // () => ranges never to touch (mention pills; rich: code too).
    property var skipFn: null
    // (start, length, replacement): replaces a range keeping its format. The
    // default is a plain remove + insert.
    property var replaceFn: null

    // The last correction while it can still be undone:
    // {start, original, replacement, cursor}; null otherwise.
    property var pending: null

    // SpellChecker::kMaxCheckedChars: autocorrect never scans more.
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

    // Forget the editor's previous length (a mode switch, a draft load).
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
        // Any edit other than our own ends the undo window.
        ac.pending = null
        if (!ac.active || !grewByOne || len > ac.maxChars)
            return
        var pos = ac.editor.cursorPosition
        if (pos < 2 || ac.editor.selectionStart !== ac.editor.selectionEnd)
            return
        // Cheap test first: only a boundary character can finish a word.
        // (\s covers the rich editor's paragraph and line separators.)
        var typed = ac.editor.getText(pos - 1, pos)
        if (typed.length !== 1 || !(/\s/.test(typed) || ",;!?".indexOf(typed) >= 0))
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

    // True when the key undid the last correction (and was accepted).
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
        // The boundary character the user typed stays where it was.
        ac.editor.cursorPosition = p.cursor - p.replacement.length + p.original.length
        ac._applying = false
        ac._lastLength = ac.editor.length
        app.spell.rejectAutocorrection(p.original)
        event.accepted = true
        return true
    }
}
