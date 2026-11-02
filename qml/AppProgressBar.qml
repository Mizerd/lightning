import QtQuick
import QtQuick.Controls
import MatrixClient

ProgressBar {
    id: root

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
