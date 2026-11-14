import QtQuick
import QtQuick.Window

Item {
    id: root

    property Flickable view: null
    property bool inverted: false
    property var minYFunc: null
    property var maxYFunc: null
    property real deadZone: 14
    property real maxSpeed: 12000
    property real latchSlop: Math.min(deadZone, Qt.styleHints.startDragDistance)
    property int wheelCooldownMs: 500

    signal scrolled()

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
    property bool dragging: false
    property bool latched: false
    property bool travelled: false
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

    function ownsMiddleClickAt(x, y) {
        if (!view || !view.contentItem)
            return false
        var p = root.mapToItem(view.contentItem, x, y)
        return ownerIn(view.contentItem, p.x, p.y)
    }
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
        if (typeof item.linkAt === "function" && item.linkAt(x, y) !== "")
            return true
        if (item.readOnly === false && item.cursorPosition !== undefined)
            return true
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

    property real lastStepMs: 0
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
    readonly property bool hostWindowActive: Window.active === true
    onHostWindowActiveChanged: if (!hostWindowActive) stop("inactive")
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
        acceptedButtons: root.latched || root.swallowing
                         ? Qt.AllButtons : Qt.MiddleButton
        // A MouseArea claims the arrow cursor by default, which would hide the
        // I-beam and pointing hand of everything beneath the pane.
        cursorShape: undefined
        hoverEnabled: root.latched
        // The gesture's own clicks and the exit click never reach a MouseArea
        // below (browsers suppress both).
        propagateComposedEvents: false

        onPressed: (mouse) => {
            if (root.latched) {
                root.stop("click")
                root.swallowing = true
                mouse.accepted = true
                return
            }
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
        onPositionChanged: (mouse) => root.track(mouse.x, mouse.y)
        onReleased: (mouse) => {
            if (!root.dragging)
                return
            if (mouse.button === Qt.MiddleButton && !root.travelled
                    && !root.beyondSlop(mouse.x, mouse.y)
                    && area.contains(Qt.point(mouse.x, mouse.y))) {
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
        onPressedChanged: if (!pressed) root.swallowing = false
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
                root.swallowing = true
                root.stop("click")
                mouse.accepted = true
            }
            onPressedChanged: if (!pressed) root.swallowing = false
            onCanceled: root.swallowing = false
            onExited: if (root.latched) root.stop("exited")
            onWheel: (wheel) => {
                wheel.accepted = true
                if (root.latched && !wheelCooldown.running)
                    root.stop("wheel")
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: root.active
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.SizeVerCursor
        }
    }

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
