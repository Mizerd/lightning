import QtQuick
import MatrixClient

// v0.6.7: the bottom-right resize corner for an AnchoredPopup, so the GIF and
// emoji pickers can be dragged bigger like a window.
//
// The drag pins the popup's top-left (AnchoredPopup.beginResize() detaches the
// placement bindings) so the corner tracks the pointer exactly instead of the
// popup growing symmetrically around its anchor.
//
// POINTER AND TOUCH ONLY. DragHandler accepts touch, so a tap-drag works, but
// there is deliberately no keyboard path: resizing is a convenience, the
// default size is always usable, and a key-driven resize would need its own
// focus stop inside a popup whose Tab order already ends at the content. Said
// plainly here rather than implied by an Accessible.Grip role that suggests
// otherwise.
//
// A DragHandler with `target: null` — never a pointer-grabbing mouse area. The
// handler reports the translation accumulated since the press and moves
// nothing itself, so the
// size is always computed from the size at press plus the total drag, never
// accumulated frame by frame (which drifts). It also cannot steal the wheel
// or the scroll gestures the grid underneath needs.
Item {
    id: grip

    // The AnchoredPopup this grip resizes.
    property var popup

    // Size at the moment the drag started; the whole drag is resolved against
    // this, so a dropped frame cannot accumulate error.
    property real pressWidth: 0
    property real pressHeight: 0

    objectName: "popupResizeGrip"
    width: 16
    height: 16
    z: 10
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
                grip.popup.beginResize()
            } else {
                grip.popup.endResize()
            }
        }
        // The notify signal is translationChanged; the property to read is
        // `activeTranslation` (a QVector2D of the drag since the press).
        // Qt 6 exposes no plain `translation` property to QML — only a C++
        // accessor — so reading `translation` here would be undefined.
        onTranslationChanged: {
            if (!active || !grip.popup)
                return
            grip.popup.resizeTo(grip.pressWidth + activeTranslation.x,
                                grip.pressHeight + activeTranslation.y)
        }
    }

    // Three diagonal ticks, the conventional grip shape. Faint at rest so it
    // does not compete with the content, and it firms up on hover/drag.
    Item {
        anchors.fill: parent
        opacity: gripHover.hovered || dragHandler.active ? 1 : 0.45
        Behavior on opacity { NumberAnimation { duration: 90 } }
        Repeater {
            model: 3
            Rectangle {
                required property int index
                width: 2
                height: 3 + index * 4
                radius: 1
                color: AppTheme.stormTextMuted
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 3
                anchors.right: parent.right
                anchors.rightMargin: 3 + index * 4
                transformOrigin: Item.Bottom
            }
        }
    }
}
