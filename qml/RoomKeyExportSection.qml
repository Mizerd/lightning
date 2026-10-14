import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Privacy & security → Export room keys, the counterpart of
// "Import room keys" beside it. matrix-sdk writes the file
// (Encryption::export_room_keys) in the standard encrypted format Element and
// other Matrix apps import; app.roomKeyExport is the policy around it.
//
// The passphrase lives only in the two fields. It reaches C++ as an argument
// of one call, and the fields are wiped right after it, when the section
// hides and when the account changes. echoMode stays Password with no reveal
// toggle, as in ChangePasswordSection.
ColumnLayout {
    id: root
    objectName: "roomKeyExportSection"
    spacing: AppTheme.spacing8

    readonly property var ctl: (typeof app !== "undefined" && app)
                               ? app.roomKeyExport : null
    readonly property bool busy: ctl ? ctl.busy : false
    readonly property int minimumLength: ctl ? ctl.minimumLength : 8
    readonly property int level: ctl ? ctl.strength(passphraseField.text) : 0
    // As Element: below this rung (its zxcvbn score 3) the export is
    // refused, not just warned about. The controller checks it again.
    readonly property int requiredStrength: ctl ? ctl.requiredStrength : 3
    readonly property bool tooWeak: passphraseField.text.length > 0
                                    && level < requiredStrength
    readonly property bool fieldsReady: level >= requiredStrength
                                        && confirmField.text === passphraseField.text
    readonly property bool signedIn: (typeof app !== "undefined" && app)
                                     ? app.loggedIn : false

    // Assign, never clear(): clear() keeps the undo history, and Ctrl+Z
    // would bring a typed passphrase back.
    function wipe() {
        passphraseField.text = ""
        confirmField.text = ""
    }

    // The fields first, then the native save dialog; the export starts when
    // a location is chosen.
    function chooseLocation() {
        if (!ctl || !exportButton.enabled)
            return
        ctl.clearResult()
        saveDialog.open()
    }

    function start(fileUrl) {
        if (!ctl)
            return
        ctl.exportKeys(fileUrl, passphraseField.text, confirmField.text)
        // Wiped whatever the outcome; a refusal says why in place.
        wipe()
    }

    onVisibleChanged: if (!visible) wipe()

    Connections {
        target: (typeof app !== "undefined" && app) ? app.accounts : null
        ignoreUnknownSignals: true
        function onActiveUserIdChanged() {
            root.wipe()
            if (root.ctl)
                root.ctl.clearResult()
        }
    }

    Label {
        objectName: "exportRoomKeysHeading"
        text: qsTr("Export room keys")
        font.weight: AppTheme.weightStrong
        color: AppTheme.stormText
    }
    Label {
        objectName: "roomKeyExportExplanation"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Save the keys that decrypt your encrypted messages to a "
                   + "file, locked with a passphrase you choose. Another "
                   + "session, or another Matrix app such as Element, can "
                   + "import it to read your message history. The file "
                   + "covers messages received up to now, and importing it "
                   + "does not verify a session.")
    }
    Label {
        objectName: "roomKeyExportWarning"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Anyone with this file and its passphrase can read your "
                   + "encrypted messages. Keep the file somewhere private, "
                   + "never send it in a chat, and store the passphrase "
                   + "separately from it.")
    }

    AppTextField {
        id: passphraseField
        objectName: "roomKeyExportPassphraseField"
        storm: true
        Layout.fillWidth: true
        echoMode: TextInput.Password
        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        placeholderText: qsTr("Passphrase for the file")
        Accessible.name: qsTr("Passphrase for the file")
        enabled: !root.busy && root.signedIn
        onTextEdited: if (root.ctl) root.ctl.clearResult()
        onAccepted: confirmField.forceActiveFocus()
    }
    // Strength hint under the passphrase.
    RowLayout {
        id: strengthRow
        objectName: "roomKeyExportStrengthRow"
        Layout.fillWidth: true
        visible: passphraseField.text.length > 0
        spacing: AppTheme.spacing8
        readonly property color ink: root.level <= 2 ? AppTheme.stormDanger
                                   : root.level === 3 ? AppTheme.warning
                                   : AppTheme.stormSuccess
        Row {
            spacing: AppTheme.spacing4
            Repeater {
                model: 4
                Rectangle {
                    required property int index
                    width: 24
                    height: 4
                    radius: 2
                    color: index < root.level ? strengthRow.ink
                                              : AppTheme.stormInset
                }
            }
        }
        // While too weak this is also why the export button is disabled.
        Label {
            objectName: "roomKeyExportStrengthLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.tooWeak ? AppTheme.stormDanger : AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: {
                if (root.level === 1)
                    return qsTr("Too short to export: use at least %1 "
                                + "characters.").arg(root.minimumLength)
                if (root.level === 2)
                    return qsTr("Too weak to export: use at least 12 "
                                + "characters, or 10 that mix upper and lower "
                                + "case, digits or symbols, and avoid "
                                + "repeating a few characters.")
                if (root.level === 3)
                    return qsTr("Fair")
                return qsTr("Strong")
            }
        }
    }
    AppTextField {
        id: confirmField
        objectName: "roomKeyExportConfirmField"
        storm: true
        Layout.fillWidth: true
        echoMode: TextInput.Password
        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        placeholderText: qsTr("Confirm the passphrase")
        Accessible.name: qsTr("Confirm the passphrase")
        enabled: !root.busy && root.signedIn
        onTextEdited: if (root.ctl) root.ctl.clearResult()
        onAccepted: root.chooseLocation()
    }
    Label {
        objectName: "roomKeyExportMismatchLabel"
        Layout.fillWidth: true
        visible: confirmField.text.length > 0
                 && confirmField.text !== passphraseField.text
        wrapMode: Text.WordWrap
        color: AppTheme.stormDanger
        font.pixelSize: AppTheme.textMeta
        text: qsTr("The passphrases do not match.")
        Accessible.name: text
    }
    AppButton {
        id: exportButton
        objectName: "roomKeyExportButton"
        storm: true
        Layout.alignment: Qt.AlignLeft
        text: root.busy ? qsTr("Exporting…") : qsTr("Export keys…")
        // The controller checks the same again before starting.
        enabled: root.ctl !== null && root.ctl.available && root.signedIn
                 && !root.busy && !saveDialog.busy && root.fieldsReady
        Accessible.description: root.tooWeak
            ? qsTr("Disabled: the passphrase is too weak.") : ""
        onClicked: root.chooseLocation()
    }

    // Progress: the SDK gathers every session, then encrypts the file in one
    // step, so the bar sweeps and the count is what it has gathered.
    AppProgressBar {
        objectName: "roomKeyExportProgress"
        Layout.fillWidth: true
        visible: root.busy
        indeterminate: true
    }
    Label {
        objectName: "roomKeyExportProgressLabel"
        Layout.fillWidth: true
        visible: root.busy
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: root.ctl && root.ctl.collected > 0
              ? qsTr("Exporting room keys… %n gathered so far.", "",
                     root.ctl.collected)
              : qsTr("Exporting room keys… This can take a while on an "
                     + "account with a long history.")
    }

    // The outcome, in place.
    Label {
        objectName: "roomKeyExportResult"
        Layout.fillWidth: true
        visible: text.length > 0
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: root.ctl && root.ctl.resultOk ? AppTheme.stormSuccess
                                             : AppTheme.stormDanger
        font.pixelSize: AppTheme.textBody
        textFormat: Text.PlainText
        text: root.ctl ? root.ctl.resultMessage : ""
        Accessible.name: text
    }

    NativeFileDialog {
        id: saveDialog
        objectName: "roomKeyExportSaveDialog"
        purpose: "keys"
        fileMode: "save"
        title: qsTr("Save room-key export")
        currentFolder: (typeof app !== "undefined" && app)
                       ? app.defaultFileDialogFolder() : ""
        currentName: root.ctl ? root.ctl.suggestedFileName
                              : "lightning-keys.txt"
        nameFilters: [ qsTr("Room-key export (*.txt)"), qsTr("All files (*)") ]
        onAccepted: root.start(selectedFile)
    }
}
