import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A message's edit history: the original and every m.replace the SDK holds
// or can fetch, in order, with the current one marked. Nothing is invented:
// undecryptable and redacted revisions say so, and a fetch failure is stated
// rather than shown as a complete-looking list.
//
// Revisions are plaintext in an encrypted room: held in this dialog's
// property while open and dropped on close.
Dialog {
    id: root
    objectName: "editHistoryDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    height: Math.min(520, parent ? parent.height - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    // The timeline model this dialog asks (room or thread panel).
    property var timelineModel: null
    property string eventId: ""
    property var revisions: []
    property bool loading: false
    property bool failed: false
    // The server was unreachable (cache answered) or had more revisions than
    // one page. Shown, since the missing rows are the oldest and a truncated
    // list would look complete.
    property bool partial: false

    function openFor(model, id) {
        timelineModel = model
        eventId = id
        revisions = []
        failed = false
        loading = true
        open()
        if (timelineModel && timelineModel.requestEditHistory)
            timelineModel.requestEditHistory(id)
    }
    onClosed: {
        revisions = []
        eventId = ""
    }
    Connections {
        target: root.timelineModel
        function onEditHistoryReceived(id, ok, partial, rows) {
            if (id !== root.eventId)
                return
            root.loading = false
            root.failed = !ok
            root.partial = ok && partial
            root.revisions = ok ? rows : []
        }
    }
    function timeLabel(ts) {
        if (!ts || typeof ts.getTime !== "function" || isNaN(ts.getTime()))
            return ""
        return Qt.formatDateTime(ts, "d MMM yyyy " + app.settings.clockTimeFormat)
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
            text: qsTr("Edit history")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        Label {
            visible: root.loading
            text: qsTr("Loading revisions…")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
        }
        Label {
            visible: root.failed
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textBody
            text: qsTr("The revisions could not be loaded. Only what this "
                       + "device has synced can be shown, and nothing was.")
        }
        Label {
            objectName: "editHistoryPartialNotice"
            visible: root.partial
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("This may be incomplete. Older revisions could not be "
                       + "loaded, so the earliest versions of this message "
                       + "may be missing.")
        }
        ListView {
            id: list
            objectName: "editHistoryList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing8
            model: root.revisions
            ScrollBar.vertical: AppScrollBar { thin: true }
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                radius: AppTheme.radiusTile
                color: modelData.isLatest ? AppTheme.stormInset : "transparent"
                border.width: 1
                border.color: modelData.isLatest ? AppTheme.stormBorderStrong
                                                 : AppTheme.stormBorder
                implicitHeight: revisionCol.implicitHeight + AppTheme.spacing12 * 2
                ColumnLayout {
                    id: revisionCol
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            text: modelData.isOriginal ? qsTr("Original")
                                                       : root.timeLabel(modelData.timestamp)
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Label {
                            visible: modelData.isOriginal
                            text: root.timeLabel(modelData.timestamp)
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Item { Layout.fillWidth: true }
                        StatusChip {
                            storm: true
                            visible: modelData.isLatest
                            label: qsTr("Current")
                            tone: "accent"
                        }
                        StatusChip {
                            storm: true
                            visible: modelData.redacted
                            label: qsTr("Deleted")
                            tone: "neutral"
                        }
                    }
                    // Rendered through the timeline's sanitizer (formatted) or
                    // as plain text; never raw HTML.
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        textFormat: modelData.formattedBody
                                    && modelData.formattedBody.length > 0
                                    ? Text.RichText : Text.PlainText
                        text: modelData.undecryptable
                              ? qsTr("This revision could not be decrypted.")
                              : modelData.redacted
                                ? qsTr("This revision was deleted.")
                                : (modelData.formattedBody
                                   && modelData.formattedBody.length > 0
                                   ? root.timelineModel.sanitizeHtml(modelData.formattedBody)
                                   : modelData.body)
                        font.italic: modelData.undecryptable || modelData.redacted
                    }
                }
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
