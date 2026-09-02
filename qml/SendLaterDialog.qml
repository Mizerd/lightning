import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.9 (phase 11): "Send later". Presets in the user's LOCAL time, a custom
// date/time, and the room's pending scheduled messages (edit text /
// reschedule / cancel / send now). The mode line is the honest part: a
// message the SERVER holds (MSC4140) goes out whether or not Lightning is
// running; one Lightning holds needs Lightning running and connected at
// that moment, and in an encrypted room it is not even written to disk.
Dialog {
    id: root
    objectName: "sendLaterDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property var scheduler: app.scheduledSends
    // The composed message snapshot to schedule; empty map = pending-only.
    property var message: ({})
    property string choice: "30m"
    // Editing an existing entry's text.
    property string editingId: ""

    function openFor(snapshot) {
        message = snapshot || ({})
        choice = "30m"
        editingId = ""
        var now = new Date()
        customDate.text = Qt.formatDate(now, "yyyy-MM-dd")
        customTime.text = Qt.formatTime(new Date(now.getTime() + 60 * 60 * 1000), "HH:mm")
        if (scheduler)
            scheduler.probeSupport()
        open()
    }
    readonly property bool hasMessage: !!message && !!message.body
                                       && message.body.length > 0
    readonly property string roomId: hasMessage ? message.roomId : app.currentRoomId

    function sendAtMs() {
        var now = new Date()
        switch (root.choice) {
        case "30m": return now.getTime() + 30 * 60 * 1000
        case "tonight": {
            var t = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 20, 0, 0, 0)
            if (t.getTime() <= now.getTime())
                t = new Date(t.getTime() + 24 * 60 * 60 * 1000)
            return t.getTime()
        }
        case "tomorrow": {
            var m = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1, 9, 0, 0, 0)
            return m.getTime()
        }
        case "custom": {
            var parsed = Date.fromLocaleString(Qt.locale(),
                                               customDate.text + " " + customTime.text,
                                               "yyyy-MM-dd HH:mm")
            if (isNaN(parsed.getTime()) || parsed.getTime() <= now.getTime())
                return -1
            return parsed.getTime()
        }
        }
        return -1
    }
    readonly property bool customInvalid: root.choice === "custom" && root.sendAtMs() < 0
    // Re-evaluated when the support probe answers: the invokable alone
    // carries no dependency, `serverScheduling` does.
    readonly property bool serverMode: {
        if (!scheduler || !hasMessage)
            return false
        var support = scheduler.serverScheduling
        return scheduler.wouldUseServer(message.roomId,
                                        message.threadRootId || "",
                                        message.replyToEventId || "")
    }
    readonly property bool encryptedRoom: scheduler && root.roomId !== ""
                                          && scheduler.roomIsEncrypted(root.roomId)
    // The room's pending entries, from the NOTIFYING `pending` property so
    // the list refreshes on every change (an invokable in a binding does
    // not re-run).
    readonly property var roomPending: {
        if (!scheduler)
            return []
        var all = scheduler.pending
        var out = []
        for (var i = 0; i < all.length; ++i)
            if (all[i].roomId === root.roomId)
                out.push(all[i])
        return out
    }
    function whenLabel(ms) {
        return Qt.formatDateTime(new Date(ms), "ddd d MMM " + app.settings.clockTimeFormat)
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
            text: root.hasMessage ? qsTr("Send later") : qsTr("Scheduled messages")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }

        // ---- scheduling a new message
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.hasMessage
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                text: root.message.body || ""
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            SegmentedControl {
                objectName: "sendLaterPresets"
                storm: true
                dense: true
                fitWidth: true
                Layout.fillWidth: true
                model: [
                    { value: "30m", label: qsTr("In 30 minutes") },
                    { value: "tonight", label: qsTr("Tonight") },
                    { value: "tomorrow", label: qsTr("Tomorrow morning") },
                    { value: "custom", label: qsTr("Pick date & time") }
                ]
                current: root.choice
                onActivated: (value) => root.choice = value
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.choice === "custom"
                spacing: AppTheme.spacing8
                AppTextField {
                    id: customDate
                    objectName: "sendLaterDate"
                    storm: true
                    Layout.fillWidth: true
                    placeholderText: qsTr("yyyy-mm-dd")
                    inputMask: "9999-99-99"
                }
                AppTextField {
                    id: customTime
                    objectName: "sendLaterTime"
                    storm: true
                    Layout.preferredWidth: 90
                    placeholderText: qsTr("hh:mm")
                    inputMask: "99:99"
                }
            }
            Label {
                visible: root.customInvalid
                text: qsTr("Pick a date and time in the future.")
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
            Label {
                Layout.fillWidth: true
                visible: !root.customInvalid
                text: qsTr("Sends %1 (your local time).").arg(root.whenLabel(root.sendAtMs()))
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
            // THE honest line. Never "will definitely send while your
            // computer is off" unless the server holds it.
            Label {
                objectName: "sendLaterModeLine"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: root.serverMode ? AppTheme.stormTextMuted : AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
                text: root.serverMode
                      ? qsTr("Your homeserver will hold this message and send it at that time, even if Lightning is closed.")
                      : root.encryptedRoom
                        ? qsTr("Lightning will keep this message and send it at that time. Lightning must be running and connected then — and because this room is encrypted, the message is kept only in memory: closing Lightning discards it.")
                        : qsTr("Lightning will keep this message and send it at that time. Lightning must be running and connected then.")
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
                    objectName: "sendLaterConfirm"
                    text: qsTr("Schedule")
                    kind: "primary"
                    enabled: !root.customInvalid
                    onClicked: {
                        var at = root.sendAtMs()
                        if (at < 0)
                            return
                        var id = root.scheduler.schedule(root.message, at)
                        if (id.length > 0) {
                            app.composer.clear()
                            root.close()
                        }
                    }
                }
            }
        }

        // ---- pending list for this room
        Label {
            visible: pendingRepeater.count > 0
            text: qsTr("Pending in this room")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            visible: !root.hasMessage && pendingRepeater.count === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Nothing is scheduled for this room.")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
        }
        Repeater {
            id: pendingRepeater
            model: root.roomPending
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                radius: AppTheme.radiusTile
                color: AppTheme.stormInset
                border.width: 1
                border.color: AppTheme.stormBorder
                implicitHeight: pendingCol.implicitHeight + AppTheme.spacing12 * 2
                ColumnLayout {
                    id: pendingCol
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing6
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            text: root.whenLabel(modelData.sendAtMs)
                            color: AppTheme.stormText
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        StatusChip {
                            storm: true
                            label: modelData.mode === "server" ? qsTr("Server") : qsTr("Lightning")
                            tone: modelData.mode === "server" ? "success" : "neutral"
                        }
                        StatusChip {
                            storm: true
                            visible: modelData.status === "failed"
                            label: qsTr("Failed")
                            tone: "danger"
                        }
                        StatusChip {
                            storm: true
                            visible: modelData.volatile === true
                            label: qsTr("Memory only")
                            tone: "neutral"
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Label {
                        visible: root.editingId !== modelData.id
                        Layout.fillWidth: true
                        text: modelData.body
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: AppTheme.textBody
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    AppTextField {
                        id: editField
                        visible: root.editingId === modelData.id
                        storm: true
                        Layout.fillWidth: true
                        text: modelData.body
                        onAccepted: {
                            if (text.trim().length > 0)
                                root.scheduler.updateText(modelData.id, text, "")
                            root.editingId = ""
                        }
                    }
                    Label {
                        visible: (modelData.error || "").length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData.error || ""
                        color: AppTheme.stormDanger
                        font.pixelSize: AppTheme.textMeta
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        AppButton {
                            storm: true
                            kind: "secondary"
                            size: "sm"
                            text: qsTr("Send now")
                            enabled: !modelData.busy
                            onClicked: root.scheduler.sendNow(modelData.id)
                        }
                        AppButton {
                            storm: true
                            kind: "ghost"
                            size: "sm"
                            text: root.editingId === modelData.id ? qsTr("Done") : qsTr("Edit text")
                            enabled: !modelData.busy
                            onClicked: {
                                if (root.editingId === modelData.id) {
                                    if (editField.text.trim().length > 0)
                                        root.scheduler.updateText(modelData.id,
                                                                  editField.text, "")
                                    root.editingId = ""
                                } else {
                                    root.editingId = modelData.id
                                }
                            }
                        }
                        AppButton {
                            storm: true
                            kind: "ghost"
                            size: "sm"
                            text: qsTr("+1 hour")
                            enabled: !modelData.busy
                            onClicked: root.scheduler.reschedule(
                                           modelData.id,
                                           Math.max(modelData.sendAtMs, Date.now())
                                               + 60 * 60 * 1000)
                        }
                        AppButton {
                            storm: true
                            kind: "danger"
                            size: "sm"
                            text: qsTr("Cancel")
                            enabled: !modelData.busy
                            onClicked: root.scheduler.cancel(modelData.id)
                        }
                    }
                }
            }
        }
        RowLayout {
            visible: !root.hasMessage
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
