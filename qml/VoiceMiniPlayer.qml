import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

// The floating mini-player for a voice/audio clip whose room is NOT on screen,
// in the manner of Discord's pop-out. The app-owned player
// (app.voicePlayback) keeps a clip playing through a room switch; this card is
// how the reader still sees and controls it from another room, Space Home or
// Home. Inside the clip's own room there is no card: the message row's player
// shows the live state.
//
// It floats over the chat area of this window (not a separate window, unlike
// the call PiP), inside `safeArea`, which the host sets to exclude the room
// header, the call bar and the composer, so it never covers them. It snaps to
// the nearest corner of that area when dragged and the corner is remembered
// (app.settings.voiceMiniPlayerCorner). When the call PiP window, or the
// window's corner prompts (an incoming call, verification, update, indexing;
// Main.qml's cornerPromptHost), sit over the remembered corner, the card takes
// the nearest free one instead.
//
// This item fills its host and takes no input itself; only the card does.
Item {
    id: root

    objectName: "voiceMiniPlayerHost"

    // Where the card may sit, in this item's coordinates.
    property rect safeArea: Qt.rect(0, 0, width, height)
    // Rectangles (this item's coordinates) the card must not cover. Kept in
    // step with the call PiP window and the corner prompts by refreshAvoid();
    // assigned, never bound, so tests may set it too.
    property var avoidRects: []

    readonly property var voice: app.voicePlayback
    // A clip played from a thread panel lives in that thread's timeline, not
    // the room's: its row is on screen only while that thread panel is open.
    // A room switch closes the panel, and so can the reader.
    readonly property bool fromThread:
        root.voice.threadRootId.length > 0
        && root.voice.threadRootId !== root.voice.eventId
    // The clip's row is on screen, so the row shows the live state and the
    // card is not needed.
    readonly property bool roomOnScreen:
        app.currentScreen === 1 && root.voice.roomId === app.currentRoomId
        && (!root.fromThread
            || (app.thread.active
                && app.thread.rootEventId === root.voice.threadRootId))
    readonly property bool wanted: root.voice.active && !root.roomOnScreen

    readonly property int margin: AppTheme.spacing12
    readonly property int savedCorner: app.settings.voiceMiniPlayerCorner
    // 0 bottom-right, 1 bottom-left, 2 top-right, 3 top-left.
    readonly property int corner: root.effectiveCorner(root.savedCorner,
                                                       root.avoidRects,
                                                       root.safeArea,
                                                       card.width, card.height)

    // What the card shows, copied while a clip is loaded, so the fade-out
    // after a stop does not draw an empty card.
    property string shownSender: ""
    property string shownSenderId: ""
    property string shownAvatar: ""
    property string shownRoom: ""
    property bool shownVoice: true
    function captureShown() {
        if (!root.voice.active)
            return
        root.shownSender = root.voice.senderName
        root.shownSenderId = root.voice.senderId
        root.shownAvatar = root.voice.senderAvatarMxc
        root.shownRoom = root.voice.roomName
        root.shownVoice = root.voice.isVoice
    }
    Connections {
        target: root.voice
        function onCurrentChanged() { root.captureShown() }
    }

    function cornerX(c, area, w) {
        var right = c === 0 || c === 2
        return right ? area.x + area.width - w - root.margin
                     : area.x + root.margin
    }
    function cornerY(c, area, h) {
        var bottom = c === 0 || c === 1
        return bottom ? area.y + area.height - h - root.margin
                      : area.y + root.margin
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
    // The remembered corner, or, if the PiP covers it, the nearest free one:
    // the other corner on the same side first, then across, then diagonal.
    function effectiveCorner(saved, avoid, area, w, h) {
        var flipV = [2, 3, 0, 1]
        var flipH = [1, 0, 3, 2]
        var order = [saved, flipV[saved], flipH[saved], flipH[flipV[saved]]]
        for (var i = 0; i < order.length; ++i) {
            var c = order[i]
            if (!root.intersectsAny(avoid, root.cornerX(c, area, w),
                                    root.cornerY(c, area, h), w, h))
                return c
        }
        return saved
    }
    // Remember the corner nearest a released card's centre.
    function snapFrom(cx, cy) {
        app.settings.voiceMiniPlayerCorner =
            root.nearestCorner(cx, cy, root.safeArea)
    }
    function nearestCorner(cx, cy, area) {
        var right = cx > area.x + area.width / 2
        var bottom = cy > area.y + area.height / 2
        return bottom ? (right ? 0 : 1) : (right ? 2 : 3)
    }

    // Back to the message: its room, then the event (or its thread). As in
    // notification routing (Main.qml), the jump waits one turn for the room
    // switch to settle.
    function openSource() {
        var roomId = root.voice.roomId
        var eventId = root.voice.eventId
        var threadRootId = root.voice.threadRootId
        if (roomId.length === 0)
            return
        app.openRoom(roomId)
        if (threadRootId.length > 0 && threadRootId !== eventId) {
            Qt.callLater(function() {
                app.thread.openThread(roomId, threadRootId)
            })
        } else if (eventId.length > 0) {
            Qt.callLater(function() { app.pagination.jumpToEvent(eventId) })
        }
    }

    // Position floors, total rounds (CLAUDE.md §16): at 25.7 s you have not
    // reached 0:26, but a 25.7 s clip is 26 s long. As in AudioPlayerCard.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    // ── The call PiP, a separate top-level window ──
    // Found by objectName on this window (Main.qml declares it), so this card
    // needs no wiring through the screen loader.
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
    // The window's corner-prompt column (Main.qml), parented to the overlay.
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
            // Both windows' positions are screen coordinates of their client
            // areas; the difference is a scene position in this window.
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
        }
    }
    Component.onCompleted: root.captureShown()

    Rectangle {
        id: card
        objectName: "voiceMiniPlayer"

        width: Math.max(220, Math.min(300, root.safeArea.width
                                           - root.margin * 2))
        height: body.implicitHeight + AppTheme.spacing8 + 14
        radius: AppTheme.radiusLg
        color: AppTheme.cardElevated
        border.width: card.activeFocus ? 2 : 1
        border.color: card.activeFocus ? AppTheme.focusRing : AppTheme.borderSubtle

        readonly property bool bottomCorner: root.corner === 0
                                             || root.corner === 1
        readonly property real restX: root.cornerX(root.corner, root.safeArea,
                                                   card.width)
        readonly property real restY: root.cornerY(root.corner, root.safeArea,
                                                   card.height)
        // While dragged the card follows the pointer from its corner;
        // otherwise it rests in the corner. Bound, never assigned, so the
        // corner keeps driving it (CLAUDE.md §16).
        x: drag.active
           ? Math.max(root.safeArea.x, Math.min(
                 root.safeArea.x + root.safeArea.width - card.width,
                 card.restX + drag.activeTranslation.x))
           : card.restX
        y: drag.active
           ? Math.max(root.safeArea.y, Math.min(
                 root.safeArea.y + root.safeArea.height - card.height,
                 card.restY + drag.activeTranslation.y))
           : card.restY
        Behavior on x {
            enabled: !drag.active && !AppTheme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
        Behavior on y {
            enabled: !drag.active && !AppTheme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        // A short fade and slide in and out, towards the edge it rests on.
        opacity: root.wanted ? 1 : 0
        visible: opacity > 0
        enabled: root.wanted
        property real slide: root.wanted ? 0 : 14
        transform: Translate { y: card.bottomCorner ? card.slide : -card.slide }
        Behavior on opacity {
            enabled: !AppTheme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }
        Behavior on slide {
            enabled: !AppTheme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }

        // The body is the way back to the message, by pointer or keyboard.
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: root.shownRoom.length > 0
            ? qsTr("Now playing: %1 in %2. Go to the message")
                  .arg(root.shownSender).arg(root.shownRoom)
            : qsTr("Now playing: %1. Go to the message").arg(root.shownSender)
        Accessible.onPressAction: root.openSource()
        Keys.onReturnPressed: root.openSource()
        Keys.onEnterPressed: root.openSource()
        Keys.onSpacePressed: root.voice.togglePlayPause()

        HoverHandler { id: cardHover }
        TapHandler {
            // WithinBounds: the exclusive grab, so a tap here never also
            // reaches the timeline beneath (CLAUDE.md §16).
            gesturePolicy: TapHandler.WithinBounds
            onTapped: root.openSource()
        }
        DragHandler {
            id: drag
            objectName: "voiceMiniPlayerDrag"
            target: null
            // Where the card's centre was at the last move: activeTranslation
            // is already reset by the time `active` falls.
            property point lastCentre: Qt.point(0, 0)
            onActiveTranslationChanged: {
                if (active)
                    lastCentre = Qt.point(
                        card.restX + activeTranslation.x + card.width / 2,
                        card.restY + activeTranslation.y + card.height / 2)
            }
            // Snap: the corner nearest that centre. Writing the setting moves
            // the card there.
            onActiveChanged: {
                if (!active)
                    root.snapFrom(lastCentre.x, lastCentre.y)
            }
        }

        RowLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: AppTheme.spacing8
            anchors.leftMargin: AppTheme.spacing12
            spacing: AppTheme.spacing8

            Avatar {
                objectName: "voiceMiniPlayerAvatar"
                Layout.alignment: Qt.AlignVCenter
                size: 34
                mxc: root.shownAvatar
                name: root.shownSender
                colorKey: root.shownSenderId
                onScreen: card.visible
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 1
                Label {
                    objectName: "voiceMiniPlayerSender"
                    Layout.fillWidth: true
                    // Remote text: never markup.
                    textFormat: Text.PlainText
                    text: root.shownSender
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: AppTheme.spacing4
                    Icon {
                        name: root.shownVoice ? "mic" : "graphic_eq"
                        size: 12
                        color: root.voice.fetchState === "failed"
                               ? AppTheme.danger : AppTheme.textMuted
                    }
                    Label {
                        objectName: "voiceMiniPlayerRoom"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        textFormat: Text.PlainText
                        text: root.voice.fetchState === "failed"
                              ? qsTr("This audio cannot be played")
                              : (root.shownRoom.length > 0
                                 ? root.shownRoom
                                 : (root.shownVoice ? qsTr("Voice message")
                                                    : qsTr("Audio")))
                        color: root.voice.fetchState === "failed"
                               ? AppTheme.danger : AppTheme.textMuted
                        font.pixelSize: AppTheme.textMicro + 1
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "voiceMiniPlayerClock"
                        text: root.formatPosition(root.voice.position) + " / "
                              + root.formatDuration(root.voice.duration)
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMicro + 1
                        Accessible.name: qsTr("%1 of %2")
                            .arg(root.formatPosition(root.voice.position))
                            .arg(root.formatDuration(root.voice.duration))
                    }
                }
            }

            IconButton {
                objectName: "voiceMiniPlayerPlayPause"
                fill: true
                implicitWidth: 30; implicitHeight: 30
                iconSize: 17
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
                objectName: "voiceMiniPlayerStop"
                implicitWidth: 26; implicitHeight: 26
                iconSize: 15
                iconName: "close"
                readonly property string actionLabel: qsTr("Stop playback")
                Accessible.name: actionLabel
                ToolTip.text: actionLabel
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: root.voice.stop()
            }
        }

        // The thin progress bar along the bottom, inset from the rounded
        // corners.
        Rectangle {
            id: progressTrack
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: AppTheme.spacing12
            anchors.rightMargin: AppTheme.spacing12
            anchors.bottomMargin: 6
            height: 3
            radius: 1.5
            color: AppTheme.borderSubtle
            Rectangle {
                objectName: "voiceMiniPlayerProgress"
                height: parent.height
                radius: parent.radius
                width: root.voice.duration > 0
                       ? parent.width * Math.min(1, root.voice.position
                                                    / root.voice.duration)
                       : 0
                color: AppTheme.accent
            }
        }
    }
}
