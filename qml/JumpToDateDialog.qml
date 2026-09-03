import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "Jump to date" — MSC3030 `timestamp_to_event`, stable since Matrix 1.6.
//
// The SERVER answers which event is closest to a timestamp. Nothing here
// scans a timeline and there is no client-side fallback: paginating backwards
// until the dates look right is an unbounded walk through a room's whole
// history to answer a question one request answers, and it would be slowest
// in exactly the rooms this feature is for.
//
// FORWARD from the start of the chosen day, which is what "jump to date"
// means: the FIRST message of that day, not the last one before it. A day
// with no messages therefore lands on the next message after it — the honest
// answer to "take me to here", rather than refusing to move.
//
// The dialog STAYS OPEN until the answer arrives. A homeserver that does not
// implement the endpoint is a real outcome and the user has to be told; a
// dialog that closes on click and then does nothing is the failure mode this
// whole surface exists to avoid.
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

    // Local midnight of the chosen day. LOCAL, not UTC: the user picks a date
    // off their own calendar, and a UTC midnight is somebody else's day for
    // most of the planet.
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
            // Round-tripped, so 2026-02-31 is refused rather than silently
            // becoming the 3rd of March.
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
            // The commonest real outcome, and now it says ONLY that. A server
            // with no MSC3030 answers 404 M_UNRECOGNIZED and classifies as
            // "unrecognized" below, so this no longer has to hedge — a hedge
            // carried on the common case is read as noise and stops carrying
            // anything on the rare one.
            return qsTr("Your homeserver could not find a message on or "
                        + "after that date.")
        case "unrecognized":
            // A homeserver too old for MSC3030 (stable since Matrix 1.6).
            // Nothing the user can do in the app, so the message says whose
            // limitation it is rather than offering a retry.
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
                        // 0 is "this build cannot ask", which is a different
                        // answer from the server saying no — and saying
                        // "your homeserver" about it would be a lie.
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
