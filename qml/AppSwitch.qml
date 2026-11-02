import QtQuick
import MatrixClient

Item {
    id: root

    // Owner-driven state; the control only requests a change via toggled().
    property bool checked: false
    signal toggled()

    readonly property bool _hot: root.enabled
                                 && (hoverHandler.hovered || tapHandler.pressed)

    implicitWidth: AppTheme.scaled(40)
    implicitHeight: AppTheme.scaled(24)
    readonly property real hitHeight: Math.max(height,
                                               AppTheme.settingsControlMinHeight)
    readonly property real _thumb: height - 4
    activeFocusOnTab: true
    property bool _mouseFocus: false
    onActiveFocusChanged: if (!activeFocus) _mouseFocus = false
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
        opacity: root.enabled ? 1.0 : 0.45
        Behavior on color {
            enabled: !AppTheme.reducedMotion
            ColorAnimation { duration: 120 }
        }

        Rectangle {
            width: root._thumb; height: root._thumb; radius: root._thumb / 2
            color: root.checked ? AppTheme.boltInk : "#FFFFFF"
            y: 2
            x: root.checked ? root.width - root._thumb - 2 : 2
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
