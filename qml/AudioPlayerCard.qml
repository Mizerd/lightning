import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Inline audio / voice-message player. A VIEW of the one app-owned player
// (app.voicePlayback): this card holds no MediaPlayer of its own, because rows
// are destroyed and rebuilt on every room switch and a clip must keep playing
// through one. The card whose eventId is the player's current clip shows the
// live state (playing, position, waveform progress) and drives it; every other
// card shows idle. Play fetches the decrypted payload through MediaBridge's
// validated playable materialization (inside the controller). Voice messages
// show their real MSC3245 waveform when the event has one (never a fabricated
// one), else a progress slider. Both are seek targets, by pointer (tap or
// drag) and by keyboard, and on an idle row a seek starts the clip there.
//
// The row's height never changes with playback (design-media B6): an audio
// file's cover art is a fixed thumbnail under its play button, and its
// embedded title replaces the file name, both from the controller's RAM-only
// record of an earlier play (clipInfo), so they stay after the clip ends. The
// speed and volume controls keep their places at rest, so the scrubber does
// not shrink when playback starts.
Rectangle {
    id: root
    objectName: "audioPlayerCard"

    property string mediaKey: ""
    // The event this card plays. The player is keyed by it, so a rebuilt row
    // (room switch, scroll, thread panel) finds its live state again.
    property string eventId: ""
    // The room the event is in, and where the player bar takes the reader
    // back to.
    property string roomId: ""
    property string senderName: ""
    property string senderId: ""
    property string senderAvatarMxc: ""
    // The thread the row is shown in ("" for the main timeline).
    property string threadRootId: ""
    property string filename: ""
    property string mimetype: ""
    property real fileSize: 0
    property real durationMs: 0
    property bool isVoice: false
    // Normalized MSC3245 amplitudes; empty when the event had none.
    property var waveform: []
    property bool rowOnScreen: true
    // Speculative prefetch gate, separate from rowOnScreen: a row that only
    // swept past during a gesture must not pull a payload. Permissive default
    // for standalone hosts.
    property bool prefetchAllowed: true
    property bool canSave: false
    // Drawn inside a chat bubble (the Bubbles layout's DMs): the bubble is the
    // surface, so the card draws no second box inside it.
    property bool inBubble: false

    signal saveRequested()
    // Non-bridge backends (plain HTTP media) open externally
    // (app.media.openExternal).
    signal openExternalRequested()

    readonly property var voice: app.voicePlayback
    // This card is the player's current clip.
    readonly property bool isCurrent:
        root.eventId.length > 0 && root.voice.active
        && root.voice.eventId === root.eventId
    readonly property bool playing: root.isCurrent && root.voice.playing
    readonly property bool ready: root.isCurrent && root.voice.loaded
    readonly property string fetchState:
        root.isCurrent ? root.voice.fetchState : "idle"
    readonly property real livePosition:
        root.isCurrent ? root.voice.position : 0
    readonly property real liveDuration:
        root.isCurrent && root.voice.duration > 0 ? root.voice.duration
                                                  : root.durationMs
    readonly property bool seekable: root.ready && root.voice.seekable
    // What an earlier play of this file found: its tags and cover art, kept
    // in RAM by the controller (never written to disk). Re-read whenever the
    // controller learns something (clipInfoRevision).
    readonly property var clipInfo: {
        var revision = root.voice.clipInfoRevision
        return root.isVoice || root.mediaKey.length === 0
            ? ({}) : root.voice.clipInfo(root.mediaKey)
    }
    // The cover art as MediaBridge's RAM-only image URL, or "".
    property string artworkSource: root.clipInfo.artworkSource || ""
    readonly property string trackTitle: root.clipInfo.title || ""
    readonly property string trackArtist: root.clipInfo.artist || ""
    // The speed this row plays at: the remembered voice speed for a voice
    // message, the file's own while it is the current clip, else 1x.
    readonly property real rate: root.isVoice
        ? app.settings.voicePlaybackRate
        : (root.isCurrent ? root.voice.rate : 1.0)

    // Position floors, total rounds. At 25.7 s you haven't reached 0:26, but a
    // 25.7 s clip is 26 s long (matching `embedDurationText`). Recording
    // counters are positions and floor too.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.round(ms / 1000))
    }
    // `ink` at 4.5:1 on the card's own surface (the muted clock was 3.9:1 and
    // the accent speed 1.9:1 on a dark theme: C21-D05), through the shared
    // helper, which keeps the hue and moves only lightness. In a bubble the
    // surface is the bubble's, which this card does not know: the ink is
    // left alone there.
    function readableInk(ink) {
        return root.inBubble ? ink : AppTheme.legibleInkOn(ink, root.color)
    }
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function meta(startAtMs) {
        var m = {
            "senderName": root.senderName,
            "senderId": root.senderId,
            "senderAvatarMxc": root.senderAvatarMxc,
            "filename": root.filename,
            "isVoice": root.isVoice,
            "durationMs": root.durationMs,
            "threadRootId": root.threadRootId,
            "waveform": root.waveform || []
        }
        if (startAtMs !== undefined && startAtMs >= 0)
            m.startAtMs = Math.round(startAtMs)
        return m
    }
    function togglePlay() {
        if (!app.mediaBridge.supported) {
            root.openExternalRequested()
            return
        }
        // Another clip stops first; this one resumes where it was.
        root.voice.toggle(root.eventId, root.roomId, root.mediaKey, root.meta())
    }
    // A seek: on the current clip, a seek; on an idle row, start it there.
    function seekTo(ms) {
        if (root.isCurrent) {
            root.voice.seek(ms)
            return
        }
        if (!app.mediaBridge.supported || root.eventId.length === 0
            || root.mediaKey.length === 0)
            return
        root.voice.play(root.eventId, root.roomId, root.mediaKey,
                        root.meta(ms))
    }
    // The speed menu: a voice row sets the remembered voice speed; an audio
    // file only its own clip's, while it plays.
    function setRate(rate) {
        if (root.isVoice)
            app.settings.voicePlaybackRate = rate
        else if (root.isCurrent)
            root.voice.setRate(rate)
    }
    // Bounded speculative prefetch (size-capped, lowest priority, deduplicated
    // by MediaBridge) so Play starts without a download wait.
    function maybePrefetch() {
        // "Preload videos and audio" off means no speculative download.
        if (rowOnScreen && prefetchAllowed && mediaKey.length > 0
            && app.mediaBridge.supported
            && app.settings.videoPrefetchEnabled)
            app.mediaBridge.prefetchPlayable(
                mediaKey,
                fileSize || app.settings.knownMediaSizeBytes(mediaKey) || 0)
    }
    onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()
    Component.onCompleted: {
        maybePrefetch()
        reportVisibility()
    }
    onMediaKeyChanged: maybePrefetch()
    // Scrolling a playing row away does not pause it: the clip belongs to the
    // app. The player bar (or the popped-out card) carries it while the row
    // is off screen, so the row tells the player whether it is on screen.
    onRowOnScreenChanged: {
        if (rowOnScreen)
            maybePrefetch()
        reportVisibility()
    }
    // `rowOnScreen` is the timeline's ACTIVATION band (a viewport of slack
    // each side), not visibility: measured live, a row scrolled a screen away
    // still counted as on screen and the popped-out card never came (U2). So
    // the current clip's row (only that one) checks its real geometry against
    // the scrolling view it sits in, a few times a second while in the band.
    property Item _viewport: null
    function findViewport() {
        for (var p = root.parent; p; p = p.parent) {
            if (p.contentY !== undefined && p.flickableDirection !== undefined)
                return p
        }
        return null
    }
    function actuallyOnScreen() {
        if (!root.visible || !root.rowOnScreen || root.width <= 0)
            return false
        if (!root._viewport)
            root._viewport = root.findViewport()
        var view = root._viewport
        if (!view)
            return true // standalone: the band is all there is
        var r = root.mapToItem(view, 0, 0, root.width, root.height)
        // At least a sliver of the controls must show.
        return r.x + r.width > 8 && r.x < view.width - 8
            && r.y + r.height > 8 && r.y < view.height - 8
    }
    function reportVisibility() {
        root.voice.reportRowVisible(root, root.eventId,
                                    root.isCurrent && root.actuallyOnScreen())
    }
    Timer {
        interval: 250
        repeat: true
        running: root.isCurrent && root.rowOnScreen
        onTriggered: root.reportVisibility()
    }
    onIsCurrentChanged: reportVisibility()
    // The window's "focus the player" shortcut lands here when the clip's
    // row is the visible player (popped out, so no strip, and no card).
    function focusPlayButton() {
        playButton.forceActiveFocus(Qt.ShortcutFocusReason)
    }
    onVisibleChanged: reportVisibility()
    Component.onDestruction: root.voice.reportRowVisible(root, root.eventId,
                                                         false)

    implicitWidth: Math.min(
        360, root.hostContentWidth >= 0
             ? root.hostContentWidth
             : (bubble ? bubble.width : 360))
    implicitHeight: cardRow.implicitHeight + 12
    color: root.inBubble ? "transparent" : AppTheme.embedSurface
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: root.inBubble ? 0 : 1
    // Legacy width source for standalone use (tests may leave it null). Its
    // outer width includes the bubble's padding, so the delegate passes
    // `hostContentWidth` (the content column's inner width) instead.
    property var bubble: null
    /// The width the host allows; negative means unset.
    property real hostContentWidth: -1

    RowLayout {
        id: cardRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        anchors.leftMargin: root.inBubble ? 0 : 6
        anchors.rightMargin: root.inBubble ? 0 : 6
        spacing: 8

        // An audio file's cover: a fixed thumbnail the play button sits on,
        // as on every music card; the tile alone until a play has seen a
        // cover. A voice message has just the button.
        Item {
            objectName: "audioArtSlot"
            Layout.alignment: Qt.AlignTop
            implicitWidth: root.isVoice ? playButton.implicitWidth : 40
            implicitHeight: root.isVoice ? playButton.implicitHeight : 40
            Rectangle {
                id: coverArtBox
                objectName: "audioCoverArtBox"
                anchors.fill: parent
                visible: !root.isVoice
                radius: AppTheme.radiusSm
                color: AppTheme.cardElevated
                clip: true
                Image {
                    id: coverArtImage
                    objectName: "audioCoverArtwork"
                    anchors.fill: parent
                    source: coverArtBox.visible ? root.artworkSource : ""
                    sourceSize.width: 96
                    // A square thumbnail of the cover.
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    Accessible.name: qsTr("Audio cover artwork")
                }
            }
            IconButton {
                id: playButton
                objectName: "audioPlayPauseButton"
                anchors.centerIn: parent
                fill: true
                // Click focus, so Space after Play toggles the clip.
                focusPolicy: Qt.StrongFocus
                implicitWidth: 30; implicitHeight: 30
                // Always a play glyph when idle (a mic read as "record"); the
                // voice label and waveform identify voice messages.
                iconName: root.fetchState === "fetching"
                          ? "schedule"
                          : (root.playing ? "pause" : "play_arrow")
                iconSize: 17
                enabled: root.fetchState !== "fetching"
                readonly property string what: root.isVoice
                    ? qsTr("voice message")
                    : (root.trackTitle || root.filename || qsTr("audio"))
                readonly property string actionLabel: root.playing
                    ? qsTr("Pause %1").arg(what) : qsTr("Play %1").arg(what)
                // The state for assistive technology; the position is the
                // scrubber's value, not a description that ticks.
                Accessible.name: actionLabel
                Accessible.description: root.isCurrent
                    ? (root.playing ? qsTr("Playing") : qsTr("Paused"))
                    : qsTr("%1 long").arg(root.formatDuration(root.liveDuration))
                // Its own tooltip, beside the button rather than over the
                // title, and hidden by the click it describes: the shared
                // attached tooltip kept saying "Play" over a clip that played.
                property bool tipSuppressed: false
                onHoveredChanged: if (!hovered) tipSuppressed = false
                onClicked: {
                    tipSuppressed = true
                    root.togglePlay()
                }
                // Return and Enter act as Space does (C21-D09).
                Keys.onReturnPressed: (event) => {
                    event.accepted = true
                    if (enabled) clicked()
                }
                Keys.onEnterPressed: (event) => {
                    event.accepted = true
                    if (enabled) clicked()
                }
                ToolTip {
                    objectName: "audioPlayPauseTip"
                    parent: playButton
                    visible: playButton.hovered && !playButton.tipSuppressed
                    delay: 600
                    text: playButton.actionLabel
                    // Centred above the button like its siblings: to the LEFT it
                    // left the timeline pane over the room list (C21-F09).
                    x: (playButton.width - implicitWidth) / 2
                    y: -implicitHeight - 6
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "audioTitle"
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.fetchState === "failed"
                      ? qsTr("This audio cannot be played")
                      : root.isVoice ? qsTr("Voice message")
                                     : (root.trackTitle || root.filename
                                        || qsTr("Audio"))
                color: root.fetchState === "failed"
                       ? AppTheme.danger : AppTheme.text
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
                elide: Label.ElideMiddle
                Layout.fillWidth: true
            }

            // Seek surface: real waveform bars when MSC3245 data exists,
            // otherwise a slim progress slider. Both are seek targets.
            Item {
                Layout.fillWidth: true
                implicitHeight: 18
                visible: root.fetchState !== "failed"

                VoiceSeekBar {
                    id: seekSlider
                    objectName: root.isVoice && root.waveform
                                && root.waveform.length > 0
                                ? "audioWaveRow" : "audioSeekSlider"
                    anchors.fill: parent
                    followsPlayer: root.isCurrent
                    idleDuration: root.durationMs
                    idleSeekable: app.mediaBridge.supported
                                  && root.eventId.length > 0
                                  && root.mediaKey.length > 0
                                  && root.liveDuration > 0
                    waveform: root.isVoice ? (root.waveform || []) : []
                    trackHeight: 4
                    // An idle row starts its clip there.
                    onSeekRequested: (ms) => root.seekTo(ms)
                    Accessible.name: root.isVoice
                        ? qsTr("Voice message position")
                        : qsTr("Seek position")
                }
            }

            Label {
                objectName: "audioClock"
                text: {
                    var pos = root.formatPosition(root.livePosition)
                    var total = root.formatDuration(root.liveDuration)
                    var line = root.isCurrent ? pos + " / " + total : total
                    if (!root.isVoice && root.trackArtist.length > 0)
                        line += " • " + root.trackArtist
                    if (!root.isVoice && root.fileSize > 0) {
                        var kb = root.fileSize / 1024
                        line += " • " + (kb < 1024 ? kb.toLocaleString(Qt.locale(AppLocale.name), "f", 0) + " KB"
                                        : Number(kb / 1024).toLocaleString(Qt.locale(AppLocale.name), "f", 1) + " MB")
                    }
                    return line
                }
                // Elides inside the card. Tags are remote text.
                Layout.fillWidth: true
                // Ticks while playing; the scrubber's value carries that.
                Accessible.ignored: root.isCurrent
                elide: Label.ElideRight
                textFormat: Text.PlainText
                color: root.readableInk(AppTheme.textMuted)
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
        }

        // Playback speed: opens a list. A voice row shows (and sets) the
        // remembered voice speed even at rest, so a speed left on is visible
        // before Play; an audio file plays at 1x until changed while playing.
        AbstractButton {
            id: audioSpeedButton
            objectName: "audioSpeedButton"
            readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
            readonly property real rate: root.rate
            Layout.alignment: Qt.AlignTop
            implicitWidth: 34; implicitHeight: 24
            enabled: root.isVoice || root.ready
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(rate)
            ToolTip.text: qsTr("Playback speed")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: speedMenu.popup()
            // Return and Enter open it as Space does (C21-D09).
            Keys.onReturnPressed: (event) => {
                event.accepted = true
                if (enabled) clicked()
            }
            Keys.onEnterPressed: (event) => {
                event.accepted = true
                if (enabled) clicked()
            }
            AppMenu {
                id: speedMenu
                objectName: "audioSpeedMenu"
                menuWidth: 140
                Repeater {
                    model: audioSpeedButton.rates
                    AppMenuItem {
                        required property real modelData
                        text: modelData + "×"
                        iconName: Math.abs(modelData - audioSpeedButton.rate)
                                  < 0.001 ? "check" : ""
                        // The player follows the remembered voice speed, or
                        // takes the file's own (VoicePlaybackController).
                        onTriggered: root.setRate(modelData)
                    }
                }
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: audioSpeedButton.hovered ? AppTheme.hover : "transparent"
                border.width: audioSpeedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: audioSpeedButton.rate + "×"
                color: root.readableInk(
                    Math.abs(audioSpeedButton.rate - 1.0) < 0.001
                    ? AppTheme.textMuted : AppTheme.accent)
                opacity: audioSpeedButton.enabled ? 1 : 0.5
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                font.weight: AppTheme.weightBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        MediaVolumeControl {
            objectName: "audioMuteButton"
            Layout.alignment: Qt.AlignTop
            audio: root.isCurrent ? root.voice.audioOutput : null
            sliderObjectName: "audioVolumeSlider"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            // Kept in place at rest (disabled), so the scrubber's width does
            // not jump when playback starts.
            opacity: root.ready ? 1 : 0.45
        }
        IconButton {
            objectName: "audioSaveButton"
            Layout.alignment: Qt.AlignTop
            iconName: "download"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.canSave
            readonly property string actionLabel:
                qsTr("Download %1").arg(root.filename || qsTr("audio"))
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.saveRequested()
        }
    }
}
