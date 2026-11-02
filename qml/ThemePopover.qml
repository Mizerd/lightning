import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    property Item anchorItem: null
    property bool opened: false
    property real popoverWidth: 340
    property bool alignRight: false
    property string accessibleTitle: ""
    property Item initialFocus: null
    default property alias content: body.data

    signal dismissRequested()

    visible: root.opened
    z: 40

    function reposition() {
        if (!root.anchorItem || root.width <= 0)
            return
        var p = root.anchorItem.mapToItem(root, 0, root.anchorItem.height + 6)
        var x = root.alignRight ? p.x + root.anchorItem.width - panel.width : p.x
        panel.x = Math.max(8, Math.min(x, root.width - panel.width - 8))
        panel.y = p.y
    }
    function focusInside() {
        const w = root.Window.window
        for (let i = w ? w.activeFocusItem : null; i; i = i.parent) {
            if (i === root)
                return true
        }
        return false
    }
    onOpenedChanged: {
        if (!root.opened) {
            // Hiding an item does not release its active focus: the closed
            // panel's Name field kept every keystroke and typing renamed the
            // theme. Hand focus back to the control the panel hung from.
            if (root.focusInside()) {
                const w = root.Window.window
                if (root.anchorItem && root.anchorItem.visible)
                    root.anchorItem.forceActiveFocus(Qt.OtherFocusReason)
                else if (w && w.activeFocusItem)
                    w.activeFocusItem.focus = false
            }
        } else {
            root.reposition()
            Qt.callLater(function () {
                if (root.opened && root.initialFocus)
                    root.initialFocus.forceActiveFocus()
            })
        }
    }
    onWidthChanged: if (root.opened) root.reposition()
    onHeightChanged: if (root.opened) root.reposition()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: root.dismissRequested()
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        id: panel
        objectName: "themePopoverPanel"
        width: Math.min(root.popoverWidth, Math.max(0, root.width - 16))
        height: Math.min(body.implicitHeight + AppTheme.spacing16 * 2,
                         Math.max(0, root.height - panel.y - 12))
        radius: AppTheme.radiusMd
        color: AppTheme.editorPanel
        border.width: 1
        border.color: AppTheme.editorBorderStrong
        Accessible.role: Accessible.Pane
        Accessible.name: root.accessibleTitle

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: AppTheme.spacing16
            spacing: AppTheme.spacing8
        }
    }
}
