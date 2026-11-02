import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    // Hands keyboard focus to the SAFE action of the current step: Dismiss on a
    // finished flow, otherwise Cancel. Never Accept / They match / It reported
    // success: the step appears asynchronously, and a stray Space or Enter must
    // not confirm a SAS without the emoji being compared (CLAUDE.md section 6).
    // The affirmative buttons are reached deliberately with Tab.
    function focusPrimary() {
        var order = [dismissButton, cancelButton]
        for (var i = 0; i < order.length; ++i) {
            var b = order[i]
            if (b && b.visible && b.enabled) {
                b.forceActiveFocus(Qt.TabFocusReason)
                return true
            }
        }
        return false
    }
    function _holdsFocus() {
        var all = [acceptButton, matchButton, qrConfirmButton,
                   dismissButton, cancelButton]
        for (var i = 0; i < all.length; ++i)
            if (all[i] && all[i].visible && all[i].activeFocus)
                return true
        return false
    }
    onVisibleChanged: if (visible) focusPrimary()
    Connections {
        target: app
        function onVerificationStateChanged() {
            Qt.callLater(function() {
                if (!root._holdsFocus())
                    root.focusPrimary()
            })
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        AppBusyIndicator {
            color: AppTheme.bolt
            size: 18
            visible: running
            running: app.verificationState === "confirming"
                     || app.verificationState === "waiting_for_peer"
                     || app.verificationQrConfirming
                     || (app.verificationState === "ready"
                         && !app.verificationQrAvailable)
        }
        Label {
            objectName: "verificationStatusLabel"
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: app.verificationState === "done"
                   ? AppTheme.stormSuccess
                   : (app.verificationState === "cancelled"
                      && app.verificationCancelReason === "mismatch")
                     ? AppTheme.stormDanger
                     : AppTheme.stormTextMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            text: {
                if (app.verificationQrAvailable) {
                    if (app.verificationQrConfirming)
                        return qsTr("Confirming verification…")
                    if (app.verificationQrScanned)
                        return qsTr(
                            "Your other device scanned the code. " +
                            "Confirm below only if that device says " +
                            "the verification succeeded.")
                    return qsTr(
                        "Scan this code with your other device to " +
                        "verify this session.")
                }
                if (app.verificationState === "starting")
                    return qsTr("Sending verification request…")
                if (app.verificationState === "waiting_for_other_session")
                    return qsTr(
                        "Verification request sent. Accept it in " +
                        "another session, such as Element.")
                if (app.verificationState === "requested")
                {
                    if (app.verificationOtherDevice !== "")
                        return qsTr("Incoming verification request from "
                                    + "%1, session %2")
                            .arg(app.verificationOtherUser)
                            .arg(app.verificationOtherDevice)
                    return qsTr("Incoming verification request from %1")
                        .arg(app.verificationOtherUser)
                }
                if (app.verificationState === "ready")
                    return qsTr(
                        "Both sessions accepted. Exchanging keys — " +
                        "the emojis will appear here shortly.")
                if (app.verificationState === "sas_ready")
                    return qsTr(
                        "Compare all seven emojis with the other " +
                        "session. Confirm only if every emoji matches " +
                        "in the same order.")
                if (app.verificationState === "confirming")
                    return qsTr("Confirming verification…")
                if (app.verificationState === "waiting_for_peer")
                    return qsTr(
                        "Waiting for your other device to confirm…")
                if (app.verificationState === "done") {
                    if (app.verificationRole === "this_session")
                        return qsTr(
                            "Verification complete. This session is now " +
                            "verified; Lightning is refreshing the trust " +
                            "state and requesting encryption keys.")
                    if (app.verificationRole === "other_session")
                        return qsTr(
                            "Verification complete. You verified your " +
                            "other session.")
                    if (app.verificationRole === "other_user")
                        return qsTr("Verification complete. You verified %1.")
                            .arg(app.verificationOtherUser)
                    return qsTr(
                        "Verification complete. Lightning is refreshing " +
                        "the trust state and requesting encryption keys.")
                }
                if (app.verificationState === "cancelled") {
                    if (app.verificationCancelReason === "mismatch")
                        return qsTr(
                            "The security check did not match, so nothing " +
                            "was verified. Someone may be intercepting the " +
                            "connection: do not trust it until you know why.")
                    if (app.verificationCancelReason === "timeout")
                        return qsTr(
                            "Verification timed out. Start it again to " +
                            "try once more.")
                    return qsTr("Verification cancelled.")
                }
                if (app.verificationState.indexOf("failed") === 0) {
                    var reason =
                        app.verificationState
                            .substring(7)
                    if (reason.length > 0)
                        return qsTr(
                            "Verification failed: %1")
                            .arg(reason)
                    return qsTr("Verification failed.")
                }
                return qsTr("Waiting…")
            }
        }
    }
    // Show-QR panel. Lightning displays a code for the other device to scan and
    // never scans (no camera), so m.qr_code.scan.v1 is never advertised. Shown
    // only while the SDK has a live code for this flow; AppController drops it
    // on every terminal state, cancel and logout.
    ColumnLayout {
        objectName: "verificationQrPanel"
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        visible: app.verificationQrAvailable

        Rectangle {
            color: "#FFFFFF"
            radius: AppTheme.radiusSm
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: qrImage.width + 2 * AppTheme.spacing8
            implicitHeight: qrImage.height + 2 * AppTheme.spacing8

            Image {
                id: qrImage
                objectName: "verificationQrImage"
                anchors.centerIn: parent
                source: app.verificationQrImage
                sourceSize: Qt.size(240, 240)
                width: implicitWidth
                height: implicitHeight
                smooth: false
                cache: false
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr(
                    "Verification QR code — scan with your " +
                    "other device")
            }
        }

        Label {
            objectName: "verificationQrHint"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.textMeta
            color: AppTheme.stormTextMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            text: app.verificationQrScanned
                ? qsTr("Only confirm if the other device reports success.")
                : qsTr(
                    "Can't scan? Choose emoji verification on the " +
                    "other device instead.")
        }

        Flow {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: AppTheme.spacing8
            visible: app.verificationQrScanned
            AppButton {
                storm: true
                id: qrConfirmButton
                objectName: "verificationQrConfirmButton"
                text: qsTr("It reported success")
                kind: "primary"
                enabled: !app.verificationQrConfirming
                onClicked: app.confirmQrVerification()
            }
            AppButton {
                storm: true
                objectName: "verificationQrRejectButton"
                text: qsTr("It did not")
                enabled: !app.verificationQrConfirming
                onClicked: app.cancelVerification()
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        visible: app.verificationState === "sas_ready"
                 || app.verificationState === "confirming"
                 || app.verificationState === "waiting_for_peer"
        Repeater {
            model: app.verificationEmojis
            delegate: Rectangle {
                color: AppTheme.stormCanvas
                border.color: AppTheme.stormBorder
                radius: AppTheme.radiusSm
                implicitWidth: 84
                implicitHeight: 78
                ColumnLayout {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: AppTheme.spacing4
                    spacing: 2
                    Label {
                        text: modelData.symbol || ""
                        font.family: app.emojiFontFamily || ""
                        font.pixelSize: 28
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Label {
                        text: modelData.description || ""
                        font.pixelSize: AppTheme.textMeta
                        color: AppTheme.stormTextMuted
                        horizontalAlignment: Text.AlignHCenter
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        AppButton {
            storm: true
            id: acceptButton
            objectName: "verificationAcceptButton"
            text: qsTr("Accept")
            visible: app.verificationState === "requested"
            kind: "primary"
            onClicked: app.acceptVerification()
        }
        AppButton {
            storm: true
            id: matchButton
            objectName: "verificationMatchButton"
            text: qsTr("They match")
            visible: app.verificationState === "sas_ready"
                    || app.verificationState === "confirming"
                    || app.verificationState === "waiting_for_peer"
            enabled: app.verificationState === "sas_ready"
            kind: "primary"
            onClicked: app.confirmVerification()
        }
        AppButton {
            storm: true
            objectName: "verificationMismatchButton"
            text: qsTr("They do not match")
            visible: app.verificationState === "sas_ready"
                    || app.verificationState === "confirming"
                    || app.verificationState === "waiting_for_peer"
            enabled: app.verificationState === "sas_ready"
            onClicked: app.mismatchVerification()
        }
        AppButton {
            storm: true
            id: cancelButton
            objectName: "verificationCancelButton"
            text: qsTr("Cancel verification")
            visible: app.verificationState !== ""
                    && app.verificationState !== "done"
                    && app.verificationState !== "cancelled"
                    && !app.verificationState.startsWith("failed")
            onClicked: app.cancelVerification()
        }
        AppButton {
            storm: true
            id: dismissButton
            objectName: "verificationDismissButton"
            text: qsTr("Dismiss")
            visible: app.verificationState === "done"
                    || app.verificationState === "cancelled"
                    || app.verificationState.indexOf("failed") === 0
            onClicked: app.cancelVerification()
        }
    }
}
