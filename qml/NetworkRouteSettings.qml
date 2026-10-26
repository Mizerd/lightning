import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Account -> Network: this account's proxy (docs/network-profiles.md).
//
// One route per account, kept on this device only. The password goes into the
// keyring through save() and is never read back here: the field stays empty
// and its placeholder says whether one is stored. A route applies the next
// time the account connects. Nothing falls back to a direct connection.
ColumnLayout {
    id: section
    objectName: "networkRouteSettings"
    spacing: AppTheme.spacing8

    readonly property var ctl: typeof app !== "undefined" && app ? app.networkRoute : null
    readonly property var route: section.ctl ? section.ctl.route : ({})
    readonly property bool hasPassword: !!section.route.hasPassword
    // What the form shows: the combo's choice, not the stored one, so the
    // fields appear as soon as a proxy kind is picked.
    readonly property bool proxyChosen: kindCombo.currentValue !== "none"
    property string result: ""
    property bool saved: false

    function load() {
        kindCombo.syncToValue(section.route.kind || "none")
        hostField.text = section.route.host || ""
        portField.text = section.route.port > 0 ? String(section.route.port) : ""
        userField.text = section.route.username || ""
        passwordField.text = ""
    }
    function save() {
        // An empty password field keeps the stored one (same username).
        var replace = passwordField.text.length > 0
        section.result = section.ctl.save(kindCombo.currentValue, hostField.text.trim(),
                                          parseInt(portField.text, 10) || 0,
                                          userField.text.trim(), passwordField.text, replace)
        passwordField.text = ""
        section.saved = section.result === ""
    }
    function errorText(code) {
        switch (code) {
        case "": return ""
        case "invalid_host": return qsTr("Enter the proxy's host name or IP address.")
        case "invalid_port": return qsTr("Enter a port from 1 to 65535.")
        case "invalid_username": return qsTr("The user name can't contain line breaks.")
        case "password_required": return qsTr("Enter the proxy password for this user name.")
        case "keyring_unavailable":
            return qsTr("The password couldn't be saved to the system keyring, so nothing was changed.")
        case "no_account": return qsTr("Sign in first; a proxy belongs to a saved account.")
        default: return qsTr("The proxy couldn't be saved.")
        }
    }

    Component.onCompleted: load()
    Connections {
        target: section.ctl
        function onChanged() { section.load() }
    }

    Label {
        text: qsTr("Network")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
    }
    Label {
        text: qsTr("Connection for this account")
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.settingsOptionSize
        font.weight: AppTheme.weightStrong
    }
    AppComboBox {
        id: kindCombo
        objectName: "networkRouteKindCombo"
        storm: true
        Layout.fillWidth: true
        enabled: !!section.ctl && section.ctl.available
        textRole: "label"
        valueRole: "value"
        model: [
            { label: qsTr("Direct (system network settings)"), value: "none" },
            { label: qsTr("HTTP proxy"),                       value: "http" },
            { label: qsTr("SOCKS5 proxy"),                     value: "socks5" }
        ]
        onActivated: { section.result = ""; section.saved = false }
        Accessible.name: qsTr("Connection for this account")
    }
    RowLayout {
        Layout.fillWidth: true
        visible: section.proxyChosen
        spacing: AppTheme.spacing8
        AppTextField {
            id: hostField
            objectName: "networkRouteHostField"
            storm: true
            Layout.fillWidth: true
            placeholderText: qsTr("Proxy host")
            Accessible.name: qsTr("Proxy host")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
            onTextEdited: { section.result = ""; section.saved = false }
        }
        AppTextField {
            id: portField
            objectName: "networkRoutePortField"
            storm: true
            Layout.preferredWidth: 96
            placeholderText: qsTr("Port")
            Accessible.name: qsTr("Proxy port")
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: 65535 }
            onTextEdited: { section.result = ""; section.saved = false }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: section.proxyChosen
        spacing: AppTheme.spacing8
        AppTextField {
            id: userField
            objectName: "networkRouteUserField"
            storm: true
            Layout.fillWidth: true
            placeholderText: qsTr("User name (optional)")
            Accessible.name: qsTr("Proxy user name")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            onTextEdited: { section.result = ""; section.saved = false }
        }
        AppTextField {
            id: passwordField
            objectName: "networkRoutePasswordField"
            storm: true
            Layout.fillWidth: true
            enabled: userField.text.trim().length > 0
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            // Never the stored value: only whether there is one.
            placeholderText: section.hasPassword
                             && userField.text.trim() === (section.route.username || "")
                             ? qsTr("Password saved (type to replace)")
                             : qsTr("Password")
            Accessible.name: qsTr("Proxy password")
            onTextEdited: { section.result = ""; section.saved = false }
            onAccepted: if (saveButton.enabled) section.save()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        AppButton {
            id: saveButton
            objectName: "networkRouteSaveButton"
            storm: true
            kind: "primary"
            text: qsTr("Save")
            enabled: !!section.ctl && section.ctl.available
                     && (!section.proxyChosen
                         || (hostField.text.trim().length > 0 && portField.acceptableInput))
            onClicked: section.save()
        }
        Label {
            objectName: "networkRouteResult"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: section.result === "" ? AppTheme.stormTextSecondary : AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            text: section.result !== "" ? section.errorText(section.result)
                  : section.saved ? qsTr("Saved. It applies the next time this account connects.")
                  : ""
        }
    }
    Label {
        objectName: "networkRouteHint"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        text: qsTr("With a proxy, everything this account sends to its homeserver "
                   + "goes through it: sync, encryption keys, media, and signing "
                   + "this account in again. A brand-new account's first sign-in "
                   + "connects directly, and a sign-in page opened in your browser "
                   + "uses the browser's own network settings. "
                   + "If the proxy can't be reached, or its password can't be read "
                   + "from the keyring, the account stays offline instead of "
                   + "connecting directly. Link previews then come from your "
                   + "homeserver only, links' videos and large images aren't "
                   + "loaded, calls aren't available, and the account doesn't run "
                   + "in the background. GIF search and update checks use the "
                   + "system's network settings. Kept on this device only.")
    }
}
