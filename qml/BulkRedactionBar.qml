import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Used by both room and thread timelines. The controller owns the selected
// model, so thread replies are redacted through their thread timeline.
Rectangle {
    id: root
    required property var timelineModel
    objectName: "bulkRedactionBar"
    readonly property bool ours:
        app.bulkRedaction.timelineId === root.timelineModel.roomId
    visible: ours && (app.bulkRedaction.selecting
                      || app.bulkRedaction.running
                      || app.bulkRedaction.total > 0)
    implicitHeight: visible ? body.implicitHeight + AppTheme.spacing8 * 2 : 0
    color: AppTheme.surface
    border.color: AppTheme.borderStrong
    border.width: 1

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.margins: AppTheme.spacing8
        spacing: AppTheme.spacing8
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: app.bulkRedaction.running
                  ? qsTr("Deleting %1 of %2 messages…")
                        .arg(app.bulkRedaction.completed)
                        .arg(app.bulkRedaction.total)
                  : app.bulkRedaction.selecting
                    ? qsTr("%n message(s) selected", "",
                           app.bulkRedaction.selectedCount)
                    : app.bulkRedaction.cancelled
                      ? qsTr("Stopped after %1 of %2 messages; %3 failed.")
                            .arg(app.bulkRedaction.completed)
                            .arg(app.bulkRedaction.total)
                            .arg(app.bulkRedaction.failed)
                      : qsTr("Deleted %1 of %2 messages; %3 failed.")
                            .arg(app.bulkRedaction.completed
                                 - app.bulkRedaction.failed)
                            .arg(app.bulkRedaction.total)
                            .arg(app.bulkRedaction.failed)
            color: AppTheme.textPrimary
        }
        AppButton {
            objectName: "bulkRedactionCancel"
            text: app.bulkRedaction.selecting || app.bulkRedaction.running
                  ? qsTr("Cancel") : qsTr("Dismiss")
            kind: "ghost"
            size: "sm"
            onClicked: app.bulkRedaction.cancel()
        }
        AppButton {
            objectName: "bulkRedactionDelete"
            visible: app.bulkRedaction.selecting
            enabled: app.bulkRedaction.selectedCount > 0
            text: qsTr("Delete %n messages…", "",
                       app.bulkRedaction.selectedCount)
            kind: "primary"
            size: "sm"
            onClicked: confirmation.open()
        }
    }
    Label {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.top
        visible: app.bulkRedaction.error.length > 0
        text: app.bulkRedaction.error
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: AppTheme.danger
        font.pixelSize: AppTheme.textMeta
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.visible && (app.bulkRedaction.selecting
                                  || app.bulkRedaction.running)
        onActivated: app.bulkRedaction.cancel()
    }
    AppDialog {
        id: confirmation
        objectName: "bulkRedactionConfirm"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(420, parent ? parent.width - 64 : 420)
        modal: true
        title: qsTr("Delete %n messages?", "", app.bulkRedaction.selectedCount)
        standardButtons: Dialog.NoButton
        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("These messages will be removed for everyone in this room. This cannot be undone.")
                color: AppTheme.textPrimary
            }
            AppTextField {
                id: reason
                Layout.fillWidth: true
                placeholderText: qsTr("Reason (optional)")
                Accessible.name: qsTr("Reason for deleting messages")
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Cancel")
                    kind: "ghost"
                    onClicked: confirmation.close()
                }
                AppButton {
                    objectName: "bulkRedactionConfirmDelete"
                    text: qsTr("Delete %n messages", "",
                               app.bulkRedaction.selectedCount)
                    kind: "primary"
                    enabled: app.bulkRedaction.selectedCount > 0
                    onClicked: {
                        app.bulkRedaction.start(reason.text.trim())
                        reason.text = ""
                        confirmation.close()
                    }
                }
            }
        }
    }
}
