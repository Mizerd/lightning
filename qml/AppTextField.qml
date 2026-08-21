import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning single-line text field: flat themed surface, subtle 1px border
// that turns accent on focus, themed selection and placeholder — never a
// native recessed frame. Optional leading search glyph and a clear button
// that appears with text.
//
// Storm skin (SPEC-storm-language §3.8): `storm: true` on storm surfaces —
// stormInset fill, 1px stormBorder, focus promotes to a bolt border with a
// soft bolt halo ring outside the field.
TextField {
    id: root

    property bool searchIcon: false
    property bool clearButton: false
    property bool storm: false

    implicitHeight: AppTheme.buttonHeight
    hoverEnabled: true
    leftPadding: searchIcon ? 32 : AppTheme.buttonPaddingH
    rightPadding: clearButton && text.length > 0 ? 32 : AppTheme.buttonPaddingH
    font.pixelSize: AppTheme.textBody
    color: storm ? AppTheme.stormText : AppTheme.textPrimary
    placeholderTextColor: storm ? AppTheme.stormTextMuted : AppTheme.textMuted
    selectionColor: storm ? AppTheme.stormSelection : AppTheme.accentSoft
    selectedTextColor: storm ? AppTheme.stormText : AppTheme.textPrimary
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: root.storm ? AppTheme.stormInset : AppTheme.inputBackground
        // Integer weights only. The storm skin used to focus at 1.5px and the
        // themed skin at 2px, so the same field showed focus at two different
        // weights depending on its host — and a 1.5px border cannot land on a
        // pixel boundary at DPR 1.0, so it rendered as two half-covered rows
        // of antialiasing next to the 1px borders beside it.
        border.width: root.activeFocus ? 2 : 1
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
        size: "sm"
        iconName: "close"
        iconSize: 14
        Accessible.name: qsTr("Clear text")
        onClicked: {
            root.clear()
            root.forceActiveFocus()
        }
    }
}
