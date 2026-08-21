import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.7.x: the first-run "verify this session" prompt — a small corner card,
// not a modal. A brand-new session is usable before it is verified, so this
// must not block the app the way a dialog would; it asks once, offers the
// two honest answers, and gets out of the way.
//
// Shown when the session genuinely needs verifying AND the reminder has not
// been dismissed for this account (app.sessionVerificationWarning). It also
// stands down while a verification flow is already running — the focused
// dialog owns the screen at that point, and a prompt telling the user to
// start what they have already started is noise.
//
// "Verify" starts the SAME flow the Settings page starts; the shared
// VerificationDialog opens itself off AppController's state, so there is
// exactly one verification presentation in the app.
// "Not now" is a real dismissal: it persists per account and is cleared
// automatically once the session verifies, so it can never hide a later
// unverified session.
Rectangle {
    id: root

    // The one-shot suppression for THIS run of the shell: hides the card
    // without persisting anything, used after Verify is pressed so the card
    // does not flash back between the press and the flow's first state.
    property bool suppressed: false

    readonly property bool shouldShow:
        app.sessionVerificationWarning
        && !app.verificationActive
        && app.verificationState === ""
        && !suppressed
        // The chat shell only (currentScreen 1). Deliberately NOT Settings
        // (2): the Sessions page already carries the TrustCard's Verify —
        // the single start affordance that page is documented to keep — and
        // a floating prompt offering the same action a few hundred pixels
        // away is exactly the duplication that decision avoided. Never over
        // login/boot either, where there is no account to act on.
        && app.currentScreen === 1

    // Re-arm on a genuinely new situation (account switch, a fresh sign-in
    // that lands unverified again) rather than staying suppressed for the
    // lifetime of the window.
    Connections {
        target: app
        function onLoggedInChanged() { root.suppressed = false }
    }

    objectName: "verifySessionPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 316
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    // Danger-toned border, not a danger FILL: this is a nudge, not an
    // error, and a solid red card in the corner of a working app reads as
    // something having gone wrong.
    border.color: AppTheme.stormDanger
    border.width: 1

    // No MultiEffect shadow here: the sanctioned pattern needs the effect
    // and its source to be SIBLINGS (see MemberProfilePopover), and this
    // card IS its own surface. The danger border already separates it from
    // the shell, and a floating card that also casts a shadow onto the
    // timeline would read heavier than a nudge should.

    Accessible.role: Accessible.AlertMessage
    Accessible.name: qsTr("This session is not verified")

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
                Layout.fillWidth: true
                text: qsTr("Verify this session")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                elide: Label.ElideRight
            }
        }

        Label {
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Until you do, your other sessions won't share "
                       + "encryption keys with this one, so some encrypted "
                       + "messages may stay unreadable here.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppButton {
                objectName: "verifySessionPromptVerify"
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Verify")
                onClicked: {
                    root.suppressed = true
                    app.startOwnVerification()
                }
            }
            AppButton {
                objectName: "verifySessionPromptDismiss"
                storm: true
                Layout.fillWidth: true
                text: qsTr("Not now")
                onClicked: app.dismissVerificationWarning()
            }
        }
    }
}
