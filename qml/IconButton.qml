import QtQuick
import QtQuick.Controls
import MatrixClient

AbstractButton {
    id: root

    property string iconName: ""
    property string size: "lg"
    property int iconSize: _metrics.glyph
    property int radius: _metrics.radius
    property bool active: false
    property bool fill: false
    property color fillColor: AppTheme.buttonPrimaryFill
    property color fillInk: AppTheme.accentText
    property color restingColor: "transparent"
    property bool storm: false
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
    Keys.onReturnPressed: (event) => root._activateFromKey(event)
    Keys.onEnterPressed: (event) => root._activateFromKey(event)
    // An instance with its own Return behaviour turns this off: an instance's
    // Keys.onReturnPressed does NOT replace this one, both run (one Return
    // would otherwise act twice).
    property bool activateOnReturn: true
    // A confirming button a stray key must not trigger (Leave room, Close
    // room): Return/Enter act only while the focus ring shows (C06-F01).
    property bool keysNeedVisualFocus: false
    signal keyRefusedWithoutRing()
    function _activateFromKey(event) {
        if (!root.activateOnReturn)
            return
        if (event.isAutoRepeat) {
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

    contentItem: Icon {
        name: root.iconName
        size: root.iconSize
        color: {
            if (root.iconColorOverride !== "" && root.enabled)
                return root.iconColorOverride
            if (root.storm) {
                if (!root.enabled) return AppTheme.stormTextFaint
                if (root.fill) return AppTheme.boltInk
                if (root.active) return AppTheme.bolt
                return (root.hovered || root.down) ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
            }
            if (!root.enabled) return AppTheme.textDisabled
            if (root.fill) return root.fillInk
            if (root.active) return AppTheme.accent
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
                if (root.active)
                    return root.enabled ? AppTheme.stormSelection
                                        : AppTheme.stormInset
                return (root.enabled && (root.down || root.hovered))
                       ? AppTheme.hover : "transparent"
            }
            if (root.fill)
                return !root.enabled ? AppTheme.buttonDisabledFill
                     : root.down ? AppTheme.buttonPrimaryPressed
                     : root.hovered ? AppTheme.buttonPrimaryHover
                     : root.fillColor
            if (root.active)
                return root.enabled ? AppTheme.accentSoft
                                    : AppTheme.buttonDisabledFill
            return (root.enabled && (root.down || root.hovered))
                   ? AppTheme.hover : root.restingColor
        }
    }

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
