import QtQuick
import QtQuick.Window

// Desktop autoscroll, as browsers do it (Firefox, Chromium on Windows).
// Middle-press and move away from the press point to scroll, faster the
// further you go.
// - Press, move, release: a hold. The release ends it.
// - A middle click that did not travel past latchSlop LATCHES it: moving the
//   pointer scrolls with no button held, and the next click of any button ends
//   it and goes no further. Escape, the wheel, focus loss, the pointer leaving
//   the pane, hiding, a view change and destruction end it too; hosts end it on
//   navigation through stop().
// Hold or latch is decided by travel alone, as in both browsers. There is no
// time limit: a press clock is what made every ordinary click latch in the
// first version (82794ed9).
// Nothing starts over a link, editable text or a MouseArea that takes the
// middle button, or when the view has nothing to scroll: the press is not
// accepted, so middle click stays theirs (browsers do the same; on Linux it is
// the primary-selection paste).
// Idle, the area takes the middle button only and never hovers, so everything
// else passes through. It takes every button, and tracks hover, only while
// latched, and a latched scroll stops when the pointer leaves the pane, so it
// never runs on a position it can no longer see.
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
    // Travel (px) ignored around the anchor, and the travel at which speed
    // reaches maxSpeed.
    property real deadZone: 14
    property real fullSpeedDistance: 220
    property real maxSpeed: 2200  // px per second
    // Travel (px, either axis) that makes a press a hold rather than a click.
    // Qt's drag distance (10 by default, Firefox's figure), never above the
    // dead zone, so a fresh latch sits still.
    property real latchSlop: Math.min(deadZone, Qt.styleHints.startDragDistance)
    // A wheel this soon after latching is swallowed without ending it:
    // pressing a wheel button can send a stray notch (Firefox: 500 ms).
    property int wheelCooldownMs: 500

    // Emitted after every step so a host can keep its scroll bookkeeping.
    signal scrolled()

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

    function stop() {
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
        var magnitude = Math.abs(travel) - deadZone
        if (magnitude <= 0)
            return
        var ratio = Math.min(1, magnitude / Math.max(1, fullSpeedDistance))
        // Squared response: precise near the anchor, fast at the edges.
        var speed = ratio * ratio * maxSpeed
        var delta = (travel < 0 ? -1 : 1) * speed * (dtMs / 1000)
        if (inverted)
            delta = -delta
        var lo = rangeMin()
        var hi = rangeMax()
        var target = view.contentY + delta
        if (target < lo) target = lo
        if (target > hi) target = hi
        if (Math.abs(target - view.contentY) < 0.01) {
            // Pinned against an end while still asking for more: the host
            // must keep hearing about it, or a reader held at the oldest row
            // never asks for older history.
            root.scrolled()
            return
        }
        view.contentY = target
        root.scrolled()
    }

    // Cancellation: the gesture writes contentY directly, so anything that
    // invalidates the view, takes the pointer away or hands contentY to another
    // owner ends it (hidden, view change, focus loss, Escape, destruction).
    onVisibleChanged: if (!visible) stop()
    onViewChanged: stop()
    onEnabledChanged: if (!enabled) stop()
    Component.onDestruction: stop()
    // Alt-tabbing away leaves no release or click event.
    readonly property bool hostWindowActive: Window.active === true
    onHostWindowActiveChanged: if (!hostWindowActive) stop()
    // Escape ends the gesture. Enabled only while it runs, so it never competes
    // with the host's own Escape (two enabled ones make Qt fire neither, only
    // an ambiguous activation in turn, which ends it too).
    Shortcut {
        sequence: "Escape"
        enabled: root.active
        onActivated: root.stop()
        onActivatedAmbiguously: root.stop()
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
                root.stop()
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
                root.dragging = false
                wheelCooldown.restart()
                return
            }
            root.stop()
        }
        onCanceled: root.stop()
        // The exit press is over, released or cancelled.
        onPressedChanged: if (!pressed) root.swallowing = false
        // Leaving the pane (or the window) ends a latched scroll. A held one
        // keeps its grab and ends with the button.
        onExited: if (root.latched) root.stop()
        onWheel: (wheel) => {
            if (!root.latched) {
                wheel.accepted = false
                return
            }
            wheel.accepted = true
            if (!wheelCooldown.running)
                root.stop()
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
