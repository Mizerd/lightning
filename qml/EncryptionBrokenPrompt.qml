import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// B011: THE ONE SURFACE FOR "THIS SESSION CAN NEVER DECRYPT ANYTHING".
//
// THE DEFECT, diagnosed on a real account 2026-09-07. A device had published
// a curve25519 identity key that its own local Olm account did not hold.
// Peers encrypt to the key the SERVER publishes, so every room key and every
// call media key addressed to this device was unreadable, permanently:
// encrypted messages sit on "Waiting for keys…" forever, and an encrypted
// call is silent one way while the other side hears you perfectly, because
// SENDING is unaffected. Only matrix-sdk's own tracing could see it, and a
// fresh sign-in repaired it instantly.
//
// Detection shipped and then stopped at a log line the user never reads.
// This card is the missing half: it says what is wrong in plain language and
// offers the one repair that works.
//
// THE REPAIR IS SIGNING OUT AND SIGNING IN AGAIN, and its real cost is
// stated rather than glossed: it is a NEW device, and message history that
// is not in key backup will not come back to it. Deliberately NOT automated:
// CLAUDE.md §6 forbids treating a crypto-store reset as a normal repair, so
// this stays an explicit, account-scoped, last-resort action the user takes
// knowingly. Nothing here touches the crypto store; app.auth.logout() is the
// same ordinary sign-out the account menu offers.
//
// Same corner-card shape as VerifySessionPrompt on purpose — an app the user
// can still send in must not be blocked by a modal — but a stronger tone,
// because this one is not a nudge: nothing they receive will open.
Rectangle {
    id: root

    // Session-only suppression. There is no persisted dismissal: a permanent
    // fault that a user silenced once would then be silent on every later
    // launch, and this is the state in which every incoming message is
    // unreadable. Re-armed on any login change, like VerifySessionPrompt.
    property bool suppressed: false

    readonly property bool shouldShow:
        app.encryptionIdentityBroken
        && !suppressed
        // The chat shell only (currentScreen 1), never over login or boot:
        // there is no account to act on there, and the repair is a sign-out.
        && app.currentScreen === 1

    Connections {
        target: app
        function onLoggedInChanged() { root.suppressed = false }
    }

    objectName: "encryptionBrokenPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 340
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    border.color: AppTheme.stormDanger
    border.width: 1

    Accessible.role: Accessible.AlertMessage
    Accessible.name: qsTr("This session cannot read encrypted messages")

    ColumnLayout {
        id: promptColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: AppTheme.spacing16
        anchors.rightMargin: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "warning"
                size: 18
                color: AppTheme.stormDanger
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                objectName: "encryptionBrokenPromptTitle"
                Layout.fillWidth: true
                text: qsTr("Encrypted messages can't be opened")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                wrapMode: Text.WordWrap
            }
        }

        // PLAIN LANGUAGE, AND THE HONEST SHAPE OF IT: sending still works,
        // which is exactly why this is so confusing to notice or report.
        Label {
            objectName: "encryptionBrokenPromptBody"
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("The encryption keys this session published no longer "
                       + "match the ones it holds, so nothing sent to it can "
                       + "be unlocked. Encrypted messages stay on "
                       + "\"Waiting for keys\", and in a call others hear you "
                       + "while you hear nothing. Sending is unaffected, so "
                       + "everything looks normal from your side.\n\n"
                       + "Signing out and signing in again is the only fix.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppButton {
                objectName: "encryptionBrokenPromptFix"
                storm: true
                kind: "danger"
                Layout.fillWidth: true
                text: qsTr("Sign out and back in")
                onClicked: signOutConfirm.open()
            }
            AppButton {
                objectName: "encryptionBrokenPromptDismiss"
                storm: true
                Layout.fillWidth: true
                text: qsTr("Not now")
                onClicked: root.suppressed = true
            }
        }
    }

    // The cost is stated HERE, on the confirmation, not only in the card:
    // this is the moment the user commits to it.
    Dialog {
        id: signOutConfirm
        objectName: "encryptionBrokenSignOutDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(460, parent ? parent.width - 32 : 460))
        modal: true
        title: qsTr("Sign out and sign in again?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            radius: AppTheme.radiusLg
        }

        header: Label {
            text: signOutConfirm.title
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing16
            rightPadding: AppTheme.spacing16
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                objectName: "encryptionBrokenSignOutConsequences"
                Layout.fillWidth: true
                text: qsTr("Signing in again creates a NEW session with new "
                           + "keys, which is what repairs this. Be aware of "
                           + "the cost:\n\n"
                           + "• Encrypted messages you have already received "
                           + "will only come back if they are in your key "
                           + "backup. Anything that is not backed up is not "
                           + "recoverable on this computer.\n"
                           + "• The new session starts unverified, so you "
                           + "will need to verify it from another device or "
                           + "with your recovery key.\n"
                           + "• Lightning's local data for this account on "
                           + "this computer is removed. Your messages stay on "
                           + "the server.")
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "encryptionBrokenSignOutCancel"
                    storm: true
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: signOutConfirm.close()
                }
                AppButton {
                    objectName: "encryptionBrokenSignOutConfirm"
                    storm: true
                    kind: "danger"
                    text: qsTr("Sign out")
                    Accessible.name: qsTr("Confirm sign out")
                    onClicked: {
                        signOutConfirm.close()
                        app.auth.logout()
                    }
                }
            }
        }
    }
}
