import QtQuick
import QtQuick.Controls
import MatrixClient

// The chevron that opens a device chooser beside a call control. A separate
// control, not a corner of the button: picking a device and muting are
// different intents.
AbstractButton {
    id: root

    /// "microphone" | "speaker" | "camera"
    property string kind: "microphone"
    property string accessibleName: ""
    /// Marks the chosen device as unavailable, explaining unexpected audio
    /// routing on the control itself.
    property bool warn: false

    // Compact and vertically centred against its control so the pair reads as
    // one affordance, while staying a separate pointer target and keyboard
    // stop.
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

    // Against the chevron, never at the pointer: popup() with no position
    // opens where the click was, so the menu covered the control bar it came
    // from, Leave included. Below the chevron, or above it when the chevron is
    // in the lower half of its window (the full-screen dock). The menu's
    // height can still change after popup() (lazy content), so an upward menu
    // is re-anchored whenever it opens or grows. A deferred one-shot flip
    // gated on `opened` missed: `opened` turns true only after the enter
    // transition, so the menu stayed below and was pushed over the bar.
    property bool menuFlipsUp: false
    function openMenu() {
        var gap = AppTheme.spacing4
        var win = root.Window.window
        var below = true
        if (win) {
            var at = root.mapToItem(null, 0, 0)
            below = at.y + root.height / 2 <= win.height / 2
        }
        root.menuFlipsUp = !below
        menu.popup(root, 0, below ? root.height + gap
                                  : -Math.max(menu.height, menu.implicitHeight) - gap)
    }
    onClicked: openMenu()

    // One menu per chevron, created lazily.
    CallDeviceMenu {
        id: menu
        kind: root.kind
        onOpened: if (root.menuFlipsUp) y = -height - AppTheme.spacing4
        onHeightChanged: if (visible && root.menuFlipsUp)
                             y = -height - AppTheme.spacing4
    }
}
