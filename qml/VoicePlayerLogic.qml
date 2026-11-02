import QtQuick
import MatrixClient

QtObject {
    id: logic

    readonly property var voice: app.voicePlayback

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
                app.showThread(roomId, threadRootId)
                if (app.timeline.threadRepliesInline === true
                        && eventId.length > 0)
                    app.pagination.jumpToEvent(eventId)
            })
        } else if (eventId.length > 0) {
            Qt.callLater(function() { app.pagination.jumpToEvent(eventId) })
        }
    }

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

    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t, 1)
    }
    function ratio(a, b) {
        var x = AppTheme.relativeLuminance(a)
        var y = AppTheme.relativeLuminance(b)
        return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
    }
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

    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return logic.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return logic.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        return AppTheme.clockText(totalSeconds)
    }
    readonly property string clock:
        logic.formatPosition(logic.voice.position) + " / "
        + logic.formatDuration(logic.voice.duration)
    readonly property string accessibleSummary:
        (logic.voice.playing ? qsTr("Playing") : qsTr("Paused")) + ", "
        + logic.shownTitle
}
