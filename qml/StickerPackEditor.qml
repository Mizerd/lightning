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

    /// The last outcome, in the user's words rather than the bridge's category.
    property string notice: ""
    property bool noticeIsError: false

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    padding: AppTheme.spacing16

    function openFor(id, name, manageable) {
        root.packId = id
        root.packName = name
        root.canManage = manageable
        root.notice = ""
        root.noticeIsError = false
        nameField.text = name
        renamingShortcode = ""
        confirmingDelete = false
        open()
    }

    /// The image being renamed, by current shortcode. One at a time, in place,
    /// so only one field competes for Return.
    property string renamingShortcode: ""
    /// Deleting a pack takes two clicks and the second says what is destroyed.
    /// Not undoable: Matrix has no delete verb for either store, so the pack is
    /// emptied and its name dropped.
    property bool confirmingDelete: false

    Connections {
        target: root.stickers
        function onEditFinished(ok, category, shortcode) {
            root.renamingShortcode = ""
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
            visible: root.canManage
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

        // The images
        ListView {
            id: imageList
            objectName: "stickerPackEditorList"
            Layout.fillWidth: true
            Layout.preferredHeight: 280
            clip: true
            model: root.stickers ? root.stickers.images : null
            ScrollBar.vertical: AppScrollBar {}
            SmoothWheelArea {}

            delegate: ItemDelegate {
                id: imageRow
                required property int index
                required property string shortcode
                required property string url
                required property bool isEmoticon

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
                        visible: root.renamingShortcode !== imageRow.shortcode
                        // A shortcode is remote text: never markup.
                        textFormat: Text.PlainText
                        text: ":" + imageRow.shortcode + ":"
                        color: AppTheme.textPrimary
                        elide: Label.ElideRight
                    }
                    AppTextField {
                        id: renameField
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: root.renamingShortcode === imageRow.shortcode
                        enabled: visible && !root.busy
                        onVisibleChanged: {
                            if (visible) {
                                text = imageRow.shortcode
                                forceActiveFocus()
                                selectAll()
                            }
                        }
                        onAccepted: root.stickers.renameImageInPack(
                            root.packId, imageRow.shortcode, text)
                    }

                    AppButton {
                        text: root.renamingShortcode === imageRow.shortcode
                              ? qsTr("Save") : qsTr("Rename")
                        kind: "ghost"
                        size: "sm"
                        visible: root.canManage
                        enabled: !root.busy
                        onClicked: {
                            if (root.renamingShortcode === imageRow.shortcode)
                                root.stickers.renameImageInPack(
                                    root.packId, imageRow.shortcode,
                                    renameField.text)
                            else
                                root.renamingShortcode = imageRow.shortcode
                        }
                    }
                    AppButton {
                        text: qsTr("Remove")
                        kind: "ghost"
                        size: "sm"
                        visible: root.canManage
                        enabled: !root.busy
                        onClicked: root.stickers.removeImageFromPack(
                            root.packId, imageRow.shortcode)
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            visible: imageList.count === 0
            text: qsTr("This pack has no images.")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
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
            Item { Layout.fillWidth: true }
            AppBusyIndicator {
                visible: root.busy
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
}
