import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    objectName: "crossSigningSetupCard"

    readonly property string setupState:
        app.cryptoHealth ? app.cryptoHealth.crossSigningSetup : "unknown"
    readonly property bool ownAction:
        app.backup && (app.backup.lastAction === "setup_cross_signing"
                       || app.backup.lastAction === "reset_cross_signing")
    readonly property bool working: app.backup && app.backup.busy && ownAction
    readonly property bool needsRecoveryChoice:
        ownAction && !app.backup.busy
        && (app.backup.errorCategory === "recovery_key_required"
            || app.backup.errorCategory === "recovery_key_wrong")

    property string pendingConfirm: ""

    // Closing Settings hides this card with the page; an armed destructive
    // confirmation or a half-typed account password must not wait for the next
    // visit (the secrets are wiped, not just hidden).
    onVisibleChanged: {
        if (!visible) {
            pendingConfirm = ""
            passwordField.text = ""
            currentKeyField.text = ""
        }
    }

    visible: app.backendName === "rust"
             && app.cryptoHealth && app.cryptoHealth.cryptoSupported
             && app.backup
             && (setupState === "not_set_up" || setupState === "unconfirmed"
                 || setupState === "keys_missing"
                 || working || needsRecoveryChoice
                 || (ownAction && (app.backup.recoveryKey.length > 0
                                   || app.backup.notice.length > 0)))
    radius: AppTheme.radiusMd
    color: AppTheme.stormPanel
    border.color: AppTheme.stormBorder
    border.width: 1
    implicitHeight: col.implicitHeight + AppTheme.spacing16 * 2

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing8

        MenuSectionLabel { text: qsTr("Cross-signing") }

        Label {
            objectName: "crossSigningExplanation"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            visible: root.setupState === "not_set_up" || root.setupState === "unconfirmed"
                     || root.setupState === "keys_missing"
            text: root.setupState === "not_set_up"
                  ? qsTr("This account has no cross-signing identity, so none of its "
                         + "sessions can be verified, including this one. Setting it up "
                         + "creates the identity and stores its keys in your secret "
                         + "storage, so your recovery key can verify new sessions.")
                  : root.setupState === "unconfirmed"
                    ? qsTr("This session holds cross-signing keys that your server has "
                           + "not confirmed, so setting up did not finish. Finishing it "
                           + "uploads these keys. If the account has meanwhile got an "
                           + "identity from another session, Lightning tells you and "
                           + "changes nothing.")
                    : qsTr("This account already has a cross-signing identity, but this "
                           + "session does not hold its keys. Verify this session with "
                           + "another session, or enter your recovery key. Lightning will "
                           + "not replace the identity on its own.")
        }

        ColumnLayout {
            objectName: "crossSigningPasswordPrompt"
            Layout.fillWidth: true
            visible: app.backup && app.backup.passwordRequired
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: app.backup && app.backup.wrongPassword ? AppTheme.stormDanger
                                                              : AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: app.backup && app.backup.wrongPassword
                      ? qsTr("That password was not accepted. Try again.")
                      : qsTr("Confirm your account password so the server accepts the new keys.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: passwordField
                    objectName: "crossSigningPasswordField"
                    storm: true
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                                      | Qt.ImhNoAutoUppercase
                    placeholderText: qsTr("Account password")
                    Accessible.name: qsTr("Account password")
                    onAccepted: submitPassword.clicked()
                }
                AppButton {
                    id: submitPassword
                    objectName: "crossSigningPasswordSubmit"
                    storm: true
                    kind: "primary"
                    text: qsTr("Continue")
                    enabled: passwordField.text.length > 0
                    onClicked: {
                        if (!enabled)
                            return
                        app.backup.submitPassword(passwordField.text)
                        passwordField.text = ""
                    }
                }
                AppButton {
                    objectName: "crossSigningPasswordCancel"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Cancel")
                    onClicked: {
                        passwordField.text = ""
                        app.backup.cancelAuth()
                    }
                }
            }
        }

        ColumnLayout {
            objectName: "crossSigningApprovalPrompt"
            Layout.fillWidth: true
            visible: app.backup && app.backup.approvalUrl.length > 0
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: qsTr("Your account provider needs you to approve the new keys. Open "
                           + "the approval page, confirm there, and Lightning continues "
                           + "on its own.")
            }
            RowLayout {
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "crossSigningOpenApproval"
                    storm: true
                    kind: "primary"
                    text: qsTr("Open approval page")
                    onClicked: Qt.openUrlExternally(app.backup.approvalUrl)
                }
                AppButton {
                    objectName: "crossSigningApprovalCancel"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Cancel")
                    onClicked: app.backup.cancelAuth()
                }
            }
        }

        Label {
            objectName: "crossSigningWorking"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.working && !app.backup.passwordRequired
                     && app.backup.approvalUrl.length === 0
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
            text: app.backup && app.backup.cancelling ? qsTr("Cancelling…") : qsTr("Working…")
        }

        ColumnLayout {
            objectName: "crossSigningRecoveryChoice"
            Layout.fillWidth: true
            visible: root.needsRecoveryChoice
            spacing: AppTheme.spacing8
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: currentKeyField
                    objectName: "crossSigningCurrentRecoveryKey"
                    storm: true
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                                      | Qt.ImhNoAutoUppercase
                    placeholderText: qsTr("Current recovery key")
                    Accessible.name: qsTr("Current recovery key")
                    onAccepted: useCurrentKey.clicked()
                }
                AppButton {
                    id: useCurrentKey
                    objectName: "crossSigningUseCurrentKey"
                    storm: true
                    kind: "primary"
                    text: qsTr("Continue")
                    enabled: currentKeyField.text.trim().length > 0 && !app.backup.busy
                    onClicked: {
                        if (!enabled)
                            return
                        root.pendingConfirm = ""
                        app.backup.runCrossSigning(app.backup.lastAction,
                                                   currentKeyField.text, false)
                        currentKeyField.text = ""
                    }
                }
            }
            Label {
                objectName: "crossSigningReplaceConsequence"
                Layout.fillWidth: true
                visible: root.pendingConfirm === "replace"
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textBody
                text: qsTr("A new recovery key replaces the current one, which stops "
                           + "working. The new one can only hold what this session has: "
                           + "if this session does not have your key backup's key, "
                           + "history that exists only in your key backup becomes "
                           + "unreachable from new sessions. Press again to confirm.")
            }
            Flow {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "crossSigningReplaceRecoveryKey"
                    storm: true
                    kind: root.pendingConfirm === "replace" ? "danger" : "ghost"
                    size: "sm"
                    enabled: !app.backup.busy
                    text: root.pendingConfirm === "replace"
                          ? qsTr("Replace my recovery key")
                          : qsTr("I don't have it…")
                    onClicked: {
                        if (root.pendingConfirm !== "replace") {
                            root.pendingConfirm = "replace"
                            return
                        }
                        root.pendingConfirm = ""
                        currentKeyField.text = ""
                        app.backup.runCrossSigning(app.backup.lastAction, "", true)
                    }
                }
                AppButton {
                    visible: root.pendingConfirm === "replace"
                    storm: true
                    kind: "ghost"
                    size: "sm"
                    text: qsTr("Cancel")
                    onClicked: root.pendingConfirm = ""
                }
            }
        }

        Label {
            objectName: "crossSigningError"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textBody
            visible: app.backup && app.backup.error.length > 0 && root.ownAction
            text: app.backup ? app.backup.error : ""
        }

        Label {
            objectName: "crossSigningNotice"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            visible: app.backup && app.backup.notice.length > 0 && root.ownAction
            text: app.backup ? app.backup.notice : ""
        }

        Rectangle {
            objectName: "crossSigningRecoveryKeyBox"
            Layout.fillWidth: true
            visible: app.backup && app.backup.recoveryKey.length > 0
            radius: AppTheme.radiusTile
            color: AppTheme.stormCanvas
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            implicitHeight: keyCol.implicitHeight + AppTheme.spacing12 * 2
            ColumnLayout {
                id: keyCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing6
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    text: qsTr("Cross-signing is set up. Your new recovery key: write it down "
                               + "now. Lightning does not keep it and will not show it again.")
                }
                TextEdit {
                    objectName: "crossSigningRecoveryKeyText"
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: AppTheme.stormText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.textBody
                    text: app.backup ? app.backup.recoveryKey : ""
                }
                RowLayout {
                    Item { Layout.fillWidth: true }
                    AppButton {
                        storm: true
                        kind: "primary"
                        size: "sm"
                        text: qsTr("I have saved it")
                        onClicked: app.backup.dismissRecoveryKey()
                    }
                }
            }
        }

        Label {
            objectName: "crossSigningConfirmText"
            Layout.fillWidth: true
            visible: root.pendingConfirm === "reset"
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textBody
            text: qsTr("Replacing your cross-signing identity cannot be undone. Everyone "
                       + "who verified you will see your identity change and has to trust "
                       + "you again, and your other sessions become unverified. If you "
                       + "have a recovery key, Lightning asks for it next so it keeps "
                       + "working. Your messages are not deleted. Press again to confirm.")
        }

        Flow {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: !(app.backup && (app.backup.passwordRequired
                                      || app.backup.approvalUrl.length > 0))
                     && !root.needsRecoveryChoice
            AppButton {
                objectName: "setUpCrossSigningButton"
                visible: root.setupState === "not_set_up" || root.setupState === "unconfirmed"
                storm: true
                kind: "primary"
                size: "sm"
                enabled: !app.backup.busy
                text: root.working ? qsTr("Setting up…")
                                   : root.setupState === "unconfirmed"
                                     ? qsTr("Finish setting up")
                                     : qsTr("Set up cross-signing")
                onClicked: {
                    root.pendingConfirm = ""
                    app.backup.runAction("setup_cross_signing")
                }
            }
            AppButton {
                objectName: "resetCrossSigningButton"
                visible: root.setupState === "keys_missing"
                storm: true
                kind: "danger"
                size: "sm"
                enabled: !app.backup.busy
                text: root.pendingConfirm === "reset"
                      ? qsTr("Replace identity") : qsTr("I can't verify or recover. Reset…")
                onClicked: {
                    if (root.pendingConfirm !== "reset") {
                        root.pendingConfirm = "reset"
                        return
                    }
                    root.pendingConfirm = ""
                    app.backup.runAction("reset_cross_signing")
                }
            }
            AppButton {
                visible: root.pendingConfirm === "reset"
                storm: true
                kind: "ghost"
                size: "sm"
                text: qsTr("Cancel")
                onClicked: root.pendingConfirm = ""
            }
        }
    }
}
