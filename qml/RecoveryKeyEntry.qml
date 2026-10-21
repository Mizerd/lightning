import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Recovery key or passphrase entry, shared by every place that offers it: the
// Recovery card in Privacy & security, the Sessions page's "Use recovery key
// instead" and the first-run verification prompt.
//
// It only hands the text to app.requestRecoverFromBackup(), which is matrix-sdk's
// recovery().recover(): the SDK, not this file, decides what the key proves.
// When the account's cross-signing keys are in its secret storage the same call
// signs this session's own device, which is what makes the session verified;
// this file never marks anything trusted itself (CLAUDE.md §6, §9). The text is
// wiped from the field the moment it is submitted and is never logged, kept in
// another property or echoed back in the status line.
ColumnLayout {
    id: root

    // objectName for the text field; the Recovery card keeps the name the
    // settings search anchors on.
    property string fieldObjectName: "recoveryEntryField"
    property string buttonText: qsTr("Restore keys")
    property string fieldPlaceholder: qsTr("Recovery key or passphrase")
    property bool storm: true

    // True while a request this instance started is in flight.
    readonly property bool running: d.started && d.running
    // Last outcome this instance saw: "", "ok", "ok_no_backup",
    // "nothing_restored", "failed" or "interrupted".
    readonly property string outcome: d.outcome

    signal submitted()

    spacing: AppTheme.spacing8

    QtObject {
        id: d
        property bool started: false
        property bool running: false
        property string outcome: ""
        property string failMessage: ""
        // The account the request was made for; a different one signed in
        // afterwards must not see this instance's outcome.
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

    // A result belongs to the moment it was reported: leaving the page (or
    // closing the settings page) must not leave "Recovery failed" or a success claim
    // waiting for the next visit. A request still in flight is kept.
    onVisibleChanged: {
        if (!visible && !d.running) {
            d.outcome = ""
            d.failMessage = ""
            // A half-typed recovery key does not wait in a hidden page for
            // the next visit (MASTER-2 L1; as CrossSigningSetupCard wipes its
            // secrets on hide).
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
            // recover() accepts a recovery key or a passphrase.
            placeholderText: root.fieldPlaceholder
            enabled: !root.running
            Accessible.name: root.fieldPlaceholder
            onAccepted: submitButton.submit()
            // The first edit after an answer retires it: the red "Recovery
            // failed" line must not stay beside a key that is being corrected.
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
                // recover() succeeded but restored less than its name says;
                // never "Recovery complete" for these (CLAUDE.md section 6).
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
            // Only the instance whose Submit started the request reacts; the
            // signal reaches every instance in the window.
            if (!d.started)
                return
            if (state === "attempted") {
                d.running = true
            } else if (state === "ok" || state === "ok_no_backup"
                       || state === "nothing_restored") {
                d.running = false
                d.started = false
                d.outcome = state
                // Recovered secrets change trust/backup state; re-read it.
                app.refreshCryptoHealth()
                app.refreshSessionTrustState()
            } else if (state === "failed") {
                d.running = false
                d.started = false
                d.outcome = "failed"
                d.failMessage = message
                // A failure can still have changed trust: recover() verifies
                // this session before it touches the key backup.
                app.refreshCryptoHealth()
                app.refreshSessionTrustState()
            }
        }
        // A restore whose session ends (sign-out, account switch) never
        // reports: the backend's task dies with the session. Without this the
        // field and button stayed disabled at "Restoring…" for good.
        function onLoggedInChanged() {
            if (!d.started) {
                // A finished attempt's answer is about the session that made
                // it; a sign-out or a new sign-in must not carry it over.
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
