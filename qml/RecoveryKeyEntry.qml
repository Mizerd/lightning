import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root

    property string fieldObjectName: "recoveryEntryField"
    property string buttonText: qsTr("Restore keys")
    property string fieldPlaceholder: qsTr("Recovery key or passphrase")
    property bool storm: true

    readonly property bool running: d.started && d.running
    readonly property string outcome: d.outcome

    signal submitted()

    spacing: AppTheme.spacing8

    QtObject {
        id: d
        property bool started: false
        property bool running: false
        property string outcome: ""
        property string failMessage: ""
        property string userId: ""
        function activeUserId() {
            return app.accounts ? app.accounts.activeUserId : ""
        }
    }

    function clear() {
        field.text = ""
        d.started = false
        d.running = false
        d.outcome = ""
        d.failMessage = ""
    }

    onVisibleChanged: {
        if (!visible && !d.running) {
            d.outcome = ""
            d.failMessage = ""
            field.text = ""
        }
    }

    GridLayout {
        Layout.fillWidth: true
        columnSpacing: AppTheme.spacing8
        rowSpacing: AppTheme.spacing8
        columns: width < 360 ? 1 : 2

        AppTextField {
            id: field
            storm: root.storm
            objectName: root.fieldObjectName
            Layout.fillWidth: true
            Layout.minimumWidth: 160
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                              | Qt.ImhNoAutoUppercase
            placeholderText: root.fieldPlaceholder
            enabled: !root.running
            Accessible.name: root.fieldPlaceholder
            onAccepted: submitButton.submit()
            onTextChanged: {
                if (text.length > 0 && !d.running && d.outcome !== "") {
                    d.outcome = ""
                    d.failMessage = ""
                }
            }
        }
        AppButton {
            id: submitButton
            objectName: root.fieldObjectName + "Submit"
            storm: root.storm
            text: root.running ? qsTr("Restoring…") : root.buttonText
            enabled: !root.running && field.text.length > 0
            function submit() {
                if (!enabled)
                    return
                d.started = true
                d.running = true
                d.outcome = ""
                d.failMessage = ""
                d.userId = d.activeUserId()
                app.requestRecoverFromBackup(field.text)
                // Wipe the field immediately; the key never stays in a QML
                // property.
                field.text = ""
                root.submitted()
            }
            onClicked: submit()
        }
    }

    Label {
        objectName: root.fieldObjectName + "Status"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        visible: text !== ""
        textFormat: Text.PlainText
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        color: d.outcome === "ok"
               ? AppTheme.stormSuccess
               : d.outcome === "failed" ? AppTheme.stormDanger
                 : d.outcome.length > 0 ? AppTheme.stormText
                                        : AppTheme.stormTextMuted
        text: d.running
              ? qsTr("Recovery started")
              : d.outcome === "ok"
                ? (app.sessionTrustState === "Verified"
                   ? qsTr("Recovery complete. This session is now verified. New "
                          + "messages should decrypt as keys arrive; some old "
                          + "messages may still require another verified "
                          + "device to share keys.")
                   : qsTr("Recovery complete. New messages should decrypt as "
                          + "keys arrive. Some old messages may still require "
                          + "another verified device to share keys. If this "
                          + "session is still not verified, your recovery "
                          + "storage may not hold your cross-signing keys; "
                          + "see Cross-signing under Privacy & security."))
                : d.outcome === "ok_no_backup"
                  ? qsTr("Your recovery key was accepted and this session is "
                         + "verified, but no key backup could be used (the "
                         + "account has none, or your secret storage does not "
                         + "hold its key), so no message history was restored.")
                  : d.outcome === "nothing_restored"
                    ? qsTr("Your recovery key was accepted, but your secret "
                           + "storage holds neither a key backup nor your "
                           + "cross-signing keys, so nothing was restored and "
                           + "this session is not verified by it. Verify it "
                           + "with another session instead.")
                    : d.outcome === "interrupted"
                      ? qsTr("The session ended before the restore finished, so "
                             + "it is not known what was restored. Enter the key "
                             + "again to finish.")
                      : d.outcome === "failed"
                        ? qsTr("Recovery failed: %1").arg(d.failMessage)
                        : ""
    }

    Connections {
        target: app
        function onRecoveryStateChanged(state, message) {
            if (!d.started)
                return
            if (state === "attempted") {
                d.running = true
            } else if (state === "ok" || state === "ok_no_backup"
                       || state === "nothing_restored") {
                d.running = false
                d.started = false
                d.outcome = state
                app.refreshCryptoHealth()
                app.refreshSessionTrustState()
            } else if (state === "failed") {
                d.running = false
                d.started = false
                d.outcome = "failed"
                d.failMessage = message
                app.refreshCryptoHealth()
                app.refreshSessionTrustState()
            }
        }
        function onLoggedInChanged() {
            if (!d.started) {
                d.outcome = ""
                d.failMessage = ""
                return
            }
            const sameAccount = d.userId === d.activeUserId()
            root.clear()
            if (sameAccount)
                d.outcome = "interrupted"
        }
    }
}
