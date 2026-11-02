import QtQuick
import QtQuick.Controls
import MatrixClient

AbstractButton {
    id: root

    property string iconName: ""
    property string role: "neutral"
    property int diameter: 44
    property int glyphSize: 20
    property int cornerRadius: -1
    /// Shown on hover/focus and used as the accessible name. There is no
    /// "unavailableReason": a disabled control gets no hover, so a tooltip
    /// can't explain it. Callers that can't offer the control should hide it.
    property string tooltip: ""

    implicitWidth: diameter
    implicitHeight: diameter
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: tooltip

    readonly property color _fill: {
        if (!root.enabled)
            return AppTheme.surface
        if (root.role === "danger")
            return root.pressed ? AppTheme.dangerFillPressed
                                : (root.hovered ? AppTheme.dangerFillHover
                                                : AppTheme.dangerFill)
        if (root.role === "active")
            return root.pressed ? AppTheme.accentPressed
                                : (root.hovered ? AppTheme.accentHover
                                                : AppTheme.accent)
        return root.pressed ? AppTheme.selectedHover
                            : (root.hovered ? AppTheme.hover
                                            : AppTheme.surfaceElevated)
    }

    readonly property color _ink: {
        if (!root.enabled)
            return AppTheme.textDisabled
        if (root.role === "danger")
            return AppTheme.dangerText
        if (root.role === "active")
            return AppTheme.accentText
        return AppTheme.textPrimary
    }

    background: Rectangle {
        radius: root.cornerRadius >= 0 ? root.cornerRadius
                                       : Math.min(width, height) / 2
        color: root._fill
        border.width: root.activeFocus ? 2 : (root.role === "neutral" ? 1 : 0)
        border.color: root.activeFocus ? AppTheme.focusRing
                                       : AppTheme.borderSubtle
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    contentItem: Icon {
        name: root.iconName
        size: root.glyphSize
        color: root._ink
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    ToolTip.visible: (root.hovered || root.activeFocus)
                     && ToolTip.text.length > 0
    ToolTip.delay: 350
    ToolTip.text: root.tooltip
}
