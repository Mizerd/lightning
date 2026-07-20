import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning single-line text field: flat themed surface, subtle 1px border
// that turns accent on focus, 8px radius, themed selection and placeholder —
// never a native recessed frame. Optional leading search glyph and a clear
// button that appears with text.
TextField {
    id: root

    property bool searchIcon: false
    property bool clearButton: false

    implicitHeight: 32
    hoverEnabled: true
    leftPadding: searchIcon ? 32 : 12
    rightPadding: clearButton && text.length > 0 ? 32 : 12
    font.pixelSize: 13
    color: AppTheme.textPrimary
    placeholderTextColor: AppTheme.textMuted
    selectionColor: AppTheme.accentSoft
    selectedTextColor: AppTheme.textPrimary
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: AppTheme.inputBackground
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? AppTheme.focusRing
                      : root.hovered ? AppTheme.borderStrong
                      : AppTheme.border
    }

    Icon {
        visible: root.searchIcon
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        name: "search"
        size: 16
        color: AppTheme.textMuted
    }

    IconButton {
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
