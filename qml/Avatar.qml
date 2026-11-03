import QtQuick
import QtQuick.Effects
import QtQuick.Controls
import MatrixClient

Rectangle {
    id: root

    property string mxc: ""
    property string name: ""
    property int size: 40
    property bool circle: true
    property int squareRadius: AppTheme.radiusMd
    property bool roomGlyph: false
    property string colorKey: ""
    property int labelSize: 0
    property bool onScreen: true

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    radius: circle ? size / 2 : squareRadius

    property bool fetchFailed: false
    readonly property string presentationState:
        !hasImage ? "missing"
        : img.status === Image.Ready ? "ready"
        : (fetchFailed || img.status === Image.Error) ? "failed"
        : "loading"

    readonly property string _paletteKey: colorKey.length > 0 ? colorKey : name
    function _paletteColor(key) {
        return AppTheme.avatarColor(key)
    }
    color: presentationState === "missing" || presentationState === "failed"
           ? (_paletteKey.length > 0 ? _paletteColor(_paletteKey)
                                     : AppTheme.cardElevated)
           : "transparent"

    function _isAlnum(cp) {
        if (cp < 0x80)
            return (cp >= 0x30 && cp <= 0x39) || (cp >= 0x41 && cp <= 0x5a)
                   || (cp >= 0x61 && cp <= 0x7a)
        if (cp < 0xc0 || cp === 0xd7 || cp === 0xf7)
            return cp === 0xaa || cp === 0xb5 || cp === 0xba
        if ((cp >= 0x2000 && cp <= 0x2bff) || (cp >= 0x3000 && cp <= 0x303f)
                || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xe000 && cp <= 0xf8ff)
                || (cp >= 0xfe00 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff0f)
                || (cp >= 0xff1a && cp <= 0xff20) || (cp >= 0xfff0 && cp <= 0xffff)
                || (cp >= 0x1f000 && cp <= 0x1faff) || cp >= 0xe0000)
            return false
        return true
    }
    function _firstInitial(word) {
        for (var i = 0; i < word.length; ) {
            var cp = word.codePointAt(i)
            var len = cp > 0xffff ? 2 : 1
            if (_isAlnum(cp))
                return word.substr(i, len).toUpperCase()
            i += len
        }
        return ""
    }
    function _initials(value) {
        var cleaned = value.replace(/^[@#!+]/, "").replace(/<[^>]*>/g, " ").trim()
        var words = cleaned.split(/\s+/)
        var out = ""
        var found = 0
        for (var i = 0; i < words.length && found < 2; i++) {
            var initial = _firstInitial(words[i])
            if (initial.length > 0) {
                out += initial
                found++
            }
        }
        return out.length > 0 ? out : "?"
    }

    // The media bridge, resolved defensively. When a Repeater builds a delegate
    // synchronously from a property-change handler (e.g. read-receipt chips),
    // the first unqualified `app` lookup can resolve to undefined while later
    // ones work (observed; the Qt cause isn't established). A throwing binding
    // would stick, as its only dependency (`mxc`) never changes, so the binding
    // is typeof-guarded and resolveBridge() re-resolves at completion and on
    // every refresh(). Keep `bridge` the first `app` reference in this file: it
    // absorbs the poisoned lookup. A consumer may inject its own bridge;
    // resolveBridge() never overwrites a non-null value.
    property var bridge: (typeof app !== "undefined" && app)
                         ? app.mediaBridge : null
    function resolveBridge() {
        if (!bridge && typeof app !== "undefined" && app && app.mediaBridge)
            bridge = app.mediaBridge
    }
    readonly property bool hasImage:
        mxc.indexOf("mxc://") === 0 && bridge !== null && bridge !== undefined
        && bridge.supported
    property string src: ""

    property string fallbackSource: ""
    readonly property bool showingFallback:
        root.src === "" && root.fallbackSource !== ""

    property bool animate: true
    property bool prominent: false
    readonly property int _motionMode: MotionPolicy.mode
    property bool motionMaskable:
        root.GraphicsInfo.api !== GraphicsInfo.Software
    readonly property bool _motionAllowed:
        animate && hasImage && _motionMode !== 2
        && size >= 20 && motionMaskable
    property bool _hovered: false
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
    readonly property bool _wantsMotion:
        _motionAllowed && onScreen && visible && _windowShown && _inViewport
        && presentationState === "ready"
        && (_motionMode === 0 || _hovered)
    property string _motionSrc: ""
    property bool _motionFailed: false
    property bool _motionRetried: false

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
    onParentChanged: _viewport = _findViewport()
    Connections {
        target: root._viewport
        enabled: root._motionAllowed && root.onScreen
                 && root.presentationState === "ready"
                 && (root._motionMode === 0 || root._hovered)
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
    property bool _slotHeld: false
    readonly property bool _playing:
        _slotHeld && _motionSrc !== "" && !_motionFailed
    readonly property bool motionShown:
        motionLoader.item !== null
        && motionLoader.item.status === AnimatedImage.Ready

    function _requestMotion() {
        _checkViewport()
        if (!_wantsMotion || _motionFailed || !bridge)
            return
        _motionSrc = bridge.avatarAnimationSource(mxc, prominent || _hovered)
        _syncSlot()
    }
    function _motionStalled() {
        if (_wantsMotion && !_motionFailed)
            _requestMotion()
    }
    function _motionError() {
        if (!_motionRetried) {
            _motionRetried = true
            _motionSrc = ""
            _requestMotion()
            return
        }
        _motionFailed = true
        _syncSlot()
    }
    function _syncSlot() {
        var want = _wantsMotion && _motionSrc !== "" && !_motionFailed
        if (want && !_slotHeld && bridge) {
            _slotHeld = bridge.claimMotionSlot(root, prominent || _hovered)
        } else if (!want && _slotHeld) {
            _slotHeld = false
            if (bridge)
                bridge.releaseMotionSlot(root)
        }
    }
    // Deferred: this runs inside _wantsMotion's own evaluation, and
    // _requestMotion() re-checks the viewport, writing _inViewport, which
    // _wantsMotion reads (Qt reports that as a binding loop and abandons the
    // update).
    // Through Timers the avatar owns, never Qt.callLater: a call queued that
    // way outlives a delegate destroyed before the loop turns (a room switch
    // tears hundreds down) and runs in a dead context, one TypeError each.
    on_WantsMotionChanged: applyWantsMotionLater.restart()
    Timer { id: applyWantsMotionLater; interval: 0; onTriggered: root._applyWantsMotion() }
    Timer { id: motionStalledLater; interval: 0; onTriggered: root._motionStalled() }
    Timer { id: motionErrorLater; interval: 0; onTriggered: root._motionError() }
    function _applyWantsMotion() {
        if (_wantsMotion)
            _requestMotion()
        else
            _syncSlot()
    }

    on_HoveredChanged: {
        if (_hovered) {
            _requestMotion()
        } else if (_slotHeld && !prominent && bridge) {
            _slotHeld = bridge.claimMotionSlot(root, false)
        }
    }
    HoverHandler {
        enabled: root._motionAllowed
        onHoveredChanged: {
            if (hovered) {
                hoverDwell.restart()
            } else {
                hoverDwell.stop()
                root._hovered = false
            }
        }
    }
    Timer {
        id: hoverDwell
        interval: 200
        onTriggered: root._hovered = true
    }

    function refresh() {
        resolveBridge()
        if (!hasImage) {
            src = ""
            return
        }
        var s = bridge.avatarSource(mxc, size)
        src = s
        if (s === "" && bridge.avatarFailureCategory(mxc) !== "")
            fetchFailed = true
    }
    readonly property string _requestKey: hasImage ? mxc : ""
    on_RequestKeyChanged: {
        fetchFailed = false
        _motionSrc = ""
        _motionFailed = false
        _motionRetried = false
        _syncSlot()
        refresh()
        _requestMotion()
    }
    Component.onCompleted: {
        _viewport = _findViewport()
        refresh()
    }

    Connections {
        target: root.bridge
        enabled: root.hasImage
        function onMediaCached(cacheKey) {
            if (cacheKey.endsWith(":" + root.mxc)
                && !cacheKey.startsWith("motion:")
                && (root.src === "" || root.fetchFailed))
                root.refresh()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey.endsWith(":" + root.mxc)
                && !cacheKey.startsWith("motion:"))
                root.fetchFailed = true
        }
        function onMediaRetryable(cacheKey) {
            if (cacheKey.endsWith(":" + root.mxc)
                && !cacheKey.startsWith("motion:"))
                root.refresh()
        }
        function onAnimatedMediaReady(cacheKey) {
            if (cacheKey === "motion:" + root.mxc)
                root._requestMotion()
        }
        function onMotionSlotFreed() {
            if (root._wantsMotion && root._motionSrc !== ""
                && !root._slotHeld)
                root._requestMotion()
        }
    }

    Skeleton {
        objectName: "avatarSkeleton"
        anchors.fill: parent
        visible: root.presentationState === "loading"
        active: root.onScreen
        circle: root.circle
        pulseOnly: true
        radius: root.circle ? Math.min(width, height) / 2 : root.squareRadius
        color: root._paletteKey.length > 0
               ? Qt.alpha(root._paletteColor(root._paletteKey), 0.28)
               : AppTheme.cardElevated
    }

    Label {
        objectName: "avatarInitials"
        anchors.centerIn: parent
        visible: root.presentationState === "missing"
                 || root.presentationState === "failed"
        text: root._initials(root.name)
        textFormat: Text.PlainText
        color: root._paletteKey.length > 0
               ? AppTheme.avatarInk(root._paletteKey)
               : AppTheme.textSecondary
        font.pixelSize: root.labelSize > 0
                        ? root.labelSize
                        : Math.max(10, Math.round(root.size
                              * (text.length > 1 ? 0.36 : 0.43)))
        font.weight: Font.ExtraBold
    }

    Item {
        id: fallbackMask
        anchors.fill: parent
        visible: false
        layer.enabled: (root.showingFallback || root._playing)
                       && root.motionMaskable
        Rectangle {
            anchors.fill: parent
            radius: root.circle ? width / 2 : root.radius
            color: "black"
        }
    }

    Image {
        id: img
        objectName: "avatarImage"
        anchors.fill: parent
        source: root.src === ""
                ? root.fallbackSource
                : root.src + (root.circle
                    ? "|shape:circle"
                    : "|shape:rsq:" + Math.max(1, Math.min(500,
                          Math.round(root.radius * 1000 / Math.max(1, root.size)))))
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        // Decode and bake at the drawn size: the 224 px bitmap is drawn at 24-48 px,
        // and letting the GPU minify it leaves the edge stair-stepped.
        sourceSize.width: Math.ceil(width > 0 ? width : root.size)
        sourceSize.height: Math.ceil(height > 0 ? height : root.size)
        layer.enabled: root.showingFallback && root.motionMaskable
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: fallbackMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        visible: img.status === Image.Ready && !root.motionShown
        onStatusChanged: {
            if (status === Image.Error)
                root.refresh()
        }
    }

    Loader {
        id: motionLoader
        objectName: "avatarMotion"
        anchors.fill: parent
        active: root._playing
        sourceComponent: AnimatedImage {
            id: motionImage
            objectName: "avatarAnimatedImage"
            source: root._motionSrc
            AnimationFileHold {}
            onSourceChanged: motionImage.playing = true
            onPlayingChanged: {
                if (!motionImage.playing
                    && motionImage.status === AnimatedImage.Ready)
                    motionStalledLater.restart()
            }
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: false
            playing: true
            paused: MotionPolicy.suspended
            smooth: true
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: fallbackMask
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }
            onStatusChanged: {
                // Deferred: handling it tears this very item down.
                if (status === AnimatedImage.Error)
                    motionErrorLater.restart()
            }
        }
    }
}
