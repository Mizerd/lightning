import QtQuick
import MatrixClient

// Lightning switch: a 40×24 pill track (scaled) and a white thumb 4px smaller than it with 150ms
// travel (respecting reducedMotion). A bare control that never mutates its
// own state: the owner binds `checked` and flips it from toggled(), so
// declarative bindings are never broken by an internal write. Whole-row
// clicking belongs to the owning row.
Item {
    id: root

    // Owner-driven state; the control only requests a change via toggled().
    property bool checked: false
    // Emitted on click, Space, or Return/Enter while enabled.
    signal toggled()

    // Pointer over it or pressing, for hover/press feedback.
    readonly property bool _hot: root.enabled
                                 && (hoverHandler.hovered || tapHandler.pressed)

    implicitWidth: AppTheme.scaled(40)
    implicitHeight: AppTheme.scaled(24)
    // The pointer target is never shorter than a Settings control row: the
    // taps and the hover cursor reach this far above and below the pill.
    readonly property real hitHeight: Math.max(height,
                                               AppTheme.settingsControlMinHeight)
    readonly property real _thumb: height - 4
    activeFocusOnTab: true
    // Focus came from a click and no key has been pressed since.
    property bool _mouseFocus: false
    onActiveFocusChanged: if (!activeFocus) _mouseFocus = false
    // For a label beside the switch that toggles it on a click: the same
    // focus the switch's own tap gives, so the keyboard ring stays hidden.
    function focusFromPointer() {
        root._mouseFocus = true
        root.forceActiveFocus(Qt.MouseFocusReason)
    }

    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: root.checked
    Accessible.onToggleAction: if (root.enabled) root.toggled()

    Keys.onSpacePressed: (event) => {
        root._mouseFocus = false
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }
    Keys.onReturnPressed: (event) => {
        root._mouseFocus = false
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }
    Keys.onEnterPressed: (event) => {
        root._mouseFocus = false
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }

    TapHandler {
        id: tapHandler
        enabled: root.enabled
        margin: (root.hitHeight - root.height) / 2
        onTapped: {
            // A click gives the switch keyboard focus, like a native checkbox,
            // so Space right afterwards toggles it. The ring stays a keyboard
            // cue: a mouse-given focus does not draw it.
            root._mouseFocus = true
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.toggled()
        }
    }
    HoverHandler {
        id: hoverHandler
        enabled: root.enabled
        margin: (root.hitHeight - root.height) / 2
        cursorShape: Qt.PointingHandCursor
    }

    // Track (every host is a storm surface): on fills bolt with a dark knob,
    // off is the strong storm border with a white knob. Hover/press step the
    // track one rung in its current direction.
    Rectangle {
        objectName: "switchTrack"
        anchors.fill: parent
        radius: AppTheme.radiusPill
        color: {
            if (root.checked)
                return root._hot ? AppTheme.accentHover : AppTheme.bolt
            return root._hot ? Qt.lighter(AppTheme.stormBorderStrong, 1.18)
                             : AppTheme.stormBorderStrong
        }
        // A disabled switch washes out, thumb included, so it can't read as
        // available.
        opacity: root.enabled ? 1.0 : 0.45
        Behavior on color {
            enabled: !AppTheme.reducedMotion
            ColorAnimation { duration: 120 }
        }

        // Thumb: the sanctioned white 16px circle; boltInk on the bolt fill so
        // the checked knob stays readable on every theme's accent.
        Rectangle {
            width: root._thumb; height: root._thumb; radius: root._thumb / 2
            color: root.checked ? AppTheme.boltInk : "#FFFFFF"
            y: 2
            x: root.checked ? root.width - root._thumb - 2 : 2
            // A slight grow on press, the only "give" a 20px control has room
            // for.
            scale: tapHandler.pressed && root.enabled ? 1.12 : 1.0
            Behavior on x {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 150 }
            }
            Behavior on scale {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 90 }
            }
        }
    }

    // Shared 2px keyboard focus ring (bolt on storm), hidden when disabled.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: AppTheme.radiusPill
        color: "transparent"
        border.width: 2
        border.color: AppTheme.bolt
        visible: root.activeFocus && root.enabled && !root._mouseFocus
    }
}
