import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning text button. Five kinds on one geometry ladder:
//   "secondary" (default) — flat surface with a subtle 1px border.
//   "primary"             — accent fill with on-accent text (main actions).
//   "danger"              — destructive, quiet: danger ink on a transparent
//                           field with a danger-tinted border.
//   "dangerPrimary"       — destructive, committed: solid danger fill with
//                           dangerText. For a destructive dialog's confirm.
//   "ghost"               — label only; no border, no resting fill. For
//                           tertiary actions inside another surface.
//
// Geometry comes from the AppTheme button ladder (`size`: sm 26 / md 32 /
// lg 40, buttonRadius, buttonPaddingH), never per-site literals.
//
// `storm: true` on storm surfaces. Accent and fill tokens are theme-routed
// (Storm's accent is bolt), so only the secondary ink differs by skin.
AbstractButton {
    id: root

    property string kind: "secondary"
    property bool storm: false
    // "sm" | "md" | "lg"; see the ladder above.
    property string size: "md"
    // Optional leading Material Symbols glyph; icon and label are centred as a
    // unit.
    property string iconName: ""
    // Width floor so short labels ("OK", "Save") don't make differently sized
    // boxes. An explicit width or Layout.preferredWidth still wins.
    property int minWidth: AppTheme.buttonMinWidth

    readonly property bool primary: kind === "primary"
    readonly property bool danger: kind === "danger"
    readonly property bool dangerPrimary: kind === "dangerPrimary"
    readonly property bool ghost: kind === "ghost"

    // Kinds with a solid resting fill; their focus ring is inked against it.
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

    // Ring ink chosen against what the ring is drawn on (see the focus ring
    // below).
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
                // One optical step below the label's cap height, so it reads as
                // part of the word.
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
                // One face and weight for interactive labels, matching
                // AppMenuItem.
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
            // Secondary / ghost: rest, hover and press are distinct steps (a
            // single step was indistinguishable under Storm's translucent
            // hover).
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

    // Keyboard focus, drawn inside the control's bounds, so it can't collide
    // with neighbours or be clipped by a container. On filled kinds the ring
    // uses the fill's contrast-guaranteed ink (accentText / dangerText)
    // instead of the accent.
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
