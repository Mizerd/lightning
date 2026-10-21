import QtQuick
import QtQuick.Window

// Desktop autoscroll, as browsers do it (Firefox, Chromium on Windows).
// Middle-press and move away from the press point to scroll, faster the
// further you go, on Firefox's curve (speedAt()).
// - Press, move, release: a hold. The release ends it.
// - A middle click that did not travel past latchSlop LATCHES it: moving the
//   pointer scrolls with no button held, and the next click of any button ends
//   it and goes no further. Escape, the wheel, focus loss, the pointer leaving
//   the WINDOW, hiding, a view change and destruction end it too; hosts end it
//   on navigation through stop().
// Hold or latch is decided by travel alone, as in both browsers. There is no
// time limit: a press clock is what made every ordinary click latch in the
// first version (82794ed9).
// Nothing starts over a link, editable text or a MouseArea that takes the
// middle button, or when the view has nothing to scroll: the press is not
// accepted, so middle click stays theirs (browsers do the same; on Linux it is
// the primary-selection paste).
// Idle, the area takes the middle button only and never hovers, so everything
// else passes through. While latched a catcher covering the whole window takes
// every button and tracks hover, so the pointer steers from anywhere in the
// window, as in browsers: moving it onto the header or the composer to go
// faster no longer ends the scroll (C26, measured `end=exited` 0.9 s into a
// gesture). Leaving the window still ends it, so it never runs on a position
// it can no longer see (2026-08-21).
//
// Usage: a sibling of the view over the same area, with the view passed
// explicitly:
//
//     Item {
//         Flickable { id: myFlick; ... }
//         MiddleClickScroller { anchors.fill: parent; view: myFlick }
//     }
//
// `view` is never derived from `parent`: the timeline's Flickable is rotated,
// so this cannot live inside it.
Item {
    id: root

    // The Flickable/ListView/GridView to scroll. Required.
    property Flickable view: null
    // For a view rotated 180° (the timeline): moving down must still scroll
    // toward newer messages, which is decreasing contentY there.
    property bool inverted: false
    // Clamp hooks. Default to the plain Flickable range; the timeline passes
    // its wheelMinY()/wheelMaxY() so this obeys the wheel's bounds.
    property var minYFunc: null
    property var maxYFunc: null
    // Travel (px) ignored around the anchor.
    property real deadZone: 14
    // Ceiling (px per second). Firefox has none; this one only stops a pointer
    // flung to the far edge of a tall window from outrunning the layout.
    property real maxSpeed: 12000
    // Travel (px, either axis) that makes a press a hold rather than a click.
    // Qt's drag distance (10 by default, Firefox's figure), never above the
    // dead zone, so a fresh latch sits still.
    property real latchSlop: Math.min(deadZone, Qt.styleHints.startDragDistance)
    // A wheel this soon after latching is swallowed without ending it:
    // pressing a wheel button can send a stray notch (Firefox: 500 ms).
    property int wheelCooldownMs: 500

    // Emitted after every step so a host can keep its scroll bookkeeping.
    signal scrolled()

    // Opt-in diagnostics (the timeline passes LIGHTNING_SCROLL_TRACE through):
    // one `autoscroll-gesture` line per gesture, counts and pixels only.
    property bool trace: false
    property real traceStartMs: 0
    property int traceSteps: 0
    property int tracePinnedSteps: 0
    property real traceMovedPx: 0
    property real traceMaxSpeed: 0
    property real traceMaxTravel: 0
    property bool traceWasLatched: false
    function traceBegin() {
        traceStartMs = Date.now()
        traceSteps = 0
        tracePinnedSteps = 0
        traceMovedPx = 0
        traceMaxSpeed = 0
        traceMaxTravel = 0
        traceWasLatched = false
    }
    function traceEnd(reason) {
        if (!trace || traceStartMs <= 0)
            return
        var ms = Math.max(1, Date.now() - traceStartMs)
        console.info("autoscroll-gesture"
            + " mode=" + (traceWasLatched ? "latched" : "held")
            + " end=" + (reason ? reason : "host")
            + " ms=" + Math.round(ms)
            + " steps=" + traceSteps
            + " pinnedSteps=" + tracePinnedSteps
            + " movedPx=" + Math.round(traceMovedPx)
            + " avgPxPerS=" + Math.round(traceMovedPx * 1000 / ms)
            + " maxPxPerS=" + Math.round(traceMaxSpeed)
            + " maxTravel=" + Math.round(traceMaxTravel))
        traceStartMs = 0
    }

    readonly property bool active: dragging || latched
    // The middle button is held.
    property bool dragging: false
    // Released without travelling: scrolling with no button held.
    property bool latched: false
    // The held press went past latchSlop at some point, so its release ends
    // it. Sticky: out and back is still a drag.
    property bool travelled: false
    // The press that ended a latch is still down: keep every button until its
    // release, so that release reaches this area and nothing below.
    property bool swallowing: false
    property real anchorX: 0
    property real anchorY: 0
    property real pointerX: 0
    property real pointerY: 0

    function stop(reason) {
        if (active)
            traceEnd(reason)
        latched = false
        dragging = false
        travelled = false
    }

    function track(x, y) {
        if (!active)
            return
        pointerX = x
        pointerY = y
        if (dragging && beyondSlop(x, y))
            travelled = true
    }

    function beyondSlop(x, y) {
        return Math.abs(x - anchorX) > latchSlop
               || Math.abs(y - anchorY) > latchSlop
    }

    // True when something under (x, y) in the view owns the middle button.
    function ownsMiddleClickAt(x, y) {
        if (!view || !view.contentItem)
            return false
        var p = root.mapToItem(view.contentItem, x, y)
        return ownerIn(view.contentItem, p.x, p.y)
    }
    // Depth first through the children under the point, topmost first.
    function ownerIn(item, x, y) {
        var kids = item.children
        for (var i = kids.length - 1; i >= 0; --i) {
            var kid = kids[i]
            if (!kid.visible || !kid.enabled)
                continue
            var p = item.mapToItem(kid, x, y)
            if (!kid.contains(p))
                continue
            if (ownerIn(kid, p.x, p.y))
                return true
        }
        return ownsMiddleClick(item, x, y)
    }
    function ownsMiddleClick(item, x, y) {
        // A link (Text, TextEdit, Label).
        if (typeof item.linkAt === "function" && item.linkAt(x, y) !== "")
            return true
        // Editable text: middle click pastes the primary selection there.
        if (item.readOnly === false && item.cursorPosition !== undefined)
            return true
        // Anything that asked for the middle button itself.
        return item.acceptedButtons !== undefined
               && (item.acceptedButtons & Qt.MiddleButton) !== 0
    }

    function rangeMin() {
        if (minYFunc)
            return minYFunc()
        return view ? view.originY : 0
    }
    function rangeMax() {
        if (maxYFunc)
            return maxYFunc()
        if (!view)
            return 0
        return view.originY + Math.max(0, view.contentHeight - view.height)
    }

    // Speed (px per second) for a pointer `travel` px from the anchor. Firefox's
    // autoscroll curve (AutoScrollChild: ((d / 12)^1.5 - 1) px per 20 ms):
    // 375 px/s at 50 px, 1150 at 100, 3350 at 200, 6200 at 300. The previous
    // squared ramp gave 59 / 336 / 1573 and stopped at 2200, which is what
    // read as "notably slower than the wheel" (C26).
    function speedAt(travel) {
        var d = Math.abs(travel)
        if (d <= deadZone)
            return 0
        var v = d / 12
        var speed = (v * Math.sqrt(v) - 1) * 50
        return Math.max(0, Math.min(maxSpeed, speed))
    }

    function canScroll() {
        return view !== null && rangeMax() - rangeMin() >= 1
    }

    // Wall-clock of the previous step. A Timer fires late when frames are
    // slow, and advancing a fixed 16 ms per fire made the scroll slow down
    // exactly when the view was struggling.
    property real lastStepMs: 0
    // Longest interval one step may cover, so a stall cannot become a leap.
    property real maxStepMs: 100

    function elapsedSinceLastStep(nominalMs) {
        var now = Date.now()
        var dt = lastStepMs > 0 ? now - lastStepMs : nominalMs
        lastStepMs = now
        return Math.max(1, Math.min(maxStepMs, dt))
    }

    function step(dtMs) {
        if (!view || !active) {
            lastStepMs = 0
            return
        }
        var travel = pointerY - anchorY
        var speed = speedAt(travel)
        if (speed <= 0)
            return
        if (trace) {
            traceSteps += 1
            if (speed > traceMaxSpeed)
                traceMaxSpeed = speed
            if (Math.abs(travel) > traceMaxTravel)
                traceMaxTravel = Math.abs(travel)
        }
        var delta = (travel < 0 ? -1 : 1) * speed * (dtMs / 1000)
        if (inverted)
            delta = -delta
        var lo = rangeMin()
        var hi = rangeMax()
        var target = view.contentY + delta
        if (target < lo) target = lo
        if (target > hi) target = hi
        if (Math.abs(target - view.contentY) < 0.01) {
            if (trace)
                tracePinnedSteps += 1
            // Pinned against an end while still asking for more: the host
            // must keep hearing about it, or a reader held at the oldest row
            // never asks for older history.
            root.scrolled()
            return
        }
        if (trace)
            traceMovedPx += Math.abs(target - view.contentY)
        view.contentY = target
        root.scrolled()
    }

    // Cancellation: the gesture writes contentY directly, so anything that
    // invalidates the view, takes the pointer away or hands contentY to another
    // owner ends it (hidden, view change, focus loss, Escape, destruction).
    onVisibleChanged: if (!visible) stop("hidden")
    onViewChanged: stop("view")
    onEnabledChanged: if (!enabled) stop("disabled")
    Component.onDestruction: stop("destroyed")
    // Alt-tabbing away leaves no release or click event.
    readonly property bool hostWindowActive: Window.active === true
    onHostWindowActiveChanged: if (!hostWindowActive) stop("inactive")
    // Escape ends the gesture. Enabled only while it runs, so it never competes
    // with the host's own Escape (two enabled ones make Qt fire neither, only
    // an ambiguous activation in turn, which ends it too).
    Shortcut {
        sequence: "Escape"
        enabled: root.active
        onActivated: root.stop("escape")
        onActivatedAmbiguously: root.stop("escape")
    }

    Timer {
        id: ticker
        interval: 16
        repeat: true
        running: root.active
        onRunningChanged: root.lastStepMs = 0
        onTriggered: root.step(root.elapsedSinceLastStep(interval))
    }

    Timer {
        id: wheelCooldown
        interval: root.wheelCooldownMs
    }

    MouseArea {
        id: area
        objectName: "autoscrollMouseArea"
        anchors.fill: parent
        // Idle: middle button only. Latched: every button, so the exit click
        // lands here and nowhere else.
        acceptedButtons: root.latched || root.swallowing
                         ? Qt.AllButtons : Qt.MiddleButton
        // Hover only while latched: nothing is held, so hover is the only way
        // to see the pointer, and it reports leaving the pane. (A MouseArea
        // does not block hover, so rows keep theirs either way.)
        hoverEnabled: root.latched
        // The gesture's own clicks and the exit click never reach a MouseArea
        // below (browsers suppress both).
        propagateComposedEvents: false
        // No cursorShape here: a MouseArea applies its cursor whenever enabled,
        // which would replace every I-beam and link cursor over the view. The
        // gesture cursor lives on the overlay below, present only while
        // scrolling.

        onPressed: (mouse) => {
            if (root.latched) {
                // Any button ends a latched scroll and goes no further.
                root.stop("click")
                root.swallowing = true
                mouse.accepted = true
                return
            }
            // Nothing to scroll, or middle click is someone else's: start
            // nothing and let the press through.
            if (mouse.button !== Qt.MiddleButton || !root.canScroll()
                    || root.ownsMiddleClickAt(mouse.x, mouse.y)) {
                mouse.accepted = false
                return
            }
            root.anchorX = mouse.x
            root.anchorY = mouse.y
            root.pointerX = mouse.x
            root.pointerY = mouse.y
            root.travelled = false
            if (root.trace)
                root.traceBegin()
            root.dragging = true
            mouse.accepted = true
        }
        // Held: moves under the grab. Latched: hover moves.
        onPositionChanged: (mouse) => root.track(mouse.x, mouse.y)
        onReleased: (mouse) => {
            if (!root.dragging)
                return
            if (mouse.button === Qt.MiddleButton && !root.travelled
                    && !root.beyondSlop(mouse.x, mouse.y)
                    && area.contains(Qt.point(mouse.x, mouse.y))) {
                // A click, not a drag: latch. Set before dragging clears so
                // `active` never blinks off.
                root.pointerX = mouse.x
                root.pointerY = mouse.y
                root.latched = true
                root.traceWasLatched = true
                root.dragging = false
                wheelCooldown.restart()
                return
            }
            root.stop("released")
        }
        onCanceled: root.stop("canceled")
        // The exit press is over, released or cancelled.
        onPressedChanged: if (!pressed) root.swallowing = false
        // A latched scroll is tracked by windowCatcher below once it has a
        // window; without one (no window yet), leaving the pane ends it. A
        // held one keeps its grab and ends with the button.
        onExited: if (root.latched && !windowCatcher.engaged)
                      root.stop("exited")
        onWheel: (wheel) => {
            if (!root.latched) {
                wheel.accepted = false
                return
            }
            wheel.accepted = true
            if (!wheelCooldown.running)
                root.stop("wheel")
        }
    }

    // While latched: one area over the whole window, so the pointer keeps
    // steering wherever it goes in the window and the exit click lands here.
    // Parented to the window's content item only while needed; idle it sits
    // inside this item, hidden, and takes nothing.
    // z: above every ordinary item of the window (the timeline's own scroller
    // is z 30 within its pane). Qt Quick Controls Popups are hosted by the
    // window's Overlay layer, which is meant to stack above ordinary content,
    // so an ALREADY open popup stays above this and keeps its own input; the
    // pointer moving onto one is a hover exit from here and ends the latch,
    // which is the cautious answer (not measured). No popup can be opened
    // through this area: it swallows every click while latched.
    Item {
        id: windowCatcher
        readonly property Item windowRoot: root.Window.contentItem
        readonly property bool engaged: (root.latched || root.swallowing)
                                        && windowRoot !== null
        parent: engaged ? windowRoot : root
        anchors.fill: parent
        z: 1000000
        visible: engaged
        MouseArea {
            id: catcherArea
            objectName: "autoscrollWindowCatcher"
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            propagateComposedEvents: false
            cursorShape: Qt.SizeVerCursor
            onPositionChanged: (mouse) => {
                if (!root.latched)
                    return
                var p = catcherArea.mapToItem(root, mouse.x, mouse.y)
                root.track(p.x, p.y)
            }
            onPressed: (mouse) => {
                // Any button ends a latched scroll and goes no further; keep
                // every button until this press is released.
                root.swallowing = true
                root.stop("click")
                mouse.accepted = true
            }
            onPressedChanged: if (!pressed) root.swallowing = false
            onCanceled: root.swallowing = false
            // Leaving the window: the pointer can no longer be seen.
            onExited: if (root.latched) root.stop("exited")
            onWheel: (wheel) => {
                wheel.accepted = true
                if (root.latched && !wheelCooldown.running)
                    root.stop("wheel")
            }
        }
    }

    // Gesture cursor, present only while scrolling.
    Item {
        anchors.fill: parent
        visible: root.active
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.SizeVerCursor
        }
    }

    // Anchor marker at the press point, as in browsers, present while the
    // gesture runs.
    Rectangle {
        objectName: "autoscrollAnchorMarker"
        visible: root.active
        x: root.anchorX - width / 2
        y: root.anchorY - height / 2
        width: 26
        height: 26
        radius: 13
        color: AppTheme.surface
        opacity: 0.9
        border.width: 1
        border.color: AppTheme.borderStrong
        Rectangle {
            anchors.centerIn: parent
            width: 4
            height: 4
            radius: 2
            color: AppTheme.accent
        }
    }
}
