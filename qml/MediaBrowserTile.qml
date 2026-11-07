import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

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
    required property bool spoiler

    readonly property bool covered: {
        var revision = SpoilerState.revision
        return tile.spoiler && !SpoilerState.isRevealed(tile.eventId)
    }
    function activate() {
        if (tile.covered) {
            SpoilerState.reveal(tile.eventId)
            return
        }
        tile.activated()
    }

    signal activated()
    signal jumpRequested(string eventId)

    function openMenu() { tileMenu.popup() }
    readonly property bool keyboardCurrent:
        !!GridView.view && GridView.isCurrentItem && GridView.view.activeFocus
    readonly property string displayName:
        filename.length > 0 ? filename : body

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
    readonly property real fullFileMaxBytes: 8 * 1024 * 1024
    readonly property bool fullFileAllowed: needsFullFile
        && Number(size) > 0 && Number(size) <= fullFileMaxBytes
        && mimetype.toLowerCase().indexOf("svg") < 0
    readonly property bool fullFileInView: {
        if (!fullFileAllowed)
            return false
        var view = GridView.view
        if (!view)
            return true
        return !view.moving && tile.y + tile.height > view.contentY
               && tile.y < view.contentY + view.height
    }
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
            value = app.mediaBridge.mediaSource(tile.mediaKey, "list_thumb")
        } else {
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

        Rectangle {
            objectName: "focusRing"
            anchors.fill: parent
            radius: AppTheme.radiusSm
            color: "transparent"
            border.width: 2
            border.color: AppTheme.focusRing
            visible: tile.keyboardCurrent
            z: 5
        }

        Image {
            id: preview
            anchors.fill: parent
            source: tile.covered ? "" : tile.resolvedSource
            fillMode: (tile.declaredAspect > 0.5 && tile.declaredAspect < 2.0)
                      ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            cache: true
            sourceSize.width: 256
            visible: status === Image.Ready
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacing8
            spacing: 2
            visible: preview.status !== Image.Ready && !tile.covered
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

        Rectangle {
            objectName: "videoBadge"
            visible: tile.kind === "video" && preview.status === Image.Ready
            anchors.centerIn: parent
            width: 26; height: 26; radius: 13
            color: Qt.rgba(0, 0, 0, 0.45)
            Icon { anchors.centerIn: parent; name: "play_arrow"; size: 16
                   color: AppTheme.scrimInk }
        }

        SpoilerCover {
            anchors.fill: parent
            covered: tile.covered
            source: tile.resolvedSource
            focusable: false
            interactive: false
        }

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
        onTapped: tile.activate()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onTapped: tileMenu.popup()
    }
    HoverHandler {
        id: tileHover
        cursorShape: Qt.PointingHandCursor
    }
    ToolTip.visible: tileHover.hovered && tile.displayName.length > 0
    ToolTip.delay: 600
    ToolTip.text: tile.displayName

    Accessible.role: Accessible.Button
    Accessible.name: tile.displayName + " — " + tile.sender
    Accessible.onPressAction: tile.activate()

    AppMenu {
        id: tileMenu
        AppMenuItem {
            text: qsTr("Go to message")
            onTriggered: tile.jumpRequested(tile.eventId)
        }
    }
}
