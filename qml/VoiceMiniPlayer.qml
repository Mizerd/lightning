import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

Item {
    id: root

    objectName: "voiceMiniPlayerHost"

    readonly property var voice: app.voicePlayback
    readonly property bool docked: app.settings.voicePlayerDocked
    readonly property bool onAppScreen: app.currentScreen === 1
                                        || app.currentScreen === 2
    readonly property bool rowVisible: app.currentScreen === 1
                                       && root.voice.rowOnScreen
    readonly property bool wanted: root.voice.active && !root.docked
                                   && root.onAppScreen && !root.rowVisible

    property var avoidRects: []
    property var hintProvider: null

    readonly property int margin: AppTheme.spacing8
    readonly property int snapDistance: 24
    readonly property int railReserve: 168
    readonly property int minWidth: 200
    readonly property int minHeight: 52
    readonly property int maxWidth: 560
    readonly property int maxHeight: 320
    readonly property int defaultWidth: 300
    readonly property int defaultHeight: 92
    readonly property int keyStep: 24

    readonly property rect area: Qt.rect(
        root.margin, root.margin,
        Math.max(0, root.width - 2 * root.margin),
        Math.max(0, root.height - 2 * root.margin))

    readonly property rect saved: app.settings.voiceMiniPlayerGeometry
    readonly property bool savedValid: root.saved.width > 0
                                       && root.saved.height > 0

    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)) }
    function fitWidth(w) {
        return Math.min(root.area.width,
                        root.clamp(w, root.minWidth, root.maxWidth))
    }
    function fitHeight(h) {
        return Math.min(root.area.height,
                        root.clamp(h, root.minHeight, root.maxHeight))
    }
    readonly property real restWidth:
        root.fitWidth(root.savedValid ? root.saved.width : root.defaultWidth)
    readonly property real restHeight:
        root.fitHeight(root.savedValid ? root.saved.height
                                       : root.defaultHeight)

    function defaultPosition(w, h) {
        var hint = root.hintProvider ? root.hintProvider() : null
        if (!hint || hint.width <= 0 || hint.height <= 0)
            hint = root.area
        var x = hint.x + hint.width - w - root.margin
        if (hint.width - root.railReserve - w >= 2 * root.margin)
            x -= root.railReserve
        var y = hint.y + hint.height - h - root.margin
        return Qt.point(
            root.clamp(x, root.area.x, root.area.x + root.area.width - w),
            root.clamp(y, root.area.y, root.area.y + root.area.height - h))
    }
    readonly property point restPosition: {
        var w = root.restWidth
        var h = root.restHeight
        if (!root.savedValid)
            return root.defaultPosition(w, h)
        var freeX = Math.max(0, root.area.width - w)
        var freeY = Math.max(0, root.area.height - h)
        return Qt.point(root.area.x + root.clamp(root.saved.x, 0, 1) * freeX,
                        root.area.y + root.clamp(root.saved.y, 0, 1) * freeY)
    }

    function intersects(a, x, y, w, h) {
        return a && a.width > 0 && a.height > 0
            && x < a.x + a.width && a.x < x + w
            && y < a.y + a.height && a.y < y + h
    }
    function intersectsAny(list, x, y, w, h) {
        for (var i = 0; list && i < list.length; ++i) {
            if (root.intersects(list[i], x, y, w, h))
                return true
        }
        return false
    }
    function avoidedPosition(x, y, w, h, avoid) {
        if (!root.intersectsAny(avoid, x, y, w, h))
            return Qt.point(x, y)
        var gap = root.margin
        var best = null
        var bestDistance = 0
        for (var i = 0; avoid && i < avoid.length; ++i) {
            var r = avoid[i]
            if (!root.intersects(r, x, y, w, h))
                continue
            var candidates = [
                Qt.point(x, r.y - h - gap), Qt.point(x, r.y + r.height + gap),
                Qt.point(r.x - w - gap, y), Qt.point(r.x + r.width + gap, y)
            ]
            for (var j = 0; j < candidates.length; ++j) {
                var c = candidates[j]
                var cx = root.clamp(c.x, root.area.x,
                                    root.area.x + root.area.width - w)
                var cy = root.clamp(c.y, root.area.y,
                                    root.area.y + root.area.height - h)
                if (root.intersectsAny(avoid, cx, cy, w, h))
                    continue
                var d = Math.abs(cx - x) + Math.abs(cy - y)
                if (best === null || d < bestDistance) {
                    best = Qt.point(cx, cy)
                    bestDistance = d
                }
            }
        }
        return best !== null ? best : Qt.point(x, y)
    }
    readonly property point placedPosition:
        root.avoidedPosition(root.restPosition.x, root.restPosition.y,
                             root.restWidth, root.restHeight, root.avoidRects)

    // ── Interaction: a move or resize in progress drives the card from these
    // plain properties; its end commits them to the setting, and the bound
    // rest geometry takes over again (CLAUDE.md §16: never assign the bound
    // properties themselves).
    property bool interacting: false
    property real liveX: 0
    property real liveY: 0
    property real liveWidth: 0
    property real liveHeight: 0
    function beginInteraction() {
        root.liveX = card.x
        root.liveY = card.y
        root.liveWidth = card.width
        root.liveHeight = card.height
        root.interacting = true
    }
    function commit(x, y, w, h, snap) {
        w = root.fitWidth(w)
        h = root.fitHeight(h)
        x = root.clamp(x, root.area.x, root.area.x + root.area.width - w)
        y = root.clamp(y, root.area.y, root.area.y + root.area.height - h)
        if (snap) {
            var right = root.area.x + root.area.width - w
            var bottom = root.area.y + root.area.height - h
            if (x - root.area.x < root.snapDistance) x = root.area.x
            else if (right - x < root.snapDistance) x = right
            if (y - root.area.y < root.snapDistance) y = root.area.y
            else if (bottom - y < root.snapDistance) y = bottom
        }
        var freeX = root.area.width - w
        var freeY = root.area.height - h
        app.settings.voiceMiniPlayerGeometry = Qt.rect(
            freeX > 0 ? (x - root.area.x) / freeX : 0,
            freeY > 0 ? (y - root.area.y) / freeY : 0, w, h)
        root.interacting = false
    }
    function nudge(dx, dy, dw, dh) {
        root.commit(card.x + dx, card.y + dy, card.width + dw,
                    card.height + dh, false)
    }
    property var resizeStart: null
    function beginResize(edges, scenePoint) {
        root.beginInteraction()
        var p = root.mapFromItem(null, scenePoint.x, scenePoint.y)
        root.resizeStart = { edges: edges, px: p.x, py: p.y,
                             x: card.x, y: card.y,
                             w: card.width, h: card.height }
    }
    function updateResize(scenePoint) {
        var s = root.resizeStart
        if (!s)
            return
        var p = root.mapFromItem(null, scenePoint.x, scenePoint.y)
        var dx = p.x - s.px
        var dy = p.y - s.py
        var x = s.x, y = s.y, w = s.w, h = s.h
        var maxW = Math.min(root.maxWidth, root.area.width)
        var maxH = Math.min(root.maxHeight, root.area.height)
        var minW = Math.min(root.minWidth, maxW)
        var minH = Math.min(root.minHeight, maxH)
        if (s.edges.indexOf("r") >= 0)
            w = root.clamp(s.w + dx, minW,
                           Math.min(maxW, root.area.x + root.area.width - s.x))
        if (s.edges.indexOf("b") >= 0)
            h = root.clamp(s.h + dy, minH,
                           Math.min(maxH, root.area.y + root.area.height - s.y))
        if (s.edges.indexOf("l") >= 0) {
            w = root.clamp(s.w - dx, minW,
                           Math.min(maxW, s.x + s.w - root.area.x))
            x = s.x + s.w - w
        }
        if (s.edges.indexOf("t") >= 0) {
            h = root.clamp(s.h - dy, minH,
                           Math.min(maxH, s.y + s.h - root.area.y))
            y = s.y + s.h - h
        }
        root.liveX = x
        root.liveY = y
        root.liveWidth = w
        root.liveHeight = h
    }
    function endResize() {
        if (!root.resizeStart)
            return
        root.resizeStart = null
        root.commit(root.liveX, root.liveY, root.liveWidth, root.liveHeight,
                    false)
    }

    property var callPip: null
    function findCallPip() {
        var w = root.Window.window
        if (!w || !w.data)
            return null
        for (var i = 0; i < w.data.length; ++i) {
            var o = w.data[i]
            if (o && o.objectName === "callPipWindow")
                return o
        }
        return null
    }
    property var cornerPrompts: null
    function findCornerPrompts() {
        var overlay = root.Overlay.overlay
        if (!overlay)
            return null
        var kids = overlay.children
        for (var i = 0; i < kids.length; ++i) {
            if (kids[i] && kids[i].objectName === "cornerPromptHost")
                return kids[i]
        }
        return null
    }
    function refreshAvoid() {
        var rects = []
        var w = root.Window.window
        if (root.callPip && root.callPip.visible && w) {
            var p = root.mapFromItem(null, root.callPip.x - w.x,
                                     root.callPip.y - w.y)
            rects.push(Qt.rect(p.x, p.y, root.callPip.width,
                               root.callPip.height))
        }
        var cp = root.cornerPrompts
        if (cp && cp.visible && cp.height > 0 && cp.width > 0) {
            var q = root.mapFromItem(cp, 0, 0)
            rects.push(Qt.rect(q.x, q.y, cp.width, cp.height))
        }
        root.avoidRects = rects
    }
    Connections {
        target: root.callPip
        enabled: root.callPip !== null
        ignoreUnknownSignals: true
        function onVisibleChanged() { root.refreshAvoid() }
        function onXChanged() { root.refreshAvoid() }
        function onYChanged() { root.refreshAvoid() }
        function onWidthChanged() { root.refreshAvoid() }
        function onHeightChanged() { root.refreshAvoid() }
    }
    Connections {
        target: root.cornerPrompts
        enabled: root.cornerPrompts !== null
        ignoreUnknownSignals: true
        function onVisibleChanged() { root.refreshAvoid() }
        function onXChanged() { root.refreshAvoid() }
        function onYChanged() { root.refreshAvoid() }
        function onWidthChanged() { root.refreshAvoid() }
        function onHeightChanged() { root.refreshAvoid() }
    }
    Connections {
        target: root.Window.window
        enabled: root.Window.window !== null
        ignoreUnknownSignals: true
        function onXChanged() { root.refreshAvoid() }
        function onYChanged() { root.refreshAvoid() }
    }
    onWidthChanged: if (root.wanted) root.refreshAvoid()
    onHeightChanged: if (root.wanted) root.refreshAvoid()
    onWantedChanged: {
        if (root.wanted) {
            if (!root.callPip)
                root.callPip = root.findCallPip()
            if (!root.cornerPrompts)
                root.cornerPrompts = root.findCornerPrompts()
            root.refreshAvoid()
        } else if (root.interacting) {
            root.interacting = false
            root.resizeStart = null
        }
    }

    VoicePlayerLogic { id: logic }

    function focusCard() {
        card.forceActiveFocus(Qt.ShortcutFocusReason)
    }

    Rectangle {
        id: shadowShape
        x: card.x
        y: card.y
        width: card.width
        height: card.height
        radius: card.radius
        color: "black"
        visible: false
    }
    MultiEffect {
        source: shadowShape
        x: card.x
        y: card.y
        width: card.width
        height: card.height
        visible: card.visible
        opacity: card.opacity
        shadowEnabled: true
        shadowColor: AppTheme.dark ? "#73000000" : "#40000000"
        shadowBlur: 1.0
        shadowVerticalOffset: 6
        shadowHorizontalOffset: 0
    }

    Rectangle {
        id: card
        objectName: "voiceMiniPlayer"

        readonly property string tier: card.height < 76 ? "compact"
            : (card.height >= 140 && card.width >= 300 ? "large" : "regular")

        x: root.interacting ? root.liveX : root.placedPosition.x
        y: root.interacting ? root.liveY : root.placedPosition.y
        width: root.interacting ? root.liveWidth : root.restWidth
        height: root.interacting ? root.liveHeight : root.restHeight
        // No position animation: following a window resize late would put
        // the card outside the window for the length of the animation.

        radius: AppTheme.radiusLg
        color: logic.cardSurface
        border.width: card.activeFocus ? 2 : 1
        border.color: card.activeFocus ? AppTheme.focusRing : logic.cardBorder

        opacity: root.wanted ? 1 : 0
        visible: opacity > 0
        enabled: root.wanted
        Behavior on opacity {
            enabled: !AppTheme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }

        activeFocusOnTab: true
        Accessible.role: Accessible.Grouping
        Accessible.name: qsTr("Audio player")
        Accessible.description: logic.accessibleSummary + ". "
            + qsTr("Alt+arrow keys move it, Alt+Shift+arrow keys resize it.")
        Keys.onPressed: (event) => {
            var alt = event.modifiers & Qt.AltModifier
            var shift = event.modifiers & Qt.ShiftModifier
            if (event.key === Qt.Key_Space && !alt) {
                if (!event.isAutoRepeat)
                    root.voice.togglePlayPause()
                event.accepted = true
                return
            }
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    && !alt) {
                logic.openSource()
                event.accepted = true
                return
            }
            if (!alt)
                return
            var s = root.keyStep
            var dx = 0
            var dy = 0
            if (event.key === Qt.Key_Left) dx = -s
            else if (event.key === Qt.Key_Right) dx = s
            else if (event.key === Qt.Key_Up) dy = -s
            else if (event.key === Qt.Key_Down) dy = s
            else return
            if (shift)
                root.nudge(0, 0, dx, dy)
            else
                root.nudge(dx, dy, 0, 0)
            event.accepted = true
        }
        // Alt+Left and Alt+Right are also the window's history shortcuts, and
        // a shortcut is offered the key before any Keys handler. While the
        // card has the focus the four Alt+arrows are the card's: it says so
        // in its accessible description (C21-F05, C21-D14).
        Keys.onShortcutOverride: (event) => {
            if ((event.modifiers & Qt.AltModifier)
                    && (event.key === Qt.Key_Left
                        || event.key === Qt.Key_Right
                        || event.key === Qt.Key_Up
                        || event.key === Qt.Key_Down))
                event.accepted = true
        }

        MouseArea {
            id: moveArea
            objectName: "voiceMiniPlayerDrag"
            anchors.fill: parent
            z: -1
            preventStealing: true
            hoverEnabled: true
            cursorShape: moving ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property bool moving: false
            property bool pressedOnInfo: false
            readonly property bool active: moving
            property point pressAt: Qt.point(0, 0)
            property point startAt: Qt.point(0, 0)
            onPressed: (mouse) => {
                moving = false
                pressedOnInfo = info.contains(mapToItem(info, mouse.x, mouse.y))
                pressAt = mapToItem(root, mouse.x, mouse.y)
                startAt = Qt.point(card.x, card.y)
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return
                var p = mapToItem(root, mouse.x, mouse.y)
                var dx = p.x - pressAt.x
                var dy = p.y - pressAt.y
                if (!moving) {
                    if (Math.abs(dx) + Math.abs(dy) < 6)
                        return
                    moving = true
                    root.beginInteraction()
                }
                root.liveX = root.clamp(startAt.x + dx, root.area.x,
                                        root.area.x + root.area.width
                                        - card.width)
                root.liveY = root.clamp(startAt.y + dy, root.area.y,
                                        root.area.y + root.area.height
                                        - card.height)
            }
            onReleased: (mouse) => {
                if (moving) {
                    moving = false
                    root.commit(root.liveX, root.liveY, root.liveWidth,
                                root.liveHeight, true)
                } else if (pressedOnInfo
                           && info.contains(mapToItem(info, mouse.x, mouse.y))) {
                    logic.openSource()
                }
            }
            onCanceled: {
                if (moving) {
                    moving = false
                    root.commit(root.liveX, root.liveY, root.liveWidth,
                                root.liveHeight, true)
                }
            }
        }

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: AppTheme.spacing8
            anchors.leftMargin: AppTheme.spacing12
            anchors.bottomMargin: card.tier === "compact" ? 14
                                                          : AppTheme.spacing8
            spacing: card.tier === "large" ? AppTheme.spacing8 : 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                Item {
                    id: artSlot
                    readonly property int edge: card.tier === "large" ? 56
                        : (card.tier === "compact" ? 26 : 34)
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: edge
                    implicitHeight: edge
                    visible: !(card.tier === "compact" && card.width < 260)
                    Avatar {
                        objectName: "voiceMiniPlayerAvatar"
                        anchors.fill: parent
                        visible: logic.shownArt.length === 0
                        size: artSlot.edge
                        mxc: logic.shownAvatar
                        name: logic.shownSender
                        colorKey: logic.shownSenderId
                        onScreen: card.visible
                    }
                    Rectangle {
                        objectName: "voiceMiniPlayerArt"
                        anchors.fill: parent
                        visible: logic.shownArt.length > 0
                        radius: AppTheme.radiusSm
                        color: AppTheme.embedSurface
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: parent.visible ? logic.shownArt : ""
                            sourceSize.width: 128
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                }

                ColumnLayout {
                    id: info
                    objectName: "voiceMiniPlayerInfo"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 1
                    Label {
                        objectName: "voiceMiniPlayerSender"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: logic.shownTitle
                        color: logic.readable(AppTheme.textPrimary,
                                              logic.cardSurface)
                        font.pixelSize: AppTheme.scaled(
                            card.tier === "large" ? AppTheme.textBody
                                                  : AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
                        font.underline: infoHover.hovered
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "voiceMiniPlayerRoom"
                        visible: card.tier !== "compact"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: logic.statusText.length > 0 ? logic.statusText
                                                          : logic.shownSubtitle
                        color: logic.readable(
                            root.voice.fetchState === "failed"
                            ? AppTheme.danger : AppTheme.textSecondary,
                            logic.cardSurface)
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
                        elide: Text.ElideRight
                        wrapMode: card.tier === "large" ? Text.Wrap
                                                        : Text.NoWrap
                        maximumLineCount: card.tier === "large" ? 2 : 1
                    }
                    HoverHandler {
                        id: infoHover
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                Item {
                    Layout.fillWidth: !info.visible
                }

                IconButton {
                    objectName: "voiceMiniPlayerPlayPause"
                    visible: card.tier !== "large"
                    fill: true
                    implicitWidth: 30; implicitHeight: 30
                    iconSize: 17
                    focusPolicy: Qt.StrongFocus
                    iconName: root.voice.fetchState === "fetching"
                              ? "schedule"
                              : (root.voice.playing ? "pause" : "play_arrow")
                    enabled: root.voice.fetchState !== "fetching"
                    readonly property string actionLabel: root.voice.playing
                        ? qsTr("Pause playback") : qsTr("Resume playback")
                    Accessible.name: actionLabel
                    ToolTip.text: actionLabel
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.voice.togglePlayPause()
                }
                IconButton {
                    objectName: "voiceMiniPlayerNext"
                    visible: card.tier === "regular" && root.voice.hasNext
                    size: "sm"
                    focusPolicy: Qt.StrongFocus
                    iconName: "chevron_right"
                    readonly property string actionLabel:
                        qsTr("Next voice message")
                    Accessible.name: actionLabel
                    ToolTip.text: actionLabel
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.voice.playNext()
                }
                IconButton {
                    objectName: "voiceMiniPlayerDock"
                    size: "sm"
                    focusPolicy: Qt.StrongFocus
                    iconName: "close_fullscreen"
                    readonly property string actionLabel:
                        qsTr("Dock the player under the header")
                    Accessible.name: actionLabel
                    ToolTip.text: actionLabel
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: app.settings.voicePlayerDocked = true
                }
                IconButton {
                    objectName: "voiceMiniPlayerStop"
                    size: "sm"
                    focusPolicy: Qt.StrongFocus
                    iconName: "close"
                    readonly property string actionLabel: qsTr("Stop playback")
                    Accessible.name: actionLabel
                    ToolTip.text: actionLabel
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.voice.stop()
                }
            }

            VoiceSeekBar {
                objectName: "voiceMiniPlayerWave"
                visible: card.tier === "large"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 14
                Layout.maximumHeight: 40
                waveform: root.voice.isVoice ? root.voice.waveform : []
                trackHeight: 4
                knobAlways: !showsWaveform
            }

            RowLayout {
                Layout.fillWidth: true
                visible: card.tier !== "compact"
                spacing: AppTheme.spacing8
                Label {
                    objectName: "voiceMiniPlayerClock"
                    text: card.tier === "large"
                          ? logic.formatPosition(root.voice.position)
                          : logic.clock
                    color: logic.readable(AppTheme.textSecondary,
                                          logic.cardSurface)
                    font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
                    Accessible.ignored: true
                }
                VoiceSeekBar {
                    objectName: "voiceMiniPlayerSeek"
                    visible: card.tier === "regular"
                    Layout.fillWidth: true
                }
                Item {
                    visible: card.tier === "large"
                    Layout.fillWidth: true
                    implicitHeight: transport.implicitHeight
                    Row {
                        id: transport
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8
                        IconButton {
                            objectName: "voiceMiniPlayerRestart"
                            anchors.verticalCenter: parent.verticalCenter
                            size: "md"
                            focusPolicy: Qt.StrongFocus
                            iconName: "refresh"
                            readonly property string actionLabel:
                                qsTr("Play from the start")
                            Accessible.name: actionLabel
                            ToolTip.text: actionLabel
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            onClicked: root.voice.restart()
                        }
                        IconButton {
                            objectName: "voiceMiniPlayerLargePlayPause"
                            anchors.verticalCenter: parent.verticalCenter
                            fill: true
                            size: "xl"
                            focusPolicy: Qt.StrongFocus
                            iconName: root.voice.fetchState === "fetching"
                                      ? "schedule"
                                      : (root.voice.playing ? "pause"
                                                            : "play_arrow")
                            enabled: root.voice.fetchState !== "fetching"
                            readonly property string actionLabel:
                                root.voice.playing ? qsTr("Pause playback")
                                                   : qsTr("Resume playback")
                            Accessible.name: actionLabel
                            ToolTip.text: actionLabel
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            onClicked: root.voice.togglePlayPause()
                        }
                        IconButton {
                            objectName: "voiceMiniPlayerLargeNext"
                            anchors.verticalCenter: parent.verticalCenter
                            size: "md"
                            focusPolicy: Qt.StrongFocus
                            enabled: root.voice.hasNext
                            opacity: enabled ? 1 : 0.4
                            iconName: "chevron_right"
                            readonly property string actionLabel:
                                qsTr("Next voice message")
                            Accessible.name: actionLabel
                            ToolTip.text: actionLabel
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            onClicked: root.voice.playNext()
                        }
                    }
                }
                VoiceSpeedChip {
                    objectName: "voiceMiniPlayerSpeed"
                    surface: logic.cardSurface
                    visible: card.tier === "large" || card.width >= 280
                }
                MediaVolumeControl {
                    objectName: "voiceMiniPlayerVolume"
                    audio: root.voice.active ? root.voice.audioOutput : null
                    sliderObjectName: "voiceMiniPlayerVolumeSlider"
                    iconSize: 15
                    implicitWidth: 24; implicitHeight: 24
                    visible: card.tier === "large" || card.width >= 280
                }
                Label {
                    objectName: "voiceMiniPlayerTotal"
                    Accessible.ignored: true
                    visible: card.tier === "large"
                    text: logic.formatDuration(root.voice.duration)
                    color: logic.readable(AppTheme.textSecondary,
                                          logic.cardSurface)
                    font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
                }
            }
        }

        VoiceSeekBar {
            objectName: "voiceMiniPlayerCompactSeek"
            visible: card.tier === "compact"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: AppTheme.spacing12
            anchors.rightMargin: AppTheme.spacing12
            anchors.bottomMargin: 1
            height: 13
        }

        Repeater {
            model: [
                { e: "l", c: Qt.SizeHorCursor }, { e: "r", c: Qt.SizeHorCursor },
                { e: "t", c: Qt.SizeVerCursor }, { e: "b", c: Qt.SizeVerCursor },
                { e: "tl", c: Qt.SizeFDiagCursor },
                { e: "br", c: Qt.SizeFDiagCursor },
                { e: "tr", c: Qt.SizeBDiagCursor },
                { e: "bl", c: Qt.SizeBDiagCursor }
            ]
            delegate: MouseArea {
                required property var modelData
                readonly property string edges: modelData.e
                readonly property int band: 8
                readonly property bool corner: edges.length === 2
                objectName: "voiceMiniPlayerResize_" + edges
                z: corner ? 3 : 2
                cursorShape: modelData.c
                hoverEnabled: true
                preventStealing: true
                x: edges.indexOf("l") >= 0 ? -band / 2
                   : (edges.indexOf("r") >= 0 ? card.width - band / 2
                                              : (corner ? 0 : band))
                y: edges.indexOf("t") >= 0 ? -band / 2
                   : (edges.indexOf("b") >= 0 ? card.height - band / 2
                                              : (corner ? 0 : band))
                width: (edges === "t" || edges === "b")
                       ? card.width - 2 * band : band
                height: (edges === "l" || edges === "r")
                        ? card.height - 2 * band : band
                onPressed: (mouse) => {
                    root.beginResize(edges, mapToItem(null, mouse.x, mouse.y))
                }
                onPositionChanged: (mouse) => {
                    if (pressed)
                        root.updateResize(mapToItem(null, mouse.x, mouse.y))
                }
                onReleased: root.endResize()
                onCanceled: root.endResize()
            }
        }
    }
}
