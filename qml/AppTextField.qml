import QtQuick
import QtQuick.Controls
import MatrixClient

TextField {
    id: root

    property bool searchIcon: false
    property bool clearButton: false
    property bool storm: false

    implicitHeight: AppTheme.buttonHeight
    hoverEnabled: true
    readonly property real iconSidePadding: searchIcon ? 32 : AppTheme.buttonPaddingH
    readonly property real clearSidePadding:
        clearButton && text.length > 0 ? 32 : AppTheme.buttonPaddingH
    leftPadding: LayoutMirroring.enabled ? clearSidePadding : iconSidePadding
    rightPadding: LayoutMirroring.enabled ? iconSidePadding : clearSidePadding
    font.pixelSize: AppTheme.textBody
    color: storm ? AppTheme.stormFieldText : AppTheme.fieldText
    placeholderTextColor: AppTheme.placeholderInk
    selectionColor: AppTheme.textSelection
    selectedTextColor: storm ? AppTheme.stormFieldText : AppTheme.fieldText
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: root.storm ? AppTheme.stormInset : AppTheme.inputBackground
        border.width: root.activeFocus ? 2 : 1
        border.color: {
            if (root.storm)
                return root.activeFocus ? AppTheme.bolt
                     : root.hovered ? AppTheme.stormBorderStrong
                     : AppTheme.stormFieldBorder
            return root.activeFocus ? AppTheme.focusRing
                 : root.hovered ? AppTheme.borderStrong
                 : AppTheme.fieldBorder
        }

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

    MouseArea {
        objectName: "textFieldContextArea"
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.IBeamCursor
        onClicked: (mouse) => {
            root.forceActiveFocus(Qt.PopupFocusReason)
            textFieldEditMenuLoader.active = true
            textFieldEditMenuLoader.item.popup(mouse.x, mouse.y)
        }
    }
    Loader {
        id: textFieldEditMenuLoader
        active: false
        sourceComponent: AppMenu {
            id: textFieldEditMenu
            objectName: "textFieldEditMenu"
            menuWidth: AppTheme.menuWidthFlyout
            readonly property bool _secret:
                root.echoMode !== TextInput.Normal
            AppMenuItem {
                objectName: "textFieldCut"
                text: qsTr("Cut")
                enabled: !root.readOnly && !textFieldEditMenu._secret
                         && root.selectedText.length > 0
                onTriggered: root.cut()
            }
            AppMenuItem {
                objectName: "textFieldCopy"
                text: qsTr("Copy")
                enabled: !textFieldEditMenu._secret && root.selectedText.length > 0
                onTriggered: root.copy()
            }
            AppMenuItem {
                objectName: "textFieldPaste"
                text: qsTr("Paste")
                enabled: !root.readOnly && root.canPaste
                onTriggered: root.paste()
            }
            AppMenuSeparator {}
            AppMenuItem {
                objectName: "textFieldSelectAll"
                text: qsTr("Select all")
                enabled: root.length > 0
                onTriggered: root.selectAll()
            }
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
