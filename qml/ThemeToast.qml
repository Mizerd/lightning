import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A short message with one action ("Unlinked from Panels and cards. Undo"),
// shown over the theme editor's preview after an edit that does not look
// reversible. It dismisses itself; the action is optional.
//
// Painted in the invariant editor tokens.
Rectangle {
    id: root

    property string message: ""
    property string actionText: ""
    // What the action does, named ("Undo: Unlink Room list"); read aloud and
    // shown as a tooltip.
    property string actionHint: ""
    // How long it stays, in milliseconds.
    property int duration: 6000
    property bool shown: false
    signal actionTriggered()

    // Shows `text` (and an action when `action` is non-empty).
    function show(text, action) {
        root.message = text
        root.actionText = action === undefined ? "" : action
        root.shown = true
        hideTimer.restart()
    }
    function dismiss() {
        root.shown = false
        hideTimer.stop()
    }

    objectName: "themeToast"
    visible: root.shown && root.message.length > 0
    implicitWidth: Math.min(480, toastRow.implicitWidth + AppTheme.spacing16 * 2)
    implicitHeight: 40
    radius: AppTheme.radiusMd
    color: AppTheme.editorPanel
    border.width: 1
    border.color: AppTheme.editorBorderStrong
    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.message

    Timer {
        id: hideTimer
        interval: root.duration
        onTriggered: root.shown = false
    }

    RowLayout {
        id: toastRow
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing16
        anchors.rightMargin: AppTheme.spacing8
        spacing: AppTheme.spacing12

        Label {
            objectName: "themeToastText"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            textFormat: Text.PlainText
            text: root.message
            color: AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            elide: Label.ElideRight
        }
        Rectangle {
            id: actionButton
            objectName: "themeToastAction"
            visible: root.actionText.length > 0
            implicitWidth: actionLabel.implicitWidth + AppTheme.spacing12 * 2
            implicitHeight: 28
            radius: AppTheme.radiusSm
            color: actionHover.containsMouse ? AppTheme.editorSelection
                                             : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.actionHint.length > 0 ? root.actionHint
                                                         : root.actionText
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                        || e.key === Qt.Key_Space) {
                    root.dismiss()
                    root.actionTriggered()
                    e.accepted = true
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: actionButton.activeFocus
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: AppTheme.editorAccent
            }
            Label {
                id: actionLabel
                anchors.centerIn: parent
                text: root.actionText
                color: AppTheme.editorAccent
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }
            MouseArea {
                id: actionHover
                anchors.fill: parent
                hoverEnabled: true
                ToolTip.visible: containsMouse && root.actionHint.length > 0
                ToolTip.delay: 400
                ToolTip.text: root.actionHint
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.dismiss()
                    root.actionTriggered()
                }
            }
        }
    }
}
