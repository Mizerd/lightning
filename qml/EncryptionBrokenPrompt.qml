import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The one surface for "this session can never decrypt anything": the device
// published a curve25519 identity key its local Olm account doesn't hold.
// Peers encrypt to the published key, so every room key and call media key
// addressed to this device is unreadable for good (messages stay on "Waiting
// for keys…", calls are silent one way), while sending still works.
//
// The repair is signing out and in again, and its cost is stated: it's a new
// device, and history not in key backup won't come back. Deliberately not
// automated (CLAUDE.md §6: no crypto-store reset as a normal repair).
// app.auth.logout() is the ordinary sign-out; nothing here touches the crypto
// store.
//
// A corner card like VerifySessionPrompt so sending isn't blocked by a modal,
// but in a stronger tone.
Rectangle {
    id: root

    // Session-only suppression, re-armed on any login change: a persisted
    // dismissal would silence a permanent fault on every later launch.
    property bool suppressed: false

    readonly property bool shouldShow:
        app.encryptionIdentityBroken
        && !suppressed
        // Chat shell only (currentScreen 1): the repair is a sign-out, and
        // there's no account to act on at login or boot.
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

        // Plain language, including that sending still works, which is why this
        // is hard to notice.
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

    // The cost is stated on the confirmation too: this is where the user
    // commits.
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
                // State whether backup exists rather than a generic "only if in
                // backup". `keyBackupUsable` is false when unknown too, so it
                // errs towards warning.
                readonly property bool backupUsable:
                    app.cryptoHealth && app.cryptoHealth.keyBackupUsable
                text: qsTr("Signing in again creates a NEW session with new "
                           + "keys, which is what repairs this. Be aware of "
                           + "the cost:\n\n")
                      + (backupUsable
                         ? qsTr("• Encrypted messages you have already "
                                + "received come back only if they are in "
                                + "your key backup. Anything not backed up "
                                + "is not recoverable on this computer.\n")
                         : qsTr("• KEY BACKUP IS NOT SET UP on this account, "
                                + "so encrypted messages already on this "
                                + "computer will NOT come back. This is not "
                                + "reversible.\n"))
                      + qsTr(""
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
