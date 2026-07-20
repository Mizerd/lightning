import QtQuick
import MatrixClient

// v0.7: shared loading-skeleton primitive for deferred timeline content
// (images, GIFs, video thumbnails, link previews, decrypting text, avatars).
//
// A neutral theme-derived surface with an optional soft highlight band that
// sweeps left to right. Deliberately cheap: one Rectangle plus one animated
// band Rectangle; the band exists only while the skeleton is animating, and
// animation runs only while the skeleton is effectively on screen
// (`active`, visible, and attached to a visible window) and reduced motion
// is off. Offscreen or reduced-motion skeletons render the static surface.
//
// The skeleton never intercepts input and stays out of the accessibility
// tree — it is presentation for "content is on its way", not a control.
Rectangle {
    id: root

    // Circle for avatars, rounded rectangle for everything else.
    property bool circle: false
    // Callers gate animation to their own on-screen knowledge (a ListView
    // row inside the cache buffer is `visible` but not on screen).
    property bool active: true
    // Static mode for reduced motion; follows the app-wide hint by default.
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
        // Soft ink-tinted band: follows the theme's text colour at low
        // alpha, so light themes get a gentle darkening and dark themes a
        // gentle lightening — never a white flash.
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
