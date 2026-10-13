import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Appearance: emoji and sticker packs (MSC2545).
//
// Lists the packs this account can use right now: its own, the open room's,
// and any room pack turned on for use everywhere (`im.ponies.emote_rooms`).
// Per room pack: a "Use everywhere" switch, Manage (rename, remove, delete,
// export) and Export. Your own pack also takes Import. Nothing here is
// optimistic: each action waits for the backend and the list is re-read.
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
                   + "the room you have open and the ones you turned on are "
                   + "listed here.")
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
                    text: (packRow.source === "room" ? qsTr("Room pack")
                                                     : qsTr("Your pack"))
                          + " · "
                          + qsTr("%n sticker(s)", "", packRow.stickerCount)
                          + " · "
                          + qsTr("%n emoji", "", packRow.emoticonCount)
                }
            }

            // Only a room's pack has anything to turn on; your own is always
            // yours. Follows the snapshot re-read after the write.
            AppSwitch {
                objectName: "stickerPackUseEverywhereSwitch"
                visible: packRow.source === "room"
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
                visible: packRow.source === "room"
                text: qsTr("Use everywhere")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }

            AppButton {
                objectName: "stickerPackManageButton"
                text: qsTr("Manage")
                kind: "ghost"
                size: "sm"
                enabled: section.available && !section.stickers.editing
                         && !section.stickers.transferring
                onClicked: {
                    // The editor edits whichever pack the manager has selected.
                    section.stickers.selectedPackId = packRow.packId
                    editor.stickers = section.stickers
                    editor.openFor(packRow.packId, packRow.displayName,
                                   packRow.canManage)
                }
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
            section.notice = ok ? ""
                : qsTr("That could not be changed. Try again in a moment.")
        }
    }

    StickerPackEditor {
        id: editor
    }
}
