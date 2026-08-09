import QtQuick
import MatrixClient

// v0.6.7: the resize corner for an AnchoredPopup.
//
// TOP-LEFT, not bottom-right. The popup is pinned by its bottom-right corner
// to the composer it sits on, so that corner cannot move — the top-left is the
// only free one, and dragging it away from the anchor is what makes the popup
// bigger. Hence the inverted arithmetic below: moving left/up is positive size.
//
// The markings are deliberately visible AT REST. The first version faded to
// 45% and the maintainer could not find it at all ("i dont see anyway to
// resize it"), so the affordance now reads as a real control: three stepped
// diagonal ticks plus a corner bracket, at full strength on hover.
//
// POINTER AND TOUCH ONLY. DragHandler accepts touch, so a tap-drag works, but
// there is deliberately no keyboard path: resizing is a convenience, the
// default size is always usable, and a key-driven resize would need its own
// focus stop inside a popup whose Tab order already ends at the content.
//
// A DragHandler with `target: null` — never a pointer-grabbing mouse area,
// which could steal the wheel or the drag gestures the grid underneath needs.
// It reports the translation accumulated since the press and moves nothing
// itself, so the size is always computed from the size at press plus the total
// drag, never accumulated frame by frame (which drifts).
Item {
    id: grip

    // The AnchoredPopup this grip resizes.
    property var popup

    // Size at the moment the drag started; the whole drag resolves against
    // this, so a dropped frame cannot accumulate error.
    property real pressWidth: 0
    property real pressHeight: 0

    objectName: "popupResizeGrip"
    width: 18
    height: 18
    z: 20
    Accessible.role: Accessible.Grip
    Accessible.name: qsTr("Resize")

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

    // Corner bracket + three stepped ticks, pointing up-left along the drag
    // direction.
    Item {
        anchors.fill: parent
        opacity: gripHover.hovered || dragHandler.active ? 1 : 0.75
        Behavior on opacity { NumberAnimation { duration: 90 } }

        // The bracket: two short strokes meeting at the corner.
        Rectangle {
            x: 2; y: 2
            width: 9; height: 1.5
            radius: 0.75
            color: AppTheme.stormTextMuted
        }
        Rectangle {
            x: 2; y: 2
            width: 1.5; height: 9
            radius: 0.75
            color: AppTheme.stormTextMuted
        }

        // Diagonal ticks stepping away from the corner.
        Repeater {
            model: 3
            Rectangle {
                required property int index
                width: 2
                height: 2
                radius: 1
                color: AppTheme.stormTextMuted
                x: 5 + index * 3
                y: 5 + index * 3
            }
        }
    }
}
