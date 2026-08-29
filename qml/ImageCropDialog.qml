import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import MatrixClient

// THE crop/adjust dialog for every display image Lightning uploads.
//
// One component, used by every site: room avatar (Room Information and the
// Space Home card), Space avatar (Space settings), room/Space banner, own
// profile banner, and the picture chosen while creating a room. Before this
// existed, each of those uploaded the chosen file verbatim, so a 4000px
// landscape photograph became a room's avatar and Matrix clients cropped it
// to a centred square nobody had approved.
//
// WHAT IT IS AND IS NOT. It is PRESENTATION ONLY (CLAUDE.md §5): it decides
// where the crop rectangle sits and hands those coordinates to C++.
// `ImageCropper` reads the file, refuses anything that is not one of the five
// raster formats by MAGIC BYTES (so an .svg cannot reach a renderer), decodes,
// crops, caps and encodes, and writes a temp file. This dialog does no image
// maths beyond a rectangle and never sees bytes.
//
// WHY THE PREVIEW IS NOT THE CHOSEN FILE. `image://lightning-staged/<token>`
// serves bytes the cropper has already sniffed. Pointing an `Image` at the
// user's own file:// URL would hand an arbitrary path to Qt's image loader,
// which renders SVG as active content — forbidden by §6.
//
// THE INTERACTION MODEL, and it is the one thing to understand before
// changing anything here:
//
//   * The crop rectangle lives in VIEWPORT coordinates and is always fully
//     inside the viewport. It can be moved (drag inside it) and resized
//     (drag a corner) with its aspect ratio LOCKED.
//   * The image is panned (drag outside the crop) and zoomed (wheel, slider)
//     behind it.
//   * The single invariant tying the two together: THE IMAGE MUST COVER THE
//     CROP RECTANGLE. Every gesture clamps against that, which is why the
//     result can never contain a strip of nothing, and why zooming out stops
//     rather than pulling the picture off the frame.
//
// A circular avatar still produces a SQUARE image. Matrix avatars are square
// and clients draw the circle; punching transparent corners in would make the
// picture wrong in every client that draws a square.
AppDialog {
    id: root

    // ── API ──────────────────────────────────────────────────────────────
    /// "avatar" (1:1, shown as a circle) or "banner" (3:1 strip). One knob:
    /// the shape, the mask and the output cap all follow from it, so a call
    /// site cannot pick a square crop with a banner's ceiling by mistake.
    property string role: "avatar"
    readonly property real aspect: role === "banner" ? 3.0 : 1.0
    readonly property bool circular: role !== "banner"

    /// Emitted with a file:// URL for the CROPPED image. The call site hands
    /// this to whatever sink it already used — every one of them takes a
    /// local path, which is the whole reason this can be a pure pre-step.
    signal cropped(url file)

    /// Open the picker's result. Refusals (SVG, an unreadable file, a format
    /// this build has no codec for) surface INSIDE the dialog rather than
    /// silently doing nothing, because "I chose a picture and nothing
    /// happened" is the worst possible answer.
    function openFor(fileUrl) {
        root.errorText = ""
        root.srcW = 0
        root.srcH = 0
        root.previewUrl = ""
        var info = app.imageCrop.load(fileUrl)
        if (!info || !info.ok) {
            root.errorText = root._describe(info ? info.error : "")
        } else {
            root.previewUrl = info.previewUrl
            root.srcW = info.width
            root.srcH = info.height
        }
        root.open()
        // The viewport has no geometry until the popup is laid out, and the
        // initial frame is computed FROM that geometry.
        Qt.callLater(root._reset)
    }

    // ── State (viewport coordinates unless named otherwise) ──────────────
    property string previewUrl: ""
    property int srcW: 0                 // decoded source width, in pixels
    property int srcH: 0
    property string errorText: ""
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
    /// The smallest scale at which the image still covers the crop frame.
    /// Zooming below this is refused rather than clamped afterwards, so the
    /// picture never jumps out from under the frame mid-gesture.
    readonly property real minScale: (srcW > 0 && srcH > 0 && cropW > 0)
                                     ? Math.max(cropW / srcW, cropH / srcH)
                                     : 1.0
    readonly property real maxScale: Math.max(minScale * 8, 1.0)

    /// The smallest the frame may get, in viewport pixels. Small enough to
    /// pick a face out of a group photograph, large enough to still have
    /// grabbable corners.
    readonly property real minCropEdge: 56

    title: qsTr("Adjust picture")
    modal: true
    focus: true
    // Overlay.overlay, not the declaring item: two of the call sites are
    // themselves inside a Dialog (Space settings), and a popup parented to a
    // popup renders underneath it.
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(720, parent ? parent.width - 64 : 720)

    // Whichever way it closed — Escape, the scrim, Cancel, Use picture — the
    // staged bytes and the decoded source go with it. Nothing survives a
    // closed dialog.
    onClosed: app.imageCrop.discard()

    // ── Geometry helpers ────────────────────────────────────────────────
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

    /// Keep the image covering the frame. Called after every gesture; it is
    /// the one place the invariant is enforced.
    function _clampPan() {
        panX = _clamp(panX, cropX + cropW - drawnW, cropX)
        panY = _clamp(panY, cropY + cropH - drawnH, cropY)
    }

    /// Keep the frame inside BOTH the viewport and the drawn image.
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

    /// Zoom about the frame's centre, so the thing being framed stays framed
    /// instead of drifting toward a corner.
    function _zoomTo(next) {
        if (!ready)
            return
        next = _clamp(next, minScale, maxScale)
        if (next === imgScale)
            return
        var cx = cropX + cropW / 2
        var cy = cropY + cropH / 2
        // The source pixel currently under the frame's centre must stay under
        // it: pan' = c - (c - pan) * next/old.
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
        // Width leads and height follows, so the ratio is exact rather than
        // the nearer of two candidates.
        var w = Math.abs(px - fixedX)
        var h = Math.abs(py - fixedY)
        w = Math.max(w, h * aspect)
        // The room available in the direction this corner is travelling,
        // bounded by the viewport AND by the drawn image.
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
        // Shrinking the frame lowers minScale, so nothing needs re-zooming;
        // growing it can only be refused above, never allowed to strand the
        // image. The pan clamp keeps the cover invariant either way.
        _clampPan()
        _clampCrop()
    }

    function _accept() {
        if (!ready)
            return
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

        // ── The stage ────────────────────────────────────────────────────
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

                // Re-fit when the dialog is resized under a frame that was
                // measured against the old geometry.
                onWidthChanged: Qt.callLater(root._reset)
                onHeightChanged: Qt.callLater(root._reset)

                Image {
                    id: preview
                    objectName: "cropPreviewImage"
                    source: root.previewUrl
                    // A CONSTANT source size, so zooming never re-decodes.
                    // The provider caps at 4096 already; 2048 is more detail
                    // than a 400px stage can show at any usable zoom.
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

                // ── The mask: everything outside the frame, dimmed ───────
                //
                // ONE path with OddEvenFill punches the frame out of a
                // full-viewport rectangle, which is what makes the circular
                // case honest — a ring outline alone leaves the corners
                // looking like part of the result.
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

                // ── Panning and moving. Declared BEFORE the handles so a
                // corner grab wins the press. ─────────────────────────────
                MouseArea {
                    id: stageArea
                    objectName: "cropStageArea"
                    anchors.fill: parent
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
                        // A notch is a notch; the pixelDelta of a touchpad is
                        // read the same way so a two-finger pinch-less scroll
                        // still zooms rather than doing nothing.
                        var up = wheel.angleDelta.y !== 0
                                 ? wheel.angleDelta.y > 0
                                 : wheel.pixelDelta.y > 0
                        root._zoomTo(root.imgScale * (up ? 1.12 : 1 / 1.12))
                    }
                }

                // ── The frame outline and its corner grips ───────────────
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
                    model: root.ready ? 4 : 0
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

            // The refusal, in the space the picture would have occupied. A
            // dialog that opens empty says nothing; this names the cause.
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

        // ── Zoom ─────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: root.ready
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
                    // Same reasoning as the microphone slider: the thumb
                    // rides the fill boundary, so a dark disc reads as
                    // disabled past half range.
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
            Layout.fillWidth: true
            visible: root.ready
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: root.circular
                  ? qsTr("Drag to move the picture, drag the frame to move "
                         + "the crop, and drag a corner to resize it. Only "
                         + "the circle is shown, and a square picture is "
                         + "uploaded.")
                  : qsTr("Drag to move the picture, drag the frame to move "
                         + "the crop, and drag a corner to resize it.")
        }

        // ── Footer. Its own row rather than standardButtons, so the accept
        // button can say what it does. ───────────────────────────────────
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
