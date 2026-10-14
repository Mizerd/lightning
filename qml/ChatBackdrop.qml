import QtQuick
import QtQuick.Effects
import MatrixClient

// The picture behind a room's timeline (app.backdrops), treated so the ink
// drawn straight on it stays readable:
//
//   dominant-colour placeholder
//   -> the picture: a still Image (its first frame) always loads first. An
//      animated GIF/WebP plays over it ONLY when the GIF autoplay setting is
//      "Always", reduced motion is off, the blur is off and the window is
//      shown (see "Animation" below); "On hover" has no pointer target on a
//      whole-pane picture, so it stays still, like "Never".
//   -> optional blur (MultiEffect; off on a software renderer)
//   -> the scrim: the theme ground moved away from the ink, at an opacity
//      that never drops below backdrop::planScrim's floor (textPrimary,
//      textSecondary and textMuted at 4.5:1 over the picture's own measured
//      pixels; the worst case until it is measured). `dim` only adds to it.
//   -> soft fades into the ground at the top and bottom, so the header and
//      the typing strip meet it without a seam.
//
// Pictures arrive only through the media bridge (shared, by mxc) or the
// staged-image store (personal and previews); no URL is ever composed here.
// Bubbles and cards keep their own opaque surfaces on top of this layer.
Item {
    id: root
    objectName: "chatBackdrop"

    // The room to resolve. Ignored while `spec` is set (editor previews).
    property string roomId: ""
    // An explicit backdrop (the shape backdropFor() returns), for previews.
    property var spec: null
    // Where this layer sits within the pane's `background` surface, 0..1, so
    // the fades meet the colour actually drawn there (Depth gradients).
    property real edgeTop: 0
    property real edgeBottom: 1
    property bool edgeFades: true
    // Fade height, in px.
    property real fadeHeight: 28

    readonly property var resolved: {
        if (root.spec)
            return root.spec
        if (typeof app === "undefined" || !app || !app.backdrops
                || root.roomId === "")
            return null
        // `revision` is the controller's change counter; backdropFor() is a
        // method call and would not re-evaluate on its own.
        var _dep = app.backdrops.revision
        return app.backdrops.backdropFor(root.roomId)
    }
    readonly property string kind: root.resolved ? (root.resolved.kind || "none")
                                                 : "none"
    readonly property bool active: kind === "mxc" || kind === "staged"
    readonly property string mxc: kind === "mxc" ? (root.resolved.mxc || "") : ""

    visible: active

    function _request() {
        if (!root.spec && root.roomId !== "" && typeof app !== "undefined"
                && app && app.backdrops)
            app.backdrops.requestRoom(root.roomId)
    }
    onRoomIdChanged: _request()
    Component.onCompleted: _request()

    // A counter the source binding reads, never an assignment to `source`
    // (which would destroy the binding).
    property int resolveTick: 0
    readonly property string imageSource: {
        var _tick = root.resolveTick
        if (!root.active)
            return ""
        if (root.kind === "staged")
            return root.resolved.imageUrl || ""
        if (typeof app === "undefined" || !app || !app.mediaBridge
                || !app.mediaBridge.supported || root.mxc.length === 0)
            return ""
        return app.mediaBridge.wideImageSource(root.mxc)
    }
    Connections {
        target: (typeof app !== "undefined" && app) ? app.mediaBridge : null
        enabled: root.mxc.length > 0 && root.imageSource.length === 0
        function onMediaCached(key) {
            if (key.endsWith(":" + root.mxc))
                root.resolveTick++
        }
        // An expired transient failure mark: ask again, as Avatar.qml does.
        function onMediaRetryable(key) {
            if (key.endsWith(":" + root.mxc))
                root.resolveTick++
        }
    }

    // ── Animation ──
    // The still above is the source of truth. An animation (a GIF or WebP with
    // two or more frames, within the banner canvas and byte bounds) is layered
    // over it once the still is ready; nothing extra is downloaded for a shared
    // picture (the bridge reuses the bytes the still already fetched) and a
    // personal one is the user's own staged bytes. The scrim already covers the
    // worst of several sampled frames (ChatBackdropController measures them).
    readonly property int _motionMode: (typeof app !== "undefined" && app
                                        && app.settings)
                                       ? app.settings.gifAutoplay : 2
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
    // A staged picture says outright whether it is an animation; a shared one
    // is asked of the bridge, which answers "" for a still.
    readonly property bool _mayAnimate: root.active && root.resolved
        && (root.kind === "mxc" || root.resolved.animated === true)
    readonly property bool _wantsMotion: _mayAnimate && _motionMode === 0
        && !AppTheme.reducedMotion && !root.blurOn && visible && _windowShown
        && width > 0 && height > 0 && picture.status === Image.Ready
    readonly property bool motionShown: motionLoader.item !== null
        && motionLoader.item.status === AnimatedImage.Ready
    property string _motionSrc: ""
    property bool _motionFailed: false
    property bool _motionRetried: false

    // Asked on every activation, never cached: the bridge's file can be
    // evicted meanwhile.
    function _askMotion() {
        if (!_wantsMotion || _motionFailed || typeof app === "undefined"
                || !app || !app.mediaBridge)
            return
        _motionSrc = root.kind === "staged"
            ? app.mediaBridge.stagedAnimationSource(root.resolved.imageUrl || "")
            : app.mediaBridge.backdropAnimationSource(root.mxc)
    }
    function _motionError() {
        if (!_motionRetried) {
            _motionRetried = true
            _motionSrc = ""
            _askMotion()
            return
        }
        _motionFailed = true
    }
    // Deferred: this runs inside _wantsMotion's own evaluation.
    on_WantsMotionChanged: Qt.callLater(root._askMotion)
    onImageSourceChanged: {
        _motionSrc = ""
        _motionFailed = false
        _motionRetried = false
        Qt.callLater(root._askMotion)
    }

    readonly property var scrim: {
        if (!root.active || typeof app === "undefined" || !app || !app.backdrops)
            return null
        var _dep = app.backdrops.revision
        // Re-planned once the picture is in: the controller measures the
        // cached bytes then.
        var _ready = picture.status
        return app.backdrops.scrimFor(root.resolved.statsKey || "",
                                      AppTheme.timelineBackground, AppTheme.textPrimary,
                                      AppTheme.textSecondary, AppTheme.textMuted,
                                      root.resolved.tint !== undefined
                                          ? root.resolved.tint : 0.25,
                                      root.resolved.dim !== undefined
                                          ? root.resolved.dim : 0.2)
    }
    readonly property real scrimAlpha: root.scrim ? root.scrim.alpha : 1
    readonly property bool blurOn: root.active && root.resolved.blur > 0.01
                                   && !(typeof app !== "undefined" && app
                                        && app.softwareRenderer)

    // Everything that shows the picture, faded in as one.
    Item {
        id: pictureLayer
        anchors.fill: parent
        clip: true
        opacity: picture.status === Image.Ready ? 1 : 0
        Behavior on opacity {
            enabled: !AppTheme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        // Under a picture with transparency, and in a "contain" fit's
        // margins: the picture's dominant colour (measured locally, or the
        // advisory colour from the event while nothing is measured).
        Rectangle {
            anchors.fill: parent
            color: root.resolved && root.resolved.color
                   ? root.resolved.color : AppTheme.timelineBackground
        }

        Image {
            id: picture
            objectName: "chatBackdropImage"
            anchors.fill: parent
            source: root.imageSource
            visible: !root.blurOn
            // Hidden under a playing animation, whose transparent pixels
            // would otherwise show the still frame through.
            opacity: root.motionShown ? 0 : 1
            asynchronous: true
            // Bounded decode: a backdrop is dimmed and often blurred, and a
            // 4K picture as a texture is memory nobody sees.
            sourceSize: Qt.size(2048, 2048)
            smooth: true
            autoTransform: true
            fillMode: {
                var fit = root.resolved ? root.resolved.fit : "cover"
                if (fit === "tile")
                    return Image.Tile
                if (fit === "contain")
                    return Image.PreserveAspectFit
                return Image.PreserveAspectCrop
            }
            horizontalAlignment: Image.AlignHCenter
            verticalAlignment: {
                var align = root.resolved ? root.resolved.align : "center"
                if (align === "top")
                    return Image.AlignTop
                if (align === "bottom")
                    return Image.AlignBottom
                return Image.AlignVCenter
            }
        }

        // Torn down whenever it may not play, so a paused backdrop holds no
        // decoder.
        Loader {
            id: motionLoader
            objectName: "chatBackdropMotion"
            anchors.fill: parent
            active: root._wantsMotion && root._motionSrc !== ""
                    && !root._motionFailed
            sourceComponent: AnimatedImage {
                id: motionImage
                objectName: "chatBackdropAnimatedImage"
                source: root._motionSrc
                // Keeps the bridge from evicting the file mid-play.
                AnimationFileHold {}
                // A decoder that stopped on its own leaves `playing` false,
                // and a new source would then load paused on its first frame.
                onSourceChanged: motionImage.playing = true
                onPlayingChanged: {
                    if (!motionImage.playing
                        && motionImage.status === AnimatedImage.Ready)
                        Qt.callLater(root._askMotion)
                }
                fillMode: picture.fillMode
                horizontalAlignment: picture.horizontalAlignment
                verticalAlignment: picture.verticalAlignment
                asynchronous: true
                cache: false
                playing: true
                smooth: true
                autoTransform: true
                onStatusChanged: {
                    // Deferred: handling it tears this very item down.
                    if (status === AnimatedImage.Error)
                        Qt.callLater(root._motionError)
                }
            }
        }

        MultiEffect {
            anchors.fill: picture
            source: picture
            visible: root.blurOn
            blurEnabled: root.blurOn
            blur: root.blurOn ? Math.min(1, root.resolved.blur) : 0
            blurMax: 48
            // The blurred edge would otherwise pull in transparent pixels and
            // darken the border; the layer is clipped instead.
            autoPaddingEnabled: false
        }
    }

    // The scrim. Drawn even before the picture arrives, at the worst-case
    // floor, so nothing is ever briefly unreadable.
    Rectangle {
        objectName: "chatBackdropScrim"
        anchors.fill: parent
        color: root.scrim ? root.scrim.color : AppTheme.timelineBackground
        opacity: root.scrimAlpha
    }

    // Soft edges into the ground actually drawn above and below.
    Rectangle {
        visible: root.edgeFades
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.min(root.fadeHeight, root.height / 4)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: AppTheme.surfaceColorAt("timelineBackground", root.edgeTop)
            }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }
    Rectangle {
        visible: root.edgeFades
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.min(root.fadeHeight, root.height / 4)
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop {
                position: 1.0
                color: AppTheme.surfaceColorAt("timelineBackground", root.edgeBottom)
            }
        }
    }
}
