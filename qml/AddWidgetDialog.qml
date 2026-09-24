import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Adding a widget to a room (see docs/widgets.md). Writes the same
// `im.vector.modular.widgets` state event every client lists; Lightning
// opens widgets in the browser and never embeds them. It offers kinds of
// widget at addresses the user already has, not a catalogue: an
// integration manager would itself have to be embedded.
Dialog {
    id: root
    objectName: "addWidgetDialog"

    // `app` is absent when a suite loads the module without the application
    // context; null keeps the bindings below quiet.
    readonly property var widgets: typeof app !== "undefined" ? app.widgets : null

    // MSC1236 types every widget-listing client understands: the event type, a
    // label and an address hint, ordered by how often they're added.
    readonly property var kinds: [
        { type: "m.custom",   label: qsTr("Web page"),
          hint: qsTr("Any https page — a dashboard, a document, a board.") },
        { type: "m.etherpad", label: qsTr("Etherpad pad"),
          hint: qsTr("The pad's own address, e.g. https://pad.example/p/notes") },
        { type: "m.jitsi",    label: qsTr("Jitsi meeting"),
          hint: qsTr("The meeting address, e.g. https://meet.example/room-name. "
                     + "Lightning has its own calls; this is for a room that "
                     + "already uses Jitsi.") },
        { type: "m.video",    label: qsTr("Video"),
          hint: qsTr("A video page address.") },
        { type: "m.image",    label: qsTr("Image"),
          hint: qsTr("An image address (https).") },
        { type: "m.grafana",  label: qsTr("Grafana dashboard"),
          hint: qsTr("A Grafana dashboard's share address.") },
    ]

    property string errorText: ""
    readonly property bool urlOk:
        widgets ? widgets.urlIsAcceptable(urlField.text) : false

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    padding: AppTheme.spacing16

    function openDialog() {
        kindBox.currentIndex = 0
        nameField.text = ""
        urlField.text = ""
        root.errorText = ""
        open()
        urlField.forceActiveFocus()
    }

    Connections {
        target: root.widgets ? root.widgets : null
        function onWriteFinished(ok, category) {
            if (!root.opened)
                return
            if (ok) {
                root.close()
                return
            }
            root.errorText = category === "forbidden"
                ? qsTr("You do not have permission to add widgets here.")
                : qsTr("The widget could not be added.")
        }
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
            text: qsTr("Add a widget")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        Label {
            text: qsTr("Kind")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppComboBox {
            id: kindBox
            objectName: "addWidgetKind"
            Layout.fillWidth: true
            model: root.kinds.map(function (k) { return k.label })
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.kinds[kindBox.currentIndex]
                  ? root.kinds[kindBox.currentIndex].hint : ""
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }

        Label {
            text: qsTr("Address")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppTextField {
            id: urlField
            objectName: "addWidgetUrl"
            Layout.fillWidth: true
            placeholderText: qsTr("https://…")
            inputMethodHints: Qt.ImhUrlCharactersOnly
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: urlField.text.trim().length > 0 && !root.urlOk
            text: qsTr("The address must start with https:// and carry no "
                       + "username or password.")
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
        }

        Label {
            text: qsTr("Name (optional)")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppTextField {
            id: nameField
            objectName: "addWidgetName"
            Layout.fillWidth: true
            placeholderText: root.kinds[kindBox.currentIndex]
                             ? root.kinds[kindBox.currentIndex].label : ""
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            // It's public to the room and opens in a browser, as the Widgets
            // tab says about every widget.
            text: qsTr("Everyone in the room will see this widget. Lightning "
                       + "opens widgets in your browser, so the page never "
                       + "reaches your account, keys or messages.")
        }

        Label {
            objectName: "addWidgetError"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: root.errorText.length > 0
            text: root.errorText
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "addWidgetSubmit"
                text: qsTr("Add")
                kind: "primary"
                enabled: root.urlOk && root.widgets && root.widgets.canManage
                         && !root.widgets.writing
                onClicked: {
                    root.errorText = ""
                    // An empty name would list the widget by its type
                    // ("m.custom"), so use the kind's label, which the
                    // placeholder showed.
                    const kind = root.kinds[kindBox.currentIndex]
                    const name = nameField.text.trim().length > 0
                               ? nameField.text.trim() : kind.label
                    root.widgets.addWidget(kind.type, name, urlField.text)
                }
            }
        }
    }
}
