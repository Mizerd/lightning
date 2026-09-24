import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// In-app image viewer: full-window scrim, click-to-zoom at the pointer,
// wheel zoom while fitted and wheel pan once zoomed, drag panning, wrapping
// previous/next across the images loaded in the timeline (no history is
// fetched), a thumbnail strip, Save As through the media bridge, animated
// GIF playback, and chrome that hides while zoomed. Escape or a click on the
// scrim closes. A pure overlay: the SDK timeline is untouched.
//
// Zoom is continuous over 0.1x-10x and the +/-/0/F keys are kept alongside
// the click/wheel gestures.
Popup {
    id: viewer

    // Sender-chosen filename -> hardened, percent-encoded leaf (see
    // TimelinePane.suggestedSaveUrl).
    function suggestedSaveUrl() {
        var raw = viewer.current ? (viewer.current.filename || "") : ""
        var leaf = app.mediaBridge.suggestedSaveName(raw)
        if (!leaf || leaf.length === 0)
            leaf = "image"
        return "file:///" + encodeURIComponent(leaf)
    }
    objectName: "imageViewerOverlay"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? parent.width : 800
    height: parent ? parent.height : 600
    modal: true
    padding: 0
    // A Popup doesn't take focus unless asked; without this every key binding,
    // including Escape via the close policy, is dead.
    focus: true
    closePolicy: Popup.CloseOnEscape

    // Entries from TimelineModel::imageEntries(); each has
    // {row, mediaKey, filename, sender, timestamp, mime, httpUrl}.
    property var entries: []
    property int currentIndex: -1
    readonly property var current:
        (currentIndex >= 0 && currentIndex < entries.length)
        ? entries[currentIndex] : null

    // `zoom` is relative to the fit-to-window size (1.0 = fits the viewport).
    // naturalWidth/Height are the decoded dimensions; actualSizeZoom shows one
    // image pixel per logical pixel.
    property real zoom: 1.0
    readonly property real minZoom: 0.1
    readonly property real maxZoom: 10.0
    property real baseWidth: 0
    property real baseHeight: 0
    property real naturalWidth: 0
    property real naturalHeight: 0
    readonly property real actualSizeZoom:
        (baseWidth > 0 && naturalWidth > 0) ? naturalWidth / baseWidth : 1.0
    readonly property int percentZoom:
        baseWidth > 0 && naturalWidth > 0
        ? Math.round(zoom * baseWidth / naturalWidth * 100)
        : Math.round(zoom * 100)

    // Bridge source plumbing (as in MessageDelegate).
    readonly property bool usesBridge:
        current !== null && current.mediaKey.length > 0 && app.mediaBridge.supported
    readonly property string bridgeCacheKey:
        current !== null ? ("full:" + current.mediaKey) : ""
    property string bridgeSource: ""
    property string animatedSource: ""
    property bool bridgeFailed: false
    readonly property string currentMime:
        current !== null ? (current.mime || "").toLowerCase() : ""
    // Not a mimetype test: MSC2545 stickers often omit the mimetype. This only
    // says "worth asking"; MediaBridge.animatedExtensionFor decides from the
    // container magic. A declared PNG or JPEG is not asked about.
    readonly property bool maybeAnimated:
        current !== null && (currentMime === "" || currentMime === "image/gif"
                             || currentMime === "image/webp")
    // The viewer is explicit intent: animations play unless autoplay is Never
    // (2).
    readonly property bool animateGifs: app.settings.gifAutoplay !== 2

    /// Open on an explicit list and index (e.g. the room's Media tab). The
    /// viewer pages through the list it is given.
    function openAt(list, index) {
        if (!list || index < 0 || index >= list.length)
            return
        entries = list
        currentIndex = index
        resetView()
        open()
        loadCurrent()
    }

    /// Open on the images the timeline has loaded, located by media key (or by
    /// URL on the HTTP backend).
    function openFor(mediaKey, httpUrl) {
        var list = app.timeline.imageEntries()
        var found = -1
        for (var i = 0; i < list.length; ++i) {
            if ((mediaKey.length > 0 && list[i].mediaKey === mediaKey)
                    || (mediaKey.length === 0 && httpUrl
                        && list[i].httpUrl.toString() === httpUrl.toString())) {
                found = i
                break
            }
        }
        if (found === -1) {
            // A miss opens the requested image alone rather than a different
            // one.
            if (mediaKey.length === 0 && !httpUrl)
                return
            list = [{
                "row": -1,
                "mediaKey": mediaKey,
                "filename": "",
                "sender": "",
                "timestamp": undefined,
                "mime": "",
                "httpUrl": httpUrl ? httpUrl : "",
                "isImage": true,
                "isVideo": false,
                "isVisual": true,
                "thumbAvailable": false,
                "size": 0
            }]
            found = 0
        }
        openAt(list, found)
    }

    function resetView() {
        zoom = 1.0
        baseWidth = 0
        baseHeight = 0
        naturalWidth = 0
        naturalHeight = 0
        bridgeSource = ""
        animatedSource = ""
        bridgeFailed = false
    }

    function loadCurrent() {
        if (!usesBridge || current === null)
            return
        bridgeFailed = false
        // Always request both; they share the "full:" cache key, so it is one
        // fetch. The still is drawn until the animation validates. The animated
        // request is speculative (see MediaBridge::animatedSource): a
        // non-animated payload answers with silence rather than marking the key
        // failed.
        if (maybeAnimated && animateGifs)
            animatedSource = app.mediaBridge.animatedSource(current.mediaKey, true)
        bridgeSource = app.mediaBridge.mediaSource(current.mediaKey, "full")
    }

    // Wraps at both ends. The double modulo handles JavaScript's `%` keeping
    // the dividend's sign (`-1 % 5` is `-1`).
    function showAt(index) {
        if (entries.length === 0)
            return
        var n = entries.length
        index = ((index % n) + n) % n
        if (index === currentIndex)
            return
        currentIndex = index
        resetView()
        loadCurrent()
    }

    function fitImage(w, h) {
        if (w <= 0 || h <= 0) return
        naturalWidth = w
        naturalHeight = h
        var availW = viewer.width - 32
        var availH = viewer.height - 120 // header + footer chrome
        var scale = Math.min(1.0, Math.min(availW / w, availH / h))
        baseWidth = w * scale
        baseHeight = h * scale
    }
    // On window resize the fit base follows the viewport; relative zoom is
    // kept.
    onWidthChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)
    onHeightChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)

    // Pointer-centred zoom: the image point under `viewportPoint` (flick
    // viewport coordinates) stays put.
    function zoomAt(viewportPoint, newZoom) {
        newZoom = Math.min(maxZoom, Math.max(minZoom, newZoom))
        if (baseWidth <= 0 || newZoom === zoom) {
            zoom = newZoom
            return
        }
        var z0 = zoom
        var img0w = baseWidth * z0
        var img0h = baseHeight * z0
        var off0x = Math.max(0, (flick.width - img0w) / 2)
        var off0y = Math.max(0, (flick.height - img0h) / 2)
        var ux = (flick.contentX + viewportPoint.x - off0x) / z0
        var uy = (flick.contentY + viewportPoint.y - off0y) / z0
        zoom = newZoom
        var img1w = baseWidth * newZoom
        var img1h = baseHeight * newZoom
        var off1x = Math.max(0, (flick.width - img1w) / 2)
        var off1y = Math.max(0, (flick.height - img1h) / 2)
        var holderW = Math.max(flick.width, img1w)
        var holderH = Math.max(flick.height, img1h)
        flick.contentX = Math.max(0, Math.min(holderW - flick.width,
            off1x + ux * newZoom - viewportPoint.x))
        flick.contentY = Math.max(0, Math.min(holderH - flick.height,
            off1y + uy * newZoom - viewportPoint.y))
    }
    function zoomStep(factor) {
        zoomAt(Qt.point(flick.width / 2, flick.height / 2), zoom * factor)
    }
    function fitView() { zoomAt(Qt.point(flick.width / 2, flick.height / 2), 1.0) }
    function actualSize(point) {
        zoomAt(point !== undefined ? point
                                   : Qt.point(flick.width / 2, flick.height / 2),
               actualSizeZoom)
    }

    // Click-to-zoom target: a fixed step rather than 1:1, which on a large
    // photo jumps to a corner. The 0 key gives actual size.
    readonly property real clickZoom: 2.5
    // Also the condition under which the wheel pans instead of zooming.
    readonly property bool zoomedIn: zoom > 1.0 + 0.001

    /// Toggle between fit and `clickZoom`, anchored at `viewportPoint` (flick
    /// viewport coordinates) so the pixel under the pointer stays under it.
    function toggleZoomAt(viewportPoint) {
        if (zoomedIn)
            zoomAt(viewportPoint, 1.0)
        else
            zoomAt(viewportPoint, clickZoom)
    }

    Connections {
        target: app.mediaBridge
        enabled: viewer.opened && viewer.usesBridge
        function onMediaCached(cacheKey) {
            if (cacheKey === viewer.bridgeCacheKey)
                viewer.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
        }
        function onAnimatedMediaReady(cacheKey) {
            if (cacheKey === viewer.bridgeCacheKey && viewer.current !== null)
                viewer.animatedSource =
                    app.mediaBridge.animatedSource(viewer.current.mediaKey, true)
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

    // Parented to the overlay so the image's Flickable never clips it.
    AppMenu {
        id: viewerMenu
        objectName: "imageViewerContextMenu"
        AppMenuItem {
            objectName: "viewerCopyImage"
            iconName: "content_copy"
            text: qsTr("Copy image")
            enabled: viewer.current !== null
                     && (viewer.current.mediaKey || "").length > 0
                     && app.mediaBridge.supported
            onTriggered: app.copyImageToClipboard(viewer.current.mediaKey)
        }
        AppMenuItem {
            objectName: "viewerSaveImage"
            iconName: "download"
            text: qsTr("Save image as…")
            enabled: viewer.current !== null
                     && (viewer.current.mediaKey || "").length > 0
                     && app.mediaBridge.supported
            onTriggered: {
                saveDialog.currentFile = viewer.suggestedSaveUrl()
                saveDialog.open()
            }
        }
        AppMenuSeparator {}
        AppMenuItem {
            iconName: "close"
            text: qsTr("Close")
            onTriggered: viewer.close()
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
        // Dark on every theme: the viewer sits over arbitrary media.
        color: AppTheme.scrimSurface
    }

    contentItem: FocusScope {
        focus: true
        Keys.onLeftPressed: viewer.showAt(viewer.currentIndex - 1)
        Keys.onRightPressed: viewer.showAt(viewer.currentIndex + 1)
        Keys.onPressed: (event) => {
            // Down and Space also mean "next", Up means "previous".
            if (event.key === Qt.Key_Down || event.key === Qt.Key_Space) {
                viewer.showAt(viewer.currentIndex + 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Up) {
                viewer.showAt(viewer.currentIndex - 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
                viewer.zoomStep(1.2)
                event.accepted = true
            } else if (event.key === Qt.Key_Minus) {
                viewer.zoomStep(1 / 1.2)
                event.accepted = true
            } else if (event.key === Qt.Key_0) {
                viewer.actualSize()
                event.accepted = true
            } else if (event.key === Qt.Key_F) {
                viewer.fitView()
                event.accepted = true
            }
        }

        // Clicking the scrim (outside the image) closes.
        TapHandler {
            onTapped: viewer.close()
        }

        // Chrome hides while zoomed in (when it would cover the detail being
        // inspected), not on an idle timer. It stays up with reduced motion,
        // while the toolbar is hovered (so controls don't vanish under the
        // pointer), and after a load failure (Retry lives there). The close
        // button is separate and never hides.
        QtObject {
            id: chrome
            readonly property bool shown:
                !viewer.zoomedIn
                || AppTheme.reducedMotion
                || toolbarHover.hovered
                || viewer.bridgeFailed
        }

        // ── Image area (fills the window; chrome floats above) ──
        Item {
            anchors.fill: parent

            Flickable {
                id: flick
                objectName: "imageViewerFlick"
                anchors.fill: parent
                contentWidth: imageHolder.width
                contentHeight: imageHolder.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Pan 1:1 with no inertia: a coast overshoots the detail being
                // aimed at. maximumFlickVelocity: 0 prevents flicks; the
                // deceleration kills any that start within a frame. Dragging is
                // unaffected.
                maximumFlickVelocity: 0
                flickDeceleration: 100000
                // Pannable only when larger than the viewport, so a fitted
                // image can't be nudged.
                interactive: imageHolder.width > width + 0.5
                             || imageHolder.height > height + 0.5

                Item {
                    id: imageHolder
                    width: Math.max(flick.width, viewer.baseWidth * viewer.zoom)
                    height: Math.max(flick.height, viewer.baseHeight * viewer.zoom)

                    // A click on the picture zooms at that point; a click on
                    // the scrim around it closes. A single tap, so nothing
                    // waits out a double-click interval; Escape and the pinned
                    // close button also close. While zoomed the Flickable takes
                    // drags for panning, so a pan never zooms.
                    TapHandler {
                        id: imageTap
                        gesturePolicy: TapHandler.WithinBounds
                        // One handler with a band check, not a nested
                        // TapHandler: tap handlers are non-exclusive across
                        // subtrees, so a nested pair would both zoom and close.
                        // imageHolder always fills the viewport, so the margin
                        // around a fitted image must be treated as scrim.
                        onTapped: (eventPoint) => {
                            var iw = viewer.baseWidth * viewer.zoom
                            var ih = viewer.baseHeight * viewer.zoom
                            var left = (imageHolder.width - iw) / 2
                            var top = (imageHolder.height - ih) / 2
                            var x = eventPoint.position.x
                            var y = eventPoint.position.y
                            if (iw <= 0 || ih <= 0
                                    || x < left || x > left + iw
                                    || y < top || y > top + ih) {
                                viewer.close()
                                return
                            }
                            var p = imageHolder.mapToItem(flick, x, y)
                            viewer.toggleZoomAt(Qt.point(p.x, p.y))
                        }
                    }
                    // Right-click on the picture opens the context menu. Left
                    // is imageTap's, so the two never both fire.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: (eventPoint) => {
                            var p = imageHolder.mapToItem(
                                Overlay.overlay,
                                eventPoint.position.x, eventPoint.position.y)
                            viewerMenu.popup(Overlay.overlay, p.x, p.y)
                        }
                    }
                    // Grab cursors while pannable.
                    HoverHandler {
                        cursorShape: flick.interactive
                                     ? (flick.dragging ? Qt.ClosedHandCursor
                                                       : Qt.OpenHandCursor)
                                     : Qt.ArrowCursor
                    }

                    // Static image path.
                    Image {
                        id: staticImage
                        // The still is the default and the fallback: it yields
                        // only once the AnimatedImage reports Ready, so
                        // undecodable animations still show a picture.
                        visible: !animatedImage.visible
                        anchors.centerIn: parent
                        width: viewer.baseWidth * viewer.zoom
                        height: viewer.baseHeight * viewer.zoom
                        fillMode: Image.PreserveAspectFit
                        // High-quality scaling at any zoom.
                        smooth: true
                        mipmap: true
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
                        visible: viewer.animateGifs
                                 && viewer.animatedSource.length > 0
                                 && status === AnimatedImage.Ready
                        anchors.centerIn: parent
                        width: viewer.baseWidth * viewer.zoom
                        height: viewer.baseHeight * viewer.zoom
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        cache: false
                        playing: visible && viewer.opened
                        // Not gated on `visible`, which waits for Ready: that
                        // would never load.
                        source: viewer.animateGifs ? viewer.animatedSource : ""
                        onStatusChanged: {
                            if (status === Image.Ready && viewer.baseWidth === 0)
                                viewer.fitImage(implicitWidth, implicitHeight)
                        }
                    }
                }

                WheelHandler {
                    target: null
                    // The wheel pans a zoomed image and zooms a fitted one.
                    // Ctrl+wheel always zooms, so zooming back out by wheel
                    // stays possible once the image is pannable.
                    onWheel: (event) => {
                        var wantsZoom = !flick.interactive
                                || (event.modifiers & Qt.ControlModifier)
                        if (wantsZoom) {
                            const factor = event.angleDelta.y > 0 ? 1.2 : 1 / 1.2
                            viewer.zoomAt(Qt.point(event.x, event.y),
                                          viewer.zoom * factor)
                            return
                        }
                        // pixelDelta comes from trackpads; a notched wheel only
                        // has angleDelta (120 units per notch).
                        var dx = event.pixelDelta.x !== 0
                                 ? event.pixelDelta.x
                                 : event.angleDelta.x / 120 * 60
                        var dy = event.pixelDelta.y !== 0
                                 ? event.pixelDelta.y
                                 : event.angleDelta.y / 120 * 60
                        flick.contentX = Math.max(
                            0, Math.min(flick.contentWidth - flick.width,
                                        flick.contentX - dx))
                        flick.contentY = Math.max(
                            0, Math.min(flick.contentHeight - flick.height,
                                        flick.contentY - dy))
                    }
                }
            }

            // Custom spinner: Basic's BusyIndicator uses palette.dark, which is
            // barely visible on the scrim.
            Item {
                id: viewerSpinner
                anchors.centerIn: parent
                implicitWidth: 34
                implicitHeight: 34
                width: implicitWidth
                height: implicitHeight
                readonly property bool running:
                    viewer.opened && !viewer.bridgeFailed
                    && ((viewer.usesBridge && viewer.bridgeSource === ""
                         && viewer.animatedSource === "")
                        || staticImage.status === Image.Loading
                        || animatedImage.status === Image.Loading)
                visible: running
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 3
                    border.color: AppTheme.scrimSurfaceRaised
                }
                Item {
                    anchors.fill: parent
                    transformOrigin: Item.Center
                    Rectangle {
                        width: 9; height: 9; radius: 4.5
                        color: AppTheme.scrimInk
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: -3
                    }
                    RotationAnimator on rotation {
                        running: viewerSpinner.visible && !AppTheme.reducedMotion
                        from: 0
                        to: 360
                        duration: 900
                        loops: Animation.Infinite
                    }
                }
            }
            ColumnLayout {
                anchors.centerIn: parent
                visible: viewer.bridgeFailed
                         || staticImage.status === Image.Error
                         || animatedImage.status === Image.Error
                spacing: AppTheme.spacing8
                Label {
                    text: qsTr("The image could not be loaded.")
                    color: AppTheme.scrimInk
                }
                AppButton {
                    Layout.alignment: Qt.AlignHCenter
                    // Primary: a secondary button's ink is textPrimary, which
                    // is near-black on light themes and invisible on the scrim.
                    kind: "primary"
                    text: qsTr("Retry")
                    onClicked: viewer.loadCurrent()
                }
            }

            // Previous / next navigation.
            IconButton {
                objectName: "viewerPrevButton"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: AppTheme.spacing12
                // Shown for any set: the list wraps, so there are no ends.
                visible: viewer.entries.length > 1 && chrome.shown
                iconName: "chevron_left"
                iconSize: 26
                implicitWidth: 40; implicitHeight: 40
                iconColorOverride: AppTheme.scrimInk
                background: Rectangle {
                    radius: AppTheme.radiusPill
                    color: AppTheme.scrimBackdrop
                }
                Accessible.name: qsTr("Previous image")
                onClicked: viewer.showAt(viewer.currentIndex - 1)
            }
            IconButton {
                objectName: "viewerNextButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.rightMargin: AppTheme.spacing12
                visible: viewer.entries.length > 1 && chrome.shown
                iconName: "chevron_right"
                iconSize: 26
                implicitWidth: 40; implicitHeight: 40
                iconColorOverride: AppTheme.scrimInk
                background: Rectangle {
                    radius: AppTheme.radiusPill
                    color: AppTheme.scrimBackdrop
                }
                Accessible.name: qsTr("Next image")
                onClicked: viewer.showAt(viewer.currentIndex + 1)
            }
        }

        // ── Header: filename + sender/time (floats over the image) ──
        RowLayout {
            id: headerRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing12
            // Clear of the pinned close button (36 px plus margin).
            anchors.rightMargin: AppTheme.spacing12 + 36 + AppTheme.spacing8
            spacing: AppTheme.spacing8
            visible: opacity > 0
            opacity: chrome.shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    // Remote or externally chosen text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: viewer.current !== null ? viewer.current.filename : ""
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideMiddle
                }
                Label {
                    // Remote or externally chosen text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    // Guarded on the sender: openFor()'s miss branch knows only
                    // the media key, and "· 1 Jan 1970" is worse than blank.
                    text: viewer.current !== null
                          && (viewer.current.sender || "").length > 0
                          ? qsTr("%1 · %2")
                                .arg(viewer.current.sender)
                                .arg(Qt.formatDateTime(viewer.current.timestamp,
                                                       "d MMM yyyy hh:mm"))
                          : ""
                    color: AppTheme.scrimInkMuted
                    font.pixelSize: AppTheme.textMeta
                    elide: Label.ElideRight
                }
            }
        }

        // Pinned: not part of `chrome.shown`. Once zoomed, the hidden chrome
        // can't be hovered back and every click on the picture zooms, so a
        // hiding close button would leave pointer users without a way out.
        IconButton {
            objectName: "viewerCloseButton"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing12
            iconName: "close"
            iconSize: 20
            implicitWidth: 36; implicitHeight: 36
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("Close image viewer")
            onClicked: viewer.close()
        }

        // ── Thumbnail strip ──
        // Only for a real set. Uses `list_thumb`, the smallest cached kind, so
        // opening doesn't pull full payloads for 48 px squares.
        ListView {
            id: thumbStrip
            objectName: "viewerThumbnailStrip"
            orientation: ListView.Horizontal
            model: viewer.entries
            spacing: 4
            clip: true
            height: 48
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: toolbar.top
            anchors.bottomMargin: AppTheme.spacing12

            // Sized to a whole number of cells so the end tiles aren't sliced
            // by the clip (which is needed so the strip doesn't paint over the
            // image). SnapToItem keeps that true after a flick.
            readonly property int cell: 48 + spacing
            width: {
                var avail = parent.width - AppTheme.spacing16 * 2
                var fit = Math.max(1, Math.floor((avail + spacing) / cell))
                // From the entry count, not contentWidth: contentWidth depends
                // on width, which would be a binding loop. Cells are a fixed 48
                // px.
                var all = viewer.entries.length * cell - spacing
                // The last item carries no trailing spacing.
                return Math.min(all, fit * cell - spacing)
            }
            snapMode: ListView.SnapToItem
            visible: opacity > 0
            opacity: (viewer.entries.length > 1 && chrome.shown) ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            // Keep the current thumbnail visible; `Contain` scrolls only when
            // it falls off an edge.
            onCurrentIndexChanged: positionViewAtIndex(currentIndex,
                                                       ListView.Contain)
            currentIndex: viewer.currentIndex

            // Swallows taps in the gaps between thumbnails, which would
            // otherwise reach the scrim and close. WithinBounds takes the
            // exclusive grab; the default DragThreshold only grabs passively,
            // so the scrim still fired.
            TapHandler {
                gesturePolicy: TapHandler.WithinBounds
                onTapped: {}
            }

            delegate: Item {
                id: thumbItem
                required property int index
                required property var modelData
                width: 48
                height: 48
                readonly property bool isCurrent: index === viewer.currentIndex

                Image {
                    id: thumbImage
                    anchors.fill: parent
                    // mediaSource() answers a miss with "" and fetches; the
                    // bytes arrive later via mediaCached. resolveTick
                    // re-evaluates the binding then (as in EmojiPicker and
                    // EmojiCompletionPopup).
                    property int resolveTick: 0
                    source: {
                        var _tick = resolveTick
                        return (modelData.mediaKey || "").length > 0
                               && app.mediaBridge.supported
                            ? app.mediaBridge.mediaSource(modelData.mediaKey,
                                                          "list_thumb")
                            : (modelData.httpUrl || "")
                    }
                    Connections {
                        target: app.mediaBridge
                        function onMediaCached(cacheKey) {
                            // Any class: mediaSource reads through to a larger
                            // cached class, so a `full:` arrival also answers.
                            if ((thumbItem.modelData.mediaKey || "").length > 0
                                && cacheKey.endsWith(
                                       ":" + thumbItem.modelData.mediaKey))
                                thumbImage.resolveTick++
                        }
                    }
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    smooth: true
                    mipmap: true
                }
                // Non-current items recede; hover brings them most of the way
                // back.
                Rectangle {
                    anchors.fill: parent
                    color: AppTheme.scrimBackdrop
                    opacity: thumbItem.isCurrent
                             ? 0.0 : (thumbHover.hovered ? 0.15 : 0.5)
                    Behavior on opacity {
                        enabled: !AppTheme.reducedMotion
                        NumberAnimation { duration: 120 }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: thumbItem.isCurrent ? 2 : 0
                    border.color: AppTheme.scrimInk
                }
                // WithinBounds takes the exclusive grab so the scrim's close
                // handler doesn't also fire (as with imageTap).
                HoverHandler {
                    id: thumbHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    gesturePolicy: TapHandler.WithinBounds
                    onTapped: viewer.showAt(thumbItem.index)
                }

                Accessible.role: Accessible.Button
                Accessible.name: thumbItem.isCurrent
                    ? qsTr("Image %1 of %2, shown")
                          .arg(thumbItem.index + 1).arg(viewer.entries.length)
                    : qsTr("Image %1 of %2")
                          .arg(thumbItem.index + 1).arg(viewer.entries.length)
                Accessible.onPressAction: viewer.showAt(thumbItem.index)
            }
        }

        // ── Floating toolbar: zoom cluster + actions ──
        Rectangle {
            id: toolbar
            objectName: "viewerToolbar"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: AppTheme.spacing16
            width: toolbarRow.implicitWidth + AppTheme.spacing12 * 2
            height: 44
            radius: AppTheme.radiusPill
            color: AppTheme.scrimSurface
            border.width: 1
            border.color: AppTheme.scrimBorder
            visible: opacity > 0
            opacity: chrome.shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            HoverHandler { id: toolbarHover }

            RowLayout {
                id: toolbarRow
                anchors.centerIn: parent
                spacing: 2
                IconButton {
                    objectName: "viewerZoomOutButton"
                    iconName: "zoom_out"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Zoom out")
                    onClicked: viewer.zoomStep(1 / 1.2)
                }
                Label {
                    objectName: "viewerZoomLabel"
                    text: viewer.percentZoom + "%"
                    color: AppTheme.scrimInkStrong
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                    horizontalAlignment: Text.AlignHCenter
                    Layout.minimumWidth: 44
                }
                IconButton {
                    objectName: "viewerZoomInButton"
                    iconName: "zoom_in"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Zoom in")
                    onClicked: viewer.zoomStep(1.2)
                }
                Rectangle {
                    width: 1; height: 22
                    color: AppTheme.scrimBorder
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                }
                IconButton {
                    objectName: "viewerFitButton"
                    iconName: "fit_screen"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Fit to window")
                    ToolTip.text: qsTr("Fit to window (F)")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: viewer.fitView()
                }
                AbstractButton {
                    id: actualSizeButton
                    objectName: "viewerActualSizeButton"
                    implicitWidth: 34; implicitHeight: 32
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Actual size")
                    ToolTip.text: qsTr("Actual size (0)")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: viewer.actualSize()
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: actualSizeButton.hovered
                               ? AppTheme.scrimSurfaceRaised : "transparent"
                        border.width: actualSizeButton.visualFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                    }
                    contentItem: Label {
                        text: "1:1"
                        color: AppTheme.scrimInkStrong
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Rectangle {
                    width: 1; height: 22
                    color: AppTheme.scrimBorder
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    visible: viewer.usesBridge
                }
                IconButton {
                    objectName: "viewerSaveButton"
                    visible: viewer.usesBridge
                    iconName: "download"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Save image as…")
                    onClicked: {
                        if (viewer.current !== null) {
                            saveDialog.currentFile = viewer.suggestedSaveUrl()
                            saveDialog.open()
                        }
                    }
                }
            }
        }

        // ── Footer: save feedback + position ──
        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: AppTheme.spacing8
            visible: opacity > 0
            opacity: chrome.shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            Label {
                id: saveNotice
                property bool ok: true
                visible: text.length > 0
                color: ok ? AppTheme.success : AppTheme.danger
                font.pixelSize: AppTheme.textMeta
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
                color: AppTheme.scrimInkMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
    }
}
