import QtQuick

// SmoothWheelArea — gives any plain Flickable/ListView/GridView the SAME
// mouse-wheel and touchpad feel as the room timeline
// (qml/TimelinePane.qml's `timelineWheelHandler`, backed by
// src/models/TimelineScrollController.*), so scrolling in Settings, dialogs
// and side panels no longer feels "slower and different" than the chat
// timeline (2026-08-16 maintainer report).
//
// ── Why this does NOT reuse TimelineScrollController's motion engine ──────
// `app.timelineScroll` is exposed to QML as ONE shared instance
// (QML_UNCREATABLE — "TimelineScrollController is exposed via
// app.timelineScroll"). Reading its header/implementation:
//   * wheelNotch(), animateTo(), pixelTargetY(), cancel(), and — this is
//     the easy-to-miss part — wheelTargetY() itself all MUTATE that one
//     instance's private motion state (m_targetY, m_direction,
//     m_motionActive, the ticker) and/or emit its single
//     wheelPositionChanged()/motionActiveChanged() signals. wheelTargetY()
//     looks like a pure "compute the target" helper but calls
//     setMotionActive(true) and writes m_targetY/m_direction as a side
//     effect, and pixelTargetY() calls cancel() as a side effect. Calling
//     either from a second, simultaneously visible surface (Room
//     Information open beside a live timeline; a dialog over the room
//     list) would flip the TIMELINE's own `wheelAnimating`/motionActive
//     state, corrupt its in-flight coalesced target, or silently kill a
//     glide already in flight without emitting wheelMotionSettled — none
//     of which this component's caller can see or account for.
//   * Only notchDistance()/notchDistanceForSpeed() are genuine `const`
//     reads with no side effects, and the `wheelSpeed` property/enum are
//     plain reads too.
// So this component (deliberately option "b" from the task) reads ONLY
// notchDistance() — the one real source of truth for "how far is one
// notch at the user's configured speed" (Settings → wheel speed governs
// this pane exactly like it governs the timeline) — and drives its own,
// fully independent local glide, ticked by its own Timer. It never
// calls wheelNotch/animateTo/pixelTargetY/wheelTargetY/cancel on the
// shared controller, and never touches TimelinePane.qml or the timeline's
// own motion state. Multiple SmoothWheelArea instances (and the timeline)
// can be visible and scrolling at the same time without fighting each
// other, because each owns nothing but its own local `glide`.
//
// ── Feel matching ───────────────────────────────────────────────────────
// The curve is NOT approximated. motionStep() is the controller's own
// per-frame integration (exponential approach at tau, the minimum-settle
// speed floor, the per-frame viewport ceiling) exposed as a pure const
// function, and this component calls it once per tick. So the distance per
// notch and the deceleration are both the timeline's, from the timeline's
// own code — there is no second copy of the maths to drift.
//
// Two earlier attempts approximated instead, and both were reported as
// feeling wrong: a SmoothedAnimation eased IN as well as out, and an
// OutExpo NumberAnimation restarted its deceleration on every notch, which
// reads as scrolling "in blocks... like rowing".
//
// ── Usage ───────────────────────────────────────────────────────────────
// Declare as a direct child of the Flickable/ListView/GridView it should
// drive (exactly like `timelineWheelHandler` is declared directly inside
// "timeline") so `parent` resolves to that view automatically:
//
//     Flickable {
//         id: myFlick
//         ...
//         SmoothWheelArea {}
//     }
//
// Pass `scrollTarget` explicitly only when this cannot be a direct child
// of the view (e.g. a WheelHandler nested one level deeper for z-order
// reasons).
WheelHandler {
    id: root

    // The Flickable/ListView/GridView this instance scrolls. Defaults to
    // the enclosing item — PointerHandler's own `parent` ("ParentProperty"
    // in the C++ metadata) already resolves to it when this is declared as
    // a direct child, so the common case needs no explicit wiring. Typed
    // as Flickable (not the generic Item `parent` itself is) so contentY/
    // contentHeight resolve statically; ListView/GridView are Flickable
    // subtypes, so both still assign here without a cast.
    // WALKS UP rather than casting `parent` directly. A handler declared
    // inside a Flickable is routed through flickableData and attaches to the
    // Flickable's CONTENT ITEM, not to the Flickable — so `parent as
    // Flickable` yields null, onWheel takes its early return, and because it
    // still sets `event.accepted = true` the pane becomes COMPLETELY
    // unscrollable. That shipped in 5429ab0 and broke Settings, Room
    // Information, Home and six dialogs at once; the contract test could not
    // see it because it only scans source for the component's name.
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

    // ── Axis ────────────────────────────────────────────────────────────
    //
    // This component was vertical-only until the 2026-08-28 sticker round,
    // and putting it on a HORIZONTAL view was strictly WORSE than leaving it
    // off: every bound below is derived from contentHeight/height, which on a
    // horizontal ListView are equal, so `maxContentY` is 0 — and because
    // onWheel still ends in `event.accepted = true`, the view would swallow
    // every wheel event and never move. That is the same failure shape as the
    // `parent as Flickable` bug recorded above, reached from a different
    // direction, so it is worth naming rather than discovering twice.
    //
    // "auto" is the default and cannot change any existing caller: it only
    // reads horizontal when the vertical axis has NO overflow at all (where
    // the old behaviour was to accept the event and do nothing) AND the
    // horizontal axis has some. Set it explicitly on a view whose orientation
    // is known — clearer at the call site, and immune to a transient layout
    // pass where the content has not been measured yet.
    //
    //     ListView { orientation: ListView.Horizontal
    //                SmoothWheelArea { axis: "horizontal" } }
    property string axis: "auto"
    readonly property bool horizontal:
        axis === "horizontal"
        || (axis === "auto" && scrollTarget !== null
            && scrollTarget.contentHeight <= scrollTarget.height + 0.5
            && scrollTarget.contentWidth > scrollTarget.width + 0.5)

    // The scrolled axis's extent, so the notch distance and the per-frame
    // ceiling are measured against the axis actually moving.
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

    // Far bound of the scroll range: the content minus the viewport, floored
    // at zero (content shorter than the viewport). The names keep their `Y`
    // for every existing caller and for the contract test that pins them;
    // they mean "along the scrolled axis", which is Y unless `horizontal`.
    readonly property real minContentY: 0
    readonly property real maxContentY: horizontal
        ? Math.max(0, (scrollTarget ? scrollTarget.contentWidth : 0)
                       - (scrollTarget ? scrollTarget.width : 0))
        : Math.max(0, (scrollTarget ? scrollTarget.contentHeight : 0)
                       - (scrollTarget ? scrollTarget.height : 0))

    // Local, fully independent glide state — see the header note above.
    // Never read or written by anything outside this instance.
    // Read defensively: several suites load this component with no `app`
    // context property at all, and an undefined read there would make the
    // whole area inert rather than merely un-animated.
    readonly property bool smoothScrollingEnabled: {
        // Explicitly coerced, never handed through raw. A stub `app.settings`
        // that does not carry this property yields UNDEFINED, and assigning
        // undefined to a bool is a QML warning — which the GIF picker suites
        // correctly treat as a failure. Defaulting to TRUE also keeps the
        // shipped feel for any surface whose settings object is incomplete.
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

    // Cancel any in-flight glide without moving the target. Exposed so an
    // embedding pane can stop it before a programmatic contentY jump of
    // its own (e.g. resetting scroll position on a tab switch), the same
    // way TimelinePane.qml's cancelWheelMotion() guards its own
    // programmatic navigation.
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

        // A mouse has ONE wheel and reports it on the Y axis, so a HORIZONTAL
        // view has to answer a vertical wheel or it cannot be scrolled with a
        // mouse at all. An x delta — a tilt wheel, or shift+wheel, which Qt
        // reports on x — is taken only when y carries nothing, and only on a
        // horizontal view: a vertical pane must not start scrolling sideways.
        var pixels = event.pixelDelta.y !== 0
            ? event.pixelDelta.y
            : (root.horizontal ? event.pixelDelta.x : 0)
        var angle = event.angleDelta.y !== 0
            ? event.angleDelta.y
            : (root.horizontal ? event.angleDelta.x : 0)

        if (pixels !== 0) {
            // High-resolution touchpad / precision wheel: apply the
            // platform's own delta directly, exactly like
            // TimelineScrollController::pixelTargetY — no coalesced glide
            // fights native momentum. Cancel any in-flight notch glide
            // first, same reason pixelTargetY() calls cancel().
            root.stopGlide()
            root.setScrollPosition(
                root.clampY(root.scrollPosition() - pixels, lo, hi))
        } else if (angle !== 0) {
            // Discrete mouse-wheel notch. Reuse the SAME per-notch
            // distance the timeline uses (TimelineScrollController::
            // notchDistance — a pure read, honours the user's configured
            // wheel speed) and the same proportional-delta conversion
            // Qt's own angleDelta convention implies (120 units == one
            // notch), then coalesce same-direction notches into one
            // continuous local glide instead of restarting per notch.
            var controller = (typeof app !== "undefined" && app)
                              ? app.timelineScroll : null
            var per = controller
                      ? controller.notchDistance(root.viewportExtent) : 120.0
            var deltaPixels = -(angle / 120.0) * per
            var dir = deltaPixels > 0 ? 1 : (deltaPixels < 0 ? -1 : 0)
            // Smooth scrolling OFF: land the whole notch immediately. The
            // DISTANCE is unchanged — this is not a different scroll speed,
            // it is the same movement without the glide, so a reader who
            // turns it off still travels exactly as far per notch. The
            // pixel-delta branch above is already instantaneous, so a
            // touchpad is unaffected either way.
            if (dir !== 0 && !root.smoothScrollingEnabled) {
                root.stopGlide()
                root.setScrollPosition(
                    root.clampY(root.scrollPosition() + deltaPixels, lo, hi))
            } else if (dir !== 0) {
                // Same-direction extension while a glide is already
                // running continues from its (still in-flight) target;
                // a reversal or a fresh gesture redirects from the live
                // position — identical policy to wheelTargetY().
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

    // Drives the glide with the CONTROLLER'S OWN per-frame integration
    // (motionStep) rather than a QML easing curve, so the feel is identical
    // to the room timeline instead of merely similar. A SmoothedAnimation
    // eased in as well as out; an OutExpo NumberAnimation restarted its
    // deceleration on every notch, which reads as scrolling "in blocks".
    // Coalescing is trivial here: a same-direction notch only moves
    // glideTargetY and the running ticker keeps going on one continuous
    // curve, exactly as the timeline's own does.
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
