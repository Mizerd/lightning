import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.9 (phase 7): "View event source" for technical users. The JSON is the
// event as the SDK holds it (decrypted, in an encrypted room), shown in a
// monospaced, scrollable, selectable viewer with Copy JSON / Copy event ID.
// The encryption block beside it describes the envelope — algorithm,
// sender key, sender device, verification — with public identifiers only:
// no key material reaches THAT BLOCK (the FFI does not carry it).
//
// The JSON body is a different matter and the distinction was previously
// stated too broadly here. It is the event verbatim, so for an attachment in
// an encrypted room it includes `content.file.key`, the per-attachment AES
// key — the user's own key, for their own room, exactly as Element's own
// view-source shows it. Copy JSON therefore puts that on the shared
// clipboard. That is the deliberate cost of a verbatim source view, but it
// is not "no key material".
//
// Plaintext in an encrypted room: held in this dialog's property while it
// is open and dropped on close; never logged.
Dialog {
    id: root
    objectName: "eventSourceDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(680, parent ? parent.width - AppTheme.spacing24 * 2 : 680)
    height: Math.min(600, parent ? parent.height - AppTheme.spacing24 * 2 : 600)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property var timelineModel: null
    property string eventId: ""
    property string json: ""
    property var encryption: ({})
    property bool loading: false
    property bool failed: false

    function openFor(model, id) {
        timelineModel = model
        eventId = id
        json = ""
        encryption = ({})
        failed = false
        loading = true
        open()
        if (timelineModel && timelineModel.requestEventSource)
            timelineModel.requestEventSource(id)
    }
    onClosed: {
        json = ""
        encryption = ({})
        eventId = ""
    }
    // Same clipboard route as MessageDelegate.copyToClipboard: a hidden
    // TextEdit, because QML has no clipboard API of its own.
    TextEdit {
        id: clipboardHelper
        visible: false
        width: 1
        height: 1
    }
    function copyText(value) {
        if (!value || value.length === 0)
            return
        clipboardHelper.text = value
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.text = ""
    }
    Connections {
        target: root.timelineModel
        function onEventSourceReceived(id, ok, text, enc) {
            if (id !== root.eventId)
                return
            root.loading = false
            root.failed = !ok
            root.json = ok ? text : ""
            root.encryption = ok ? enc : ({})
        }
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Event source")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "eventSourceCopyId"
                kind: "ghost"
                size: "sm"
                text: qsTr("Copy event ID")
                onClicked: root.copyText(root.eventId)
            }
            AppButton {
                objectName: "eventSourceCopyJson"
                kind: "secondary"
                size: "sm"
                enabled: root.json.length > 0
                text: qsTr("Copy JSON")
                onClicked: root.copyText(root.json)
            }
        }
        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: root.encryption && root.encryption.encrypted === true
            wrapMode: Text.WrapAnywhere
            color: AppTheme.stormTextMuted
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Encrypted · %1 · sender %2 · device %3 · %4")
                      .arg(root.encryption.algorithm || "")
                      .arg(root.encryption.sender || "")
                      .arg(root.encryption.senderDevice || "")
                      .arg(root.encryption.verification || "")
        }
        Label {
            visible: root.loading
            text: qsTr("Loading…")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
        }
        Label {
            visible: root.failed
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textBody
            text: qsTr("The event could not be loaded.")
        }
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: sourceText.paintedWidth
            contentHeight: sourceText.paintedHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { thin: true }
            ScrollBar.horizontal: AppScrollBar { thin: true }
            TextEdit {
                id: sourceText
                objectName: "eventSourceText"
                readOnly: true
                selectByMouse: true
                selectByKeyboard: true
                textFormat: TextEdit.PlainText
                color: AppTheme.stormText
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.textMeta
                text: root.json
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Close")
                kind: "primary"
                onClicked: root.close()
            }
        }
    }
}
