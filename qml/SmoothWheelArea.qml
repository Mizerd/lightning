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

    // Bottom bound of the vertical scroll range. All current callers are
    // plain (non-rotated) Flickables, so this is simply the content minus
    // the viewport, floored at zero (content shorter than the viewport).
    readonly property real minContentY: 0
    readonly property real maxContentY:
        Math.max(0, (scrollTarget ? scrollTarget.contentHeight : 0)
                     - (scrollTarget ? scrollTarget.height : 0))

    // Local, fully independent glide state — see the header note above.
    // Never read or written by anything outside this instance.
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

        if (event.pixelDelta.y !== 0) {
            // High-resolution touchpad / precision wheel: apply the
            // platform's own delta directly, exactly like
            // TimelineScrollController::pixelTargetY — no coalesced glide
            // fights native momentum. Cancel any in-flight notch glide
            // first, same reason pixelTargetY() calls cancel().
            root.stopGlide()
            t.contentY = root.clampY(t.contentY - event.pixelDelta.y, lo, hi)
        } else if (event.angleDelta.y !== 0) {
            // Discrete mouse-wheel notch. Reuse the SAME per-notch
            // distance the timeline uses (TimelineScrollController::
            // notchDistance — a pure read, honours the user's configured
            // wheel speed) and the same proportional-delta conversion
            // Qt's own angleDelta convention implies (120 units == one
            // notch), then coalesce same-direction notches into one
            // continuous local glide instead of restarting per notch.
            var controller = (typeof app !== "undefined" && app)
                              ? app.timelineScroll : null
            var per = controller ? controller.notchDistance(t.height) : 120.0
            var deltaPixels = -(event.angleDelta.y / 120.0) * per
            var dir = deltaPixels > 0 ? 1 : (deltaPixels < 0 ? -1 : 0)
            if (dir !== 0) {
                // Same-direction extension while a glide is already
                // running continues from its (still in-flight) target;
                // a reversal or a fresh gesture redirects from the live
                // position — identical policy to wheelTargetY().
                var base = (root.ticker.running && dir === root.glideDirection)
                           ? root.glideTargetY : t.contentY
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
            var remaining = root.glideTargetY - t.contentY
            if (!controller || Math.abs(remaining) <= 0.5) {
                t.contentY = root.clampY(root.glideTargetY,
                                         root.minContentY, root.maxContentY)
                root.glideDirection = 0
                stop()
                return
            }
            t.contentY = root.clampY(
                t.contentY + controller.motionStep(remaining, interval,
                                                   t.height),
                root.minContentY, root.maxContentY)
        }
    }

    Component.onDestruction: root.ticker.stop()
}
