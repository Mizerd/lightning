import QtQuick
import QtQuick.Controls
import MatrixClient

// The gradient as a bar, with one handle per colour stop (GradientEditor hosts
// it). It is deliberately dumb: it draws `spec`, reports what the user did and
// writes nothing; the editor owns the store.
//
//   * drag a handle                -> stopMoved(index, position)
//   * click the bar                -> stopAddRequested(position)   (or, when the
//                                    gradient is full, the nearest handle is
//                                    selected)
//   * press a handle               -> stopSelected(index)
//   * keyboard (the bar is a tab stop)
//       Left / Right      move the selected stop (Shift: ten times as far)
//       Home / End        send it to the start / end
//       Up / Down         select the previous / next stop
//       Delete, Backspace remove the selected stop
//       Insert, "+"       add a stop in the widest gap
//
// A handle cannot be dragged past its neighbour, so the stop order (and with
// it every stop index the picker and the stop rows use) never changes by
// dragging. A radial gradient runs from its centre (left) to its edge (right).
FocusScope {
    id: bar
    objectName: "gradientBar"

    // { type, angle, stops, positions? } or null.
    property var spec: null
    // The stop the host's picker is editing, or -1.
    property int selectedStop: -1
    property int maxStops: 3
    property int minStops: 2
    // Names a stop for assistive technology ("Start", "Middle", "End").
    property var stopNamer: function (i) { return String(i + 1) }

    signal stopSelected(int index)
    signal stopMoved(int index, real position)
    signal stopAddRequested(real position)
    signal stopRemoveRequested(int index)

    readonly property int stopCount: spec && spec.stops ? spec.stops.length : 0
    readonly property real handleWidth: 16
    readonly property real handleHeight: 34
    readonly property real trackHeight: 22
    readonly property real inner: Math.max(1, track.width - bar.handleWidth)

    // How far one arrow press moves a stop.
    readonly property real smallStep: 0.02
    readonly property real largeStep: 0.1

    implicitHeight: bar.handleHeight + 4
    implicitWidth: 240
    activeFocusOnTab: true

    function positionOf(i) {
        var n = bar.stopCount
        if (n < 2)
            return 0
        var p = bar.spec ? bar.spec.positions : undefined
        if (p && p.length === n)
            return Math.max(0, Math.min(1, p[i]))
        return i / (n - 1)
    }
    function xOf(position) {
        return bar.handleWidth / 2 + position * bar.inner
    }
    function positionAtX(x) {
        return Math.max(0, Math.min(1, (x - bar.handleWidth / 2) / bar.inner))
    }
    // The room a stop has to move in: between its neighbours.
    function lowerLimit(i) { return i > 0 ? bar.positionOf(i - 1) : 0 }
    function upperLimit(i) { return i < bar.stopCount - 1 ? bar.positionOf(i + 1) : 1 }
    function clampedMove(i, p) {
        return Math.round(Math.max(bar.lowerLimit(i), Math.min(bar.upperLimit(i), p)) * 100) / 100
    }
    function nearestStop(position) {
        var best = -1
        var bestD = 2
        for (var i = 0; i < bar.stopCount; ++i) {
            var d = Math.abs(bar.positionOf(i) - position)
            if (d < bestD) {
                bestD = d
                best = i
            }
        }
        return best
    }
    // The middle of the widest gap, where "add a stop" puts one.
    function widestGapMiddle() {
        var best = 0.5
        var bestGap = -1
        var prev = 0
        for (var i = 0; i <= bar.stopCount; ++i) {
            var p = i < bar.stopCount ? bar.positionOf(i) : 1
            if (p - prev > bestGap) {
                bestGap = p - prev
                best = (p + prev) / 2
            }
            prev = p
        }
        return Math.round(best * 100) / 100
    }
    function nudge(delta) {
        if (bar.selectedStop < 0 || bar.selectedStop >= bar.stopCount)
            return
        var i = bar.selectedStop
        var next = bar.clampedMove(i, bar.positionOf(i) + delta)
        if (next !== bar.positionOf(i))
            bar.stopMoved(i, next)
    }

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Gradient colour stops")
    Accessible.description: bar.selectedStop >= 0 && bar.selectedStop < bar.stopCount
        ? qsTr("%1 colour at %2 percent. Left and Right move it, Up and Down "
               + "pick another colour, Delete removes it, Insert adds one.")
          .arg(bar.stopNamer(bar.selectedStop))
          .arg(Math.round(bar.positionOf(bar.selectedStop) * 100))
        : qsTr("No colour picked. Up or Down picks one.")

    Keys.onPressed: (event) => {
        if (bar.stopCount === 0)
            return
        var step = (event.modifiers & Qt.ShiftModifier) ? bar.largeStep : bar.smallStep
        var handled = true
        var sel = bar.selectedStop
        switch (event.key) {
        case Qt.Key_Left:
        case Qt.Key_Right:
            if (sel < 0) {
                bar.stopSelected(0)
            } else {
                bar.nudge(event.key === Qt.Key_Left ? -step : step)
            }
            break
        case Qt.Key_Home:
            if (sel >= 0)
                bar.nudge(-1)
            break
        case Qt.Key_End:
            if (sel >= 0)
                bar.nudge(1)
            break
        case Qt.Key_Up:
            bar.stopSelected(sel <= 0 ? bar.stopCount - 1 : sel - 1)
            break
        case Qt.Key_Down:
            bar.stopSelected(sel < 0 || sel >= bar.stopCount - 1 ? 0 : sel + 1)
            break
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            if (sel >= 0 && bar.stopCount > bar.minStops)
                bar.stopRemoveRequested(sel)
            break
        case Qt.Key_Insert:
        case Qt.Key_Plus:
            if (bar.stopCount < bar.maxStops)
                bar.stopAddRequested(bar.widestGapMiddle())
            break
        default:
            handled = false
        }
        event.accepted = handled
    }

    // The gradient itself, drawn along the bar whatever the real direction.
    Rectangle {
        id: track
        objectName: "gradientBarTrack"
        x: 0
        y: (bar.height - bar.trackHeight) / 2
        width: bar.width
        height: bar.trackHeight
        radius: AppTheme.radiusSm
        color: AppTheme.editorInset
        border.width: 1
        border.color: AppTheme.editorBorderStrong

        ThemedSurface {
            anchors.fill: parent
            anchors.margins: 1
            flatFill: false
            specOverride: bar.spec ? {
                type: "linear",
                angle: 90,
                stops: bar.spec.stops,
                positions: bar.spec.positions
            } : null
        }

        // Keyboard focus ring on the whole bar.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: bar.activeFocus
            radius: AppTheme.radiusSm + 3
            color: "transparent"
            border.width: 2
            border.color: AppTheme.editorAccent
        }

        MouseArea {
            id: trackArea
            objectName: "gradientBarArea"
            anchors.fill: parent
            cursorShape: bar.stopCount < bar.maxStops ? Qt.CrossCursor
                                                       : Qt.ArrowCursor
            onClicked: (mouse) => {
                bar.forceActiveFocus()
                var p = bar.positionAtX(mouse.x)
                if (bar.stopCount < bar.maxStops)
                    bar.stopAddRequested(Math.round(p * 100) / 100)
                else
                    bar.stopSelected(bar.nearestStop(p))
            }
        }
    }

    Repeater {
        model: bar.stopCount
        delegate: Item {
            id: handle
            required property int index
            readonly property bool selected: bar.selectedStop === handle.index
            readonly property real position: bar.positionOf(handle.index)
            objectName: "gradientBarHandle_" + handle.index
            width: bar.handleWidth
            height: bar.handleHeight
            x: bar.xOf(handle.position) - width / 2
            y: (bar.height - height) / 2
            z: handle.selected ? 2 : 1

            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                color: bar.spec ? bar.spec.stops[handle.index] : "transparent"
                // A dark outer ring and a light inner keyline, so the handle
                // reads on any colour it can hold.
                border.width: handle.selected ? 3 : 2
                border.color: handle.selected ? AppTheme.editorAccent
                                              : AppTheme.editorText
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: handle.selected ? 3 : 2
                    radius: Math.max(0, AppTheme.radiusSm - 2)
                    color: "transparent"
                    border.width: 1
                    border.color: "#B0000000"
                }
            }

            MouseArea {
                id: handleArea
                objectName: "gradientBarHandleArea_" + handle.index
                anchors.fill: parent
                anchors.margins: -2
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                onPressed: {
                    bar.forceActiveFocus()
                    bar.stopSelected(handle.index)
                }
                onPositionChanged: (mouse) => {
                    if (!handleArea.pressed)
                        return
                    var pt = handleArea.mapToItem(bar, mouse.x, 0)
                    var next = bar.clampedMove(handle.index, bar.positionAtX(pt.x))
                    if (next !== handle.position)
                        bar.stopMoved(handle.index, next)
                }
            }
        }
    }
}
