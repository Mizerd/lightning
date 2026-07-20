import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
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
        var p = emojiButton.mapToItem(Overlay.overlay,
                                      emojiButton.width / 2, 0)
        emojiPicker.anchorPoint = p
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
        input.forceActiveFocus()
    }

    EmojiPicker {
        id: emojiPicker
        mode: "composer"
        onEmojiChosen: (emoji) => root.insertEmoji(emoji)
        onClosed: Qt.callLater(input.forceActiveFocus)
    }

    function openGifPicker() {
        emojiPicker.close()
        var p = gifButton.mapToItem(Overlay.overlay, gifButton.width / 2, 0)
        gifPicker.anchorPoint = p
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

                        // Local image preview for picked files; icon otherwise.
                        Image {
                            visible: model.isImage && model.localUrl.toString().length > 0
                            source: visible ? model.localUrl : ""
                            sourceSize.width: 40
                            sourceSize.height: 40
                            fillMode: Image.PreserveAspectCrop
                            width: 32; height: 32
                            asynchronous: true
                        }
                        Icon {
                            visible: !model.isImage || model.localUrl.toString().length === 0
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

                // Formatting toolbar row — exact order per spec §2.
                RowLayout {
                    id: toolbarRow
                    objectName: "composerToolbarRow"
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

                // 1px divider between the two rows.
                Rectangle {
                    objectName: "composerRowDivider"
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
                        Accessible.name: qsTr("Attach files")
                        onClicked: {
                            if (app.composer.attachmentsSupported)
                                pickAttachmentsDialog.open()
                            else
                                legacyAttachMenu.popup()
                        }
                        ToolTip.text: qsTr("Attach")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
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
                        }
                        onSelectionStartChanged: root.refreshFormatState()
                        onSelectionEndChanged: root.refreshFormatState()
                        onCursorPositionChanged: root.refreshFormatState()
                        Keys.onReturnPressed: (event) => {
                            if (event.modifiers & Qt.ShiftModifier) {
                                event.accepted = false
                                return
                            }
                            event.accepted = true
                            app.composer.send()
                            input.forceActiveFocus()
                        }
                        Keys.onPressed: (event) => {
                            // Clipboard images / file URLs become attachments;
                            // ordinary text pastes normally.
                            if (event.matches(StandardKey.Paste)
                                    && app.composer.pasteFromClipboard()) {
                                event.accepted = true
                            }
                        }
                        // The card is the visual container: the field itself
                        // is borderless and transparent — no inner pill.
                        background: Rectangle { color: "transparent" }
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
