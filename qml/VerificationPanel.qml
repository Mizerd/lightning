import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The session-verification flow body, hosted by VerificationDialog.qml (a
// focused modal, as in Element: the user looks back and forth between two
// devices). A plain layout so the one flow can be embedded elsewhere without a
// second copy. Nothing here promotes trust locally: every button is a request
// to the SDK, and the states are AppController's cache of what the SDK said.
ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        // Progress for the post-"They match" states: the press is acknowledged
        // and the peer wait is named.
        AppBusyIndicator {
            color: AppTheme.bolt
            size: 18
            visible: running
            // A displayed QR code asks the user to act, so "ready" does not
            // spin; confirming the scan does.
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
            color: app.verificationState === "done"
                   ? AppTheme.stormSuccess
                   : AppTheme.stormTextMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            text: {
                // The show-QR leg owns the message while a code is displayed.
                // Cleared on every terminal state, so it never masks
                // done/cancelled/failed.
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
                    return qsTr("Incoming verification request from %1")
                        .arg(app.verificationOtherUser)
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
                if (app.verificationState === "done")
                    return qsTr(
                        "Verification complete. This session is now " +
                        "verified; Lightning is refreshing the trust " +
                        "state and requesting encryption keys.")
                if (app.verificationState === "cancelled")
                    return qsTr("Verification cancelled.")
                if (app.verificationState.indexOf("failed") === 0) {
                    // The reason is what tells the user what to do.
                    // AppController stores it as "failed:<msg>".
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
            // Theme-independent: a QR code needs dark modules on a light field
            // with a quiet zone. The modules and 4-module quiet zone are baked
            // in by QrImageProvider.
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
                // The provider rounds down to whole modules and returns that
                // exact bitmap, so nothing resamples a module edge.
                sourceSize: Qt.size(240, 240)
                width: implicitWidth
                height: implicitHeight
                smooth: false
                // A verification code is single-use and secret; never cache it.
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
            // Starting emoji from this side would invalidate the displayed
            // code, so the switch is left to the other device (it also happens
            // automatically if the code goes unscanned). An in-app "Use emoji
            // instead" needs a new FFI.
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
            // Appears once the SDK reports the peer scanned. Nothing is
            // auto-confirmed, and the press is a request to the SDK, never a
            // local trust promotion.
            visible: app.verificationQrScanned
            AppButton {
                storm: true
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
        // Emoji stay visible through confirming/waiting for continued
        // comparison.
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
                    // Width-bound, not centerIn: a long SAS word would bleed
                    // over its neighbours.
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: AppTheme.spacing4
                    spacing: 2
                    Label {
                        text: modelData.symbol || ""
                        // The colour emoji face by name: Qt 6.8 picks a
                        // monochrome face otherwise.
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
                        // Wrap, never elide: the word is compared across
                        // devices.
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
            text: qsTr("Accept")
            visible: app.verificationState === "requested"
            kind: "primary"
            onClicked: app.acceptVerification()
        }
        AppButton {
            storm: true
            text: qsTr("They match")
            // Stays visible but disabled through the confirm/peer wait so the
            // card does not jump; only sas_ready accepts the press
            // (AppController guards too).
            visible: app.verificationState === "sas_ready"
                    || app.verificationState === "confirming"
                    || app.verificationState === "waiting_for_peer"
            enabled: app.verificationState === "sas_ready"
            kind: "primary"
            onClicked: app.confirmVerification()
        }
        AppButton {
            storm: true
            text: qsTr("They do not match")
            visible: app.verificationState === "sas_ready"
                    || app.verificationState === "confirming"
                    || app.verificationState === "waiting_for_peer"
            enabled: app.verificationState === "sas_ready"
            onClicked: app.mismatchVerification()
        }
        AppButton {
            storm: true
            text: qsTr("Cancel verification")
            // Cancel must exist in every non-terminal state, so the condition
            // is inverted rather than listing states: a newly added state
            // cannot lose the only way out.
            visible: app.verificationState !== ""
                    && app.verificationState !== "done"
                    && app.verificationState !== "cancelled"
                    && !app.verificationState.startsWith("failed")
            onClicked: app.cancelVerification()
        }
        AppButton {
            storm: true
            text: qsTr("Dismiss")
            visible: app.verificationState === "done"
                    || app.verificationState === "cancelled"
                    || app.verificationState.indexOf("failed") === 0
            onClicked: app.cancelVerification()
        }
    }
}
