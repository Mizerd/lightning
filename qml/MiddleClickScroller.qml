import QtQuick
import QtQuick.Window

// Desktop autoscroll: hold the middle mouse button and move away from the press
// point to scroll continuously, faster the further you go; releasing ends it.
// There is deliberately no latched (click-to-toggle) mode. A latch triggered on
// ordinary middle clicks, needed all buttons and hover to see its own exit
// click (stealing clicks and hover from rows underneath), and kept scrolling
// after the pointer left the pane. A press-and-hold gesture owns a real mouse
// grab, accepts only the middle button and ends with it. Do not reintroduce the
// latch. The MouseArea accepts only the middle button and never enables hover,
// so all other interaction passes through. Usage: a sibling of the view over
// the same area, with the view passed explicitly:
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

    // Emitted after every step so a host can keep its scroll bookkeeping.
    signal scrolled()

    // Live exactly while the middle button is held; `active` is the public
    // name.
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

    // Cancellation: the gesture writes contentY directly, so anything that
    // invalidates the view, takes the pointer away or hands contentY to another
    // owner ends it (hidden, view change, focus loss, Escape, destruction).
    onVisibleChanged: if (!visible) stop()
    onViewChanged: stop()
    onEnabledChanged: if (!enabled) stop()
    Component.onDestruction: stop()
    // Alt-tabbing away mid-gesture leaves no release event.
    readonly property bool hostWindowActive: Window.active === true
    onHostWindowActiveChanged: if (!hostWindowActive) stop()
    // Escape ends the gesture. Enabled only while it runs, so it never competes
    // with the host's own Escape (two enabled ones make Qt fire neither).
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
        // Middle button only.
        acceptedButtons: Qt.MiddleButton
        // Hover stays off; the held gesture's grab delivers moves outside this
        // item.
        hoverEnabled: false
        propagateComposedEvents: true
        // No cursorShape here: a MouseArea applies its cursor whenever enabled,
        // which would replace every I-beam and link cursor over the view. The
        // gesture cursor lives on the overlay below, present only while
        // scrolling.

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
        // A quick click starts nothing: press, move, release is the gesture.
        onReleased: root.stop()
        onCanceled: root.stop()
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

    // Anchor marker at the press point, as in browsers, present only while the
    // button is held.
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
