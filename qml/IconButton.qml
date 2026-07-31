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
    // Storm surfaces (menus, pickers, dialogs, Settings): storm inks and
    // fills; the themed hover tint would render a near-white block on the
    // navy panels. Themed hosts (timeline, room list, media) keep default.
    property bool storm: false
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
        color: {
            if (root.iconColorOverride !== "" && root.enabled)
                return root.iconColorOverride
            if (root.storm) {
                if (!root.enabled) return AppTheme.stormTextFaint
                // Ink on the bolt fill, not the panel ink — boltInk stays
                // readable once bolt routes to each legacy theme's own
                // accent.
                if (root.fill) return AppTheme.boltInk
                if (root.active) return AppTheme.bolt
                return (root.hovered || root.down) ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
            }
            return !root.enabled ? AppTheme.textDisabled
                 : root.fill ? AppTheme.accentText
                 : root.active ? AppTheme.accent
                 : AppTheme.icon
        }
    }

    background: Rectangle {
        radius: root.radius
        color: {
            if (root.storm) {
                if (root.fill) {
                    if (!root.enabled) return AppTheme.stormInset
                    // Deliberately procedural, not accentHover/accentPressed
                    // — see the matching note in AppButton.qml's primary
                    // fill. Accepted documented divergence, not a defect.
                    if (root.down) return Qt.darker(AppTheme.bolt, 1.12)
                    if (root.hovered) return Qt.darker(AppTheme.bolt, 1.05)
                    return AppTheme.bolt
                }
                if (root.active) return AppTheme.stormSelection
                return (root.enabled && (root.down || root.hovered))
                       ? AppTheme.stormSelection : "transparent"
            }
            return root.fill
                 ? (!root.enabled ? AppTheme.cardElevated
                    : root.down ? AppTheme.accentPressed
                    : root.hovered ? AppTheme.accentHover
                    : AppTheme.accent)
                 : root.active ? AppTheme.accentSoft
                 : (root.enabled && (root.down || root.hovered))
                   ? AppTheme.hover : "transparent"
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -4
        radius: root.radius + 4
        color: "transparent"
        border.color: root.storm ? AppTheme.bolt : AppTheme.focusRing
        border.width: 2
        visible: root.visualFocus
    }
}
