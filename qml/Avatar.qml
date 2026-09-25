import QtQuick
import QtQuick.Effects
import QtQuick.Controls
import MatrixClient

// Shared avatar element for people, rooms and Spaces (a Space is a room).
//
// Resolves the mxc URI through the shared MediaBridge (deduplicated, bounded,
// account-separated, cleared on sign-out) and shows initials until a real
// bitmap has fully loaded, so no broken-image icon or stale avatar ever
// flashes. The shape (circle or rounded square) is baked into the decoded
// bitmap by MediaImageProvider via the "|shape:" suffix, once per cached
// image, rather than a per-item MultiEffect mask (two extra render passes
// per avatar per frame).
//
// An animated avatar (GIF or animated WebP) plays over that still picture;
// see the "Animated avatars" block below.
Rectangle {
    id: root

    // mxc:// avatar URI (empty → initials only).
    property string mxc: ""
    // Name used for the initial placeholder.
    property string name: ""
    property int size: 40
    property bool circle: true
    // Rounded-square corner radius for room/Space avatars.
    property int squareRadius: AppTheme.radiusMd
    // Unused: rooms fall back to their initial like people (_initials() strips
    // a leading #/!/@/+ sigil). Kept for compatibility.
    property bool roomGlyph: false
    // Stable identity key for the fallback colour (roomId/userId); falls back
    // to the display name.
    property string colorKey: ""
    // Explicit initials font size; 0 derives from the avatar size.
    property int labelSize: 0
    // No identity rim around avatars; identity colour lives in the initials
    // fallback and the sender name.
    //
    // Rows outside the viewport set this false so their loading skeletons stop
    // animating (off-screen infinite animations keep the scene graph dirty).
    property bool onScreen: true

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    radius: circle ? size / 2 : squareRadius

    // Presentation states:
    //   missing — nothing to load: deterministic initials fallback.
    //   loading — fetch/decode in flight: shape-matched skeleton.
    //   ready   — only the decoded bitmap; no fallback fill beneath it, since
    //             its transparent pixels must show the surface behind.
    //   failed  — fetch/decode failed: initials (a later cache completion
    //             still promotes to ready).
    property bool fetchFailed: false
    readonly property string presentationState:
        !hasImage ? "missing"
        : img.status === Image.Ready ? "ready"
        : (fetchFailed || img.status === Image.Error) ? "failed"
        : "loading"

    // Per-identity colour; neutral surface when there is nothing to derive
    // from.
    readonly property string _paletteKey: colorKey.length > 0 ? colorKey : name
    // Shared with the sender-name ink, so both agree for a key.
    function _paletteColor(key) {
        return AppTheme.avatarColor(key)
    }
    color: presentationState === "missing" || presentationState === "failed"
           ? (_paletteKey.length > 0 ? _paletteColor(_paletteKey)
                                     : AppTheme.cardElevated)
           : "transparent"

    // Up to two initials: first letters of the first two words, with
    // Matrix sigils stripped ("@user:hs" → "U").
    function _initials(value) {
        var cleaned = value.replace(/^[@#!+]/, "").trim()
        if (cleaned.length === 0)
            return "?"
        var words = cleaned.split(/\s+/).filter(function (w) { return w.length > 0 })
        if (words.length >= 2)
            return (words[0].charAt(0) + words[1].charAt(0)).toUpperCase()
        return cleaned.charAt(0).toUpperCase()
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
    // Idempotent; app.mediaBridge is constant, so replacing the binding loses
    // nothing.
    function resolveBridge() {
        if (!bridge && typeof app !== "undefined" && app && app.mediaBridge)
            bridge = app.mediaBridge
    }
    readonly property bool hasImage:
        mxc.length > 0 && bridge !== null && bridge !== undefined
        && bridge.supported
    property string src: ""

    /// A last-known local picture, opt-in (only the account switcher uses it).
    /// The bridge fetches through the active client, so an inactive account's
    /// avatar can't be fetched at all. Drawn only while `src` is empty, so the
    /// real picture wins once available. Not a general fallback: other avatars
    /// keep honest initials.
    ///
    /// It bypasses the provider, so it isn't mask-baked; the mask below is
    /// enabled only while `showingFallback`.
    property string fallbackSource: ""
    readonly property bool showingFallback:
        root.src === "" && root.fallbackSource !== ""

    // ── Animated avatars ──
    // The still thumbnail always loads first; an animation is layered over it
    // only while it may play, and the layer is torn down (active: false) the
    // moment it may not. Policy follows the GIF autoplay setting, which
    // already governs all passive media: Always plays what is on screen, On
    // hover plays while the pointer rests on the avatar, Never and reduced
    // motion keep every avatar still and fetch nothing extra. Server
    // thumbnails never animate, so finding out needs the original; see
    // MediaBridge::avatarAnimationSource.
    //
    // Call sites may opt out (e.g. tiny facepiles).
    property bool animate: true
    // Profile card, Space Home: probe any format, not only non-JPEG
    // thumbnails.
    property bool prominent: false
    readonly property int _motionMode:
        (typeof app !== "undefined" && app && app.settings)
        ? app.settings.gifAutoplay : 2
    // The shape mask is a shader effect, which the software scene graph
    // cannot draw: there the animation would cover the still picture with
    // nothing. Writable only so tests can drive playback on that backend.
    property bool motionMaskable:
        root.GraphicsInfo.api !== GraphicsInfo.Software
    readonly property bool _motionAllowed:
        animate && hasImage && _motionMode !== 2 && !AppTheme.reducedMotion
        && size >= 20 && motionMaskable
    // Set after a short dwell, so a pointer crossing a list starts nothing.
    property bool _hovered: false
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
    readonly property bool _wantsMotion:
        _motionAllowed && onScreen && visible && _windowShown && _inViewport
        && presentationState === "ready"
        && (_motionMode === 0 || _hovered)
    property string _motionSrc: ""
    // The decoder refused the file (e.g. no WebP plugin): stay still.
    property bool _motionFailed: false
    // One reload is allowed after an error: the scratch file may have been
    // evicted, and asking the bridge again fetches it back.
    property bool _motionRetried: false

    // Many sites cannot bind `onScreen` (Repeaters inside a Flickable on Home,
    // the Space lobby, room info), so it stays true there. The avatar checks
    // the nearest Flickable's viewport itself: on asking, and again once
    // scrolling pauses, and only while it is on screen by every other measure.
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
        // Hover mode needs no viewport: a hovered avatar is on screen.
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
        // Asked on every activation, never cached here: the bridge's file can
        // be evicted meanwhile, and a stale URL would only fail to load. Hover
        // or a profile card asks for any format; a passive ask only where the
        // thumbnail allows (see avatarAnimationSource).
        _motionSrc = bridge.avatarAnimationSource(mxc, prominent || _hovered)
        _syncSlot()
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
    // A slot is held exactly while the animation should play.
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
    on_WantsMotionChanged: {
        if (_wantsMotion)
            _requestMotion()
        else
            _syncSlot()
    }
    // A destroyed avatar's slot is freed by the bridge (destroyed()).

    // Hover is intent in both modes: On hover plays under the pointer, and
    // under Always it probes an avatar the passive rule skipped (a JPEG
    // thumbnail may still be an animated WebP).
    on_HoveredChanged: {
        if (_hovered) {
            _requestMotion()   // also retries a refused slot, with intent
        } else if (_slotHeld && !prominent && bridge) {
            // The intent is over: keep playing only as a passive holder, so a
            // hover never keeps a reserve slot for good.
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
        // Recovery point in case both earlier bridge lookups missed.
        resolveBridge()
        if (!hasImage) {
            src = ""
            return
        }
        // One canonical server-side size per avatar (one request, cache entry
        // and failure mark); the Image scales down.
        var s = bridge.avatarSource(mxc, size)
        src = s
        // "" is either in flight or suppressed by a failure mark. The mark has
        // no later signal, so check it now and show initials rather than an
        // endless skeleton; mediaRetryable promotes it back later.
        if (s === "" && bridge.avatarFailureCategory(mxc) !== "")
            fetchFailed = true
    }
    // The single trigger for a new bridge request. Size doesn't affect the
    // request (avatarSource ignores it), and hasImage derives from mxc plus
    // `bridge.supported`, so this key covers both, including a late
    // `supported` flip after session restore or an account switch.
    readonly property string _requestKey: hasImage ? mxc : ""
    on_RequestKeyChanged: {
        // A new identity is a new attempt; the old failure must not leak
        // across delegate reuse.
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
        // Completion half of the defensive bridge resolution; refresh() calls
        // resolveBridge() itself.
        refresh()
    }

    Connections {
        target: root.bridge
        enabled: root.hasImage
        function onMediaCached(cacheKey) {
            // Cache keys end with the mxc ("mxc:<edge>:<uri>"). Skip once
            // resolved: same-sender rows share an mxc, and refreshing them all
            // on every completion churns. Failed avatars still retry.
            if (cacheKey.endsWith(":" + root.mxc)
                && !cacheKey.startsWith("motion:")
                && (root.src === "" || root.fetchFailed))
                root.refresh()
        }
        // "motion:" keys are the animation probe, never the still picture: a
        // failed probe must not turn a loaded avatar into initials.
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey.endsWith(":" + root.mxc)
                && !cacheKey.startsWith("motion:"))
                root.fetchFailed = true
        }
        function onMediaRetryable(cacheKey) {
            // An expired transient failure: re-resolve. Bounded, since the
            // bridge re-arms the mark on another failure.
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
                root._syncSlot()
        }
    }

    // Loading skeleton, washed with the identity colour so a late avatar
    // settles into its identity instead of flashing grey.
    Skeleton {
        objectName: "avatarSkeleton"
        anchors.fill: parent
        visible: root.presentationState === "loading"
        active: root.onScreen
        circle: root.circle
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
        // The ink the disc can carry: discs follow the theme accent and half
        // are pale, so white isn't always readable.
        color: root._paletteKey.length > 0
               ? AppTheme.avatarInk(root._paletteKey)
               : AppTheme.textSecondary
        font.pixelSize: root.labelSize > 0
                        ? root.labelSize
                        : Math.max(10, Math.round(root.size
                              * (text.length > 1 ? 0.36 : 0.43)))
        font.weight: Font.ExtraBold
    }

    // Mask for the fallback path, in the avatar's shape. `visible: false` with
    // `layer.enabled` makes it a texture rather than drawn content.
    Item {
        id: fallbackMask
        anchors.fill: parent
        visible: false
        layer.enabled: root.showingFallback || root._playing
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
        // The provider bakes the mask into the bitmap; the suffix selects
        // circle or rounded-square (radius as a permille of the edge).
        source: root.src === ""
                ? root.fallbackSource
                : root.src + (root.circle
                    ? "|shape:circle"
                    : "|shape:rsq:" + Math.max(1, Math.min(500,
                          Math.round(root.radius * 1000 / Math.max(1, root.size)))))
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        // Only while a fallback is drawn; `layer.effect` is created lazily,
        // so provider bitmaps pay nothing.
        layer.enabled: root.showingFallback
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: fallbackMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        // Only shown once fully decoded: no broken-image glyph, no flash.
        // Hidden under a playing animation, whose transparent pixels would
        // otherwise show this first frame through.
        visible: img.status === Image.Ready && !root.motionShown
        // Self-heal a cache hit evicted before the provider read it (Error):
        // refresh() re-dispatches, and re-cached bytes get a new revision
        // URL. A cache hit returns the identical string, so no loop.
        onStatusChanged: {
            if (status === Image.Error)
                root.refresh()
        }
    }

    // The animation, masked to the avatar's shape. Only instantiated while it
    // plays, so a paused or scrolled-away avatar holds no decoder.
    Loader {
        id: motionLoader
        objectName: "avatarMotion"
        anchors.fill: parent
        active: root._playing
        sourceComponent: AnimatedImage {
            objectName: "avatarAnimatedImage"
            source: root._motionSrc
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            // One frame at a time; a cached animation holds every frame.
            cache: false
            playing: true
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
                    Qt.callLater(root._motionError)
            }
        }
    }
}
