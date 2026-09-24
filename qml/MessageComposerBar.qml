import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// The main message composer: one card at the bottom of the timeline with an
// optional formatting toolbar above the input row. The toolbar edits real
// markdown over the selection (MessageComposer.toggleFormat); the Rust send
// path parses it at send time. Enter sends, Shift+Enter inserts a newline
// (configurable).
Item {
    id: root
    focus: false
    implicitHeight: composerCol.implicitHeight + AppTheme.spacing16
                    + AppTheme.spacing4

    // The formatting toolbar is collapsed by default and opens from the format
    // toggle. Format shortcuts work regardless.
    property bool toolbarExpanded: false

    // Below this width the optional controls (format toggle, emoji, GIF) move
    // into the attach menu so the text field keeps a usable width. Measured
    // against this bar's width, not the input row's, so hiding a child cannot
    // feed back into the test.
    readonly property bool compactInputRow: root.width > 0 && root.width < 460

    // The formatting row's own threshold. Its controls cannot shrink (fixed
    // icon buttons plus the mode chip, whose label does not elide), so below
    // this the chip would paint outside the card. One number for both modes so
    // the chip does not flicker on a mode switch; inside compactInputRow's 460
    // so the overflow menu carrying the action is on screen. Uses the bar's
    // width to avoid a binding loop. theModeChipNeverPaintsOutsideTheCard pins
    // it.
    readonly property bool compactToolbarRow: root.width > 0 && root.width < 420

    // A voice recording is in progress. Derived from app.voiceOwner, never
    // assigned: the recorder is shared with the thread composer, and a local
    // flag could let both believe they own it and send twice.
    readonly property bool voiceActive: app.voiceOwner === "room"

    // Transient validation feedback ("folder rejected", "too large", …).
    property string attachmentNotice: ""
    property int emojiSelectionStart: 0
    property int emojiSelectionEnd: 0
    property int emojiCursorPosition: 0

    // Active-state flags for the toolbar chips, from MarkdownFormat::state.
    property var formatFlags: ({})
    // The voice button's action, shared with the compact-row menu.
    function startVoiceMessage() {
            // Report failure from the return value: the failure Connections
            // only arm once this composer owns the recorder. A press while the
            // thread composer is recording is refused, never stolen.
            if (!app.startVoiceRecording("room")) {
                // "Already recording" is not "unavailable".
                root.attachmentNotice =
                    app.voiceRecordingBusy()
                    ? qsTr("A recording is already in "
                           + "progress.")
                    : qsTr("Voice recording is unavailable.")
                noticeTimer.restart()
            }
    }

    function focusStagedAttachmentSend() {
        // Focus the input, so Return takes the same path as a click. A closure
        // rather than a bare method reference, which Qt.callLater would call
        // unbound.
        if (app.composer.hasAttachments && app.composer.canSend)
            Qt.callLater(function () { root.focusEditor() })
    }
    // Which modifier sends is a setting: by default Enter sends and Shift+Enter
    // inserts a newline; inverted, Ctrl+Enter sends. One predicate for every
    // Return/Enter path in this file.
    function _returnShouldSend(modifiers) {
        if (app.settings.enterInsertsNewline)
            return (modifiers & Qt.ControlModifier) !== 0
        return (modifiers & Qt.ShiftModifier) === 0
    }
    Keys.onReturnPressed: (event) => {
        if (!root._returnShouldSend(event.modifiers)
                || !app.composer.hasAttachments
                || !app.composer.canSend) {
            event.accepted = false
            return
        }
        event.accepted = true
        root.submitComposer()
    }
    Keys.onEnterPressed: (event) => {
        if (!root._returnShouldSend(event.modifiers)
                || !app.composer.hasAttachments
                || !app.composer.canSend) {
            event.accepted = false
            return
        }
        event.accepted = true
        root.submitComposer()
    }
    function refreshFormatState() {
        if (root.richMode) {
            formatFlags = app.richComposer.formatState(richInput.textDocument,
                                                       richInput.selectionStart,
                                                       richInput.selectionEnd)
            return
        }
        formatFlags = app.composer.formatState(input.text,
                                               input.selectionStart,
                                               input.selectionEnd)
    }
    // Editor-context shortcuts. Qt sends ShortcutOverride to the focus item
    // first; accepting it turns the shortcut into a key press here, so Ctrl+B
    // is Bold while this box has focus and keeps its global meaning elsewhere.
    // Only claim combinations we handle, or this box would swallow every global
    // shortcut. The registry's EditorContext flag decides (see
    // ShortcutRegistry).
    function _composerFormatFor(key, modifiers) {
        return app.shortcuts.editorActionForKey(key, modifiers)
    }
    // Composer modes: "markdown" is the source editor (`input`); "rich" is the
    // WYSIWYG editor (`richInput`), whose document is the message and whose
    // wire bodies come from app.richComposer. Both editors exist and are
    // visibility-exclusive. The composer text is the markdown mirror in both
    // modes: the rich editor pushes toMarkdown() on every edit, so canSend,
    // typing notices, drafts and slash commands behave identically and a mode
    // switch preserves the draft.
    readonly property bool richMode: app.settings
                                     && app.settings.composerMode === "rich"
    property bool richSyncing: false
    function activeInput() { return root.richMode ? richInput : input }
    function submitComposer() {
        if (root.richMode)
            app.richComposer.sendDocument(richInput.textDocument)
        else
            app.composer.send()
    }
    // The composed message without sending, for the scheduler: expanded
    // markdown or the serialized rich document.
    readonly property int pendingScheduledCount: {
        if (!app.scheduledSends || app.currentRoomId === "")
            return 0
        var tick = app.scheduledSends.pendingCount // dependency
        return app.scheduledSends.pendingForRoom(app.currentRoomId).length
    }
    SendLaterDialog { id: sendLaterDialog }
    // The room's pending scheduled messages.
    function openScheduledList() {
        sendLaterDialog.openFor({})
    }
    function openSendLater() {
        if (!app.composer.canSend || app.composer.text.trim().length === 0) {
            sendLaterDialog.openFor({})
            return
        }
        var snapshot = app.composer.composedMessage()
        if (root.richMode) {
            var composed = app.richComposer.composeDocument(richInput.textDocument)
            snapshot.body = composed.body
            snapshot.html = composed.html
            snapshot.mentionIds = composed.mentionIds
        }
        sendLaterDialog.openFor(snapshot)
    }
    onRichModeChanged: {
        // The other editor's ranges belong to the other document.
        root.spellUnderlines = []
        root.richSpellUnderlines = []
        root.refreshRichBlank()
        if (root.richMode)
            richSpellTimer.restart()
        else
            spellTimer.restart()
        if (root.richMode) {
            root.richSyncing = true
            app.richComposer.loadMarkdown(richInput.textDocument,
                                          app.composer.text)
            root.richSyncing = false
            mentionPopup.close()
            commandPopup.close()
            Qt.callLater(function () { richInput.forceActiveFocus() })
        } else {
            mentionPopup.close()
            commandPopup.close()
            Qt.callLater(function () { root.focusEditor() })
        }
        refreshFormatState()
    }
    // Reverse sync: a draft restore, edit start or post-send clear rewrites the
    // composer text from C++, and the rich document follows. The markdown
    // comparison skips the echo of the editor's own push.
    Connections {
        target: app.composer
        function onTextChanged() {
            if (!root.richMode || root.richSyncing)
                return
            var current = app.richComposer.toMarkdown(richInput.textDocument)
            if (current.trim() === app.composer.text.trim())
                return
            root.richSyncing = true
            app.richComposer.loadMarkdown(richInput.textDocument,
                                          app.composer.text)
            root.richSyncing = false
        }
    }
    function applyFormat(format) {
        if (root.richMode) {
            root.applyRichFormat(format, "")
            return
        }
        // Underline has no markdown form; it is a rich-mode key.
        if (format === "underline")
            return
        var result = app.composer.toggleFormat(format, input.text,
                                               input.selectionStart,
                                               input.selectionEnd)
        input.text = result.text
        app.composer.text = result.text
        input.select(result.selectionStart, result.selectionEnd)
        root.focusEditor()
        refreshFormatState()
    }
    function applyRichFormat(format, argument) {
        if (format === "link" && argument === ""
                && root.formatFlags["link"] !== true) {
            // A new link needs a target: a selected URL is its own, anything
            // else asks through the link dialog.
            var selected = richInput.selectedText
            if (selected.length > 0
                    && app.richComposer.isSafeLinkTarget(selected)) {
                argument = selected
            } else {
                linkDialog.openFor(richInput.selectionStart,
                                   richInput.selectionEnd)
                return
            }
        }
        app.richComposer.toggleFormat(richInput.textDocument,
                                      richInput.selectionStart,
                                      richInput.selectionEnd, format,
                                      argument)
        richInput.forceActiveFocus()
        refreshFormatState()
        // List/quote/code toggles change what is drawn without changing
        // characters.
        root.refreshRichBlank()
    }
    // Completion popups (mention, slash command, emoji shortcode) are placed
    // above the composer card, not the text field, so they never cover the
    // toolbar, reply banner or refusal strip. x still comes from the flickable
    // to align with the text being completed. Recomputed on every keystroke
    // that moves the popup.
    function composerPopupAnchor(flick) {
        var p = flick.mapToItem(Overlay.overlay, 0, 0)
        var card = composerCard.mapToItem(Overlay.overlay, 0, 0)
        return Qt.point(p.x, card.y)
    }
    // Rich-mode @-mentions: the same token scan over the editor's plain text,
    // whose offsets are document positions. Insertion writes a matrix.to
    // anchor, serialized as a link plus an m.mentions id.
    function updateRichMentionState() {
        if (!root.richMode)
            return
        if (app.currentRoomId === "") {
            mentionPopup.close()
            return
        }
        var plain = richInput.getText(0, richInput.length)
        var tok = app.composer.mentionTokenAt(plain, richInput.cursorPosition)
        // A completed pill is an anchor, not a token. The rich editor records
        // no mention refs, so skip a token inside an anchor or the popup
        // reopens on every keystroke after it.
        if (tok && tok.active === true
                && richInput.getFormattedText(tok.start, tok.start + 1)
                       .indexOf("matrix.to/#/") >= 0)
            tok = { active: false }
        if (tok && tok.active === true) {
            root.mentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.currentRoomId
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            mentionPopup.anchorInputTop =
                root.composerPopupAnchor(richFlick)
            mentionPopup.anchorWidth = richFlick.width
            if (!mentionPopup.visible)
                mentionPopup.open()
        } else {
            root.mentionTokenStart = -1
            mentionPopup.close()
        }
    }
    function escapeHtmlText(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function insertRichMention(userId, displayName) {
        var start = root.mentionTokenStart
        var end = richInput.cursorPosition
        if (start < 0 || end < start)
            return
        var name = displayName && displayName.length > 0
                   ? displayName : String(userId).substring(1)
        richInput.remove(start, end)
        // The trailing space sits OUTSIDE the anchor so typing after the
        // pill continues as ordinary text, not as more link.
        richInput.insert(start, "<a href=\"https://matrix.to/#/"
                         + encodeURIComponent(userId) + "\">@"
                         + root.escapeHtmlText(name) + "</a> ")
        richInput.cursorPosition = start + name.length + 2
        mentionPopup.close()
        richInput.forceActiveFocus()
    }

    // The editor owning the caret in the current mode; every refocus path uses
    // this so rich mode never focuses the hidden markdown editor.
    function activeEditor() {
        return root.richMode ? richInput : input
    }
    function focusEditor() {
        activeEditor().forceActiveFocus()
    }

    // Composer buttons the user switched off (Settings › Appearance › Message
    // box). A property rather than a Q_INVOKABLE so `visible` bindings track
    // it; composerButtonShown() reads it, so calling it registers the
    // dependency.
    readonly property var hiddenComposerButtons:
        app.settings ? app.settings.hiddenComposerButtons : []
    function composerButtonShown(key) {
        return root.hiddenComposerButtons.indexOf(key) < 0
    }

    // Picker buttons toggle. Pickers close on press outside, and the button is
    // outside, so a press closes the panel and the release (onClicked) would
    // reopen it. A panel dismissed moments ago followed by a click on its own
    // button means close. The window only needs to outlast a press.
    // `fromButton`: menu entries and demo hooks open unconditionally.
    readonly property int pickerToggleWindowMs: 600
    property string lastPickerDismissed: ""
    property double lastPickerDismissedAtMs: 0
    function notePickerDismissed(which) {
        root.lastPickerDismissed = which
        root.lastPickerDismissedAtMs = Date.now()
    }
    function clearPickerDismissal() {
        root.lastPickerDismissed = ""
        root.lastPickerDismissedAtMs = 0
    }
    // True when this press should close the panel. Handles both delivery
    // orders.
    function pickerButtonShouldClose(which, popup, fromButton) {
        if (popup.opened)
            return true
        if (fromButton !== true)
            return false
        var recent = root.lastPickerDismissed === which
                     && (Date.now() - root.lastPickerDismissedAtMs)
                        < root.pickerToggleWindowMs
        root.clearPickerDismissal()
        return recent
    }

    function openEmojiPicker(fromButton) {
        if (root.pickerButtonShouldClose("emoji", emojiPicker, fromButton)) {
            emojiPicker.close()
            root.clearPickerDismissal()
            root.focusEditor()
            return
        }
        var editor = root.activeEditor()
        emojiSelectionStart = editor.selectionStart
        emojiSelectionEnd = editor.selectionEnd
        emojiCursorPosition = editor.cursorPosition
        // Anchored to the composer card, which becomes the popup's parent, so
        // the picker stays rigidly aligned with the card.
        emojiPicker.anchorItem = composerCard
        emojiPicker.open()
    }

    function insertEmoji(emoji) {
        var editor = root.activeEditor()
        var start = Math.min(emojiSelectionStart, emojiSelectionEnd)
        var end = Math.max(emojiSelectionStart, emojiSelectionEnd)
        if (start === end) start = end = emojiCursorPosition
        editor.remove(start, end)
        editor.insert(start, emoji)
        editor.cursorPosition = start + emoji.length
        if (!root.richMode)
            app.composer.text = input.text
        // While the sticky picker stays open, keep focus there: the grid needs
        // it for keyboard multi-pick, and Escape must close the picker. Its
        // onClosed restores input focus.
        if (!emojiPicker.opened || emojiPicker.closeAfterSelection)
            root.focusEditor()
    }

    EmojiPicker {
        id: emojiPicker
        objectName: "composerEmojiPicker"
        mode: "composer"
        // The picker stays open after each insert; reaction pickers close on
        // pick.
        closeAfterSelection: false
        onEmojiChosen: (emoji) => root.insertEmoji(emoji)
        onAboutToHide: root.notePickerDismissed("emoji")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    // Outgoing @-mentions: the popup lists room members while an @-token is at
    // the caret; the input keeps focus and forwards navigation keys. Expansion
    // and m.mentions happen in MessageComposer at send time.
    property int mentionTokenStart: -1
    MentionPopup {
        id: mentionPopup
        suggestions: app.mentionSuggestions
        onChosen: (userId, displayName) => root.insertMention(userId, displayName)
        // Closing must drop the synthetic in-progress range without rescanning
        // (see refreshMentionHighlight).
        onVisibleChanged: root.refreshMentionHighlight()
    }

    // Slash-command completion. The input forwards navigation keys, command
    // popup first; it cannot coexist with the mention popup.
    property bool commandPopupDismissed: false
    SlashCommandPopup {
        id: commandPopup
        completions: app.composer.commandCompletions
        onChosen: (name) => {
            var pos = app.composer.acceptCommandCompletion(name)
            if (pos >= 0)
                input.cursorPosition = pos
        }
    }
    function updateCommandPopupState() {
        var comps = app.composer.commandCompletions
        var flick = root.richMode ? richFlick : inputFlick
        if (comps.length > 0 && !root.commandPopupDismissed
                && root.activeInput().activeFocus && app.currentRoomId !== "") {
            commandPopup.anchorInputTop = root.composerPopupAnchor(flick)
            commandPopup.anchorWidth = flick.width
            if (!commandPopup.visible)
                commandPopup.open()
        } else if (commandPopup.visible && comps.length === 0) {
            commandPopup.close()
        }
    }
    Connections {
        target: app.composer
        function onCommandCompletionsChanged() { root.updateCommandPopupState() }
    }

    // MSC2545 shortcode completion, refreshed from the input's signals since
    // only the editor knows where the caret is.
    property bool emojiPopupDismissed: false
    EmojiCompletionPopup {
        id: emojiPopup
        onChosen: (shortcode) => {
            var pos = app.composer.acceptEmojiCompletionAt(
                root.activeInput().cursorPosition, shortcode)
            if (pos >= 0)
                input.cursorPosition = pos
            emojiPopup.close()
        }
    }
    function updateEmojiPopupState() {
        // Never with the command or mention popup.
        if (commandPopup.visible || mentionPopup.visible) {
            emojiPopup.close()
            return
        }
        var editor = root.activeInput()
        if (!editor || !editor.activeFocus || app.currentRoomId === "") {
            emojiPopup.close()
            return
        }
        var comps = app.composer.emojiCompletionsAt(editor.cursorPosition)
        if (comps.length > 0 && !root.emojiPopupDismissed) {
            emojiPopup.completions = comps
            var flick = root.richMode ? richFlick : inputFlick
            emojiPopup.anchorInputTop = root.composerPopupAnchor(flick)
            emojiPopup.anchorWidth = flick.width
            if (!emojiPopup.visible)
                emojiPopup.open()
        } else if (emojiPopup.visible) {
            emojiPopup.close()
        }
    }
    // Link-target prompt: only targets the serializer would emit are accepted,
    // so an unsafe scheme cannot enter the document.
    Dialog {
        id: linkDialog
        objectName: "composerLinkDialog"
        modal: true
        Overlay.modal: Rectangle { color: AppTheme.modalScrim }
        focus: true
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape
        parent: Overlay.overlay
        width: Math.min(420, parent ? parent.width - AppTheme.spacing24 * 2 : 420)
        anchors.centerIn: parent
        padding: AppTheme.spacing16
        property int selStart: 0
        property int selEnd: 0
        function openFor(start, end) {
            selStart = start
            selEnd = end
            linkField.text = ""
            open()
            Qt.callLater(function () { linkField.forceActiveFocus() })
        }
        function apply() {
            var target = linkField.text.trim()
            if (!app.richComposer.isSafeLinkTarget(target))
                return
            app.richComposer.toggleFormat(richInput.textDocument,
                                          linkDialog.selStart,
                                          linkDialog.selEnd, "link", target)
            close()
            richInput.forceActiveFocus()
            root.refreshFormatState()
        }
        background: Rectangle {
            radius: AppTheme.radiusLg
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                text: qsTr("Add link")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
            }
            AppTextField {
                id: linkField
                objectName: "composerLinkField"
                Layout.fillWidth: true
                storm: true
                placeholderText: qsTr("https://…")
                onAccepted: linkDialog.apply()
            }
            Label {
                Layout.fillWidth: true
                visible: linkField.text.trim().length > 0
                         && !app.richComposer.isSafeLinkTarget(linkField.text.trim())
                text: qsTr("Only http, https, mailto and matrix links can be added.")
                color: AppTheme.danger
                font.pixelSize: AppTheme.fontChip
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Cancel")
                    kind: "ghost"
                    onClicked: linkDialog.close()
                }
                AppButton {
                    objectName: "composerLinkApply"
                    text: qsTr("Add link")
                    kind: "primary"
                    enabled: app.richComposer.isSafeLinkTarget(linkField.text.trim())
                    onClicked: linkDialog.apply()
                }
            }
        }
    }
    // Permission hints for completion rows, only when the room-info controller
    // is on the composer's room. A missing key counts as allowed; the server
    // enforces.
    Binding {
        target: app.composer
        property: "commandPermissions"
        value: app.roomInfo.roomId === app.currentRoomId
               ? { "kick": app.roomInfo.canKick,
                   "ban": app.roomInfo.canBan,
                   "unban": app.roomInfo.canUnban,
                   "invite": app.roomInfo.canInvite,
                   "topic": app.roomInfo.canEditTopic,
                   "roomname": app.roomInfo.canEditName }
               : ({})
    }
    // Format-only rehighlights re-emit textChanged with unchanged text and
    // cursor; rescanning then would loop the highlighter and reopen a dismissed
    // popup. Only genuine edits or cursor moves rescan.
    property string lastMentionScanText: ""
    property int lastMentionScanCursor: -1
    function updateMentionState() {
        // Rich mode scans its own editor; this field's changes are the mirror.
        if (root.richMode)
            return
        if (input.text === root.lastMentionScanText
            && input.cursorPosition === root.lastMentionScanCursor)
            return
        root.lastMentionScanText = input.text
        root.lastMentionScanCursor = input.cursorPosition
        if (app.currentRoomId === "") {
            mentionPopup.close()
            root.refreshMentionHighlight()
            return
        }
        var tok = app.composer.mentionTokenAt(input.text, input.cursorPosition)
        if (tok && tok.active === true) {
            root.mentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.currentRoomId
            // @room permission is not set here: MentionSuggestionModel takes it
            // from its own roster snapshot (see onRoomMembersReceived).
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            // Anchor to the Flickable viewport, not the TextArea, whose scene
            // top moves above the composer once a long draft scrolls. y comes
            // from the card (see composerPopupAnchor).
            mentionPopup.anchorInputTop =
                root.composerPopupAnchor(inputFlick)
            mentionPopup.anchorWidth = inputFlick.width
            if (!mentionPopup.visible)
                mentionPopup.open()
        } else {
            root.mentionTokenStart = -1
            mentionPopup.close()
        }
        root.refreshMentionHighlight()
    }
    // The authoritative mentionRanges plus one presentation-only range for the
    // token being typed while the popup is open. Never written back to
    // app.composer, so send-time logic is untouched. Assigned explicitly rather
    // than bound: rehighlighting nudges cursor signals, and a binding on
    // cursorPosition would loop. The ranges are copied: on Qt 6.8 a
    // QVariantList read from a property stays a live reference, so a stored
    // copy always compared equal and the previous message's mention range
    // stuck.
    property var mentionHighlightRanges: []
    function refreshMentionHighlight() {
        var ranges = []
        var live = app.composer.mentionRanges
        for (var k = 0; k < live.length; ++k)
            ranges.push({ start: live[k].start, length: live[k].length })
        if (mentionPopup.visible && root.mentionTokenStart >= 0) {
            var len = input.cursorPosition - root.mentionTokenStart
            if (len > 0)
                ranges = ranges.concat([{ start: root.mentionTokenStart,
                                          length: len }])
        }
        // Assign only on a real change; a fresh array would notify and
        // rehighlight.
        var current = root.mentionHighlightRanges
        if (current.length === ranges.length) {
            var same = true
            for (var i = 0; i < ranges.length; ++i) {
                if (current[i].start !== ranges[i].start
                    || current[i].length !== ranges[i].length) {
                    same = false
                    break
                }
            }
            if (same)
                return
        }
        root.mentionHighlightRanges = ranges
    }
    Connections {
        target: app.composer
        function onMentionRangesChanged() { root.refreshMentionHighlight() }
    }

    // Spell checking. Underlines are drawn here as Rectangles, not by the
    // highlighter: Qt Quick's text node only paints SingleUnderline, in the
    // text's colour, so SpellCheckUnderline shows nothing. And a document can
    // carry only one QSyntaxHighlighter (a second would erase the mention ink).
    readonly property bool spellActive: app.spell !== null
                                        && app.spell !== undefined
                                        && app.spell.available
                                        && app.spell.enabled
    // [{x, y, w}] in `input`'s coordinates; children of the TextArea, so they
    // scroll with the draft.
    property var spellUnderlines: []
    // The same for the rich editor.
    property var richSpellUnderlines: []
    // Whether the rich editor shows nothing. `length` is 0 for an empty list
    // item, which would paint the placeholder under Qt's "1.".
    property bool richBlank: true
    function refreshRichBlank() {
        root.richBlank = !app.richComposer
                         || app.richComposer.documentIsBlank(richInput.textDocument)
    }
    // The word the context menu was opened on; cleared on every open.
    property string spellMenuWord: ""
    property int spellMenuStart: -1
    property int spellMenuLength: 0
    property var spellMenuSuggestions: []

    // The text the checker sees and the ranges it skips: mention pills in both
    // modes, plus code in rich mode. Rich positions are document positions.
    function spellEditorText() {
        return root.richMode ? richInput.getText(0, richInput.length) : input.text
    }
    function spellSkipRanges() {
        // Rich mode uses document-derived ranges only: mentionRanges index the
        // markdown mirror, which differs once formatting is present.
        if (root.richMode)
            return app.richComposer.spellSkipRanges(richInput.textDocument)
        return app.composer.mentionRanges
    }

    // [{x, y, w}] under every range, in `editor`'s coordinates.
    function spellUnderlineRects(editor, ranges) {
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var start = ranges[i].start
            var end = start + ranges[i].length
            var p = start
            var guard = 0
            // One iteration per word per line; the guard bounds pathological
            // cases.
            while (p < end && guard++ < 64) {
                var head = editor.positionToRectangle(p)
                var q = end
                var tail = editor.positionToRectangle(q)
                if (tail.y !== head.y) {
                    while (q > p + 1
                           && editor.positionToRectangle(q).y !== head.y)
                        --q
                    tail = editor.positionToRectangle(q)
                }
                var w = tail.x - head.x
                if (w > 0)
                    out.push({ x: head.x,
                               y: head.y + head.height - 2,
                               w: w })
                p = q
            }
        }
        return out
    }

    function refreshSpellUnderlines() {
        if (!root.spellActive || root.richMode || input.text.length === 0) {
            if (root.spellUnderlines.length > 0)
                root.spellUnderlines = []
            return
        }
        // Skip the word at the caret, and the composer's mention ranges.
        var ranges = app.spell.misspelledRanges(input.text,
                                                input.cursorPosition,
                                                app.composer.mentionRanges)
        root.spellUnderlines = root.spellUnderlineRects(input, ranges)
    }

    // Rich mode: same policy over the document's plain text, excluding code and
    // mentions. Nothing is written into the document.
    function refreshRichSpellUnderlines() {
        if (!root.spellActive || !root.richMode || richInput.length === 0) {
            if (root.richSpellUnderlines.length > 0)
                root.richSpellUnderlines = []
            return
        }
        var ranges = app.spell.misspelledRanges(root.spellEditorText(),
                                                richInput.cursorPosition,
                                                root.spellSkipRanges())
        root.richSpellUnderlines = root.spellUnderlineRects(richInput, ranges)
    }

    // Fills spellMenu* for the word under the pointer before the menu opens.
    function prepareSpellMenu(mx, my) {
        root.spellMenuWord = ""
        root.spellMenuStart = -1
        root.spellMenuLength = 0
        root.spellMenuSuggestions = []
        if (!root.spellActive)
            return
        var editor = root.activeEditor()
        var text = root.spellEditorText()
        var hit = app.spell.wordAt(text, editor.positionAt(mx, my))
        if (!hit || hit.word === "")
            return
        // Only words the dictionary rejects get suggestions.
        var wrong = app.spell.misspelledRanges(text, -1, root.spellSkipRanges())
        var rejected = false
        for (var i = 0; i < wrong.length; ++i) {
            if (wrong[i].start === hit.start) {
                rejected = true
                break
            }
        }
        if (!rejected)
            return
        root.spellMenuWord = hit.word
        root.spellMenuStart = hit.start
        root.spellMenuLength = hit.length
        root.spellMenuSuggestions = app.spell.suggestions(hit.word).slice(0, 5)
    }

    function applySpellSuggestion(replacement) {
        if (root.spellMenuStart < 0 || replacement === undefined
            || replacement === "")
            return
        var at = root.spellMenuStart
        if (root.richMode) {
            // Replace exactly the misspelled range, keeping its character
            // format.
            app.richComposer.replaceRange(richInput.textDocument, at,
                                          root.spellMenuLength, replacement)
            richInput.cursorPosition = at + replacement.length
        } else {
            input.remove(at, at + root.spellMenuLength)
            input.insert(at, replacement)
            input.cursorPosition = at + replacement.length
        }
        root.focusEditor()
    }

    Connections {
        target: app.spell
        // Adding or ignoring a word invalidates the underlines.
        function onDictionaryChanged() {
            root.refreshSpellUnderlines()
            root.refreshRichSpellUnderlines()
        }
        function onEnabledChanged() {
            root.refreshSpellUnderlines()
            root.refreshRichSpellUnderlines()
        }
    }

    function insertMention(userId, displayName) {
        if (root.richMode) {
            root.insertRichMention(userId, displayName)
            return
        }
        var newCursor = app.composer.insertMention(userId, displayName,
                                                   root.mentionTokenStart,
                                                   input.cursorPosition)
        if (input.text !== app.composer.text)
            input.text = app.composer.text
        input.cursorPosition = newCursor
        mentionPopup.close()
        root.focusEditor()
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            mentionPopup.close()
            // A recording belongs to its room; switching away discards it.
            if (root.voiceActive)
                app.cancelVoiceRecording()
            // Same for a finished, unsent recording; its file is deleted.
            root.voiceWantsPreview = false
            root.discardPendingVoice()
        }
    }
    // Recorder results. Only targets the recorder while recording, so binding
    // this never constructs it.
    Connections {
        target: root.voiceActive ? app.voiceRecorder : null
        function onReady(filePath, mime, durationMs, waveform) {
            // Release ownership first, so a re-entrant signal does not find us
            // armed.
            app.endVoiceRecording()
            // "Done" finalizes into the preview bar; the pill's send button
            // keeps the one-press path.
            if (root.voiceWantsPreview) {
                root.voiceWantsPreview = false
                // An unanswered preview is replaced, and its file deleted.
                root.discardPendingVoice()
                root.pendingVoice = { filePath: filePath, mime: mime,
                                      durationMs: durationMs,
                                      waveform: waveform }
                return
            }
            app.composer.sendVoiceMessage(filePath, mime, durationMs,
                                          waveform)
        }
        function onFailed(message) {
            app.endVoiceRecording()
            root.voiceWantsPreview = false
            root.attachmentNotice = message
            noticeTimer.restart()
        }
    }

    // A finalized recording awaiting a decision, or null. This composer owns
    // the file: it is either sent or deleted.
    property var pendingVoice: null
    // Set by "Done" so the next ready() goes to the preview.
    property bool voiceWantsPreview: false

    function sendPendingVoice() {
        if (!root.pendingVoice)
            return
        var v = root.pendingVoice
        root.pendingVoice = null
        app.composer.sendVoiceMessage(v.filePath, v.mime, v.durationMs,
                                      v.waveform)
    }
    function discardPendingVoice() {
        if (!root.pendingVoice)
            return
        var v = root.pendingVoice
        root.pendingVoice = null
        app.discardPreparedVoice(v.filePath)
    }

    // GIFs and stickers, as one window
    readonly property bool mediaPickerBothKinds:
        app.gif.available && app.stickers.available
    // Which kind the button opens. Session-scoped: the switcher inside is one
    // click away.
    property string mediaPickerKind: "gif"
    function effectiveMediaKind() {
        if (root.mediaPickerKind === "sticker" && app.stickers.available)
            return "sticker"
        if (app.gif.available)
            return "gif"
        return app.stickers.available ? "sticker" : "gif"
    }
    // Both pickers report dismissal under the same key, so the toggle closes
    // whichever is showing.
    function openMediaPicker(fromButton) {
        var showing = gifPicker.opened || stickerPicker.opened
        if (showing || root.pickerButtonShouldClose("media", gifPicker,
                                                    fromButton)) {
            gifPicker.close()
            stickerPicker.close()
            root.clearPickerDismissal()
            root.focusEditor()
            return
        }
        if (root.effectiveMediaKind() === "sticker")
            root.openStickerPicker()
        else
            root.openGifPicker()
    }
    // Requested from inside a picker's GIFs/Stickers strip.
    function swapMediaPicker(kind) {
        root.mediaPickerKind = kind
        if (kind === "sticker")
            root.openStickerPicker()
        else
            root.openGifPicker()
    }

    function openGifPicker() {
        emojiPicker.close()
        stickerPicker.close()
        // Our own close is not a dismissal the next click should undo (see
        // pickerButtonShouldClose).
        root.clearPickerDismissal()
        root.mediaPickerKind = "gif"
        gifPicker.anchorItem = composerCard
        gifPicker.open()
    }

    GifPicker {
        id: gifPicker
        objectName: "composerGifPicker"
        target: "room"
        offerKindTabs: root.mediaPickerBothKinds
        onKindRequested: (kind) => root.swapMediaPicker(kind)
        onGifChosen: (result) => root.onGifPicked(result)
        // One key for both pickers: they are one window to the user.
        onAboutToHide: root.notePickerDismissed("media")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    // MSC2545 stickers, in their own picker (see StickerPicker.qml).
    function openStickerPicker() {
        emojiPicker.close()
        gifPicker.close()
        root.clearPickerDismissal()
        root.mediaPickerKind = "sticker"
        stickerPicker.anchorItem = composerCard
        stickerPicker.open()
    }

    StickerPicker {
        id: stickerPicker
        objectName: "composerStickerPicker"
        target: "room"
        offerKindTabs: root.mediaPickerBothKinds
        onKindRequested: (kind) => root.swapMediaPicker(kind)
        onStickerChosen: (image) => root.onStickerPicked(image)
        onAboutToHide: root.notePickerDismissed("media")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    // Send the chosen pack sticker as m.sticker to the room open now. The mxc
    // goes straight into the event; the SDK owns the send, echo and retry.
    function onStickerPicked(image) {
        app.stickers.sendToRoom(app.currentRoomId, image)
    }

    // Download, validate and send the chosen GIF, captured to this room so a
    // room switch cannot reroute it.
    function onGifPicked(result) {
        app.gifSend.sendToRoom(app.currentRoomId, result)
    }
    Connections {
        target: app.gifSend
        function onSendFailed(category, thread) {
            if (thread) return
            root.attachmentNotice = qsTr("The GIF could not be sent.")
            noticeTimer.restart()
        }
    }
    Timer {
        id: noticeTimer
        interval: 6000
        onTriggered: root.attachmentNotice = ""
    }
    Connections {
        target: app.composer
        function onAttachmentRejected(reason) {
            root.attachmentNotice = reason
            noticeTimer.restart()
        }
    }

    // Modern picker (Rust): multiple files, queued in the tray.
    FileDialog {
        id: pickAttachmentsDialog
        title: qsTr("Attach files")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i)
                app.composer.addAttachment(selectedFiles[i])
            root.focusStagedAttachmentSend()
        }
    }

    // Legacy pickers (HTTP backend: immediate upload).
    FileDialog {
        id: pickImageDialog
        title: qsTr("Send image")
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"),
                       qsTr("All files (*)") ]
        onAccepted: app.media.sendPickedImage(app.currentRoomId, selectedFile)
    }
    FileDialog {
        id: pickFileDialog
        title: qsTr("Send file")
        onAccepted: app.media.sendPickedFile(app.currentRoomId, selectedFile)
    }
    AppMenu {
        id: legacyAttachMenu
        // Anchored above the card like the other menus in this bar; a bare
        // popup() opens at the pointer and runs off the bottom of the window.
        // Left-aligned because attach is the leftmost control. See
        // sendOptionsMenu for the positioning binding.
        parent: composerCard
        x: 0
        y: -height - 4
        AppMenuItem {
            iconName: "image"
            text: qsTr("Send image…")
            onTriggered: pickImageDialog.open()
        }
        AppMenuItem {
            iconName: "attach_file"
            text: qsTr("Send file…")
            onTriggered: pickFileDialog.open()
        }
        // Displaced by a narrow window, as in the Rust-backend menu.
        AppMenuItem {
            objectName: "composerLegacyEmojiMenuItem"
            iconName: "mood"
            text: qsTr("Emoji…")
            visible: root.compactInputRow && root.composerButtonShown("emoji")
            onTriggered: root.openEmojiPicker()
        }
        AppMenuItem {
            objectName: "composerLegacyMediaMenuItem"
            iconName: "gif_box"
            text: root.mediaPickerBothKinds ? qsTr("GIFs and stickers…")
                                            : qsTr("GIF…")
            visible: root.compactInputRow
                     && root.composerButtonShown("media")
                     && (app.gif.available || app.stickers.available)
            onTriggered: root.openMediaPicker(false)
        }
    }

    // Rust-backend attach menu: files plus polls. The legacy menu keeps the
    // HTTP backend's immediate-upload paths.
    AppMenu {
        id: attachMenu
        objectName: "composerAttachMenu"
        // Same anchoring as legacyAttachMenu.
        parent: composerCard
        x: 0
        y: -height - 4
        AppMenuItem {
            iconName: "attach_file"
            text: qsTr("Attach files…")
            onTriggered: pickAttachmentsDialog.open()
        }
        AppMenuItem {
            objectName: "createPollMenuItem"
            iconName: "check_circle"
            text: qsTr("Create poll…")
            visible: app.composer.pollsSupported()
            onTriggered: createPollDialog.openDialog()
        }
        // Only while the input row is too narrow for their own buttons.
        AppMenuItem {
            objectName: "composerEmojiMenuItem"
            iconName: "mood"
            text: qsTr("Emoji…")
            visible: root.compactInputRow && root.composerButtonShown("emoji")
            onTriggered: root.openEmojiPicker()
        }
        AppMenuItem {
            objectName: "composerMediaMenuItem"
            iconName: "gif_box"
            text: root.mediaPickerBothKinds ? qsTr("GIFs and stickers…")
                                            : qsTr("GIF…")
            visible: root.compactInputRow
                     && root.composerButtonShown("media")
                     && (app.gif.available || app.stickers.available)
            onTriggered: root.openMediaPicker(false)
        }
    }
    CreatePollDialog {
        id: createPollDialog
    }

    // Keyboard access to attach, emoji and GIF. GlobalContext rather than
    // EditorContext: editor shortcuts are routed into applyFormat(), and a
    // global key also works while the timeline has focus. This bar exists once
    // per window, so the sequences cannot collide. Gated on the chat screen,
    // since MainScreen stays loaded under Settings and Shortcuts match by
    // window.
    function openAttachFiles() {
        if (app.currentRoomId === "")
            return
        // The key opens the file picker directly: a menu opened from a key
        // would appear at the mouse pointer.
        if (app.composer.attachmentsSupported)
            pickAttachmentsDialog.open()
        else
            pickFileDialog.open()
    }
    Shortcut {
        // bindingRevision is read inside the binding because sequenceFor()
        // creates no dependency.
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.emojiPicker")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
        // fromButton = false: a key press is not the press-then-click gesture.
        // It still closes an open picker.
        onActivated: root.openEmojiPicker(false)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.gifPicker")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
                 && (app.gif.available || app.stickers.available)
        onActivated: {
            // Opens on GIFs only when opening, so the key does not swap the
            // Stickers tab instead of closing. effectiveMediaKind() falls back
            // to stickers without a GIF provider.
            if (!gifPicker.opened && !stickerPicker.opened)
                root.mediaPickerKind = "gif"
            root.openMediaPicker(false)
        }
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.attach")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
        onActivated: root.openAttachFiles()
    }

    // Up in an empty message box edits your last message. canEditEvent() is the
    // same gate as the hover bar and context menu; beginEdit() takes the same
    // triple. A Keys handler rather than a Shortcut: a bare-key global Shortcut
    // would be consumed before any text field. Bounded at 200 source rows.
    readonly property int editLastMessageScanRows: 200
    function composerIsEmpty() {
        return root.richMode ? root.richBlank : (input.length === 0)
    }
    function editLastOwnMessage() {
        if (app.currentRoomId === "" || !app.timeline)
            return false
        if (!root.composerIsEmpty())
            return false
        // Not while editing (Up is cursor movement), in a thread, or with a
        // staged attachment.
        if (app.composer.isEditing || app.composer.inThread
                || app.composer.hasAttachments)
            return false
        // The model must show the room this composer sends to, or an m.replace
        // for one room could be sent to another.
        if (app.timeline.roomId !== app.currentRoomId)
            return false
        // Source rows: row 0 is the oldest (the rotated view counts the other
        // way).
        var newest = app.timeline.count - 1
        var oldestScanned =
            Math.max(0, newest - root.editLastMessageScanRows + 1)
        for (var row = newest; row >= oldestScanned; --row) {
            var eventId = app.timeline.eventIdAt(row)
            if (eventId === "" || !app.timeline.canEditEvent(eventId))
                continue
            app.composer.beginEdit(
                eventId,
                app.timeline.visibleTextForEvent(eventId),
                app.timeline.sanitizedHtmlForEvent(eventId))
            root.focusEditor()
            return true
        }
        return false
    }

    // Screenshot-demo hooks; inert in a non-demo build. All targets have ids in
    // this file.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenEmojiPicker() { root.openEmojiPicker() }
        function onDemoOpenGifPicker() { root.openGifPicker() }
        function onDemoOpenMentionPopup(prefix) {
            input.text = "@" + prefix
            input.cursorPosition = input.text.length
            root.updateMentionState()
        }
        function onDemoOpenCreatePoll() { createPollDialog.openDialog() }
    }

    // Files dragged over the composer are queued (Rust backend).
    DropArea {
        id: dropArea
        anchors.fill: parent
        enabled: app.composer.attachmentsSupported
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            for (var i = 0; i < drop.urls.length; ++i)
                app.composer.addAttachment(drop.urls[i])
            drop.accept(Qt.CopyAction)
            root.focusStagedAttachmentSend()
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: dropArea.containsDrag
        color: "transparent"
        border.color: AppTheme.focusRing
        border.width: 2
        radius: AppTheme.radiusSm
        z: 10
    }

    ColumnLayout {
        id: composerCol
        anchors {
            left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: AppTheme.spacing20
            rightMargin: AppTheme.spacing20
            bottomMargin: AppTheme.spacing16
        }
        spacing: AppTheme.spacing6

        // Attachment validation notice.
        Label {
            visible: root.attachmentNotice.length > 0
            Layout.fillWidth: true
            text: root.attachmentNotice
            color: AppTheme.warning
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            wrapMode: Text.WordWrap
        }

        // Attachment tray
        Flow {
            visible: app.composer.hasAttachments
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            Repeater {
                model: app.composer.attachments
                Rectangle {
                    id: attachmentChip
                    objectName: "composerAttachmentChip"
                    // Every chip is the same height: the Flow top-aligns, so a
                    // floor from the preview tile (the tallest content) keeps
                    // the row even.
                    readonly property int previewTileHeight: 48
                    radius: AppTheme.radiusSm
                    color: AppTheme.cardElevated
                    border.color: model.state === "failed" ? AppTheme.danger
                                                           : AppTheme.border
                    border.width: 1
                    implicitWidth: Math.min(chipLayout.implicitWidth + AppTheme.spacingS * 2, 280)
                    implicitHeight: Math.max(
                        chipLayout.implicitHeight,
                        attachmentChip.previewTileHeight) + AppTheme.spacingS

                    RowLayout {
                        id: chipLayout
                        anchors.centerIn: parent
                        spacing: AppTheme.spacingXS

                        // A picked file resolves to its file:// URL; a pasted
                        // image to image://lightning-staged/<token>.
                        readonly property string previewSource:
                            model.previewSource || ""
                        readonly property bool hasPreview:
                            chipLayout.previewSource.length > 0
                        readonly property bool hasLocalFile:
                            model.localUrl.toString().length > 0
                        // Guarded: roles can resolve undefined during teardown.
                        readonly property bool isGifChip:
                            (model.mime || "") === "image/gif"
                        readonly property bool isVideoChip:
                            (model.mime || "").indexOf("video/") === 0

                        // Preview tile: images get a thumbnail, GIFs animate,
                        // videos show their first frame through a muted,
                        // paused, per-chip player destroyed with the chip.
                        Rectangle {
                            visible: chipLayout.hasPreview
                                     && (model.isImage || chipLayout.isVideoChip)
                            width: 64
                            height: attachmentChip.previewTileHeight
                            radius: AppTheme.radiusSm
                            color: AppTheme.surface
                            clip: true

                            Image {
                                anchors.fill: parent
                                // Everything but an animating GIF file.
                                // AnimatedImage cannot animate an image://
                                // source, so a pasted GIF shows its first
                                // frame.
                                visible: model.isImage
                                         && !(chipLayout.isGifChip
                                              && chipLayout.hasLocalFile)
                                source: visible ? chipLayout.previewSource : ""
                                sourceSize.width: 128
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            AnimatedImage {
                                anchors.fill: parent
                                visible: chipLayout.isGifChip
                                         && chipLayout.hasLocalFile
                                source: visible ? model.localUrl : ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                playing: visible
                            }
                            Loader {
                                id: chipVideoLoader
                                anchors.fill: parent
                                // Bounded decoders: only the first few video
                                // chips get a poster player.
                                active: chipLayout.isVideoChip
                                        && chipLayout.hasLocalFile
                                        && index < 4
                                property bool posterFailed: false
                                sourceComponent: Item {
                                    VideoOutput {
                                        id: chipVideoOut
                                        anchors.fill: parent
                                        fillMode: VideoOutput.PreserveAspectCrop
                                    }
                                    MediaPlayer {
                                        property bool posterDone: false
                                        source: model.localUrl
                                        videoOutput: chipVideoOut
                                        // No audioOutput: sound discarded.
                                        onMediaStatusChanged: {
                                            // Render the first frame, then hold.
                                            if (mediaStatus
                                                    === MediaPlayer.LoadedMedia
                                                && !posterDone) {
                                                posterDone = true
                                                play()
                                                pause()
                                                position = 0
                                            }
                                        }
                                        onErrorOccurred:
                                            chipVideoLoader.posterFailed = true
                                    }
                                }
                            }
                            Icon {
                                // Video chips without a live poster player
                                // still identify themselves.
                                anchors.centerIn: parent
                                visible: chipLayout.isVideoChip
                                         && (!chipVideoLoader.active
                                             || chipVideoLoader.posterFailed)
                                name: "videocam"
                                size: 18
                                color: AppTheme.textMuted
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: chipLayout.isVideoChip
                                         && chipVideoLoader.active
                                         && !chipVideoLoader.posterFailed
                                name: "play_arrow"
                                size: 18
                                color: AppTheme.scrimInk
                            }
                        }
                        Icon {
                            visible: !chipLayout.hasPreview
                                     || (!model.isImage && !chipLayout.isVideoChip)
                            name: model.isImage ? "image" : "attach_file"
                            size: 16
                        }
                        ColumnLayout {
                            spacing: 0
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                text: model.fileName
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightMedium
                                elide: Label.ElideMiddle
                                Layout.maximumWidth: 140
                            }
                            Label {
                                text: {
                                    if (model.state === "failed")
                                        return model.error || qsTr("Failed")
                                    if (model.state === "dispatching")
                                        return qsTr("Sending…")
                                    return model.sizeLabel
                                }
                                color: model.state === "failed" ? AppTheme.danger
                                                                : AppTheme.textMuted
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                Layout.maximumWidth: 140
                            }
                        }
                        IconButton {
                            visible: model.state === "failed"
                            implicitWidth: 20; implicitHeight: 20
                            radius: AppTheme.radiusSm
                            iconName: "refresh"
                            iconSize: 14
                            Accessible.name: qsTr("Retry sending %1").arg(model.fileName)
                            onClicked: {
                                app.composer.attachments.retryAt(index)
                                app.composer.send()
                            }
                        }
                        IconButton {
                            enabled: model.state !== "dispatching"
                            implicitWidth: 20; implicitHeight: 20
                            radius: AppTheme.radiusSm
                            iconName: "close"
                            iconSize: 13
                            Accessible.name: qsTr("Remove attachment %1").arg(model.fileName)
                            onClicked: app.composer.attachments.removeAt(index)
                        }
                    }
                }
            }
        }

        // The composer card: toolbar row / divider / input row
        Item {
            Layout.fillWidth: true
            implicitHeight: composerCard.implicitHeight

            // Composer shadow, one of the four the design allows.
            MultiEffect {
                source: composerCard
                anchors.fill: composerCard
                z: -1
                shadowEnabled: true
                shadowColor: AppTheme.shadow
                shadowBlur: 0.6
                shadowVerticalOffset: 2
                shadowHorizontalOffset: 0
            }

            Rectangle {
                id: composerCard
                objectName: "composerCard"
                anchors.fill: parent
                implicitHeight: cardColumn.implicitHeight
                radius: AppTheme.radiusLg
                color: AppTheme.surface
                border.color: AppTheme.border
                border.width: 1

            ColumnLayout {
                id: cardColumn
                anchors.left: parent.left
                anchors.right: parent.right
                // Centred: the card fills the wrapper, so without a vertical
                // anchor all the slack sat below the column and the icons sat
                // high relative to the text.
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                // Slash-command refusal strip: only a known command that cannot
                // run (e.g. missing its argument). The draft stays; "Send as
                // message" posts it literally. Unknown commands are sent as
                // text, since a bot's command cannot be told from a typo.
                Item {
                    id: commandErrorRow
                    objectName: "composerCommandError"
                    visible: app.composer.commandError.length > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12 + 2
                    Layout.rightMargin: AppTheme.spacing8
                    Layout.topMargin: visible ? AppTheme.spacing8 : 0
                    implicitHeight: visible ? commandErrorLayout.implicitHeight : 0

                    RowLayout {
                        id: commandErrorLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: AppTheme.spacing8

                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            elide: Label.ElideRight
                            text: app.composer.commandError
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.scaled(12)
                        }
                        AppButton {
                            objectName: "composerSendAnywayButton"
                            text: qsTr("Send as message")
                            kind: "ghost"
                            size: "sm"
                            visible: app.composer.canSend
                            onClicked: app.composer.sendBypassingCommands()
                        }
                    }
                }

                // Reply / Edit / Thread context strip, drawn as the same quote
                // the timeline uses. The empty-Label viewport-observer hazard
                // does not apply: this is not a descendant of the timeline.
                Item {
                    id: contextRow
                    objectName: "composerContextBanner"
                    visible: app.composer.isReplying || app.composer.isEditing
                             || app.composer.inThread
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12 + 2
                    Layout.rightMargin: AppTheme.spacing8
                    Layout.topMargin: AppTheme.spacing8
                    Layout.bottomMargin: AppTheme.spacing6
                    implicitHeight: contextLayout.implicitHeight

                    // Only a reply has a target to jump to.
                    readonly property bool jumpable:
                        app.composer.isReplying && !app.composer.isEditing
                        && (app.composer.replyingToEventId || "").length > 0
                    readonly property string titleText: {
                        if (app.composer.isEditing)
                            return qsTr("Editing message")
                        if (app.composer.inThread)
                            return qsTr("Replying in thread")
                        return qsTr("Replying to %1")
                                   .arg(app.composer.replyingToSender
                                        || qsTr("someone"))
                    }
                    readonly property string bodyText: {
                        if (app.composer.isEditing)
                            return ""
                        if (app.composer.inThread)
                            return root.previewLine(app.composer.threadPreview)
                        return root.previewLine(app.composer.replyingToPreview)
                    }
                    function jumpToTarget() {
                        if (contextRow.jumpable)
                            app.pagination.jumpToEvent(
                                app.composer.replyingToEventId)
                    }

                    activeFocusOnTab: visible && jumpable
                    Accessible.role: jumpable ? Accessible.Button
                                              : Accessible.StaticText
                    Accessible.name: contextRow.bodyText.length > 0
                                     ? contextRow.titleText + ": "
                                       + contextRow.bodyText
                                     : contextRow.titleText
                    Accessible.onPressAction: contextRow.jumpToTarget()
                    Keys.onReturnPressed: contextRow.jumpToTarget()
                    Keys.onEnterPressed: contextRow.jumpToTarget()
                    Keys.onSpacePressed: contextRow.jumpToTarget()

                    // The whole strip is the target; does not take focus.
                    TapHandler {
                        enabled: contextRow.jumpable
                        onTapped: contextRow.jumpToTarget()
                    }
                    HoverHandler {
                        id: contextHover
                        enabled: contextRow.jumpable
                        cursorShape: Qt.PointingHandCursor
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        border.color: AppTheme.focusRing
                        border.width: 2
                        visible: contextRow.activeFocus
                    }

                    RowLayout {
                        id: contextLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: AppTheme.spacing8

                        // The quote rule, matching the timeline's quote.
                        Rectangle {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 2
                            radius: 1
                            color: contextHover.hovered ? AppTheme.accentHover
                                                        : AppTheme.accent
                            Behavior on color { ColorAnimation { duration: 90 } }
                        }

                        // Thumbnail when replying to an image, same bridge key
                        // as the timeline quote. The rounded corner is baked by
                        // MediaImageProvider ("|shape:rsq:<permille>"), not a
                        // MultiEffect mask.
                        Image {
                            id: composerReplyThumb
                            objectName: "composerReplyThumb"
                            readonly property string replyKey:
                                app.composer.isReplying
                                ? (app.composer.replyingToMediaKey || "") : ""
                            readonly property string bridgeSource: {
                                var _tick = resolveTick
                                return replyKey.length > 0
                                    && app.mediaBridge.supported
                                    ? app.mediaBridge.mediaSource(replyKey,
                                                                  "thumb")
                                    : ""
                            }
                            visible: replyKey.length > 0
                                     && status !== Image.Error
                                     && app.mediaBridge.supported
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            Layout.alignment: Qt.AlignVCenter
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 52
                            // mediaSource() returns "" on a cache miss and
                            // dispatches a fetch; bump this counter when the
                            // bytes land so the binding re-resolves. Never
                            // assign `source`, which would destroy the binding.
                            property int resolveTick: 0
                            source: bridgeSource.length > 0
                                    ? bridgeSource + "|shape:rsq:230" : ""
                            Connections {
                                target: app.mediaBridge
                                enabled: composerReplyThumb.replyKey.length > 0
                                function onMediaCached(key) {
                                    if (key === "thumb:" + composerReplyThumb.replyKey
                                        && composerReplyThumb.source.toString()
                                               .length === 0)
                                        composerReplyThumb.resolveTick++
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0
                            Label {
                                objectName: "composerContextTitle"
                                Layout.fillWidth: true
                                text: contextRow.titleText
                                textFormat: Text.PlainText
                                color: AppTheme.textPrimary
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                            Label {
                                objectName: "composerContextBody"
                                Layout.fillWidth: true
                                visible: contextRow.bodyText.length > 0
                                text: contextRow.bodyText
                                textFormat: Text.PlainText
                                color: AppTheme.textSecondary
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                        }
                        IconButton {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 24; implicitHeight: 24
                            radius: AppTheme.radiusControl
                            iconName: "close"
                            iconSize: 16
                            Accessible.name: app.composer.isEditing
                                             ? qsTr("Cancel editing")
                                             : qsTr("Cancel reply")
                            ToolTip.text: Accessible.name
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: app.composer.cancelReplyOrEdit()
                        }
                    }
                }
                Rectangle {
                    visible: contextRow.visible
                    Layout.fillWidth: true
                    Layout.leftMargin: 1
                    Layout.rightMargin: 1
                    implicitHeight: 1
                    color: AppTheme.border
                }

                // Formatting toolbar row, collapsed by default.
                RowLayout {
                    id: toolbarRow
                    objectName: "composerToolbarRow"
                    visible: root.toolbarExpanded
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing8 + 2
                    Layout.rightMargin: AppTheme.spacing8 + 2
                    Layout.topMargin: AppTheme.spacing8
                    Layout.bottomMargin: AppTheme.spacing8
                    spacing: 2

                    Repeater {
                        model: [
                            { key: "bold",   icon: "format_bold",
                              label: qsTr("Bold") },
                            { key: "italic", icon: "format_italic",
                              label: qsTr("Italic") },
                            { key: "strike", icon: "strikethrough_s",
                              label: qsTr("Strikethrough") },
                            { key: "code",   icon: "code",
                              label: qsTr("Inline code") },
                        ]
                        IconButton {
                            objectName: "composerFormat_" + modelData.key
                            implicitWidth: 28; implicitHeight: 28
                            radius: AppTheme.radiusControl
                            iconName: modelData.icon
                            iconSize: 18
                            enabled: app.currentRoomId !== ""
                            active: root.formatFlags[modelData.key] === true
                            Accessible.name: modelData.label
                            ToolTip.text: modelData.label
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.applyFormat(modelData.key)
                        }
                    }
                    Rectangle {
                        objectName: "composerToolbarDivider"
                        implicitWidth: 1
                        implicitHeight: 16
                        Layout.leftMargin: 4
                        Layout.rightMargin: 4
                        color: AppTheme.border
                    }
                    Repeater {
                        model: [
                            { key: "link",  icon: "link",
                              label: qsTr("Link") },
                            { key: "list",  icon: "format_list_bulleted",
                              label: qsTr("Bulleted list") },
                            { key: "quote", icon: "format_quote",
                              label: qsTr("Quote") },
                        ]
                        IconButton {
                            objectName: "composerFormat_" + modelData.key
                            implicitWidth: 28; implicitHeight: 28
                            radius: AppTheme.radiusControl
                            iconName: modelData.icon
                            iconSize: 18
                            enabled: app.currentRoomId !== ""
                            active: root.formatFlags[modelData.key] === true
                            Accessible.name: modelData.label
                            ToolTip.text: modelData.label
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.applyFormat(modelData.key)
                        }
                    }
                    // Rich-only controls as text chips: the icon font subset
                    // has no underline or numbered-list glyphs.
                    AppButton {
                        objectName: "composerFormat_underline"
                        visible: root.richMode
                        kind: "ghost"
                        size: "sm"
                        // The 72px minimum is for worded buttons; here it would
                        // read as a gap.
                        minWidth: 0
                        // U + COMBINING LOW LINE: AppButton sets its own font,
                        // so font.underline would not reach the label.
                        text: "U̲"
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Underline")
                        ToolTip.text: qsTr("Underline")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.applyFormat("underline")
                    }
                    AppButton {
                        objectName: "composerFormat_orderedlist"
                        visible: root.richMode
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        text: "1."
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Numbered list")
                        ToolTip.text: qsTr("Numbered list")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.applyFormat("orderedlist")
                    }
                    // Mode switch, draft-preserving both ways (see richMode);
                    // /markdown does the same. Left of the spacer: the empty
                    // right side must stay empty surface.
                    AppButton {
                        objectName: "composerModeToggle"
                        // Moves into the overflow menu below the width its
                        // label needs (see compactToolbarRow).
                        visible: !root.compactToolbarRow
                        kind: "ghost"
                        size: "sm"
                        text: root.richMode ? qsTr("Rich text") : qsTr("Markdown")
                        Accessible.name: root.richMode
                                         ? qsTr("Switch to Markdown composing")
                                         : qsTr("Switch to rich-text composing")
                        ToolTip.text: Accessible.name
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            if (app.settings)
                                app.settings.composerMode =
                                    root.richMode ? "markdown" : "rich"
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                // Divider between the rows, only while the toolbar is open.
                Rectangle {
                    objectName: "composerRowDivider"
                    visible: root.toolbarExpanded
                    Layout.fillWidth: true
                    Layout.leftMargin: 1
                    Layout.rightMargin: 1
                    implicitHeight: 1
                    color: AppTheme.border
                }

                // Input row: attach · format · input · emoji · media · mic ·
                // send · send options.
                RowLayout {
                    id: inputRow
                    objectName: "composerInputRow"
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12 + 2
                    Layout.rightMargin: AppTheme.spacing12 + 2
                    Layout.topMargin: AppTheme.spacing8 + 2
                    Layout.bottomMargin: AppTheme.spacing8 + 2
                    spacing: AppTheme.spacing8 + 2

                    IconButton {
                        objectName: "composerAttachButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "add_circle"
                        iconSize: 20
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Attach files or create a poll")
                        onClicked: {
                            if (!app.composer.attachmentsSupported) {
                                legacyAttachMenu.open()
                                return
                            }
                            // Offer the menu when polls are available, or when
                            // a narrow window has moved emoji/GIF into it;
                            // otherwise open the file picker directly.
                            if (app.composer.pollsSupported()
                                    || root.compactInputRow)
                                attachMenu.open()
                            else
                                pickAttachmentsDialog.open()
                        }
                        ToolTip.text: qsTr("Attach")
                        // Hidden while an attach menu is open: the menu is
                        // above the card and would slice through the tooltip.
                        ToolTip.visible: hovered && !attachMenu.visible
                                         && !legacyAttachMenu.visible
                        ToolTip.delay: 500
                    }

                    // Format toggle: opens/closes the formatting toolbar.
                    IconButton {
                        objectName: "composerFormatToggleButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: optional controls yield width to the
                        // text field. Also hidden when switched off.
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("formatting")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "edit_square"
                        iconSize: 20
                        // Usable regardless of room state; the format buttons
                        // it reveals are room-gated.
                        active: root.toolbarExpanded
                        Accessible.name: qsTr("Formatting")
                        ToolTip.text: root.toolbarExpanded
                                      ? qsTr("Hide formatting")
                                      : qsTr("Show formatting")
                        // Not while the toolbar is open: the tooltip sits above
                        // its button, exactly where the raised toolbar row
                        // lands, and would cover its buttons. Same rule as the
                        // emoji, media and overflow buttons.
                        ToolTip.visible: hovered && !root.toolbarExpanded
                        ToolTip.delay: 500
                        onClicked: root.toolbarExpanded = !root.toolbarExpanded
                    }

                    // The rich (WYSIWYG) editor: same growth, padding and
                    // insets as the markdown field (see inputFlick);
                    // visibility-exclusive with it.
                    Flickable {
                        id: richFlick
                        objectName: "composerRichInputFlick"
                        visible: root.richMode
                        Layout.fillWidth: true
                        Layout.minimumWidth: 120
                        Layout.alignment: Qt.AlignVCenter
                        Layout.maximumHeight: AppTheme.scaled(140)
                        implicitHeight: richInput.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        function clampContentToTop() {
                            if (contentHeight <= height && contentY !== 0)
                                contentY = 0
                        }
                        onContentYChanged: clampContentToTop()
                        onContentHeightChanged: clampContentToTop()
                        onHeightChanged: clampContentToTop()
                        ScrollBar.vertical: AppScrollBar { thin: true }

                        TextArea.flickable: TextArea {
                            id: richInput
                            objectName: "composerRichInput"
                            onWidthChanged: richSpellTimer.restart()
                            // Spell underlines for the rich editor: same
                            // debounce, own geometry.
                            Timer {
                                id: richSpellTimer
                                objectName: "composerRichSpellTimer"
                                interval: 150
                                repeat: false
                                onTriggered: root.refreshRichSpellUnderlines()
                            }
                            Repeater {
                                objectName: "composerRichSpellUnderlines"
                                model: root.richSpellUnderlines
                                delegate: Rectangle {
                                    objectName: "composerRichSpellUnderline"
                                    required property var modelData
                                    x: modelData.x
                                    y: modelData.y
                                    width: modelData.w
                                    height: 2
                                    radius: 1
                                    color: Qt.alpha(AppTheme.danger, 0.85)
                                }
                            }
                            // Same context menu as the markdown editor:
                            // spelling rows, then editing rows.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: (mouse) => {
                                    root.focusEditor()
                                    root.prepareSpellMenu(mouse.x, mouse.y)
                                    composerEditMenu.popup()
                                }
                            }
                            textFormat: TextEdit.RichText
                            // Empty once the document draws anything, structure
                            // included.
                            placeholderText: root.richBlank
                                             ? input.placeholderText : ""
                            placeholderTextColor: AppTheme.textMuted
                            font: app.textFontWithEmoji(AppTheme.uiFont,
                                                        AppTheme.scaled(14))
                            topPadding: AppTheme.spacing6
                            bottomPadding: AppTheme.spacing6
                            topInset: 0
                            bottomInset: 0
                            leftInset: 0
                            rightInset: 0
                            inputMethodHints: Qt.ImhNone
                            verticalAlignment: TextEdit.AlignVCenter
                            wrapMode: TextArea.Wrap
                            enabled: app.currentRoomId !== ""
                            background: Rectangle { color: "transparent" }
                            onTextChanged: {
                                richSpellTimer.restart()
                                root.refreshRichBlank()
                                if (root.richSyncing || !root.richMode)
                                    return
                                // The markdown mirror (see root.richMode).
                                root.richSyncing = true
                                app.composer.text =
                                    app.richComposer.toMarkdown(textDocument)
                                root.richSyncing = false
                                root.refreshFormatState()
                                root.updateRichMentionState()
                                root.commandPopupDismissed = false
                                root.updateCommandPopupState()
                                root.emojiPopupDismissed = false
                                root.updateEmojiPopupState()
                            }
                            onSelectionStartChanged: root.refreshFormatState()
                            onSelectionEndChanged: root.refreshFormatState()
                            onCursorPositionChanged: {
                                root.refreshFormatState()
                                root.updateRichMentionState()
                                root.updateEmojiPopupState()
                                richSpellTimer.restart()
                            }
                            Keys.onReturnPressed: (event) => {
                                if (commandPopup.visible) {
                                    commandPopup.accept()
                                    event.accepted = true
                                    return
                                }
                                if (mentionPopup.visible) {
                                    mentionPopup.accept()
                                    event.accepted = true
                                    return
                                }
                                if (emojiPopup.visible) {
                                    emojiPopup.accept()
                                    event.accepted = true
                                    return
                                }
                                if (!root._returnShouldSend(event.modifiers)) {
                                    event.accepted = false
                                    return
                                }
                                event.accepted = true
                                root.submitComposer()
                                richInput.forceActiveFocus()
                            }
                            Keys.onShortcutOverride: (event) => {
                                if (root._composerFormatFor(event.key,
                                                            event.modifiers) !== "")
                                    event.accepted = true
                            }
                            Keys.onUpPressed: (event) => {
                                if (commandPopup.visible) {
                                    commandPopup.moveUp()
                                    event.accepted = true
                                } else if (mentionPopup.visible) {
                                    mentionPopup.moveUp()
                                    event.accepted = true
                                } else if (emojiPopup.visible) {
                                    emojiPopup.moveUp()
                                    event.accepted = true
                                } else if (root.editLastOwnMessage()) {
                                    // Last, from the else arm: the completion
                                    // popups own Up while open.
                                    // editLastOwnMessage() refuses unless the
                                    // box is empty and in no special state, so
                                    // Up is otherwise unchanged.
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onDownPressed: (event) => {
                                if (commandPopup.visible) {
                                    commandPopup.moveDown()
                                    event.accepted = true
                                } else if (mentionPopup.visible) {
                                    mentionPopup.moveDown()
                                    event.accepted = true
                                } else if (emojiPopup.visible) {
                                    emojiPopup.moveDown()
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onTabPressed: (event) => {
                                if (commandPopup.visible) {
                                    commandPopup.accept()
                                    event.accepted = true
                                } else if (mentionPopup.visible) {
                                    mentionPopup.accept()
                                    event.accepted = true
                                } else if (emojiPopup.visible) {
                                    emojiPopup.accept()
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onEscapePressed: (event) => {
                                if (commandPopup.visible) {
                                    root.commandPopupDismissed = true
                                    commandPopup.close()
                                    event.accepted = true
                                } else if (mentionPopup.visible) {
                                    mentionPopup.close()
                                    event.accepted = true
                                } else if (app.composer.isReplying
                                           || app.composer.isEditing
                                           || app.composer.inThread) {
                                    app.composer.cancelReplyOrEdit()
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onPressed: (event) => {
                                // Clipboard images and file URLs become
                                // attachments; formatted text pastes into the
                                // document (the serializer's whitelist decides
                                // what is sent).
                                if (event.matches(StandardKey.Paste)
                                        && app.composer.pasteFromClipboard()) {
                                    event.accepted = true
                                    return
                                }
                                var formatAction =
                                    root._composerFormatFor(event.key,
                                                            event.modifiers)
                                if (formatAction !== "") {
                                    event.accepted = true
                                    root.applyFormat(
                                        formatAction.substring("composer.".length))
                                }
                            }
                        }
                    }

                    Flickable {
                        id: inputFlick
                        objectName: "composerInputFlick"
                        visible: !root.richMode
                        Layout.fillWidth: true
                        // A readable floor for the text field; optional
                        // controls step aside instead (root.compactInputRow).
                        Layout.minimumWidth: 120
                        Layout.alignment: Qt.AlignVCenter
                        // Grows with content up to ~6 lines, then scrolls. A
                        // bare TextArea cannot scroll itself;
                        // TextArea.flickable provides scrolling and keeps the
                        // caret visible.
                        Layout.maximumHeight: AppTheme.scaled(140)
                        implicitHeight: input.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        // Content that fits is never scrolled. Qt's
                        // TextArea-in-Flickable integration can leave contentY
                        // negative (the single line sits low until a click runs
                        // ensureVisible). boundsBehavior only constrains
                        // dragging, so state the invariant directly. Depends on
                        // the app font's metrics, so a test harness with the
                        // default font does not show it.
                        function clampContentToTop() {
                            if (contentHeight <= height && contentY !== 0)
                                contentY = 0
                        }
                        onContentYChanged: clampContentToTop()
                        onContentHeightChanged: clampContentToTop()
                        onHeightChanged: clampContentToTop()
                        ScrollBar.vertical: AppScrollBar { thin: true }

                        TextArea.flickable: TextArea {
                        id: input
                        objectName: "composerInput"
                        placeholderText: {
                            if (app.currentRoomId === "")
                                return qsTr("Select a room to start typing")
                            if (app.composer.isEditing)
                                return qsTr("Edit message…")
                            return qsTr("Message %1").arg(root.roomDisplayName())
                        }
                        placeholderTextColor: AppTheme.textMuted
                        // A whole font with the colour emoji face behind the UI
                        // face: QML cannot express a families list, and Qt
                        // 6.8's automatic fallback picks a monochrome face.
                        font: app.textFontWithEmoji(AppTheme.uiFont, AppTheme.scaled(14))
                        // Explicit, symmetric vertical padding: the platform
                        // style's padding is asymmetric and placed a single
                        // line below centre.
                        topPadding: AppTheme.spacing6
                        bottomPadding: AppTheme.spacing6
                        // Insets pinned too: a style's focus-dependent
                        // background insets can shift the text on focus while
                        // the icons stay put. Not reproduced offscreen, which
                        // does not load the platform style.
                        topInset: 0
                        bottomInset: 0
                        leftInset: 0
                        rightInset: 0
                        // Text centred in the field's height, so its position
                        // is declared rather than emergent. Input method hints
                        // are declared empty on purpose: prediction-disabling
                        // or sensitive-data hints do not belong on a message
                        // composer, and ComposerSpellContractTest fails if
                        // either appears. Windows 11 hardware keyboard
                        // suggestions need TSF, which Qt's Windows plugin does
                        // not implement (it uses IMM32); spell checking is
                        // Lightning's own, via app.spell.
                        inputMethodHints: Qt.ImhNone
                        verticalAlignment: TextEdit.AlignVCenter
                        wrapMode: TextArea.Wrap
                        enabled: app.currentRoomId !== ""
                        text: app.composer.text
                        onTextChanged: {
                            if (app.composer.text !== text) app.composer.text = text
                            root.refreshFormatState()
                            root.updateMentionState()
                            // A dismissed command popup reopens once the text
                            // moves on.
                            root.commandPopupDismissed = false
                            root.updateCommandPopupState()
                            root.emojiPopupDismissed = false
                            root.updateEmojiPopupState()
                            spellTimer.restart()
                        }
                        onSelectionStartChanged: root.refreshFormatState()
                        onSelectionEndChanged: root.refreshFormatState()
                        onCursorPositionChanged: {
                            root.refreshFormatState()
                            root.updateMentionState()
                            root.updateEmojiPopupState()
                            spellTimer.restart()
                        }
                        // A reflow moves every underline.
                        onWidthChanged: spellTimer.restart()
                        Keys.onReturnPressed: (event) => {
                            // While a completion popup is open, Return picks
                            // the highlighted entry.
                            if (commandPopup.visible) {
                                commandPopup.accept()
                                event.accepted = true
                                return
                            }
                            if (mentionPopup.visible) {
                                mentionPopup.accept()
                                event.accepted = true
                                return
                            }
                            if (emojiPopup.visible) {
                                emojiPopup.accept()
                                event.accepted = true
                                return
                            }
                            if (!root._returnShouldSend(event.modifiers)) {
                                event.accepted = false
                                return
                            }
                            event.accepted = true
                            app.composer.send()
                            root.focusEditor()
                        }
                        Keys.onShortcutOverride: (event) => {
                            if (root._composerFormatFor(event.key,
                                                        event.modifiers) !== "")
                                event.accepted = true
                        }
                        Keys.onUpPressed: (event) => {
                            if (commandPopup.visible) {
                                commandPopup.moveUp()
                                event.accepted = true
                            } else if (mentionPopup.visible) {
                                mentionPopup.moveUp()
                                event.accepted = true
                            } else if (emojiPopup.visible) {
                                emojiPopup.moveUp()
                                event.accepted = true
                            } else if (root.editLastOwnMessage()) {
                                // Same branch as the rich editor's: after the
                                // popups, else arm only.
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onDownPressed: (event) => {
                            if (commandPopup.visible) {
                                commandPopup.moveDown()
                                event.accepted = true
                            } else if (mentionPopup.visible) {
                                mentionPopup.moveDown()
                                event.accepted = true
                            } else if (emojiPopup.visible) {
                                emojiPopup.moveDown()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onTabPressed: (event) => {
                            if (commandPopup.visible) {
                                commandPopup.accept()
                                event.accepted = true
                            } else if (mentionPopup.visible) {
                                mentionPopup.accept()
                                event.accepted = true
                            } else if (emojiPopup.visible) {
                                emojiPopup.accept()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onEscapePressed: (event) => {
                            // Escape closes an open completion popup without
                            // touching reply/edit state; only with both closed
                            // does it cancel the reply/edit.
                            if (commandPopup.visible) {
                                root.commandPopupDismissed = true
                                commandPopup.close()
                                event.accepted = true
                            } else if (mentionPopup.visible) {
                                mentionPopup.close()
                                event.accepted = true
                            } else if (app.composer.isReplying
                                       || app.composer.isEditing
                                       || app.composer.inThread) {
                                app.composer.cancelReplyOrEdit()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        // Right-click editing menu. The default menu's Paste is
                        // text only, so a copied image would paste its URL
                        // while Ctrl+V pasted the picture; both go through
                        // Composer::pasteFromClipboard() now. A MouseArea
                        // consumes the right press so the built-in menu does
                        // not open; left presses are not accepted.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: (mouse) => {
                                root.focusEditor()
                                // Resolved before the menu opens so the
                                // spelling rows are settled.
                                root.prepareSpellMenu(mouse.x, mouse.y)
                                composerEditMenu.popup()
                            }
                        }
                        AppMenu {
                            id: composerEditMenu
                            objectName: "composerEditMenu"
                            menuWidth: AppTheme.menuWidthFlyout
                            // Spelling rows first. Written out rather than
                            // generated; a hidden AppMenuItem takes no height.
                            AppMenuItem {
                                objectName: "composerSpellSuggestion0"
                                visible: root.spellMenuSuggestions.length > 0
                                text: visible ? root.spellMenuSuggestions[0] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[0])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion1"
                                visible: root.spellMenuSuggestions.length > 1
                                text: visible ? root.spellMenuSuggestions[1] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[1])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion2"
                                visible: root.spellMenuSuggestions.length > 2
                                text: visible ? root.spellMenuSuggestions[2] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[2])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion3"
                                visible: root.spellMenuSuggestions.length > 3
                                text: visible ? root.spellMenuSuggestions[3] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[3])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion4"
                                visible: root.spellMenuSuggestions.length > 4
                                text: visible ? root.spellMenuSuggestions[4] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[4])
                            }
                            AppMenuItem {
                                objectName: "composerSpellAdd"
                                visible: root.spellMenuWord !== ""
                                text: qsTr("Add to dictionary")
                                onTriggered: app.spell.addToDictionary(
                                                 root.spellMenuWord)
                            }
                            AppMenuItem {
                                objectName: "composerSpellIgnore"
                                visible: root.spellMenuWord !== ""
                                text: qsTr("Ignore")
                                onTriggered: app.spell.ignoreWord(
                                                 root.spellMenuWord)
                            }
                            AppMenuSeparator {
                                visible: root.spellMenuWord !== ""
                            }
                            AppMenuItem {
                                text: qsTr("Cut")
                                enabled: root.activeEditor().selectedText.length > 0
                                onTriggered: root.activeEditor().cut()
                            }
                            AppMenuItem {
                                text: qsTr("Copy")
                                enabled: root.activeEditor().selectedText.length > 0
                                onTriggered: root.activeEditor().copy()
                            }
                            AppMenuItem {
                                objectName: "composerPasteItem"
                                text: qsTr("Paste")
                                onTriggered: {
                                    if (!app.composer.pasteFromClipboard())
                                        root.activeEditor().paste()
                                }
                            }
                            AppMenuSeparator {}
                            AppMenuItem {
                                text: qsTr("Select all")
                                enabled: root.activeEditor().length > 0
                                onTriggered: root.activeEditor().selectAll()
                            }
                        }
                        Keys.onPressed: (event) => {
                            // Clipboard images and file URLs become
                            // attachments; text pastes normally.
                            if (event.matches(StandardKey.Paste)
                                    && app.composer.pasteFromClipboard()) {
                                event.accepted = true
                                return
                            }
                            // Atomic mention delete: Backspace at a chip's
                            // trailing edge (or Delete at its leading edge)
                            // removes the whole mention.
                            if ((event.key === Qt.Key_Backspace
                                 || event.key === Qt.Key_Delete)
                                    && input.selectionStart === input.selectionEnd) {
                                var ranges = app.composer.mentionRanges
                                for (var i = 0; i < ranges.length; ++i) {
                                    var r = ranges[i]
                                    var hit = event.key === Qt.Key_Backspace
                                        ? input.cursorPosition === r.start + r.length
                                        : input.cursorPosition === r.start
                                    if (hit) {
                                        input.remove(r.start, r.start + r.length)
                                        event.accepted = true
                                        return
                                    }
                                }
                            }
                            // Formatting keys, delivered as plain presses via
                            // the ShortcutOverride above. Registry ids map onto
                            // applyFormat() keys. Anything unrecognised is not
                            // accepted.
                            var formatAction =
                                root._composerFormatFor(event.key,
                                                        event.modifiers)
                            if (formatAction !== "") {
                                event.accepted = true
                                root.applyFormat(
                                    formatAction.substring("composer.".length))
                            }
                        }
                        // The card is the visual container; the field is
                        // borderless.
                        background: Rectangle { color: "transparent" }

                        // Spell underlines, coalesced behind a short timer: the
                        // geometry pass calls positionToRectangle per
                        // misspelling.
                        Timer {
                            id: spellTimer
                            objectName: "composerSpellTimer"
                            interval: 150
                            repeat: false
                            onTriggered: root.refreshSpellUnderlines()
                        }
                        Repeater {
                            objectName: "composerSpellUnderlines"
                            model: root.spellUnderlines
                            delegate: Rectangle {
                                objectName: "composerSpellUnderline"
                                required property var modelData
                                x: modelData.x
                                y: modelData.y
                                width: modelData.w
                                height: 2
                                radius: 1
                                // The conventional red, as ink at reduced
                                // weight; nothing destructive.
                                color: Qt.alpha(AppTheme.danger, 0.85)
                            }
                        }

                        // Inline mention chips over the ranges the composer
                        // re-anchors on every edit.
                        MentionHighlighter {
                            document: input.textDocument
                            ranges: root.mentionHighlightRanges
                            accentColor: AppTheme.accent
                            // Named, because Qt 6.8 picks a monochrome emoji
                            // face; per-range, so surrounding words keep the UI
                            // face.
                            emojiFontFamily: app.emojiFontFamily || ""
                        }
                        }
                    }

                    IconButton {
                        id: emojiButton
                        objectName: "composerEmojiButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: moves into the attach menu. Switched
                        // off in settings it is gone from both.
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("emoji")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "mood"
                        iconSize: 20
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Insert emoji")
                        ToolTip.text: qsTr("Emoji")
                        // Same rule as the GIF button: the picker covers it.
                        ToolTip.visible: hovered && !emojiPicker.visible
                        ToolTip.delay: 500
                        onClicked: root.openEmojiPicker(true)
                    }

                    // GIFs and stickers: one button, one window. The two
                    // pickers stay separate components with a shared
                    // GIFs/Stickers strip; swapMediaPicker() swaps them at the
                    // same anchor and size, so it reads as a tab change. A
                    // glyph like its neighbours.
                    IconButton {
                        id: mediaButton
                        objectName: "composerMediaButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: moves into the attach menu. Hidden
                        // only when switched off; a backend with neither kind
                        // shows it disabled with an explanatory tooltip.
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("media")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "gif_box"
                        iconSize: 20
                        enabled: app.currentRoomId !== ""
                                 && (app.gif.available || app.stickers.available)
                        Accessible.name: qsTr("Insert a GIF or sticker")
                        ToolTip.text: !app.gif.available && !app.stickers.available
                                      ? qsTr("GIFs and stickers are unavailable "
                                             + "on this backend")
                                      : root.mediaPickerBothKinds
                                        ? qsTr("GIFs and stickers")
                                        : (app.gif.available ? qsTr("GIF")
                                                             : qsTr("Sticker"))
                        // Not while the picker is open above it.
                        ToolTip.visible: hovered && !gifPicker.visible
                        ToolTip.delay: 500
                        onClicked: root.openMediaPicker(true)
                    }

                    // Voice capture. Idle: the mic slot. Recording: a pill with
                    // a pulsing dot, elapsed time, cancel and send.
                    // app.voiceRecorder is created lazily on the first press;
                    // missing hardware or encoders surface through failed().
                    IconButton {
                        objectName: "composerMicButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "mic"
                        iconSize: 20
                        // A recording in flight keeps its controls whatever the
                        // setting says; the pill is the way to stop it. In a
                        // compact row the "…" button carries this action along
                        // with emoji and GIF.
                        visible: !root.voiceActive
                                 && root.composerButtonShown("voice")
                                 && !root.compactInputRow
                        enabled: app.currentRoomId !== ""
                                 && app.composer.attachmentsSupported
                        Accessible.name: qsTr("Record a voice message")
                        ToolTip.text: qsTr("Record a voice message")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.startVoiceMessage()
                    }
                    IconButton {
                        objectName: "composerOverflowButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_horiz"
                        iconSize: 20
                        // Compact rows only: emoji, GIFs/stickers and voice in
                        // one menu. Send later stays under the send button's
                        // chevron.
                        visible: root.compactInputRow && !root.voiceActive
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("More")
                        ToolTip.text: qsTr("More")
                        ToolTip.visible: hovered && !composerOverflowMenu.visible
                        ToolTip.delay: 500
                        onClicked: composerOverflowMenu.open()
                    }
                    Rectangle {
                        id: voicePill
                        objectName: "composerVoicePill"
                        // Never touch app.voiceRecorder while idle: its getter
                        // constructs the recorder and audio backend.
                        readonly property var rec:
                            root.voiceActive ? app.voiceRecorder : null
                        visible: root.voiceActive
                        Layout.alignment: Qt.AlignVCenter
                        implicitHeight: 28
                        implicitWidth: voicePillRow.implicitWidth + 16
                        radius: AppTheme.radiusPill
                        color: AppTheme.accentSoft
                        border.color: AppTheme.accent
                        border.width: 1
                        RowLayout {
                            id: voicePillRow
                            anchors.centerIn: parent
                            spacing: AppTheme.spacing8
                            Rectangle {
                                id: voiceDot
                                width: 8; height: 8; radius: 4
                                // A fill: `danger` is an ink-only role.
                                color: AppTheme.dangerFill
                                // Solid while finalizing; pulsing while live
                                // (steady with reduced motion).
                                property real t: 0
                                opacity: (voicePill.rec
                                          && voicePill.rec.processing)
                                         || AppTheme.reducedMotion
                                         ? 1.0 : 0.35 + 0.65 * voiceDot.t
                                SequentialAnimation on t {
                                    running: root.voiceActive
                                             && !AppTheme.reducedMotion
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 0; to: 1; duration: 700 }
                                    NumberAnimation { from: 1; to: 0; duration: 700 }
                                }
                            }
                            Label {
                                text: {
                                    var ms = voicePill.rec
                                             ? voicePill.rec.durationMs : 0
                                    var total = Math.floor(ms / 1000)
                                    var m = Math.floor(total / 60)
                                    var s = total % 60
                                    return m + ":" + (s < 10 ? "0" : "") + s
                                }
                                color: AppTheme.text
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightStrong
                            }
                            // Pause/resume: keeps the microphone and file,
                            // suspends capture and freezes the elapsed time.
                            IconButton {
                                objectName: "composerVoicePauseButton"
                                implicitWidth: 24; implicitHeight: 24
                                iconName: voicePill.rec && voicePill.rec.paused
                                          ? "play_arrow" : "pause"
                                iconSize: 15
                                enabled: voicePill.rec
                                         && voicePill.rec.recording
                                Accessible.name:
                                    voicePill.rec && voicePill.rec.paused
                                    ? qsTr("Resume recording")
                                    : qsTr("Pause recording")
                                ToolTip.text: Accessible.name
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: {
                                    if (!voicePill.rec)
                                        return
                                    if (voicePill.rec.paused)
                                        voicePill.rec.resume()
                                    else
                                        voicePill.rec.pause()
                                }
                            }
                            IconButton {
                                objectName: "composerVoiceCancelButton"
                                implicitWidth: 24; implicitHeight: 24
                                iconName: "close"
                                iconSize: 15
                                Accessible.name: qsTr("Discard the recording")
                                ToolTip.text: qsTr("Discard")
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: app.cancelVoiceRecording()
                            }
                            // Done: finish and review before sending.
                            IconButton {
                                objectName: "composerVoiceDoneButton"
                                implicitWidth: 24; implicitHeight: 24
                                iconName: "check"
                                iconSize: 15
                                enabled: voicePill.rec
                                         && voicePill.rec.recording
                                Accessible.name: qsTr("Finish and review")
                                ToolTip.text: qsTr("Done")
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                onClicked: {
                                    root.voiceWantsPreview = true
                                    app.voiceRecorder.stop()
                                }
                            }
                            IconButton {
                                objectName: "composerVoiceSendButton"
                                implicitWidth: 24; implicitHeight: 24
                                fill: true
                                iconName: "send"
                                iconSize: 14
                                enabled: voicePill.rec
                                         && voicePill.rec.recording
                                Accessible.name: qsTr("Send the voice message")
                                ToolTip.text: qsTr("Send")
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                // stop() finalizes and derives the waveform;
                                // ready() performs the send.
                                onClicked: app.voiceRecorder.stop()
                            }
                        }
                    }

                    VoicePreviewBar {
                        objectName: "composerVoicePreview"
                        Layout.alignment: Qt.AlignVCenter
                        visible: root.pendingVoice !== null
                        filePath: root.pendingVoice
                                  ? root.pendingVoice.filePath : ""
                        mime: root.pendingVoice ? root.pendingVoice.mime : ""
                        durationMs: root.pendingVoice
                                    ? root.pendingVoice.durationMs : 0
                        waveform: root.pendingVoice
                                  ? root.pendingVoice.waveform : []
                        onSendRequested: root.sendPendingVoice()
                        onDiscardRequested: root.discardPendingVoice()
                    }

                    // Accent-fill send (34px, radius 9).
                    IconButton {
                        id: sendButton
                        objectName: "composerSendButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 34; implicitHeight: 34
                        radius: AppTheme.radiusTile
                        fill: true
                        iconName: app.composer.isEditing ? "check" : "send"
                        iconSize: 20
                        enabled: app.composer.canSend
                        Accessible.name: app.composer.isEditing
                                         ? qsTr("Save edit") : qsTr("Send message")
                        ToolTip.text: app.composer.isEditing ? qsTr("Save") : qsTr("Send")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            root.submitComposer()
                            root.activeInput().forceActiveFocus()
                        }
                    }

                    // Send options: a chevron right of the send button (a split
                    // button, "another way to send this"), always present so
                    // Send later is never displaced. Not accent-filled, so it
                    // does not read as a second send. Its tooltip hides while
                    // the menu is open, as for the attach button.
                    IconButton {
                        id: sendOptionsButton
                        objectName: "composerSendOptionsButton"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: -AppTheme.spacing4
                        visible: app.scheduledSends !== null
                                 && root.composerButtonShown("sendOptions")
                        implicitWidth: 20; implicitHeight: 34
                        radius: AppTheme.radiusControl
                        iconName: "expand_more"
                        iconSize: 18
                        // This room has scheduled messages waiting.
                        active: root.pendingScheduledCount > 0
                        enabled: app.currentRoomId !== ""
                                 && !app.composer.isEditing
                        Accessible.name: qsTr("Send options")
                        ToolTip.text: root.pendingScheduledCount > 0
                                      ? qsTr("Send options (%1 scheduled)")
                                            .arg(root.pendingScheduledCount)
                                      : qsTr("Send options")
                        // See the note above this button.
                        ToolTip.visible: hovered && !sendOptionsMenu.visible
                        ToolTip.delay: 500
                        // The menu positions itself above the whole composer
                        // through bindings (see sendOptionsMenu); popup(x, y)
                        // computed on first click would read the lazily built
                        // menu's height as 0 and cover the bar.
                        onClicked: sendOptionsMenu.open()
                    }
                }
            }
            }
        }
    }

    // The compact row's "…" menu, anchored above the card like send options.
    AppMenu {
        id: composerOverflowMenu
        objectName: "composerOverflowMenu"
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
        // No width here: AppMenu refits itself from its rows on `opened`
        // (theCompactOverflowMenuDoesNotElideItsOwnRows). Formatting is
        // displaced here, not removed: toolbarRow stays visible when its toggle
        // hides, so the toggle must remain reachable. Same gate as the button.
        AppMenuItem {
            objectName: "composerOverflowFormattingItem"
            iconName: "edit_square"
            text: root.toolbarExpanded ? qsTr("Hide formatting")
                                       : qsTr("Show formatting")
            visible: root.composerButtonShown("formatting")
            height: visible ? implicitHeight : 0
            onTriggered: root.toolbarExpanded = !root.toolbarExpanded
        }
        // The mode switch too. `code` is a constant glyph in both states (the
        // icon subset has no compose symbol); the strings are the chip's own.
        AppMenuItem {
            objectName: "composerOverflowModeItem"
            iconName: "code"
            text: root.richMode ? qsTr("Switch to Markdown composing")
                                : qsTr("Switch to rich-text composing")
            visible: root.compactToolbarRow
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (app.settings)
                    app.settings.composerMode =
                        root.richMode ? "markdown" : "rich"
            }
        }
        AppMenuItem {
            objectName: "composerOverflowEmojiItem"
            iconName: "mood"
            text: qsTr("Emoji")
            enabled: app.currentRoomId !== ""
            onTriggered: root.openEmojiPicker(true)
        }
        AppMenuItem {
            objectName: "composerOverflowMediaItem"
            iconName: "gif_box"
            text: qsTr("GIFs and stickers")
            visible: app.gif.available || app.stickers.available
            height: visible ? implicitHeight : 0
            enabled: app.currentRoomId !== ""
            onTriggered: root.openMediaPicker(true)
        }
        AppMenuItem {
            objectName: "composerOverflowVoiceItem"
            iconName: "mic"
            text: qsTr("Record a voice message")
            visible: root.composerButtonShown("voice")
            height: visible ? implicitHeight : 0
            enabled: app.currentRoomId !== "" && app.composer.attachmentsSupported
            onTriggered: root.startVoiceMessage()
        }
    }

    AppMenu {
        id: sendOptionsMenu
        objectName: "composerSendOptionsMenu"
        // Parented to the composer card and right-aligned through the card's
        // width, which is observable, so it follows window resizes. `height` is
        // 0 until the content is built, so `y` is a binding too.
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
        AppMenuItem {
            objectName: "composerSendLaterMenuItem"
            iconName: "schedule"
            text: qsTr("Send later…")
            // Nothing to schedule: simply unavailable.
            enabled: app.composer.canSend
            onTriggered: root.openSendLater()
        }
        AppMenuItem {
            objectName: "composerScheduledListMenuItem"
            iconName: "format_list_bulleted"
            text: root.pendingScheduledCount > 0
                  ? qsTr("Scheduled messages (%1)")
                        .arg(root.pendingScheduledCount)
                  : qsTr("Scheduled messages")
            onTriggered: root.openScheduledList()
        }
    }

    // Presentation-only normalization for reply/thread preview lines, mirroring
    // matrix::preview::normalizePreviewText (mention link -> label,
    // U+2028/U+2029 -> space, collapsed whitespace). beginReply stores raw
    // visible text, so without this the strip shows matrix.to markdown and
    // multi-line targets grow the banner. Should move to that C++ choke point.
    function previewLine(text) {
        if (!text)
            return ""
        return String(text)
            .replace(/\[([^\]\n]{1,120})\]\(https:\/\/matrix\.to\/#\/[^)\s]{1,512}\)/g,
                     "$1")
            .replace(/[\u2028\u2029]/g, " ")
            .replace(/\s+/g, " ")
            .trim()
    }

    // Room name for the "Message #room" placeholder; people keep their plain
    // name.
    function roomDisplayName() {
        var room = app.roomList.findRoom(app.currentRoomId)
        var name = room && room.name ? room.name : qsTr("this room")
        return room && room.isDirect === true ? name : "#" + name
    }

    Connections {
        target: app.composer
        function onTextChanged() {
            if (input.text !== app.composer.text) input.text = app.composer.text
        }
        function onEditStateChanged() {
            // Put the caret at the end of an edit so the tail is visible.
            // Deferred: editStateChanged fires before the text syncs into the
            // field. A named function so Qt.callLater deduplicates.
            if (app.composer.isEditing)
                Qt.callLater(root.placeEditCaret)
        }
        // A restored draft puts the caret at the end, like an edit. Focus is
        // not taken: switching rooms must not steal the keyboard.
        function onRoomIdChanged() {
            Qt.callLater(root.placeDraftCaret)
        }
    }
    function placeEditCaret() {
        input.cursorPosition = input.length
        root.focusEditor()
    }
    function placeDraftCaret() {
        if (app.composer.isEditing)
            return
        input.cursorPosition = input.length
    }
}
