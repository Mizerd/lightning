import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    property bool suppressed: false

    readonly property bool shouldShow:
        app.sessionVerificationWarning
        && !app.verificationActive
        && app.verificationState === ""
        && !suppressed
        && app.currentScreen === 1

    Connections {
        target: app
        function onLoggedInChanged() { root.suppressed = false }
    }

    objectName: "verifySessionPrompt"

    signal regionCycleRequested(bool backward)
    // F6 inside the card goes up through the overlay, never through
    // MainScreen, so the card hands it to the shell's cycle.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F6
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                        | Qt.MetaModifier))) {
            root.regionCycleRequested(!!(event.modifiers & Qt.ShiftModifier))
            event.accepted = true
        }
    }
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 316
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    border.color: AppTheme.stormDanger
    border.width: 1

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
                id: verifyButton
                objectName: "verifySessionPromptVerify"
                Keys.onEscapePressed: app.dismissVerificationWarning()
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
                Keys.onEscapePressed: app.dismissVerificationWarning()
                storm: true
                Layout.fillWidth: true
                text: qsTr("Not now")
                onClicked: app.dismissVerificationWarning()
            }
        }

        AppButton {
            objectName: "verifySessionPromptRecoveryToggle"
            storm: true
            kind: "ghost"
            size: "sm"
            visible: app.backendName === "rust"
            Layout.alignment: Qt.AlignLeft
            text: recoveryEntry.visible ? qsTr("Hide recovery key")
                                        : qsTr("Use recovery key instead")
            Accessible.name: text
            onClicked: recoveryEntry.visible = !recoveryEntry.visible
        }
        RecoveryKeyEntry {
            id: recoveryEntry
            objectName: "verifySessionPromptRecoveryEntry"
            Layout.fillWidth: true
            visible: false
            fieldObjectName: "verifySessionPromptRecoveryField"
            buttonText: qsTr("Use key")
        }
    }
}
