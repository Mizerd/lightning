import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "Jump to date": MSC3030 `timestamp_to_event` (stable since Matrix 1.6).
//
// The server answers which event is closest; there is no client-side
// fallback, since paginating back through history is unbounded and slowest in
// the rooms this is for. Searches forward from the start of the chosen day,
// so it lands on that day's first message, or the next message after an
// empty day.
//
// The dialog stays open until the answer arrives, so an unsupported
// homeserver can be reported rather than the click silently doing nothing.
Dialog {
    id: root
    objectName: "jumpToDateDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(460, parent ? parent.width - AppTheme.spacing24 * 2 : 460)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    /// "" | "asking" | the sanitized failure category.
    property string status: ""
    property string choice: "today"
    property real pendingOp: 0

    function openDialog() {
        status = ""
        choice = "today"
        pendingOp = 0
        var now = new Date()
        customDate.text = Qt.formatDate(now, "yyyy-MM-dd")
        open()
    }

    // Local midnight of the chosen day: the user picks from their own
    // calendar.
    function chosenMs() {
        var now = new Date()
        switch (root.choice) {
        case "today":
            return new Date(now.getFullYear(), now.getMonth(),
                            now.getDate()).getTime()
        case "week":
            return new Date(now.getFullYear(), now.getMonth(),
                            now.getDate() - 7).getTime()
        case "month":
            return new Date(now.getFullYear(), now.getMonth() - 1,
                            now.getDate()).getTime()
        case "custom": {
            var parts = customDate.text.split("-")
            if (parts.length !== 3)
                return -1
            var y = parseInt(parts[0], 10)
            var m = parseInt(parts[1], 10)
            var d = parseInt(parts[2], 10)
            if (!(y >= 1970 && m >= 1 && m <= 12 && d >= 1 && d <= 31))
                return -1
            var picked = new Date(y, m - 1, d)
            // Round-tripped, so 2026-02-31 is refused rather than becoming
            // 3 March.
            if (picked.getFullYear() !== y || picked.getMonth() !== m - 1
                    || picked.getDate() !== d)
                return -1
            if (picked.getTime() > now.getTime())
                return -1
            return picked.getTime()
        }
        }
        return -1
    }
    readonly property bool customInvalid: root.choice === "custom"
                                          && root.chosenMs() < 0
    readonly property bool asking: root.status === "asking"

    function failureText(category) {
        switch (category) {
        case "not_found":
            // The common outcome. A server without MSC3030 is classified as
            // "unrecognized" below, so this needn't hedge.
            return qsTr("Your homeserver could not find a message on or "
                        + "after that date.")
        case "unrecognized":
            // Homeserver too old for MSC3030: nothing to retry in the app, so
            // say whose limitation it is.
            return qsTr("Your homeserver does not support jumping to a date. "
                        + "That needs a newer homeserver; searching this "
                        + "room still works.")
        case "forbidden":
            return qsTr("Your homeserver refused the request for this room.")
        case "rate_limited":
            return qsTr("Your homeserver is rate limiting requests. Try "
                        + "again in a moment.")
        case "stale":
            return qsTr("The room changed while the answer was on its way.")
        case "backend":
            return qsTr("This build cannot ask the server for a date.")
        default:
            return qsTr("Could not reach your homeserver.")
        }
    }

    Connections {
        target: app
        function onJumpToDateFinished(opId, ok, category) {
            if (opId !== root.pendingOp)
                return
            root.pendingOp = 0
            if (ok) {
                root.status = ""
                root.close()
                return
            }
            root.status = category.length > 0 ? category : "network"
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Jump to date")
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Goes to the first message on or after the date you "
                       + "pick. Your homeserver answers this, so it works "
                       + "through history that is not loaded yet.")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        SegmentedControl {
            objectName: "jumpToDatePresets"
            storm: true
            Layout.fillWidth: false
            model: [
                { value: "today", label: qsTr("Today") },
                { value: "week", label: qsTr("A week ago") },
                { value: "month", label: qsTr("A month ago") },
                { value: "custom", label: qsTr("Pick a date") }
            ]
            current: root.choice
            onActivated: (value) => {
                root.choice = value
                root.status = ""
            }
        }
        AppTextField {
            id: customDate
            objectName: "jumpToDateField"
            visible: root.choice === "custom"
            storm: true
            Layout.fillWidth: true
            placeholderText: qsTr("yyyy-mm-dd")
            inputMask: "9999-99-99"
            onTextChanged: root.status = ""
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.customInvalid
            text: qsTr("Pick a real date that is not in the future.")
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            objectName: "jumpToDateStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.status.length > 0 && !root.asking
            text: root.failureText(root.status)
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }
        RowLayout {
            Layout.fillWidth: true
            AppBusyIndicator {
                visible: root.asking
                implicitWidth: 18
                implicitHeight: 18
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "jumpToDateConfirm"
                text: qsTr("Jump")
                kind: "primary"
                enabled: !root.customInvalid && !root.asking
                onClicked: {
                    var at = root.chosenMs()
                    if (at < 0)
                        return
                    var op = app.jumpToDate(at)
                    if (op === 0) {
                        // 0 means this build can't ask, which isn't the server
                        // saying no.
                        root.status = "backend"
                        return
                    }
                    root.pendingOp = op
                    root.status = "asking"
                }
            }
        }
    }
}
