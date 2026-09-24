import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Consent step before opening a widget in the browser. A widget URL is room
// state any permitted member can write, and opening it hands its origin
// whatever the URL templates (display name, device id, room id). This says
// exactly what this widget receives, derived from its own URL, so the notice
// never overstates. The resolved address is shown in full, in mono, since the
// origin is what a person can judge.
Dialog {
    id: root
    objectName: "widgetOpenSheet"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    /// The row from WidgetController.rowAt() and its index; the index opens it,
    /// so QML never names an address.
    property var widget: ({})
    property int widgetRow: -1
    readonly property string widgetName: widget && widget.name ? widget.name : ""
    readonly property string widgetUrl: widget && widget.url ? widget.url : ""
    readonly property string widgetCreator:
        widget && widget.creator ? widget.creator : ""
    readonly property var discloses:
        widget && widget.discloses ? widget.discloses : []

    function openFor(index, row) {
        widgetRow = index
        widget = row || ({})
        open()
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Open this widget?")
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // Remote text: Label's AutoText would render tags, and an <img>
            // would beacon as soon as the sheet opened.
            textFormat: Text.PlainText
            text: qsTr("Lightning opens widgets in your browser rather than "
                       + "inside the app, so the page cannot reach your "
                       + "account, your keys, or your messages.")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: addressCol.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusMd
            color: AppTheme.stormInset
            border.width: 1
            border.color: AppTheme.stormBorderStrong
            ColumnLayout {
                id: addressCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing4
                Label {
                    text: root.widgetName
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "widgetSheetUrl"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    // Mono for an address, where lookalike characters matter.
                    text: root.widgetUrl
                    color: AppTheme.stormTextSecondary
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.textMeta
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.widgetCreator.length > 0
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: qsTr("Added by %1").arg(root.widgetCreator)
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }

        Label {
            text: qsTr("The site will receive:")
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }
        Repeater {
            model: root.discloses
            Label {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing8
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: "•  " + app.widgets.disclosureText(modelData)
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "widgetSheetOpen"
                text: qsTr("Open in browser")
                kind: "primary"
                enabled: root.widgetUrl.length > 0
                onClicked: {
                    // By row, never by address: no QML path can hand the
                    // desktop a URL outside the model's validated list. The
                    // controller re-checks it at the single desktop exit.
                    app.widgets.openWidget(root.widgetRow)
                    root.close()
                }
            }
        }
    }
}
