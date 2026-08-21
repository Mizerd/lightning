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

    // Pointer is on it, or it is being pressed. Both handlers existed before
    // and NEITHER was read by a binding — the HoverHandler only set a cursor
    // shape — so the switches on Settings and every creation dialog were the
    // one control class in the app that acknowledged nothing but the click.
    readonly property bool _hot: root.enabled
                                 && (hoverHandler.hovered || tapHandler.pressed)

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
        id: tapHandler
        enabled: root.enabled
        onTapped: root.toggled()
    }
    HoverHandler {
        id: hoverHandler
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }

    // Track — Storm §3.3 on/off states (every AppSwitch host is a storm
    // surface: Settings and the creation dialogs): ON fills bolt with a
    // dark knob, OFF is the strong storm border with the white knob.
    // Hover/press step the track one rung in the direction it is already
    // going: brighter when on (toward accentHover), lighter when off.
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
        // The disabled track is a wash of the enabled one; the thumb dims
        // with it so an off/disabled switch cannot read as off/available.
        opacity: root.enabled ? 1.0 : 0.45
        Behavior on color {
            enabled: !AppTheme.reducedMotion
            ColorAnimation { duration: 120 }
        }

        // Thumb: the spec's white 16px circle (same sanctioned literal as
        // the Settings switch/slider thumbs — "#FFFFFF" here is that same
        // accepted exception, not a token gap); boltInk-dark on the bolt
        // fill so the checked knob stays readable once bolt routes to each
        // legacy theme's own accent.
        Rectangle {
            width: 16; height: 16; radius: 8
            color: root.checked ? AppTheme.boltInk : "#FFFFFF"
            y: 2
            x: root.checked ? 18 : 2
            // A 1px grow on press is the only "give" a 20px control has room
            // for, and it is what makes the press register as a press.
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

    // Shared 2px focus ring (keyboard focus only) — bolt on storm.
    // Gated on `enabled`: a disabled switch could previously still hold
    // focus visuals, claiming an interaction it would refuse.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: AppTheme.radiusPill
        color: "transparent"
        border.width: 2
        border.color: AppTheme.bolt
        visible: root.activeFocus && root.enabled
    }
}
