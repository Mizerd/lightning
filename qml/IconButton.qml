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
// ── The size ladder ─────────────────────────────────────────────────────
// The 2026-08-21 audit counted 66 instances spanning ELEVEN implicit sizes
// and SEVEN corner radii — including the composer's 28px buttons at radius 6
// sitting directly under the action bar's 28px buttons at radiusControl (7).
// `size` is the fix: four named rungs, each pairing a box, a corner and an
// optical glyph size so those three can never drift apart again.
//
//   "sm" 24 / radiusControl / 16   inline affordances, clear buttons
//   "md" 28 / radiusMd      / 20   composer row, message action bar
//   "lg" 34 / radiusTile    / 21   panel headers (the historical default)
//   "xl" 40 / radiusOmnibox / 22   rail modes, media chrome
//
// A call site may still override implicitWidth/implicitHeight/radius/iconSize
// individually — but it should pick a rung instead, and new code must.
AbstractButton {
    id: root

    property string iconName: ""
    // "sm" | "md" | "lg" | "xl". Default "lg" is the historical 34/21 default.
    property string size: "lg"
    property int iconSize: _metrics.glyph
    property int radius: _metrics.radius
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

    readonly property var _metrics: {
        if (size === "sm")
            return { box: 24, radius: AppTheme.radiusControl, glyph: 16 }
        if (size === "md")
            return { box: 28, radius: AppTheme.radiusMd, glyph: 20 }
        if (size === "xl")
            return { box: 40, radius: AppTheme.radiusOmnibox, glyph: 22 }
        return { box: 34, radius: AppTheme.radiusTile, glyph: 21 }
    }

    // A treatment whose resting state is a SOLID fill; its focus ring is
    // inked against that fill rather than against the page.
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
                // Ink on the bolt fill, not the panel ink — boltInk stays
                // readable once bolt routes to each legacy theme's own
                // accent.
                if (root.fill) return AppTheme.boltInk
                if (root.active) return AppTheme.bolt
                return (root.hovered || root.down) ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
            }
            if (!root.enabled) return AppTheme.textDisabled
            if (root.fill) return AppTheme.accentText
            if (root.active) return AppTheme.accent
            // The storm branch has always brightened the glyph on hover; the
            // themed branch did not, and under every themed host the only
            // remaining feedback was a background wash measured at ~3-10 per
            // channel — invisible. The glyph is the thing the eye is already
            // on, so that is what moves.
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
                // A disabled-but-active button used to keep the full selection
                // chip while its glyph greyed out, reading as "selected and
                // available". Both the active and the hover branches now
                // answer to `enabled` the way the fill branch always did.
                if (root.active)
                    return root.enabled ? AppTheme.stormSelection
                                        : AppTheme.stormInset
                return (root.enabled && (root.down || root.hovered))
                       ? AppTheme.stormSelection : "transparent"
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
                   ? AppTheme.hover : "transparent"
        }
    }

    // Keyboard focus, drawn INSIDE the control — see the long note in
    // AppButton.qml. Icon buttons are the worst case for an outset ring:
    // they cluster at 2px spacing inside toolbars that clip.
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
