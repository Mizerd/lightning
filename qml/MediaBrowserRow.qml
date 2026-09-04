import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One row in the media browser's list view — files, audio and links, and
// visual media when the reader prefers a list.
//
// Links get a different shape from attachments on purpose: what identifies a
// link is its HOST, and what identifies a file is its name and size.
ItemDelegate {
    id: row

    required property int index
    required property string kind
    required property string body
    required property string filename
    required property string mimetype
    required property var size
    required property var durationMs
    required property string mxc
    required property string thumbnailMxc
    required property bool encrypted
    required property string url
    required property string host
    required property string sender
    required property var timestampMs
    required property string eventId

    signal activated()
    signal jumpRequested(string eventId)

    onClicked: row.activated()
    height: layout.implicitHeight + AppTheme.spacing8 * 2

    Accessible.name: (filename.length > 0 ? filename : body) + " — " + sender

    contentItem: RowLayout {
        id: layout
        spacing: AppTheme.spacing8

        Icon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
            name: row.kind === "video" ? "movie"
                : row.kind === "image" ? "image"
                : row.kind === "voice" ? "mic"
                : row.kind === "audio" ? "music_note"
                : row.kind === "link" ? "link" : "attach_file"
            size: 18
            color: AppTheme.icon
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                Layout.fillWidth: true
                // Remote text: never markup, in either line.
                textFormat: Text.PlainText
                text: {
                    if (row.kind === "link")
                        return row.host.length > 0 ? row.host : row.url
                    return row.filename.length > 0 ? row.filename
                                                   : (row.body.length > 0
                                                      ? row.body
                                                      : qsTr("(unnamed)"))
                }
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.textBody
                elide: Label.ElideMiddle
            }

            // A link's second line is the URL itself; an attachment's is its
            // sender, size and date. Different questions, different answers.
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                visible: row.kind === "link"
                text: row.url
                color: AppTheme.link
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
            }

            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: {
                    var bits = [row.sender]
                    if (row.kind !== "link" && row.size > 0)
                        bits.push(row.humanSize(row.size))
                    if (row.durationMs > 0)
                        bits.push(row.humanDuration(row.durationMs))
                    bits.push(Qt.formatDateTime(
                        new Date(row.timestampMs), "d MMM yyyy hh:mm"))
                    return bits.join(" · ")
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
            }
        }

        Icon {
            visible: row.encrypted
            name: "lock"
            size: 12
            color: AppTheme.textMuted
            Layout.alignment: Qt.AlignVCenter
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: rowMenu.popup()
    }

    AppMenu {
        id: rowMenu
        AppMenuItem {
            text: qsTr("Go to message")
            onTriggered: row.jumpRequested(row.eventId)
        }
        AppMenuItem {
            text: qsTr("Copy link")
            visible: row.kind === "link" && row.url.length > 0
            height: visible ? implicitHeight : 0
            onTriggered: app.copyToClipboard(row.url)
        }
    }

    // Locale-aware enough to be honest without pretending to be a formatter:
    // binary units, one decimal, and never "0.0 KB" for a 400-byte file.
    function humanSize(bytes) {
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg((bytes / 1024).toFixed(1))
        if (bytes < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(1))
    }

    function humanDuration(ms) {
        var total = Math.round(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
}
