import QtQuick
import QtQuick.Controls
import MatrixClient

AbstractButton {
    id: root

    property string kind: "secondary"
    property bool storm: false
    property string size: "md"
    property string iconName: ""
    property int minWidth: AppTheme.buttonMinWidth
    property bool busy: false

    readonly property bool primary: kind === "primary"
    readonly property bool danger: kind === "danger"
    readonly property bool dangerPrimary: kind === "dangerPrimary"
    readonly property bool ghost: kind === "ghost"

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

    Keys.onReturnPressed: (event) => root._activateFromKey(event)
    Keys.onEnterPressed: (event) => root._activateFromKey(event)
    // An instance with its own Return behaviour turns this off: an instance's
    // Keys.onReturnPressed does NOT replace this one, both run (one Return
    // would otherwise act twice).
    property bool activateOnReturn: true
    property bool keysNeedVisualFocus: false
    signal keyRefusedWithoutRing()
    function _activateFromKey(event) {
        if (!root.activateOnReturn)
            return
        if (event.isAutoRepeat) {
            // Holding Return must not fire a confirming button repeatedly
            // (Space does not repeat either).
            event.accepted = true
            return
        }
        if (!root.enabled || !root.visible)
            return
        event.accepted = true
        if (root.keysNeedVisualFocus && !root.visualFocus) {
            root.keyRefusedWithoutRing()
            return
        }
        if (typeof root.click === "function") {
            root.click()
        } else {
            if (root.checkable)
                root.toggle()
            root.clicked()
        }
    }

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: (root.iconName.length > 0 || root.busy)
                     ? AppTheme.buttonIconGap : 0

            Loader {
                active: root.busy
                visible: active
                anchors.verticalCenter: parent.verticalCenter
                sourceComponent: AppBusyIndicator {
                    objectName: "buttonBusy"
                    running: root.busy
                    size: root.size === "sm" ? 13 : 15
                    color: root._ink
                }
            }

            Icon {
                objectName: "buttonIcon"
                visible: root.iconName.length > 0 && !root.busy
                name: root.iconName
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
                return root.ghost ? "transparent" : AppTheme.buttonDisabledTint
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
            if (root.storm)
                return root.down ? AppTheme.stormSelection
                     : root.hovered ? Qt.alpha(AppTheme.stormSelection, 0.55)
                     : root.ghost ? "transparent"
                     : Qt.alpha(AppTheme.stormTextSecondary, 0.06)
            if (root.ghost)
                return root.down ? AppTheme.buttonGhostPressed
                     : root.hovered ? AppTheme.buttonGhostHover
                     : "transparent"
            return root.down ? AppTheme.buttonGhostPressed
                 : root.hovered ? AppTheme.buttonGhostHover
                 : AppTheme.buttonSecondaryFill
        }
        border.width: root.ghost ? 0 : (root._filled && root.enabled) ? 0 : 1
        border.color: {
            if (!root.enabled) return AppTheme.buttonDisabledBorder
            if (root.danger)
                return root.storm ? AppTheme.stormDangerBorder
                                  : Qt.alpha(AppTheme.danger, 0.45)
            return root.storm ? AppTheme.stormBorderStrong
                              : AppTheme.buttonSecondaryBorder
        }
    }

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
