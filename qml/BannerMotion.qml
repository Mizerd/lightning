import QtQuick
import MatrixClient

Item {
    id: root

    property string mxc: ""
    property bool stillReady: false
    property int fillMode: Image.PreserveAspectCrop
    readonly property bool shown: loader.item !== null
                                  && loader.item.status === AnimatedImage.Ready

    readonly property var _bridge: (typeof app !== "undefined" && app)
                                   ? app.mediaBridge : null
    readonly property int _mode: MotionPolicy.mode
    readonly property bool _allowed: mxc.length > 0 && _mode !== 2
    property bool _hovered: false
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
    readonly property bool _wants: _allowed && stillReady && visible
                                   && _windowShown && _inViewport
                                   && (_mode === 0 || _hovered)

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
    property bool _retried: false

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
        active: root._wants && root._src !== "" && !root._failed
        sourceComponent: AnimatedImage {
            id: bannerImage
            objectName: "bannerAnimatedImage"
            source: root._src
            AnimationFileHold {}
            onSourceChanged: bannerImage.playing = true
            onPlayingChanged: {
                if (!bannerImage.playing
                    && bannerImage.status === AnimatedImage.Ready)
                    Qt.callLater(root._ask)
            }
            fillMode: root.fillMode
            asynchronous: true
            cache: false
            playing: true
            paused: MotionPolicy.suspended
            smooth: true
            onStatusChanged: {
                if (status === AnimatedImage.Error)
                    Qt.callLater(root._error)
            }
        }
    }
}
