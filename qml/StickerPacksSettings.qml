import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: section
    objectName: "stickerPacksSettings"
    spacing: AppTheme.spacing8

    readonly property var stickers: typeof app !== "undefined" && app
                                    ? app.stickers : null
    readonly property bool available: !!stickers && stickers.available

    property string notice: ""
    property bool noticeIsError: false
    property bool creating: false

    function unavailableText(reason) {
        if (reason === "not_joined")
            return qsTr("you are not in its room")
        if (reason === "absent")
            return qsTr("this pack no longer exists")
        if (reason === "forbidden")
            return qsTr("you cannot see this pack")
        return qsTr("could not be loaded")
    }

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
            required property bool hidden

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
                        if (packRow.hidden)
                            text += " · " + qsTr("hidden here")
                        return text
                    }
                }
            }

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
                visible: packRow.source === "room" && section.width >= 460
                text: qsTr("Use everywhere")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }

            AppButton {
                objectName: "stickerPackHideButton"
                text: packRow.hidden ? qsTr("Show") : qsTr("Hide")
                kind: "ghost"
                size: "sm"
                Accessible.description: qsTr("Hides this pack from your pickers "
                                             + "on this device only. Nothing "
                                             + "changes for anyone else.")
                onClicked: section.stickers.setPackHidden(packRow.packId,
                                                          !packRow.hidden)
            }
            AppButton {
                id: manageButton
                objectName: "stickerPackManageButton"
                text: qsTr("Manage")
                kind: "ghost"
                size: "sm"
                enabled: section.available && packRow.available
                         && !section.stickers.editing
                         && !section.stickers.transferring
                onClicked: {
                    section.stickers.selectedPackId = packRow.packId
                    editor.stickers = section.stickers
                    editor.restoreTo = manageButton
                    editor.openFor(packRow.packId, packRow.displayName,
                                   section.stickers.canManagePack(packRow.packId))
                }
            }
        }
    }

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
