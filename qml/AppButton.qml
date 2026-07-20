import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning text button. Three kinds, one geometry:
//   "secondary" (default) — flat themed surface with a subtle 1px border.
//   "primary"             — accent fill with on-accent text (main actions).
//   "danger"              — destructive: danger text on a soft-danger hover
//                            surface with a subtle danger-tinted border.
// All kinds: 8px radius, consistent height, no gradients, no bevels, no
// shadows; the shared 2px accent focus ring; a flat low-contrast disabled
// state that never looks like a native widget.
AbstractButton {
    id: root

    property string kind: "secondary"

    readonly property bool primary: kind === "primary"
    readonly property bool danger: kind === "danger"

    implicitWidth: label.implicitWidth + leftPadding + rightPadding
    implicitHeight: 32
    leftPadding: 14
    rightPadding: 14
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button

    contentItem: Label {
        id: label
        text: root.text
        color: !root.enabled ? AppTheme.textDisabled
               : root.primary ? AppTheme.accentText
               : root.danger ? AppTheme.danger
               : AppTheme.textPrimary
        font.pixelSize: 13
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Label.ElideRight
    }

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: {
            if (root.primary) {
                if (!root.enabled) return AppTheme.cardElevated
                if (root.down) return AppTheme.accentPressed
                if (root.hovered) return AppTheme.accentHover
                return AppTheme.accent
            }
            if (root.danger) {
                if (root.enabled && (root.down || root.hovered))
                    return Qt.alpha(AppTheme.danger, 0.12)
                return "transparent"
            }
            if (root.enabled && (root.down || root.hovered))
                return AppTheme.hover
            return "transparent"
        }
        border.width: root.primary ? 0 : 1
        border.color: !root.enabled ? AppTheme.border
                      : root.danger ? Qt.alpha(AppTheme.danger, 0.45)
                      : AppTheme.borderStrong
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -4
        radius: AppTheme.radiusMd + 4
        color: "transparent"
        border.color: AppTheme.focusRing
        border.width: 2
        visible: root.visualFocus
    }
}
