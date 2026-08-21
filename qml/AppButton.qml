import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning text button. Five kinds on ONE geometry ladder:
//   "secondary" (default) — flat surface with a subtle 1px border.
//   "primary"             — accent fill with on-accent text (main actions).
//   "danger"              — destructive, quiet: danger ink on a transparent
//                           field with a danger-tinted border.
//   "dangerPrimary"       — destructive, committed: solid danger fill with
//                           dangerText on it. For the confirm button of a
//                           destructive dialog, where "quiet" is wrong.
//   "ghost"               — label only; no border, no resting fill. For
//                           tertiary actions sitting inside another surface.
//
// Geometry comes from the AppTheme button ladder (`size`: sm 26 / md 32 /
// lg 40, buttonRadius, buttonPaddingH), never from per-site literals — the
// 2026-08-21 audit found identical buttons 30px and 40px tall side by side
// because every host picked its own numbers.
//
// Storm skin: `storm: true` on storm surfaces. Note what this no longer
// branches on. bolt/boltInk/accentHover/accentPressed are all THEME-ROUTED
// now (Storm's accent IS bolt, its accentText IS boltInk), so the primary
// and destructive fills resolve identically on both paths and are written
// once. The earlier storm branch open-coded `Qt.darker(bolt, 1.05/1.12)`
// for hover/pressed and therefore disagreed with the themed path's
// hand-tuned accentHover/accentPressed for no reason anyone wanted. Only
// the SECONDARY ink genuinely differs (storm quiets it one step), so that
// is the only branch left.
AbstractButton {
    id: root

    property string kind: "secondary"
    property bool storm: false
    // "sm" | "md" | "lg" — see the ladder note above.
    property string size: "md"
    // Optional leading Material Symbols glyph. Buttons that carry one still
    // centre the icon+label pair as a unit, so a row of mixed buttons keeps
    // one optical centre line.
    property string iconName: ""
    // Width floor so a row of short labels ("OK", "Save") does not render as
    // a row of differently sized boxes. Overridable: a call site with an
    // explicit width or Layout.preferredWidth wins over implicitWidth anyway.
    property int minWidth: AppTheme.buttonMinWidth

    readonly property bool primary: kind === "primary"
    readonly property bool danger: kind === "danger"
    readonly property bool dangerPrimary: kind === "dangerPrimary"
    readonly property bool ghost: kind === "ghost"

    // A kind whose resting state is a SOLID fill. Those need their focus ring
    // inked against the fill, not against the page.
    readonly property bool _filled: primary || dangerPrimary

    readonly property int _height: size === "sm" ? AppTheme.buttonHeightSm
                                : size === "lg" ? AppTheme.buttonHeightLg
                                : AppTheme.buttonHeight
    readonly property int _padH: size === "sm" ? AppTheme.buttonPaddingHSm
                                               : AppTheme.buttonPaddingH
    readonly property int _radius: AppTheme.buttonRadius

    readonly property color _ink: {
        if (!enabled)
            return AppTheme.buttonDisabledInk
        if (primary) return AppTheme.buttonPrimaryInk
        if (dangerPrimary) return AppTheme.buttonDangerInk
        if (danger) return storm ? AppTheme.stormDanger : AppTheme.danger
        if (ghost) return storm ? AppTheme.stormTextSecondary
                                : AppTheme.buttonGhostInk
        return storm ? AppTheme.stormTextSecondary : AppTheme.buttonNeutralInk
    }

    // Ring ink chosen against what the ring is drawn ON — see the focus-ring
    // note at the bottom of the file.
    readonly property color _focusInk: {
        if (primary) return AppTheme.buttonPrimaryInk
        if (dangerPrimary) return AppTheme.buttonDangerInk
        return storm ? AppTheme.bolt : AppTheme.focusRing
    }

    implicitWidth: Math.max(minWidth,
                            contentRow.implicitWidth + leftPadding + rightPadding)
    implicitHeight: _height
    leftPadding: _padH
    rightPadding: _padH
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: root.iconName.length > 0 ? AppTheme.buttonIconGap : 0

            Icon {
                objectName: "buttonIcon"
                visible: root.iconName.length > 0
                name: root.iconName
                // One optical step below the label's cap height reads as part
                // of the word rather than as a separate badge.
                size: root.size === "sm" ? 15 : 17
                color: root._ink
                anchors.verticalCenter: parent.verticalCenter
            }
            Label {
                id: label
                objectName: "buttonLabel"
                visible: root.text.length > 0
                text: root.text
                color: root._ink
                // One face, one weight for the interactive-label role. The
                // menu row (AppMenuItem) matches it exactly; they used to sit
                // in the same popover at Bold and DemiBold.
                font.family: root.storm ? AppTheme.menuFont : AppTheme.uiFont
                font.pixelSize: root.size === "sm" ? AppTheme.textMeta
                                                   : AppTheme.textBody
                font.weight: AppTheme.weightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Label.ElideRight
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    background: Rectangle {
        radius: root._radius
        color: {
            if (!root.enabled)
                return root._filled ? AppTheme.buttonDisabledFill : "transparent"
            if (root.primary)
                return root.down ? AppTheme.buttonPrimaryPressed
                     : root.hovered ? AppTheme.buttonPrimaryHover
                     : AppTheme.buttonPrimaryFill
            if (root.dangerPrimary)
                return root.down ? AppTheme.buttonDangerPressed
                     : root.hovered ? AppTheme.buttonDangerHover
                     : AppTheme.buttonDangerFill
            if (root.danger)
                return root.down ? AppTheme.stormDangerBorder
                     : root.hovered ? AppTheme.stormDangerSoft
                     : "transparent"
            // secondary / ghost: a real two-step ladder. Rest is the host
            // surface, hover lifts one rung, press lifts a second — the
            // single-step version was measured indistinguishable under Storm,
            // where `hover` is a 22%-alpha wash.
            if (root.storm)
                return root.down ? AppTheme.stormSelection
                     : root.hovered ? Qt.alpha(AppTheme.stormSelection, 0.55)
                     : "transparent"
            return root.down ? AppTheme.buttonGhostPressed
                 : root.hovered ? AppTheme.buttonGhostHover
                 : "transparent"
        }
        border.width: (root._filled || root.ghost) ? 0 : 1
        border.color: {
            if (!root.enabled) return AppTheme.buttonDisabledBorder
            if (root.danger)
                return root.storm ? AppTheme.stormDangerBorder
                                  : Qt.alpha(AppTheme.danger, 0.45)
            return root.storm ? AppTheme.stormBorderStrong
                              : AppTheme.buttonNeutralBorder
        }
    }

    // Keyboard focus, drawn INSIDE the control's own bounds.
    //
    // It used to be a 2px stroke at margins -4, i.e. 4px of ring outside a
    // button whose host often gives it 2px of padding: on the message action
    // bar the ring crossed the bar's own border and rounded corner, and in
    // any of the 37 clipping containers it was scissored into an L. Drawing
    // it inside cannot collide with a neighbour and cannot be clipped.
    //
    // The cost of drawing inside is that on a filled kind the accent ring
    // would land on the accent fill and vanish, so `_focusInk` switches to
    // the ink that is already contrast-guaranteed against that fill
    // (accentText / dangerText) instead. Transparent kinds keep the accent.
    Rectangle {
        objectName: "focusRing"
        anchors.fill: parent
        radius: root._radius
        color: "transparent"
        border.color: root._focusInk
        border.width: 2
        visible: root.visualFocus
    }
}
