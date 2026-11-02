import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

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
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Flickable {
                id: sourceFlick
                objectName: "eventSourceFlick"
                anchors.fill: parent
                clip: true
                contentWidth: width
                contentHeight: sourceText.paintedHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar { thin: true }
                TextEdit {
                    id: sourceText
                    width: parent.width
                    wrapMode: TextEdit.WrapAnywhere
                    objectName: "eventSourceText"
                    readOnly: true
                    activeFocusOnTab: true
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Event source")
                    onCursorRectangleChanged: {
                        var r = cursorRectangle
                        if (r.y < sourceFlick.contentY)
                            sourceFlick.contentY = r.y
                        else if (r.y + r.height
                                 > sourceFlick.contentY + sourceFlick.height)
                            sourceFlick.contentY =
                                r.y + r.height - sourceFlick.height
                    }
                    selectByMouse: true
                    selectByKeyboard: true
                    textFormat: TextEdit.PlainText
                    color: AppTheme.stormText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.textMeta
                    text: root.json
                }
            }
            MiddleClickScroller {
                objectName: "eventSourceMiddleClickScroller"
                anchors.fill: parent
                view: sourceFlick
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
