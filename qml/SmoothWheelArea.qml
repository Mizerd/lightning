import QtQuick

// SmoothWheelArea gives a plain Flickable/ListView/GridView the same wheel and
// touchpad feel as the room timeline (TimelinePane's timelineWheelHandler,
// backed by TimelineScrollController).
//
// It does not reuse app.timelineScroll's motion engine: that is one shared
// instance, and wheelNotch(), animateTo(), pixelTargetY(), cancel() and even
// wheelTargetY() mutate its motion state, so calling them from a second visible
// surface would disturb the timeline's own glide. Only
// notchDistance()/notchDistanceForSpeed() and motionStep() are pure const
// reads. This component uses those and drives its own local glide on its own
// Timer, so any number of instances and the timeline can scroll at once.
//
// The curve is the controller's own per-frame integration (motionStep), not an
// approximation; easing-curve animations felt wrong ("in blocks").
//
// Usage: declare as a direct child of the view it drives:
//
//     Flickable {
//         id: myFlick
//         ...
//         SmoothWheelArea {}
//     }
//
// Pass `scrollTarget` only when it cannot be a direct child.
WheelHandler {
    id: root

    // The view this scrolls. Walks up from `parent`: a handler declared inside
    // a Flickable attaches to its content item, so casting `parent` would yield
    // null, and the early return still accepts the event, making the pane
    // unscrollable.
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

    // The scrolled axis's extent, for the notch distance and per-frame ceiling.
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

    // Scroll range along the scrolled axis (floored at zero). The names keep
    // `Y` for existing callers and the contract test.
    readonly property real minContentY: 0
    readonly property real maxContentY: horizontal
        ? Math.max(0, (scrollTarget ? scrollTarget.contentWidth : 0)
                       - (scrollTarget ? scrollTarget.width : 0))
        : Math.max(0, (scrollTarget ? scrollTarget.contentHeight : 0)
                       - (scrollTarget ? scrollTarget.height : 0))

    // Local glide state, never touched from outside. Read defensively: several
    // suites load this without `app`.
    readonly property bool smoothScrollingEnabled: {
        // Coerce explicitly: a stub settings object yields undefined, and
        // assigning undefined to a bool warns (the GIF picker suites fail on
        // that). Defaults to true.
        if (typeof app === "undefined" || !app || !app.settings)
            return true
        var v = app.settings.smoothScrolling
        return v === undefined ? true : !!v
    }

    property real glideTargetY: 0
    property int glideDirection: 0   // -1 up, +1 down, 0 idle/just-reset

    function clampY(y, lo, hi) {
        if (hi < lo)
            hi = lo
        return y < lo ? lo : (y > hi ? hi : y)
    }

    // Cancel any in-flight glide without moving the target, so an embedding
    // pane can make a programmatic jump (like TimelinePane's
    // cancelWheelMotion()).
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

        // A mouse wheel reports on Y, so a horizontal view must answer vertical
        // wheels. An x delta (tilt wheel, shift+wheel) is used only when y is
        // empty and only on a horizontal view.
        var pixels = event.pixelDelta.y !== 0
            ? event.pixelDelta.y
            : (root.horizontal ? event.pixelDelta.x : 0)
        var angle = event.angleDelta.y !== 0
            ? event.angleDelta.y
            : (root.horizontal ? event.angleDelta.x : 0)

        // A phased frame is a touchpad frame even at 0 whole pixels (Qt Wayland
        // carries the remainder); treating it as a notch made slow swipes
        // travel far too far. See TimelinePane.qml. Wheels never have a phase.
        var continuousSource = event.phase !== Qt.NoScrollPhase
        if (pixels !== 0 || (continuousSource && angle !== 0)) {
            // High-resolution touchpad / precision wheel: apply the platform
            // delta directly, as TimelineScrollController::pixelTargetY does,
            // cancelling any notch glide first.
            root.stopGlide()
            root.setScrollPosition(
                root.clampY(root.scrollPosition() - pixels, lo, hi))
        } else if (angle !== 0) {
            // Discrete wheel notch: the timeline's notchDistance() (honouring
            // the wheel speed setting), 120 angle units per notch, with
            // same-direction notches coalesced into one glide.
            var controller = (typeof app !== "undefined" && app)
                              ? app.timelineScroll : null
            var per = controller
                      ? controller.notchDistance(root.viewportExtent) : 120.0
            var deltaPixels = -(angle / 120.0) * per
            var dir = deltaPixels > 0 ? 1 : (deltaPixels < 0 ? -1 : 0)
            // Smooth scrolling off: land the whole notch immediately, same
            // distance.
            if (dir !== 0 && !root.smoothScrollingEnabled) {
                root.stopGlide()
                root.setScrollPosition(
                    root.clampY(root.scrollPosition() + deltaPixels, lo, hi))
            } else if (dir !== 0) {
                // Same direction while gliding extends from the in-flight
                // target; a reversal or fresh gesture starts from the live
                // position (as wheelTargetY()).
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

    // Ticks the glide with the controller's motionStep, so the feel matches the
    // timeline. A same-direction notch only moves glideTargetY.
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
