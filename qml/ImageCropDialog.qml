import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import MatrixClient

// The crop/adjust dialog for every display image Lightning uploads: room and
// Space avatars, banners, the own profile banner, and the picture chosen when
// creating a room.
//
// Presentation only: it decides where the crop rectangle sits and hands the
// coordinates to C++. `ImageCropper` reads the file, accepts only the five
// raster formats by magic bytes (so SVG never reaches a renderer), decodes,
// crops, caps, encodes and writes a temp file.
//
// The preview uses `image://lightning-staged/<token>`, bytes the cropper has
// already sniffed. Pointing an Image at the user's file:// URL would let Qt's
// loader render SVG as active content.
//
// Interaction model:
//   * The crop rectangle is in viewport coordinates and always fully inside
//     the viewport. It moves (drag inside) and resizes (drag a corner) with
//     its aspect ratio locked.
//   * The image pans (drag outside the crop) and zooms (wheel, slider)
//     behind it.
//   * Invariant: the image always covers the crop rectangle. Every gesture
//     clamps against it, so the result never contains empty space.
//
// A circular avatar still produces a square image: Matrix avatars are square
// and clients draw the circle.
//
// Animated sources (GIF, animated WebP): Qt cannot encode an animation, so any
// crop flattens it to one frame. "Keep animation" uploads the original frames
// instead, uncropped and with the metadata stripped, the frame locked to the
// centre, which is where clients crop it when drawing. The animated preview is
// `animatedUrl`, the stripped copy the cropper wrote after sniffing it, never
// the chosen file.
AppDialog {
    id: root

    // ── API ──
    /// "avatar" (1:1, shown as a circle) or "banner" (3:1 strip). Shape, mask
    /// and output cap all follow from it.
    property string role: "avatar"
    readonly property real aspect: role === "banner" ? 3.0 : 1.0
    readonly property bool circular: role !== "banner"

    /// Emitted with a file:// URL for the cropped image; every call site's sink
    /// already takes a local path.
    signal cropped(url file)

    /// Open the picker's result. Refusals (SVG, unreadable file, missing codec)
    /// are shown inside the dialog rather than silently ignored.
    function openFor(fileUrl) {
        root.errorText = ""
        root.srcW = 0
        root.srcH = 0
        root.previewUrl = ""
        root.animated = false
        root.animatedUrl = ""
        root.canKeepAnimation = false
        var info = app.imageCrop.load(fileUrl)
        if (!info || !info.ok) {
            root.errorText = root._describe(info ? info.error : "")
        } else {
            root.previewUrl = info.previewUrl
            root.srcW = info.width
            root.srcH = info.height
            root.animated = info.animated === true
            root.animatedUrl = info.animatedUrl || ""
            // Asked once here, not bound: it is a Q_INVOKABLE with no notify.
            root.canKeepAnimation = app.imageCrop.canKeepAnimation(root.role)
        }
        root.keepAnimation = root.canKeepAnimation
        root.open()
        // The viewport has no geometry until the popup is laid out.
        Qt.callLater(root._reset)
    }

    // ── State (viewport coordinates unless named otherwise) ──
    property string previewUrl: ""
    property int srcW: 0                 // decoded source width, in pixels
    property int srcH: 0
    property string errorText: ""
    /// The source is an animation, and whether it can be kept in this role.
    property bool animated: false
    property string animatedUrl: ""
    property bool canKeepAnimation: false
    /// Upload the original animation rather than a cropped still frame.
    property bool keepAnimation: false
    readonly property bool cropLocked: keepAnimation && canKeepAnimation
    /// Displayed pixels per source pixel.
    property real imgScale: 1.0
    /// Top-left of the drawn image, in viewport coordinates.
    property real panX: 0
    property real panY: 0
    property real cropX: 0
    property real cropY: 0
    property real cropW: 0
    property real cropH: 0

    readonly property bool ready: srcW > 0 && srcH > 0 && cropW > 0
    readonly property real drawnW: srcW * imgScale
    readonly property real drawnH: srcH * imgScale
    /// The smallest scale at which the image still covers the frame. Zooming
    /// below it is refused, so the picture never jumps mid-gesture.
    readonly property real minScale: (srcW > 0 && srcH > 0 && cropW > 0)
                                     ? Math.max(cropW / srcW, cropH / srcH)
                                     : 1.0
    readonly property real maxScale: Math.max(minScale * 8, 1.0)

    /// Minimum frame edge in viewport pixels: small enough to pick out a face,
    /// large enough to keep grabbable corners.
    readonly property real minCropEdge: 56

    title: qsTr("Adjust picture")
    modal: true
    focus: true
    // Overlay.overlay: some call sites are inside a Dialog, and a popup
    // parented to a popup renders underneath it.
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(720, parent ? parent.width - 64 : 720)

    // However it closes, the staged bytes and decoded source are discarded.
    onClosed: app.imageCrop.discard()

    // ── Geometry helpers ──
    function _describe(category) {
        if (category === "unsupported_image")
            return qsTr("That file isn't a picture Lightning can use. "
                        + "Choose a PNG, JPEG, GIF, WebP or BMP image.")
        if (category === "too_large")
            return qsTr("That picture is too large to open.")
        if (category === "undecodable")
            return qsTr("That picture couldn't be opened. It may be "
                        + "incomplete, or in a format this build can't read.")
        if (category === "unreadable")
            return qsTr("That file couldn't be read.")
        if (category === "animation_too_large")
            return qsTr("That animation can't be kept. "
                        + "Turn off \"Keep animation\" to use a still frame.")
        return qsTr("That picture couldn't be used.")
    }

    /// Fit the image, then centre the largest frame of the right shape on it.
    function _reset() {
        if (srcW <= 0 || srcH <= 0 || viewport.width <= 0 || viewport.height <= 0)
            return
        imgScale = Math.min(viewport.width / srcW, viewport.height / srcH)
        panX = (viewport.width - drawnW) / 2
        panY = (viewport.height - drawnH) / 2
        var w = Math.min(drawnW, drawnH * aspect)
        var h = w / aspect
        cropW = w
        cropH = h
        cropX = panX + (drawnW - w) / 2
        cropY = panY + (drawnH - h) / 2
        zoomSlider.value = imgScale
    }

    function _clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)) }

    /// Keep the image covering the frame; the one place the invariant is
    /// enforced, called after every gesture.
    function _clampPan() {
        panX = _clamp(panX, cropX + cropW - drawnW, cropX)
        panY = _clamp(panY, cropY + cropH - drawnH, cropY)
    }

    /// Keep the frame inside both the viewport and the drawn image.
    function _clampCrop() {
        var loX = Math.max(0, panX)
        var loY = Math.max(0, panY)
        var hiX = Math.min(viewport.width, panX + drawnW) - cropW
        var hiY = Math.min(viewport.height, panY + drawnH) - cropH
        cropX = _clamp(cropX, loX, Math.max(loX, hiX))
        cropY = _clamp(cropY, loY, Math.max(loY, hiY))
    }

    function _pan(dx, dy) {
        panX += dx
        panY += dy
        _clampPan()
    }

    function _moveCrop(dx, dy) {
        cropX += dx
        cropY += dy
        _clampCrop()
    }

    /// Zoom about the frame's centre, so the framed subject stays framed.
    function _zoomTo(next) {
        if (!ready)
            return
        next = _clamp(next, minScale, maxScale)
        if (next === imgScale)
            return
        var cx = cropX + cropW / 2
        var cy = cropY + cropH / 2
        // Keep the source pixel under the frame's centre fixed:
        // pan' = c - (c - pan) * next/old.
        var k = next / imgScale
        panX = cx - (cx - panX) * k
        panY = cy - (cy - panY) * k
        imgScale = next
        _clampPan()
        if (zoomSlider.value !== next)
            zoomSlider.value = next
    }

    /// Resize from one corner, aspect locked, anchored at the opposite one.
    /// `corner` is 0 TL, 1 TR, 2 BR, 3 BL.
    function _resizeCrop(corner, px, py) {
        if (!ready)
            return
        var fixedX = (corner === 0 || corner === 3) ? cropX + cropW : cropX
        var fixedY = (corner === 0 || corner === 1) ? cropY + cropH : cropY
        // Width leads and height follows, so the ratio is exact.
        var w = Math.abs(px - fixedX)
        var h = Math.abs(py - fixedY)
        w = Math.max(w, h * aspect)
        // Room available in the corner's direction, bounded by the viewport and
        // the drawn image.
        var leftBound = Math.max(0, panX)
        var topBound = Math.max(0, panY)
        var rightBound = Math.min(viewport.width, panX + drawnW)
        var bottomBound = Math.min(viewport.height, panY + drawnH)
        var roomX = (corner === 0 || corner === 3) ? fixedX - leftBound
                                                   : rightBound - fixedX
        var roomY = (corner === 0 || corner === 1) ? fixedY - topBound
                                                   : bottomBound - fixedY
        w = Math.min(w, roomX, roomY * aspect)
        w = Math.max(w, minCropEdge, minCropEdge * aspect)
        if (w > roomX || w / aspect > roomY)
            return   // no room to grow; leave the frame exactly as it was
        var newH = w / aspect
        cropX = (corner === 0 || corner === 3) ? fixedX - w : fixedX
        cropY = (corner === 0 || corner === 1) ? fixedY - newH : fixedY
        cropW = w
        cropH = newH
        // Growing is refused above rather than allowed to strand the image; the
        // pan clamp keeps the cover invariant.
        _clampPan()
        _clampCrop()
    }

    function _accept() {
        if (!ready)
            return
        if (root.cropLocked) {
            var kept = app.imageCrop.useAnimation(root.role)
            if (!kept || kept.toString().length === 0) {
                root.errorText = root._describe(app.imageCrop.lastError)
                return
            }
            root.cropped(kept)
            root.close()
            return
        }
        var sx = (cropX - panX) / imgScale
        var sy = (cropY - panY) / imgScale
        var sw = cropW / imgScale
        var sh = cropH / imgScale
        var out = app.imageCrop.crop(sx, sy, sw, sh,
                                     app.imageCrop.maxEdgeForRole(root.role))
        if (!out || out.toString().length === 0) {
            root.errorText = root._describe(app.imageCrop.lastError)
            return
        }
        root.cropped(out)
        root.close()
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // ── The stage ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(
                220, Math.min(400, root.parent ? root.parent.height - 320 : 360))
            radius: AppTheme.radiusMd
            color: AppTheme.stormInset
            border.color: AppTheme.stormBorder
            border.width: 1
            clip: true

            Item {
                id: viewport
                objectName: "cropViewport"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                visible: root.errorText.length === 0

                // Re-fit when the dialog is resized.
                onWidthChanged: Qt.callLater(root._reset)
                onHeightChanged: Qt.callLater(root._reset)

                Image {
                    id: preview
                    objectName: "cropPreviewImage"
                    source: root.previewUrl
                    // A constant source size so zooming never re-decodes; 2048
                    // exceeds what the stage can show (the provider caps at
                    // 4096).
                    sourceSize.width: Math.min(root.srcW, 2048)
                    asynchronous: true
                    smooth: true
                    mipmap: true
                    fillMode: Image.Stretch
                    x: root.panX
                    y: root.panY
                    width: root.drawnW
                    height: root.drawnH
                }
                // The animation that will be uploaded, over the still frame.
                Loader {
                    objectName: "cropAnimatedPreview"
                    x: root.panX
                    y: root.panY
                    width: root.drawnW
                    height: root.drawnH
                    // Plays only where every other animation would: the
                    // still frame beneath stands in otherwise.
                    active: root.cropLocked && root.animatedUrl.length > 0
                            && !AppTheme.reducedMotion
                            && app.settings.gifAutoplay !== 2
                    sourceComponent: AnimatedImage {
                        source: root.animatedUrl
                        fillMode: Image.Stretch
                        asynchronous: true
                        cache: false
                        playing: true
                        smooth: true
                    }
                }

                // ── The mask: everything outside the frame, dimmed ── One
                // OddEvenFill path punches the frame out of the viewport, so
                // the circular case dims the corners too.
                Shape {
                    anchors.fill: parent
                    visible: root.ready && !root.circular
                    ShapePath {
                        fillColor: AppTheme.overlayScrim
                        strokeWidth: 0
                        strokeColor: "transparent"
                        fillRule: ShapePath.OddEvenFill
                        startX: 0; startY: 0
                        PathLine { x: viewport.width; y: 0 }
                        PathLine { x: viewport.width; y: viewport.height }
                        PathLine { x: 0; y: viewport.height }
                        PathLine { x: 0; y: 0 }
                        PathMove { x: root.cropX; y: root.cropY }
                        PathLine { x: root.cropX + root.cropW; y: root.cropY }
                        PathLine { x: root.cropX + root.cropW; y: root.cropY + root.cropH }
                        PathLine { x: root.cropX; y: root.cropY + root.cropH }
                        PathLine { x: root.cropX; y: root.cropY }
                    }
                }
                Shape {
                    anchors.fill: parent
                    visible: root.ready && root.circular
                    ShapePath {
                        fillColor: AppTheme.overlayScrim
                        strokeWidth: 0
                        strokeColor: "transparent"
                        fillRule: ShapePath.OddEvenFill
                        startX: 0; startY: 0
                        PathLine { x: viewport.width; y: 0 }
                        PathLine { x: viewport.width; y: viewport.height }
                        PathLine { x: 0; y: viewport.height }
                        PathLine { x: 0; y: 0 }
                        PathAngleArc {
                            centerX: root.cropX + root.cropW / 2
                            centerY: root.cropY + root.cropH / 2
                            radiusX: root.cropW / 2
                            radiusY: root.cropH / 2
                            startAngle: 0
                            sweepAngle: 360
                            moveToStart: true
                        }
                    }
                }

                // ── Panning and moving; declared before the handles so a
                // corner grab wins the press. ──
                MouseArea {
                    id: stageArea
                    objectName: "cropStageArea"
                    anchors.fill: parent
                    // A kept animation is uploaded whole; nothing to move.
                    enabled: !root.cropLocked
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    cursorShape: containsMouse
                                 ? (movingCrop || _inCrop(mouseX, mouseY)
                                    ? Qt.SizeAllCursor : Qt.OpenHandCursor)
                                 : Qt.ArrowCursor

                    property bool movingCrop: false
                    property bool dragging: false
                    property real lastX: 0
                    property real lastY: 0

                    function _inCrop(px, py) {
                        return px >= root.cropX && px <= root.cropX + root.cropW
                               && py >= root.cropY
                               && py <= root.cropY + root.cropH
                    }

                    onPressed: function (mouse) {
                        if (!root.ready)
                            return
                        lastX = mouse.x
                        lastY = mouse.y
                        movingCrop = _inCrop(mouse.x, mouse.y)
                        dragging = true
                    }
                    onPositionChanged: function (mouse) {
                        if (!dragging || !root.ready)
                            return
                        var dx = mouse.x - lastX
                        var dy = mouse.y - lastY
                        lastX = mouse.x
                        lastY = mouse.y
                        if (movingCrop)
                            root._moveCrop(dx, dy)
                        else
                            root._pan(dx, dy)
                    }
                    onReleased: { dragging = false; movingCrop = false }
                    onCanceled: { dragging = false; movingCrop = false }
                    onWheel: function (wheel) {
                        if (!root.ready)
                            return
                        // Touchpad pixelDelta is read like a wheel notch, so
                        // scrolling zooms.
                        var up = wheel.angleDelta.y !== 0
                                 ? wheel.angleDelta.y > 0
                                 : wheel.pixelDelta.y > 0
                        root._zoomTo(root.imgScale * (up ? 1.12 : 1 / 1.12))
                    }
                }

                // ── The frame outline and its corner grips ──
                Rectangle {
                    objectName: "cropFrame"
                    visible: root.ready
                    x: root.cropX
                    y: root.cropY
                    width: root.cropW
                    height: root.cropH
                    color: "transparent"
                    border.color: AppTheme.scrimInk
                    border.width: 1
                    radius: root.circular ? Math.min(width, height) / 2 : 0
                }

                Repeater {
                    model: root.ready && !root.cropLocked ? 4 : 0
                    delegate: Item {
                        required property int index
                        readonly property real hx: (index === 0 || index === 3)
                                                   ? root.cropX
                                                   : root.cropX + root.cropW
                        readonly property real hy: (index === 0 || index === 1)
                                                   ? root.cropY
                                                   : root.cropY + root.cropH
                        x: hx - 13
                        y: hy - 13
                        width: 26
                        height: 26

                        Rectangle {
                            anchors.centerIn: parent
                            width: 12
                            height: 12
                            radius: 3
                            color: AppTheme.scrimInk
                            border.color: AppTheme.overlayScrim
                            border.width: 1
                        }

                        MouseArea {
                            objectName: "cropCornerHandle"
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            cursorShape: (parent.index === 0 || parent.index === 2)
                                         ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                            onPositionChanged: function (mouse) {
                                if (!pressed)
                                    return
                                var p = mapToItem(viewport, mouse.x, mouse.y)
                                root._resizeCrop(parent.index, p.x, p.y)
                            }
                        }
                    }
                }
            }

            // The refusal, shown where the picture would be.
            Label {
                objectName: "cropErrorLabel"
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing24 * 2
                visible: root.errorText.length > 0
                text: root.errorText
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textBody
            }
        }

        // ── Animation ──
        RowLayout {
            Layout.fillWidth: true
            visible: root.ready && root.animated
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                text: root.canKeepAnimation
                      ? qsTr("Keep animation")
                      : qsTr("This animation can't be kept (it is too large "
                             + "or unreadable), so a still frame will be used.")
            }
            AppSwitch {
                objectName: "cropKeepAnimationSwitch"
                visible: root.canKeepAnimation
                checked: root.keepAnimation
                Accessible.name: qsTr("Keep animation")
                onToggled: {
                    root.keepAnimation = !root.keepAnimation
                    // The kept animation is shown centred, as it will be drawn.
                    if (root.keepAnimation)
                        root._reset()
                }
            }
        }

        // ── Zoom ──
        // Hidden but still laid out while an animation is kept, so toggling
        // "Keep animation" never moves the switch out from under the pointer.
        RowLayout {
            Layout.fillWidth: true
            visible: root.ready
            opacity: root.cropLocked ? 0 : 1
            enabled: !root.cropLocked
            spacing: AppTheme.spacing8

            Icon {
                name: "zoom_out"
                size: 16
                color: AppTheme.stormTextMuted
            }

            Slider {
                id: zoomSlider
                objectName: "cropZoomSlider"
                Layout.fillWidth: true
                from: root.minScale
                to: root.maxScale
                value: root.imgScale
                Accessible.name: qsTr("Zoom")
                onMoved: root._zoomTo(value)

                background: Rectangle {
                    x: zoomSlider.leftPadding
                    y: zoomSlider.topPadding
                       + zoomSlider.availableHeight / 2 - 2
                    width: zoomSlider.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.stormInset
                    Rectangle {
                        width: zoomSlider.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.bolt
                    }
                }
                handle: Rectangle {
                    x: zoomSlider.leftPadding + zoomSlider.visualPosition
                       * (zoomSlider.availableWidth - width)
                    y: zoomSlider.topPadding
                       + zoomSlider.availableHeight / 2 - height / 2
                    width: 16
                    height: 16
                    radius: 8
                    // White: a dark thumb on the fill boundary reads as
                    // disabled.
                    color: "#FFFFFF"
                    border.width: zoomSlider.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
            }

            Icon {
                name: "zoom_in"
                size: 16
                color: AppTheme.stormTextMuted
            }
        }

        Label {
            id: cropHint
            Layout.fillWidth: true
            visible: root.ready
            // Two lines either way, for the same reason as the zoom row.
            Layout.minimumHeight: root.animated
                                  ? Math.ceil(hintMetrics.lineSpacing * 2)
                                  : 0
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            FontMetrics {
                id: hintMetrics
                font: cropHint.font
            }
            text: root.cropLocked
                  ? (root.circular
                     ? qsTr("An animation is uploaded whole and can't be "
                            + "cropped. Everyone sees it centred, as outlined.")
                     : qsTr("An animation is uploaded whole and can't be "
                            + "cropped. It is shown roughly as outlined; some "
                            + "views crop it differently."))
                  : root.circular
                  ? qsTr("Drag to move the picture, drag the frame to move "
                         + "the crop, and drag a corner to resize it. Only "
                         + "the circle is shown, and a square picture is "
                         + "uploaded.")
                  : qsTr("Drag to move the picture, drag the frame to move "
                         + "the crop, and drag a corner to resize it.")
        }

        // ── Footer: its own row rather than standardButtons, so the accept
        // button can say what it does. ──
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            spacing: AppTheme.spacing8

            Item { Layout.fillWidth: true }

            AppButton {
                objectName: "cropCancelButton"
                storm: true
                kind: "secondary"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            AppButton {
                objectName: "cropAcceptButton"
                storm: true
                kind: "primary"
                text: qsTr("Use picture")
                enabled: root.ready
                onClicked: root._accept()
            }
        }
    }
}
