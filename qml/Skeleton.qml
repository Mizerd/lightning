import QtQuick
import MatrixClient

Rectangle {
    id: root

    property bool circle: false
    property bool active: true
    // A shape that is not a plain rectangle (an avatar's disc or rounded
    // square) cannot carry the sweeping band: `clip` clips to the item's
    // bounding rectangle, not its radius, so the band's square ends showed
    // over the corners as a square placeholder. Such skeletons pulse their
    // own fill instead, which is inside the shape by construction.
    property bool pulseOnly: circle
    property bool shimmer: !AppTheme.reducedMotion

    readonly property bool animating: active && shimmer && visible
                                      && Window.window !== null
                                      && Window.window.visible

    radius: circle ? Math.min(width, height) / 2 : AppTheme.radiusSm
    color: AppTheme.cardElevated
    clip: true

    Accessible.ignored: true

    Loader {
        active: root.animating && root.pulseOnly
        visible: false
        sourceComponent: SequentialAnimation {
            running: true
            loops: Animation.Infinite
            NumberAnimation {
                target: root
                property: "opacity"
                to: 0.55
                duration: 700
                easing.type: Easing.InOutSine
            }
            NumberAnimation {
                target: root
                property: "opacity"
                to: 1.0
                duration: 700
                easing.type: Easing.InOutSine
            }
            Component.onDestruction: root.opacity = 1.0
        }
    }

    Loader {
        active: root.animating && !root.pulseOnly
        visible: active
        sourceComponent: Rectangle {
            id: band
            width: Math.max(24, root.width * 0.35)
            height: root.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop {
                    position: 0.5
                    color: Qt.alpha(AppTheme.text, 0.08)
                }
                GradientStop { position: 1.0; color: "transparent" }
            }
            NumberAnimation on x {
                from: -band.width
                to: root.width
                duration: 1100
                loops: Animation.Infinite
            }
        }
    }
}
