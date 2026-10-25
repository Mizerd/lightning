import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Appearance: emoji and sticker packs (Matrix v1.19 image packs,
// with the older im.ponies.* format still read and edited in place).
//
// Lists the packs this account can use right now: its own, the ones turned on
// for use everywhere, the open room's, and those of the room's Spaces. Per
// room pack: a "Use everywhere" switch and Manage. A room the account may
// edit can get a new pack here. Nothing here is optimistic: each action waits
// for the backend and the list is re-read.
//
// Packs of rooms and Spaces you are not in the middle of are not discovered:
// that would be one state read per joined room. Open the room or Space, or
// enable its pack from the sticker picker, and it appears here.
ColumnLayout {
    id: section
    objectName: "stickerPacksSettings"
    spacing: AppTheme.spacing8

    readonly property var stickers: typeof app !== "undefined" && app
                                    ? app.stickers : null
    readonly property bool available: !!stickers && stickers.available

    property string notice: ""
    property bool noticeIsError: false
    // A Create was pressed; its editFinished is ours to report.
    property bool creating: false

    // Why a subscribed pack could not be read, in the user's words.
    function unavailableText(reason) {
        if (reason === "not_joined")
            return qsTr("you are not in its room")
        if (reason === "absent")
            return qsTr("this pack no longer exists")
        if (reason === "forbidden")
            return qsTr("you cannot see this pack")
        return qsTr("could not be loaded")
    }

    // A fresh read whenever this page is shown; a no-op when the snapshot is
    // current.
    onVisibleChanged: if (visible && available) stickers.refreshIfStale()
    Component.onCompleted: if (visible && available) stickers.refreshIfStale()

    Label {
        text: qsTr("Emoji and sticker packs")
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }
    Label {
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("A room's packs work inside that room. Turn on Use "
                   + "everywhere to use a pack in every room. Packs from "
                   + "the room you have open, its Spaces and the ones you "
                   + "turned on are listed here.")
    }
    Label {
        objectName: "stickerPacksPrivacyNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Pack images are stored as regular Matrix media and are "
                   + "visible to the hosting server, including when you use "
                   + "them in encrypted chats.")
    }

    Label {
        objectName: "stickerPacksUnavailable"
        Layout.fillWidth: true
        visible: !section.available
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Packs are not available with this connection.")
    }

    Repeater {
        id: packList
        model: section.available ? section.stickers.packs : null

        delegate: RowLayout {
            id: packRow
            required property string packId
            required property string displayName
            required property string source
            required property bool enabledGlobally
            required property bool canManage
            required property int stickerCount
            required property int emoticonCount
            required property string packNamespace
            required property bool available
            required property string unavailableReason
            required property bool legacySubscribed
            required property bool fromSpace

            objectName: "stickerPackRow"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    // A pack name is remote text: never markup.
                    textFormat: Text.PlainText
                    text: packRow.displayName
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    elide: Label.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    elide: Label.ElideRight
                    text: {
                        var where = packRow.source !== "room" ? qsTr("Your pack")
                                  : packRow.fromSpace ? qsTr("Space pack")
                                  : qsTr("Room pack")
                        // The older im.ponies format, said once and plainly;
                        // the protocol names live in the docs.
                        if (packRow.packNamespace === "legacy")
                            where += " · " + qsTr("older format")
                        if (!packRow.available)
                            return where + " · " + section.unavailableText(
                                packRow.unavailableReason)
                        var text = where
                            + " · " + qsTr("%n sticker(s)", "", packRow.stickerCount)
                            + " · " + qsTr("%n emoji", "", packRow.emoticonCount)
                        if (packRow.legacySubscribed)
                            text += " · " + qsTr("turned on in an older app")
                        return text
                    }
                }
            }

            // Only a room's pack has anything to turn on; your own is always
            // yours. Follows the snapshot re-read after the write.
            AppSwitch {
                objectName: "stickerPackUseEverywhereSwitch"
                visible: packRow.source === "room"
                ToolTip.text: qsTr("Use everywhere")
                ToolTip.visible: _hot && !useEverywhereLabel.visible
                ToolTip.delay: 500
                enabled: section.available
                         && !section.stickers.togglingRoomPack
                         && !section.stickers.loading
                checked: packRow.enabledGlobally
                Accessible.name: qsTr("Use %1 in every room")
                                     .arg(packRow.displayName)
                onToggled: section.stickers.setRoomPackEnabled(
                    packRow.packId, !packRow.enabledGlobally)
            }
            Label {
                id: useEverywhereLabel
                // Dropped on a narrow page so the pack's name keeps its room
                // (C41-F14); the switch keeps its accessible name and tooltip.
                visible: packRow.source === "room" && section.width >= 460
                text: qsTr("Use everywhere")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }

            AppButton {
                id: manageButton
                objectName: "stickerPackManageButton"
                text: qsTr("Manage")
                kind: "ghost"
                size: "sm"
                // An unreadable pack has nothing to manage; its row keeps the
                // Use everywhere switch so the stale reference can be removed.
                enabled: section.available && packRow.available
                         && !section.stickers.editing
                         && !section.stickers.transferring
                onClicked: {
                    // The editor edits whichever pack the manager has selected.
                    section.stickers.selectedPackId = packRow.packId
                    editor.stickers = section.stickers
                    editor.restoreTo = manageButton
                    // The manager's rule, not the model role: the role is the
                    // ROOM permission and is false for the account's own pack,
                    // which is always writable (C41-D01, C41-F06).
                    editor.openFor(packRow.packId, packRow.displayName,
                                   section.stickers.canManagePack(packRow.packId))
                }
            }
        }
    }

    // A new, empty pack in the open room (stable format, a fresh state key),
    // for a room this account may edit. Images are added from the picker or
    // the editor.
    RowLayout {
        id: createRow
        objectName: "stickerPackCreateRow"
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        readonly property string roomId: typeof app !== "undefined" && app
                                         ? app.currentRoomId : ""
        visible: section.available && createRow.roomId !== ""
                 && section.stickers.revision >= 0
                 && section.stickers.canCreateRoomPack(createRow.roomId)
        AppTextField {
            id: newPackName
            objectName: "stickerPackNewName"
            Layout.fillWidth: true
            placeholderText: qsTr("New pack name")
            Accessible.name: qsTr("New pack name")
        }
        AppComboBox {
            id: newPackUsage
            objectName: "stickerPackNewUsage"
            storm: true
            implicitWidth: 150
            textRole: "label"
            valueRole: "value"
            model: [ { label: qsTr("Emoji and stickers"), value: "both" },
                     { label: qsTr("Emoji only"), value: "emoticon" },
                     { label: qsTr("Stickers only"), value: "sticker" } ]
            Accessible.name: qsTr("What the pack is for")
        }
        AppButton {
            objectName: "stickerPackCreateButton"
            text: qsTr("Create pack")
            kind: "primary"
            size: "sm"
            enabled: section.available && !section.stickers.editing
                     && newPackName.text.trim().length > 0
            onClicked: {
                section.creating = true
                section.stickers.createRoomPack(createRow.roomId,
                                                newPackName.text,
                                                newPackUsage.currentValue
                                                    || "both")
            }
        }
    }

    Label {
        objectName: "stickerPacksEmpty"
        Layout.fillWidth: true
        visible: section.available && !!section.stickers
                 && section.stickers.loaded && packList.count === 0
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("No packs yet. Import one, or add a sticker from the "
                   + "sticker picker to start your own.")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        visible: section.available

        NativeFileDialog {
            id: importDialog
            purpose: "stickerpack"
            fileMode: "open"
            title: qsTr("Import a sticker pack")
            currentFolder: app.defaultFileDialogFolder()
            nameFilters: [qsTr("Sticker pack (*.zip)")]
            onAccepted: section.stickers.importPack(selectedFile)
        }
        AppButton {
            objectName: "stickerPackImportButton"
            text: qsTr("Import a pack…")
            kind: "ghost"
            size: "sm"
            enabled: section.available && !section.stickers.transferring
                     && !section.stickers.saving && !section.stickers.editing
            onClicked: importDialog.open()
        }
        AppBusyIndicator {
            visible: section.available
                     && (section.stickers.transferring
                         || section.stickers.loading)
            implicitWidth: 18
            implicitHeight: 18
        }
        Label {
            visible: section.available && section.stickers.transferring
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: section.available
                  ? qsTr("%1 of %2").arg(section.stickers.transferDone)
                        .arg(section.stickers.transferTotal)
                  : ""
        }
        Item { Layout.fillWidth: true }
    }

    Label {
        objectName: "stickerPacksNotice"
        Layout.fillWidth: true
        visible: section.notice.length > 0
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: section.notice
        color: section.noticeIsError ? AppTheme.danger : AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    Connections {
        target: section.available ? section.stickers : null
        function onTransferFinished(kind, ok, category, added, skipped, failed,
                                    name) {
            if (kind === "emoji" || editor.visible)
                return
            section.noticeIsError = !ok
            section.notice = section.stickers.transferMessage(
                kind, ok, category, added, skipped, failed, name)
        }
        function onRoomPackToggleFinished(ok, category, enabled) {
            section.noticeIsError = !ok
            if (!ok)
                section.notice =
                    qsTr("That could not be changed. Try again in a moment.")
            else if (section.stickers.lastMirrorFailed())
                // Two writes, not one transaction: say which half landed.
                section.notice = qsTr("Saved. Older apps on your other "
                                      + "devices may not see this change yet.")
            else
                section.notice = ""
        }
        function onEditFinished(ok, category, shortcode) {
            if (!section.creating)
                return
            section.creating = false
            section.noticeIsError = !ok
            if (ok) {
                newPackName.text = ""
                section.notice = qsTr("Pack created. Add images from the "
                                      + "sticker picker or Manage.")
            } else if (category === "forbidden") {
                section.notice = qsTr("You do not have permission to add "
                                      + "packs to this room.")
            } else if (category === "pack_exists") {
                section.notice = qsTr("A pack with that key already exists. "
                                      + "Try again.")
            } else {
                section.notice = qsTr("The pack could not be created.")
            }
        }
    }

    StickerPackEditor {
        id: editor
    }
}
