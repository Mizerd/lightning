import QtQuick
import MatrixClient

// What the "now playing" strip (VoicePlayerBar) and the popped-out card
// (VoiceMiniPlayer) both show about the app-owned clip (app.voicePlayback),
// and the actions they share. Not visual.
//
// The shown* values are copied while a clip is loaded, so a strip or card
// fading out after a stop does not draw an empty title.
QtObject {
    id: logic

    readonly property var voice: app.voicePlayback

    // A clip played from a thread panel lives in that thread's timeline.
    readonly property bool fromThread:
        logic.voice.threadRootId.length > 0
        && logic.voice.threadRootId !== logic.voice.eventId

    property string shownTitle: ""
    property string shownSubtitle: ""
    property string shownSender: ""
    property string shownSenderId: ""
    property string shownAvatar: ""
    property string shownArt: ""
    property bool shownVoice: true

    // Remote text (display names, room names, file tags) is shown as plain
    // text only, by every consumer.
    function titleText() {
        if (logic.voice.isVoice)
            return logic.voice.senderName.length > 0
                ? logic.voice.senderName : qsTr("Voice message")
        if (logic.voice.trackTitle.length > 0)
            return logic.voice.trackTitle
        return logic.voice.filename.length > 0 ? logic.voice.filename
                                               : qsTr("Audio")
    }
    // "Voice message · Thread · Room", "Artist · Room".
    function subtitleText() {
        var parts = []
        if (logic.voice.isVoice)
            parts.push(qsTr("Voice message"))
        else if (logic.voice.trackArtist.length > 0)
            parts.push(logic.voice.trackArtist)
        else if (logic.voice.senderName.length > 0)
            parts.push(logic.voice.senderName)
        if (logic.fromThread)
            parts.push(qsTr("Thread"))
        if (logic.voice.roomName.length > 0)
            parts.push(logic.voice.roomName)
        return parts.join(" · ")
    }
    function capture() {
        if (!logic.voice.active)
            return
        logic.shownTitle = logic.titleText()
        logic.shownSubtitle = logic.subtitleText()
        logic.shownSender = logic.voice.senderName
        logic.shownSenderId = logic.voice.senderId
        logic.shownAvatar = logic.voice.senderAvatarMxc
        logic.shownArt = logic.voice.isVoice ? "" : logic.voice.artworkSource
        logic.shownVoice = logic.voice.isVoice
    }
    property Connections _follow: Connections {
        target: logic.voice
        function onCurrentChanged() { logic.capture() }
        function onMetadataChanged() { logic.capture() }
    }
    Component.onCompleted: logic.capture()

    // The status line: a failure, the call's pause, or the next clip in the
    // last seconds; "" when there is nothing to say beyond the subtitle.
    readonly property string statusText: {
        if (!logic.voice.active)
            return ""
        if (logic.voice.fetchState === "failed")
            return qsTr("This audio cannot be played")
        if (logic.voice.pausedForCall)
            return qsTr("Paused for the call")
        var remaining = logic.voice.duration - logic.voice.position
        if (logic.voice.hasNext && logic.voice.playing
            && logic.voice.duration > 0 && remaining < 2500) {
            var next = logic.voice.upNext[0]
            return qsTr("Next: %1")
                .arg(next && next.senderName ? next.senderName
                                             : qsTr("Voice message"))
        }
        return ""
    }

    // Back to the message: its room, then the event (or its thread). As in
    // notification routing (Main.qml), the jump waits one turn for the room
    // switch to settle. From Settings it closes Settings first.
    function openSource() {
        var roomId = logic.voice.roomId
        var eventId = logic.voice.eventId
        var threadRootId = logic.voice.threadRootId
        if (roomId.length === 0)
            return
        if (app.currentScreen === 2)
            app.showMain()
        app.openRoom(roomId)
        if (threadRootId.length > 0 && threadRootId !== eventId) {
            Qt.callLater(function() {
                app.thread.openThread(roomId, threadRootId)
            })
        } else if (eventId.length > 0) {
            Qt.callLater(function() { app.pagination.jumpToEvent(eventId) })
        }
    }

    // The speed chip: 1x -> 1.5x -> 2x -> 1x; a speed chosen elsewhere (the
    // row's menu) moves to the next step above it.
    readonly property var rateSteps: [1.0, 1.5, 2.0]
    function nextRate(current) {
        for (var i = 0; i < logic.rateSteps.length; ++i) {
            if (logic.rateSteps[i] > current + 0.001)
                return logic.rateSteps[i]
        }
        return logic.rateSteps[0]
    }
    function cycleRate() {
        logic.voice.setRate(logic.nextRate(logic.voice.rate))
    }
    function rateText(rate) {
        return (Math.round(rate * 100) / 100) + "×"
    }

    // ── Colours (design-media P1). The floating card's surface must read as
    // its own thing over a bubble in every theme: the stock popover surface
    // IS the other-bubble colour in seven of the eleven themes (measured, 1:1).
    // So a dark theme lifts it a step above the other bubble, and a light
    // theme uses white over its tinted canvas and bubbles. Every ink on it is
    // then guarded to 4.5:1 by construction (readable()), whatever palette,
    // custom ones included.
    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t, 1)
    }
    function ratio(a, b) {
        var x = AppTheme.relativeLuminance(a)
        var y = AppTheme.relativeLuminance(b)
        return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
    }
    // `ink`, moved towards the primary text colour until it reads at 4.5:1
    // on `bg` (WCAG AA for small text).
    function readable(ink, bg) {
        var c = ink
        for (var i = 0; i < 10 && logic.ratio(c, bg) < 4.5; ++i)
            c = logic.mix(c, AppTheme.textPrimary, 0.25)
        return logic.ratio(c, bg) >= 4.5 ? c : AppTheme.textPrimary
    }
    readonly property color cardSurface: AppTheme.dark
        ? logic.mix(AppTheme.otherBubble, AppTheme.textPrimary, 0.10)
        : "#ffffff"
    readonly property color cardBorder:
        logic.mix(logic.cardSurface, AppTheme.textPrimary, 0.30)

    // Position floors, total rounds (CLAUDE.md §16): at 25.7 s you have not
    // reached 0:26, but a 25.7 s clip is 26 s long. As in AudioPlayerCard.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return logic.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return logic.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    readonly property string clock:
        logic.formatPosition(logic.voice.position) + " / "
        + logic.formatDuration(logic.voice.duration)
    // For assistive technology: state and title. The position is the
    // scrubber's value (its value interface), so this does not change every
    // second.
    readonly property string accessibleSummary:
        (logic.voice.playing ? qsTr("Playing") : qsTr("Paused")) + ", "
        + logic.shownTitle
}
