import QtQuick
import QtQuick.Window

// MiddleClickScroller — the desktop "autoscroll" gesture every browser and
// most native clients have: press and HOLD the MIDDLE mouse button, then move
// the pointer away from the press point to scroll continuously, faster the
// further you move. Releasing the button ends the gesture.
//
// 2026-08-18 tester report: "still no middle click scroll".
//
// ── There is deliberately NO latched mode ────────────────────────────────
// The first version latched on a quick click (`pressMs < 350 && !travelled`),
// the Windows/Firefox convention. Its 50 ms hold clock meant an ORDINARY
// middle click reported pressMs === 0, so every short middle click latched —
// the reported "quick middle click makes the timeline scroll on its own".
// Worse, the latched mode had to take Qt.AllButtons and hoverEnabled to see
// its own exit click, which stole the next click from the timeline and hover
// from every message row underneath, and a latched pointer that left the pane
// kept scrolling at its last inside speed with nothing left to update it.
// A press-and-hold gesture has none of those problems: it owns a real mouse
// grab (so moves outside the item still arrive), it accepts only the middle
// button, and it ends when the button does. Do not reintroduce the latch.
//
// ── How it stays out of everything else's way ────────────────────────────
// The MouseArea accepts ONLY the middle button and never enables hover, so
// left clicks, text selection, the hover action bar and every other pointer
// interaction pass straight through to the items underneath.
//
// ── Usage ────────────────────────────────────────────────────────────────
// Declare it as a SIBLING of the view, over the same area, and pass the view
// explicitly:
//
//     Item {
//         Flickable { id: myFlick; ... }
//         MiddleClickScroller { anchors.fill: parent; view: myFlick }
//     }
//
// `view` is never derived from `parent`: the room timeline's Flickable is
// rotated 180 degrees, so this cannot live inside it, and a silent
// `parent as Flickable` null is exactly the trap that left nine panes
// without smooth wheel scrolling in the 2026-08-16 round.
Item {
    id: root

    // The Flickable/ListView/GridView to scroll. Required.
    property Flickable view: null
    // Set for a view whose content is rotated 180 degrees (the room
    // timeline): moving the pointer DOWN must still scroll towards newer
    // messages, which is DECREASING contentY there.
    property bool inverted: false
    // Clamp hooks. Default to the plain Flickable range; the timeline passes
    // its own wheelMinY()/wheelMaxY() so this obeys exactly the same bounds
    // as the wheel path.
    property var minYFunc: null
    property var maxYFunc: null
    // Pointer travel (px) that is ignored around the anchor, and the travel
    // at which the speed reaches maxSpeed.
    property real deadZone: 14
    property real fullSpeedDistance: 220
    property real maxSpeed: 2200  // px per second

    // Emitted after every applied step, so a host with its own scroll
    // bookkeeping (pagination, stick-to-bottom) can keep up.
    signal scrolled()

    // One state, not two: the gesture is live exactly while the middle
    // button is held. `active` is kept as the public name every host and
    // test already uses.
    readonly property bool active: dragging
    property bool dragging: false
    property real anchorX: 0
    property real anchorY: 0
    property real pointerY: 0

    function stop() {
        dragging = false
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

    function step(dtMs) {
        if (!view || !active)
            return
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
        if (Math.abs(target - view.contentY) < 0.01)
            return
        view.contentY = target
        root.scrolled()
    }

    // ── The complete cancellation set ────────────────────────────────────
    // The gesture writes view.contentY directly, so anything that
    // invalidates the view, takes the pointer away, or hands contentY to
    // another owner has to end it. Before this list existed the only exits
    // were the next press, onCanceled, onVisibleChanged and onViewChanged —
    // no key, no focus loss, no destruction — which is how a stuck gesture
    // could outlive the surface it was started on.
    onVisibleChanged: if (!visible) stop()
    onViewChanged: stop()
    onEnabledChanged: if (!enabled) stop()
    Component.onDestruction: stop()
    // Alt-tabbing away mid-gesture leaves no release event behind.
    readonly property bool hostWindowActive: Window.active === true
    onHostWindowActiveChanged: if (!hostWindowActive) stop()
    // Escape is the conventional way out of an autoscroll. Enabled ONLY
    // while the gesture runs, so it never competes with the host's own
    // Escape handling (an always-enabled duplicate makes Qt report an
    // ambiguous shortcut and fire NEITHER).
    Shortcut {
        sequence: "Escape"
        enabled: root.active
        onActivated: root.stop()
    }

    Timer {
        id: ticker
        interval: 16
        repeat: true
        running: root.active
        onTriggered: root.step(interval)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        // Middle button only, always: nothing else in the view is affected,
        // and there is no latched state left that would need to see a
        // different button to exit.
        acceptedButtons: Qt.MiddleButton
        // Hover stays OFF. A held gesture owns the mouse grab, so move
        // events arrive even outside this item's bounds — including the
        // ones that used to be lost when a latched pointer left the pane.
        hoverEnabled: false
        propagateComposedEvents: true
        // NO cursorShape here: MouseArea applies its cursor whenever it is
        // enabled, regardless of acceptedButtons, and this area covers the
        // whole view — an idle scroller would replace the text I-beam and
        // every link's pointing hand underneath it with a plain arrow. The
        // gesture cursor lives on the overlay below, which only exists
        // while the gesture is running.

        onPressed: (mouse) => {
            if (mouse.button !== Qt.MiddleButton) {
                mouse.accepted = false
                return
            }
            root.anchorX = mouse.x
            root.anchorY = mouse.y
            root.pointerY = mouse.y
            root.dragging = true
            mouse.accepted = true
        }
        onPositionChanged: (mouse) => {
            if (root.active)
                root.pointerY = mouse.y
        }
        // A quick click does nothing visible and starts no motion: press,
        // move, release IS the whole gesture.
        onReleased: root.stop()
        onCanceled: root.stop()
    }

    // Gesture cursor, present ONLY while scrolling (see the MouseArea).
    Item {
        anchors.fill: parent
        visible: root.active
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.SizeVerCursor
        }
    }

    // The anchor marker: a small ring at the press point, exactly like the
    // browser convention, so the gesture is visible rather than a
    // mysteriously scrolling view. It exists only while the button is held,
    // which is also what makes "a quick click leaves no marker behind" a
    // property a test can assert on.
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
