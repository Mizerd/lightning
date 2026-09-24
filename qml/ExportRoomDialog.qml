import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Export a room's loaded messages to a file.
//
// Encrypted rooms: CLAUDE.md §6 keeps encrypted plaintext memory-only, and
// this is the one place that offers to break that. It is a decision, not a
// format option: the checkbox is off by default, its words say what the file
// will contain, and declining still exports the conversation's shape with
// every body replaced by a withheld marker. An unknown encryption state counts
// as encrypted (AppController::exportOptions).
//
// The file holds only the messages Lightning has loaded and no attachments.
// Both limits are shown before a file is picked and repeated in the file,
// since a partial export can be mistaken for the whole history.
Dialog {
    id: root
    objectName: "exportRoomDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string format: "text"
    property bool includeEncryptedText: false
    property int messageCount: 0
    property bool encrypted: false
    property string roomName: ""
    property string failure: ""

    function openDialog() {
        format = "text"
        // Re-armed on every open; never inherited from a previous export.
        includeEncryptedText = false
        failure = ""
        messageCount = app.exportableMessageCount()
        var info = app.roomList.findRoom(app.currentRoomId)
        roomName = info && info.name ? info.name : app.currentRoomId
        // Unknown counts as encrypted, matching the C++ side.
        encrypted = !!info
                    && (info.encryptionKnown === false || info.encrypted === true)
        open()
    }

    FileDialog {
        id: saveDialog
        objectName: "exportRoomSaveDialog"
        fileMode: FileDialog.SaveFile
        title: qsTr("Export room")
        nameFilters: root.format === "json"
                     ? [ qsTr("JSON (*.json)"), qsTr("All files (*)") ]
                     : [ qsTr("Text (*.txt)"), qsTr("All files (*)") ]
        onAccepted: {
            var problem = app.exportCurrentRoom(selectedFile, root.format,
                                                root.includeEncryptedText)
            if (problem.length > 0) {
                root.failure = problem
                return
            }
            root.failure = ""
            root.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Export room")
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }
        Label {
            objectName: "exportRoomScope"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // The room name is attacker-chosen: with AutoText, markup such as
            // `<img src=...>` would render and beacon.
            textFormat: Text.PlainText
            // The count first: it answers "is this the whole conversation?",
            // usually no. Branched rather than `%n message(s)`, which renders
            // "(s)" literally without a loaded catalog.
            text: root.messageCount === 1
                  ? qsTr("Exports the 1 message Lightning has loaded for %1. "
                         + "Scroll further back first to include more. "
                         + "Attachments are not included.").arg(root.roomName)
                  : qsTr("Exports the %1 messages Lightning has loaded for "
                         + "%2. Scroll further back first to include more. "
                         + "Attachments are not included.")
                        .arg(root.messageCount).arg(root.roomName)
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        SegmentedControl {
            objectName: "exportRoomFormat"
            storm: true
            Layout.fillWidth: false
            model: [
                { value: "text", label: qsTr("Text") },
                { value: "json", label: qsTr("JSON") }
            ]
            current: root.format
            onActivated: (value) => root.format = value
        }

        // ── The encrypted-room exception ──
        Rectangle {
            objectName: "exportRoomEncryptedNotice"
            visible: root.encrypted
            Layout.fillWidth: true
            implicitHeight: encryptedCol.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusMd
            color: AppTheme.stormInset
            border.width: 1
            border.color: AppTheme.stormBorderStrong
            ColumnLayout {
                id: encryptedCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("This room is encrypted. Lightning keeps its "
                               + "messages in memory and never writes them to "
                               + "disk.")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                }
                CheckBox {
                    objectName: "exportRoomIncludeEncrypted"
                    palette.windowText: AppTheme.stormText
                    // Says what the file will contain, not what the option is
                    // called.
                    text: qsTr("Write the message text into this file in the "
                               + "clear")
                    checked: root.includeEncryptedText
                    onToggled: root.includeEncryptedText = checked
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.includeEncryptedText
                          ? qsTr("Anyone who can read the file can read the "
                                 + "conversation. It is not encrypted, and "
                                 + "backups, sync folders and shared drives "
                                 + "copy it like any other file.")
                          : qsTr("Without this, the export lists who sent "
                                 + "what and when, with each message's text "
                                 + "withheld.")
                    color: root.includeEncryptedText ? AppTheme.stormDanger
                                                     : AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }

        Label {
            objectName: "exportRoomFailure"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.failure.length > 0
            text: root.failure
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "exportRoomConfirm"
                text: qsTr("Choose file…")
                kind: "primary"
                enabled: root.messageCount > 0
                onClicked: {
                    root.failure = ""
                    saveDialog.currentFile = ""
                    saveDialog.selectedFile =
                        app.suggestedExportFileName(root.format)
                    saveDialog.open()
                }
            }
        }
    }
}
