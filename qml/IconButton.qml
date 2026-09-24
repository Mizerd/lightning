import QtQuick
import QtQuick.Controls
import MatrixClient

// The one icon control in the app. Three treatments:
//   Style A (default)      — bare icon: transparent at rest, soft theme tint
//                            on hover, never a border or shadow.
//   Style B (active: true) — accent chip: accent-soft fill, accent icon,
//                            same geometry as the rest state.
//   Style C (fill: true)   — accent fill: solid accent + on-accent icon;
//                            reserved for primary actions (send buttons,
//                            active rail modes).
//
// ── The size ladder ──
// Each rung pairs a box, a corner radius and an optical glyph size so the
// three stay consistent:
//
//   "sm" 24 / radiusControl / 16   inline affordances, clear buttons
//   "md" 28 / radiusMd      / 20   composer row, message action bar
//   "lg" 34 / radiusTile    / 21   panel headers (the default)
//   "xl" 40 / radiusOmnibox / 22   rail modes, media chrome
//
// implicitWidth/implicitHeight/radius/iconSize can still be overridden, but
// new code should pick a rung.
AbstractButton {
    id: root

    property string iconName: ""
    // "sm" | "md" | "lg" | "xl"; default "lg".
    property string size: "lg"
    property int iconSize: _metrics.glyph
    property int radius: _metrics.radius
    // Style B: the selected/active state of a bare icon button.
    property bool active: false
    // Style C: primary accent fill.
    property bool fill: false
    /// Resting background, for an icon button that must read as a tile rather
    /// than a bare glyph (e.g. the Spaces rail's settings cog beside avatars).
    /// Transparent by default; hover, press, active and disabled override it.
    property color restingColor: "transparent"
    // Storm surfaces (menus, pickers, dialogs, Settings) use storm inks and
    // fills; the themed hover tint would be a near-white block on navy panels.
    property bool storm: false
    // Constant ink for scrim contexts (video bars, media viewers), where the
    // themed colour can vanish over video. Empty keeps normal theming.
    property string iconColorOverride: ""

    readonly property var _metrics: {
        if (size === "sm")
            return { box: 24, radius: AppTheme.radiusControl, glyph: 16 }
        if (size === "md")
            return { box: 28, radius: AppTheme.radiusMd, glyph: 20 }
        if (size === "xl")
            return { box: 40, radius: AppTheme.radiusOmnibox, glyph: 22 }
        return { box: 34, radius: AppTheme.radiusTile, glyph: 21 }
    }

    // A treatment with a solid resting fill; its focus ring is inked against
    // that fill.
    readonly property bool _filled: fill

    readonly property color _focusInk: {
        if (fill) return storm ? AppTheme.boltInk : AppTheme.accentText
        return storm ? AppTheme.bolt : AppTheme.focusRing
    }

    implicitWidth: _metrics.box
    implicitHeight: _metrics.box
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
                // Ink on the bolt fill, readable on every theme's accent.
                if (root.fill) return AppTheme.boltInk
                if (root.active) return AppTheme.bolt
                return (root.hovered || root.down) ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
            }
            if (!root.enabled) return AppTheme.textDisabled
            if (root.fill) return AppTheme.accentText
            if (root.active) return AppTheme.accent
            // The glyph brightens on hover; the background wash alone is too
            // faint on themed hosts.
            return (root.hovered || root.down) ? AppTheme.textPrimary
                                               : AppTheme.icon
        }
    }

    background: Rectangle {
        radius: root.radius
        color: {
            if (root.storm) {
                if (root.fill) {
                    if (!root.enabled) return AppTheme.stormInset
                    if (root.down) return AppTheme.buttonPrimaryPressed
                    if (root.hovered) return AppTheme.buttonPrimaryHover
                    return AppTheme.buttonPrimaryFill
                }
                // Disabled active buttons drop the selection chip, so they
                // don't read as selected and available.
                if (root.active)
                    return root.enabled ? AppTheme.stormSelection
                                        : AppTheme.stormInset
                // Hover uses the translucent `hover` wash, not stormSelection,
                // so a toggled-on icon and a hovered one look different.
                return (root.enabled && (root.down || root.hovered))
                       ? AppTheme.hover : "transparent"
            }
            if (root.fill)
                return !root.enabled ? AppTheme.buttonDisabledFill
                     : root.down ? AppTheme.buttonPrimaryPressed
                     : root.hovered ? AppTheme.buttonPrimaryHover
                     : AppTheme.buttonPrimaryFill
            if (root.active)
                return root.enabled ? AppTheme.accentSoft
                                    : AppTheme.buttonDisabledFill
            return (root.enabled && (root.down || root.hovered))
                   ? AppTheme.hover : root.restingColor
        }
    }

    // Keyboard focus ring drawn inside the control (see AppButton.qml):
    // icon buttons sit close together in clipping toolbars.
    Rectangle {
        objectName: "focusRing"
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.color: root._focusInk
        border.width: 2
        visible: root.visualFocus
    }
}
