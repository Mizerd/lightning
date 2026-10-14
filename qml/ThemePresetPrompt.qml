import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The question asked when a preset is chosen while the theme already has edits:
// "Start from Dusk? You have changed 7 colours." with Keep my changes (the
// preset becomes the base and the edits stay on top of it), Replace (the edits
// go, as one undoable step) and Cancel. Shown over the sample window, where
// the eye already is; it holds no theme state and decides nothing itself.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
Rectangle {
    id: root

    // The preset asked for, by name.
    property string presetName: ""
    // How many colours (and gradients) the theme has of its own.
    property int changes: 0

    signal keepRequested()
    signal replaceRequested()
    signal cancelRequested()

    objectName: "themePresetPrompt"
    implicitWidth: Math.min(460, column.implicitWidth + AppTheme.spacing16 * 2)
    implicitHeight: column.implicitHeight + AppTheme.spacing16 * 2
    radius: AppTheme.radiusMd
    color: AppTheme.editorPanel
    border.width: 1
    border.color: AppTheme.editorBorderStrong
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Start from %1?").arg(root.presetName)

    // Self-contained button, so this file depends on no other file's parts.
    component PromptButton: Rectangle {
        id: btn
        property string text: ""
        property bool primary: false
        property bool danger: false
        signal clicked()
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing16 * 2
        implicitHeight: 32
        radius: AppTheme.radiusMd
        color: btn.primary
               ? (btnHover.containsMouse ? Qt.lighter(AppTheme.editorAccent, 1.08)
                                         : AppTheme.editorAccent)
               : btnHover.containsMouse ? AppTheme.editorInset : "transparent"
        border.width: btn.primary ? 0 : 1
        border.color: btn.danger ? AppTheme.editorDanger
                                 : AppTheme.editorBorderStrong
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: btn.text
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                    || e.key === Qt.Key_Space) {
                btn.clicked()
                e.accepted = true
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            visible: btn.activeFocus
            radius: AppTheme.radiusSm
            color: "transparent"
            border.width: 2
            border.color: btn.primary ? AppTheme.editorAccentInk
                                      : AppTheme.editorAccent
        }
        Label {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.text
            color: btn.primary ? AppTheme.editorAccentInk
                 : btn.danger ? AppTheme.editorDanger : AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }
        MouseArea {
            id: btnHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    // A press must not fall through to the sample window beneath.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing8

        Label {
            objectName: "themePresetPromptTitle"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: qsTr("Start from %1?").arg(root.presetName)
            color: AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("This theme has %n colour(s) of its own.",
                       "custom theme, count of edited roles", root.changes)
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("Keep them on top of the new preset, or start over from it?")
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            spacing: AppTheme.spacing8
            PromptButton {
                objectName: "themePresetKeepButton"
                primary: true
                text: qsTr("Keep my changes")
                onClicked: root.keepRequested()
            }
            PromptButton {
                objectName: "themePresetReplaceButton"
                danger: true
                text: qsTr("Replace")
                onClicked: root.replaceRequested()
            }
            PromptButton {
                objectName: "themePresetCancelButton"
                text: qsTr("Cancel")
                onClicked: root.cancelRequested()
            }
        }
    }
}
