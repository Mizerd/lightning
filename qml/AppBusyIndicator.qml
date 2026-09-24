import QtQuick
import MatrixClient

// Lightning loading spinner. The Basic BusyIndicator inks palette.dark (the
// body-text grey), so it never read as active and was barely visible over
// scrims. `scrim: true` uses the scrim ink for those hosts.
//
// Built from plain Rectangles because Canvas paints nothing here (as noted
// in StormNode.qml, TrustCard.qml and PopupResizeGrip.qml): eight dots on a
// circle with an opacity ramp, rotating as one item.
Item {
    id: root

    property bool running: true
    property int size: 32
    // For hosts painting over media/scrim rather than a theme surface.
    property bool scrim: false
    // Explicit ink for hosts that need one (a spinner on an accent fill).
    property color color: scrim ? AppTheme.scrimInkStrong : AppTheme.accent

    readonly property real _dot: Math.max(2, size * 0.16)
    readonly property real _orbit: size / 2 - _dot / 2 - 1

    implicitWidth: size
    implicitHeight: size
    // Visibility is not bound to `running`, as with the stock BusyIndicator.
    // Hosts commonly bind `running: visible`, and since `visible` is effective
    // visibility, the pair would latch off for an indicator created under a
    // hidden ancestor. Hosts own visibility; this owns the animation.
    Accessible.role: Accessible.Indicator
    Accessible.name: qsTr("Loading")

    Item {
        id: ring
        anchors.fill: parent

        Repeater {
            model: 8
            delegate: Rectangle {
                required property int index
                readonly property real _angle: index * Math.PI / 4
                width: root._dot
                height: root._dot
                radius: width / 2
                color: root.color
                // Ramp from faint to solid so the ring has a head and a tail
                // and visibly rotates.
                opacity: 0.15 + 0.85 * (index / 7)
                x: root.size / 2 - width / 2 + Math.cos(_angle) * root._orbit
                y: root.size / 2 - height / 2 + Math.sin(_angle) * root._orbit
            }
        }

        // Stops with `running` and with visibility, so a spinner in a hidden
        // pane doesn't keep the render loop awake.
        RotationAnimator on rotation {
            running: root.running && root.visible && !AppTheme.reducedMotion
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }
}
