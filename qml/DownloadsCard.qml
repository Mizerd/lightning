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
// The list is app.downloads.items, owned by C++, so it survives a room
// switch. The card rests in the bottom-left corner of `safeArea`, which the
// host sets to the chat column between the room header and the composer (the
// area the voice mini-player uses), so it never covers the Spaces rail, the
// account and settings buttons, the header or the composer. When the
// mini-player rests in the same corner (`avoid`), the card stands above it.
Rectangle {
    id: card

    objectName: "downloadsCard"

    // Where the card may sit, in the parent's coordinates.
    property rect safeArea: Qt.rect(0, 0, parent ? parent.width : 0,
                                    parent ? parent.height : 0)
    // Another floating card in the parent's coordinates, or null.
    property Item avoid: null
    // [{id, fileName, folderName, state, message, risky, canOpen}]
    property var items: app && app.downloads ? app.downloads.items : []
    readonly property int margin: AppTheme.spacing12

    readonly property rect avoidRect: card.avoid && card.avoid.visible
                                      && card.avoid.opacity > 0
        ? Qt.rect(card.avoid.x, card.avoid.y, card.avoid.width,
                  card.avoid.height)
        : Qt.rect(0, 0, 0, 0)
    readonly property real restY: card.safeArea.y + card.safeArea.height
                                  - card.height - card.margin
    readonly property bool overlapsAvoid:
        card.avoidRect.width > 0
        && card.x < card.avoidRect.x + card.avoidRect.width
        && card.avoidRect.x < card.x + card.width
        && card.restY < card.avoidRect.y + card.avoidRect.height
        && card.avoidRect.y < card.restY + card.height

    visible: opacity > 0
    opacity: card.items.length > 0 ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    radius: AppTheme.radiusLg
    color: AppTheme.cardElevated
    border.width: 1
    border.color: AppTheme.borderSubtle
    width: Math.max(0, Math.min(340, card.safeArea.width - card.margin * 2))
    height: rows.implicitHeight + AppTheme.spacing8 * 2
    x: card.safeArea.x + card.margin
    y: Math.max(card.safeArea.y + card.margin,
                card.overlapsAvoid
                ? card.avoidRect.y - card.height - AppTheme.spacing8
                : card.restY)

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
                // here too, held while the pointer is on the card.
                Timer {
                    interval: 10000
                    running: row.done && !cardHover.hovered
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
