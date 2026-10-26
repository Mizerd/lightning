import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Asks before an image from an ENCRYPTED conversation goes into the personal
// pack. A pack holds plain Matrix media, so this is a new, unencrypted upload
// that the server can see, not a copy of the private message. Nothing is
// fetched or uploaded until Add is pressed; Cancel, Escape or closing does
// nothing at all (PACK-PRO-041, PACK-UX-015/016).
Dialog {
    id: root
    objectName: "privateImageRepublishDialog"

    property string mediaKey: ""

    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent ? parent.width - AppTheme.spacing24 * 2 : 460)
    padding: AppTheme.spacing16
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.NoButton
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }

    function ask(key) {
        root.mediaKey = key
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
            text: qsTr("Add this private image to your stickers?")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: qsTr("This image is from an encrypted conversation. Sticker "
                       + "packs hold regular, unencrypted media, so Lightning "
                       + "will upload a new, unencrypted copy to your server and "
                       + "add it to your own pack. Your server can see it, and "
                       + "so can anyone you share the pack with. It is added as "
                       + "\"sticker\"; rename it in your pack if you like.")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "privateImageRepublishCancel"
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "privateImageRepublishConfirm"
                text: qsTr("Upload unencrypted copy")
                kind: "primary"
                enabled: root.mediaKey.length > 0 && app.stickers.available
                onClicked: {
                    app.stickers.republishPrivateImage(root.mediaKey)
                    root.close()
                }
            }
        }
    }
}
