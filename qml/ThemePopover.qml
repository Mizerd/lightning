import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A panel that opens under a button in the theme editor's top bar: the theme
// menu, Share / Import and the readability report. It is an in-tree overlay
// rather than a Popup: the editor is itself a modal Popup, whose shortcuts and
// Escape handling must keep working while one of these is open, and a second
// Popup layer would take both.
//
// Fill the editor's content area with it (`anchors.fill: parent`). While open
// it covers everything with a transparent catcher, so a click anywhere else
// closes it (`dismissRequested`) rather than reaching what is underneath.
// Escape is the editor's own business: it asks `opened` before closing itself.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
Item {
    id: root

    // The control it hangs from; the panel sits just below it.
    property Item anchorItem: null
    property bool opened: false
    property real popoverWidth: 340
    // Right edges line up (for a control at the right end of the bar).
    property bool alignRight: false
    // Spoken when it opens.
    property string accessibleTitle: ""
    // Focus goes here when it opens, so the keyboard lands inside the panel.
    property Item initialFocus: null
    // The panel's contents, laid out in a column.
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
    // True when keyboard focus is on something inside this panel.
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
        // As tall as the contents, never past the bottom of the editor.
        height: Math.min(body.implicitHeight + AppTheme.spacing16 * 2,
                         Math.max(0, root.height - panel.y - 12))
        radius: AppTheme.radiusMd
        color: AppTheme.editorPanel
        border.width: 1
        border.color: AppTheme.editorBorderStrong
        Accessible.role: Accessible.Pane
        Accessible.name: root.accessibleTitle

        // Presses inside the panel stay inside it.
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
