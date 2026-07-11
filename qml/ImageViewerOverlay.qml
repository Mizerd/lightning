import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 15): in-app image viewer. Full-window scrim, fit-to-window,
// wheel/keyboard zoom with pan, previous/next across the images currently
// loaded in the room timeline (no history is fetched), Save As through the
// media bridge, animated GIF playback. Escape or a click on the scrim
// closes. The SDK timeline itself is untouched — this is a pure overlay.
Popup {
    id: viewer
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? parent.width : 800
    height: parent ? parent.height : 600
    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape

    // Entries from TimelineModel::imageEntries(); each has
    // {row, mediaKey, filename, sender, timestamp, mime, httpUrl}.
    property var entries: []
    property int currentIndex: -1
    readonly property var current:
        (currentIndex >= 0 && currentIndex < entries.length)
        ? entries[currentIndex] : null

    // Zoom/pan state.
    property real zoom: 1.0
    readonly property real minZoom: 0.2
    readonly property real maxZoom: 8.0
    // Fit-to-window base size, computed when the image loads.
    property real baseWidth: 0
    property real baseHeight: 0

    // Bridge source plumbing (mirrors MessageDelegate's pattern).
    readonly property bool usesBridge:
        current !== null && current.mediaKey.length > 0 && app.mediaBridge.supported
    readonly property string bridgeCacheKey:
        current !== null ? ("full:" + current.mediaKey) : ""
    property string bridgeSource: ""
    property string animatedSource: ""
    property bool bridgeFailed: false
    readonly property bool isGif:
        current !== null && current.mime === "image/gif"

    function openFor(mediaKey, httpUrl) {
        entries = app.timeline.imageEntries()
        currentIndex = -1
        for (var i = 0; i < entries.length; ++i) {
            if ((mediaKey.length > 0 && entries[i].mediaKey === mediaKey)
                    || (mediaKey.length === 0 && httpUrl
                        && entries[i].httpUrl.toString() === httpUrl.toString())) {
                currentIndex = i
                break
            }
        }
        if (currentIndex === -1 && entries.length > 0)
            currentIndex = entries.length - 1
        if (currentIndex === -1)
            return
        resetView()
        open()
        loadCurrent()
    }

    function resetView() {
        zoom = 1.0
        baseWidth = 0
        baseHeight = 0
        bridgeSource = ""
        animatedSource = ""
        bridgeFailed = false
    }

    function loadCurrent() {
        if (!usesBridge || current === null)
            return
        bridgeFailed = false
        if (isGif && app.settings.animateGifPreviews)
            animatedSource = app.mediaBridge.animatedSource(current.mediaKey)
        else
            bridgeSource = app.mediaBridge.mediaSource(current.mediaKey, "full")
    }

    function showAt(index) {
        if (index < 0 || index >= entries.length)
            return
        currentIndex = index
        resetView()
        loadCurrent()
    }

    function fitImage(w, h) {
        if (w <= 0 || h <= 0) return
        var availW = viewer.width - 32
        var availH = viewer.height - 120 // header + footer chrome
        var scale = Math.min(1.0, Math.min(availW / w, availH / h))
        baseWidth = w * scale
        baseHeight = h * scale
    }

    Connections {
        target: app.mediaBridge
        enabled: viewer.opened && viewer.usesBridge
        function onMediaCached(cacheKey) {
            if (cacheKey === viewer.bridgeCacheKey)
                viewer.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
        }
        function onAnimatedMediaReady(cacheKey) {
            if (cacheKey === viewer.bridgeCacheKey)
                viewer.animatedSource = app.mediaBridge.animatedSource(viewer.current.mediaKey)
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === viewer.bridgeCacheKey)
                viewer.bridgeFailed = true
        }
        function onSaveFinished(ok, message) {
            if (viewer.opened) {
                saveNotice.text = message
                saveNotice.ok = ok
                saveNoticeTimer.restart()
            }
        }
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save image as…")
        fileMode: FileDialog.SaveFile
        onAccepted: {
            if (viewer.current !== null)
                app.mediaBridge.saveAs(viewer.current.mediaKey, selectedFile)
        }
    }

    background: Rectangle {
        // Deliberate scrim: readable over both themes.
        color: Qt.rgba(0, 0, 0, 0.85)
    }

    contentItem: FocusScope {
        focus: true
        Keys.onLeftPressed: viewer.showAt(viewer.currentIndex - 1)
        Keys.onRightPressed: viewer.showAt(viewer.currentIndex + 1)
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
                viewer.zoom = Math.min(viewer.zoom * 1.2, viewer.maxZoom)
                event.accepted = true
            } else if (event.key === Qt.Key_Minus) {
                viewer.zoom = Math.max(viewer.zoom / 1.2, viewer.minZoom)
                event.accepted = true
            } else if (event.key === Qt.Key_0) {
                viewer.zoom = 1.0
                event.accepted = true
            }
        }

        // Clicking the scrim (outside the image) closes.
        TapHandler {
            onTapped: viewer.close()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Header: filename + actions ────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: viewer.current !== null ? viewer.current.filename : ""
                        color: "#F8FAFC"
                        font.pixelSize: AppTheme.fontSizeM
                        font.weight: Font.DemiBold
                        elide: Label.ElideMiddle
                    }
                    Label {
                        Layout.fillWidth: true
                        text: viewer.current !== null
                              ? qsTr("%1 · %2")
                                    .arg(viewer.current.sender)
                                    .arg(Qt.formatDateTime(viewer.current.timestamp,
                                                           "d MMM yyyy hh:mm"))
                              : ""
                        color: "#94A3B8"
                        font.pixelSize: AppTheme.fontSizeXS
                        elide: Label.ElideRight
                    }
                }
                ToolButton {
                    text: "−"
                    Accessible.name: qsTr("Zoom out")
                    onClicked: viewer.zoom = Math.max(viewer.zoom / 1.2, viewer.minZoom)
                }
                Label {
                    text: Math.round(viewer.zoom * 100) + "%"
                    color: "#CBD5E1"
                    font.pixelSize: AppTheme.fontSizeS
                }
                ToolButton {
                    text: "＋"
                    Accessible.name: qsTr("Zoom in")
                    onClicked: viewer.zoom = Math.min(viewer.zoom * 1.2, viewer.maxZoom)
                }
                ToolButton {
                    text: qsTr("Fit")
                    Accessible.name: qsTr("Reset zoom")
                    onClicked: viewer.zoom = 1.0
                }
                ToolButton {
                    visible: viewer.usesBridge
                    text: qsTr("Save as…")
                    onClicked: {
                        if (viewer.current !== null) {
                            saveDialog.currentFile =
                                "file:///" + (viewer.current.filename || "image")
                            saveDialog.open()
                        }
                    }
                }
                ToolButton {
                    text: "✕"
                    Accessible.name: qsTr("Close image viewer")
                    onClicked: viewer.close()
                }
            }

            // ── Image area ────────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Flickable {
                    id: flick
                    anchors.fill: parent
                    contentWidth: imageHolder.width
                    contentHeight: imageHolder.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Item {
                        id: imageHolder
                        width: Math.max(flick.width, viewer.baseWidth * viewer.zoom)
                        height: Math.max(flick.height, viewer.baseHeight * viewer.zoom)

                        // Consume taps over the image so only scrim clicks close.
                        TapHandler { onTapped: {} }

                        // Static image path.
                        Image {
                            id: staticImage
                            visible: !viewer.isGif || !app.settings.animateGifPreviews
                            anchors.centerIn: parent
                            width: viewer.baseWidth * viewer.zoom
                            height: viewer.baseHeight * viewer.zoom
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            source: visible
                                    ? (viewer.usesBridge ? viewer.bridgeSource
                                                         : (viewer.current !== null
                                                            ? viewer.current.httpUrl : ""))
                                    : ""
                            onStatusChanged: {
                                if (status === Image.Ready && viewer.baseWidth === 0)
                                    viewer.fitImage(implicitWidth, implicitHeight)
                            }
                        }
                        // Animated GIF path.
                        AnimatedImage {
                            id: animatedImage
                            visible: viewer.isGif && app.settings.animateGifPreviews
                                     && viewer.animatedSource.length > 0
                            anchors.centerIn: parent
                            width: viewer.baseWidth * viewer.zoom
                            height: viewer.baseHeight * viewer.zoom
                            fillMode: Image.PreserveAspectFit
                            cache: false
                            playing: visible && viewer.opened
                            source: visible ? viewer.animatedSource : ""
                            onStatusChanged: {
                                if (status === Image.Ready && viewer.baseWidth === 0)
                                    viewer.fitImage(implicitWidth, implicitHeight)
                            }
                        }
                    }

                    WheelHandler {
                        target: null
                        onWheel: (event) => {
                            const factor = event.angleDelta.y > 0 ? 1.2 : 1 / 1.2
                            viewer.zoom = Math.min(Math.max(viewer.zoom * factor,
                                                            viewer.minZoom),
                                                   viewer.maxZoom)
                        }
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    running: viewer.opened && !viewer.bridgeFailed
                             && ((viewer.usesBridge && viewer.bridgeSource === "")
                                 || staticImage.status === Image.Loading
                                 || animatedImage.status === Image.Loading)
                    visible: running
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: viewer.bridgeFailed
                             || staticImage.status === Image.Error
                             || animatedImage.status === Image.Error
                    spacing: AppTheme.spacing8
                    Label {
                        text: qsTr("The image could not be loaded.")
                        color: "#F8FAFC"
                    }
                    Button {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Retry")
                        onClicked: viewer.loadCurrent()
                    }
                }

                // Previous / next navigation.
                ToolButton {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: AppTheme.spacing8
                    visible: viewer.currentIndex > 0
                    text: "‹"
                    font.pixelSize: 28
                    Accessible.name: qsTr("Previous image")
                    onClicked: viewer.showAt(viewer.currentIndex - 1)
                }
                ToolButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: AppTheme.spacing8
                    visible: viewer.currentIndex < viewer.entries.length - 1
                    text: "›"
                    font.pixelSize: 28
                    Accessible.name: qsTr("Next image")
                    onClicked: viewer.showAt(viewer.currentIndex + 1)
                }
            }

            // ── Footer: save feedback + position ─────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing8
                Label {
                    id: saveNotice
                    property bool ok: true
                    visible: text.length > 0
                    color: ok ? AppTheme.success : AppTheme.danger
                    font.pixelSize: AppTheme.fontSizeS
                    Timer {
                        id: saveNoticeTimer
                        interval: 5000
                        onTriggered: saveNotice.text = ""
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: viewer.entries.length > 1
                    text: qsTr("%1 of %2").arg(viewer.currentIndex + 1)
                                          .arg(viewer.entries.length)
                    color: "#94A3B8"
                    font.pixelSize: AppTheme.fontSizeS
                }
            }
        }
    }
}
