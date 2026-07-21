import QtQuick
import QtQuick.Controls
import MatrixClient

// Design button system (SPEC-composer-settings-buttons §1): every icon
// control in the app is one of exactly three treatments.
//   Style A (default)      — bare icon: transparent at rest, soft theme tint
//                            on hover, never a border or shadow.
//   Style B (active: true) — accent chip: accent-soft fill, accent icon,
//                            same geometry as the rest state.
//   Style C (fill: true)   — accent fill: solid accent + on-accent icon;
//                            reserved for primary actions (send buttons,
//                            active rail modes).
// Keyboard focus draws the shared 2px accent outline offset 2px; hover and
// press never add borders or elevation.
AbstractButton {
    id: root

    property string iconName: ""
    property int iconSize: 21
    property int radius: AppTheme.radiusMd
    // Style B: the selected/active state of a bare icon button.
    property bool active: false
    // Style C: primary accent fill.
    property bool fill: false
    // Scrim contexts (video control bars, media viewers) need explicit
    // constant ink — the themed icon colour can vanish over video. Empty
    // keeps the standard three-style theming.
    property string iconColorOverride: ""

    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button

    contentItem: Icon {
        name: root.iconName
        size: root.iconSize
        color: !root.enabled ? AppTheme.textDisabled
               : root.iconColorOverride !== "" ? root.iconColorOverride
               : root.fill ? AppTheme.accentText
               : root.active ? AppTheme.accent
               : AppTheme.icon
    }

    background: Rectangle {
        radius: root.radius
        color: root.fill
               ? (!root.enabled ? AppTheme.cardElevated
                  : root.down ? AppTheme.accentPressed
                  : root.hovered ? AppTheme.accentHover
                  : AppTheme.accent)
               : root.active ? AppTheme.accentSoft
               : (root.enabled && (root.down || root.hovered))
                 ? AppTheme.hover : "transparent"
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -4
        radius: root.radius + 4
        color: "transparent"
        border.color: AppTheme.focusRing
        border.width: 2
        visible: root.visualFocus
    }
}
