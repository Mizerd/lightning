import QtQuick
import MatrixClient

Item {
    id: root

    property int size: 16
    property bool complete: false
    property string iconName: "check"

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        objectName: "stormNodeFill"
        visible: root.complete
        anchors.fill: parent
        radius: width / 2
        color: AppTheme.bolt
    }

    Item {
        objectName: "stormNodeDashRing"
        visible: !root.complete
        anchors.fill: parent
        Repeater {
            model: root.size >= 22 ? 8 : 6
            delegate: Rectangle {
                required property int index
                readonly property int dashCount: root.size >= 22 ? 8 : 6
                readonly property real angle: index * 2 * Math.PI / dashCount
                width: root.size >= 22 ? 6 : 4.5
                height: 2
                radius: 1
                color: AppTheme.stormBorderStrong
                x: parent.width / 2
                   + Math.cos(angle) * (parent.width / 2 - 1)
                   - width / 2
                y: parent.height / 2
                   + Math.sin(angle) * (parent.height / 2 - 1)
                   - height / 2
                rotation: angle * 180 / Math.PI + 90
            }
        }
    }

    Icon {
        visible: root.iconName.length > 0
        anchors.centerIn: parent
        name: root.iconName
        size: Math.round(root.size * 0.62)
        color: root.complete ? AppTheme.boltInk : AppTheme.stormTextMuted
    }
}
