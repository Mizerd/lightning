import QtQuick
import MatrixClient

// Shared loading skeleton for deferred content (images, GIFs, video thumbnails,
// link previews, decrypting text, avatars): a neutral surface with an optional
// soft band sweeping left to right. The band exists only while animating, which
// happens only when `active`, visible, in a visible window, and reduced motion
// is off. Never intercepts input; not in the accessibility tree.
Rectangle {
    id: root

    // Circle for avatars, rounded rectangle for everything else.
    property bool circle: false
    // Callers gate animation on their own on-screen knowledge (a cached
    // ListView row is visible but off screen).
    property bool active: true
    // Static under reduced motion.
    property bool shimmer: !AppTheme.reducedMotion

    readonly property bool animating: active && shimmer && visible
                                      && Window.window !== null
                                      && Window.window.visible

    radius: circle ? Math.min(width, height) / 2 : AppTheme.radiusSm
    color: AppTheme.cardElevated
    clip: true

    Accessible.ignored: true

    Rectangle {
        id: band
        visible: root.animating
        width: Math.max(24, root.width * 0.35)
        height: root.height
        // A band in the theme's text colour at low alpha: a gentle darkening on
        // light themes, lightening on dark ones.
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
            running: root.animating
            from: -band.width
            to: root.width
            duration: 1100
            loops: Animation.Infinite
        }
    }
}
