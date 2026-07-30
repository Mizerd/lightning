import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning single-line text field: flat themed surface, subtle 1px border
// that turns accent on focus, 8px radius, themed selection and placeholder —
// never a native recessed frame. Optional leading search glyph and a clear
// button that appears with text.
//
// Storm skin (SPEC-storm-language §3.8): `storm: true` on storm surfaces —
// stormInset fill, 1px stormBorder, focus promotes to a bolt border with a
// soft bolt halo ring outside the field.
TextField {
    id: root

    property bool searchIcon: false
    property bool clearButton: false
    property bool storm: false

    implicitHeight: 32
    hoverEnabled: true
    leftPadding: searchIcon ? 32 : 12
    rightPadding: clearButton && text.length > 0 ? 32 : 12
    font.pixelSize: 13
    color: storm ? AppTheme.stormText : AppTheme.textPrimary
    placeholderTextColor: storm ? AppTheme.stormTextMuted : AppTheme.textMuted
    selectionColor: storm ? AppTheme.stormSelection : AppTheme.accentSoft
    selectedTextColor: storm ? AppTheme.stormText : AppTheme.textPrimary
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: root.storm ? AppTheme.stormInset : AppTheme.inputBackground
        border.width: root.activeFocus ? (root.storm ? 1.5 : 2) : 1
        border.color: {
            if (root.storm)
                return root.activeFocus ? AppTheme.bolt
                     : root.hovered ? AppTheme.stormBorderStrong
                     : AppTheme.stormBorder
            return root.activeFocus ? AppTheme.focusRing
                 : root.hovered ? AppTheme.borderStrong
                 : AppTheme.border
        }

        // §3.8 focus halo: 0 0 0 3px bolt at 12% — an outside ring, never
        // part of the field's own geometry.
        Rectangle {
            visible: root.storm && root.activeFocus
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 3
            border.color: AppTheme.stormBoltGlow
        }
    }

    Icon {
        visible: root.searchIcon
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        name: "search"
        size: 16
        color: root.storm ? AppTheme.stormTextMuted : AppTheme.textMuted
    }

    IconButton {
        storm: root.storm
        visible: root.clearButton && root.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 24
        implicitHeight: 24
        radius: 6
        iconName: "close"
        iconSize: 14
        Accessible.name: qsTr("Clear text")
        onClicked: {
            root.clear()
            root.forceActiveFocus()
        }
    }
}
