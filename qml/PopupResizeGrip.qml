import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.6.7: the resize handle for an AnchoredPopup.
//
// The popup is pinned by its bottom-right corner to the composer it sits on,
// so that corner cannot move — the top-left is the only free one, and dragging
// it away from the anchor is what makes the popup bigger. Hence the inverted
// arithmetic below: moving up/left is positive size.
//
// ── The mark: one quarter-arc in bolt ──────────────────────────────────
//
// Drawn as TANGENTIAL SEGMENTS, not as clipped rings. That is not a stylistic
// choice: QtQuick.Shapes is not linked in this application and Canvas paints
// nothing under the offscreen platform (established by the trust card, see
// StormNode.qml and TrustCard.qml, verified there by pixel assertion). The
// previous version drew full rounded Rectangles and clipped them to the corner
// quadrant, which is exactly the hard sliced-off ends visible in the report —
// a clip has square edges, so a clipped ring can never end cleanly. Segments
// with rounded caps end where the arc ends.
//
// Five earlier attempts, each failing differently:
//
//   1. Bottom-right at 45% opacity — invisible.
//   2. Bracket + dots — visible but meaningless; read as a placeholder arrow.
//   3. Straight diagonal strokes in the corner — swallowed by the search
//      field's bolt focus ring, which is drawn there the moment the picker
//      opens (the field takes focus on open).
//   4. A bordered chip in the header row — legible, but a clunky button
//      occupying layout space the header had none of.
//   5. Clipped concentric rings — right idea, wrong construction: the clip
//      sliced the arcs off square.
//   6. Two nested segment arcs — the outer one landed; the inner degraded
//      into a stub at this size and was cut.
//
// The hit area is deliberately larger than the mark and overlaps the search
// field's rounded corner. That is safe: DragHandler takes no exclusive grab
// until the drag threshold is crossed, and HoverHandler consumes nothing, so a
// plain click still lands on the field — only an actual drag resizes.
//
// POINTER AND TOUCH ONLY. DragHandler accepts touch, so a tap-drag works, but
// there is deliberately no keyboard path: resizing is a convenience, the
// default size is always usable, and a key-driven resize would need its own
// focus stop inside a popup whose Tab order already ends at the content.
//
// `target: null` — never a pointer-grabbing mouse area, which could steal the
// wheel or the drag gestures the grid underneath needs. The handler reports
// the translation accumulated since the press and moves nothing itself, so the
// size is always computed from the size at press plus the total drag, never
// accumulated frame by frame (which drifts).
Item {
    id: grip

    // The AnchoredPopup this grip resizes.
    property var popup
    // Centre of the arcs, measured from this item's top-left. Sits at the
    // panel's corner-radius centre so the arcs run parallel to the border.
    property real arcCentre: 18
    property real outerRadius: 15
    property real strokeWidth: 2.5
    // Segment count: enough that the round caps overlap into a smooth curve.
    property int segments: 14

    // Size at the moment the drag started; the whole drag resolves against
    // this, so a dropped frame cannot accumulate error.
    property real pressWidth: 0
    property real pressHeight: 0

    readonly property bool engaged: gripHover.hovered || dragHandler.active

    objectName: "popupResizeGrip"
    width: 28
    height: 28
    z: 20
    Accessible.role: Accessible.Grip
    Accessible.name: qsTr("Resize")
    ToolTip.text: qsTr("Drag to resize")
    ToolTip.visible: gripHover.hovered
    ToolTip.delay: 600

    HoverHandler {
        id: gripHover
        cursorShape: Qt.SizeFDiagCursor
    }

    DragHandler {
        id: dragHandler
        target: null
        onActiveChanged: {
            if (!grip.popup)
                return
            if (active) {
                grip.pressWidth = grip.popup.width
                grip.pressHeight = grip.popup.height
            } else {
                grip.popup.endResize()
            }
        }
        // Inverted: dragging the TOP-LEFT corner up and to the left (negative
        // translation) has to make the popup bigger.
        onTranslationChanged: {
            if (!active || !grip.popup)
                return
            grip.popup.resizeTo(grip.pressWidth - activeTranslation.x,
                                grip.pressHeight - activeTranslation.y)
        }
    }

    // ONE quarter-arc sweeping the 180°..270° quadrant — the one facing the
    // top-left corner. A dense run of short tangential segments with rounded
    // caps, so it reads as a single continuous stroke that ends cleanly.
    //
    // A second, inner arc was tried and cut: at this size it degraded into a
    // stub rather than reading as depth, and the single arc is the cleaner
    // mark. Keeping it flat (one Repeater, not a nested pair) also removes the
    // parent-chain depth that produced undefined geometry in the nested
    // version.
    Repeater {
        model: grip.segments
        Rectangle {
            required property int index
            readonly property real angle:
                Math.PI + (index / (grip.segments - 1)) * (Math.PI / 2)
            // Segment length set so consecutive round caps just touch.
            width: (Math.PI / 2) * grip.outerRadius / (grip.segments - 1)
                   + grip.strokeWidth
            height: grip.strokeWidth
            radius: grip.strokeWidth / 2
            antialiasing: true
            color: AppTheme.bolt
            opacity: grip.engaged ? 1 : 0.9
            x: grip.arcCentre + Math.cos(angle) * grip.outerRadius - width / 2
            y: grip.arcCentre + Math.sin(angle) * grip.outerRadius - height / 2
            rotation: angle * 180 / Math.PI + 90
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }
}
