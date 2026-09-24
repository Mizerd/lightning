import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The one reusable User-Interactive Authentication prompt, opened by
// UiaController's challenge state. Renders the password stage and states
// unsupported stages honestly. The field is wiped after every dispatch, on
// cancel and on close; echoMode stays Password with no reveal toggle.
Dialog {
    id: root
    objectName: "uiaPromptDialog"
    modal: true
    // The shared modal scrim.
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    // No click-outside dismissal: an auth prompt ends in an answer or an
    // explicit cancel.
    closePolicy: Popup.CloseOnEscape
    width: Math.min(420, parent ? parent.width - AppTheme.spacing24 * 2 : 420)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property bool challengeActive: app.uia.challengeActive
    onChallengeActiveChanged: {
        if (challengeActive) {
            open()
            passwordField.text = ""
            Qt.callLater(function() { passwordField.forceActiveFocus() })
        } else if (opened) {
            passwordField.text = ""
            close()
        }
    }
    onClosed: {
        passwordField.text = ""
        // Escape or a programmatic close while a challenge is live cancels it.
        if (app.uia.challengeActive)
            app.uia.cancel()
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Confirm it's you")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        Label {
            Layout.fillWidth: true
            text: app.uia.passwordStage
                  ? qsTr("The server requires your account password to "
                         + "sign out the selected session.")
                  : qsTr("The server requires an authentication step "
                         + "Lightning does not support yet (%1). Use "
                         + "another client or the server's account page "
                         + "for this action.")
                        .arg(app.uia.stages.join(", "))
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: app.uia.wrongPassword
            text: qsTr("That password was not accepted. Try again.")
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            wrapMode: Text.Wrap
            Accessible.name: text
        }
        AppTextField {
            id: passwordField
            objectName: "uiaPasswordField"
            Layout.fillWidth: true
            visible: app.uia.passwordStage
            storm: true
            echoMode: TextInput.Password
            placeholderText: qsTr("Account password")
            Accessible.name: qsTr("Account password")
            onAccepted: submitButton.clicked()
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                text: qsTr("Cancel")
                onClicked: {
                    passwordField.text = ""
                    app.uia.cancel()
                }
            }
            AppButton {
                id: submitButton
                objectName: "uiaSubmitButton"
                visible: app.uia.passwordStage
                storm: true
                kind: "primary"
                enabled: passwordField.text.length > 0
                text: qsTr("Confirm")
                onClicked: {
                    app.uia.submitPassword(passwordField.text)
                    // Wipe immediately after dispatch.
                    passwordField.text = ""
                }
            }
        }
    }
}
