import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// In-app image viewer. Full-window scrim, click-to-zoom at the pointer,
// wheel zoom while fitted and wheel PAN once zoomed, drag panning with grab
// cursors, previous/next across the images currently loaded in the room
// timeline (no history is fetched, and the list WRAPS at both ends), a
// thumbnail strip, Save As through the media bridge, animated GIF playback,
// and floating chrome that gets out of the way while zoomed. Escape or a
// click on the scrim closes. The SDK timeline is untouched — this is a pure
// overlay.
//
// The gesture model follows the one most people arrive here with (asked for
// in those words): click the picture to zoom at the point you clicked, wheel
// to pan once there is somewhere to pan to. Where that model is POORER than
// what this viewer already had it was NOT adopted — zoom stays continuous
// over 0.1x-10x rather than a single fixed step, and the +/-/0/F keys stay,
// because a viewer with no keyboard zoom is worse for the same reason a
// viewer with no keyboard navigation would be.
//
// The header comment used to claim "double-click fit/actual toggling" and
// "a toolbar that fades while idle". Neither was true: the double-click
// handler had already been removed and nothing replaced the claim, and the
// idle fade is now a zoom-driven hide. A stale header is worse than none —
// it is read as an inventory.
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
    // A POPUP DOES NOT TAKE FOCUS UNLESS IT ASKS FOR IT, and this one did
    // not — so every key the viewer declares was dead: Left/Right,
    // Up/Down/Space, +/-/0/F, and Escape through the close policy below.
    // `contentItem: FocusScope { focus: true }` cannot rescue it; a focus
    // scope inside a popup that never becomes the active focus item never
    // becomes one either. VideoViewerOverlay, written to the same pattern,
    // has always set this.
    //
    // Escape is the half that matters most: taking click-to-close off the
    // picture was justified by "closing is still instant everywhere else —
    // the scrim, Escape, the close button", and Escape was not one of them.
    focus: true
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
    readonly property string currentMime:
        current !== null ? (current.mime || "").toLowerCase() : ""
    // NOT a mimetype test any more, and the difference is the whole point.
    // An `m.sticker`'s `info.mimetype` is optional under MSC2545, so a GIF
    // sticker routinely arrives with no declared type at all and used to
    // open here as a frozen frame. The BYTES decide: this only says "worth
    // asking about", and MediaBridge.animatedExtensionFor answers from the
    // container magic. A payload that says it is a PNG or a JPEG is taken at
    // its word for the sole purpose of not asking.
    readonly property bool maybeAnimated:
        current !== null && (currentMime === "" || currentMime === "image/gif"
                             || currentMime === "image/webp")
    // The viewer is explicit user intent: animations play unless autoplay is
    // globally Never (2). Matches the timeline's tri-state policy instead
    // of the legacy boolean.
    readonly property bool animateGifs: app.settings.gifAutoplay !== 2

    /// Open on an EXPLICIT list and index — for a caller that already knows
    /// both, which is what the room's Media tab is.
    ///
    /// The viewer pages through the list it is GIVEN. It used to build the
    /// list itself, always from `app.timeline.imageEntries()`, which is only
    /// the right list for a click in the timeline.
    function openAt(list, index) {
        if (!list || index < 0 || index >= list.length)
            return
        entries = list
        currentIndex = index
        resetView()
        open()
        loadCurrent()
    }

    /// Open on the images the TIMELINE has loaded, located by media key (or
    /// by URL on the HTTP backend). This is the timeline's own entry point.
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
            // A MISS OPENS WHAT WAS ASKED FOR, ALONE. This used to be
            // `currentIndex = entries.length - 1`, which turned "I could not
            // find that" into "here is something else" — the user clicked one
            // picture and got another. There is nothing to page through when
            // the row is not in the loaded list, and that is a truthful state.
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
        // BOTH, always. They share one cache key ("full:"), so this is one
        // fetch; the still frame is what is drawn until — and unless — the
        // animation is validated and decodable. Asking for the animation is
        // SPECULATIVE (see MediaBridge::animatedSource): a payload that is
        // not one answers with silence rather than marking the key failed,
        // which would put an error card over a perfectly good picture.
        if (maybeAnimated && animateGifs)
            animatedSource = app.mediaBridge.animatedSource(current.mediaKey, true)
        bridgeSource = app.mediaBridge.mediaSource(current.mediaKey, "full")
    }

    // WRAPS at both ends: next on the last image is the first, previous on
    // the first is the last. This used to return early at the bounds, so the
    // one gesture a person repeats — tap next, tap next, tap next — simply
    // stopped, with an arrow that vanished rather than a list that came
    // round. Wrapping makes the set feel like a set.
    //
    // The modulo is written twice on purpose: JavaScript's `%` keeps the
    // sign of the dividend, so `-1 % 5` is `-1`, not `4`.
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

    // What a click on the picture jumps to. A fixed step rather than actual
    // size, because actual size on a 6000px photo is a jump to one corner of
    // it, and the gesture people expect here is "closer", not "1:1" — which
    // the 0 key still gives.
    readonly property real clickZoom: 2.5
    // True once there is somewhere to pan to, which is also the condition
    // under which the wheel stops zooming and starts panning.
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
            // Down and Space join Right as "next", Up joins Left as
            // "previous". One axis is not enough: a person who has just
            // scrolled a list reaches for Down, and Space is the oldest
            // "advance" key there is.
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

        // CHROME GETS OUT OF THE WAY WHILE ZOOMED, NOT ON A TIMER.
        //
        // This was a 2.6s idle fade, and an idle timer is the wrong question:
        // it hides the controls from someone who is reading a picture and
        // has simply stopped moving the mouse, and it keeps them over the
        // picture during the one activity they actually obstruct — zooming
        // in to look at detail underneath them. Tying it to zoom answers the
        // real question, "is the user inspecting the image right now", and
        // it needs no timer, no wake calls and no pointer tracking.
        //
        // Three carve-outs, each load-bearing:
        //   * reduced motion keeps chrome permanently on, as it did before —
        //     an interface that appears and disappears IS motion;
        //   * hovering the toolbar keeps it up, so a control cannot vanish
        //     from under the pointer on its way to being clicked;
        //   * a load failure keeps it up, because the Retry button lives
        //     there and a viewer that hid it would be a dead end.
        //
        // The close button is deliberately NOT part of this — it is anchored
        // separately and never fades; see its own note. A way out must never
        // be conditional, and this gate is not recoverable by pointer the way
        // the idle fade it replaced was.
        QtObject {
            id: chrome
            readonly property bool shown:
                !viewer.zoomedIn
                || AppTheme.reducedMotion
                || toolbarHover.hovered
                || viewer.bridgeFailed
        }

        // ── Image area (fills the window; chrome floats above) ───────────
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
                // THE IMAGE TRACKS THE POINTER AND STOPS DEAD. No glide.
                //
                // A Flickable flicks by default — Qt's own deceleration is
                // ~1500 px/s², so letting go mid-drag sent the picture
                // coasting. That is right for a list, where the content is
                // long and the gesture is "throw me further", and wrong for
                // inspecting an image, where the gesture is "put this part
                // under my eyes" and any coast overshoots the thing you were
                // aiming at. The model this viewer follows pans 1:1 with no
                // inertia at all, which is the behaviour asked for.
                //
                // Both properties, because they close different doors:
                // `maximumFlickVelocity: 0` refuses to start a flick, and the
                // deceleration makes any flick that does start decay inside a
                // single frame. Dragging is untouched — only the release.
                maximumFlickVelocity: 0
                flickDeceleration: 100000
                // Panning only when zoomed beyond the viewport — otherwise
                // drags cannot accidentally nudge a fitted image.
                interactive: imageHolder.width > width + 0.5
                             || imageHolder.height > height + 0.5

                Item {
                    id: imageHolder
                    width: Math.max(flick.width, viewer.baseWidth * viewer.zoom)
                    height: Math.max(flick.height, viewer.baseHeight * viewer.zoom)

                    // A click on the PICTURE zooms at the point clicked; a
                    // click anywhere else — the scrim around it — still
                    // closes, instantly.
                    //
                    // THIS REPLACED AN INSTANT CLOSE ON THE PICTURE ITSELF,
                    // and that was a deliberate decision made on live
                    // feedback: closing must never require the X button, and
                    // must never wait out the platform's ~400ms double-click
                    // interval, which is why the double-click zoom that used
                    // to live here was dropped rather than kept alongside.
                    // Neither constraint is broken by this. A single tap is
                    // not a double tap, so nothing waits for an interval, and
                    // the scrim is most of the window — plus Escape, plus a
                    // pinned close button. What IS given up is closing by
                    // clicking the picture, which is the trade that was
                    // asked for. If it reads worse in the hand, this handler
                    // is the one line to put back.
                    //
                    // While zoomed the Flickable steals drags for panning
                    // before they can count as taps, so a pan never zooms.
                    TapHandler {
                        id: imageTap
                        gesturePolicy: TapHandler.WithinBounds
                        // ONE handler with an explicit band check — NOT a
                        // second TapHandler on an image-sized child. Tap
                        // handlers are non-exclusive across subtrees (§16,
                        // four rounds of exactly this), so a nested pair
                        // fires BOTH on one click: the inner zoom and the
                        // outer close.
                        //
                        // The band is needed because `imageHolder` is
                        // `Math.max(flick.width, baseWidth * zoom)` — it
                        // always fills the viewport, so a fitted image leaves
                        // a wide margin of holder that is not picture. That
                        // margin reads as scrim and must close. It used to,
                        // back when both regions closed and the distinction
                        // cost nothing; splitting the two gestures is what
                        // turned the holder's size into a defect.
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
                    // Right-click the picture itself for the actions a person
                    // expects there — asked for in those words, "same as in
                    // Discord". LeftButton is imageTap's default, so the two
                    // handlers cannot both fire on one press.
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
                        // The still frame is the default AND the fallback: it
                        // yields only once the AnimatedImage actually reports
                        // Ready, so a build whose plugins cannot decode the
                        // animation shows the picture instead of nothing.
                        visible: !animatedImage.visible
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
                        // NOT gated on `visible`: `visible` waits for Ready,
                        // and a source that only appears once the image is
                        // ready can never become ready.
                        source: viewer.animateGifs ? viewer.animatedSource : ""
                        onStatusChanged: {
                            if (status === Image.Ready && viewer.baseWidth === 0)
                                viewer.fitImage(implicitWidth, implicitHeight)
                        }
                    }
                }

                WheelHandler {
                    target: null
                    // THE WHEEL PANS A ZOOMED IMAGE AND ZOOMS A FITTED ONE.
                    // Once the picture is bigger than the window the wheel
                    // is the gesture for moving around it, which is what a
                    // scroll wheel means everywhere else; while it fits
                    // there is nowhere to pan to, so the wheel keeps its
                    // pointer-centred zoom and nothing is lost.
                    //
                    // Ctrl+wheel always zooms, at either size. Without it,
                    // zooming back OUT by wheel would be unreachable the
                    // moment zooming in made the image pannable — the
                    // gesture would work in one direction only. (The model
                    // this follows drops ctrl+wheel on the floor and accepts
                    // exactly that; the keys and the toolbar are its way
                    // back. Keeping it costs nothing and removes a trap.)
                    onWheel: (event) => {
                        var wantsZoom = !flick.interactive
                                || (event.modifiers & Qt.ControlModifier)
                        if (wantsZoom) {
                            const factor = event.angleDelta.y > 0 ? 1.2 : 1 / 1.2
                            viewer.zoomAt(Qt.point(event.x, event.y),
                                          viewer.zoom * factor)
                            return
                        }
                        // pixelDelta is populated by trackpads and is zero
                        // for a notched mouse wheel, where angleDelta is the
                        // only signal: 120 units is one notch by convention.
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
                // Shown for any SET, not for any position. The list wraps, so
                // there is no first and no last to disable against — an arrow
                // that vanished at the ends would be telling the user the set
                // had run out when it has not.
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

        // ── Header: filename + sender/time (floats over the image) ───────
        RowLayout {
            id: headerRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing12
            // Clear of the pinned close button, which is no longer part of
            // this row: 36px of button plus its own margin. Without this the
            // filename would run underneath it.
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
                    // Guarded on the sender rather than on `current`: a
                    // viewer opened on a row the timeline does not hold
                    // (openFor's miss branch) knows the media key and
                    // nothing else, and "· 1 Jan 1970" is worse than blank.
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

        // PINNED. Not part of `chrome.shown`, and this is the one control
        // that must not be.
        //
        // It used to live inside headerRow, which fades with the rest of the
        // chrome — and once the chrome started hiding on ZOOM rather than on
        // an idle timer, that became a trap. The old idle fade was
        // recoverable by any pointer movement; the zoom gate is not. At
        // opacity 0 the toolbar is `visible: false` and cannot be hovered
        // back into existence, and a zoomed picture fills the viewport so
        // every click takes the zoom branch of the band check. A pointer-only
        // user was left with Escape, or un-zooming first, as the only ways
        // out of a full-screen overlay.
        //
        // A way out must never be conditional, so this one never fades.
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

        // ── Thumbnail strip ──────────────────────────────────────────────
        //
        // Only for a real SET: one image has nothing to strip. It asks the
        // bridge for `list_thumb`, the smallest cached kind, so opening a
        // room's worth of pictures does not pull full payloads for every
        // one of them just to draw 48px squares.
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

            // THE WIDTH IS A WHOLE NUMBER OF THUMBNAILS, NEVER THE RAW
            // AVAILABLE SPACE.
            //
            // `Math.min(available, contentWidth)` cut the strip wherever the
            // window happened to end, so the first and last tiles were sliced
            // down the middle — a row of pictures with two ragged stumps on
            // it. `clip: true` is what makes that visible, and it cannot be
            // dropped: without it the strip paints over the image instead.
            //
            // So the strip is sized to fit N COMPLETE cells and no fraction
            // of one. `snapMode` keeps that true after a flick, and because
            // every cell is the same width, a `Contain` scroll from an
            // aligned position stays aligned.
            readonly property int cell: 48 + spacing
            width: {
                var avail = parent.width - AppTheme.spacing16 * 2
                var fit = Math.max(1, Math.floor((avail + spacing) / cell))
                // Sized from the MODEL, never from `contentWidth`.
                //
                // `contentWidth` is an output of the ListView, derived from
                // the delegates it has created — and how many it creates is
                // derived from `width`. Reading it here is a binding loop:
                // Qt either logs one or quietly settles on whatever estimate
                // the view had while items were still being built. Every cell
                // is a fixed 48px, so the real content width is known from
                // the entry count without asking the view anything.
                var all = viewer.entries.length * cell - spacing
                // The trailing item carries no spacing after it.
                return Math.min(all, fit * cell - spacing)
            }
            snapMode: ListView.SnapToItem
            visible: opacity > 0
            opacity: (viewer.entries.length > 1 && chrome.shown) ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 180 }
            }
            // Keep the current thumbnail reachable as the selection moves —
            // `Contain` scrolls only when it has fallen off an edge, so the
            // strip does not lurch on every step.
            onCurrentIndexChanged: positionViewAtIndex(currentIndex,
                                                       ListView.Contain)
            currentIndex: viewer.currentIndex

            // The strip sits over the scrim, whose tap closes the viewer.
            // Without this, a click on the gap BETWEEN two thumbnails would
            // close rather than do nothing — the one place a miss is most
            // likely, since the targets are 48px.
            //
            // AND IT SWALLOWED NOTHING WITHOUT `gesturePolicy`. On the
            // default `DragThreshold` a TapHandler takes only a PASSIVE
            // grab, so the scrim's close handler fired on the same press and
            // a near miss closed the viewer — precisely what this was
            // written to prevent. `WithinBounds` takes the exclusive grab,
            // which is the same thing `imageTap` has always relied on.
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
                    anchors.fill: parent
                    source: (modelData.mediaKey || "").length > 0
                            && app.mediaBridge.supported
                            ? app.mediaBridge.mediaSource(modelData.mediaKey,
                                                          "list_thumb")
                            : (modelData.httpUrl || "")
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    smooth: true
                    mipmap: true
                }
                // Everything that is not the current item recedes; hovering
                // brings it most of the way back so the strip answers the
                // pointer before it is clicked.
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
                // `gesturePolicy` IS THE FIX, and its absence was the bug.
                //
                // On the default `DragThreshold` policy this took only a
                // PASSIVE grab, so the scrim's close handler fired on the
                // same press: the picture was selected and the viewer shut
                // underneath it. `WithinBounds` takes the exclusive grab and
                // the ancestor never sees the tap — which is why `imageTap`,
                // which has asked for it since it was written, zooms the
                // picture without closing.
                //
                // A first attempt at this concluded the policy did NOT help
                // and reached for an AbstractButton instead. That was
                // measured through a broken fixture: the strip carries
                // `visible: opacity > 0` behind a 180ms fade, so the
                // synthesized click was landing on the scrim and closing the
                // viewer for a reason that had nothing to do with the
                // policy. The case waits for the fade now, and with it the
                // policy is provably the whole fix.
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
