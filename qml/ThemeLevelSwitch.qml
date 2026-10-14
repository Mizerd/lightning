import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Simple / Groups / Elements: how much structure the theme editor shows. One
// editor, three amounts of detail; nothing is removed at a lower level, a click
// on the sample window always reaches the element it landed on.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml):
// a theme being edited must not be able to hide its own editor.
Rectangle {
    id: root

    // 0 Simple, 1 Groups, 2 Elements.
    property int level: 0
    signal levelRequested(int level)

    readonly property var names: [qsTr("Simple"), qsTr("Groups"), qsTr("Elements")]
    readonly property var hints: [
        qsTr("A few base colours; everything else follows them"),
        qsTr("Each group of colours, such as panels or text"),
        qsTr("Every part of the window on its own")
    ]

    objectName: "themeLevelSwitch"
    implicitWidth: row.implicitWidth + 4
    implicitHeight: 32
    radius: AppTheme.radiusMd
    color: AppTheme.editorInset
    border.width: 1
    border.color: AppTheme.editorBorderStrong
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Detail level")

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            model: 3
            delegate: Rectangle {
                id: seg
                required property int index
                readonly property bool current: root.level === seg.index
                objectName: "themeLevel_" + seg.index
                width: segLabel.implicitWidth + AppTheme.spacing12 * 2
                height: 26
                radius: AppTheme.radiusSm
                color: seg.current ? AppTheme.editorAccent
                     : segHover.containsMouse ? AppTheme.editorSelection
                                              : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: root.names[seg.index]
                Accessible.description: root.hints[seg.index]
                Accessible.checked: seg.current
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                            || e.key === Qt.Key_Space) {
                        root.levelRequested(seg.index)
                        e.accepted = true
                    } else if (e.key === Qt.Key_Left && seg.index > 0) {
                        root.levelRequested(seg.index - 1)
                        e.accepted = true
                    } else if (e.key === Qt.Key_Right && seg.index < 2) {
                        root.levelRequested(seg.index + 1)
                        e.accepted = true
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    visible: seg.activeFocus
                    radius: AppTheme.radiusSm
                    color: "transparent"
                    border.width: 2
                    border.color: seg.current ? AppTheme.editorAccentInk
                                              : AppTheme.editorAccent
                }
                Label {
                    id: segLabel
                    anchors.centerIn: parent
                    text: root.names[seg.index]
                    color: seg.current ? AppTheme.editorAccentInk
                                       : AppTheme.editorText
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                }
                MouseArea {
                    id: segHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.levelRequested(seg.index)
                }
            }
        }
    }
}
