import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning progress bar. `value` is 0..1; `indeterminate` runs the sweep.
//
// Styled like the app's sliders (bolt fill on stormInset, pill ends) instead
// of Basic's body-text grey on a hairline grey, so bars and sliders read as
// one family.
ProgressBar {
    id: root

    // For hosts painting over media/scrim rather than a theme surface.
    property bool scrim: false

    from: 0
    to: 1
    implicitWidth: 200
    implicitHeight: 4

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 4
        radius: AppTheme.radiusPill
        color: root.scrim ? AppTheme.scrimSurfaceRaised : AppTheme.stormInset
    }

    contentItem: Item {
        implicitWidth: 200
        implicitHeight: 4
        clip: true

        // Determinate: one filled pill whose width tracks `position`.
        Rectangle {
            visible: !root.indeterminate
            width: parent.width * root.position
            height: parent.height
            radius: AppTheme.radiusPill
            color: root.scrim ? AppTheme.scrimInkStrong : AppTheme.bolt
            Behavior on width {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 140 }
            }
        }

        // Indeterminate: a short pill sweeping the track, a plain moving
        // Rectangle (Canvas paints nothing here; see StormNode.qml).
        Rectangle {
            id: sweep
            visible: root.indeterminate
            width: Math.max(24, parent.width * 0.28)
            height: parent.height
            radius: AppTheme.radiusPill
            color: root.scrim ? AppTheme.scrimInkStrong : AppTheme.bolt
            // With reduced motion, a static half-filled bar that still reads as
            // busy.
            x: AppTheme.reducedMotion ? 0 : -width
            XAnimator on x {
                running: root.indeterminate && root.visible
                         && !AppTheme.reducedMotion
                from: -sweep.width
                to: root.width
                duration: 1200
                loops: Animation.Infinite
            }
        }
    }
}
