import QtQuick
import QtQuick.Controls
import MatrixClient

// One circular control on the call bar. Discord-style shape and states;
// Lightning tokens throughout.
//
// Three visual roles:
//   "neutral" — normal control, subdued fill
//   "active"  — a toggle that is ENGAGED (mic muted, camera on, sharing)
//   "danger"  — leave/hang up, deliberately distinct from everything else
//
// Icons carry no text label by design (PART 10), so a tooltip and an
// accessible name are mandatory, not optional: without them the control is
// unusable by keyboard and screen reader alike.
AbstractButton {
    id: root

    property string iconName: ""
    /// "neutral" | "active" | "danger"
    property string role: "neutral"
    property int diameter: 44
    property int glyphSize: 20
    /// Shown on hover/focus AND used as the accessible name.
    ///
    /// There is deliberately no "unavailableReason" property. A DISABLED
    /// AbstractButton receives no hover events in Qt Quick, so a tooltip is
    /// structurally incapable of explaining a disabled control — the reason
    /// has to be rendered somewhere hover-independent (RoomCallBanner does
    /// this with an inline label). A caller that cannot offer this control
    /// should not show it.
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
        // min(w,h)/2 rather than w/2: the hang-up control is deliberately
        // wider than the round ones, and w/2 would render it as an ellipse.
        radius: Math.min(width, height) / 2
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
