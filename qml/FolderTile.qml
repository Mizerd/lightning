import QtQuick
import QtQuick.Controls
import MatrixClient

// A local Space folder's tile: a composite of the Spaces inside it.
//
// Discord's folder icon is a grid of the servers it holds, and that is not
// decoration. A collapsed folder raises exactly one question — WHICH folder is
// this — and a generic letter tile answers the one question the user already
// knows the answer to. Four member avatars answer the real one at a glance, on
// 40 px, without a label.
//
// The bundled Material Symbols subset carries no folder glyph (regenerating it
// needs the network), so there is no icon fallback to fall back TO: an empty
// folder shows its NAME, which is at least the thing the user typed.
//
// Every colour is a theme token. Nothing here is Discord's.
Item {
    id: root

    /// Up to four { spaceId, name, avatarUrl } maps, in the folder's order.
    property var members: []
    /// Shown when the folder has no members left to preview.
    property string fallbackName: ""
    /// Fallback-avatar colour key for the empty case.
    property string colorKey: ""
    /// True while a drag is hovering this folder.
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

    // One member: a single larger tile, centred. A 2×2 grid with one cell
    // filled reads as a rendering fault rather than as a folder.
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

    // Two to four: the grid, filled from the top left in folder order.
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

    // An empty folder still renders: it is a place the user made, and one
    // that vanished when its last Space moved out would be a bug report.
    // Behind a Loader because the name is legitimately empty while a rename
    // is in flight, and a never-laid-out empty Text keeps
    // ItemObservesViewport for the life of the delegate.
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
