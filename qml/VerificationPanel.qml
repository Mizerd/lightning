import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.x: the session-verification flow, extracted verbatim from the card
// that used to live inline at the bottom of Settings → Sessions.
//
// It moved because that is the wrong place for it: verification is a
// two-device task where the user is looking back and forth between screens,
// and burying it under a scrolled settings page meant the emojis could be
// off-screen at the moment they matter. Element puts this in a focused
// modal; VerificationDialog.qml is that modal, and this is its body — kept
// as a plain layout so the ONE flow presentation can also be embedded
// anywhere else without a second copy drifting out of step.
//
// Every state string, guard and objectName is unchanged from the inline
// card. Nothing here promotes trust locally: each button is a request to
// the SDK, and the states are AppController's cache of what the SDK said.
ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        // v0.7.1: live progress feedback
        // for the two post-"They match"
        // states — the press is
        // acknowledged immediately and
        // the peer wait is named.
        BusyIndicator {
            implicitWidth: 18
            implicitHeight: 18
            visible: running
            // While a QR code is on
            // screen the user is meant
            // to act, not wait, so
            // "ready" does not spin —
            // but confirming the scan
            // does.
            running: app.verificationState === "confirming"
                     || app.verificationState === "waiting_for_peer"
                     || app.verificationQrConfirming
                     || (app.verificationState === "ready"
                         && !app.verificationQrAvailable)
        }
        Label {
            objectName: "verificationStatusLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: app.verificationState === "done"
                   ? AppTheme.stormSuccess
                   : AppTheme.stormTextMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            text: {
                // The show-QR leg owns
                // the message while a
                // code is displayed. It
                // is cleared on every
                // terminal state, so
                // this can never mask
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
                    // The reason is the
                    // whole value of this
                    // line: "no
                    // cross-signing
                    // identity" tells the
                    // user what to do,
                    // "Verification
                    // failed." does not.
                    // AppController stores
                    // it as "failed:<msg>".
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
    // ── Show-QR panel ───────────────
    // Lightning DISPLAYS a code for the
    // other device to scan; it never
    // scans (no camera), so
    // m.qr_code.scan.v1 is never
    // advertised. Shown only while the
    // SDK has a live code for THIS flow;
    // AppController drops it on every
    // terminal state, on cancel, and on
    // logout, so a code cannot outlive
    // its flow.
    ColumnLayout {
        objectName: "verificationQrPanel"
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        visible: app.verificationQrAvailable

        Rectangle {
            // Deliberately theme-INDEPENDENT.
            // A QR code has to be dark
            // modules on a light field
            // with a quiet zone for a
            // camera to read it; that is
            // a physical constraint, not
            // a styling choice, so this
            // one surface stays white
            // under every theme. The
            // dark modules and the
            // 4-module quiet zone are
            // baked into the image by
            // QrImageProvider.
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
                // Ask for a size the
                // provider can round DOWN
                // to whole modules; it
                // returns the exact
                // whole-module bitmap and
                // the item takes that
                // natural size, so no
                // resampling can blur a
                // module edge.
                sourceSize: Qt.size(240, 240)
                width: implicitWidth
                height: implicitHeight
                smooth: false
                // A verification code is
                // single-use and secret;
                // it must never sit in
                // the QML image cache.
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
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.fontCaption
            color: AppTheme.stormTextMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            // The honest fallback note.
            // Starting emoji from THIS
            // side would invalidate the
            // code currently on screen,
            // so while one is displayed
            // the switch is left to the
            // other device — and it also
            // happens automatically if
            // the code goes unscanned.
            // An in-app "Use emoji
            // instead" button is an
            // accepted follow-up (it
            // needs a new FFI).
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
            // Appears only once the SDK
            // reports the peer actually
            // scanned. Nothing is ever
            // auto-confirmed, and this
            // press is a request to the
            // SDK — never a local trust
            // promotion.
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
        // Emoji stay on screen through
        // confirming/waiting so users can
        // keep comparing while the flow
        // settles.
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
                    // Width-bound, not
                    // centerIn: a long SAS
                    // word ("Headphones")
                    // otherwise widens the
                    // layout past the 84px
                    // tile and bleeds over
                    // its neighbours.
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: AppTheme.spacing4
                    spacing: 2
                    Label {
                        text: modelData.symbol || ""
                        font.pixelSize: 28
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Label {
                        text: modelData.description || ""
                        font.pixelSize: AppTheme.fontCaption
                        color: AppTheme.stormTextMuted
                        horizontalAlignment: Text.AlignHCenter
                        // Wrap, never elide: the user is
                        // asked to COMPARE this word across
                        // devices — a truncated word is
                        // worse than a two-line caption.
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
            // v0.7.1: stays visible (but
            // disabled) through the
            // confirm/peer wait so the
            // card does not jump; only
            // sas_ready accepts the press
            // (AppController enforces the
            // same guard).
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
            // Cancel must exist in EVERY
            // non-terminal state. Listing
            // states positively meant a
            // newly added one ("ready")
            // silently lost the only way
            // out: that card shows a
            // spinner and no buttons at
            // all, so a peer that never
            // advertised m.sas.v1 pinned
            // the user to it. Inverted so
            // the next added state cannot
            // drop the escape hatch again.
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
