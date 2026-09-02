import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// In-app image viewer. Full-window scrim, pointer-centered wheel zoom,
// drag panning with grab cursors, double-click fit/actual toggling,
// previous/next across the images currently loaded in the room timeline
// (no history is fetched), Save As through the media bridge, animated GIF
// playback, and a floating shared-control toolbar that fades while idle.
// Escape or a click on the scrim closes. The SDK timeline is untouched —
// this is a pure overlay.
Popup {
    id: viewer

    // Sender-chosen filename -> hardened leaf, percent-encoded. See
    // TimelinePane.suggestedSaveUrl for why the raw concatenation was a
    // one-click write to a sender-chosen directory.
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
    closePolicy: Popup.CloseOnEscape

    // Entries from TimelineModel::imageEntries(); each has
    // {row, mediaKey, filename, sender, timestamp, mime, httpUrl}.
    property var entries: []
    property int currentIndex: -1
    readonly property var current:
        (currentIndex >= 0 && currentIndex < entries.length)
        ? entries[currentIndex] : null

    // Zoom model: `zoom` is relative to the fit-to-window base size, so
    // 1.0 always means "fits the viewport". `naturalWidth/Height` are the
    // decoded dimensions; actualSizeZoom shows one image pixel per logical
    // pixel.
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
    // The viewer is explicit user intent: GIFs animate unless autoplay is
    // globally Never (2). Matches the timeline's tri-state policy instead
    // of the legacy boolean.
    readonly property bool animateGifs: app.settings.gifAutoplay !== 2

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
        chrome.wake()
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
        if (isGif && animateGifs)
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
        chrome.wake()
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
    // Window resizes keep the image sensibly placed: the fit base follows
    // the new viewport while the relative zoom is preserved.
    onWidthChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)
    onHeightChanged: if (opened && naturalWidth > 0) fitImage(naturalWidth, naturalHeight)

    // Pointer-centered zoom: the image point under `viewportPoint` (in
    // flick viewport coordinates) stays put across the scale change.
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

    // Popped into the overlay, not into the viewer, so it is never clipped
    // by the image's own Flickable.
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
        // The viewer chrome is deliberately dark on EVERY theme — it sits
        // over arbitrary user media, so it cannot follow the palette. That
        // is what the scrim* tokens are for; this file used to hardcode
        // nine different values for the role, including two of Lightning
        // Dark's own text inks copied out of AppTheme by value.
        color: AppTheme.scrimSurface
    }

    contentItem: FocusScope {
        focus: true
        Keys.onLeftPressed: viewer.showAt(viewer.currentIndex - 1)
        Keys.onRightPressed: viewer.showAt(viewer.currentIndex + 1)
        Keys.onPressed: (event) => {
            chrome.wake()
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
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

        // Toolbar/chrome auto-hide: fades after idle while the pointer is
        // still; wakes on movement, keys, navigation, errors, and while
        // any control is hovered. Reduced motion keeps chrome always on.
        QtObject {
            id: chrome
            property bool shown: true
            function wake() {
                shown = true
                if (!AppTheme.reducedMotion)
                    idleTimer.restart()
            }
        }
        Timer {
            id: idleTimer
            interval: 2600
            onTriggered: {
                if (!AppTheme.reducedMotion && !toolbarHover.hovered
                        && !viewer.bridgeFailed)
                    chrome.shown = false
            }
        }
        HoverHandler {
            // Any pointer movement over the viewer wakes the chrome.
            onPointChanged: chrome.wake()
        }

        // ── Image area (fills the window; chrome floats above) ───────────
        Item {
            anchors.fill: parent

            Flickable {
                id: flick
                anchors.fill: parent
                contentWidth: imageHolder.width
                contentHeight: imageHolder.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Panning only when zoomed beyond the viewport — otherwise
                // drags cannot accidentally nudge a fitted image.
                interactive: imageHolder.width > width + 0.5
                             || imageHolder.height > height + 0.5

                Item {
                    id: imageHolder
                    width: Math.max(flick.width, viewer.baseWidth * viewer.zoom)
                    height: Math.max(flick.height, viewer.baseHeight * viewer.zoom)

                    // A click ANYWHERE — image included — closes the viewer
                    // IMMEDIATELY (live feedback twice over: closing must
                    // never require the X button, and must never wait out
                    // the platform's ~400ms double-click interval, which an
                    // exclusive single/double-tap split forces). Double-
                    // click zoom was dropped for that instant close: the
                    // wheel, the +/- toolbar buttons, and the +/-/0/F keys
                    // all still zoom. While zoomed, the Flickable steals
                    // drags for panning before they can count as taps.
                    TapHandler {
                        id: imageTap
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: viewer.close()
                    }
                    // Right-click the picture itself for the actions a person
                    // expects there — asked for in those words, "same as in
                    // Discord". LeftButton is imageTap's default, so the two
                    // handlers cannot both fire on one press.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: (eventPoint) => {
                            chrome.wake()
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
                        visible: !viewer.isGif || !viewer.animateGifs
                        anchors.centerIn: parent
                        width: viewer.baseWidth * viewer.zoom
                        height: viewer.baseHeight * viewer.zoom
                        fillMode: Image.PreserveAspectFit
                        // High-quality scaling at any zoom without a
                        // permanent re-rasterization of the source.
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
                        visible: viewer.isGif && viewer.animateGifs
                                 && viewer.animatedSource.length > 0
                        anchors.centerIn: parent
                        width: viewer.baseWidth * viewer.zoom
                        height: viewer.baseHeight * viewer.zoom
                        fillMode: Image.PreserveAspectFit
                        smooth: true
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
                    // Pointer-centered wheel zoom: the image point under
                    // the cursor stays fixed.
                    onWheel: (event) => {
                        chrome.wake()
                        const factor = event.angleDelta.y > 0 ? 1.2 : 1 / 1.2
                        viewer.zoomAt(Qt.point(event.x, event.y),
                                      viewer.zoom * factor)
                    }
                }
            }

            // Basic's BusyIndicator inks palette.dark — the theme's
            // secondary TEXT colour — which on an 85%-black scrim is
            // barely perceptible. A ring with a travelling head, in the
            // scrim ink, so loading actually reads as loading.
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
                    // Primary, not secondary: a secondary button's ink is
                    // the theme's textPrimary, which on a light theme is
                    // near-black — invisible on this scrim.
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
                visible: viewer.currentIndex > 0 && chrome.shown
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
                visible: viewer.currentIndex < viewer.entries.length - 1
                         && chrome.shown
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

        // ── Header: filename + sender/time (floats over the image) ───────
        RowLayout {
            id: headerRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing12
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
                    text: viewer.current !== null
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
            IconButton {
                objectName: "viewerCloseButton"
                iconName: "close"
                iconSize: 20
                implicitWidth: 36; implicitHeight: 36
                iconColorOverride: AppTheme.scrimInk
                Accessible.name: qsTr("Close image viewer")
                onClicked: viewer.close()
            }
        }

        // ── Floating toolbar: zoom cluster + actions ─────────────────────
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

        // ── Footer: save feedback + position ─────────────────────────────
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
