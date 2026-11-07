import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtMultimedia
import MatrixClient
import "ComposerBehavior.js" as ComposerBehavior
import "VimEditing.js" as VimEditing

Item {
    id: root
    AttachmentImageEditDialog { id: attachmentImageEditor }
    focus: false
    property bool composerExpanded: false
    function toggleComposerHeight() { composerExpanded = !composerExpanded }
    readonly property int editorCornerReserve: 26
    implicitHeight: (root.postBlocked ? blockedBar.implicitHeight
                                      : composerCol.implicitHeight)
                    + AppTheme.composerInsetBottom + AppTheme.spacing4

    readonly property bool roomReplaced: app.roomUpgrade.upgraded === true
    readonly property bool openFactsAreOurs:
        !!app.openRoomPermissions
        && app.openRoomPermissions.roomId === app.currentRoomId
        && app.openRoomPermissions.sendKnown === true
    readonly property bool roomInfoIsOurs:
        app.roomInfo.roomId === app.currentRoomId
    readonly property var postFacts: openFactsAreOurs
        ? app.openRoomPermissions : (roomInfoIsOurs ? app.roomInfo : null)
    readonly property bool postBlocked:
        roomReplaced || (!!postFacts && postFacts.canSendMessages === false)
    readonly property string postBlockedReason: {
        if (roomReplaced)
            return qsTr("This room has been replaced.")
        var why = postFacts ? postFacts.sendBlockedReason : ""
        return (typeof why === "string" && why.length > 0)
                ? why : qsTr("You do not have permission to post in this room.")
    }

    property bool toolbarExpanded: false

    readonly property bool compactInputRow: root.width > 0 && root.width < 460

    readonly property bool compactToolbarRow: root.width > 0 && root.width < 420
    readonly property bool tightToolbarRow: root.width > 0 && root.width < 360

    // A voice recording is in progress. Derived from app.voiceOwner, never
    // assigned: the recorder is shared with the thread composer, and a local
    // flag could let both believe they own it and send twice.
    readonly property bool voiceActive: app.voiceOwner === "room"

    property string attachmentNotice: ""
    property int emojiSelectionStart: 0
    property int emojiSelectionEnd: 0
    property int emojiCursorPosition: 0

    property var formatFlags: ({})
    function startVoiceMessage() {
            if (!app.startVoiceRecording("room")) {
                root.attachmentNotice =
                    app.voiceRecordingBusy()
                    ? qsTr("A recording is already in "
                           + "progress.")
                    : qsTr("Voice recording is unavailable.")
                noticeTimer.restart()
            }
    }

    function focusStagedAttachmentSend() {
        if (app.composer.hasAttachments && app.composer.canSend)
            Qt.callLater(function () { root.focusEditor() })
    }
    function _returnShouldSend(modifiers) {
        return ComposerBehavior.returnShouldSend(
            modifiers, app.settings.enterInsertsNewline,
            Qt.ControlModifier, Qt.ShiftModifier)
    }
    // Vim-style editing (Settings > Chat > Message box). The keymap lives in
    // app.composer.vim; this file only routes keys and applies its results.
    // Esc order in Insert: completion popup, then reply/edit/thread cancel,
    // then Normal. In Normal: a pending count/operator, then reply/edit/thread
    // cancel, then the key leaves the composer as before.
    readonly property bool vimEnabled: !!app.settings
                                       && app.settings.vimComposer === true
    readonly property bool vimNormal: root.vimEnabled && app.composer.vim.normal
    onVimEnabledChanged: if (!root.vimEnabled) app.composer.vim.enterInsert()
    function _vimEnterNormal(editor) {
        VimEditing.enterNormal(app.composer.vim, editor)
    }
    function _vimOverride(event) {
        return VimEditing.shortcutOverride(root.vimNormal, event)
    }
    function _vimKey(event, editor) {
        if (!root.vimNormal)
            return false
        var r = VimEditing.handleKey(app.composer.vim, event, editor,
                                     editor === richInput)
        if (!r)
            return false
        if (r.send) {
            root.submitComposer()
            editor.forceActiveFocus()
        }
        return true
    }
    function openEditMenuAtCaret(editor) {
        var r = editor.cursorRectangle
        root.focusEditor()
        root.prepareSpellMenu(r.x, r.y + r.height / 2)
        composerEditMenu.popup(editor, r.x, r.y + r.height)
    }
    function _plainReturn(event) {
        if (root._vimKey(event, input))
            return
        if (commandPopup.visible && !commandPopup.returnSends) {
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
    function _richReturn(event) {
        if (root._vimKey(event, richInput))
            return
        if (commandPopup.visible && !commandPopup.returnSends) {
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
    // Return/Enter on a focused toolbar or tray button presses it, as Space
    // does, instead of bubbling here and sending the staged files (C42-F01,
    // C42-D09). Plain Return only: Ctrl/Shift+Return keep their send/newline
    // meaning, and nothing outside this composer is affected.
    function _pressFocusedButton(modifiers, autoRepeat) {
        if ((modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0)
            return false
        var it = root.Window.activeFocusItem
        if (!it || it === root || typeof it.click !== "function"
                || typeof it.clicked === "undefined")
            return false
        if (it.enabled && !autoRepeat)
            it.click()
        return true
    }
    Keys.onReturnPressed: (event) => {
        if (root._pressFocusedButton(event.modifiers, event.isAutoRepeat)) {
            event.accepted = true
            return
        }
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
        if (root._pressFocusedButton(event.modifiers, event.isAutoRepeat)) {
            event.accepted = true
            return
        }
        if (!root._returnShouldSend(event.modifiers)
                || !app.composer.hasAttachments
                || !app.composer.canSend) {
            event.accepted = false
            return
        }
        event.accepted = true
        root.submitComposer()
    }
    readonly property bool inlineFormatAvailable:
        !root.richMode || root.formatFlags["inlineTarget"] !== false
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
    function _composerFormatFor(key, modifiers) {
        var ed = root.activeInput()
        return app.shortcuts.editorOverrideFor(
            key, modifiers, ed.selectionStart !== ed.selectionEnd)
    }
    signal timelineKeyRequested(string actionId)
    function _forwardTimelineKey(actionId) {
        if (commandPopup.visible || mentionPopup.visible || emojiPopup.visible)
            return false
        var page = actionId === "timeline.pageUp"
                   || actionId === "timeline.pageDown"
        if (page) {
            var ed = root.activeInput()
            var r = ed.cursorRectangle
            var first = ed.positionToRectangle(0)
            var last = ed.positionToRectangle(ed.length)
            var slack = r.height * 0.5
            var canMove = actionId === "timeline.pageUp"
                ? r.y > first.y + slack
                : r.y < last.y - slack
            if (canMove)
                return false
        }
        root.timelineKeyRequested(actionId)
        return true
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
    property string richTextAtLeave: ""
    function activeInput() { return root.richMode ? richInput : input }
    function submitComposer() {
        root.flushRichMirror()
        if (root.richMode)
            app.richComposer.sendDocument(richInput.textDocument)
        else
            app.composer.send()
    }
    readonly property int pendingScheduledCount: {
        if (!app.scheduledSends || app.currentRoomId === "")
            return 0
        var tick = app.scheduledSends.pendingCount
        return app.scheduledSends.pendingForRoom(app.currentRoomId).length
    }
    SendLaterDialog { id: sendLaterDialog }
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
        if (richMirrorTimer.running) {
            richMirrorTimer.stop()
            if (!root.richMode) {
                root.richSyncing = true
                app.composer.text =
                    app.richComposer.toMarkdown(richInput.textDocument)
                root.richSyncing = false
            }
        }
        root.spellUnderlines = []
        root.richSpellUnderlines = []
        plainAutocorrect.reset()
        richAutocorrect.reset()
        root.refreshRichBlank()
        if (root.richMode)
            richSpellTimer.restart()
        else
            spellTimer.restart()
        if (root.richMode) {
            root.richSyncing = true
            // What the markdown box showed, line for line. Returning with the
            // box untouched keeps the document: the text is its mirror, folded
            // at 80 columns, and a fold is not a line.
            if (app.composer.text !== root.richTextAtLeave)
                app.richComposer.loadMarkdownLines(richInput.textDocument,
                                                   app.composer.text)
            root.richSyncing = false
            mentionPopup.tokenActive = false
            mentionPopup.close()
            commandPopup.close()
            Qt.callLater(function () { richInput.forceActiveFocus() })
        } else {
            if (app.richComposer.toMarkdown(richInput.textDocument).trim()
                    === app.composer.text.trim())
                app.composer.text =
                    app.richComposer.markdownForPlainBox(richInput.textDocument)
            root.richTextAtLeave = app.composer.text
            mentionPopup.tokenActive = false
            mentionPopup.close()
            commandPopup.close()
            Qt.callLater(function () { root.focusEditor() })
        }
        refreshFormatState()
    }
    Connections {
        target: app.composer
        function onOwnMessageSent() { root.composerExpanded = false }
        function onTextChanged() {
            if (!root.richMode || root.richSyncing)
                return
            var current = app.richComposer.toMarkdown(richInput.textDocument)
            if (current.trim() === app.composer.text.trim())
                return
            root.richSyncing = true
            if (app.composer.isEditing)
                app.richComposer.loadMarkdownLines(richInput.textDocument,
                                                   app.composer.text)
            else
                app.richComposer.loadMarkdown(richInput.textDocument,
                                              app.composer.text)
            root.richSyncing = false
        }
    }
    readonly property int richMirrorSyncLimit: 2000
    Timer {
        id: richMirrorTimer
        interval: 150
        onTriggered: root.mirrorRichText()
    }
    function mirrorRichText() {
        richMirrorTimer.stop()
        if (!root.richMode || root.richSyncing)
            return
        root.richSyncing = true
        app.composer.text = app.richComposer.toMarkdown(richInput.textDocument)
        root.richSyncing = false
        root.commandPopupDismissed = false
        root.updateCommandPopupState()
        root.emojiPopupDismissed = false
        root.updateEmojiPopupState()
        root.checkEmojiAutoConvert(richInput)
    }
    function flushRichMirror() {
        if (richMirrorTimer.running)
            root.mirrorRichText()
    }
    function applyFormat(format) {
        if (root.richMode) {
            root.applyRichFormat(format, "")
            return
        }
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
        root.reseatCaret(richInput)
        refreshFormatState()
        root.refreshRichBlank()
    }
    // A format change widens the text around a caret that did not move, and
    // TextEdit keeps the old cursor rectangle until the position changes: Bold
    // left the caret drawn inside the last word. Step it off and back, keeping
    // the selection and its direction.
    function reseatCaret(field) {
        var start = field.selectionStart
        var end = field.selectionEnd
        var at = field.cursorPosition
        field.cursorPosition = at > 0 ? at - 1 : at + 1
        if (end === start)
            field.cursorPosition = at
        else if (at === start)
            field.select(end, start)
        else
            field.select(start, end)
    }
    function composerPopupAnchor(flick) {
        var p = flick.mapToItem(Overlay.overlay, 0, 0)
        var card = composerCard.mapToItem(Overlay.overlay, 0, 0)
        return Qt.point(p.x, card.y)
    }
    function updateRichMentionState() {
        if (!root.richMode)
            return
        if (app.currentRoomId === "" || root.vimNormal) {
            mentionPopup.tokenActive = false
            mentionPopup.close()
            return
        }
        var plain = richInput.getText(0, richInput.length)
        var tok = app.composer.mentionTokenAt(plain, richInput.cursorPosition)
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
            mentionPopup.tokenActive = true
            if (mentionPopup.shouldShow && !mentionPopup.visible)
                mentionPopup.open()
        } else {
            root.mentionTokenStart = -1
            mentionPopup.tokenActive = false
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
        richInput.insert(start, "<a href=\"https://matrix.to/#/"
                         + encodeURIComponent(userId) + "\">@"
                         + root.escapeHtmlText(name) + "</a> ")
        richInput.cursorPosition = start + name.length + 2
        mentionPopup.close()
        richInput.forceActiveFocus()
    }

    function activeEditor() {
        return root.richMode ? richInput : input
    }
    function focusEditor() {
        activeEditor().forceActiveFocus()
    }

    function focusForTyping() {
        if (!root.visible || app.currentRoomId === ""
                || app.eventContext.active || app.thread.active)
            return false
        const editor = activeEditor()
        if (!editor || !editor.visible || !editor.enabled || editor.readOnly)
            return false
        editor.forceActiveFocus()
        return true
    }
    property int composerFocusGeneration: 0
    function cancelPendingFocus() {
        ++root.composerFocusGeneration
        composerFocusSettle.stop()
    }
    Connections {
        target: app
        function onComposerFocusRequested(roomId) {
            const generation = ++root.composerFocusGeneration
            Qt.callLater(function () {
                if (generation === root.composerFocusGeneration
                        && roomId === app.currentRoomId)
                    root.focusForTyping()
            })
            composerFocusSettle.roomId = roomId
            composerFocusSettle.restart()
        }
    }
    Timer {
        id: composerFocusSettle
        property string roomId: ""
        interval: 250
        onTriggered: {
            if (roomId !== app.currentRoomId)
                return
            const editor = root.activeEditor()
            if (editor && editor.activeFocus)
                return
            const focused = root.Window.activeFocusItem
            if (focused && (focused instanceof TextInput || focused instanceof TextEdit))
                return
            root.focusForTyping()
        }
    }

    readonly property var hiddenComposerButtons:
        app.settings ? app.settings.hiddenComposerButtons : []
    function composerButtonShown(key) {
        return root.hiddenComposerButtons.indexOf(key) < 0
    }

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
        emojiSelectionStart = start + emoji.length
        emojiSelectionEnd = start + emoji.length
        emojiCursorPosition = start + emoji.length
        if (!root.richMode)
            app.composer.text = input.text
        if (!emojiPicker.opened || emojiPicker.closeAfterSelection)
            root.focusEditor()
    }

    EmojiPicker {
        id: emojiPicker
        objectName: "composerEmojiPicker"
        mode: "composer"
        closeAfterSelection: false
        onEmojiChosen: (emoji) => root.insertEmoji(emoji)
        onCustomChosen: (row) => {
            var editor = root.activeEditor()
            var start = Math.min(emojiSelectionStart, emojiSelectionEnd)
            if (emojiSelectionStart === emojiSelectionEnd)
                start = emojiCursorPosition
            root.insertEmoji(":" + (row.key || row.shortcode) + ": ")
            app.composer.bindEmoticonAt(start, row)
        }
        onAboutToHide: root.notePickerDismissed("emoji")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    property int mentionTokenStart: -1
    MentionPopup {
        id: mentionPopup
        suggestions: app.mentionSuggestions
        onChosen: (userId, displayName) => root.insertMention(userId, displayName)
        onVisibleChanged: root.refreshMentionHighlight()
    }

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
        if (comps.length > 0 && !root.commandPopupDismissed && !root.vimNormal
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

    property bool emojiPopupDismissed: false
    property bool emojiAutoConverting: false
    function checkEmojiAutoConvert(editor) {
        if (root.emojiAutoConverting)
            return
        root.emojiAutoConverting = true
        var pos = app.composer.maybeAutoConvertShortcodeBeforeCursor(
            editor.cursorPosition)
        root.emojiAutoConverting = false
        if (pos >= 0)
            editor.cursorPosition = pos
    }
    EmojiCompletionPopup {
        id: emojiPopup
        onChosen: (entry) => {
            var editor = root.activeInput()
            var pos = entry.kind === "unicode"
                ? app.composer.acceptUnicodeEmojiCompletionAt(
                      editor.cursorPosition, entry.emoji)
                : app.composer.acceptEmoticonCompletionAt(
                      editor.cursorPosition, entry)
            if (pos >= 0)
                editor.cursorPosition = pos
            emojiPopup.close()
        }
    }
    function updateEmojiPopupState() {
        if (commandPopup.visible || mentionPopup.visible) {
            emojiPopup.close()
            return
        }
        var editor = root.activeInput()
        if (!editor || !editor.activeFocus || app.currentRoomId === ""
                || root.vimNormal) {
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
    readonly property var openFacts:
        !!app.openRoomPermissions
        && app.openRoomPermissions.roomId === app.currentRoomId
        ? app.openRoomPermissions.commandFacts : ({})
    readonly property bool infoOnRoom: app.roomInfo.roomId === app.currentRoomId
    Binding {
        target: app.composer
        property: "commandPermissions"
        value: ({
            "kick": root.openFacts.kick !== undefined ? root.openFacts.kick
                    : (root.infoOnRoom ? app.roomInfo.canKick : true),
            "ban": root.openFacts.ban !== undefined ? root.openFacts.ban
                   : (root.infoOnRoom ? app.roomInfo.canBan : true),
            "unban": root.openFacts.unban !== undefined ? root.openFacts.unban
                     : (root.infoOnRoom ? app.roomInfo.canUnban : true),
            "invite": root.openFacts.invite !== undefined
                      ? root.openFacts.invite
                      : (root.infoOnRoom ? app.roomInfo.canInvite : true),
            "topic": root.openFacts.topic !== undefined ? root.openFacts.topic
                     : (root.infoOnRoom ? app.roomInfo.canEditTopic : true),
            "roomname": root.openFacts.roomname !== undefined
                        ? root.openFacts.roomname
                        : (root.infoOnRoom ? app.roomInfo.canEditName : true)
        })
    }
    property string lastMentionScanText: ""
    property int lastMentionScanCursor: -1
    function updateMentionState() {
        if (root.richMode)
            return
        if (input.text === root.lastMentionScanText
            && input.cursorPosition === root.lastMentionScanCursor)
            return
        root.lastMentionScanText = input.text
        root.lastMentionScanCursor = input.cursorPosition
        if (app.currentRoomId === "" || root.vimNormal) {
            mentionPopup.tokenActive = false
            mentionPopup.close()
            root.refreshMentionHighlight()
            return
        }
        var tok = app.composer.mentionTokenAt(input.text, input.cursorPosition)
        if (tok && tok.active === true) {
            root.mentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.currentRoomId
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            mentionPopup.anchorInputTop =
                root.composerPopupAnchor(inputFlick)
            mentionPopup.anchorWidth = inputFlick.width
            mentionPopup.tokenActive = true
            if (mentionPopup.shouldShow && !mentionPopup.visible)
                mentionPopup.open()
        } else {
            root.mentionTokenStart = -1
            mentionPopup.tokenActive = false
            mentionPopup.close()
        }
        root.refreshMentionHighlight()
    }
    property var mentionHighlightRanges: []
    function refreshMentionHighlight() {
        var ranges = []
        var live = app.composer.mentionRanges
        for (var k = 0; k < live.length; ++k)
            ranges.push({ start: live[k].start, length: live[k].length })
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

    readonly property bool spellActive: app.spell !== null
                                        && app.spell !== undefined
                                        && app.spell.available
                                        && app.spell.enabled
    property var spellUnderlines: []
    property var richSpellUnderlines: []
    property bool richBlank: true
    function refreshRichBlank() {
        root.richBlank = !app.richComposer
                         || app.richComposer.documentIsBlank(richInput.textDocument)
    }
    property string spellMenuWord: ""
    property int spellMenuStart: -1
    property int spellMenuLength: 0
    property var spellMenuSuggestions: []

    function spellEditorText() {
        return root.richMode ? richInput.getText(0, richInput.length) : input.text
    }
    function spellSkipRanges() {
        if (root.richMode)
            return app.richComposer.spellSkipRanges(richInput.textDocument)
        return app.composer.mentionRanges
    }

    readonly property bool autocorrectActive: root.spellActive
                                              && app.spell.autocorrectEnabled
    SpellAutocorrect {
        id: plainAutocorrect
        objectName: "composerPlainAutocorrect"
        editor: input
        active: root.autocorrectActive && !root.richMode
        textFn: function () { return input.text }
        skipFn: function () { return app.composer.mentionRanges }
    }
    SpellAutocorrect {
        id: richAutocorrect
        objectName: "composerRichAutocorrect"
        editor: richInput
        active: root.autocorrectActive && root.richMode
        textFn: function () { return richInput.getText(0, richInput.length) }
        skipFn: function () {
            return app.richComposer.spellSkipRanges(richInput.textDocument)
        }
        replaceFn: function (start, length, text) {
            app.richComposer.replaceRange(richInput.textDocument, start,
                                          length, text)
        }
    }

    function spellWindow(editor, flick) {
        var top = Math.max(0, flick.contentY - flick.height)
        var bottom = flick.contentY + 2 * Math.max(flick.height, 1)
        var from = editor.positionAt(0, top)
        var to = editor.positionAt(editor.width, bottom)
        return { from: from, to: Math.max(from + 1, to + 1) }
    }

    function spellUnderlineRects(editor, ranges) {
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var start = ranges[i].start
            var end = start + ranges[i].length
            var p = start
            var guard = 0
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
        var w = root.spellWindow(input, inputFlick)
        var ranges = app.spell.misspelledRanges(input.text,
                                                input.cursorPosition,
                                                app.composer.mentionRanges,
                                                w.from, w.to)
        root.spellUnderlines = root.spellUnderlineRects(input, ranges)
    }

    function refreshRichSpellUnderlines() {
        if (!root.spellActive || !root.richMode || richInput.length === 0) {
            if (root.richSpellUnderlines.length > 0)
                root.richSpellUnderlines = []
            return
        }
        var w = root.spellWindow(richInput, richFlick)
        var ranges = app.spell.misspelledRanges(root.spellEditorText(),
                                                richInput.cursorPosition,
                                                root.spellSkipRanges(),
                                                w.from, w.to)
        root.richSpellUnderlines = root.spellUnderlineRects(richInput, ranges)
    }

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
        var wrong = app.spell.misspelledRanges(text, -1, root.spellSkipRanges(),
                                               hit.start, hit.start + hit.length)
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
            mentionPopup.tokenActive = false
            mentionPopup.close()
            var discarded = root.voiceActive || root.pendingVoice !== null
            if (root.voiceActive)
                app.cancelVoiceRecording()
            root.voiceWantsPreview = false
            root.discardPendingVoice()
            if (discarded) {
                root.attachmentNotice =
                    qsTr("Voice recording discarded when you left the room.")
                noticeTimer.restart()
            }
        }
    }
    Connections {
        target: root.voiceActive ? app.voiceRecorder : null
        function onReady(filePath, mime, durationMs, waveform) {
            // Release ownership first, so a re-entrant signal does not find us
            // armed.
            app.endVoiceRecording()
            if (root.voiceWantsPreview) {
                root.voiceWantsPreview = false
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

    property var pendingVoice: null
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

    readonly property bool mediaPickerBothKinds:
        app.gif.available && app.stickers.available
    property string mediaPickerKind: "gif"
    function effectiveMediaKind() {
        if (root.mediaPickerKind === "sticker" && app.stickers.available)
            return "sticker"
        if (app.gif.available)
            return "gif"
        return app.stickers.available ? "sticker" : "gif"
    }
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
        onAboutToHide: root.notePickerDismissed("media")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

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

    function onStickerPicked(image) {
        var threadRoot = app.composer.sendThreadRootId || ""
        if (threadRoot.length > 0)
            app.stickers.sendToThread(app.currentRoomId, threadRoot, image)
        else
            app.stickers.sendToRoom(app.currentRoomId, image)
    }

    function onGifPicked(result) {
        var threadRoot = app.composer.sendThreadRootId || ""
        if (threadRoot.length > 0) {
            var replyTo = app.composer.replyingToEventId === threadRoot
                    ? "" : app.composer.replyingToEventId
            app.gifSend.sendToThread(app.currentRoomId, threadRoot, result,
                                     replyTo)
        } else {
            app.gifSend.sendToRoom(app.currentRoomId, result,
                                   app.composer.replyingToEventId)
        }
        app.composer.clearReplyTarget()
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

    function openPicker(dialog) {
        dialog.open()
    }

    NativeFileDialog {
        id: pickAttachmentsDialog
        purpose: "attach"
        title: qsTr("Attach files")
        currentFolder: app.defaultFileDialogFolder()
        fileMode: "openMany"
        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i)
                app.composer.addAttachment(selectedFiles[i])
            root.focusStagedAttachmentSend()
        }
    }

    NativeFileDialog {
        id: pickImageDialog
        purpose: "attach"
        title: qsTr("Send image")
        currentFolder: app.defaultFileDialogFolder()
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"),
                       qsTr("All files (*)") ]
        onAccepted: app.media.sendPickedImage(app.currentRoomId, selectedFile)
    }
    NativeFileDialog {
        id: pickFileDialog
        purpose: "attach"
        title: qsTr("Send file")
        currentFolder: app.defaultFileDialogFolder()
        onAccepted: app.media.sendPickedFile(app.currentRoomId, selectedFile)
    }
    AppMenu {
        id: legacyAttachMenu
        parent: composerCard
        x: 0
        y: -height - 4
        AppMenuItem {
            iconName: "image"
            text: qsTr("Send image…")
            onTriggered: root.openPicker(pickImageDialog)
        }
        AppMenuItem {
            iconName: "attach_file"
            text: qsTr("Send file…")
            onTriggered: root.openPicker(pickFileDialog)
        }
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

    AppMenu {
        id: attachMenu
        objectName: "composerAttachMenu"
        parent: composerCard
        x: 0
        y: -height - 4
        AppMenuItem {
            iconName: "attach_file"
            text: qsTr("Attach files…")
            onTriggered: root.openPicker(pickAttachmentsDialog)
        }
        AppMenuItem {
            objectName: "createPollMenuItem"
            iconName: "check_circle"
            text: qsTr("Create poll…")
            visible: app.composer.pollsSupported()
            onTriggered: createPollDialog.openDialog()
        }
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

    function openAttachFiles() {
        if (app.currentRoomId === "")
            return
        if (app.composer.attachmentsSupported)
            root.openPicker(pickAttachmentsDialog)
        else
            root.openPicker(pickFileDialog)
    }
    Shortcut {
        // bindingRevision is read inside the binding because sequenceFor()
        // creates no dependency.
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.emojiPicker")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
        onActivated: root.openEmojiPicker(false)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("composer.gifPicker")]
        }
        enabled: app.currentScreen === 1 && app.currentRoomId !== ""
                 && !app.timeline.searchActive
                 && (app.gif.available || app.stickers.available)
        onActivated: {
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

    readonly property int editLastMessageScanRows: 200
    function composerIsEmpty() {
        return root.richMode ? root.richBlank : (input.length === 0)
    }
    function editLastOwnMessage() {
        if (app.currentRoomId === "" || !app.timeline)
            return false
        if (!root.composerIsEmpty())
            return false
        if (app.composer.isEditing || app.composer.inThread
                || app.composer.hasAttachments)
            return false
        if (app.timeline.roomId !== app.currentRoomId)
            return false
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

    // Files dragged over the composer are queued (Rust backend). A sandboxed
    // (Flatpak) build is handed HOST paths it cannot open; the file manager
    // puts a FileTransfer portal key beside them, and the composer resolves the
    // files through it. The portal formats are accepted on their own too.
    DropArea {
        id: dropArea
        anchors.fill: parent
        enabled: app.composer.attachmentsSupported
        keys: ["text/uri-list", "application/vnd.portal.filetransfer",
               "application/vnd.portal.files"]
        function portalKey(drop) {
            var formats = ["application/vnd.portal.filetransfer",
                           "application/vnd.portal.files"]
            for (var i = 0; i < formats.length; ++i) {
                if (drop.formats.indexOf(formats[i]) >= 0)
                    return drop.getDataAsString(formats[i])
            }
            return ""
        }
        onDropped: (drop) => {
            var key = portalKey(drop)
            if (!drop.hasUrls && key === "") return
            app.composer.addDroppedAttachments(drop.hasUrls ? drop.urls : [],
                                               key)
            drop.accept(Qt.CopyAction)
        }
    }
    Connections {
        target: app.composer
        function onDroppedAttachmentsResolved() {
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

    Rectangle {
        id: blockedBar
        objectName: "composerBlockedBar"
        visible: root.postBlocked
        anchors {
            left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: AppTheme.composerInsetH
            rightMargin: AppTheme.composerInsetH
            bottomMargin: AppTheme.composerInsetBottom
        }
        implicitHeight: blockedRow.implicitHeight + AppTheme.spacing12 * 2
        height: implicitHeight
        radius: AppTheme.radiusLg
        color: AppTheme.chipInfoFill
        border.color: AppTheme.chipInfoBorder
        border.width: 1

        RowLayout {
            id: blockedRow
            anchors.fill: parent
            anchors.margins: AppTheme.spacing12
            spacing: AppTheme.spacingS

            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "info"
                size: 18
                color: AppTheme.chipInfoInk
            }
            Text {
                objectName: "composerBlockedReason"
                Layout.fillWidth: true
                text: root.postBlockedReason
                textFormat: Text.PlainText
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                font.family: AppTheme.uiFont
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            AppButton {
                objectName: "composerContinueButton"
                visible: root.roomReplaced
                text: qsTr("Continue in new room")
                kind: "primary"
                enabled: !app.roomUpgrade.busy
                onClicked: app.roomUpgrade.continueToSuccessor()
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Continue in the new room")
            }
        }
    }

    ColumnLayout {
        id: composerCol
        visible: !root.postBlocked
        anchors {
            left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: root.compactInputRow ? AppTheme.spacing8
                                             : AppTheme.composerInsetH
            rightMargin: root.compactInputRow ? AppTheme.spacing8
                                              : AppTheme.composerInsetH
            bottomMargin: AppTheme.composerInsetBottom
        }
        spacing: AppTheme.spacing6

        Label {
            visible: root.attachmentNotice.length > 0
            Layout.fillWidth: true
            text: root.attachmentNotice
            color: AppTheme.warning
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            wrapMode: Text.WordWrap
        }

        Flow {
            visible: app.composer.hasAttachments
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            Repeater {
                model: app.composer.attachments
                Rectangle {
                    id: attachmentChip
                    objectName: "composerAttachmentChip"
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

                        readonly property string previewSource:
                            model.previewSource || ""
                        readonly property bool hasPreview:
                            chipLayout.previewSource.length > 0
                        readonly property bool hasLocalFile:
                            model.localUrl.toString().length > 0
                        readonly property bool isGifChip:
                            (model.mime || "") === "image/gif"
                        readonly property bool gifAnimates:
                            isGifChip && hasLocalFile && MotionPolicy.allowed
                        readonly property bool isVideoChip:
                            (model.mime || "").indexOf("video/") === 0

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
                                         && !chipLayout.gifAnimates
                                source: visible ? chipLayout.previewSource : ""
                                sourceSize.width: 128
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            AnimatedImage {
                                objectName: "composerGifChipAnimation"
                                anchors.fill: parent
                                visible: chipLayout.gifAnimates
                                source: visible ? model.localUrl : ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                playing: visible
                                         && MotionPolicy.plays(gifChipHover.hovered)
                                         && !MotionPolicy.suspended
                                HoverHandler { id: gifChipHover }
                            }
                            Loader {
                                id: chipVideoLoader
                                anchors.fill: parent
                                active: chipLayout.isVideoChip
                                        && chipLayout.hasLocalFile
                                        && index < 4
                                        && !(typeof app !== "undefined" && app
                                             && app.softwareRenderer === true)
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
                                        onMediaStatusChanged: {
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
                                textFormat: Text.PlainText
                                HoverHandler { id: chipErrorHover }
                                ToolTip.visible: chipErrorHover.hovered
                                                 && model.state === "failed"
                                                 && (model.error || "").length > 0
                                ToolTip.delay: 500
                                ToolTip.text: model.error || ""
                            }
                        }
                        IconButton {
                            visible: model.isImage && model.state !== "dispatching"
                                     && model.previewSource
                                     && (model.mime || "") !== "image/svg+xml"
                            implicitWidth: 20; implicitHeight: 20
                            radius: AppTheme.radiusSm
                            iconName: "edit_square"
                            iconSize: 14
                            Accessible.name: qsTr("Edit image %1").arg(model.fileName)
                            onClicked: attachmentImageEditor.openFor(app.composer.attachments, index)
                        }
                        IconButton {
                            visible: model.state === "failed"
                                     && model.retryable !== false
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

        Item {
            Layout.fillWidth: true
            implicitHeight: composerCard.implicitHeight

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
                color: AppTheme.composerField
                border.color: AppTheme.composerBorder
                border.width: 1

            ColumnLayout {
                id: cardColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

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
                                     && app.composer.commandErrorBypassable
                            onClicked: app.composer.sendBypassingCommands()
                        }
                    }
                }

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
                            return root.previewLine(
                                app.composer.editOriginalText)
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

                        Rectangle {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 2
                            radius: 1
                            color: contextHover.hovered ? AppTheme.accentHover
                                                        : AppTheme.accent
                            Behavior on color { ColorAnimation { duration: 90 } }
                        }

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
                                     && root.inlineFormatAvailable
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
                    AppButton {
                        objectName: "composerFormat_underline"
                        visible: !root.tightToolbarRow
                        opacity: root.richMode ? 1 : 0
                        Accessible.ignored: !root.richMode
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        // U + COMBINING LOW LINE: AppButton sets its own font,
                        // so font.underline would not reach the label.
                        text: "U̲"
                        enabled: root.richMode && app.currentRoomId !== ""
                                 && root.inlineFormatAvailable
                        Accessible.name: qsTr("Underline")
                        ToolTip.text: qsTr("Underline")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.applyFormat("underline")
                    }
                    AppButton {
                        objectName: "composerFormat_orderedlist"
                        visible: !root.tightToolbarRow
                        opacity: root.richMode ? 1 : 0
                        Accessible.ignored: !root.richMode
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        text: "1."
                        enabled: root.richMode && app.currentRoomId !== ""
                        Accessible.name: qsTr("Numbered list")
                        ToolTip.text: qsTr("Numbered list")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.applyFormat("orderedlist")
                    }
                    AppButton {
                        objectName: "composerModeToggle"
                        minWidth: 84
                        visible: !root.compactToolbarRow
                        kind: "secondary"
                        size: "sm"
                        text: root.richMode ? qsTr("Rich text")
                                            : qsTr("Markdown")
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

                Rectangle {
                    objectName: "composerRowDivider"
                    visible: root.toolbarExpanded
                    Layout.fillWidth: true
                    Layout.leftMargin: 1
                    Layout.rightMargin: 1
                    implicitHeight: 1
                    color: AppTheme.border
                }

                RowLayout {
                    id: inputRow
                    objectName: "composerInputRow"
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12 + 2
                    Layout.rightMargin: AppTheme.spacing12 + 2
                    Layout.topMargin: AppTheme.spacing8 + 2
                    Layout.bottomMargin: AppTheme.spacing8 + 2
                    spacing: root.compactInputRow ? AppTheme.spacing4 + 2
                                                  : AppTheme.spacing8 + 2

                    IconButton {
                        objectName: "composerAttachButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
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
                            if (app.composer.pollsSupported()
                                    || root.compactInputRow)
                                attachMenu.open()
                            else
                                root.openPicker(pickAttachmentsDialog)
                        }
                        ToolTip.text: qsTr("Attach")
                        ToolTip.visible: hovered && !attachMenu.visible
                                         && !legacyAttachMenu.visible
                        ToolTip.delay: 500
                    }

                    IconButton {
                        objectName: "composerFormatToggleButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("formatting")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        id: formatToggleButton
                        iconName: ""
                        contentItem: Label {
                            objectName: "composerFormatToggleGlyph"
                            text: qsTr("Aa", "text-formatting button mark")
                            textFormat: Text.PlainText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.family: AppTheme.uiFont
                            font.pixelSize: 14
                            font.weight: AppTheme.weightStrong
                            color: {
                                if (!formatToggleButton.enabled)
                                    return AppTheme.textDisabled
                                if (formatToggleButton.active)
                                    return AppTheme.accent
                                return (formatToggleButton.hovered
                                        || formatToggleButton.down)
                                       ? AppTheme.textPrimary : AppTheme.icon
                            }
                        }
                        active: root.toolbarExpanded
                        Accessible.name: qsTr("Formatting")
                        ToolTip.text: root.toolbarExpanded
                                      ? qsTr("Hide formatting")
                                      : qsTr("Show formatting")
                        ToolTip.visible: hovered && !root.toolbarExpanded
                        ToolTip.delay: 500
                        onClicked: root.toolbarExpanded = !root.toolbarExpanded
                    }

                    VimModeBadge {
                        objectName: "composerVimMode"
                        labelObjectName: "composerVimModeLabel"
                        visible: root.vimEnabled
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        normal: root.vimNormal
                        pending: app.composer.vim.pending
                    }

                    Flickable {
                        id: richFlick
                        objectName: "composerRichInputFlick"
                        visible: root.richMode
                        Layout.fillWidth: true
                        Layout.minimumWidth: 120
                        Layout.alignment: Qt.AlignVCenter
                        Layout.maximumHeight: ComposerBehavior.editorHeight(
                            root.composerExpanded, AppTheme.scaled(140),
                            AppTheme.scaled(300))
                        implicitHeight: root.composerExpanded
                            ? Math.max(richInput.implicitHeight, AppTheme.scaled(220))
                            : richInput.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        function clampContentToTop() {
                            if (contentHeight <= height && contentY !== 0)
                                contentY = 0
                        }
                        onContentYChanged: {
                            clampContentToTop()
                            richSpellTimer.restart()
                        }
                        onContentHeightChanged: clampContentToTop()
                        onHeightChanged: clampContentToTop()
                        ScrollBar.vertical: AppScrollBar { thin: true }

                        TextArea.flickable: TextArea {
                            id: richInput
                            objectName: "composerRichInput"
                            readOnly: root.vimNormal
                            cursorDelegate: richIdleCaret.delegate
                            IdleCaret { id: richIdleCaret; block: root.vimNormal; field: richInput }
                            onWidthChanged: richSpellTimer.restart()
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
                            placeholderText: root.richBlank
                                             ? input.placeholderText : ""
                            placeholderTextColor: AppTheme.placeholderInk
                            color: AppTheme.composerText
                            font: app.textFontWithEmoji(AppTheme.uiFont,
                                                        AppTheme.scaled(14))
                            leftPadding: 2
                            rightPadding: root.editorCornerReserve
                            topPadding: AppTheme.spacing6
                            bottomPadding: AppTheme.spacing6
                            topInset: 0
                            bottomInset: 0
                            leftInset: 0
                            rightInset: 0
                            inputMethodHints: Qt.ImhNone
                            verticalAlignment: root.composerExpanded
                                               ? TextEdit.AlignTop : TextEdit.AlignVCenter
                            wrapMode: TextArea.Wrap
                            enabled: app.currentRoomId !== ""
                            background: Rectangle { color: "transparent" }
                            onTextChanged: {
                                richSpellTimer.restart()
                                root.refreshRichBlank()
                                if (root.richSyncing || !root.richMode) {
                                    richAutocorrect.reset()
                                    return
                                }
                                richAutocorrect.edited()
                                root.refreshFormatState()
                                root.updateRichMentionState()
                                if (richInput.length > root.richMirrorSyncLimit)
                                    richMirrorTimer.restart()
                                else
                                    root.mirrorRichText()
                            }
                            onSelectionStartChanged: root.refreshFormatState()
                            onSelectionEndChanged: root.refreshFormatState()
                            onCursorPositionChanged: {
                                root.refreshFormatState()
                                root.updateRichMentionState()
                                root.updateEmojiPopupState()
                                richAutocorrect.cursorMoved()
                                richSpellTimer.restart()
                            }
                            Keys.onReturnPressed: (event) => root._richReturn(event)
                            Keys.onEnterPressed: (event) => root._richReturn(event)
                            Keys.onShortcutOverride: (event) => {
                                if (root._vimOverride(event)) {
                                    event.accepted = true
                                    return
                                }
                                if (root._composerFormatFor(event.key,
                                                            event.modifiers) !== "")
                                    event.accepted = true
                            }
                            Keys.onUpPressed: (event) => {
                                if (root._vimKey(event, richInput))
                                    return
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
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onDownPressed: (event) => {
                                if (root._vimKey(event, richInput))
                                    return
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
                                if (root._vimKey(event, richInput))
                                    return
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
                                    event.accepted = true
                                    var next = richInput.nextItemInFocusChain(true)
                                    if (next && next !== richInput)
                                        next.forceActiveFocus(Qt.TabFocusReason)
                                }
                            }
                            Keys.onMenuPressed: (event) => {
                                event.accepted = true
                                root.openEditMenuAtCaret(richInput)
                            }
                            Keys.onEscapePressed: (event) => {
                                if (root._vimKey(event, richInput))
                                    return
                                if (commandPopup.visible) {
                                    root.commandPopupDismissed = true
                                    commandPopup.close()
                                    event.accepted = true
                                } else if (mentionPopup.visible) {
                                    mentionPopup.tokenActive = false
                                    mentionPopup.close()
                                    event.accepted = true
                                } else if (emojiPopup.visible) {
                                    root.emojiPopupDismissed = true
                                    emojiPopup.close()
                                    event.accepted = true
                                } else if (app.composer.isReplying
                                           || app.composer.isEditing
                                           || app.composer.inThread) {
                                    app.composer.cancelReplyOrEdit()
                                    event.accepted = true
                                } else if (root.vimEnabled && !root.vimNormal) {
                                    root._vimEnterNormal(richInput)
                                    event.accepted = true
                                } else {
                                    event.accepted = false
                                }
                            }
                            Keys.onPressed: (event) => {
                                if (root._vimKey(event, richInput))
                                    return
                                if (event.key === Qt.Key_F10
                                        && (event.modifiers & Qt.ShiftModifier)) {
                                    event.accepted = true
                                    root.openEditMenuAtCaret(richInput)
                                    return
                                }
                                if (richAutocorrect.handleKey(event))
                                    return
                                var timelineAction =
                                    app.shortcuts.timelineActionForKey(
                                        event.key, event.modifiers)
                                if (timelineAction !== ""
                                        && root._forwardTimelineKey(timelineAction)) {
                                    event.accepted = true
                                    return
                                }
                                if (event.matches(StandardKey.Paste)
                                        && app.composer.pasteFromClipboard()) {
                                    event.accepted = true
                                    return
                                }
                                var formatAction =
                                    root._composerFormatFor(event.key,
                                                            event.modifiers)
                                if (formatAction !== "" && !root.vimNormal) {
                                    event.accepted = true
                                    if (formatAction === "composer.toggleHeight")
                                        root.toggleComposerHeight()
                                    else
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
                        Layout.minimumWidth: 120
                        Layout.alignment: Qt.AlignVCenter
                        Layout.maximumHeight: ComposerBehavior.editorHeight(
                            root.composerExpanded, AppTheme.scaled(140),
                            AppTheme.scaled(300))
                        implicitHeight: root.composerExpanded
                            ? Math.max(input.implicitHeight, AppTheme.scaled(220))
                            : input.implicitHeight
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
                        onContentYChanged: {
                            clampContentToTop()
                            spellTimer.restart()
                        }
                        onContentHeightChanged: clampContentToTop()
                        onHeightChanged: clampContentToTop()
                        ScrollBar.vertical: AppScrollBar { thin: true }

                        TextArea.flickable: TextArea {
                        id: input
                        objectName: "composerInput"
                        readOnly: root.vimNormal
                        cursorDelegate: idleCaret.delegate
                        IdleCaret { id: idleCaret; block: root.vimNormal; field: input }
                        placeholderText: {
                            if (app.currentRoomId === "")
                                return qsTr("Select a room to start typing")
                            if (app.composer.isEditing)
                                return qsTr("Edit message…")
                            if ((app.composer.threadTargetRootId || "").length > 0)
                                return qsTr("Reply in thread…")
                            var _rev = root.roomListRevision
                            return qsTr("Message %1").arg(root.isolatedRoomName())
                        }
                        placeholderTextColor: AppTheme.placeholderInk
                        color: AppTheme.composerText
                        // A whole font with the colour emoji face behind the UI
                        // face: QML cannot express a families list, and Qt
                        // 6.8's automatic fallback picks a monochrome face.
                        font: app.textFontWithEmoji(AppTheme.uiFont, AppTheme.scaled(14))
                        leftPadding: 2
                        rightPadding: root.editorCornerReserve
                        topPadding: AppTheme.spacing6
                        bottomPadding: AppTheme.spacing6
                        topInset: 0
                        bottomInset: 0
                        leftInset: 0
                        rightInset: 0
                        inputMethodHints: Qt.ImhNone
                        verticalAlignment: root.composerExpanded
                                           ? TextEdit.AlignTop : TextEdit.AlignVCenter
                        wrapMode: TextArea.Wrap
                        enabled: app.currentRoomId !== ""
                        text: app.composer.text
                        onTextChanged: {
                            if (app.composer.text !== text) app.composer.text = text
                            root.refreshFormatState()
                            root.updateMentionState()
                            root.commandPopupDismissed = false
                            root.updateCommandPopupState()
                            root.emojiPopupDismissed = false
                            root.updateEmojiPopupState()
                            root.checkEmojiAutoConvert(input)
                            plainAutocorrect.edited()
                            spellTimer.restart()
                        }
                        onSelectionStartChanged: root.refreshFormatState()
                        onSelectionEndChanged: root.refreshFormatState()
                        onCursorPositionChanged: {
                            root.refreshFormatState()
                            root.updateMentionState()
                            root.updateEmojiPopupState()
                            plainAutocorrect.cursorMoved()
                            spellTimer.restart()
                        }
                        onWidthChanged: spellTimer.restart()
                        Keys.onReturnPressed: (event) => root._plainReturn(event)
                        Keys.onEnterPressed: (event) => root._plainReturn(event)
                        Keys.onShortcutOverride: (event) => {
                            if (root._vimOverride(event)) {
                                event.accepted = true
                                return
                            }
                            if (root._composerFormatFor(event.key,
                                                        event.modifiers) !== "")
                                event.accepted = true
                        }
                        Keys.onUpPressed: (event) => {
                            if (root._vimKey(event, input))
                                return
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
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onDownPressed: (event) => {
                            if (root._vimKey(event, input))
                                return
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
                            if (root._vimKey(event, input))
                                return
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
                                event.accepted = true
                                var next = input.nextItemInFocusChain(true)
                                if (next && next !== input)
                                    next.forceActiveFocus(Qt.TabFocusReason)
                            }
                        }
                        Keys.onMenuPressed: (event) => {
                            event.accepted = true
                            root.openEditMenuAtCaret(input)
                        }
                        Keys.onEscapePressed: (event) => {
                            if (root._vimKey(event, input))
                                return
                            if (commandPopup.visible) {
                                root.commandPopupDismissed = true
                                commandPopup.close()
                                event.accepted = true
                            } else if (mentionPopup.visible) {
                                mentionPopup.tokenActive = false
                                mentionPopup.close()
                                event.accepted = true
                            } else if (emojiPopup.visible) {
                                root.emojiPopupDismissed = true
                                emojiPopup.close()
                                event.accepted = true
                            } else if (app.composer.isReplying
                                       || app.composer.isEditing
                                       || app.composer.inThread) {
                                app.composer.cancelReplyOrEdit()
                                event.accepted = true
                            } else if (root.vimEnabled && !root.vimNormal) {
                                root._vimEnterNormal(input)
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: (mouse) => {
                                root.focusEditor()
                                root.prepareSpellMenu(mouse.x, mouse.y)
                                composerEditMenu.popup()
                            }
                        }
                        AppMenu {
                            id: composerEditMenu
                            objectName: "composerEditMenu"
                            menuWidth: AppTheme.menuWidthFlyout
                            AppMenuItem {
                                objectName: "composerSpellSuggestion0"
                                visible: root.spellMenuSuggestions.length > 0
                                enabled: visible
                                text: visible ? root.spellMenuSuggestions[0] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[0])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion1"
                                visible: root.spellMenuSuggestions.length > 1
                                enabled: visible
                                text: visible ? root.spellMenuSuggestions[1] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[1])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion2"
                                visible: root.spellMenuSuggestions.length > 2
                                enabled: visible
                                text: visible ? root.spellMenuSuggestions[2] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[2])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion3"
                                visible: root.spellMenuSuggestions.length > 3
                                enabled: visible
                                text: visible ? root.spellMenuSuggestions[3] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[3])
                            }
                            AppMenuItem {
                                objectName: "composerSpellSuggestion4"
                                visible: root.spellMenuSuggestions.length > 4
                                enabled: visible
                                text: visible ? root.spellMenuSuggestions[4] : ""
                                onTriggered: root.applySpellSuggestion(
                                                 root.spellMenuSuggestions[4])
                            }
                            AppMenuItem {
                                objectName: "composerSpellAdd"
                                visible: root.spellMenuWord !== ""
                                enabled: visible
                                text: qsTr("Add to dictionary")
                                onTriggered: app.spell.addToDictionary(
                                                 root.spellMenuWord)
                            }
                            AppMenuItem {
                                objectName: "composerSpellIgnore"
                                visible: root.spellMenuWord !== ""
                                enabled: visible
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
                            if (root._vimKey(event, input))
                                return
                            if (event.key === Qt.Key_F10
                                    && (event.modifiers & Qt.ShiftModifier)) {
                                event.accepted = true
                                root.openEditMenuAtCaret(input)
                                return
                            }
                            if (plainAutocorrect.handleKey(event))
                                return
                            var timelineAction =
                                app.shortcuts.timelineActionForKey(
                                    event.key, event.modifiers)
                            if (timelineAction !== ""
                                    && root._forwardTimelineKey(timelineAction)) {
                                event.accepted = true
                                return
                            }
                            if (event.matches(StandardKey.Paste)
                                    && app.composer.pasteFromClipboard()) {
                                event.accepted = true
                                return
                            }
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
                            var formatAction =
                                root._composerFormatFor(event.key,
                                                        event.modifiers)
                            if (formatAction !== "" && !root.vimNormal) {
                                event.accepted = true
                                if (formatAction === "composer.toggleHeight")
                                    root.toggleComposerHeight()
                                else
                                    root.applyFormat(
                                        formatAction.substring("composer.".length))
                            }
                        }
                        background: Rectangle { color: "transparent" }

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
                                color: Qt.alpha(AppTheme.danger, 0.85)
                            }
                        }

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
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("emoji")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "mood"
                        iconSize: 20
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Insert emoji")
                        ToolTip.text: qsTr("Emoji")
                        ToolTip.visible: hovered && !emojiPicker.visible
                        ToolTip.delay: 500
                        onClicked: root.openEmojiPicker(true)
                    }

                    IconButton {
                        id: mediaButton
                        objectName: "composerMediaButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
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
                        ToolTip.visible: hovered && !gifPicker.visible
                        ToolTip.delay: 500
                        onClicked: root.openMediaPicker(true)
                    }

                    IconButton {
                        objectName: "composerMicButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "mic"
                        iconSize: 20
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
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_horiz"
                        iconSize: 20
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
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
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
                                color: AppTheme.dangerFill
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
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
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

                    IconButton {
                        id: sendButton
                        objectName: "composerSendButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        implicitWidth: 34; implicitHeight: 34
                        radius: AppTheme.radiusTile
                        fill: true
                        fillColor: AppTheme.sendButton
                        fillInk: AppTheme.sendButtonInk
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

                    IconButton {
                        id: sendOptionsButton
                        objectName: "composerSendOptionsButton"
                        Layout.alignment: root.composerExpanded ? Qt.AlignBottom : Qt.AlignVCenter
                        Layout.leftMargin: -AppTheme.spacing4
                        visible: app.scheduledSends !== null
                                 && root.composerButtonShown("sendOptions")
                        implicitWidth: 20; implicitHeight: 34
                        radius: AppTheme.radiusControl
                        iconName: "expand_more"
                        iconSize: 18
                        active: root.pendingScheduledCount > 0
                        enabled: app.currentRoomId !== ""
                                 && !app.composer.isEditing
                        Accessible.name: qsTr("Send options")
                        ToolTip.text: root.pendingScheduledCount > 0
                                      ? qsTr("Send options (%1 scheduled)")
                                            .arg(root.pendingScheduledCount)
                                      : qsTr("Send options")
                        ToolTip.visible: hovered && !sendOptionsMenu.visible
                        ToolTip.delay: 500
                        onClicked: sendOptionsMenu.open()
                    }
                }
            }

            IconButton {
                id: composerHeightButton
                objectName: "composerHeightButton"
                readonly property Item editor: root.richMode ? richFlick : inputFlick
                visible: root.composerExpanded
                         || (root.richMode ? richInput.lineCount
                                           : input.lineCount) > 1
                x: cardColumn.x + inputRow.x + editor.x + editor.width - width
                y: cardColumn.y + inputRow.y + editor.y + 2
                z: 1
                width: 22; height: 22
                radius: AppTheme.radiusControl
                iconName: root.composerExpanded ? "unfold_less" : "unfold_more"
                iconSize: 16
                Accessible.name: root.composerExpanded
                                 ? qsTr("Collapse message box")
                                 : qsTr("Expand message box")
                ToolTip.text: Accessible.name
                ToolTip.visible: hovered
                onClicked: {
                    root.toggleComposerHeight()
                    Qt.callLater(function() { root.focusEditor() })
                }
            }
            }
        }
    }

    AppMenu {
        id: composerOverflowMenu
        objectName: "composerOverflowMenu"
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
        AppMenuItem {
            objectName: "composerOverflowFormattingItem"
            iconName: "edit_square"
            text: root.toolbarExpanded ? qsTr("Hide formatting")
                                       : qsTr("Show formatting")
            visible: root.composerButtonShown("formatting")
            height: visible ? implicitHeight : 0
            onTriggered: root.toolbarExpanded = !root.toolbarExpanded
        }
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
            objectName: "composerOverflowUnderlineItem"
            text: qsTr("Underline")
            visible: root.richMode && root.tightToolbarRow
            height: visible ? implicitHeight : 0
            enabled: visible && app.currentRoomId !== ""
                     && root.inlineFormatAvailable
            onTriggered: root.applyFormat("underline")
        }
        AppMenuItem {
            objectName: "composerOverflowOrderedListItem"
            text: qsTr("Numbered list")
            visible: root.richMode && root.tightToolbarRow
            height: visible ? implicitHeight : 0
            enabled: visible && app.currentRoomId !== ""
            onTriggered: root.applyFormat("orderedlist")
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
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
        AppMenuItem {
            objectName: "composerSendLaterMenuItem"
            iconName: "schedule"
            text: qsTr("Send later…")
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

    // Bumped whenever the room list's rows change, so bindings that read a
    // room through findRoom() (a Q_INVOKABLE, invisible to the binding engine)
    // re-evaluate. The placeholder read "Message #Empty Room" for the whole
    // life of a room that was named after it was opened.
    property int roomListRevision: 0
    Connections {
        target: app.roomList
        function onDataChanged() { root.roomListRevision++ }
        function onModelReset() { root.roomListRevision++ }
        function onRowsInserted() { root.roomListRevision++ }
    }
    // Room name for the "Message #room" placeholder; people keep their plain
    // name.
    // The name wrapped in Unicode isolates (LRI ... PDI) in an RTL UI, so the
    // "#" and an English name stay ONE left-to-right run instead of the "#"
    // landing on the far side of it ("Quotes lists tables spoilers# ...").
    function isolatedRoomName() {
        var n = root.roomDisplayName()
        return app.localization.rightToLeft ? "\u2066" + n + "\u2069" : n
    }
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
            if (app.composer.isEditing)
                Qt.callLater(root.placeEditCaret)
        }
        function onReplyStateChanged() {
            if (!app.composer.isReplying
                    || app.composer.roomId !== root.composerRoomSeen)
                return
            Qt.callLater(function () { root.focusEditor() })
        }
        function onRoomIdChanged() {
            Qt.callLater(root.placeDraftCaret)
        }
    }
    readonly property string composerRoomSeen:
        app.composer ? app.composer.roomId : ""
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
