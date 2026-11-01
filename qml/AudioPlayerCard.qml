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
// The row's height never changes with playback (design-media B6), only once
// when a cover is first seen with "Large album art" on (the big square cannot
// be known before an earlier play: clip info is RAM-only): an audio
// file's cover art is a fixed tile (40 px, or with "Large album art" a 96 px
// square the height of the control block, beside the title and controls), and its
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
    // The event carries a cover another client attached (`info.thumbnail_url`
    // or `thumbnail_file`). It is fetched through the media bridge, never from
    // the URL, and only stands in until the file's own embedded art is read.
    property bool thumbAvailable: false
    // Drawn inside a chat bubble (the Bubbles layout's DMs): the bubble is the
    // surface, so the card draws no second box inside it.
    property bool inBubble: false

    signal saveRequested()
    // The cover was clicked: show `source` (MediaBridge's RAM-only image URL,
    // never the homeserver's) enlarged, titled `title`. The host owns the
    // viewer; the clip keeps playing.
    signal artworkViewRequested(string source, string title)
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
    property string artworkSource: root.clipInfo.artworkSource || root.wireCoverSource
    // The event's own cover (see thumbAvailable): "" until the row is near the
    // screen, and "" for good once it has failed to load, so a broken cover
    // looks exactly like no cover (no empty square).
    property string wireCoverSource: ""
    property bool wireCoverFailed: false
    function refreshWireCover() {
        if (wireCoverSource.length > 0 || wireCoverFailed || !thumbAvailable
                || isVoice || mediaKey.length === 0 || !rowOnScreen
                || !app.mediaBridge || !app.mediaBridge.supported)
            return
        wireCoverSource = app.mediaBridge.mediaSource(mediaKey, "thumb") || ""
    }
    function noteCoverStatus(image) {
        if (image.status === Image.Ready && root.wireCoverSource.length > 0
                && image.source.toString() === root.wireCoverSource
                && !root.clipInfo.artworkSource)
            root.voice.rememberEventCover(root.mediaKey, root.wireCoverSource)

        if (image.status === Image.Error && image.source.toString().length > 0
                && image.source.toString() === root.wireCoverSource) {
            root.wireCoverSource = ""
            root.wireCoverFailed = true
        }
    }
    function requestArtworkView() {
        if (root.artworkSource.length > 0)
            root.artworkViewRequested(root.artworkSource,
                                      root.filename || root.trackTitle)
    }
    onThumbAvailableChanged: refreshWireCover()
    readonly property string wireCoverCacheKey: "thumb:" + root.mediaKey
    Connections {
        target: root.thumbAvailable && app.mediaBridge ? app.mediaBridge : null
        function onMediaCached(cacheKey) {
            if (cacheKey === root.wireCoverCacheKey && !root.wireCoverFailed)
                root.wireCoverSource = app.mediaBridge.cachedSource(cacheKey)
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === root.wireCoverCacheKey)
                root.wireCoverFailed = true
        }
        function onMediaRetryable(cacheKey) {
            if (cacheKey === root.wireCoverCacheKey) {
                root.wireCoverFailed = false
                root.refreshWireCover()
            }
        }
    }
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
        return AppTheme.clockText(totalSeconds)
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
        refreshWireCover()
        maybePrefetch()
        reportVisibility()
    }
    onMediaKeyChanged: {
        wireCoverSource = ""
        wireCoverFailed = false
        refreshWireCover()
        maybePrefetch()
    }
    // Scrolling a playing row away does not pause it: the clip belongs to the
    // app. The player bar (or the popped-out card) carries it while the row
    // is off screen, so the row tells the player whether it is on screen.
    onRowOnScreenChanged: {
        if (rowOnScreen) {
            refreshWireCover()
            maybePrefetch()
        }
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
    // "Large album art" (app.settings.largeAlbumArt, default on): an audio
    // file with a cover shows it as a square tile the height of the control
    // block, to the left of the title, scrubber and controls (one composed
    // card). The tile is the SAME object as the compact 40 px one, so the
    // row's object count does not change. A cover the event declares
    // (`thumbAvailable`) reserves the big tile from the first frame, so it
    // does not grow when the picture lands; a file's own embedded art is only
    // known after a first play (clip info is RAM-only), so the row grows
    // once then. Narrow cards (< 280 px) keep the compact tile, because the
    // controls need the width.
    readonly property bool largeCover:
        !root.isVoice && root.width >= 280
        && app.settings && app.settings.largeAlbumArt === true
        && (root.artworkSource.length > 0
            || (root.thumbAvailable && !root.wireCoverFailed))
    readonly property real tileSize:
        root.largeCover ? Math.round(AppTheme.scaled(96)) : 40
    implicitHeight: cardGrid.implicitHeight + 12
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

    // Compact: one row (tile, play, title/scrubber, speed, volume, save).
    // Large: the tile spans two rows; the title and scrubber sit above the
    // controls, the controls at the tile's foot. Same children either way.
    GridLayout {
        id: cardGrid
        readonly property bool big: root.largeCover
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        anchors.leftMargin: root.inBubble ? 0 : 6
        anchors.rightMargin: root.inBubble ? 0 : 6
        columns: 6
        columnSpacing: 8
        rowSpacing: 6

        // The cover tile of an audio file (a voice message has none). Inert
        // on the placeholder of a file with no cover.
        Item {
            objectName: "audioArtSlot"
            Layout.alignment: Qt.AlignTop
            Layout.row: 0
            Layout.column: 0
            Layout.rowSpan: cardGrid.big ? 2 : 1
            visible: !root.isVoice
            implicitWidth: root.tileSize
            implicitHeight: root.tileSize
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
                    onStatusChanged: root.noteCoverStatus(this)
                    // Twice the tile, for a sharp picture at 2x.
                    sourceSize.width: root.largeCover ? 320 : 96
                    // A square tile of the cover, whatever its shape.
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    // Fades in over the reserved tile; the tile never moves.
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity {
                        enabled: !AppTheme.reducedMotion
                        NumberAnimation { duration: 220 }
                    }
                    Accessible.name: qsTr("Audio cover artwork")
                }
                // A hairline that warms to the accent while it plays.
                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: "transparent"
                    border.width: 1
                    border.color: root.playing ? AppTheme.accent
                                               : AppTheme.border
                    Behavior on border.color {
                        enabled: !AppTheme.reducedMotion
                        ColorAnimation { duration: 180 }
                    }
                }
            }
            CoverArtButton {
                objectName: "audioCoverButton"
                anchors.fill: parent
                enabled: root.artworkSource.length > 0
                onActivated: root.requestArtworkView()
            }
        }
        Item {
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.big ? 1 : 0
            Layout.column: 1
            Layout.fillWidth: cardGrid.big
            implicitWidth: playButton.implicitWidth
            implicitHeight: root.isVoice ? playButton.implicitHeight
                                         : (cardGrid.big ? 30 : 40)
            IconButton {
                id: playButton
                objectName: "audioPlayPauseButton"
                x: cardGrid.big ? 0 : (parent.width - width) / 2
                y: (parent.height - height) / 2
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
            Layout.alignment: Qt.AlignTop
            Layout.row: 0
            Layout.column: cardGrid.big ? 1 : 2
            Layout.columnSpan: cardGrid.big ? 4 : 1
            Layout.fillHeight: cardGrid.big
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
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.big ? 1 : 0
            Layout.column: cardGrid.big ? 2 : 3
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
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.big ? 1 : 0
            Layout.column: cardGrid.big ? 3 : 4
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
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.big ? 1 : 0
            Layout.column: cardGrid.big ? 4 : 5
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
