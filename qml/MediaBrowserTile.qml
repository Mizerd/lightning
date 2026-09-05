import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One square in the media browser's grid.
//
// Thumbnails resolve through the authenticated media bridge exactly as the
// timeline's do — an encrypted attachment is fetched and decrypted by the
// same path, and a raw mxc never reaches an Image source. The bridge answers
// ASYNCHRONOUSLY: a miss returns "" and starts a fetch, so the binding reads
// `resolveTick`, which the mediaCached signal bumps. Without that the tile
// would stay empty forever on the first view of every image.
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
    // Prefer the server's thumbnail. Asking for a thumbnail of something that
    // has none falls back to the FULL attachment, which is how a media list
    // once downloaded whole videos to fill 42px tiles.
    readonly property string source: {
        var _ = tile.resolveTick
        if (!visual || !app.mediaBridge.supported)
            return ""
        // Through the media REGISTRY, keyed like a timeline row, whenever the
        // scanner registered one: that is the only path that can decrypt an
        // encrypted attachment's thumbnail. Asking the server for a thumbnail
        // of an encrypted mxc is impossible, and every tile in an encrypted
        // room failed with "network" (reported with a screenshot). The mxc
        // path below stays for a row the scanner could not register.
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
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            // A width-only sourceSize keeps the aspect; the provider honours
            // it (see MediaImageProvider — a width-only request used to read
            // as "no size asked for" and decoded at full resolution).
            sourceSize.width: 256
            visible: status === Image.Ready
        }

        // The fallback is a LABEL, not a broken image: a video with no
        // thumbnail, an undecryptable attachment or a deleted file all land
        // here, and an empty grey square tells the reader nothing.
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

        // A video is not an image, and a grid that does not say so invites a
        // click that opens the wrong thing.
        Rectangle {
            visible: tile.kind === "video"
            anchors.centerIn: parent
            width: 26; height: 26; radius: 13
            color: Qt.rgba(0, 0, 0, 0.45)
            Icon { anchors.centerIn: parent; name: "play_arrow"; size: 16
                   color: "#FFFFFF" }
        }

        // Encrypted is worth showing: it is why a thumbnail may take longer,
        // and it is the reassurance that the browser did not fetch plaintext.
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
