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
// fully independent local glide via a QtQuick SmoothedAnimation. It never
// calls wheelNotch/animateTo/pixelTargetY/wheelTargetY/cancel on the
// shared controller, and never touches TimelinePane.qml or the timeline's
// own motion state. Multiple SmoothWheelArea instances (and the timeline)
// can be visible and scrolling at the same time without fighting each
// other, because each owns nothing but its own local `glide`.
//
// ── Feel-matching caveat (honest limitation) ───────────────────────────
// TimelineScrollController's motion is a hand-integrated exponential
// approach (tau=90ms, a 700px/s minimum settle speed, a 0.5px snap
// distance) — tuning constants private to that class, not exposed for
// reuse. Reimplementing that exact curve here would duplicate "the
// controller's maths" as a second, driftable copy. Instead this uses
// Qt Quick's own SmoothedAnimation (fixed-duration mode: velocity -1)
// with a duration tuned to *approximate* the same settle time for a
// typical notch. It is a deliberate best-effort visual match, not a
// bit-identical port — compare it against the timeline on a real build
// before calling the two indistinguishable.
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
    property Flickable scrollTarget: parent as Flickable

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
        root.glide.stop()
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
                var base = (root.glide.running && dir === root.glideDirection)
                           ? root.glideTargetY : t.contentY
                var newTarget = root.clampY(base + deltaPixels, lo, hi)
                root.glideTargetY = newTarget
                root.glideDirection = dir
                root.glide.to = newTarget
                if (!root.glide.running)
                    root.glide.start()
            }
        }
        event.accepted = true
    }

    // WheelHandler (like every PointerHandler) declares no default
    // property, so a nested animation must be assigned to an explicit
    // named property rather than nested anonymously.
    property SmoothedAnimation glide: SmoothedAnimation {
        target: root.scrollTarget
        property: "contentY"
        // Fixed-duration mode (velocity < 0 disables velocity-based
        // timing — see SmoothedAnimation docs): every glide settles in
        // about the same time regardless of distance, which is what
        // reading the controller's own notch-per-speed distance already
        // encodes as "how far", leaving "how long" as the only free
        // parameter here. 220ms approximates the timeline's own settle
        // window (tau=90ms with a 700px/s floor) for a typical notch;
        // see the feel-matching caveat above.
        velocity: -1
        duration: 220
        reversingMode: SmoothedAnimation.Immediate
    }

    Component.onDestruction: root.glide.stop()
}
