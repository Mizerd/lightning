import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Forwarding a selection: N messages to M destinations. Unlike
// ForwardMessageDialog it stays open through the send and reports each pair,
// since N×M sends can partially fail.
Dialog {
    id: root
    objectName: "forwardSelectionDialog"

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    padding: AppTheme.spacing16

    // [{roomId, name, threadRootId}]: chosen destinations.
    property var targets: []
    property string filter: ""

    readonly property bool sending: app.forward.busy
    readonly property bool finished:
        app.forward.progressTotal > 0
        && app.forward.progressDone >= app.forward.progressTotal

    function openDialog() {
        targets = []
        filter = ""
        filterField.text = ""
        open()
    }

    function isChosen(roomId) {
        for (var i = 0; i < targets.length; ++i) {
            if (targets[i].roomId === roomId)
                return true
        }
        return false
    }
    function toggleTarget(roomId, name) {
        var next = []
        var removed = false
        for (var i = 0; i < targets.length; ++i) {
            if (targets[i].roomId === roomId) {
                removed = true
                continue
            }
            next.push(targets[i])
        }
        if (!removed)
            next.push({ roomId: roomId, name: name, threadRootId: "" })
        targets = next
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
            text: qsTr("Forward %n message(s)", "", app.forward.selectedCount)
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        // ── Mode ──
        // Including context is an explicit choice that says what it discloses:
        // the sender and source room name go to recipients who may not be in
        // that room.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: !root.sending && !root.finished
            RowLayout {
                spacing: AppTheme.spacing8
                AppButton {
                    text: qsTr("Just the message")
                    kind: app.forward.forwardMode === "content" ? "secondary"
                                                                : "ghost"
                    size: "sm"
                    onClicked: app.forward.setForwardMode("content")
                }
                AppButton {
                    text: qsTr("With original sender")
                    kind: app.forward.forwardMode === "context" ? "secondary"
                                                                : "ghost"
                    size: "sm"
                    onClicked: app.forward.setForwardMode("context")
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                // Our own text, but the contract scan conservatively requires
                // plain text everywhere.
                textFormat: Text.PlainText
                text: app.forward.forwardMode === "context"
                      ? qsTr("Each copy will name the original sender, this "
                             + "room and the time — visible to everyone in "
                             + "the rooms you pick.")
                      : qsTr("A clean copy, with nothing identifying where it "
                             + "came from.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        AppTextField {
            id: filterField
            Layout.fillWidth: true
            visible: !root.sending && !root.finished
            placeholderText: qsTr("Search rooms…")
            onTextChanged: root.filter = text
        }

        // ── Destinations ──
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 240
            visible: !root.sending && !root.finished
            clip: true
            model: app.allRooms
            ScrollBar.vertical: AppScrollBar {}
            SmoothWheelArea {}
            delegate: ItemDelegate {
                required property int index
                required property string roomId
                required property string name
                required property bool isSpace
                width: ListView.view.width
                height: visible ? 38 : 0
                // A Space is not a room and can't receive a message.
                visible: !isSpace
                         && (root.filter === ""
                             || name.toLowerCase().indexOf(
                                    root.filter.toLowerCase()) >= 0)
                onClicked: root.toggleTarget(roomId, name)
                contentItem: RowLayout {
                    spacing: AppTheme.spacing8
                    CheckBox {
                        checked: root.isChosen(roomId)
                        // The row owns the click; this is an indicator.
                        enabled: false
                        opacity: 1
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: name
                        color: AppTheme.textPrimary
                        elide: Label.ElideRight
                    }
                }
            }
        }

        // ── Progress and results ──
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: root.sending || root.finished
            Label {
                Layout.fillWidth: true
                text: root.sending
                      ? qsTr("Sending %1 of %2…").arg(app.forward.progressDone)
                            .arg(app.forward.progressTotal)
                      : (app.forward.failureCount === 0
                         ? qsTr("Sent %n copy(ies).", "",
                                app.forward.progressTotal)
                         : qsTr("%1 of %2 sent — %n failed.", "",
                                app.forward.failureCount)
                               .arg(app.forward.progressTotal
                                    - app.forward.failureCount)
                               .arg(app.forward.progressTotal))
                color: AppTheme.textPrimary
                wrapMode: Text.WordWrap
            }
            // Which pair failed, not just a count, so it can be acted on.
            Repeater {
                model: app.forward.failures
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: "• " + modelData.message
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: root.finished ? qsTr("Done") : qsTr("Cancel")
                kind: "ghost"
                onClicked: {
                    if (root.finished)
                        app.forward.cancelSelecting()
                    root.close()
                }
            }
            AppButton {
                visible: root.finished && app.forward.failureCount > 0
                text: qsTr("Retry failed")
                onClicked: app.forward.retryFailures()
            }
            AppButton {
                visible: !root.sending && !root.finished
                text: qsTr("Send")
                kind: "primary"
                enabled: root.targets.length > 0
                         && app.forward.selectedCount > 0
                // Not beginSelection(), which would reset the snapshots
                // captured at click time by toggleSelected().
                onClicked: app.forward.sendSelection(root.targets)
            }
        }
    }
}
