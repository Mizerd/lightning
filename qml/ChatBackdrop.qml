import QtQuick
import QtQuick.Effects
import MatrixClient

Item {
    id: root
    objectName: "chatBackdrop"

    property string roomId: ""
    property var spec: null
    property real edgeTop: 0
    property real edgeBottom: 1
    property bool edgeFades: true
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
        function onMediaRetryable(key) {
            if (key.endsWith(":" + root.mxc))
                root.resolveTick++
        }
    }

    readonly property int _motionMode: MotionPolicy.mode
    readonly property bool _windowShown:
        root.Window.visibility !== Window.Minimized
        && root.Window.visibility !== Window.Hidden
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

    Item {
        id: pictureLayer
        anchors.fill: parent
        clip: true
        opacity: picture.status === Image.Ready ? 1 : 0
        Behavior on opacity {
            enabled: !AppTheme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

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
            opacity: root.motionShown ? 0 : 1
            asynchronous: true
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
                AnimationFileHold {}
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
                paused: MotionPolicy.suspended
                smooth: true
                autoTransform: true
                onStatusChanged: {
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
            autoPaddingEnabled: false
        }
    }

    Rectangle {
        objectName: "chatBackdropScrim"
        anchors.fill: parent
        color: root.scrim ? root.scrim.color : AppTheme.timelineBackground
        opacity: root.scrimAlpha
    }

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
