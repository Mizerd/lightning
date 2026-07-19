import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// v0.5.9: unified composer bar — attach button, expanding multiline editor,
// Send — with an attachment tray (file picker, drag-and-drop, clipboard
// image paste) feeding the SDK send queue on the Rust backend. The HTTP
// backend keeps its legacy immediate image/file send menu. Enter sends,
// Shift+Enter inserts a newline; typing notifications are unchanged.
Rectangle {
    id: root
    color: AppTheme.surface
    implicitHeight: composerCol.implicitHeight + AppTheme.spacing12

    // Transient validation feedback ("folder rejected", "too large", …).
    property string attachmentNotice: ""
    property int emojiSelectionStart: 0
    property int emojiSelectionEnd: 0
    property int emojiCursorPosition: 0

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
    Menu {
        id: legacyAttachMenu
        MenuItem {
            text: qsTr("Send image…")
            onTriggered: pickImageDialog.open()
        }
        MenuItem {
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
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing12
        anchors.rightMargin: AppTheme.spacing12
        anchors.topMargin: AppTheme.spacing4
        anchors.bottomMargin: AppTheme.spacing8
        spacing: 2

        // Reply / Edit / Thread banner
        Rectangle {
            id: contextBar
            visible: app.composer.isReplying || app.composer.isEditing || app.composer.inThread
            Layout.fillWidth: true
            implicitHeight: contextRow.implicitHeight + 6
            color: AppTheme.cardElevated
            radius: 4
            RowLayout {
                id: contextRow
                anchors.fill: parent
                anchors.margins: 4
                spacing: 6
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
                ToolButton {
                    text: "✕"
                    onClicked: app.composer.cancelReplyOrEdit()
                    ToolTip.text: qsTr("Cancel")
                    ToolTip.visible: hovered
                }
            }
        }

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
                        Label {
                            visible: !model.isImage || model.localUrl.toString().length === 0
                            text: model.isImage ? "🖼" : "📎"
                            font.pixelSize: 16
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
                        ToolButton {
                            visible: model.state === "failed"
                            implicitWidth: 20; implicitHeight: 20
                            text: "↻"
                            Accessible.name: qsTr("Retry sending %1").arg(model.fileName)
                            onClicked: {
                                app.composer.attachments.retryAt(index)
                                app.composer.send()
                            }
                        }
                        ToolButton {
                            enabled: model.state !== "dispatching"
                            implicitWidth: 20; implicitHeight: 20
                            text: "✕"
                            Accessible.name: qsTr("Remove attachment %1").arg(model.fileName)
                            onClicked: app.composer.attachments.removeAt(index)
                        }
                    }
                }
            }
        }

        // ── Composer card (design shell): one bordered rounded card
        //    holding attach · input · emoji · GIF · send. ─────────────────
        Rectangle {
            id: composerCard
            Layout.fillWidth: true
            implicitHeight: composerRow.implicitHeight + AppTheme.spacing8
            radius: AppTheme.radiusLg
            color: AppTheme.inputBackground
            border.color: input.activeFocus ? AppTheme.focusRing
                                            : AppTheme.border
            border.width: 1

            RowLayout {
                id: composerRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing6
                    rightMargin: AppTheme.spacing6
                }
                spacing: AppTheme.spacing4

                ToolButton {
                    text: "＋"
                    font.pixelSize: 18
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
                    // Grows with content up to ~6 lines, then scrolls.
                    Layout.maximumHeight: 140
                    placeholderText: app.currentRoomId === ""
                                     ? qsTr("Select a room to start typing")
                                     : (app.composer.isEditing
                                        ? qsTr("Edit message…")
                                        : qsTr("Message %1").arg(
                                              root.roomDisplayName()))
                    wrapMode: TextArea.Wrap
                    enabled: app.currentRoomId !== ""
                    text: app.composer.text
                    onTextChanged: if (app.composer.text !== text) app.composer.text = text
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
                    // The card carries the chrome; the field itself is bare.
                    background: Rectangle { color: "transparent" }
                }

                ToolButton {
                    id: emojiButton
                    text: "☺"
                    font.pixelSize: 18
                    enabled: app.currentRoomId !== ""
                    Accessible.name: qsTr("Insert emoji")
                    ToolTip.text: qsTr("Emoji")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.openEmojiPicker()
                }

                // GIF keycap chip (design: mono, 1.5px border, radius 5).
                ToolButton {
                    id: gifButton
                    enabled: app.currentRoomId !== "" && app.gif.available
                    Accessible.name: qsTr("Insert a GIF")
                    ToolTip.text: app.gif.available ? qsTr("GIF")
                        : qsTr("GIFs are unavailable on this backend")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    implicitWidth: gifCap.implicitWidth + AppTheme.spacing12
                    implicitHeight: 26
                    contentItem: Label {
                        id: gifCap
                        text: qsTr("GIF")
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontCaption
                        font.weight: Font.Bold
                        color: gifButton.enabled ? AppTheme.textSecondary
                                                 : AppTheme.textDisabled
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 5
                        color: gifButton.hovered ? AppTheme.hover : "transparent"
                        border.color: AppTheme.borderStrong
                        border.width: 1
                    }
                    onClicked: root.openGifPicker()
                }

                // Accent send button (34 px, radius 9).
                Button {
                    id: sendButton
                    enabled: app.composer.canSend
                    Accessible.name: app.composer.isEditing
                                     ? qsTr("Save edit") : qsTr("Send message")
                    implicitWidth: 34
                    implicitHeight: 34
                    ToolTip.text: app.composer.isEditing ? qsTr("Save") : qsTr("Send")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    contentItem: Label {
                        text: app.composer.isEditing ? "✓" : "➤"
                        color: sendButton.enabled ? AppTheme.accentText
                                                  : AppTheme.textDisabled
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 9
                        color: !sendButton.enabled ? AppTheme.cardElevated
                               : sendButton.down ? AppTheme.accentPressed
                               : sendButton.hovered ? AppTheme.accentHover
                               : AppTheme.accent
                    }
                    onClicked: {
                        app.composer.send()
                        input.forceActiveFocus()
                    }
                }
            }
        }
    }

    // Room display name for the "Message #room" placeholder.
    function roomDisplayName() {
        var room = app.roomList.findRoom(app.currentRoomId)
        return room && room.name ? room.name : qsTr("this room")
    }

    Connections {
        target: app.composer
        function onTextChanged() {
            if (input.text !== app.composer.text) input.text = app.composer.text
        }
    }
}
