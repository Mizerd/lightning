import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Dialog {
    id: root
    objectName: "exportRoomDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    anchors.centerIn: parent
    padding: AppTheme.spacing16
    height: Math.min(implicitHeight, parent ? parent.height - AppTheme.spacing24 * 2
                                              : implicitHeight)

    property string format: "html"
    property string rangeMode: "loaded"
    property bool includeAttachments: true
    property bool includeEncryptedText: false
    property int messageCount: 0
    property bool encrypted: false
    property string roomName: ""
    property string failure: ""
    property string success: ""
    property string inputProblemShown: ""

    function clearInputProblem() {
        if (inputProblemShown.length > 0 && failure === inputProblemShown)
            failure = ""
        inputProblemShown = ""
    }

    function isRealDate(text) {
        var m = /^([0-9]{4})-([0-9]{2})-([0-9]{2})$/.exec(text)
        if (!m)
            return false
        var y = parseInt(m[1], 10), mo = parseInt(m[2], 10), d = parseInt(m[3], 10)
        var date = new Date(Date.UTC(y, mo - 1, d))
        return date.getUTCFullYear() === y && date.getUTCMonth() === mo - 1
               && date.getUTCDate() === d
    }

    readonly property var exporter: app ? app.roomExport : null
    readonly property bool busy: !!exporter && exporter.busy
    readonly property bool historyAvailable: !!exporter
                                             && exporter.historyAvailable
    readonly property bool isHtml: format === "html"

    readonly property var rangeValues: historyAvailable
                                       ? ["loaded", "latest", "all", "dates"]
                                       : ["loaded"]
    readonly property var rangeLabels: historyAvailable
        ? [qsTr("Loaded messages"), qsTr("Latest messages"),
           qsTr("From the beginning"), qsTr("Between dates")]
        : [qsTr("Loaded messages")]

    function openDialog() {
        format = "html"
        rangeMode = "loaded"
        includeAttachments = true
        includeEncryptedText = false
        failure = ""
        success = ""
        countField.text = "100"
        fromField.text = ""
        toField.text = ""
        sizeField.text = "100"
        rangeBox.currentIndex = 0
        if (exporter)
            exporter.clearResult()
        messageCount = app.exportableMessageCount()
        var info = app.roomList.findRoom(app.currentRoomId)
        roomName = info && info.name ? info.name : app.currentRoomId
        // Unknown counts as encrypted, matching the C++ side.
        encrypted = !!info
                    && (info.encryptionKnown === false || info.encrypted === true)
        open()
    }

    function inputProblem() {
        if (rangeMode === "latest") {
            var n = parseInt(countField.text, 10)
            if (isNaN(n) || n < 1 || n > 100000)
                return qsTr("Enter a number of messages from 1 to 100,000.")
        }
        if (rangeMode === "dates") {
            var from = fromField.text.trim()
            var to = toField.text.trim()
            if ((from.length === 0 && to.length === 0)
                    || (from.length > 0 && !isRealDate(from))
                    || (to.length > 0 && !isRealDate(to))
                    || (from.length > 0 && to.length > 0 && from > to))
                return qsTr("Use real dates in YYYY-MM-DD format, with From "
                            + "not after To.")
        }
        if (isHtml && includeAttachments) {
            var mb = parseInt(sizeField.text, 10)
            if (isNaN(mb) || mb < 1 || mb > 2000)
                return qsTr("Enter a size limit from 1 to 2000 MB.")
        }
        return ""
    }

    onClosed: {
        if (root.busy)
            root.exporter.cancel()
    }

    Connections {
        target: root.exporter
        function onFinished(ok, message) {
            if (ok) {
                root.failure = ""
                root.success = message
            } else {
                root.success = ""
                root.failure = message
            }
        }
    }

    NativeFileDialog {
        id: saveDialog
        purpose: "export"
        objectName: "exportRoomSaveDialog"
        currentFolder: app.defaultFileDialogFolder()
        fileMode: "save"
        title: qsTr("Export room")
        nameFilters: root.format === "json"
                     ? [ qsTr("JSON (*.json)"), qsTr("All files (*)") ]
                     : root.format === "html"
                       ? [ qsTr("Web page (*.html)"), qsTr("All files (*)") ]
                       : [ qsTr("Text (*.txt)"), qsTr("All files (*)") ]
        onAccepted: {
            root.failure = ""
            root.success = ""
            var problem = app.startRoomExport(
                selectedFile, root.format, root.rangeMode,
                parseInt(countField.text, 10) || 0,
                fromField.text, toField.text,
                root.isHtml && root.includeAttachments,
                parseInt(sizeField.text, 10) || 0,
                root.includeEncryptedText)
            if (problem.length > 0)
                root.failure = problem
        }
    }

    function ensureVisible(item) {
        var flick = scroller.contentItem
        if (!item || !flick || !flick.contentHeight)
            return
        for (var p = item; p && p !== bodyColumn; p = p.parent) {}
        if (!p)
            return
        var top = item.mapToItem(bodyColumn, 0, 0).y
        var bottom = top + item.height
        var y = flick.contentY
        if (top < y)
            y = top
        else if (bottom > y + flick.height)
            y = bottom - flick.height
        flick.contentY = Math.max(0, Math.min(y, flick.contentHeight - flick.height))
    }
    Connections {
        target: scroller.Window.window
        function onActiveFocusItemChanged() {
            root.ensureVisible(scroller.Window.window.activeFocusItem)
        }
    }

    contentItem: ScrollView {
        id: scroller
        objectName: "exportRoomScroller"
        contentWidth: availableWidth
        implicitHeight: bodyColumn.implicitHeight
        clip: true

    ColumnLayout {
        id: bodyColumn
        width: scroller.availableWidth
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Export room")
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        Label {
            text: qsTr("Format")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        SegmentedControl {
            objectName: "exportRoomFormat"
            storm: true
            enabled: !root.busy
            Layout.fillWidth: false
            model: [
                { value: "html", label: qsTr("HTML") },
                { value: "text", label: qsTr("Text") },
                { value: "json", label: qsTr("JSON") }
            ]
            current: root.format
            onActivated: (value) => root.format = value
        }

        Label {
            text: qsTr("Messages")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppComboBox {
            id: rangeBox
            objectName: "exportRoomRange"
            storm: true
            enabled: !root.busy
            Layout.fillWidth: true
            model: root.rangeLabels
            onActivated: (index) => root.rangeMode = root.rangeValues[index]
            Accessible.name: qsTr("Messages to export")
        }
        AppTextField {
            id: countField
            onTextChanged: root.clearInputProblem()
            objectName: "exportRoomCount"
            storm: true
            visible: root.rangeMode === "latest"
            enabled: !root.busy
            Layout.preferredWidth: 160
            text: "100"
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: 100000 }
            placeholderText: qsTr("Number of messages")
            Accessible.name: qsTr("Number of latest messages to export")
        }
        RowLayout {
            objectName: "exportRoomDateRow"
            visible: root.rangeMode === "dates"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppTextField {
                id: fromField
                onTextChanged: root.clearInputProblem()
                objectName: "exportRoomFromDate"
                storm: true
                enabled: !root.busy
                Layout.fillWidth: true
                placeholderText: qsTr("From YYYY-MM-DD")
                inputMethodHints: Qt.ImhDate
                Accessible.name: qsTr("Export messages from this date")
            }
            AppTextField {
                id: toField
                onTextChanged: root.clearInputProblem()
                objectName: "exportRoomToDate"
                storm: true
                enabled: !root.busy
                Layout.fillWidth: true
                placeholderText: qsTr("To YYYY-MM-DD")
                inputMethodHints: Qt.ImhDate
                Accessible.name: qsTr("Export messages up to and including "
                                      + "this date")
            }
        }

        Label {
            objectName: "exportRoomScope"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // The room name is attacker-chosen: with AutoText, markup such as
            // `<img src=...>` would render and beacon.
            textFormat: Text.PlainText
            text: {
                if (root.rangeMode === "latest")
                    return qsTr("Reads the latest messages of %1 from the "
                                + "server. A large number can take a while.")
                           .arg(root.roomName)
                if (root.rangeMode === "all")
                    return qsTr("Reads every message of %1 from the start of "
                                + "the history this account can see. In a "
                                + "large room this can take a long time.")
                           .arg(root.roomName)
                if (root.rangeMode === "dates")
                    return qsTr("Reads the messages of %1 between two dates. "
                                + "Either date may be left empty.")
                           .arg(root.roomName)
                if (!root.isHtml)
                    return root.messageCount === 1
                           ? qsTr("Exports the 1 event Lightning has loaded "
                                  + "for %1. Scroll further back first to "
                                  + "include more. Attachments are not "
                                  + "included.").arg(root.roomName)
                           : qsTr("Exports the %1 events Lightning has "
                                  + "loaded for %2. Scroll further back first "
                                  + "to include more. Attachments are not "
                                  + "included.")
                                 .arg(root.messageCount).arg(root.roomName)
                return root.messageCount === 1
                       ? qsTr("Exports the 1 event Lightning has loaded for "
                              + "%1. Scroll further back first to include "
                              + "more.").arg(root.roomName)
                       : qsTr("Exports the %1 events Lightning has loaded "
                              + "for %2. Scroll further back first to include "
                              + "more.").arg(root.messageCount)
                                        .arg(root.roomName)
            }
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }

        ColumnLayout {
            objectName: "exportRoomAttachmentOptions"
            visible: root.isHtml
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            CheckBox {
                objectName: "exportRoomAttachments"
                palette.windowText: AppTheme.stormText
                enabled: !root.busy
                text: qsTr("Include attachments")
                checked: root.includeAttachments
                onToggled: root.includeAttachments = checked
            }
            RowLayout {
                visible: root.includeAttachments
                spacing: AppTheme.spacing8
                Label {
                    text: qsTr("Size limit")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                }
                AppTextField {
                    id: sizeField
                    onTextChanged: root.clearInputProblem()
                    objectName: "exportRoomSizeLimit"
                    storm: true
                    enabled: !root.busy
                    Layout.preferredWidth: 100
                    text: "100"
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: 2000 }
                    Accessible.name: qsTr("Size limit for attachments, in "
                                          + "megabytes")
                }
                Label {
                    text: qsTr("MB")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.includeAttachments
                      ? qsTr("Attachments are embedded in the file until the "
                             + "size limit is reached; the rest are listed by "
                             + "name. SVG images are never embedded.")
                      : qsTr("Attachments are listed by name, without their "
                             + "contents.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        Rectangle {
            objectName: "exportRoomEncryptedNotice"
            visible: root.encrypted
            Layout.fillWidth: true
            implicitHeight: encryptedCol.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusMd
            color: AppTheme.stormInset
            border.width: 1
            border.color: AppTheme.stormBorderStrong
            ColumnLayout {
                id: encryptedCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("This room is encrypted. Lightning keeps its "
                               + "messages in memory and never writes them to "
                               + "disk.")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                }
                CheckBox {
                    id: includeEncryptedBox
                    objectName: "exportRoomIncludeEncrypted"
                    palette.windowText: AppTheme.stormText
                    enabled: !root.busy
                    Layout.fillWidth: true
                    contentItem: Label {
                        text: includeEncryptedBox.text
                        wrapMode: Text.Wrap
                        color: AppTheme.stormText
                        font: includeEncryptedBox.font
                        opacity: includeEncryptedBox.enabled ? 1.0 : 0.5
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: includeEncryptedBox.indicator
                            ? includeEncryptedBox.indicator.width
                              + includeEncryptedBox.spacing : 0
                    }
                    text: root.isHtml && root.includeAttachments
                          ? qsTr("Write the message text and attachments "
                                 + "(photos, files) into this file in the "
                                 + "clear")
                          : qsTr("Write the message text into this file in the "
                                 + "clear")
                    checked: root.includeEncryptedText
                    onToggled: root.includeEncryptedText = checked
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.includeEncryptedText
                          ? (root.isHtml && root.includeAttachments
                             ? qsTr("Anyone who can read the file can read the "
                                    + "conversation and open its photos and "
                                    + "files. It is not encrypted, and backups, "
                                    + "sync folders and shared drives copy it "
                                    + "like any other file.")
                             : qsTr("Anyone who can read the file can read the "
                                    + "conversation. It is not encrypted, and "
                                    + "backups, sync folders and shared drives "
                                    + "copy it like any other file."))
                          : root.isHtml && root.includeAttachments
                            ? qsTr("Without this, the export lists who sent "
                                   + "what and when, with each message's text "
                                   + "and attachments withheld.")
                            : qsTr("Without this, the export lists who sent "
                                   + "what and when, with each message's text "
                                   + "withheld.")
                    color: root.includeEncryptedText ? AppTheme.stormDanger
                                                     : AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }

        RowLayout {
            objectName: "exportRoomProgressRow"
            visible: root.busy
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            BusyIndicator {
                running: root.busy
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
            }
            Label {
                objectName: "exportRoomProgress"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.exporter ? root.exporter.progressText : ""
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
            }
        }

        Label {
            objectName: "exportRoomFailure"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.failure.length > 0
            text: root.failure
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }

        Label {
            objectName: "exportRoomSuccess"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.success.length > 0
            text: root.success
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textMeta
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "exportRoomShowInFolder"
                visible: root.success.length > 0
                text: qsTr("Show in folder")
                kind: "ghost"
                onClicked: root.exporter.showInFolder()
            }
            AppButton {
                objectName: "exportRoomCancel"
                text: root.busy ? qsTr("Cancel export")
                                : (root.success.length > 0 ? qsTr("Close")
                                                           : qsTr("Cancel"))
                kind: "ghost"
                onClicked: {
                    if (root.busy)
                        root.exporter.cancel()
                    else
                        root.close()
                }
            }
            AppButton {
                objectName: "exportRoomConfirm"
                text: qsTr("Choose file…")
                kind: "primary"
                enabled: !root.busy
                         && (root.rangeMode !== "loaded" || root.messageCount > 0)
                onClicked: {
                    var problem = root.inputProblem()
                    root.failure = problem
                    root.inputProblemShown = problem
                    if (problem.length > 0)
                        return
                    saveDialog.currentName =
                        app.suggestedExportFileName(root.format)
                    saveDialog.open()
                }
            }
        }
    }
    }
}
