import QtQuick
import MatrixClient

// Declared inside an AnimatedImage that shows one of MediaBridge's scratch
// files (animatedSource, mxcAnimatedSource, previewAnimatedSource, the motion
// sources). It holds the file while the image shows it, so the bridge's
// eviction cannot delete it mid-play: a decoder whose file vanishes stops at
// its next loop without ever reporting an error, frozen on its last frame.
//
// The hold follows the image's source and goes with the image (this item is
// its child); a source the bridge did not write, or "", holds nothing.
Item {
    id: hold
    visible: false
    // The AnimatedImage whose source is held; the item it is declared in.
    property Item image: parent
    readonly property var _bridge: (typeof app !== "undefined" && app)
                                   ? app.mediaBridge : null
    function refresh() {
        if (hold._bridge && hold.image)
            hold._bridge.holdAnimation(hold, hold.image.source.toString())
    }
    Component.onCompleted: refresh()
    Connections {
        target: hold.image
        function onSourceChanged() { hold.refresh() }
    }
}
