import QtQuick
import MatrixClient

// A pack image (MSC2545) drawn from its thumbnail, and animated under Play
// automatically. The motion file comes from the bridge's motion probe, which
// writes one only for a sniffed animated GIF/WebP within the avatar pixel cap,
// so SVG or a lying mimetype never reaches the AnimatedImage.
Item {
    id: emoji

    property string mxc: ""
    property int edge: 64
    property bool retained: true
    property bool onScreen: true
    // A picker or popup the user is looking at outranks passive rows for a
    // motion slot.
    property bool prominent: false
    property bool stillCache: false

    // Typeof-guarded and re-resolved at completion, as in Avatar.qml: a
    // delegate built synchronously can see `app` undefined on its first lookup.
    property var bridge: (typeof app !== "undefined" && app)
                         ? app.mediaBridge : null
    readonly property bool _usable: mxc.indexOf("mxc://") === 0 && mxc.length > 6
                                    && !!bridge && bridge.supported === true
    readonly property bool wantsMotion: _usable && retained && onScreen
                                        && visible && MotionPolicy.autoplay
    property int _stillTick: 0
    property int _motionTick: 0
    readonly property string motionSource: {
        var _ = _motionTick
        return wantsMotion ? bridge.avatarAnimationSource(mxc, true) : ""
    }
    property bool _slotHeld: false
    readonly property bool animating:
        motion.item !== null && motion.item.status === AnimatedImage.Ready

    function _syncSlot() {
        if (!bridge)
            return
        var want = motionSource !== ""
        if (want && !_slotHeld)
            _slotHeld = bridge.claimMotionSlot(emoji, prominent)
        else if (!want && _slotHeld) {
            _slotHeld = false
            bridge.releaseMotionSlot(emoji)
        }
    }
    onMotionSourceChanged: _syncSlot()
    Component.onCompleted: {
        if (!bridge && typeof app !== "undefined" && app && app.mediaBridge)
            bridge = app.mediaBridge
        _syncSlot()
    }
    Component.onDestruction: if (_slotHeld && bridge) bridge.releaseMotionSlot(emoji)

    Image {
        id: still
        objectName: "customEmojiStill"
        anchors.fill: parent
        visible: !emoji.animating
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: emoji.stillCache
        sourceSize.width: emoji.edge
        sourceSize.height: emoji.edge
        source: {
            var _ = emoji._stillTick
            return emoji._usable && emoji.retained
                ? emoji.bridge.mxcImageSource(emoji.mxc, emoji.edge) : ""
        }
    }

    Loader {
        id: motion
        anchors.fill: parent
        active: emoji._slotHeld && emoji.motionSource !== ""
        sourceComponent: AnimatedImage {
            objectName: "customEmojiAnimatedImage"
            source: emoji.motionSource
            fillMode: Image.PreserveAspectFit
            playing: !MotionPolicy.suspended
            asynchronous: true
            cache: false
            smooth: true
            AnimationFileHold {}
        }
    }

    Connections {
        target: emoji.bridge
        enabled: emoji._usable
        ignoreUnknownSignals: true
        function onMediaCached(cacheKey) {
            if (cacheKey.endsWith(":" + emoji.mxc) && !cacheKey.startsWith("motion:"))
                emoji._stillTick++
        }
        function onAnimatedMediaReady(cacheKey) {
            if (cacheKey === "motion:" + emoji.mxc)
                emoji._motionTick++
        }
        function onMotionSlotFreed() { emoji._syncSlot() }
    }
}
