import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One square in the media browser's grid. Thumbnails go through the
// authenticated media bridge like the timeline's (encrypted attachments are
// fetched and decrypted there; a raw mxc never reaches an Image). A miss
// returns "" and starts a fetch; mediaCached asks again until a source
// arrives, and an arrived source is kept, so a later eviction from the
// bridge's RAM cache cannot blank a drawn tile and send it round again.
Item {
    id: tile

    required property int index
    required property string kind
    required property string body
    required property string filename
    required property string mimetype
    required property var size
    required property int mediaWidth
    required property int mediaHeight
    required property string mxc
    required property string thumbnailMxc
    required property string mediaKey
    required property bool encrypted
    required property string sender
    required property string eventId

    signal activated()
    signal jumpRequested(string eventId)

    readonly property bool visual: kind === "image" || kind === "video"
                                   || kind === "sticker"
    // An encrypted picture whose sender attached no thumbnail has no small copy
    // anywhere: a server cannot scale ciphertext, so the bridge refuses the
    // list class for it ("unavailable", rust/src/rooms.rs media_fetch). The
    // file itself, decrypted by the bridge, is the only picture there is, and
    // the timeline shows such an image the same way (mediaThumbAvailable ?
    // "thumb" : "full"). Never a video: the file is the video.
    readonly property bool needsFullFile: encrypted
        && thumbnailMxc.length === 0 && mediaKey.length > 0
        && (kind === "image" || kind === "sticker")
    // Bounded, as a grid of a whole history must be: only a picture of a known
    // size up to this many bytes (the bridge's own "large" line), never SVG
    // (refused on every path), and only once this tile has stayed on screen
    // for a moment. A flick, a wheel glide (which moves contentY without
    // `moving`) or a scrollbar drag through hundreds of tiles fetches nothing,
    // and neither does a delegate the view builds and then positions
    // elsewhere. The bridge caps how many full payloads are in flight.
    readonly property real fullFileMaxBytes: 8 * 1024 * 1024
    readonly property bool fullFileAllowed: needsFullFile
        && Number(size) > 0 && Number(size) <= fullFileMaxBytes
        && mimetype.toLowerCase().indexOf("svg") < 0
    // Only a tile that may fetch its file follows the scroll position; the
    // rest never read contentY.
    readonly property bool fullFileInView: {
        if (!fullFileAllowed)
            return false
        var view = GridView.view
        if (!view)
            return true
        return !view.moving && tile.y + tile.height > view.contentY
               && tile.y < view.contentY + view.height
    }
    // Set by the settle timer, cleared when the tile leaves the screen.
    property bool fullFileSettled: false
    onFullFileInViewChanged: if (!fullFileInView) fullFileSettled = false
    Timer {
        interval: 300
        running: tile.fullFileInView && !tile.fullFileSettled
                 && tile.resolvedSource.length === 0
        onTriggered: {
            tile.fullFileSettled = true
            tile.refresh()
        }
    }

    property string resolvedSource: ""

    function refresh() {
        if (resolvedSource.length > 0 || !visual || !app.mediaBridge.supported)
            return
        var value = ""
        if (needsFullFile) {
            if (!fullFileAllowed || !fullFileSettled)
                return
            value = app.mediaBridge.mediaSource(tile.mediaKey, "full")
        } else if (tile.mediaKey.length > 0) {
            // Through the media registry, keyed like a timeline row, when the
            // scanner registered one: the only path that can decrypt an
            // encrypted thumbnail. The server's thumbnail for anything else.
            value = app.mediaBridge.mediaSource(tile.mediaKey, "list_thumb")
        } else {
            // A row the scanner could not register.
            var uri = tile.thumbnailMxc.length > 0
                ? tile.thumbnailMxc
                : (kind === "image" || kind === "sticker" ? tile.mxc : "")
            if (uri.length > 0)
                value = app.mediaBridge.mxcImageSource(uri, 256)
        }
        if (value.length > 0)
            resolvedSource = value
    }
    Component.onCompleted: refresh()
    onMediaKeyChanged: {
        resolvedSource = ""
        refresh()
    }

    Connections {
        target: app.mediaBridge
        function onMediaCached(cacheKey) {
            if (tile.resolvedSource.length === 0)
                tile.refresh()
        }
        function onMediaRetryable(cacheKey) {
            if (tile.resolvedSource.length === 0)
                tile.refresh()
        }
    }

    // Crop what is nearly square (between half and double), fit what is not,
    // so tall or wide pictures keep what identifies them. The threshold is a
    // judgement. The event's DECLARED size decides, never the loaded image's:
    // Image's implicit size follows its fillMode (switching to or from
    // PreserveAspectFit with a sourceSize zeroes it while the picture is loaded
    // again), so an aspect read from it flipped crop and fit on every load and
    // logged a binding loop. Unknown dimensions crop, as mainstream grids do.
    readonly property real declaredAspect:
        (mediaWidth > 0 && mediaHeight > 0) ? mediaWidth / mediaHeight : 1

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
            source: tile.resolvedSource
            fillMode: (tile.declaredAspect > 0.5 && tile.declaredAspect < 2.0)
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
                    : tile.kind === "image" || tile.kind === "sticker" ? "image"
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
        // Touch has no buttons, so a tap would fire this too.
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
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
