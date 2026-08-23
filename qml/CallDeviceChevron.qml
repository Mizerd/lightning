import QtQuick
import QtQuick.Controls
import MatrixClient

// The small chevron that opens a device chooser beside a call control.
//
// Deliberately its own control rather than a corner of the button: a device
// change and a mute are different intents, and merging them means every
// attempt to pick a microphone also toggles the microphone. Narrow, but a
// full-height hit target, so it stays reachable without being easy to press
// by accident.
AbstractButton {
    id: root

    /// "microphone" | "speaker" | "camera"
    property string kind: "microphone"
    property string accessibleName: ""
    /// Marks the chosen device as unavailable, so the reason audio is coming
    /// from somewhere unexpected is visible on the control itself.
    property bool warn: false

    // Compact and vertically centred against the 40 px control it belongs
    // to, so the pair reads as one affordance. A full-height slab beside a
    // round button reads as a separate control, which is what the first
    // rendering looked like.
    //
    // 26 px keeps a comfortable pointer target while staying clearly
    // secondary to the button it qualifies; it is not overlapped, so it also
    // remains an independent keyboard stop.
    implicitWidth: 20
    implicitHeight: 26
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.ButtonMenu
    Accessible.name: root.accessibleName
    Accessible.description: root.warn
                            ? qsTr("The chosen device isn't connected")
                            : ""

    background: Rectangle {
        radius: AppTheme.radiusSm
        color: root.pressed
               ? AppTheme.selectedHover
               : (root.hovered || root.activeFocus ? AppTheme.hover
                                                   : AppTheme.surfaceElevated)
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? AppTheme.focusRing
                                       : AppTheme.borderSubtle
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    contentItem: Icon {
        name: "expand_more"
        size: 14
        color: root.warn ? AppTheme.warning : AppTheme.textSecondary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    ToolTip.visible: (root.hovered || root.activeFocus)
                     && ToolTip.text.length > 0
    ToolTip.delay: 400
    ToolTip.text: root.accessibleName

    onClicked: menu.popup()

    // ONE menu per chevron, created lazily: a device list is small, but a
    // Menu per control instantiated eagerly is the per-row-menu mistake at a
    // smaller scale.
    CallDeviceMenu {
        id: menu
        kind: root.kind
    }
}
