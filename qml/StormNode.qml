import QtQuick
import MatrixClient

// Storm node state (SPEC-storm-language §3.3), the shared on/off circle for
// radio rows, flyout options, toggles and chain steps: complete/on = bolt
// fill with a dark glyph; pending/off = dashed stormBorderStrong ring with a
// muted glyph. The dashed ring is plain declarative geometry (tangential
// dash rectangles) — QtQuick.Shapes is not linked and Canvas paints nothing
// under the offscreen platform (see the trust card, which established the
// idiom).
Item {
    id: root

    // 16px in menu radio rows, 24px in trust chains.
    property int size: 16
    property bool complete: false
    // Glyph inside the node; empty hides it (a bare dashed ring).
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
            // 6 dashes at 16px, 8 at trust-chain scale — the same ~7px arc
            // pitch either way.
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
        // Ink on the bolt fill, not the panel ink — boltInk (Storm: deep
        // canvas navy; legacy: accentText) stays readable once bolt routes
        // to each legacy theme's own accent.
        color: root.complete ? AppTheme.boltInk : AppTheme.stormTextMuted
    }
}
