import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: grip

    property var popup
    property string corner: "topLeft"
    readonly property bool onRight:
        corner === "topRight" || corner === "bottomRight"
    readonly property bool onBottom:
        corner === "bottomLeft" || corner === "bottomRight"
    property real grabMargin: 0

    property real arcCentre: 18
    readonly property real drawCentre: arcCentre + grabMargin
    property real outerRadius: 15
    property real strokeWidth: 2.5
    property int segments: 14

    property real pressWidth: 0
    property real pressHeight: 0

    readonly property bool engaged: gripHover.hovered || dragHandler.active

    objectName: "popupResizeGrip"
    width: 28 + grabMargin
    height: 28 + grabMargin
    z: 20
    Accessible.role: Accessible.Grip
    Accessible.name: qsTr("Resize")
    ToolTip.text: qsTr("Drag to resize")
    ToolTip.visible: gripHover.hovered
    ToolTip.delay: 600

    HoverHandler {
        id: gripHover
        cursorShape: grip.onRight === grip.onBottom ? Qt.SizeFDiagCursor
                                                    : Qt.SizeBDiagCursor
    }

    DragHandler {
        id: dragHandler
        target: null
        // Keep the grab once the drag is ours: with the default Approves* bits
        // the timeline Flickable underneath could take it mid-resize (scrolling
        // the chat and stopping the resize). CanTakeOverFrom* stays so crossing
        // the threshold can take the press from the field underneath.
        grabPermissions: PointerHandler.CanTakeOverFromItems
                         | PointerHandler.CanTakeOverFromHandlersOfDifferentType
        onActiveChanged: {
            if (!grip.popup)
                return
            if (active) {
                grip.pressWidth = grip.popup.width
                grip.pressHeight = grip.popup.height
                if (grip.popup.beginResize)
                    grip.popup.beginResize()
            } else {
                grip.popup.endResize()
            }
        }
        onTranslationChanged: {
            if (!active || !grip.popup)
                return
            var dx = grip.onRight ? activeTranslation.x : -activeTranslation.x
            var dy = grip.onBottom ? activeTranslation.y : -activeTranslation.y
            grip.popup.resizeTo(grip.pressWidth + dx, grip.pressHeight + dy)
        }
    }

    Item {
        anchors.fill: parent
        rotation: grip.corner === "topRight" ? 90
                : grip.corner === "bottomRight" ? 180
                : grip.corner === "bottomLeft" ? 270 : 0
        Repeater {
            model: grip.segments
            Rectangle {
                required property int index
                readonly property real angle:
                    Math.PI + (index / (grip.segments - 1)) * (Math.PI / 2)
                width: (Math.PI / 2) * grip.outerRadius / (grip.segments - 1)
                       + grip.strokeWidth
                height: grip.strokeWidth
                radius: grip.strokeWidth / 2
                antialiasing: true
                color: AppTheme.bolt
                opacity: grip.engaged ? 1 : 0.9
                x: grip.drawCentre + Math.cos(angle) * grip.outerRadius - width / 2
                y: grip.drawCentre + Math.sin(angle) * grip.outerRadius - height / 2
                rotation: angle * 180 / Math.PI + 90
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }
    }
}
