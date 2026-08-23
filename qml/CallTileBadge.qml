import QtQuick
import MatrixClient

// Small circular state badge for a call participant tile (muted, sharing,
// hand raised). Tones are semantic names, never literals, so the badge
// follows every theme.
Rectangle {
    id: root

    property string iconName: ""
    /// "accent" | "muted" | "danger"
    property string tone: "muted"
    property int glyphSize: 12

    implicitWidth: glyphSize + 8
    implicitHeight: glyphSize + 8
    radius: width / 2

    readonly property color _ink: tone === "accent" ? AppTheme.accentText : (tone === "danger" ? AppTheme.dangerText : AppTheme.textSecondary)

    color: tone === "accent" ? AppTheme.accent : (tone === "danger" ? AppTheme.dangerFill : AppTheme.surfaceElevated)
    border.width: tone === "muted" ? 1 : 0
    border.color: AppTheme.borderSubtle

    Icon {
        anchors.centerIn: parent
        name: root.iconName
        size: root.glyphSize
        color: root._ink
    }
}
