import QtQuick
import QtQuick.Controls
import MatrixClient

// Resize handle for an AnchoredPopup, mounted on the popup's free corner
// (`corner`, normally the popup's own `gripCorner`): moving that corner away
// from the popup grows it, and the opposite corner stays put. The mark is one
// quarter-arc drawn as tangential segments with round caps, drawn for the
// top-left corner and turned for the others (the item is square).
// QtQuick.Shapes is not linked and Canvas paints nothing offscreen (see
// StormNode.qml), and clipping full rings leaves square ends. The hit area is
// larger than the mark and overlaps the search field's corner. DragHandler
// takes no exclusive grab until the drag threshold, and HoverHandler consumes
// nothing, so a plain click still reaches the field. Once the threshold is
// crossed the grab is held for the whole gesture (see grabPermissions). Pointer
// and touch only; no keyboard path (the default size is always usable).
// `target: null`: the handler moves nothing itself, and the size is computed
// from the size at press plus the total translation, so it never drifts.
Item {
    id: grip

    // The AnchoredPopup this grip resizes.
    property var popup
    // "topLeft" | "topRight" | "bottomRight" | "bottomLeft": the popup corner
    // this grip sits on, which must be the corner that moves.
    property string corner: "topLeft"
    readonly property bool onRight:
        corner === "topRight" || corner === "bottomRight"
    readonly property bool onBottom:
        corner === "bottomLeft" || corner === "bottomRight"
    // How far the grab area reaches beyond the popup's corner. A press just
    // outside the popup's item rect counts as outside: CloseOnPressOutside
    // fires and, since the picker is not modal, the press reaches the chat
    // behind. A band straddling the edge fixes that. Defaults to 0; the host
    // must also move the item out by the same amount and keep the band inside
    // the popup's item rect (asserted by the host's suite).
    property real grabMargin: 0

    // Arc centre, measured from the popup corner (not this item's origin), at
    // the panel's corner-radius centre so the arc runs parallel to the border.
    property real arcCentre: 18
    // Where the arc is drawn in this item's coordinates; the mark does not move
    // when the grab band grows.
    readonly property real drawCentre: arcCentre + grabMargin
    property real outerRadius: 15
    property real strokeWidth: 2.5
    // Enough segments that the round caps form a smooth curve.
    property int segments: 14

    // Size at drag start; the whole drag resolves against it.
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
        // Diagonal through the corner: ↖↘ for top-left and bottom-right.
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
        // Moving the corner away from the popup makes it bigger: right and
        // down on the right/bottom edges, left and up on the left/top ones.
        onTranslationChanged: {
            if (!active || !grip.popup)
                return
            var dx = grip.onRight ? activeTranslation.x : -activeTranslation.x
            var dy = grip.onBottom ? activeTranslation.y : -activeTranslation.y
            grip.popup.resizeTo(grip.pressWidth + dx, grip.pressHeight + dy)
        }
    }

    // One quarter-arc sweeping 180°..270° for the top-left corner, as short
    // tangential segments with round caps so it reads as one continuous
    // stroke; turned about the square's centre for the other corners.
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
                // Segment length so consecutive round caps just touch.
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
