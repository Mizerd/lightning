import QtQuick
import QtQuick.Controls
import MatrixClient

ScrollBar {
    id: root

    property bool thin: false
    property bool scrim: false

    readonly property bool _wide: !thin && (hovered || pressed)
    readonly property int _thickness: _wide ? AppTheme.scrollbarWidth
                                            : AppTheme.scrollbarWidthThin

    implicitWidth: _thickness
    implicitHeight: _thickness
    padding: AppTheme.scrollbarMargin
    // Explicit: widen-on-hover depends on it, and Basic's default has changed
    // between Qt versions.
    hoverEnabled: true
    minimumSize: orientation === Qt.Vertical
                 ? Math.min(1, 28 / Math.max(1, height))
                 : Math.min(1, 28 / Math.max(1, width))

    Behavior on implicitWidth {
        enabled: !AppTheme.reducedMotion
        NumberAnimation { duration: 90 }
    }
    Behavior on implicitHeight {
        enabled: !AppTheme.reducedMotion
        NumberAnimation { duration: 90 }
    }

    visible: policy !== ScrollBar.AlwaysOff

    background: Rectangle {
        id: groove
        opacity: 0.0
        color: root.scrim ? AppTheme.scrimSurfaceRaised
                          : AppTheme.scrollbarTrackHover
        radius: AppTheme.scrollbarRadius
    }

    contentItem: Rectangle {
        id: handle
        implicitWidth: root._thickness - 2 * AppTheme.scrollbarMargin
        implicitHeight: root._thickness - 2 * AppTheme.scrollbarMargin
        radius: AppTheme.scrollbarRadius
        opacity: 0.0
        color: {
            if (root.scrim)
                return root.pressed ? AppTheme.scrimInkStrong
                     : root.hovered ? AppTheme.scrimSurfaceHover
                     : AppTheme.scrimSurfaceRaised
            return root.pressed ? AppTheme.scrollbarHandlePressed
                 : root.hovered ? AppTheme.scrollbarHandleHover
                 : AppTheme.scrollbarHandle
        }
    }

    states: State {
        name: "active"
        when: root.policy === ScrollBar.AlwaysOn
              || (root.active && root.size < 1.0)
        PropertyChanges { handle.opacity: 1.0 }
        PropertyChanges { groove.opacity: root.hovered || root.pressed ? 1.0 : 0.0 }
    }

    transitions: [
        Transition {
            to: "active"
            NumberAnimation {
                targets: [handle, groove]
                property: "opacity"
                duration: AppTheme.reducedMotion ? 0 : 120
            }
        },
        Transition {
            from: "active"
            SequentialAnimation {
                PauseAnimation { duration: AppTheme.reducedMotion ? 0 : 450 }
                NumberAnimation {
                    targets: [handle, groove]
                    property: "opacity"
                    to: 0.0
                    duration: AppTheme.reducedMotion ? 0 : 200
                }
            }
        }
    ]
}
