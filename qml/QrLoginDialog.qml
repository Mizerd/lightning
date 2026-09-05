import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// MSC4108 — signing ANOTHER device in from this one.
//
// # What the two flows are, in the user's terms
//
// SHOW: this device puts a code on screen, the new device's camera reads it,
// the new device then shows two digits and the user types them here.
// ENTER: the new device shows the code, its text is pasted here, and THIS
// device shows two digits for the user to type over there.
//
// Both finish at a page the user opens to confirm — and then the new device
// is not only signed in but CROSS-SIGNED, because the SDK moves the private
// cross-signing keys and the backup key across the channel. That is worth
// saying on screen: it is the difference between this and typing a password,
// and it is why the digits matter.
//
// # There is no camera
//
// Lightning bundles no camera-frame decoder, so the ENTER flow takes the
// code's TEXT — which every client that displays one also offers. The dialog
// says that rather than showing a viewfinder that will never fill.
Dialog {
    id: root
    objectName: "qrLoginDialog"

    readonly property var qr: app.qrLogin
    readonly property string state: qr ? qr.state : "idle"

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    // Escape cancels the FLOW, not just the dialog: a channel left open is a
    // channel something else can still complete.
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent ? parent.width - AppTheme.spacing24 * 2 : 460)
    padding: AppTheme.spacing16

    onClosed: if (root.qr) root.qr.cancel()

    function openDialog() {
        if (root.qr)
            root.qr.cancel()
        pasteField.text = ""
        codeField.text = ""
        open()
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            text: qsTr("Sign in another device")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        // ── Idle: choose a direction ─────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "idle" || root.state === "failed"

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("The other device will be signed in AND verified: "
                           + "your cross-signing and message-backup keys are "
                           + "sent to it over an encrypted channel, so it can "
                           + "read your existing conversations straight away.")
            }

            AppButton {
                objectName: "qrLoginShowButton"
                Layout.fillWidth: true
                text: qsTr("Show a code for the other device to scan")
                kind: "primary"
                enabled: root.qr && root.qr.available
                onClicked: root.qr.showCode()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("…or paste the code the other device is showing")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: pasteField
                    Layout.fillWidth: true
                    // A field a layout can squeeze to nothing takes its
                    // neighbour's width with it.
                    Layout.minimumWidth: 0
                    placeholderText: qsTr("Paste the sign-in code")
                    onAccepted: root.qr.enterCode(text)
                }
                AppButton {
                    text: qsTr("Continue")
                    size: "sm"
                    enabled: root.qr && root.qr.available
                             && pasteField.text.trim().length > 0
                    onClicked: root.qr.enterCode(pasteField.text)
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: root.qr && !root.qr.available
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("This build cannot sign in another device.")
            }
        }

        // ── Showing our code ─────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "showing"

            Image {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 220
                Layout.preferredHeight: 220
                visible: source != ""
                // Nearest-neighbour, black on white, quiet zone included —
                // the provider owns all of that, because a QR code has to be
                // readable by a camera and that is a physical constraint
                // rather than a styling choice.
                smooth: false
                fillMode: Image.PreserveAspectFit
                source: root.qr ? root.qr.qrSource : ""
                cache: false
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Scan this with the other device. If it cannot "
                           + "scan, copy the text below into it instead.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                visible: root.qr && root.qr.qrText.length > 0
                AppTextField {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    readOnly: true
                    text: root.qr ? root.qr.qrText : ""
                }
                AppButton {
                    text: qsTr("Copy")
                    size: "sm"
                    // QML has no clipboard API; a hidden TextEdit's copy() is
                    // the route every other surface here uses.
                    onClicked: {
                        clipboardHelper.text = root.qr.qrText
                        clipboardHelper.selectAll()
                        clipboardHelper.copy()
                        clipboardHelper.text = ""
                    }
                }
            }
        }

        // ── They scanned: we need their digits ───────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "waiting_for_code"

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
                text: qsTr("The other device is showing two digits. Type them "
                           + "here.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                // Why it exists, not just what to do. Someone who knows this
                // is the check against a code intercepted in transit will not
                // guess at it.
                text: qsTr("This proves the two devices are talking to each "
                           + "other and not to something in between. If the "
                           + "digits do not match, stop.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: codeField
                    Layout.preferredWidth: 90
                    placeholderText: qsTr("00")
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 0; top: 99 }
                    onAccepted: root.qr.submitCheckCode(parseInt(text, 10))
                }
                AppButton {
                    text: qsTr("Confirm")
                    kind: "primary"
                    size: "sm"
                    enabled: codeField.acceptableInput
                    onClicked: root.qr.submitCheckCode(parseInt(codeField.text, 10))
                }
            }
        }

        // ── We scanned: they need OUR digits ─────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "code_shown"

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.qr && root.qr.checkCode >= 0
                      ? ("" + root.qr.checkCode).padStart(2, "0") : ""
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.textDisplay
                font.weight: AppTheme.weightBold
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Type these digits on the other device. If it "
                           + "shows something different, stop.")
            }
        }

        // ── Consent ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "waiting_for_auth"

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
                text: qsTr("One more step: confirm the new sign-in on your "
                           + "account page.")
            }
            AppButton {
                Layout.fillWidth: true
                text: qsTr("Open the confirmation page")
                kind: "primary"
                enabled: root.qr && root.qr.verificationUri.length > 0
                // Through the app's own launcher, whose allowlist accepts
                // http/https and nothing else.
                // app.media.openWebUrl, which routes through UrlLauncher —
                // its allowlist accepts http/https and refuses everything
                // else at the one exit to xdg-open.
                onClicked: app.media.openWebUrl(root.qr.verificationUri)
            }
        }

        // ── Working / finished ───────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: root.state === "starting" || root.state === "syncing"
            AppBusyIndicator { implicitWidth: 18; implicitHeight: 18 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                text: root.state === "syncing"
                      ? qsTr("Sending your keys to the new device…")
                      : qsTr("Starting…")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.state === "done"
            color: AppTheme.textPrimary
            text: qsTr("The other device is signed in and verified.")
        }

        Label {
            objectName: "qrLoginError"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: root.qr && root.qr.errorText.length > 0
            text: root.qr ? root.qr.errorText : ""
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: root.state === "done" || root.state === "idle"
                      || root.state === "failed"
                      ? qsTr("Close") : qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
        }
    }

    TextEdit {
        id: clipboardHelper
        visible: false
        width: 0
        height: 0
    }
}
