import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning progress bar. `value` is 0..1; `indeterminate` runs the sweep.
//
// The stock Basic bar fills in `palette.dark` on a `palette.midlight` track,
// which Main.qml maps to textSecondary on border: a BODY-TEXT grey on a
// hairline grey. Five of them shipped that way, including the update
// download bar — the single most-watched progress affordance in the app —
// sitting directly beneath a text-size slider that fills in bolt yellow on
// stormInset. Same treatment as that slider now, plus pill ends, so a
// progress bar and a slider read as the same family.
ProgressBar {
    id: root

    // Contexts painting over media/scrim ink rather than a theme surface.
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

        // Indeterminate: a short pill sweeping the track. Deliberately a
        // plain moving Rectangle rather than the Basic style's animator
        // stack — QtQuick.Shapes is not linked in this application and
        // Canvas paints nothing here (see StormNode.qml), so every animated
        // primitive in this codebase is built from Items.
        Rectangle {
            id: sweep
            visible: root.indeterminate
            width: Math.max(24, parent.width * 0.28)
            height: parent.height
            radius: AppTheme.radiusPill
            color: root.scrim ? AppTheme.scrimInkStrong : AppTheme.bolt
            // Reduced motion parks it as a static half-filled bar rather
            // than pulsing: "busy" must still be legible without animation.
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
