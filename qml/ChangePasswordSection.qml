import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Account → Change password, as Element offers it: current, new
// and confirm fields, an opt-in "sign out of all other devices", and the
// result in place. OAuth (MAS) accounts, and servers that turn the
// m.change_password capability off, get the account page instead.
//
// The passwords live only in the three fields. They reach C++ as the
// arguments of one call, and the fields are wiped right after it, when the
// section hides and when the account changes. echoMode stays Password with
// no reveal toggle, as in UiaPromptDialog.
ColumnLayout {
    id: root
    objectName: "changePasswordSection"
    spacing: AppTheme.spacing8

    readonly property var ctl: (typeof app !== "undefined" && app)
                               ? app.passwordChange : null
    readonly property string mode: ctl ? ctl.mode : "form"
    readonly property bool busy: ctl ? ctl.busy : false
    readonly property int minimumLength: ctl ? ctl.minimumLength : 8
    // Synapse refuses a new password longer than this with a bare 400.
    readonly property int maximumLength: 512

    // Assign, never clear(): clear() keeps the undo history, and Ctrl+Z
    // would bring a typed password back.
    function wipe() {
        currentField.text = ""
        newField.text = ""
        confirmField.text = ""
    }

    // A hint only, from length and character variety: Element's zxcvbn is
    // not available here, and the server's own policy decides
    // (M_WEAK_PASSWORD). 0 empty, 1 too short, 2 weak, 3 fair, 4 strong.
    function strength(pw) {
        if (pw.length === 0)
            return 0
        var chars = 0
        for (var i = 0; i < pw.length; ++i) {
            var c = pw.charCodeAt(i)
            if (c < 0xDC00 || c > 0xDFFF)
                ++chars
        }
        if (chars < minimumLength)
            return 1
        var kinds = (/[a-z]/.test(pw) ? 1 : 0) + (/[A-Z]/.test(pw) ? 1 : 0)
                    + (/[0-9]/.test(pw) ? 1 : 0)
                    + (/[^A-Za-z0-9]/.test(pw) ? 1 : 0)
        if (chars >= 16 || (chars >= 12 && kinds >= 3))
            return 4
        if (chars >= 12 || (chars >= 10 && kinds >= 3))
            return 3
        return 2
    }

    function submit() {
        if (!ctl || !sendButton.enabled)
            return
        var sent = ctl.changePassword(currentField.text, newField.text,
                                      confirmField.text,
                                      signOutOthers.checked)
        // Element clears the fields whatever the outcome; so do we.
        wipe()
        if (sent)
            signOutOthers.checked = false
    }

    // Asked on every show: the screen is built ahead of time and kept.
    onVisibleChanged: {
        if (visible) {
            if (ctl)
                ctl.refresh()
        } else {
            wipe()
        }
    }
    Component.onCompleted: if (visible && ctl) ctl.refresh()
    // The form hides when the probe hands the account to its account page.
    onModeChanged: if (mode !== "form") wipe()

    Connections {
        target: (typeof app !== "undefined" && app) ? app.accounts : null
        ignoreUnknownSignals: true
        function onActiveUserIdChanged() {
            root.wipe()
            signOutOthers.checked = false
            if (root.ctl) {
                root.ctl.clearResult()
                root.ctl.refresh()
            }
        }
    }

    Label {
        text: qsTr("Change password")
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }

    // ── Password accounts ──
    ColumnLayout {
        objectName: "changePasswordForm"
        Layout.fillWidth: true
        visible: root.mode === "form"
        spacing: AppTheme.spacing8

        AppTextField {
            id: currentField
            objectName: "currentPasswordField"
            storm: true
            Layout.fillWidth: true
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            placeholderText: qsTr("Current password")
            Accessible.name: qsTr("Current password")
            enabled: !root.busy
            onTextEdited: if (root.ctl) root.ctl.clearResult()
            onAccepted: newField.forceActiveFocus()
        }
        AppTextField {
            id: newField
            objectName: "newPasswordField"
            storm: true
            Layout.fillWidth: true
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            placeholderText: qsTr("New password")
            maximumLength: root.maximumLength
            Accessible.name: qsTr("New password")
            enabled: !root.busy
            onTextEdited: if (root.ctl) root.ctl.clearResult()
            onAccepted: confirmField.forceActiveFocus()
        }
        // Strength hint under the new password.
        RowLayout {
            id: strengthRow
            objectName: "passwordStrengthRow"
            Layout.fillWidth: true
            visible: newField.text.length > 0
            spacing: AppTheme.spacing8
            readonly property int level: root.strength(newField.text)
            readonly property color ink: level <= 2 ? AppTheme.stormDanger
                                       : level === 3 ? AppTheme.warning
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
                        color: index < strengthRow.level
                               ? strengthRow.ink : AppTheme.stormInset
                    }
                }
            }
            Label {
                objectName: "passwordStrengthLabel"
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: {
                    var level = strengthRow.level
                    if (level === 1)
                        return qsTr("Too short: use at least %1 characters.")
                               .arg(root.minimumLength)
                    if (level === 2)
                        return qsTr("Weak")
                    if (level === 3)
                        return qsTr("Fair")
                    return qsTr("Strong")
                }
            }
        }
        AppTextField {
            id: confirmField
            objectName: "confirmPasswordField"
            storm: true
            Layout.fillWidth: true
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            placeholderText: qsTr("Confirm new password")
            maximumLength: root.maximumLength
            Accessible.name: qsTr("Confirm new password")
            enabled: !root.busy
            onTextEdited: if (root.ctl) root.ctl.clearResult()
            onAccepted: root.submit()
        }
        Label {
            objectName: "passwordMismatchLabel"
            Layout.fillWidth: true
            visible: confirmField.text.length > 0
                     && confirmField.text !== newField.text
            wrapMode: Text.WordWrap
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Passwords don't match")
            Accessible.name: text
        }
        CheckBox {
            id: signOutOthers
            objectName: "signOutOtherDevicesCheck"
            palette.windowText: AppTheme.stormText
            enabled: !root.busy
            text: qsTr("Sign out of all other devices")
        }
        // Element's warning for the same choice.
        Label {
            Layout.fillWidth: true
            visible: signOutOthers.checked
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Signed-out devices lose the encryption keys stored "
                       + "on them. Set up your recovery key first if you "
                       + "want to keep your encrypted history.")
        }
        AppButton {
            id: sendButton
            objectName: "changePasswordButton"
            storm: true
            kind: "primary"
            Layout.alignment: Qt.AlignLeft
            text: root.busy ? qsTr("Changing…") : qsTr("Change password")
            // The controller checks the same again before sending.
            enabled: !root.busy && root.ctl !== null
                     && currentField.text.length > 0
                     && strengthRow.level >= 2
                     && confirmField.text === newField.text
            onClicked: root.submit()
        }
    }

    // ── OAuth (MAS) accounts, or a server that manages passwords itself ──
    Label {
        objectName: "changePasswordExternalText"
        Layout.fillWidth: true
        visible: root.mode !== "form"
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textBody
        text: {
            if (root.mode === "unavailable")
                return qsTr("Your server does not allow changing the "
                            + "password here.")
            if (root.ctl && root.ctl.managementUrl.length > 0)
                return qsTr("Your password is managed by your account "
                            + "provider.")
            return qsTr("Your password is managed by your account "
                        + "provider. Change it on its account page.")
        }
    }
    AppButton {
        objectName: "manageAccountButton"
        visible: root.mode === "external" && root.ctl !== null
                 && root.ctl.managementUrl.length > 0
        storm: true
        Layout.alignment: Qt.AlignLeft
        text: qsTr("Manage account")
        // Web links only (MediaManager::openWebUrl filters the scheme).
        onClicked: app.media.openWebUrl(root.ctl.managementUrl)
    }

    // The outcome, in place.
    Label {
        objectName: "changePasswordResult"
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
}
