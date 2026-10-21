import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Downloads, as Element shows them: one row per file while it downloads
// (cancellable), then "Saved to <folder>" with Open and Show in folder, the
// way Element's "Download completed" toast offers them. A finished row clears
// itself after ten seconds unless the pointer rests on the card; a failed one
// stays until dismissed or retried. A file whose type can run programs is
// never opened from here: it offers Show in folder only and says why.
//
// A Snap or Flatpak gets a portal-granted file back from the save dialog in
// the document portal's mount, whose folder is a document id; folderName is
// empty then (unless the portal named the real folder) and the row says only
// "Saved" under the file's name, never the id.
//
// The list is app.downloads.items, owned by C++, so it survives a room
// switch. PaneDownloadsCard places this card and feeds it the downloads no
// row shows; a row's own AttachmentDownloadNotice (a separate one-line strip)
// shows the download its file card claims (DownloadNotices).
Rectangle {
    id: card
    objectName: "downloadsCard"
    // [{id, fileName, folderName, state, message, risky, canOpen, revealFailed}]
    property var items: []
    // How long a finished row stays; a property so a test need not wait ten
    // seconds.
    property int dismissAfterMs: 10000
    visible: items.length > 0
    radius: AppTheme.radiusLg
    color: AppTheme.cardElevated
    border.width: 1
    border.color: AppTheme.borderSubtle
    width: 300
    implicitHeight: rows.implicitHeight + AppTheme.spacing8 * 2
    height: implicitHeight

    // The pointer resting anywhere on the card holds every finished row.
    HoverHandler { id: cardHover }

    // A soft shadow so the card reads as floating over the timeline.
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: -3
        radius: card.radius
        color: AppTheme.shadowSoft
    }

    ColumnLayout {
        id: rows
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: AppTheme.spacing8
        anchors.leftMargin: AppTheme.spacing12
        spacing: AppTheme.spacing8

        Repeater {
            model: card.items
            delegate: ColumnLayout {
                id: row
                required property var modelData
                required property int index
                readonly property bool saving: modelData.state === "saving"
                readonly property bool done: modelData.state === "done"
                readonly property bool failed: modelData.state === "failed"

                objectName: "downloadRow"
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                // Element's toast expires after ten seconds; a finished row
                // here too, held while the pointer is on the card. It counts
                // only while the card is actually visible (effective
                // visibility: a scrolled-away or hidden host stops it), so an
                // outcome nobody saw is never dismissed unseen; it is shown
                // once, for the full interval, when it next appears.
                // Not for a download that was shown beside its file card
                // (keepUntilDismissed, set by PaneDownloadsCard): that one
                // stays until the user opens or dismisses it.
                Timer {
                    interval: card.dismissAfterMs
                    running: row.done && row.modelData.keepUntilDismissed !== true
                             && !cardHover.hovered && card.visible
                    onTriggered: app.downloads.dismiss(row.modelData.id)
                }

                // A hairline between rows, none above the first.
                Rectangle {
                    visible: row.index > 0
                    Layout.fillWidth: true
                    Layout.rightMargin: AppTheme.spacing4
                    Layout.preferredHeight: 1
                    color: AppTheme.borderSubtle
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8

                    Item {
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 2
                        AppBusyIndicator {
                            anchors.centerIn: parent
                            visible: row.saving
                            running: row.saving
                            size: 18
                        }
                        Icon {
                            anchors.centerIn: parent
                            visible: !row.saving
                            name: row.done ? (row.modelData.risky ? "shield"
                                                                   : "check_circle")
                                           : "error"
                            size: 18
                            color: row.failed ? AppTheme.danger
                                 : row.modelData.risky ? AppTheme.warning
                                 : AppTheme.success
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 1
                        Label {
                            objectName: "downloadFileName"
                            Layout.fillWidth: true
                            // Sender-chosen: never markup.
                            textFormat: Text.PlainText
                            text: row.modelData.fileName
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.textMeta + 1
                            font.weight: AppTheme.weightMedium
                            elide: Text.ElideMiddle
                        }
                        Label {
                            objectName: "downloadStatus"
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            color: row.failed ? AppTheme.danger
                                              : AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            text: {
                                var folder = row.modelData.folderName || ""
                                if (row.saving)
                                    return folder.length > 0
                                        ? qsTr("Downloading to %1…").arg(folder)
                                        : qsTr("Downloading…")
                                if (row.failed)
                                    return row.modelData.message
                                           || qsTr("The download failed.")
                                // Show in folder found no way to show it.
                                if (row.modelData.revealFailed)
                                    return qsTr("Saved, but its folder "
                                                + "couldn't be opened.")
                                return folder.length > 0
                                    ? qsTr("Saved to %1").arg(folder)
                                    : qsTr("Saved")
                            }
                        }
                    }

                    IconButton {
                        objectName: row.saving ? "downloadCancelButton"
                                               : "downloadDismissButton"
                        Layout.alignment: Qt.AlignTop
                        iconName: "close"
                        iconSize: 16
                        implicitWidth: 28; implicitHeight: 28
                        Accessible.name: row.saving
                            ? qsTr("Cancel download of %1")
                                  .arg(row.modelData.fileName)
                            : qsTr("Dismiss")
                        ToolTip.text: row.saving ? qsTr("Cancel download")
                                                 : qsTr("Dismiss")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: row.saving
                                   ? app.downloads.cancel(row.modelData.id)
                                   : app.downloads.dismiss(row.modelData.id)
                    }
                }

                // Why a finished file has no Open button.
                Label {
                    objectName: "downloadRiskNotice"
                    visible: row.done && row.modelData.risky
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    Layout.rightMargin: AppTheme.spacing8
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("This type of file can run programs, so "
                               + "Lightning won't open it. Open it from its "
                               + "folder only if you trust the sender.")
                }

                RowLayout {
                    visible: row.done || row.failed
                    Layout.leftMargin: 28
                    spacing: AppTheme.spacing8
                    AppButton {
                        objectName: "downloadOpenButton"
                        visible: row.done && row.modelData.canOpen
                        size: "sm"
                        kind: "primary"
                        text: qsTr("Open")
                        Accessible.name: qsTr("Open %1").arg(row.modelData.fileName)
                        onClicked: {
                            if (app.downloads.open(row.modelData.id))
                                app.downloads.dismiss(row.modelData.id)
                        }
                    }
                    AppButton {
                        objectName: "downloadShowInFolderButton"
                        visible: row.done
                        size: "sm"
                        kind: row.modelData.canOpen ? "secondary" : "primary"
                        text: qsTr("Show in folder")
                        onClicked: app.downloads.showInFolder(row.modelData.id)
                    }
                    AppButton {
                        objectName: "downloadRetryButton"
                        visible: row.failed
                        size: "sm"
                        kind: "secondary"
                        text: qsTr("Try again")
                        onClicked: app.downloads.retry(row.modelData.id)
                    }
                }
            }
        }
    }
}
