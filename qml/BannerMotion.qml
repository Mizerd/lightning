import QtQuick
import MatrixClient

// Plays an animated banner (a GIF with two or more frames, or an animated
// WebP) over the still banner Image beneath it. Profile banners and Space
// banners share it.
//
// The still Image stays the source of truth: it loads the full payload through
// MediaBridge::wideImageSource(), and once it is ready this asks
// wideAnimationSource() for the same bytes as a scratch file. Nothing extra is
// downloaded, and nothing plays until the still picture is on screen. The
// caller clips or masks; this fills its parent.
//
// Policy is the GIF autoplay setting, like every other passive animation:
// Always plays, On hover plays while the pointer is over the banner, Never and
// reduced motion keep the still picture.
Item {
    id: root

    // The banner's mxc URI, as the still Image uses it.
    property string mxc: ""
    // The still Image beneath has loaded (its bytes are in the cache).
    property bool stillReady: false
    property int fillMode: Image.PreserveAspectCrop
    // The animation has a frame on screen; the still Image may hide under it.
    readonly property bool shown: loader.item !== null
                                  && loader.item.status === AnimatedImage.Ready

    readonly property var _bridge: (typeof app !== "undefined" && app)
                                   ? app.mediaBridge : null
    readonly property int _mode: (typeof app !== "undefined" && app
                                  && app.settings)
                                 ? app.settings.gifAutoplay : 2
    readonly property bool _allowed: mxc.length > 0 && _mode !== 2
                                     && !AppTheme.reducedMotion
    property bool _hovered: false
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
    readonly property bool _wants: _allowed && stillReady && visible
                                   && _windowShown
                                   && (_mode === 0 || _hovered)
    property string _src: ""
    property bool _failed: false
    // One reload after an error: the scratch file may have been evicted.
    property bool _retried: false

    // Asked on every activation, never cached here: the bridge's file can be
    // evicted meanwhile.
    function _ask() {
        if (!_wants || _failed || !_bridge)
            return
        _src = _bridge.wideAnimationSource(mxc)
    }
    function _error() {
        if (!_retried) {
            _retried = true
            _src = ""
            _ask()
            return
        }
        _failed = true
    }
    on_WantsChanged: _ask()
    onMxcChanged: {
        _src = ""
        _failed = false
        _retried = false
        _ask()
    }

    HoverHandler {
        enabled: root._allowed && root._mode === 1
        onHoveredChanged: root._hovered = hovered
    }

    Loader {
        id: loader
        objectName: "bannerMotion"
        anchors.fill: parent
        // Torn down whenever it may not play, so a paused banner holds no
        // decoder.
        active: root._wants && root._src !== "" && !root._failed
        sourceComponent: AnimatedImage {
            objectName: "bannerAnimatedImage"
            source: root._src
            fillMode: root.fillMode
            asynchronous: true
            cache: false
            playing: true
            smooth: true
            onStatusChanged: {
                // Deferred: handling it tears this very item down.
                if (status === AnimatedImage.Error)
                    Qt.callLater(root._error)
            }
        }
    }
}
