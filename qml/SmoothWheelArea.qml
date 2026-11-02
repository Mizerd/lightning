import QtQuick

WheelHandler {
    id: root

    property Flickable scrollTarget: {
        var p = parent
        while (p) {
            var f = p as Flickable
            if (f)
                return f
            p = p.parent
        }
        return null
    }

    // "auto" (default), "vertical" or "horizontal". Bounds on a horizontal view
    // must come from widths: height-derived bounds are zero there and every
    // wheel event would be accepted and ignored. "auto" reads horizontal only
    // when the vertical axis has no overflow and the horizontal one does; set
    // it explicitly on a view of known orientation:
    //
    //     ListView { orientation: ListView.Horizontal
    //                SmoothWheelArea { axis: "horizontal" } }
    property string axis: "auto"
    readonly property bool horizontal:
        axis === "horizontal"
        || (axis === "auto" && scrollTarget !== null
            && scrollTarget.contentHeight <= scrollTarget.height + 0.5
            && scrollTarget.contentWidth > scrollTarget.width + 0.5)

    readonly property real viewportExtent: scrollTarget
        ? (horizontal ? scrollTarget.width : scrollTarget.height) : 0
    function scrollPosition() {
        var t = root.scrollTarget
        if (!t)
            return 0
        return root.horizontal ? t.contentX : t.contentY
    }
    function setScrollPosition(value) {
        var t = root.scrollTarget
        if (!t)
            return
        if (root.horizontal)
            t.contentX = value
        else
            t.contentY = value
    }

    // A ListView's origin moves as rows are inserted above the first one.
    readonly property real minContentY: !scrollTarget ? 0
        : horizontal ? scrollTarget.originX - scrollTarget.leftMargin
                     : scrollTarget.originY - scrollTarget.topMargin
    readonly property real maxContentY: !scrollTarget ? 0
        : Math.max(minContentY, horizontal
            ? scrollTarget.originX + scrollTarget.contentWidth
              + scrollTarget.rightMargin - scrollTarget.width
            : scrollTarget.originY + scrollTarget.contentHeight
              + scrollTarget.bottomMargin - scrollTarget.height)

    readonly property bool smoothScrollingEnabled: {
        if (typeof app === "undefined" || !app || !app.settings)
            return true
        var v = app.settings.smoothScrolling
        return v === undefined ? true : !!v
    }

    property real glideTargetY: 0
    property int glideDirection: 0

    function clampY(y, lo, hi) {
        if (hi < lo)
            hi = lo
        return y < lo ? lo : (y > hi ? hi : y)
    }

    function stopGlide() {
        root.ticker.stop()
        root.glideDirection = 0
    }

    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    onWheel: (event) => {
        var t = root.scrollTarget
        if (!t) {
            event.accepted = true
            return
        }
        var lo = root.minContentY
        var hi = root.maxContentY

        var pixels = event.pixelDelta.y !== 0
            ? event.pixelDelta.y
            : (root.horizontal ? event.pixelDelta.x : 0)
        var angle = event.angleDelta.y !== 0
            ? event.angleDelta.y
            : (root.horizontal ? event.angleDelta.x : 0)

        var continuousSource = event.phase !== Qt.NoScrollPhase
        if (pixels !== 0 || (continuousSource && angle !== 0)) {
            root.stopGlide()
            root.setScrollPosition(
                root.clampY(root.scrollPosition() - pixels, lo, hi))
        } else if (angle !== 0) {
            var controller = (typeof app !== "undefined" && app)
                              ? app.timelineScroll : null
            var per = controller
                      ? controller.notchDistance(root.viewportExtent) : 120.0
            var deltaPixels = -(angle / 120.0) * per
            var dir = deltaPixels > 0 ? 1 : (deltaPixels < 0 ? -1 : 0)
            if (dir !== 0 && !root.smoothScrollingEnabled) {
                root.stopGlide()
                root.setScrollPosition(
                    root.clampY(root.scrollPosition() + deltaPixels, lo, hi))
            } else if (dir !== 0) {
                var base = (root.ticker.running && dir === root.glideDirection)
                           ? root.glideTargetY : root.scrollPosition()
                var newTarget = root.clampY(base + deltaPixels, lo, hi)
                root.glideTargetY = newTarget
                root.glideDirection = dir
                if (!root.ticker.running)
                    root.ticker.start()
            }
        }
        event.accepted = true
    }

    property Timer ticker: Timer {
        interval: 16
        repeat: true
        running: false
        onTriggered: {
            var t = root.scrollTarget
            if (!t) {
                stop()
                return
            }
            var controller = (typeof app !== "undefined" && app)
                              ? app.timelineScroll : null
            var remaining = root.glideTargetY - root.scrollPosition()
            if (!controller || Math.abs(remaining) <= 0.5) {
                root.setScrollPosition(
                    root.clampY(root.glideTargetY,
                                root.minContentY, root.maxContentY))
                root.glideDirection = 0
                stop()
                return
            }
            root.setScrollPosition(root.clampY(
                root.scrollPosition()
                    + controller.motionStep(remaining, interval,
                                            root.viewportExtent),
                root.minContentY, root.maxContentY))
        }
    }

    Component.onDestruction: root.ticker.stop()
}
