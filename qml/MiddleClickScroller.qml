import QtQuick

// MiddleClickScroller — the desktop "autoscroll" gesture every browser and
// most native clients have: press the MIDDLE mouse button, then move the
// pointer away from the press point to scroll continuously, faster the
// further you move. Releasing it ends the gesture; a quick middle-click
// without moving LATCHES the mode on (Windows/Firefox behaviour) until the
// next button press.
//
// 2026-08-18 tester report: "still no middle click scroll".
//
// ── How it stays out of everything else's way ────────────────────────────
// The MouseArea accepts ONLY the middle button while idle, so left clicks,
// text selection, the hover action bar and every other pointer interaction
// pass straight through to the items underneath. Hover tracking is switched
// on ONLY while the gesture is live (a latched gesture has no pressed grab
// to deliver move events, so it needs hover) and off again the moment it
// ends, which is what keeps the message rows' own hover behaviour intact.
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

    readonly property bool active: dragging || latched
    property bool dragging: false
    property bool latched: false
    property real anchorX: 0
    property real anchorY: 0
    property real pointerY: 0
    property real pressMs: 0

    function stop() {
        dragging = false
        latched = false
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

    // Any view change that invalidates the gesture ends it rather than
    // leaving an invisible latched mode behind.
    onVisibleChanged: if (!visible) stop()
    onViewChanged: stop()

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
        // Idle: middle button only, so nothing else in the view is affected.
        // Latched: any button ends the mode, which is the conventional way
        // out of autoscroll.
        acceptedButtons: root.latched ? Qt.AllButtons : Qt.MiddleButton
        // Hover is needed only to track the pointer of a LATCHED gesture;
        // enabling it permanently would take hover away from the rows.
        hoverEnabled: root.latched
        propagateComposedEvents: true
        // NO cursorShape here: MouseArea applies its cursor whenever it is
        // enabled, regardless of acceptedButtons, and this area covers the
        // whole view — an idle scroller would replace the text I-beam and
        // every link's pointing hand underneath it with a plain arrow. The
        // gesture cursor lives on the overlay below, which only exists
        // while the gesture is running.

        onPressed: (mouse) => {
            if (root.latched) {
                root.stop()
                mouse.accepted = true
                return
            }
            if (mouse.button !== Qt.MiddleButton) {
                mouse.accepted = false
                return
            }
            root.anchorX = mouse.x
            root.anchorY = mouse.y
            root.pointerY = mouse.y
            root.pressMs = 0
            root.dragging = true
            holdClock.restart()
            mouse.accepted = true
        }
        onPositionChanged: (mouse) => {
            if (root.active)
                root.pointerY = mouse.y
        }
        onReleased: (mouse) => {
            if (!root.dragging)
                return
            holdClock.stop()
            root.dragging = false
            // A quick click that did not travel latches the mode on; a
            // press-and-drag simply ends with the release.
            var travelled = Math.abs(mouse.y - root.anchorY) > 6
                            || Math.abs(mouse.x - root.anchorX) > 6
            if (!travelled && root.pressMs < 350) {
                root.pointerY = mouse.y
                root.latched = true
            }
        }
        onCanceled: root.stop()
    }

    // Press duration, measured without Date.now() so the component stays
    // usable in tests that forbid wall-clock reads.
    Timer {
        id: holdClock
        interval: 50
        repeat: true
        running: root.dragging
        onTriggered: root.pressMs += interval
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
    // browser convention, so a latched gesture is visible rather than a
    // mysteriously scrolling view.
    Rectangle {
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
