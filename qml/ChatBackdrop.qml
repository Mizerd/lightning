import QtQuick
import QtQuick.Effects
import MatrixClient

// The picture behind a room's timeline (app.backdrops), treated so the ink
// drawn straight on it stays readable:
//
//   dominant-colour placeholder
//   -> the picture, FIRST FRAME only (a still Image, never AnimatedImage)
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

    readonly property var scrim: {
        if (!root.active || typeof app === "undefined" || !app || !app.backdrops)
            return null
        var _dep = app.backdrops.revision
        // Re-planned once the picture is in: the controller measures the
        // cached bytes then.
        var _ready = picture.status
        return app.backdrops.scrimFor(root.resolved.statsKey || "",
                                      AppTheme.background, AppTheme.textPrimary,
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
                   ? root.resolved.color : AppTheme.background
        }

        Image {
            id: picture
            objectName: "chatBackdropImage"
            anchors.fill: parent
            source: root.imageSource
            visible: !root.blurOn
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
        color: root.scrim ? root.scrim.color : AppTheme.background
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
                color: AppTheme.surfaceColorAt("background", root.edgeTop)
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
                color: AppTheme.surfaceColorAt("background", root.edgeBottom)
            }
        }
    }
}
