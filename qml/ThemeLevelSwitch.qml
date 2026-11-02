import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

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
    Accessible.focusable: true
    Accessible.name: qsTr("Detail level")
    Accessible.description: root.names[root.level] + ". " + root.hints[root.level]

    // A radio group is ONE tab stop: the switch itself holds focus and the
    // arrows move the selection, with the ring drawn on the current segment.
    // (Per-segment tab stops left the ring on the old segment after an arrow.)
    activeFocusOnTab: true
    Keys.onPressed: (e) => {
        let target = -1
        if (e.key === Qt.Key_Left) target = root.level - 1
        else if (e.key === Qt.Key_Right) target = root.level + 1
        else if (e.key === Qt.Key_Home) target = 0
        else if (e.key === Qt.Key_End) target = 2
        else return
        if (target >= 0 && target <= 2 && target !== root.level)
            root.levelRequested(target)
        e.accepted = true
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            id: segments
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
                Accessible.role: Accessible.RadioButton
                Accessible.name: root.names[seg.index]
                Accessible.description: root.hints[seg.index]
                Accessible.checked: seg.current
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    visible: root.activeFocus && seg.current
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
                    onClicked: {
                        root.forceActiveFocus(Qt.MouseFocusReason)
                        root.levelRequested(seg.index)
                    }
                }
            }
        }
    }
}
