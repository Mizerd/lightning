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

    onClicked: menu.popup()

    // One menu per chevron, created lazily.
    CallDeviceMenu {
        id: menu
        kind: root.kind
    }
}
