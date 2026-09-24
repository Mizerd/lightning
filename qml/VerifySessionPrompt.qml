import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// First-run "verify this session" corner card, not a modal: an unverified
// session still works. Shown when verification is needed and the reminder is
// not dismissed for this account (app.sessionVerificationWarning), and hidden
// while a flow is running. "Verify" starts the same flow as Settings (the
// shared VerificationDialog opens from AppController's state). "Not now"
// persists per account and clears once the session verifies.
Rectangle {
    id: root

    // One-shot suppression for this run, used after Verify so the card does not
    // flash back before the flow's first state.
    property bool suppressed: false

    readonly property bool shouldShow:
        app.sessionVerificationWarning
        && !app.verificationActive
        && app.verificationState === ""
        && !suppressed
        // Chat shell only: not Settings, where the TrustCard's Verify is the
        // single start affordance, and not login/boot.
        && app.currentScreen === 1

    // Re-arm on a new situation (account switch, fresh unverified sign-in).
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
    // A danger-toned border, not a fill: this is a nudge, not an error.
    border.color: AppTheme.stormDanger
    border.width: 1

    // No shadow: the sanctioned pattern needs effect and source as siblings
    // (see MemberProfilePopover), and the border already separates the card.

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
