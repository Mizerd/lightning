import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.x: report a message to the homeserver administrator. ONE instance
// (hosted by Main.qml) that opens itself off ModerationController's
// pending-report state, so every message menu shares the same surface.
//
// Only the event reference and the typed reason are submitted — the report
// API receives no surrounding messages and no decrypted context beyond what
// the server already holds for that event.
Dialog {
    id: root
    objectName: "reportMessageDialog"
    modal: true
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(440, parent ? parent.width - AppTheme.spacing24 * 2 : 440)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property bool promptActive: app.moderation.reportPromptActive
    onPromptActiveChanged: {
        if (promptActive) {
            reasonField.text = ""
            open()
            Qt.callLater(function() { reasonField.forceActiveFocus() })
        } else if (opened) {
            close()
        }
    }
    onClosed: {
        reasonField.text = ""
        if (app.moderation.reportPromptActive)
            app.moderation.cancelReport()
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
            text: qsTr("Report message")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: 16
            font.weight: Font.Bold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("The report goes to the administrator of your "
                       + "homeserver, together with a reference to this "
                       + "one message. It is not sent to the message's "
                       + "author.")
            color: AppTheme.stormTextSecondary
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        AppTextField {
            id: reasonField
            objectName: "reportReasonField"
            Layout.fillWidth: true
            storm: true
            placeholderText: qsTr("Reason (optional)")
            Accessible.name: qsTr("Report reason")
            onAccepted: submitReportButton.clicked()
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                text: qsTr("Cancel")
                onClicked: app.moderation.cancelReport()
            }
            AppButton {
                id: submitReportButton
                objectName: "reportSubmitButton"
                storm: true
                kind: "danger"
                text: qsTr("Report")
                onClicked: app.moderation.submitReport(reasonField.text)
            }
        }
    }
}
