import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property var members: []
    property string fallbackName: ""
    property string colorKey: ""
    property bool highlighted: false

    readonly property int memberCount: members ? members.length : 0
    readonly property int cell: 16
    readonly property int gap: 2

    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusMd
        color: "transparent"
        border.width: 2
        border.color: root.highlighted ? AppTheme.accent
                                       : AppTheme.borderStrong
        z: 1
    }

    Loader {
        anchors.centerIn: parent
        active: root.memberCount === 1
        visible: active
        sourceComponent: Avatar {
            size: 24
            circle: false
            squareRadius: 7
            labelSize: 10
            name: (root.members[0] && root.members[0].name) || ""
            colorKey: (root.members[0] && root.members[0].spaceId) || ""
            mxc: (root.members[0] && root.members[0].avatarUrl) || ""
        }
    }

    Grid {
        anchors.centerIn: parent
        visible: root.memberCount >= 2
        columns: 2
        spacing: root.gap
        Repeater {
            model: root.memberCount >= 2 ? root.members.slice(0, 4) : []
            delegate: Avatar {
                required property var modelData
                size: root.cell
                circle: false
                squareRadius: 5
                labelSize: 8
                name: modelData.name || ""
                colorKey: modelData.spaceId || ""
                mxc: modelData.avatarUrl || ""
            }
        }
    }

    // An empty folder still renders: the user made it. Behind a Loader
    // because the name is empty during a rename, and an empty Text keeps
    // ItemObservesViewport.
    Loader {
        anchors.centerIn: parent
        active: root.memberCount === 0 && root.fallbackName.length > 0
        visible: active
        sourceComponent: Avatar {
            size: 28
            circle: false
            squareRadius: 8
            labelSize: 11
            name: root.fallbackName
            colorKey: root.colorKey
            mxc: ""
        }
    }
}
