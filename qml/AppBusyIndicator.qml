import QtQuick
import MatrixClient

// Lightning loading spinner.
//
// Roughly eighteen BusyIndicators shipped as the unstyled Basic control,
// whose contentItem inks `palette.dark` — mapped in Main.qml to
// textSecondary, the theme's body-TEXT grey. Loading therefore never read as
// an active state anywhere in the app, and over the image viewer's 85%-black
// scrim the grey spinner was barely perceptible. `scrim: true` switches to
// the committed-light scrim ink for those hosts.
//
// Built from plain Rectangles on purpose: QtQuick.Shapes is not linked in
// this application and Canvas paints nothing here (the same constraint
// StormNode.qml, TrustCard.qml and PopupResizeGrip.qml record), so an arc is
// not available. Eight dots on a circle with an opacity ramp, rotating as
// one item, is the shape that survives that constraint and still reads as a
// spinner rather than as decoration.
Item {
    id: root

    property bool running: true
    property int size: 32
    // Contexts painting over media/scrim ink rather than a theme surface.
    property bool scrim: false
    // Explicit ink for hosts that need one (a spinner on an accent fill).
    property color color: scrim ? AppTheme.scrimInkStrong : AppTheme.accent

    readonly property real _dot: Math.max(2, size * 0.16)
    readonly property real _orbit: size / 2 - _dot / 2 - 1

    implicitWidth: size
    implicitHeight: size
    // Visibility is deliberately NOT bound to `running`, which is what the
    // stock BusyIndicator does too.
    //
    // `visible: running` looks tidy and is a trap: the stock idiom at a host
    // is `running: visible`, and the two together are a cycle. QQuickItem's
    // `visible` is EFFECTIVE visibility, so an indicator created while an
    // ancestor is hidden reads false, writes running=false, and the pair
    // latches dead with no warning and no way back — ThreadPanel is built
    // exactly that way, and three of its spinners never turned again. Hosts
    // own visibility; this owns the animation.
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
                // Ramp from nearly invisible to solid so the ring has a
                // head and a tail; without it a rotating ring of equal dots
                // looks stationary.
                opacity: 0.15 + 0.85 * (index / 7)
                x: root.size / 2 - width / 2 + Math.cos(_angle) * root._orbit
                y: root.size / 2 - height / 2 + Math.sin(_angle) * root._orbit
            }
        }

        // Animation stops with `running` AND with visibility: an indicator
        // parked inside a hidden pane must not keep the render loop awake.
        RotationAnimator on rotation {
            running: root.running && root.visible && !AppTheme.reducedMotion
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }
}
