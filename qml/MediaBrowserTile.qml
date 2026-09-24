import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One square in the media browser's grid. Thumbnails go through the
// authenticated media bridge like the timeline's (encrypted attachments are
// fetched and decrypted there; a raw mxc never reaches an Image). A miss
// returns "" and starts a fetch, so the binding reads `resolveTick`, bumped by
// mediaCached.
Item {
    id: tile

    required property int index
    required property string kind
    required property string body
    required property string filename
    required property string mxc
    required property string thumbnailMxc
    required property string mediaKey
    required property bool encrypted
    required property string sender
    required property string eventId

    signal activated()
    signal jumpRequested(string eventId)

    property int resolveTick: 0
    readonly property bool visual: kind === "image" || kind === "video"
    // Prefer the server's thumbnail; a thumbnail request for something without
    // one falls back to the full attachment.
    readonly property string source: {
        var _ = tile.resolveTick
        if (!visual || !app.mediaBridge.supported)
            return ""
        // Through the media registry, keyed like a timeline row, when the
        // scanner registered one: the only path that can decrypt an encrypted
        // thumbnail. The mxc path below is for rows the scanner could not
        // register.
        if (tile.mediaKey.length > 0)
            return app.mediaBridge.mediaSource(tile.mediaKey, "list_thumb")
        var uri = tile.thumbnailMxc.length > 0 ? tile.thumbnailMxc
                                               : (kind === "image" ? tile.mxc : "")
        return uri.length > 0 ? app.mediaBridge.mxcImageSource(uri, 256) : ""
    }

    Connections {
        target: app.mediaBridge
        function onMediaCached(cacheKey) { tile.resolveTick += 1 }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        radius: AppTheme.radiusSm
        color: AppTheme.inputBackground
        border.width: 1
        border.color: AppTheme.border
        clip: true

        Image {
            id: preview
            anchors.fill: parent
            source: tile.source
            // Crop what is nearly square (between half and double), fit what is
            // not, so tall or wide pictures keep what identifies them. The
            // threshold is a judgement.
            readonly property real aspect:
                (implicitWidth > 0 && implicitHeight > 0)
                ? implicitWidth / implicitHeight : 1
            fillMode: (aspect > 0.5 && aspect < 2.0)
                      ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            cache: true
            // A width-only sourceSize keeps the aspect; MediaImageProvider
            // honours it.
            sourceSize.width: 256
            visible: status === Image.Ready
        }

        // A label fallback (no thumbnail, undecryptable, deleted), not an empty
        // square.
        ColumnLayout {
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacing8
            spacing: 2
            visible: preview.status !== Image.Ready
            Icon {
                Layout.alignment: Qt.AlignHCenter
                name: tile.kind === "video" ? "movie"
                    : tile.kind === "image" ? "image"
                    : tile.kind === "audio" || tile.kind === "voice" ? "mic"
                    : tile.kind === "link" ? "link" : "attach_file"
                size: 20
                color: AppTheme.textMuted
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                textFormat: Text.PlainText
                text: tile.filename.length > 0 ? tile.filename : tile.body
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideMiddle
                maximumLineCount: 2
                wrapMode: Text.Wrap
            }
        }

        // Marks videos, so a click does not open the wrong thing.
        Rectangle {
            visible: tile.kind === "video"
            anchors.centerIn: parent
            width: 26; height: 26; radius: 13
            color: Qt.rgba(0, 0, 0, 0.45)
            Icon { anchors.centerIn: parent; name: "play_arrow"; size: 16
                   color: "#FFFFFF" }
        }

        // Encrypted is worth showing: it explains slower thumbnails.
        Icon {
            visible: tile.encrypted
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 3
            name: "lock"
            size: 12
            color: AppTheme.textMuted
        }
    }

    TapHandler {
        onTapped: tile.activated()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: tileMenu.popup()
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }

    Accessible.role: Accessible.Button
    Accessible.name: (tile.filename.length > 0 ? tile.filename : tile.body)
                     + " — " + tile.sender

    AppMenu {
        id: tileMenu
        AppMenuItem {
            text: qsTr("Go to message")
            onTriggered: tile.jumpRequested(tile.eventId)
        }
    }
}
