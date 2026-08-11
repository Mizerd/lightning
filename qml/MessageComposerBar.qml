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
    implicitHeight: composerCol.implicitHeight + AppTheme.spacing16
                    + AppTheme.spacing4

    // v0.7.1: the formatting toolbar collapses by default and rises above the
    // input row when the format toggle is pressed, so the compact composer
    // does not permanently spend a row on formatting controls. Format
    // keyboard shortcuts still apply regardless of visibility.
    property bool toolbarExpanded: false

    // Transient validation feedback ("folder rejected", "too large", …).
    property string attachmentNotice: ""
    property int emojiSelectionStart: 0
    property int emojiSelectionEnd: 0
    property int emojiCursorPosition: 0

    // Active-state flags for the toolbar chips, recomputed from the live
    // selection (MarkdownFormat::state on the C++ side).
    property var formatFlags: ({})
    function refreshFormatState() {
        formatFlags = app.composer.formatState(input.text,
                                               input.selectionStart,
                                               input.selectionEnd)
    }
    function applyFormat(format) {
        var result = app.composer.toggleFormat(format, input.text,
                                               input.selectionStart,
                                               input.selectionEnd)
        input.text = result.text
        app.composer.text = result.text
        input.select(result.selectionStart, result.selectionEnd)
        input.forceActiveFocus()
        refreshFormatState()
    }

    function openEmojiPicker() {
        emojiSelectionStart = input.selectionStart
        emojiSelectionEnd = input.selectionEnd
        emojiCursorPosition = input.cursorPosition
        // The COMPOSER CARD, not the button: the picker sits directly on
        // top of the card with a hairline gap and its right edge lined up
        // with the card's. AnchoredPopup makes the card the popup's parent,
        // so Qt keeps the two rigid — the picker cannot lag or drift on a
        // window resize because it never moves relative to the card at all.
        emojiPicker.anchorItem = composerCard
        emojiPicker.open()
    }

    function insertEmoji(emoji) {
        var start = Math.min(emojiSelectionStart, emojiSelectionEnd)
        var end = Math.max(emojiSelectionStart, emojiSelectionEnd)
        if (start === end) start = end = emojiCursorPosition
        input.remove(start, end)
        input.insert(start, emoji)
        input.cursorPosition = start + emoji.length
        app.composer.text = input.text
        // review M1: while the sticky picker stays open, focus STAYS with
        // it — stealing focus back here killed keyboard multi-pick (the
        // grid's Return/Space path needs the grid focused) and routed
        // Escape to the composer's cancel-edit handler instead of closing
        // the picker. The picker's onClosed already restores input focus.
        if (!emojiPicker.opened || emojiPicker.closeAfterSelection)
            input.forceActiveFocus()
    }

    EmojiPicker {
        id: emojiPicker
        mode: "composer"
        // Composing often means several emoji in a row — the picker stays
        // open after each insert (close with Escape, the toggle button, or
        // by clicking outside). Reaction pickers keep the close-on-pick
        // default: a reaction is a single choice.
        closeAfterSelection: false
        onEmojiChosen: (emoji) => root.insertEmoji(emoji)
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
    // Format-only rehighlights re-emit textChanged with an unchanged value
    // and cursor (QSyntaxHighlighter marks the document changed even for
    // pure format passes); rescanning then would loop the highlighter and
    // reopen a popup the user just dismissed with Escape. Only genuine
    // edits or cursor moves rescan.
    property string lastMentionScanText: ""
    property int lastMentionScanCursor: -1
    function updateMentionState() {
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
            app.mentionSuggestions.query = tok.query
            mentionPopup.query = tok.query
            var p = input.mapToItem(Overlay.overlay, 0, 0)
            mentionPopup.anchorInputTop = Qt.point(p.x, p.y)
            mentionPopup.anchorWidth = input.width
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

    function insertMention(userId, displayName) {
        var newCursor = app.composer.insertMention(userId, displayName,
                                                   root.mentionTokenStart,
                                                   input.cursorPosition)
        if (input.text !== app.composer.text)
            input.text = app.composer.text
        input.cursorPosition = newCursor
        mentionPopup.close()
        input.forceActiveFocus()
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() { mentionPopup.close() }
    }

    function openGifPicker() {
        emojiPicker.close()
        gifPicker.anchorItem = composerCard
        gifPicker.open()
    }

    GifPicker {
        id: gifPicker
        target: "room"
        onGifChosen: (result) => root.onGifPicked(result)
        onClosed: Qt.callLater(input.forceActiveFocus)
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
            font.pixelSize: 11
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
                            visible: chipLayout.hasLocalFile
                                     && (model.isImage || chipLayout.isVideoChip)
                            width: 64; height: 48
                            radius: AppTheme.radiusSm
                            color: AppTheme.surface
                            clip: true

                            Image {
                                anchors.fill: parent
                                visible: model.isImage && !chipLayout.isGifChip
                                source: visible ? model.localUrl : ""
                                sourceSize.width: 128
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            AnimatedImage {
                                anchors.fill: parent
                                visible: chipLayout.isGifChip
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
                            visible: !chipLayout.hasLocalFile
                                     || (!model.isImage && !chipLayout.isVideoChip)
                            name: model.isImage ? "image" : "attach_file"
                            size: 16
                        }
                        ColumnLayout {
                            spacing: 0
                            Label {
                                text: model.fileName
                                color: AppTheme.textPrimary
                                font.pixelSize: 11
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
                                font.pixelSize: 10
                                elide: Label.ElideRight
                                Layout.maximumWidth: 140
                            }
                        }
                        IconButton {
                            visible: model.state === "failed"
                            implicitWidth: 20; implicitHeight: 20
                            radius: 5
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
                            radius: 5
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
                spacing: 0

                // Reply / Edit / Thread banner (top row when active; keeps
                // the toolbar+input hierarchy intact below it).
                RowLayout {
                    id: contextRow
                    objectName: "composerContextBanner"
                    visible: app.composer.isReplying || app.composer.isEditing
                             || app.composer.inThread
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12 + 2
                    Layout.rightMargin: AppTheme.spacing8
                    Layout.topMargin: AppTheme.spacing4
                    spacing: AppTheme.spacing6
                    Label {
                        text: {
                            if (app.composer.isEditing)
                                return qsTr("Editing message")
                            if (app.composer.inThread)
                                return qsTr("Replying in thread: %1").arg(app.composer.threadPreview || "")
                            return qsTr("Replying to %1: %2")
                                        .arg(app.composer.replyingToSender || qsTr("someone"))
                                        .arg(app.composer.replyingToPreview || "")
                        }
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    IconButton {
                        implicitWidth: 22; implicitHeight: 22
                        radius: 5
                        iconName: "close"
                        iconSize: 14
                        Accessible.name: qsTr("Cancel")
                        onClicked: app.composer.cancelReplyOrEdit()
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
                            radius: 6
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
                            radius: 6
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

                // Input row — attach · input · emoji · GIF · mic · send.
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
                        radius: 6
                        iconName: "add_circle"
                        iconSize: 22
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Attach files or create a poll")
                        onClicked: {
                            if (!app.composer.attachmentsSupported) {
                                legacyAttachMenu.popup()
                                return
                            }
                            // Polls available → offer the menu; otherwise
                            // keep the direct one-click file picker.
                            if (app.composer.pollsSupported())
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
                        implicitWidth: 28; implicitHeight: 28
                        radius: 6
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

                    TextArea {
                        id: input
                        objectName: "composerInput"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        // Grows with content up to ~6 lines, then scrolls.
                        Layout.maximumHeight: 140
                        placeholderText: {
                            if (app.currentRoomId === "")
                                return qsTr("Select a room to start typing")
                            if (app.composer.isEditing)
                                return qsTr("Edit message…")
                            return qsTr("Message %1").arg(root.roomDisplayName())
                        }
                        placeholderTextColor: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(14)
                        wrapMode: TextArea.Wrap
                        enabled: app.currentRoomId !== ""
                        text: app.composer.text
                        onTextChanged: {
                            if (app.composer.text !== text) app.composer.text = text
                            root.refreshFormatState()
                            root.updateMentionState()
                        }
                        onSelectionStartChanged: root.refreshFormatState()
                        onSelectionEndChanged: root.refreshFormatState()
                        onCursorPositionChanged: {
                            root.refreshFormatState()
                            root.updateMentionState()
                        }
                        Keys.onReturnPressed: (event) => {
                            // While the mention popup is open, Return picks the
                            // highlighted member instead of sending.
                            if (mentionPopup.visible) {
                                mentionPopup.accept()
                                event.accepted = true
                                return
                            }
                            if (event.modifiers & Qt.ShiftModifier) {
                                event.accepted = false
                                return
                            }
                            event.accepted = true
                            app.composer.send()
                            input.forceActiveFocus()
                        }
                        Keys.onUpPressed: (event) => {
                            if (mentionPopup.visible) {
                                mentionPopup.moveUp()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onDownPressed: (event) => {
                            if (mentionPopup.visible) {
                                mentionPopup.moveDown()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onTabPressed: (event) => {
                            if (mentionPopup.visible) {
                                mentionPopup.accept()
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Keys.onEscapePressed: (event) => {
                            // Escape closes the mention popup WITHOUT touching
                            // reply/edit state; only when it is closed does it
                            // fall through to cancelling a reply/edit.
                            if (mentionPopup.visible) {
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
                        }
                        // The card is the visual container: the field itself
                        // is borderless and transparent — no inner pill.
                        background: Rectangle { color: "transparent" }

                        // Inline mention chips over the semantic ranges the
                        // composer re-anchors on every edit.
                        MentionHighlighter {
                            document: input.textDocument
                            ranges: root.mentionHighlightRanges
                            accentColor: AppTheme.accent
                            softColor: AppTheme.accentSoft
                        }
                    }

                    IconButton {
                        id: emojiButton
                        objectName: "composerEmojiButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: 6
                        iconName: "mood"
                        iconSize: 22
                        enabled: app.currentRoomId !== ""
                        Accessible.name: qsTr("Insert emoji")
                        ToolTip.text: qsTr("Emoji")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.openEmojiPicker()
                    }

                    // GIF keycap (design: mono 11/700, 1.5px border, radius 5
                    // — a bordered text chip, not an icon).
                    AbstractButton {
                        id: gifButton
                        objectName: "composerGifButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: gifCap.implicitWidth + AppTheme.spacing8
                        implicitHeight: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.Button
                        enabled: app.currentRoomId !== "" && app.gif.available
                        Accessible.name: qsTr("Insert a GIF")
                        ToolTip.text: app.gif.available ? qsTr("GIF")
                            : qsTr("GIFs are unavailable on this backend")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        contentItem: Item {
                            implicitWidth: gifCap.implicitWidth
                            implicitHeight: 28
                            Label {
                                id: gifCap
                                anchors.centerIn: parent
                                text: qsTr("GIF")
                                font.family: AppTheme.monoFont
                                font.pixelSize: 11
                                font.weight: Font.Bold
                                leftPadding: 4
                                rightPadding: 4
                                topPadding: 2
                                bottomPadding: 2
                                color: gifButton.enabled ? AppTheme.icon
                                                         : AppTheme.textDisabled
                            }
                        }
                        background: Item {
                            Rectangle {
                                objectName: "composerGifKeycap"
                                anchors.centerIn: parent
                                width: gifCap.implicitWidth
                                height: gifCap.implicitHeight
                                radius: 5
                                color: gifButton.hovered ? AppTheme.hover
                                                         : "transparent"
                                border.color: AppTheme.borderStrong
                                border.width: 1.5
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -3
                            radius: 8
                            color: "transparent"
                            border.color: AppTheme.focusRing
                            border.width: 2
                            visible: gifButton.visualFocus
                        }
                        onClicked: root.openGifPicker()
                    }

                    // Voice capture has no backend yet: the control keeps the
                    // designed slot with the app's honest unavailable state.
                    IconButton {
                        objectName: "composerMicButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 28; implicitHeight: 28
                        radius: 6
                        iconName: "mic"
                        iconSize: 22
                        enabled: false
                        Accessible.name: qsTr("Voice messages are not available yet")
                        ToolTip.text: qsTr("Voice messages are not available yet")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }

                    // Accent-fill send (34px, radius 9 — a rounded square).
                    IconButton {
                        id: sendButton
                        objectName: "composerSendButton"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 34; implicitHeight: 34
                        radius: 9
                        fill: true
                        iconName: app.composer.isEditing ? "check" : "send"
                        iconSize: 19
                        enabled: app.composer.canSend
                        Accessible.name: app.composer.isEditing
                                         ? qsTr("Save edit") : qsTr("Send message")
                        ToolTip.text: app.composer.isEditing ? qsTr("Save") : qsTr("Send")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            app.composer.send()
                            input.forceActiveFocus()
                        }
                    }
                }
            }
            }
        }
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
    }
}
