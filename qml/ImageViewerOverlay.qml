import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Popup {
    id: viewer

    // Sender-chosen filename -> hardened, percent-encoded leaf (see
    // TimelinePane.suggestedSaveUrl).
    // Element's Download (straight to the downloads folder unless the user
    // asked to always choose), or Save as… (`ask`). app.downloads sanitizes
    // the sender's name and fixes its extension from the type.
    function saveCurrent(ask) {
        if (viewer.current === null || !app.downloads)
            return
        var key = viewer.current.mediaKey || ""
        if (key.length === 0)
            return
        var name = viewer.current.filename || ""
        var mime = viewer.current.mime || ""
        if (ask)
            app.downloads.saveAs(key, name, mime)
        else
            app.downloads.download(key, name, mime)
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

    property var entries: []
    property int currentIndex: -1
    readonly property var current:
        (currentIndex >= 0 && currentIndex < entries.length)
        ? entries[currentIndex] : null

    property real zoom: 1.0
    readonly property real minZoom: 0.1
    readonly property real maxZoom: 10.0
    readonly property string currentLinkUrl:
        current !== null ? (current.linkUrl || "") : ""
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

    readonly property bool usesBridge:
        current !== null && current.mediaKey.length > 0 && app.mediaBridge.supported
    readonly property string bridgeCacheKey:
        current !== null ? ("full:" + current.mediaKey) : ""
    property string bridgeSource: ""
    property string animatedSource: ""
    property bool bridgeFailed: false
    readonly property string currentMime:
        current !== null ? (current.mime || "").toLowerCase() : ""
    readonly property bool maybeAnimated:
        current !== null && (currentMime === "" || currentMime === "image/gif"
                             || currentMime === "image/webp")
    // The viewer is explicit intent: animations play unless Animated images is
    // Don't play (MotionPolicy), Reduce motion and Play on hover included.
    readonly property bool animateGifs: MotionPolicy.allowed

    function openAt(list, index) {
        if (!list || index < 0 || index >= list.length)
            return
        entries = list
        currentIndex = index
        resetView()
        open()
        loadCurrent()
    }

    function openFor(mediaKey, httpUrl) {
        if (mediaKey.indexOf("link:") === 0) {
            var entry = app.linkPreviews.viewerEntry(mediaKey)
            if (!entry || !entry.url)
                return
            openAt([{
                "row": -1,
                "mediaKey": mediaKey,
                "filename": entry.fileName || entry.host || "",
                "sender": "",
                "timestamp": undefined,
                "mime": entry.mime || "",
                "httpUrl": "",
                "linkUrl": entry.url,
                "linkHost": entry.host || "",
                "isImage": true,
                "isVideo": false,
                "isVisual": true,
                "thumbAvailable": false,
                "size": 0
            }], 0)
            return
        }
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

    function openArtwork(source, title) {
        if (!source || String(source).length === 0)
            return
        openAt([{
            "row": -1,
            "mediaKey": "",
            "filename": title || "",
            "sender": "",
            "timestamp": undefined,
            "mime": "",
            "httpUrl": source,
            "isImage": true,
            "isVideo": false,
            "isVisual": true,
            "thumbAvailable": false,
            "size": 0
        }], 0)
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
        if (maybeAnimated && animateGifs)
            animatedSource = app.mediaBridge.animatedSource(current.mediaKey, true)
        bridgeSource = app.mediaBridge.mediaSource(current.mediaKey, "full")
    }

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

    function pageStep() {
        return Math.max(1, Math.floor(thumbStrip.width / thumbStrip.cell))
    }
    function showClamped(index) {
        if (entries.length === 0)
            return
        showAt(Math.max(0, Math.min(entries.length - 1, index)))
    }

    function fitImage(w, h) {
        if (w <= 0 || h <= 0) return
        naturalWidth = w
        naturalHeight = h
        var availW = viewer.width - 32
        var footer = toolbar.height + AppTheme.spacing16 + AppTheme.spacing8
                     + (viewer.entries.length > 1
                        ? thumbStrip.height + AppTheme.spacing12 : 0)
        var availH = Math.max(120, viewer.height - 2 * Math.max(60, footer))
        var scale = Math.min(1.0, Math.min(availW / w, availH / h))
        baseWidth = w * scale
        baseHeight = h * scale
    }
    onWidthChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)
    onHeightChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)

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

    readonly property real clickZoom: 2.5
    readonly property bool zoomedIn: zoom > 1.0 + 0.001

    function toggleZoomAt(viewportPoint) {
        if (zoomedIn)
            zoomAt(viewportPoint, 1.0)
        else
            zoomAt(viewportPoint, clickZoom)
    }

    onOpened: {
        if (viewer.usesBridge && viewer.bridgeSource === "")
            viewer.loadCurrent()
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
        function onSaveFinished(ok, message, mediaKey) {
            if (viewer.opened && viewer.current !== null
                    && mediaKey === (viewer.current.mediaKey || "")) {
                saveNotice.text = message
                saveNotice.ok = ok
                saveNoticeTimer.restart()
            }
        }
    }

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
            onTriggered: viewer.saveCurrent(true)
        }
        AppMenuItem {
            objectName: "viewerOpenLinkInBrowser"
            iconName: "link"
            text: qsTr("Open in browser")
            visible: viewer.currentLinkUrl.length > 0
            height: visible ? implicitHeight : 0
            onTriggered: app.media.openWebUrl(viewer.currentLinkUrl)
        }
        AppMenuSeparator {}
        AppMenuItem {
            iconName: "close"
            text: qsTr("Close")
            onTriggered: viewer.close()
        }
    }

    background: Rectangle {
        color: AppTheme.scrimSurface
    }

    contentItem: FocusScope {
        focus: true
        Keys.onLeftPressed: viewer.showAt(viewer.currentIndex - 1)
        Keys.onRightPressed: viewer.showAt(viewer.currentIndex + 1)
        Keys.onPressed: (event) => {
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
            } else if (event.key === Qt.Key_Home) {
                viewer.showAt(0)
                event.accepted = true
            } else if (event.key === Qt.Key_End) {
                viewer.showAt(viewer.entries.length - 1)
                event.accepted = true
            } else if (event.key === Qt.Key_PageDown) {
                viewer.showClamped(viewer.currentIndex + viewer.pageStep())
                event.accepted = true
            } else if (event.key === Qt.Key_PageUp) {
                viewer.showClamped(viewer.currentIndex - viewer.pageStep())
                event.accepted = true
            } else if (event.key === Qt.Key_Menu
                       || (event.key === Qt.Key_F10
                           && (event.modifiers & Qt.ShiftModifier))) {
                viewerMenu.popup(Overlay.overlay,
                                 viewer.width / 2, viewer.height / 2)
                event.accepted = true
            }
        }

        TapHandler {
            onTapped: viewer.close()
        }

        QtObject {
            id: chrome
            readonly property bool shown:
                !viewer.zoomedIn
                || AppTheme.reducedMotion
                || toolbarHover.hovered
                || viewer.bridgeFailed
        }

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
                interactive: imageHolder.width > width + 0.5
                             || imageHolder.height > height + 0.5

                Item {
                    id: imageHolder
                    width: Math.max(flick.width, viewer.baseWidth * viewer.zoom)
                    height: Math.max(flick.height, viewer.baseHeight * viewer.zoom)

                    TapHandler {
                        id: imageTap
                        gesturePolicy: TapHandler.WithinBounds
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
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: (eventPoint) => {
                            var p = imageHolder.mapToItem(
                                Overlay.overlay,
                                eventPoint.position.x, eventPoint.position.y)
                            viewerMenu.popup(Overlay.overlay, p.x, p.y)
                        }
                    }
                    HoverHandler {
                        cursorShape: flick.interactive
                                     ? (flick.dragging ? Qt.ClosedHandCursor
                                                       : Qt.OpenHandCursor)
                                     : Qt.ArrowCursor
                    }

                    Image {
                        id: staticImage
                        objectName: "viewerStaticImage"
                        visible: !animatedImage.visible
                        anchors.centerIn: parent
                        width: viewer.baseWidth * viewer.zoom
                        height: viewer.baseHeight * viewer.zoom
                        fillMode: Image.PreserveAspectFit
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
                                 && !MotionPolicy.suspended
                        source: viewer.animateGifs ? viewer.animatedSource : ""
                        // Keeps the bridge from evicting the file mid-play.
                        AnimationFileHold {}
                        onStatusChanged: {
                            if (status === Image.Ready && viewer.baseWidth === 0)
                                viewer.fitImage(implicitWidth, implicitHeight)
                        }
                    }
                }

                WheelHandler {
                    target: null
                    onWheel: (event) => {
                        var wantsZoom = !flick.interactive
                                || (event.modifiers & Qt.ControlModifier)
                        if (wantsZoom) {
                            const factor = event.angleDelta.y > 0 ? 1.2 : 1 / 1.2
                            viewer.zoomAt(Qt.point(event.x, event.y),
                                          viewer.zoom * factor)
                            return
                        }
                        var dx = event.pixelDelta.x !== 0
                                 ? event.pixelDelta.x
                                 : event.angleDelta.x / 120 * 60
                        var dy = event.pixelDelta.y !== 0
                                 ? event.pixelDelta.y
                                 : event.angleDelta.y / 120 * 60
                        if ((event.modifiers & Qt.ShiftModifier)
                                && dx === 0
                                && flick.contentWidth > flick.width + 1) {
                            dx = dy
                            dy = 0
                        }
                        flick.contentX = Math.max(
                            0, Math.min(flick.contentWidth - flick.width,
                                        flick.contentX - dx))
                        flick.contentY = Math.max(
                            0, Math.min(flick.contentHeight - flick.height,
                                        flick.contentY - dy))
                    }
                }
            }

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
                    kind: "primary"
                    text: qsTr("Retry")
                    onClicked: viewer.loadCurrent()
                }
            }

            IconButton {
                objectName: "viewerPrevButton"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: AppTheme.spacing12
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

        RowLayout {
            id: headerRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing12
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
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: viewer.current !== null ? viewer.current.filename : ""
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideMiddle
                }
                Label {
                    objectName: "viewerSubtitle"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: viewer.current !== null
                          && (viewer.current.sender || "").length > 0
                          ? qsTr("%1 · %2")
                                .arg(viewer.current.sender)
                                .arg(Qt.locale(AppLocale.name).toString(viewer.current.timestamp,
                                                       "d MMM yyyy hh:mm"))
                          : viewer.current !== null
                            ? (viewer.current.linkHost || "") : ""
                    color: AppTheme.scrimInkMuted
                    font.pixelSize: AppTheme.textMeta
                    elide: Label.ElideRight
                }
            }
        }

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

            readonly property int cell: 48 + spacing
            width: {
                var avail = parent.width - AppTheme.spacing16 * 2
                var fit = Math.max(1, Math.floor((avail + spacing) / cell))
                var all = viewer.entries.length * cell - spacing
                return Math.min(all, fit * cell - spacing)
            }
            snapMode: ListView.SnapToItem
            visible: opacity > 0
            opacity: (viewer.entries.length > 1 && chrome.shown) ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            onCurrentIndexChanged: { userScrolled = false; ensureCurrentVisible() }
            currentIndex: viewer.currentIndex
            property bool userScrolled: false
            onMovementStarted: userScrolled = true
            onWidthChanged: if (!userScrolled) ensureCurrentVisible()
            onCountChanged: if (!userScrolled) ensureCurrentVisible()
            function ensureCurrentVisible() {
                if (count > 0 && currentIndex >= 0)
                    positionViewAtIndex(currentIndex, ListView.Contain)
            }
            Connections {
                target: viewer
                function onOpened() {
                    thumbStrip.userScrolled = false
                    thumbStrip.ensureCurrentVisible()
                }
            }

            WheelHandler {
                objectName: "viewerThumbnailWheel"
                target: null
                acceptedModifiers: Qt.NoModifier
                property real residual: 0
                onWheel: (event) => {
                    var px = event.pixelDelta.x !== 0 ? event.pixelDelta.x
                                                      : event.pixelDelta.y
                    var d = event.angleDelta.x !== 0 ? event.angleDelta.x
                                                     : event.angleDelta.y
                    var cells
                    if (px !== 0) {
                        residual += px
                        cells = Math.trunc(residual / thumbStrip.cell)
                        residual -= cells * thumbStrip.cell
                    } else {
                        cells = Math.round(d / 120)
                    }
                    var maxX = thumbStrip.contentWidth - thumbStrip.width
                               + thumbStrip.originX
                    var base = Math.round((thumbStrip.contentX
                                           - thumbStrip.originX)
                                          / thumbStrip.cell) * thumbStrip.cell
                                + thumbStrip.originX
                    thumbStrip.contentX = Math.max(
                        thumbStrip.originX,
                        Math.min(maxX, base - cells * thumbStrip.cell))
                    thumbStrip.userScrolled = true
                    event.accepted = true
                }
            }

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
                    ToolTip.text: qsTr("Zoom out (−)")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
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
                    ToolTip.text: qsTr("Zoom in (+)")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
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
                    objectName: "viewerOpenInBrowserButton"
                    visible: viewer.currentLinkUrl.length > 0
                    iconName: "link"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Open in browser")
                    ToolTip.text: qsTr("Open in browser")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: app.media.openWebUrl(viewer.currentLinkUrl)
                }
                IconButton {
                    objectName: "viewerSaveButton"
                    visible: viewer.usesBridge
                    iconName: "download"
                    iconSize: 20
                    implicitWidth: 32; implicitHeight: 32
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Download image")
                    ToolTip.text: qsTr("Download")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: viewer.saveCurrent(false)
                }
            }
        }

        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: AppTheme.spacing8
            visible: opacity > 0
            opacity: chrome.shown || saveNotice.text.length > 0 ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            Label {
                id: saveNotice
                objectName: "imageViewerSaveNotice"
                property bool ok: true
                visible: text.length > 0
                color: ok ? AppTheme.scrimSuccessInk : AppTheme.scrimDangerInk
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
