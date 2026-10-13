import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> About -> Export logs. Saves the application's recent log, with a
// short diagnostics header, as a .zip through the desktop's own file dialog.
// Nothing is uploaded. Everything in the file is sanitized in C++
// (LogExportController); this card only says what the file contains and shows
// its real size, which the controller computes by building it.
ColumnLayout {
    id: root
    objectName: "logExportSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing8

    readonly property var exporter: (typeof app !== "undefined" && app)
                                    ? app.logExport : null
    readonly property var preview: root.exporter ? root.exporter.preview : ({})
    readonly property bool ready: root.preview.ready === true
    readonly property bool preparing: root.preview.preparing === true
    property string statusText: ""

    function sizeText(bytes) {
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg(Math.round(bytes / 1024))
        return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
    }

    // Rebuilt whenever the About page is shown, so the numbers are current.
    function refresh() {
        if (root.exporter)
            root.exporter.prepare()
    }
    onVisibleChanged: if (visible) refresh()
    Component.onCompleted: if (visible) refresh()

    SettingsGroupLabel {
        objectName: "logExportHeading"
        text: qsTr("Troubleshooting")
    }

    SettingsCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Label {
                text: qsTr("Export logs")
                font.weight: AppTheme.weightStrong
                color: AppTheme.stormText
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Save Lightning's recent log, with basic facts about "
                           + "this build and computer, as a .zip you can attach "
                           + "to a bug report. Nothing is uploaded.")
            }
            Label {
                objectName: "logExportPromise"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
                text: qsTr("This file has no messages, passwords or keys; user "
                           + "and room ids are replaced with codes.")
            }
            Label {
                objectName: "logExportPreviewText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: {
                    if (!root.ready)
                        return qsTr("Preparing…")
                    if (root.preview.available !== true)
                        return qsTr("Nothing has been logged yet.")
                    return qsTr("About %1, %2, from %3.")
                        .arg(root.sizeText(root.preview.zipBytes))
                        .arg(qsTr("%n line(s)", "", root.preview.lines))
                        .arg(qsTr("%n log file(s)", "", root.preview.sessionFiles))
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "logExportSave"
                    storm: true
                    text: qsTr("Export logs…")
                    enabled: !!root.exporter && !logSaveDialog.busy
                    onClicked: {
                        root.statusText = ""
                        logSaveDialog.currentName = root.exporter.suggestedFileName()
                        logSaveDialog.open()
                    }
                }
                AppButton {
                    objectName: "logExportCopy"
                    storm: true
                    text: qsTr("Copy last 200 lines")
                    enabled: root.ready && root.preview.available === true
                    onClicked: root.exporter.copyRecentLines(200)
                }
            }
            Label {
                objectName: "logExportStatus"
                visible: root.statusText !== ""
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: root.statusText
                textFormat: Text.PlainText
            }
        }
    }

    NativeFileDialog {
        id: logSaveDialog
        purpose: "logs"
        fileMode: "save"
        currentFolder: (typeof app !== "undefined" && app)
                       ? app.defaultFileDialogFolder() : ""
        title: qsTr("Save logs")
        nameFilters: [qsTr("Zip archive (*.zip)")]
        onAccepted: root.exporter.exportTo(selectedFile)
    }

    Connections {
        target: root.exporter
        function onExportFinished(ok, message) {
            root.statusText = ok ? qsTr("Saved.")
                                 : qsTr("Could not save the file: %1").arg(message)
        }
        function onCopied(lines) {
            root.statusText = qsTr("Copied %n line(s).", "", lines)
        }
    }
}
