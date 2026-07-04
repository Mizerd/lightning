import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.surface
    implicitHeight: composerCol.implicitHeight + AppTheme.spacingS

    FileDialog {
        id: pickImageDialog
        title: qsTr("Send image")
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"),
                       qsTr("All files (*)") ]
        onAccepted: app.media.sendPickedImage(app.currentRoomId, selectedFile)
    }
    FileDialog {
        id: pickFileDialog
        title: qsTr("Send file")
        onAccepted: app.media.sendPickedFile(app.currentRoomId, selectedFile)
    }
    Menu {
        id: attachMenu
        MenuItem {
            text: qsTr("Send image…")
            onTriggered: pickImageDialog.open()
        }
        MenuItem {
            text: qsTr("Send file…")
            onTriggered: pickFileDialog.open()
        }
    }

    ColumnLayout {
        id: composerCol
        anchors.fill: parent
        anchors.margins: AppTheme.spacingXS
        spacing: 2

        // Reply / Edit / Thread banner
        Rectangle {
            id: contextBar
            visible: app.composer.isReplying || app.composer.isEditing || app.composer.inThread
            Layout.fillWidth: true
            implicitHeight: contextRow.implicitHeight + 6
            color: Qt.rgba(0, 0, 0, 0.06)
            radius: 4
            RowLayout {
                id: contextRow
                anchors.fill: parent
                anchors.margins: 4
                spacing: 6
                Label {
                    text: {
                        if (app.composer.isEditing)
                            return qsTr("Editing message")
                        if (app.composer.inThread)
                            return qsTr("Replying in thread: %1").arg(app.composer.threadPreview || "")
                        return qsTr("Replying to %1: %2")
                                    .arg(app.composer.replyingToSender || qsTr("someone"))
                                    .arg(app.composer.replyingToPreview || "")
                    }
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                ToolButton {
                    text: "✕"
                    onClicked: app.composer.cancelReplyOrEdit()
                    ToolTip.text: qsTr("Cancel")
                    ToolTip.visible: hovered
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingS

            ToolButton {
                text: "＋"
                font.pixelSize: 18
                enabled: app.currentRoomId !== ""
                onClicked: attachMenu.popup()
                ToolTip.text: qsTr("Attach")
                ToolTip.visible: hovered
                ToolTip.delay: 500
            }

            TextArea {
                id: input
                Layout.fillWidth: true
                placeholderText: app.currentRoomId === ""
                                 ? qsTr("Select a room to start typing")
                                 : (app.composer.isEditing ? qsTr("Edit message…")
                                                           : qsTr("Write a message…"))
                wrapMode: TextArea.Wrap
                enabled: app.currentRoomId !== ""
                text: app.composer.text
                onTextChanged: if (app.composer.text !== text) app.composer.text = text
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false
                        return
                    }
                    event.accepted = true
                    app.composer.send()
                }
                background: Rectangle {
                    color: AppTheme.background
                    border.color: AppTheme.border
                    radius: AppTheme.radius
                }
            }

            Button {
                text: app.composer.isEditing ? qsTr("Save") : qsTr("Send")
                highlighted: true
                enabled: app.composer.canSend
                onClicked: app.composer.send()
            }
        }
    }

    Connections {
        target: app.composer
        function onTextChanged() {
            if (input.text !== app.composer.text) input.text = app.composer.text
        }
    }
}
