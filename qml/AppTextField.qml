import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning single-line text field: flat themed surface, a 1px border that
// turns accent on focus, themed selection and placeholder. Optional leading
// search glyph and a clear button that appears with text.
//
// `storm: true` on storm surfaces: stormInset fill, 1px stormBorder, and on
// focus a bolt border with a soft bolt halo outside the field.
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
    // `selectedHover` for the selection: accentSoft (a tile fill) and
    // stormSelection (hover outside Storm) are nearly invisible against the
    // field on several themes, including both defaults. `selected` alone isn't
    // enough either (it equals accentSoft on Moss Light).
    // theTextSelectionIsVisibleOnEveryTheme enforces the floor.
    selectionColor: AppTheme.selectedHover
    selectedTextColor: storm ? AppTheme.stormText : AppTheme.textPrimary
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: root.storm ? AppTheme.stormInset : AppTheme.inputBackground
        // Integer weights only: a 1.5px border can't land on a pixel boundary
        // at DPR 1.0 and renders as blurred rows.
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

        // Focus halo: a 3px bolt ring at 12% outside the field's geometry.
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
