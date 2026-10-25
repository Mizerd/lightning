import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Managing a custom emoji/sticker pack: rename or remove an image, rename the
// pack, empty it. Nothing is optimistic: each action waits for the bridge, and
// on success the manager re-reads the authoritative pack, so the list (bound to
// the live model) always shows what the server holds. Room packs are
// power-level gated: canManagePack is the snapshot's recorded permission, and
// its absence is not permission. Without it the actions are absent and the
// reason is shown.
Dialog {
    id: root
    objectName: "stickerPackEditor"

    /// The manager, passed in so this component has no globals and can be
    /// tested.
    property var stickers: null
    /// The pack being edited; the manager's grid model is already narrowed to
    /// it.
    property string packId: ""
    property string packName: ""
    property bool canManage: false
    /// The control that opened the dialog; it gets the focus back on close
    /// (C41-D04, C41-F09).
    property Item restoreTo: null

    /// The last outcome, in the user's words rather than the bridge's category.
    property string notice: ""
    property bool noticeIsError: false

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    // Centred on a NOMINAL height, not the live one: the dialog's own height
    // moves with the notice and the delete confirmation, and a centring
    // anchor re-centred it on every change so the button under the pointer
    // moved (C41-D03).
    readonly property real nominalHeight: 520
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.max(AppTheme.spacing24,
                         Math.round((parent.height - nominalHeight) / 2)) : 0
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    padding: AppTheme.spacing16

    function openFor(id, name, manageable) {
        root.packId = id
        root.packName = name
        root.canManage = manageable
        root.notice = ""
        root.noticeIsError = false
        nameField.text = name
        renamingKey = null
        confirmingDelete = false
        removingKey = null
        open()
    }

    onClosed: {
        if (restoreTo)
            restoreTo.forceActiveFocus(Qt.TabFocusReason)
    }

    /// The image whose Remove was pressed once, by its EXACT key (`null` when
    /// none: "" is itself a key a malformed pack can hold); the second press
    /// removes it.
    property var removingKey: null

    /// The image being renamed, by exact key (`null` when none). One at a
    /// time, in place, so only one field competes for Return.
    property var renamingKey: null
    /// Why the name being typed cannot be used, "" when it can. Fed by the one
    /// visible rename field so the reason shows once, in the notice area.
    property string renameProblem: ""
    /// Deleting a pack takes two clicks and the second says what is destroyed.
    /// Not undoable: Matrix has no delete verb for either store, so the pack is
    /// emptied and its name dropped.
    property bool confirmingDelete: false

    // Export / import outcomes, in the same notice line as an edit's.
    Connections {
        target: root.stickers
        function onTransferFinished(kind, ok, category, added, skipped, failed, name) {
            if (kind === "emoji" || !root.visible)
                return
            root.noticeIsError = !ok
            root.notice = root.stickers.transferMessage(
                kind, ok, category, added, skipped, failed, name)
        }
    }

    NativeFileDialog {
        id: exportDialog
        purpose: "stickerpack"
        fileMode: "save"
        title: qsTr("Export pack")
        currentFolder: app.defaultFileDialogFolder()
        nameFilters: [qsTr("Sticker pack (*.zip)")]
        onAccepted: root.stickers.exportPack(root.packId, selectedFile)
    }
    NativeFileDialog {
        id: importDialog
        purpose: "stickerpack"
        fileMode: "open"
        title: qsTr("Import a sticker pack")
        currentFolder: app.defaultFileDialogFolder()
        nameFilters: [qsTr("Sticker pack (*.zip)")]
        onAccepted: root.stickers.importPack(selectedFile)
    }

    Connections {
        target: root.stickers
        function onEditFinished(ok, category, shortcode) {
            root.renamingKey = null
            root.removingKey = null
            root.confirmingDelete = false
            root.noticeIsError = !ok
            if (ok) {
                root.notice = qsTr("Saved.")
                return
            }
            // The bridge's categories in words; unknown ones get a generic
            // message.
            if (category === "shortcode_taken")
                root.notice = qsTr("That name is already used in this pack.")
            else if (category === "invalid_shortcode")
                root.notice = qsTr("That name cannot be used.")
            else if (category === "not_found")
                root.notice = qsTr("That is no longer in the pack.")
            else if (category === "forbidden")
                root.notice = qsTr("You do not have permission to change "
                                   + "this room's pack.")
            else
                root.notice = qsTr("That change could not be saved.")
        }
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // The pack's own name
        Label {
            Layout.fillWidth: true
            text: qsTr("Manage pack")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.canManage
            AppTextField {
                id: nameField
                Layout.fillWidth: true
                // Without a zero minimum the field would take its neighbour's
                // width.
                Layout.minimumWidth: 0
                placeholderText: qsTr("Pack name")
                enabled: !root.busy
            }
            AppButton {
                text: qsTr("Rename")
                size: "sm"
                enabled: !root.busy && nameField.text !== root.packName
                onClicked: root.stickers.renamePack(root.packId, nameField.text)
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.canManage && root.packId !== "user"
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Leave the name empty to use the default — for a "
                       + "room's pack that is the room's own name.")
        }

        // Not manageable: say why once.
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: !root.canManage
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: qsTr("This pack belongs to a room, and you do not have "
                       + "permission to change it here.")
        }

        Label {
            objectName: "stickerPackEditorNotice"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: root.notice.length > 0
            text: root.notice
            color: root.noticeIsError ? AppTheme.danger : AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }

        Label {
            objectName: "stickerRenameProblem"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: root.renamingKey.length > 0
                     && root.renameProblem.length > 0
            text: root.renameProblem
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
        }

        // The images. The band keeps its height when the pack is empty or
        // shrinks, so the dialog does not collapse around the footer.
        Item {
        Layout.fillWidth: true
        Layout.preferredHeight: 280
        ListView {
            id: imageList
            objectName: "stickerPackEditorList"
            anchors.fill: parent
            clip: true
            // Every image of the pack, not the picker's usage-narrowed grid.
            model: root.stickers ? root.stickers.editorImages : null
            // Arrow keys do not move a hidden current item under a focused
            // button; paging is done here (C41-F11).
            keyNavigationEnabled: false
            Keys.onPressed: (event) => {
                var page = Math.max(44, imageList.height - 44)
                var maxY = Math.max(0, imageList.contentHeight - imageList.height)
                if (event.key === Qt.Key_PageDown)
                    imageList.contentY = Math.min(maxY, imageList.contentY + page)
                else if (event.key === Qt.Key_PageUp)
                    imageList.contentY = Math.max(0, imageList.contentY - page)
                else if (event.key === Qt.Key_Home)
                    imageList.contentY = 0
                else if (event.key === Qt.Key_End)
                    imageList.contentY = maxY
                else
                    return
                event.accepted = true
            }
            ScrollBar.vertical: AppScrollBar {}
            SmoothWheelArea {}

            delegate: ItemDelegate {
                id: imageRow
                required property int index
                required property string shortcode
                required property string key
                required property bool keyValid
                required property string url
                required property bool isEmoticon
                required property bool isSticker

                // A focused control inside a row brings the row into view.
                function reveal() {
                    ListView.view.positionViewAtIndex(index, ListView.Contain)
                }

                width: ListView.view.width
                height: 44
                // The row itself does nothing; every action is a deliberate
                // button. Not `enabled: false`: enabled propagates and would
                // disable the per-image buttons and rename field.
                hoverEnabled: false
                background: null

                contentItem: RowLayout {
                    spacing: AppTheme.spacing8

                    Image {
                        id: packRowImage
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        sourceSize.width: 56
                        // Re-asked when the bytes land (see
                        // EmojiCompletionPopup): mxcImageSource returns empty
                        // on a miss.
                        property int resolveTick: 0
                        source: {
                            var _tick = resolveTick
                            return app.mediaBridge.supported && imageRow.url
                                ? app.mediaBridge.mxcImageSource(imageRow.url, 56)
                                : ""
                        }
                        Connections {
                            target: app.mediaBridge
                            function onMediaCached(cacheKey) {
                                if (cacheKey.endsWith(":" + imageRow.url))
                                    packRowImage.resolveTick++
                            }
                        }
                    }

                    // The name, or the field renaming it.
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: root.renamingKey !== imageRow.key
                        // A shortcode is remote text: never markup.
                        textFormat: Text.PlainText
                        text: ":" + imageRow.shortcode + ":"
                        color: AppTheme.textPrimary
                        elide: Label.ElideRight
                    }
                    // A key another client wrote outside the shortcode rules:
                    // shown as it is (never silently renamed), with the reason
                    // and a Rename to repair it.
                    Label {
                        objectName: "stickerInvalidKeyBadge"
                        visible: !imageRow.keyValid
                                 && root.renamingKey !== imageRow.key
                        textFormat: Text.PlainText
                        text: qsTr("Invalid name")
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textMeta
                        Accessible.description: qsTr(
                            "This name cannot be typed as a shortcode. "
                            + "Rename it to repair it.")
                    }
                    AppTextField {
                        id: renameField
                        objectName: "stickerRenameField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: root.renamingKey === imageRow.key
                        enabled: visible && !root.busy
                        // Why this name cannot be used, from the same rules
                        // the backend applies, so the answer is a sentence
                        // here and not a different name saved quietly.
                        readonly property string problem:
                            visible && root.stickers
                                ? root.stickers.shortcodeError(
                                      root.packId, imageRow.key, text)
                                : ""
                        color: problem.length > 0 ? AppTheme.danger
                                                  : AppTheme.textPrimary
                        Accessible.description: problem
                        Binding {
                            target: root
                            property: "renameProblem"
                            value: renameField.problem
                            when: renameField.visible
                        }
                        // The first Escape cancels the edit; the dialog stays
                        // (C41-F10).
                        Keys.onEscapePressed: (event) => {
                            root.renamingKey = null
                            event.accepted = true
                        }
                        onActiveFocusChanged: if (activeFocus) imageRow.reveal()
                        onVisibleChanged: {
                            if (visible) {
                                // A malformed key starts from a suggested
                                // repair; a valid one from itself.
                                text = imageRow.keyValid
                                    ? imageRow.key
                                    : root.stickers.sanitizeShortcode(imageRow.key)
                                forceActiveFocus()
                                selectAll()
                            }
                        }
                        onAccepted: if (problem.length === 0)
                            root.stickers.renameImageInPack(
                                root.packId, imageRow.key, text)
                    }

                    // Which kind the image is, so a pack's emoji are not
                    // mistaken for stickers (and vice versa).
                    Label {
                        textFormat: Text.PlainText
                        text: imageRow.isEmoticon
                              ? (imageRow.isSticker ? qsTr("Sticker and emoji")
                                                    : qsTr("Emoji"))
                              : qsTr("Sticker")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                    }

                    AppButton {
                        onActiveFocusChanged: if (activeFocus) imageRow.reveal()
                        text: root.renamingKey === imageRow.key
                              ? qsTr("Save") : qsTr("Rename")
                        kind: "ghost"
                        size: "sm"
                        visible: root.canManage
                        enabled: !root.busy
                                 && (root.renamingKey !== imageRow.key
                                     || renameField.problem.length === 0)
                        onClicked: {
                            if (root.renamingKey === imageRow.key)
                                root.stickers.renameImageInPack(
                                    root.packId, imageRow.key,
                                    renameField.text)
                            else
                                root.renamingKey = imageRow.key
                        }
                    }
                    AppButton {
                        objectName: "stickerRemoveButton"
                        onActiveFocusChanged: if (activeFocus) imageRow.reveal()
                        // Two presses, like Delete pack: a removal is not
                        // undoable and sat one stray click away (C41-D03).
                        readonly property bool armed:
                            root.removingKey === imageRow.key
                        text: armed ? qsTr("Confirm remove") : qsTr("Remove")
                        kind: armed ? "danger" : "ghost"
                        size: "sm"
                        visible: root.canManage
                        enabled: !root.busy
                        onClicked: {
                            if (armed)
                                root.stickers.removeImageFromPack(
                                    root.packId, imageRow.key)
                            else
                                root.removingKey = imageRow.key
                        }
                    }
                }
            }
        }

        Label {
            objectName: "stickerPackEditorEmpty"
            anchors.centerIn: parent
            visible: imageList.count === 0
            text: qsTr("This pack has no images.")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
        }
        }

        // Footer
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            AppButton {
                objectName: "stickerPackDeleteButton"
                visible: root.canManage
                text: root.confirmingDelete
                      ? qsTr("Delete everything in this pack")
                      : qsTr("Delete pack")
                kind: root.confirmingDelete ? "danger" : "ghost"
                size: "sm"
                enabled: !root.busy
                onClicked: {
                    if (root.confirmingDelete)
                        root.stickers.deletePack(root.packId)
                    else
                        root.confirmingDelete = true
                }
            }
            AppButton {
                objectName: "stickerPackExportButton"
                text: qsTr("Export…")
                kind: "ghost"
                size: "sm"
                enabled: !root.busy && !root.transferring
                         && imageList.count > 0
                onClicked: {
                    exportDialog.currentName =
                        (root.packName.length > 0 ? root.packName : qsTr("sticker-pack"))
                        + ".zip"
                    exportDialog.open()
                }
            }
            // Only your own pack takes imports; a room's pack is changed
            // through the room's own permissions.
            AppButton {
                objectName: "stickerPackImportButton"
                visible: root.packId === "user"
                text: qsTr("Import…")
                kind: "ghost"
                size: "sm"
                enabled: !root.busy && !root.transferring
                onClicked: importDialog.open()
            }
            Item { Layout.fillWidth: true }
            AppBusyIndicator {
                visible: root.busy || root.transferring
                implicitWidth: 18
                implicitHeight: 18
            }
            AppButton {
                text: qsTr("Done")
                kind: "ghost"
                onClicked: root.close()
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.confirmingDelete
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            // Matrix has no delete verb for either store, so this empties the
            // pack; it is not a redaction and old state stays in the room's
            // history.
            text: qsTr("This removes every image and the pack's name. It "
                       + "cannot be undone, and for a room's pack everyone "
                       + "in the room loses it.")
        }
    }

    readonly property bool busy: root.stickers ? root.stickers.editing : false
    readonly property bool transferring:
        root.stickers ? root.stickers.transferring : false
}
