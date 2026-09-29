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
// reduced motion keep the still picture. A banner scrolled out of its
// Flickable (Space Home) stops too.
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
                                   && _windowShown && _inViewport
                                   && (_mode === 0 || _hovered)

    // The nearest Flickable's viewport, checked on every activation and once
    // scrolling pauses.
    property Item _viewport: null
    property bool _inViewport: true
    function _findViewport() {
        for (var p = root.parent; p; p = p.parent) {
            if (p.contentY !== undefined && p.flickableDirection !== undefined)
                return p
        }
        return null
    }
    function _checkViewport() {
        var f = _viewport
        if (!f || width <= 0 || height <= 0) {
            _inViewport = true
            return
        }
        var r = root.mapToItem(f, 0, 0, width, height)
        _inViewport = r.x + r.width > 0 && r.x < f.width
                      && r.y + r.height > 0 && r.y < f.height
    }
    Component.onCompleted: _viewport = _findViewport()
    onParentChanged: _viewport = _findViewport()
    Connections {
        target: root._viewport
        enabled: root._allowed && root.stillReady
        function onContentYChanged() { viewportSettle.restart() }
        function onContentXChanged() { viewportSettle.restart() }
        function onHeightChanged() { viewportSettle.restart() }
        function onWidthChanged() { viewportSettle.restart() }
    }
    Timer {
        id: viewportSettle
        interval: 120
        onTriggered: root._checkViewport()
    }
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
    // Deferred: this runs inside _wants' own evaluation, and the viewport check
    // writes _inViewport, which _wants reads.
    on_WantsChanged: Qt.callLater(root._apply)
    function _apply() {
        _checkViewport()
        _ask()
    }
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
            id: bannerImage
            objectName: "bannerAnimatedImage"
            source: root._src
            // Keeps the bridge from evicting the file mid-play.
            AnimationFileHold {}
            // A decoder that stopped on its own leaves `playing` false, and a
            // new source would then load paused on its first frame.
            onSourceChanged: bannerImage.playing = true
            // Stopped on its own: a finite animation ended, or its file is
            // gone. Only the second changes anything when the bridge is asked.
            onPlayingChanged: {
                if (!bannerImage.playing
                    && bannerImage.status === AnimatedImage.Ready)
                    Qt.callLater(root._ask)
            }
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
