import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning scrollbar. Drop-in for the stock control in BOTH forms:
//
//   ScrollBar.vertical: AppScrollBar {}        // attached
//   AppScrollBar { orientation: Qt.Vertical }  // standalone
//
// The stock Basic ScrollBar takes its handle from `palette.mid` and its
// track from `palette.midlight`, i.e. a border grey on a border grey, with
// square ends and no hover or press step — around thirty of them were
// rendering that way beside a UI whose smallest rounded surface is 4px.
//
// Behaviour: a pill handle that darkens through hover and press, a track
// that only appears under the pointer, and a bar that thins to
// `scrollbarWidthThin` unless it is being used. `thin: true` pins the narrow
// size for dense hosts (combo popups, code blocks, inline lists) where the
// extra 4px would reflow the band reserved for it.
//
// The fade states below are the Basic style's own show/hide contract,
// re-expressed with Lightning's tokens: replacing `contentItem` throws away
// the style's opacity states, and without them an AsNeeded bar would simply
// never hide — every list in the app would grow a permanent rail.
ScrollBar {
    id: root

    // Dense hosts keep the narrow bar even while hovered.
    property bool thin: false
    // Contexts painting over media/scrim ink instead of a theme surface.
    property bool scrim: false

    readonly property bool _wide: !thin && (hovered || pressed)
    readonly property int _thickness: _wide ? AppTheme.scrollbarWidth
                                            : AppTheme.scrollbarWidthThin

    implicitWidth: _thickness
    implicitHeight: _thickness
    padding: AppTheme.scrollbarMargin
    // Explicit rather than inherited: the widen-on-hover behaviour is dead
    // without it, and the Basic style's default has moved between Qt
    // versions.
    hoverEnabled: true
    // Below this a long conversation's handle becomes a two-pixel tick that
    // cannot be grabbed.
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
        // The groove only joins in under the pointer: a permanently visible
        // track reads as a border down the edge of the pane.
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
