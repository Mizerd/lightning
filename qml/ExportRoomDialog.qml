import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Export a room's loaded messages to a file.
//
// # The encrypted-room decision, in the open
//
// CLAUDE.md §6 keeps encrypted-room plaintext memory-only. This dialog is the
// one place in Lightning that offers to break that, and it is offered as a
// DECISION rather than a format option: the checkbox is off by default, its
// words say what the file will be rather than what the feature is called, and
// declining still produces a usable export — every body replaced by a
// withheld marker, so the shape of the conversation survives and none of its
// text does.
//
// An UNKNOWN encryption state counts as encrypted (AppController::
// exportOptions), so a room whose state has not loaded yet gets the careful
// treatment rather than the convenient one.
//
// # What the file is
//
// The messages Lightning has LOADED, and no attachments. Both limits are on
// screen before the user picks a file, and both are repeated inside the file
// itself — a partial export mistaken for a whole history is the failure this
// surface has to design against, and the person who reads the file later may
// not be the person who made it.
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
        // Re-armed on every open. A decision this specific must not be
        // inherited from a previous export of a different room.
        includeEncryptedText = false
        failure = ""
        messageCount = app.exportableMessageCount()
        var info = app.roomList.findRoom(app.currentRoomId)
        roomName = info && info.name ? info.name : app.currentRoomId
        // Unknown counts as encrypted, matching the C++ side: the careful
        // treatment for a state we have not learned.
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
            // The room NAME is remote, attacker-chosen text. Label defaults
            // to Text.AutoText, so a name containing a known tag would be
            // rendered as rich text and `<img src=...>` would beacon the
            // moment this dialog opened.
            textFormat: Text.PlainText
            // The count first, because it is the honest answer to "will this
            // be the whole conversation" and the answer is usually no.
            //
            // BRANCHED, not `%n message(s)`. A source string written that way
            // renders its "(s)" LITERALLY whenever no catalog is loaded, and
            // English only escapes that because its catalog carries numerus
            // entries a fresh `lupdate` leaves unfinished — so the first run
            // after any refresh would say "1 message(s)". §16 records this;
            // "Seen by N people" is branched for the same reason.
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

        // ── The encrypted-room exception ────────────────────────────────
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
                    // Says what the FILE will be, not what the option is
                    // called. "Include message text" would be a setting; this
                    // is a consequence.
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
