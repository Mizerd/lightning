import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// The main message composer (SPEC-composer-settings-buttons §2): ONE card at
// the bottom of the timeline — a formatting toolbar row above the input row,
// separated by a 1px divider. The card floats on the timeline background
// (20px side padding, 16px bottom) and carries one of the app's four
// permitted shadows. The toolbar edits real markdown over the selection via
// MessageComposer.toggleFormat; the Rust send path parses it at send time.
// Attach / emoji / GIF / send keep their existing integrations; Enter sends,
// Shift+Enter inserts a newline; typing notifications are unchanged.
Item {
    id: root
    focus: false
    implicitHeight: composerCol.implicitHeight + AppTheme.spacing16
                    + AppTheme.spacing4

    // v0.7.1: the formatting toolbar collapses by default and rises above the
    // input row when the format toggle is pressed, so the compact composer
    // does not permanently spend a row on formatting controls. Format
    // keyboard shortcuts still apply regardless of visibility.
    property bool toolbarExpanded: false

    // 2026-08-18 tester report: at a narrow window the fixed-width buttons in
    // the input row left the text field ~10px wide ("net nematai pilnos
    // vienos raides ka typini"), which also made editing a message through
    // the composer impossible in a half-screen window. Below this width the
    // OPTIONAL controls (formatting toggle, emoji, GIF) leave the row and are
    // offered from the attach menu instead, so the field keeps a usable
    // width and no action is lost. Measured against this bar's own width, not
    // the input row's, so hiding a child can never feed back into the test.
    readonly property bool compactInputRow: root.width > 0 && root.width < 460

    // v0.7: a voice recording (or its finalization) is in progress — the
    // mic slot shows the recording pill instead of the idle button.
    //
    // DERIVED, never assigned: the recorder is shared with the thread
    // composer and app.voiceOwner is the single authority for which one owns
    // it. A local flag would let both composers believe they own the same
    // recording and send it twice (see AppController::voiceOwner).
    readonly property bool voiceActive: app.voiceOwner === "room"

    // Transient validation feedback ("folder rejected", "too large", …).
    property string attachmentNotice: ""
    property int emojiSelectionStart: 0
    property int emojiSelectionEnd: 0
    property int emojiCursorPosition: 0

    // Active-state flags for the toolbar chips, recomputed from the live
    // selection (MarkdownFormat::state on the C++ side).
    property var formatFlags: ({})
    // The voice button's action, shared with the compact-row "…" menu.
    function startVoiceMessage() {
            // Failure on this FIRST press is reported from
            // the return value: the failure Connections
            // only arms once this composer owns the
            // recorder. A press while the thread composer
            // is recording is REFUSED (never stolen), so
            // that recording keeps its owner and its
            // controls.
            if (!app.startVoiceRecording("room")) {
                // A refusal because something is already
                // recording is NOT "unavailable" — saying
                // so would send the user looking for a
                // hardware fault that does not exist.
                root.attachmentNotice =
                    app.voiceRecordingBusy()
                    ? qsTr("A recording is already in "
                           + "progress.")
                    : qsTr("Voice recording is unavailable.")
                noticeTimer.restart()
            }
    }

    function focusStagedAttachmentSend() {
        // Focus the INPUT, not the composer surface.
        //
        // This used to focus `root` and rely on its own Keys.onReturnPressed,
        // on the reasoning that the caret should be left alone. It did not
        // work: after dropping a file, Return did nothing until the message
        // box was clicked — which is the whole point of the affordance.
        // forceActiveFocus does not move the caret anyway, so focusing the
        // field costs nothing and makes Return take the path a click already
        // proves works.
        //
        // Wrapped in a closure rather than passed as a bare method reference:
        // Qt.callLater(root.forceActiveFocus) hands the engine an unbound
        // function, which is the kind of thing that silently does nothing.
        if (app.composer.hasAttachments && app.composer.canSend)
            Qt.callLater(function () { root.focusEditor() })
    }
    // Which modifier means SEND is now a setting. Default (false): Enter
    // sends, Shift+Enter inserts a newline. Inverted (true): Ctrl+Enter
    // sends, plain Enter inserts a newline.
    //
    // ONE predicate for every Return/Enter path in this file — the staged
    // attachment path below and the message box's own handler — or the
    // setting would apply to one of them and not the other.
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
    // Editor-context shortcuts. Qt sends a ShortcutOverride to the FOCUS
    // ITEM before it dispatches a shortcut; accepting it turns that shortcut
    // back into an ordinary key press delivered here. That is what lets
    // Ctrl+B mean Bold while this box has focus WITHOUT taking Ctrl+B away
    // from "toggle the conversation list" everywhere else — neither existing
    // binding had to be silently rebound. See ShortcutRegistry's header.
    //
    // The override must be claimed only for combinations we actually handle:
    // accepting everything would make this box swallow Ctrl+K, Ctrl+Q and
    // every other global shortcut while it has focus.
    // The registry answers this: it carries the EditorContext flag, so a
    // seventh editor shortcut is picked up here and in the THREAD composer
    // without either file being edited. The list of ids that used to live
    // here was a second copy of that flag.
    function _composerFormatFor(key, modifiers) {
        return app.shortcuts.editorActionForKey(key, modifiers)
    }
    // v0.9 composer modes. "markdown" is the historical source editor
    // (`input`); "rich" is the WYSIWYG editor (`richInput`), whose
    // QTextDocument is the canonical message and whose wire bodies come from
    // RichComposition through app.richComposer. The two editors are both
    // instantiated and visibility-exclusive, so every `input.` reference in
    // this file keeps working and the rich editor adds its own handlers.
    //
    // The composer's text property is the MARKDOWN MIRROR in both modes: the
    // rich editor pushes toMarkdown() on every edit, which is what keeps
    // canSend, typing notices, drafts and slash commands identical across
    // modes — and what makes a mode switch draft-preserving in both
    // directions (markdown -> rich loads the mirror; rich -> markdown needs
    // nothing, the markdown editor is already bound to it).
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
    // v0.9 (phase 11): the composed message WITHOUT sending, for the
    // scheduler — markdown mode hands over the expanded markdown, rich mode
    // the serialized document (both bodies from one document).
    readonly property int pendingScheduledCount: {
        if (!app.scheduledSends || app.currentRoomId === "")
            return 0
        var tick = app.scheduledSends.pendingCount // dependency
        return app.scheduledSends.pendingForRoom(app.currentRoomId).length
    }
    SendLaterDialog { id: sendLaterDialog }
    // The room's pending scheduled messages, with nothing to add to them.
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
    // Reverse sync: a draft restore, an edit begin or a post-send clear
    // rewrites the composer text from C++; the rich document must follow.
    // The markdown comparison skips the echo of the editor's own push.
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
        // Underline has no markdown form; the shortcut is a rich-mode key.
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
            // A new link needs a target. A selected URL is its own target;
            // anything else asks through the link dialog.
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
        // A list/quote/code toggle changes what is DRAWN without changing a
        // single character, so nothing else would refresh this.
        root.refreshRichBlank()
    }
    // Rich-mode @-mentions: the same token scan as markdown mode, over the
    // editor's PLAIN text, whose offsets are the document's cursor offsets.
    // Insertion writes a matrix.to anchor, which the serializer turns into
    // the formatted link plus an m.mentions id.
    function updateRichMentionState() {
        if (!root.richMode)
            return
        if (app.currentRoomId === "") {
            mentionPopup.close()
            return
        }
        var plain = richInput.getText(0, richInput.length)
        var tok = app.composer.mentionTokenAt(plain, richInput.cursorPosition)
        if (tok && tok.active === true) {
            root.mentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.currentRoomId
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            var p = richFlick.mapToItem(Overlay.overlay, 0, 0)
            mentionPopup.anchorInputTop = Qt.point(p.x, p.y)
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

    // The editor that owns the caret in the current mode. Every "give the
    // composer focus back" path goes through here, so rich mode never hands
    // focus (or an emoji) to the hidden markdown editor.
    function activeEditor() {
        return root.richMode ? richInput : input
    }
    function focusEditor() {
        activeEditor().forceActiveFocus()
    }

    // ── Composer buttons the user switched off ───────────────────────────
    //
    // Settings › Appearance › Message box › Message box buttons. Read through a property rather than a
    // Q_INVOKABLE so every `visible` binding below re-evaluates when the list
    // changes; composerButtonShown() reads that property, so calling it from
    // a binding still registers the dependency.
    readonly property var hiddenComposerButtons:
        app.settings ? app.settings.hiddenComposerButtons : []
    function composerButtonShown(key) {
        return root.hiddenComposerButtons.indexOf(key) < 0
    }

    // ── The picker buttons must TOGGLE ───────────────────────────────────
    //
    // Reported by a tester: pressing the emoji/GIF/sticker icon while its
    // panel is open made the panel blink and stay open instead of closing.
    //
    // Every picker carries `Popup.CloseOnPressOutside`, and the composer icon
    // that opens it is OUTSIDE the popup — so the press on that icon closes
    // the panel, and then the button's own onClicked (which arrives on the
    // RELEASE) opened it again. One gesture, close then open.
    //
    // A plain `if (picker.opened) picker.close()` cannot fix it: the popup
    // layer sees the press first, so by the time onClicked runs `opened`
    // already reads false. What identifies the gesture is that the panel was
    // dismissed a moment ago and the very next thing to happen is a click on
    // that same panel's own button. The window only has to outlast a press —
    // it is not a debounce and nothing depends on its exact value.
    //
    // `fromButton` is what keeps the menu items honest: the compact-window
    // menu entries and the screenshot-demo hooks are one-shot "open this"
    // actions, not toggles, and they pass nothing.
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
    // True when this press should CLOSE the panel rather than open one.
    // Covers both orderings, so it stays correct if Qt ever delivers the
    // press to the button before the popup layer: `opened` still true means
    // the popup did not close on our press and we close it ourselves.
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
        // The COMPOSER CARD, not the button: the picker sits directly on
        // top of the card with a hairline gap and its right edge lined up
        // with the card's. AnchoredPopup makes the card the popup's parent,
        // so Qt keeps the two rigid — the picker cannot lag or drift on a
        // window resize because it never moves relative to the card at all.
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
        // review M1: while the sticky picker stays open, focus STAYS with
        // it — stealing focus back here killed keyboard multi-pick (the
        // grid's Return/Space path needs the grid focused) and routed
        // Escape to the composer's cancel-edit handler instead of closing
        // the picker. The picker's onClosed already restores input focus.
        if (!emojiPicker.opened || emojiPicker.closeAfterSelection)
            root.focusEditor()
    }

    EmojiPicker {
        id: emojiPicker
        objectName: "composerEmojiPicker"
        mode: "composer"
        // Composing often means several emoji in a row — the picker stays
        // open after each insert (close with Escape, the toggle button, or
        // by clicking outside). Reaction pickers keep the close-on-pick
        // default: a reaction is a single choice.
        closeAfterSelection: false
        onEmojiChosen: (emoji) => root.insertEmoji(emoji)
        onAboutToHide: root.notePickerDismissed("emoji")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    // v0.7 outgoing @-mentions. The popup presents current-room members while
    // an @-token is active at the caret; the input keeps focus and forwards the
    // navigation keys. No Matrix protocol logic here — expansion + m.mentions
    // happen in MessageComposer at send time.
    property int mentionTokenStart: -1
    MentionPopup {
        id: mentionPopup
        suggestions: app.mentionSuggestions
        onChosen: (userId, displayName) => root.insertMention(userId, displayName)
        // Closing (Escape included) must drop the synthetic in-progress
        // range without re-running updateMentionState — see
        // refreshMentionHighlight's loop rationale.
        onVisibleChanged: root.refreshMentionHighlight()
    }

    // v0.9 slash commands: completion popup while the command word is being
    // typed. Same non-focus-taking construction as the mention popup; the
    // input forwards the navigation keys (command popup first — the two can
    // never be open together, since an active command word cannot contain an
    // @-token).
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
            var p = flick.mapToItem(Overlay.overlay, 0, 0)
            commandPopup.anchorInputTop = Qt.point(p.x, p.y)
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

    // MSC2545 shortcode completion. Cursor-driven, so it is refreshed from
    // the input's own signals rather than a model NOTIFY: a shortcode can be
    // anywhere in the text and only the editor knows where the caret is.
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
        // Never both: an active command word cannot contain a shortcode, and
        // a mention token cannot either.
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
            var p = flick.mapToItem(Overlay.overlay, 0, 0)
            emojiPopup.anchorInputTop = Qt.point(p.x, p.y)
            emojiPopup.anchorWidth = flick.width
            if (!emojiPopup.visible)
                emojiPopup.open()
        } else if (emojiPopup.visible) {
            emojiPopup.close()
        }
    }
    // v0.9 rich composer: the link-target prompt. Only a target that the
    // serializer would emit is accepted (the same policy the toolbar and
    // the wire share), so an unsafe scheme cannot enter the document.
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
    // Permission COURTESY hints for the completion rows, only when the
    // room-info controller happens to be inspecting the composer's room —
    // it is repointable (Space settings), so its booleans are meaningless
    // for any other room. A missing key counts as allowed; the server is
    // the enforcer either way.
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
    // Format-only rehighlights re-emit textChanged with an unchanged value
    // and cursor (QSyntaxHighlighter marks the document changed even for
    // pure format passes); rescanning then would loop the highlighter and
    // reopen a popup the user just dismissed with Escape. Only genuine
    // edits or cursor moves rescan.
    property string lastMentionScanText: ""
    property int lastMentionScanCursor: -1
    function updateMentionState() {
        // Rich mode scans its own editor (updateRichMentionState); the
        // hidden markdown field's text changes are the mirror, not typing.
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
            // @room's permission is NOT set here. It used to read
            // app.roomInfo.canNotifyRoom whenever that controller happened to
            // point at this room, which suppressed @room entirely: that value
            // is false while the roster loads, false after every
            // clearSnapshot(), and false on any backend that does not send the
            // key at all. The room-info panel is part of the default layout,
            // so the condition was usually TRUE and the answer usually FALSE.
            // The model now takes it from its own roster snapshot, for its own
            // room. See MentionSuggestionModel::onRoomMembersReceived.
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            // Anchor to the Flickable VIEWPORT, not the TextArea: the
            // field is reparented into the flickable's content item, so
            // once a long draft has scrolled (contentY > 0) the
            // TextArea's scene top sits above the visible composer and
            // the popup would detach (review M1).
            var p = inputFlick.mapToItem(Overlay.overlay, 0, 0)
            mentionPopup.anchorInputTop = Qt.point(p.x, p.y)
            mentionPopup.anchorWidth = inputFlick.width
            if (!mentionPopup.visible)
                mentionPopup.open()
        } else {
            root.mentionTokenStart = -1
            mentionPopup.close()
        }
        root.refreshMentionHighlight()
    }
    // v0.6.5 (SPEC §1q composer echo): the in-progress "@token" chip. This
    // concatenates the composer's authoritative send-time mentionRanges with
    // ONE synthetic presentation-only range covering the currently-typed
    // token, but ONLY while the mention popup is open — it is never written
    // back to app.composer.mentionRanges, so the C++ tokenizer/payload logic
    // that decides what actually gets sent is completely untouched.
    // MentionHighlighter applies one uniform accent/soft format to every
    // range regardless of source and clamps out-of-range values, so an
    // extra locally-computed range is safe to feed it.
    //
    // Deliberately NOT a declarative binding: rehighlighting can nudge the
    // input's layout/cursor signals, and a binding that reads
    // cursorPosition would then re-evaluate in a loop (and reopen the
    // popup Escape just closed). Explicit assignment from the two real
    // change sources breaks the cycle.
    property var mentionHighlightRanges: []
    function refreshMentionHighlight() {
        var ranges = app.composer.mentionRanges
        if (mentionPopup.visible && root.mentionTokenStart >= 0) {
            var len = input.cursorPosition - root.mentionTokenStart
            if (len > 0)
                ranges = ranges.concat([{ start: root.mentionTokenStart,
                                          length: len }])
        }
        // Assign only on a semantic change — an identical list would still
        // notify (fresh JS array) and rehighlight for nothing.
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

    // ---- Spell checking -------------------------------------------------
    //
    // WHY THE UNDERLINE IS DRAWN HERE AND NOT BY THE HIGHLIGHTER. The obvious
    // route is `QTextCharFormat::SpellCheckUnderline` through the
    // MentionHighlighter that is already attached to this document. It does
    // not work in Qt Quick: `QTextCharFormat::fontUnderline()` is
    // `underlineStyle() == SingleUnderline`, and the scene graph's text node
    // builds its decorations from the glyph run's boolean underline flag —
    // so any style other than SingleUnderline paints NOTHING, and even a
    // single underline would be drawn in the text's own colour rather than
    // the format's underline colour. Two Rectangles per misspelling are
    // fewer moving parts than either of those facts, and they are pixels we
    // control on every platform.
    //
    // Second reason, recorded so it is not "simplified" away: a QTextDocument
    // may carry only ONE QSyntaxHighlighter. QSyntaxHighlighter's
    // applyFormatChanges() CLEARS every format range outside the preedit area
    // before writing its own, so a second highlighter attached for spelling
    // would silently erase the mention ink (and vice versa, depending on
    // which ran last).
    readonly property bool spellActive: app.spell !== null
                                        && app.spell !== undefined
                                        && app.spell.available
                                        && app.spell.enabled
    // [{x, y, w}] in `input`'s own coordinates. The rectangles are children
    // of the TextArea, so they scroll with a long draft for free.
    property var spellUnderlines: []
    // The same, for the rich editor: the two editors never show at once,
    // but each keeps its own geometry.
    property var richSpellUnderlines: []
    // Whether the RICH editor is showing nothing at all. `length` counts
    // characters, so an empty ordered-list item leaves it at 0 and the
    // TextArea keeps painting its placeholder UNDER the "1." Qt draws.
    // Recomputed wherever the document can have changed.
    property bool richBlank: true
    function refreshRichBlank() {
        root.richBlank = !app.richComposer
                         || app.richComposer.documentIsBlank(richInput.textDocument)
    }
    // The word the context menu was opened on, and nothing else: cleared on
    // every open so a stale suggestion can never be applied to new text.
    property string spellMenuWord: ""
    property int spellMenuStart: -1
    property int spellMenuLength: 0
    property var spellMenuSuggestions: []

    // The text the checker sees for the current editor, and the ranges it
    // must leave alone: mention pills in both modes (a member's display
    // name is never "misspelled"), plus code fragments and code blocks in
    // rich mode. Rich positions are document positions — the same UTF-16
    // units getText() and positionToRectangle() use.
    function spellEditorText() {
        return root.richMode ? richInput.getText(0, richInput.length) : input.text
    }
    function spellSkipRanges() {
        // Rich mode: document-derived ranges ONLY. mentionRanges are offsets
        // into the Markdown MIRROR, which differ from the document's whenever
        // formatting is present; rich mention pills are anchors the document
        // scan already covers.
        if (root.richMode)
            return app.richComposer.spellSkipRanges(richInput.textDocument)
        return app.composer.mentionRanges
    }

    // [{x, y, w}] under every range, in `editor`'s own coordinates.
    function spellUnderlineRects(editor, ranges) {
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var start = ranges[i].start
            var end = start + ranges[i].length
            var p = start
            var guard = 0
            // One iteration for a word on one line, which is every word that
            // is not longer than the field. The guard bounds the pathological
            // case rather than trusting the geometry.
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
        // The caret position is passed so the word being typed is not
        // underlined mid-word, and the composer's own re-anchored mention
        // ranges are passed so a member's display name is never "misspelled".
        var ranges = app.spell.misspelledRanges(input.text,
                                                input.cursorPosition,
                                                app.composer.mentionRanges)
        root.spellUnderlines = root.spellUnderlineRects(input, ranges)
    }

    // Rich mode: the same policy over the document's plain text, with the
    // document's code and mention fragments excluded. Underlines are pixels
    // beside the editor; nothing is written into the document, so no
    // decoration can reach formatted_body, the clipboard, undo or a draft.
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

    // Fills spellMenu* for the word under the pointer, or clears them when
    // there is no misspelled word there. Called before the menu opens, so the
    // rows' `visible` bindings are already correct when it appears.
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
        // Only a word the dictionary actually REJECTS gets a menu. Offering
        // "did you mean" on a correctly spelled word is the kind of thing
        // that makes people turn a checker off.
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
            // Exactly the misspelled range, with its own character format
            // kept: a bold or linked word stays bold or linked.
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
        // Adding or ignoring a word makes every drawn underline stale.
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
            // A recording targets the room it was started in; switching
            // away discards it rather than sending into the wrong room.
            if (root.voiceActive)
                app.cancelVoiceRecording()
            // Same rule for one that is finished but not sent: it belongs to
            // the room it was recorded in, and its file is deleted rather
            // than left on disk.
            root.voiceWantsPreview = false
            root.discardPendingVoice()
        }
    }
    // Recorder results. target uses the lazy getter only while a recording
    // is active, so binding this block never constructs the recorder.
    Connections {
        target: root.voiceActive ? app.voiceRecorder : null
        function onReady(filePath, mime, durationMs, waveform) {
            // Release ownership FIRST: the send is this composer's, and a
            // re-entrant signal must not find us still armed.
            app.endVoiceRecording()
            // "Done" finalizes into the preview bar instead of sending; the
            // pill's own send button keeps the one-press path.
            if (root.voiceWantsPreview) {
                root.voiceWantsPreview = false
                // A preview that was never answered is replaced, not
                // stacked: its file is deleted before the new one takes the
                // slot, or it would sit in the temp dir until sign-out.
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

    // A finalized recording awaiting the user's decision, or null. Holding
    // it here means THIS composer owns the file: it is either handed to the
    // send queue or deleted, never left behind.
    property var pendingVoice: null
    // Set by "Done" so the next ready() lands in the preview rather than
    // going straight out.
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

    // ── GIFs and stickers, as one window ─────────────────────────────────
    readonly property bool mediaPickerBothKinds:
        app.gif.available && app.stickers.available
    // Which kind the single button opens. Session-scoped on purpose: the
    // strip inside the window is one click away, and a persisted tab would
    // be one more setting to explain.
    property string mediaPickerKind: "gif"
    function effectiveMediaKind() {
        if (root.mediaPickerKind === "sticker" && app.stickers.available)
            return "sticker"
        if (app.gif.available)
            return "gif"
        return app.stickers.available ? "sticker" : "gif"
    }
    // The one thing the composer button does. Both pickers report their
    // dismissal under the SAME key, so the toggle closes whichever of the
    // pair is showing.
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
    // Asked for from inside a picker's own GIFs/Stickers strip.
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
        // Our OWN close is not a dismissal the next click should undo — see
        // pickerButtonShouldClose. Without this, opening the GIF panel from
        // the emoji panel would arm a toggle-off on the emoji button, and the
        // swap below would arm one on the media button.
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
        // ONE key for both pickers: they are one window to the user, and the
        // single composer button has to toggle whichever of them is showing.
        onAboutToHide: root.notePickerDismissed("media")
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    // MSC2545 stickers. Its OWN picker, not a tab on the emoji one — see the
    // header of qml/StickerPicker.qml for why.
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

    // Send the chosen pack sticker as a real m.sticker, captured to the room
    // that is open RIGHT NOW. A pack image is already Matrix media, so there
    // is nothing to download and nothing to upload: the mxc goes straight
    // into the event and the SDK owns the send, the local echo and Retry.
    function onStickerPicked(image) {
        app.stickers.sendToRoom(app.currentRoomId, image)
    }

    // Download → validate → send the chosen GIF as Matrix media, captured to
    // THIS room so a later room switch cannot reroute it.
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

    // Legacy pickers (HTTP backend: immediate upload path).
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
        // Displaced by a narrow window, exactly as in the Rust-backend menu.
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

    // Rust-backend attach menu (v0.7): files plus poll creation. The
    // legacy menu above keeps the HTTP backend's immediate-upload paths.
    AppMenu {
        id: attachMenu
        objectName: "composerAttachMenu"
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
        // Only while the input row is too narrow to carry these as their own
        // buttons — the action is displaced, never removed.
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

    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op. All four popovers/dialogs targeted
    // here already have `id`s in THIS file's own scope, so no cross-file
    // descendant search is needed (contrast RoomsPanel.qml/MainScreen.qml).
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

    // Files dragged anywhere over the composer are queued (Rust backend).
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

        // ── Attachment tray ──────────────────────────────────────────────
        Flow {
            visible: app.composer.hasAttachments
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            Repeater {
                model: app.composer.attachments
                Rectangle {
                    radius: AppTheme.radiusSm
                    color: AppTheme.cardElevated
                    border.color: model.state === "failed" ? AppTheme.danger
                                                           : AppTheme.border
                    border.width: 1
                    implicitWidth: Math.min(chipLayout.implicitWidth + AppTheme.spacingS * 2, 280)
                    implicitHeight: chipLayout.implicitHeight + AppTheme.spacingS

                    RowLayout {
                        id: chipLayout
                        anchors.centerIn: parent
                        spacing: AppTheme.spacingXS

                        // What to point an Image at. A picked file resolves
                        // to its file:// URL; a PASTED image has no file at
                        // all and resolves to image://lightning-staged/<token>
                        // — which is why every pasted screenshot used to show
                        // a generic icon instead of itself.
                        readonly property string previewSource:
                            model.previewSource || ""
                        readonly property bool hasPreview:
                            chipLayout.previewSource.length > 0
                        readonly property bool hasLocalFile:
                            model.localUrl.toString().length > 0
                        // review L2: guarded — model roles can resolve
                        // undefined during delegate teardown.
                        readonly property bool isGifChip:
                            (model.mime || "") === "image/gif"
                        readonly property bool isVideoChip:
                            (model.mime || "").indexOf("video/") === 0

                        // Preview tile (live feedback): images get a real
                        // thumbnail, GIFs animate, and videos show their
                        // first frame through a muted, paused, per-chip
                        // player — bounded by the tray size and destroyed
                        // with the chip on remove/send.
                        Rectangle {
                            visible: chipLayout.hasPreview
                                     && (model.isImage || chipLayout.isVideoChip)
                            width: 64; height: 48
                            radius: AppTheme.radiusSm
                            color: AppTheme.surface
                            clip: true

                            Image {
                                anchors.fill: parent
                                // Everything that is not an animating GIF
                                // FILE, including a pasted image served by
                                // the staged provider — AnimatedImage needs
                                // a real URL to decode frames from and
                                // cannot animate an image:// source, so a
                                // pasted GIF shows its first frame here.
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
                                // review M3: BOUNDED decoders — only the
                                // first few video chips instantiate a
                                // poster player; a 30-video drop must not
                                // open 30 demuxers at once. Later chips
                                // keep the styled tile + play glyph.
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
                                            // Render exactly the first
                                            // frame, then hold (one-shot —
                                            // review L3).
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
                                // (beyond the decoder cap, or the file did
                                // not decode — review L3) still identify
                                // themselves.
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
                                // Remote or externally chosen text: never markup.
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

        // ── The composer card: toolbar row / divider / input row ─────────
        Item {
            Layout.fillWidth: true
            implicitHeight: composerCard.implicitHeight

            // Composer shadow — one of the four shadows the design budget
            // allows (composer card, quick-switcher modal, account popover,
            // slider thumb).
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
                // CENTRED, not parked at the top.
                //
                // The card is `anchors.fill: parent`, so its height is the
                // wrapper's, not its own implicitHeight — and this column had
                // no vertical anchor at all, which put it at y = 0 and every
                // pixel of slack at the bottom. That is what "the text is not
                // centred until you click it" actually was: the icons sat
                // high with the column, while the text — VCenter-aligned
                // inside its own row — looked correctly placed, so the two
                // disagreed by exactly the slack.
                //
                // Centring costs nothing when the card hugs its content
                // (slack is zero) and keeps the row aligned with the card
                // whenever it does not.
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                // v0.9 slash commands: the non-destructive refusal strip. An
                // unknown command or missing arguments lands here — the
                // draft stays in the box, nothing was sent, and "Send as
                // message" posts the text literally for the deliberate case.
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

                // Reply / Edit / Thread context strip.
                //
                // Element draws the message you are answering as the SAME
                // quote its timeline draws — an accent rule, the target on
                // its own line, one ellipsised line of body, clearly
                // subordinate. This was ONE interpolated string ("Replying
                // to X: Y") at a raw 11px in a single muted ink: sender and
                // quote indistinguishable, no way back to the message, and
                // the RAW body (see root.previewLine).
                //
                // The empty-Label ItemObservesViewport hazard documented in
                // MessageDelegate.qml does not apply here: this strip is a
                // sibling of the timeline, not a descendant, so a contentY
                // change never walks it.
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

                    // Only a REPLY has somewhere to go back to; editing and
                    // the thread banner describe a state, not a target
                    // event, so neither is offered as a control.
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

                    // The whole strip is the target, as in the timeline —
                    // and deliberately does NOT take focus: the caret stays
                    // in the field the user is typing in.
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

                        // The quote rule: the reply signifier the timeline
                        // quote uses, so the two read as one component
                        // rather than as two unrelated captions.
                        Rectangle {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 2
                            radius: 1
                            color: contextHover.hovered ? AppTheme.accentHover
                                                        : AppTheme.accent
                            Behavior on color { ColorAnimation { duration: 90 } }
                        }

                        // 2026-08-18 tester report #2: replying to an image
                        // shows its thumbnail while typing too — same bridge
                        // key the timeline quote uses. The rounded corner is
                        // BAKED into the cached bitmap by MediaImageProvider
                        // ("|shape:rsq:<permille of the edge>"), never a
                        // per-item MultiEffect mask (Avatar.qml records the
                        // per-frame cost of the mask approach).
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
                            // mediaSource() returns "" on a cache MISS and
                            // dispatches a fetch; nothing this binding depends
                            // on changes when the bytes land, so without the
                            // re-resolve below the thumbnail simply never
                            // appeared unless the image happened to be cached
                            // already. The timeline's reply quote has had the
                            // same re-ask since 2026-08-18 — the composer
                            // never got it, which is the "replying to an image
                            // still doesn't show the image above the text box"
                            // report.
                            //
                            // The re-ask is a COUNTER, never an assignment to
                            // `source`: assigning a bound property destroys
                            // its binding, and this thumbnail would then stay
                            // on the first image ever replied to.
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

                // Formatting toolbar row — exact order per spec §2. Collapsed
                // by default; the input-row toggle raises it above the input.
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
                    // v0.9 rich-only controls. The bundled icon font is a
                    // SUBSET with no underline / numbered-list glyphs, so
                    // these are text chips rather than tofu.
                    AppButton {
                        objectName: "composerFormat_underline"
                        visible: root.richMode
                        kind: "ghost"
                        size: "sm"
                        // AppButton's 72px minWidth is for a real button with
                        // a word on it; on a two-character chip beside 28px
                        // icon buttons it reads as a gap in the toolbar.
                        minWidth: 0
                        // U + COMBINING LOW LINE: AppButton's label sets its
                        // own font, so a font.underline here would not reach
                        // it; the glyph carries the underline itself.
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
                    // Mode switch. Draft-preserving in both directions (see
                    // richMode); the same switch answers /markdown. It sits
                    // LEFT of the spacer: the row's empty right side is the
                    // theme's raised surface the design-acceptance samples
                    // read, and must stay empty.
                    AppButton {
                        objectName: "composerModeToggle"
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

                // 1px divider between the two rows (only when the toolbar is
                // open — otherwise the compact composer is a single row).
                Rectangle {
                    objectName: "composerRowDivider"
                    visible: root.toolbarExpanded
                    Layout.fillWidth: true
                    Layout.leftMargin: 1
                    Layout.rightMargin: 1
                    implicitHeight: 1
                    color: AppTheme.border
                }

                // Input row — attach · format · input · emoji · media ·
                // mic · send · send options.
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
                                legacyAttachMenu.popup()
                                return
                            }
                            // Polls available → offer the menu; otherwise
                            // keep the direct one-click file picker. In a
                            // narrow window the menu is also where the
                            // displaced emoji/GIF actions live, so it has to
                            // open there regardless of poll support — without
                            // this they would be unreachable on a backend
                            // that has no polls.
                            if (app.composer.pollsSupported()
                                    || root.compactInputRow)
                                attachMenu.popup()
                            else
                                pickAttachmentsDialog.open()
                        }
                        ToolTip.text: qsTr("Attach")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }

                    // Format toggle — raises/closes the formatting toolbar.
                    IconButton {
                        objectName: "composerFormatToggleButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: the optional controls yield their
                        // width to the text field (see inputFlick). Also
                        // hidden outright when the user switched it off.
                        visible: !root.compactInputRow
                                 && root.composerButtonShown("formatting")
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "edit_square"
                        iconSize: 20
                        // Pure presentation toggle — usable regardless of the
                        // room state (the whole composer is hidden with no
                        // room anyway); the format buttons it reveals stay
                        // room-gated.
                        active: root.toolbarExpanded
                        Accessible.name: qsTr("Formatting")
                        ToolTip.text: root.toolbarExpanded
                                      ? qsTr("Hide formatting")
                                      : qsTr("Show formatting")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toolbarExpanded = !root.toolbarExpanded
                    }

                    // v0.9 rich composer: the WYSIWYG editor. Same growth,
                    // padding and inset rules as the markdown field beside
                    // it (see inputFlick's comments for why each exists);
                    // visibility-exclusive with it on root.richMode.
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
                            // debounce, same pixels, own geometry.
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
                            // The same context menu as the markdown editor:
                            // spelling rows for the word under the pointer,
                            // then the editing rows, which follow the active
                            // editor.
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
                            // Empty once the document draws anything at all,
                            // structure included (see richBlank).
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
                                // Clipboard images / file URLs become
                                // attachments exactly as in markdown mode;
                                // formatted TEXT pastes into the document
                                // (Qt keeps formatting, never markup — the
                                // serializer's whitelist is what reaches
                                // the wire).
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
                        // 2026-08-18 tester report ("kai sushrinkini app iki
                        // max net nematai pilnos vienos raides ka typini"):
                        // every other control in this row has a fixed width,
                        // so the text field was the only item left to absorb
                        // a narrow window and collapsed to ~10px at the
                        // application's own 640px minimum. It now keeps a
                        // readable floor and the OPTIONAL controls step aside
                        // instead (root.compactInputRow below), which is also
                        // what makes editing a message usable in a half-screen
                        // window.
                        Layout.minimumWidth: 120
                        Layout.alignment: Qt.AlignVCenter
                        // Grows with content up to ~6 lines (at the current
                        // text scale), then scrolls. The cap alone used to
                        // clamp a bare TextArea, which cannot scroll itself —
                        // lines past the cap painted outside the box and long
                        // drafts/edits became invisible. TextArea.flickable
                        // provides the scrolling and keeps the caret in view.
                        Layout.maximumHeight: AppTheme.scaled(140)
                        implicitHeight: input.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        // Content that FITS is never scrolled.
                        //
                        // Qt's TextArea-in-Flickable integration parks
                        // contentY NEGATIVE here — measured at -6 with the
                        // application font — which paints the single line six
                        // pixels below where the flickable actually sits. The
                        // flickable itself is centred correctly; every icon
                        // beside it is on the row's centre line; only the text
                        // is low. Clicking the field runs the integration's
                        // ensureVisible(cursorRectangle) and resets contentY
                        // to 0, which is precisely the reported "it moves to
                        // the right place when you click it".
                        //
                        // boundsBehavior does not cover this: it constrains
                        // DRAGGING, not a programmatic contentY. So the
                        // invariant is stated directly — with nothing to
                        // scroll, there is no scroll.
                        //
                        // Not visible without the app's own font: with the
                        // default family the content height happens to land
                        // where the integration leaves contentY at 0, which
                        // is why this measured perfectly centred in a test
                        // harness for hours.
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
                        // A whole FONT, not a family: it carries the UI face
                        // with the colour emoji face behind it, which is real
                        // Qt per-character fallback. QML's font value type
                        // cannot express a families list, and Qt 6.8's
                        // automatic fallback picks a monochrome face — so a
                        // mixed-text surface has to be given the font.
                        font: app.textFontWithEmoji(AppTheme.uiFont, AppTheme.scaled(14))
                        // Vertical padding set EXPLICITLY. This app picks up
                        // the platform Breeze style for TextArea (it even logs
                        // an assignment error from it), so the style's own
                        // padding decides where the first line sits — and it
                        // is not symmetric here: the placeholder and the text
                        // rendered visibly below the centre of a single-line
                        // composer while every icon beside them was centred.
                        // Equal top and bottom is what makes one line centre;
                        // the field still grows to the 6-line cap above.
                        topPadding: AppTheme.spacing6
                        bottomPadding: AppTheme.spacing6
                        // ...and the INSETS pinned too, which the padding
                        // above did not cover.
                        //
                        // Reported as "text is not centred when the room is
                        // opened, and gets centred when you click it". Padding
                        // alone cannot explain a change on FOCUS — it does not
                        // vary — but a style's background insets can, and this
                        // app picks up the platform Breeze style for TextArea.
                        // An inset shifts where the content sits inside the
                        // control, so a focus-dependent one moves the text and
                        // leaves every icon beside it where it was, which is
                        // exactly the offset in the screenshot.
                        //
                        // Zeroing them takes that decision away from the style
                        // in both states. NOT reproduced locally: an offscreen
                        // test does not load Breeze, and measuring the items
                        // showed the input, its flickable and the attach
                        // button sharing one centre line before and after
                        // focus — the item geometry was never the problem.
                        topInset: 0
                        bottomInset: 0
                        leftInset: 0
                        rightInset: 0
                        // ...and the text CENTRED in whatever height the
                        // field ends up with, rather than left to fall
                        // wherever padding puts it. When the field hugs its
                        // content this changes nothing — measured: field 32,
                        // text at y 6 height 20, centre 16 either way — but
                        // it is the difference between a position that is
                        // declared and one that is emergent, and every
                        // remaining explanation for the reported offset is a
                        // field taller than the line inside it.
                        // INPUT METHOD HINTS: DECLARED, AND DELIBERATELY EMPTY.
                        //
                        // `Qt.ImhNoPredictiveText` is the flag that turns the
                        // platform's own prediction, autocorrect and IME
                        // learning OFF, and `Qt.ImhSensitiveData` does the
                        // same thing by a different name (it also tells the
                        // platform not to remember what was typed). Neither
                        // belongs on a message composer, and neither was ever
                        // set here — this line exists so that stays true on
                        // purpose rather than by accident, and
                        // ComposerSpellContractTest fails the build if either
                        // appears on either composer.
                        //
                        // Qt.ImhNone is also the default, so this changes no
                        // behaviour today. What it cannot do is conjure
                        // Windows 11's hardware-keyboard text suggestions:
                        // those are delivered through a Text Services
                        // Framework text store, and Qt's Windows platform
                        // plugin implements IMM32 instead (qtbase 6.11's
                        // src/plugins/platforms/windows has no TSF file at
                        // all and qwindowsinputcontext.cpp is entirely Imm*
                        // and WM_IME_*). CJK/IME composition, dead keys and
                        // the on-screen keyboard all work through that path;
                        // Latin word suggestions do not exist for any Qt
                        // application. Spell checking is Lightning's own,
                        // through app.spell.
                        inputMethodHints: Qt.ImhNone
                        verticalAlignment: TextEdit.AlignVCenter
                        wrapMode: TextArea.Wrap
                        enabled: app.currentRoomId !== ""
                        text: app.composer.text
                        onTextChanged: {
                            if (app.composer.text !== text) app.composer.text = text
                            root.refreshFormatState()
                            root.updateMentionState()
                            // A dismissed command popup reopens once the
                            // text moves on (the standard completion feel).
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
                        // A reflow moves every rectangle; the ranges are
                        // unchanged but their geometry is not.
                        onWidthChanged: spellTimer.restart()
                        Keys.onReturnPressed: (event) => {
                            // While a completion popup is open, Return picks
                            // the highlighted entry instead of sending.
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
                            // Escape closes an open completion popup WITHOUT
                            // touching reply/edit state; only with both
                            // closed does it fall through to cancelling a
                            // reply/edit. A dismissed command popup stays
                            // closed until the text changes again.
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
                        // Right-click editing menu.
                        //
                        // Ours, because the default one's Paste is TextEdit's
                        // own — text only. A copied image carries BOTH the
                        // bitmap and its source URL, so right-click Paste sent
                        // the link while Ctrl+V sent the picture: the same
                        // gesture, two different messages, reported exactly
                        // that way. Both routes go through
                        // Composer::pasteFromClipboard() now and fall back to
                        // a plain text paste when there is no image.
                        //
                        // A MouseArea rather than a TapHandler: it CONSUMES
                        // the right press, which is what stops the built-in
                        // menu opening behind ours. Left presses are not
                        // accepted, so caret placement and selection are
                        // untouched.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: (mouse) => {
                                root.focusEditor()
                                // Resolved BEFORE the menu opens, so every
                                // spelling row's `visible` binding is already
                                // settled when it appears.
                                root.prepareSpellMenu(mouse.x, mouse.y)
                                composerEditMenu.popup()
                            }
                        }
                        AppMenu {
                            id: composerEditMenu
                            objectName: "composerEditMenu"
                            menuWidth: AppTheme.menuWidthFlyout
                            // Spelling rows first, because they are what the
                            // right-click was FOR when there is a squiggle
                            // under the pointer. Written out rather than
                            // generated: five fixed rows have no model-reset
                            // or insertion-order behaviour to reason about,
                            // and a hidden AppMenuItem already takes no
                            // height (see AppMenu's own contract).
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
                            // Clipboard images / file URLs become attachments;
                            // ordinary text pastes normally.
                            if (event.matches(StandardKey.Paste)
                                    && app.composer.pasteFromClipboard()) {
                                event.accepted = true
                                return
                            }
                            // Atomic mention delete: Backspace at a chip's
                            // trailing edge (or Delete at its leading edge)
                            // removes the whole mention in one keystroke.
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
                            // Formatting keys, delivered here because the
                            // ShortcutOverride above turned them back into
                            // ordinary presses. Registry ids map 1:1 onto
                            // applyFormat()'s existing keys, which the
                            // formatting toolbar already drives — the key is
                            // the only new part. Nothing this handler does not
                            // recognise is accepted.
                            var formatAction =
                                root._composerFormatFor(event.key,
                                                        event.modifiers)
                            if (formatAction !== "") {
                                event.accepted = true
                                root.applyFormat(
                                    formatAction.substring("composer.".length))
                            }
                        }
                        // The card is the visual container: the field itself
                        // is borderless and transparent — no inner pill.
                        background: Rectangle { color: "transparent" }

                        // Spell underlines. Coalesced behind one short timer
                        // rather than recomputed per keystroke: the geometry
                        // pass calls positionToRectangle per misspelling and
                        // there is no point running it between two letters of
                        // the same word.
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
                                // The conventional colour for this, and the
                                // only place in the composer that uses it —
                                // it marks nothing destructive, so it is the
                                // ink alone at reduced weight rather than a
                                // danger surface.
                                color: Qt.alpha(AppTheme.danger, 0.85)
                            }
                        }

                        // Inline mention chips over the semantic ranges the
                        // composer re-anchors on every edit.
                        MentionHighlighter {
                            document: input.textDocument
                            ranges: root.mentionHighlightRanges
                            accentColor: AppTheme.accent
                            softColor: AppTheme.accentSoft
                            // Named, because Qt 6.8 picks a monochrome face for emoji
                            // where 6.11 picks the colour one. Per-range, so the words
                            // around them keep the UI face.
                            emojiFontFamily: app.emojiFontFamily || ""
                        }
                        }
                    }

                    IconButton {
                        id: emojiButton
                        objectName: "composerEmojiButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: moves into the attach menu, which
                        // keeps the action reachable rather than dropping it.
                        // Switched off in settings it is gone from both.
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

                    // ── GIFs and stickers: ONE button, ONE window ─────
                    //
                    // They used to be two buttons opening two popups, and a
                    // tester asked for one clean window. The two pickers keep
                    // their own components (a pack is not a GIF — see the
                    // header of GifPicker.qml) and now carry a shared
                    // GIFs/Stickers strip at the top; swapMediaPicker() closes
                    // one and opens the other at the same anchor, with the
                    // same remembered size and no transitions, so it reads as
                    // the window changing tab rather than two windows.
                    //
                    // The mono "GIF" keycap this replaces was a design fix in
                    // its own right (the 2026-08-21 audit found it the only
                    // bordered chip in a row of borderless glyphs). A single
                    // button covering both kinds cannot carry a word for one
                    // of them, so it is a glyph like its neighbours — which is
                    // where that audit was heading anyway.
                    IconButton {
                        id: mediaButton
                        objectName: "composerMediaButton"
                        Layout.alignment: Qt.AlignVCenter
                        // Narrow window: moves into the attach menu. Hidden
                        // outright only when the user has switched it off —
                        // a backend with neither kind leaves it PRESENT and
                        // disabled with a tooltip that says why, which is
                        // what the GIF button it replaces did. A control that
                        // vanishes teaches nothing.
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
                        // Not while the picker is up: it opens ABOVE this
                        // button with the pointer still over it, so the
                        // tooltip stayed shown and sat half-hidden under the
                        // popup's bottom edge (reported with a screenshot).
                        ToolTip.visible: hovered && !gifPicker.visible
                        ToolTip.delay: 500
                        onClicked: root.openMediaPicker(true)
                    }

                    // v0.7: voice capture. Idle: the designed mic slot.
                    // Recording: a compact pill with a pulsing dot, the
                    // elapsed time, cancel, and send. app.voiceRecorder is
                    // created lazily on the FIRST press, so the audio
                    // backend never spins up for a session that never
                    // records; hardware/encoder absence surfaces honestly
                    // through the recorder's failed() signal on press.
                    IconButton {
                        objectName: "composerMicButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "mic"
                        iconSize: 20
                        // A recording in flight keeps its controls on screen
                        // whatever the setting says — the pill IS the way to
                        // stop it, and hiding it mid-record would strand a
                        // live capture.
                        // In a compact row the "…" button below carries
                        // this action (and the emoji and GIF ones the row
                        // has no space for) in one menu — reported: "when
                        // the window is fully narrowed add … or something
                        // that would show voice messages, emojis, gifs and
                        // all the rest instead of just voice messages".
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
                        // Compact rows only: everything the row had to hide
                        // (emoji, GIFs and stickers, the voice message, send
                        // later) lives in one menu here.
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
                        // NEVER touch app.voiceRecorder while idle: the
                        // property getter constructs the recorder (and the
                        // audio backend) on first access, and this pill is
                        // instantiated with the composer.
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
                                // A solid dot is a FILL: `danger` became an
                                // ink-only role on 2026-08-21 and resolves to
                                // a light rose on the dark themes.
                                color: AppTheme.dangerFill
                                // Solid while finalizing; pulsing while
                                // live (steady with reduced motion).
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
                            // Pause / resume (2026-08-18 tester report).
                            // A paused recording keeps the microphone and
                            // the file; only the capture is suspended, and
                            // the elapsed time freezes with it.
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
                            // Done: finish the recording and review it
                            // before deciding, instead of sending blind.
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
                                // the ready() handler below performs the send.
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

                    // Accent-fill send (34px, radius 9 — a rounded square).
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

                    // ── Send options ────────────────────────────────────
                    //
                    // "Send later" used to be a clock icon out in the row
                    // with the emoji and GIF buttons, where it read as one
                    // more unrelated glyph and disappeared entirely in a
                    // narrow window (`!compactInputRow`, and no menu entry
                    // stood in for it). Requested by Rokas as a chevron on
                    // the RIGHT of the send button: the split-button shape
                    // says "another way to send THIS", which is exactly what
                    // it is, and it rides beside a button that is always
                    // present so the action is never displaced.
                    //
                    // Narrower than the send button and deliberately NOT
                    // accent-filled: two filled blocks separated by a hairline
                    // would read as two sends.
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
                        // The badge the clock button carried: this room has
                        // messages waiting to go out.
                        active: root.pendingScheduledCount > 0
                        enabled: app.currentRoomId !== ""
                                 && !app.composer.isEditing
                        Accessible.name: qsTr("Send options")
                        ToolTip.text: root.pendingScheduledCount > 0
                                      ? qsTr("Send options (%1 scheduled)")
                                            .arg(root.pendingScheduledCount)
                                      : qsTr("Send options")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        // ABOVE THE WHOLE COMPOSER, right-aligned to this
                        // button. A bare popup() opened at the pointer, over
                        // the message box; anchoring 4 px above the BUTTON
                        // still covered the bar, because the button sits
                        // inside it (reported twice, with screenshots). The
                        // menu's bottom edge now sits above the composer's
                        // top edge, so nothing of the bar is hidden.
                        // open(), not popup(x, y): the menu carries its own
                        // position as BINDINGS (see sendOptionsMenu), because
                        // a value computed here on the first click reads the
                        // menu's height as 0 — its content is built lazily —
                        // and placed its top 4 px above the bar with the rest
                        // hanging down over it (reported three times).
                        onClicked: sendOptionsMenu.open()
                    }
                }
            }
            }
        }
    }

    // The compact row's "…" menu (see composerOverflowButton). Anchored
    // above the composer card exactly like the send-options menu.
    AppMenu {
        id: composerOverflowMenu
        objectName: "composerOverflowMenu"
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
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
        AppMenuItem {
            objectName: "composerOverflowSendLaterItem"
            iconName: "schedule"
            text: qsTr("Send later…")
            enabled: app.composer.canSend
            onTriggered: root.openSendLater()
        }
    }

    AppMenu {
        id: sendOptionsMenu
        objectName: "composerSendOptionsMenu"
        // Anchored the way the GIF picker is (AnchoredPopup): parented to the
        // composer CARD and right-aligned to it through the card's own width,
        // which is observable — so the menu follows a window resize. A
        // mapped-coordinate call is not observable and evaluated before the
        // button was laid out, which parked the menu at x = 0 (reported). `height`
        // is 0 until the content is first built, so `y` is a binding too.
        parent: composerCard
        x: Math.max(0, composerCard.width - width)
        y: -height - 4
        AppMenuItem {
            objectName: "composerSendLaterMenuItem"
            iconName: "schedule"
            text: qsTr("Send later…")
            // Nothing to schedule is not an error worth a dialog: the item
            // is simply unavailable, and the one below still is.
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

    // Presentation-only normalization for the reply / thread preview lines.
    //
    // The TIMELINE quote is normalized once in C++, at the ingest choke
    // point (matrix::preview::normalizePreviewText), and the comment there
    // records why: "a Lightning-sent mention contains the matrix.to markdown
    // link verbatim". MessageComposer::beginReply stores
    // `visibleTextForEvent(...).substring(0, 80)` instead, so this strip
    // rendered raw "[Name](https://matrix.to/#/@x:y)" — the exact regression
    // already fixed once for the timeline — and a multi-line target grew the
    // banner until it shoved the composer card upwards.
    //
    // These are the same three rules as the C++ function (mention link ->
    // its label, U+2028/U+2029 -> space, whitespace runs collapsed). The
    // real fix is to route beginReply through that choke point so there is
    // ONE implementation; until it is, the composer must not show markup.
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

    // Room display name for the "Message #room" placeholder (rooms get the
    // design's # prefix; people keep their plain name).
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
            // A long edit used to load with the caret at 0 and everything
            // past the height cap invisible. Put the caret at the end (as
            // Element does); the caret-following scroll brings the tail
            // into view. Deferred: editStateChanged fires before the
            // beginEdit text has synced into the field. Named function so
            // Qt.callLater's identity-based deduplication applies.
            if (app.composer.isEditing)
                Qt.callLater(root.placeEditCaret)
        }
        // 2026-08-18 tester report ("kai iseini ir grizti i chat tavo
        // typewriteri numeti i gala o ne i prieki"): leaving a room and
        // coming back restored the draft text but left the caret at
        // position 0, so the next character typed landed in FRONT of what
        // was already written. A restored draft comes back ready to
        // continue, exactly like the edit path above. Focus is deliberately
        // NOT taken here — switching rooms must not steal the keyboard from
        // wherever the user actually is.
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
