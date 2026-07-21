import QtQuick
import MatrixClient

// Lightning switch primitive (design spec: 36×20 pill track, 16px white
// thumb, 150ms travel honoring AppTheme.reducedMotion). This is the BARE
// control: like SegmentedControl, it never mutates its own state — the
// owner binds `checked` and flips it from the toggled() signal, so
// declarative bindings (e.g. "forced off while the room is public") are
// never broken by an internal write. Whole-row click behavior belongs to
// the OWNING row, not to this control.
Item {
    id: root

    // Owner-driven state; the control only requests a change via toggled().
    property bool checked: false
    // Emitted on click, Space, or Return/Enter while enabled.
    signal toggled()

    implicitWidth: 36
    implicitHeight: 20
    activeFocusOnTab: true

    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: root.checked
    Accessible.onToggleAction: if (root.enabled) root.toggled()

    Keys.onSpacePressed: (event) => {
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }
    Keys.onReturnPressed: (event) => {
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }
    Keys.onEnterPressed: (event) => {
        if (root.enabled) {
            root.toggled()
            event.accepted = true
        }
    }

    TapHandler {
        enabled: root.enabled
        onTapped: root.toggled()
    }
    HoverHandler {
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }

    // Track.
    Rectangle {
        objectName: "switchTrack"
        anchors.fill: parent
        radius: AppTheme.radiusPill
        color: root.checked ? AppTheme.accent : AppTheme.textDisabled
        opacity: root.enabled ? 1.0 : 0.45

        // Thumb: the spec's white 16px circle (same sanctioned literal as
        // the Settings switch/slider thumbs).
        Rectangle {
            width: 16; height: 16; radius: 8
            color: "#FFFFFF"
            y: 2
            x: root.checked ? 18 : 2
            Behavior on x {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 150 }
            }
        }
    }

    // Shared 2px accent focus ring (keyboard focus only).
    Rectangle {
        anchors.fill: parent
        anchors.margins: -4
        radius: AppTheme.radiusPill
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
        visible: root.activeFocus
    }
}
