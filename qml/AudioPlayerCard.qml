import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Inline audio / voice-message player. Play fetches the decrypted payload
// through MediaBridge's validated playable materialization and plays the
// session-scoped temp file in-process. Voice messages show their real MSC3245
// waveform when the event has one (never a fabricated one), else a progress
// slider. One audible card at a time via app.playback; room/account switches
// force a stop.
Rectangle {
    id: root
    objectName: "audioPlayerCard"

    property string mediaKey: ""
    property string ownerKey: ""
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

    signal saveRequested()
    // Non-bridge backends (plain HTTP media) open externally
    // (app.media.openExternal).
    signal openExternalRequested()

    // The player loads on the first Play press, so a busy room doesn't hold one
    // QMediaPlayer per visible row.
    property bool engaged: false
    readonly property var player: engine.item
    readonly property bool playing:
        player ? player.playbackState === MediaPlayer.PlayingState : false
    readonly property bool ready:
        player ? player.source.toString().length > 0 : false
    property string fetchState: "idle" // idle / fetching / failed
    // Stable failure identity: MediaBridge marks/signals by this cache key.
    readonly property string fetchCacheKey: "full:" + mediaKey
    // The media key pinned against LRU eviction while the player holds the file
    // open. Recorded at pin time, since delegate reuse changes mediaKey before
    // resetPlayback runs.
    property string pinnedKey: ""
    // Position kept across an offscreen engine unload, so Play resumes.
    property real resumePositionMs: 0
    // Embedded cover art, available once the backend has opened the file.
    // MediaBridge keeps the decoded pixels in a small RAM-only LRU; no
    // decrypted artwork is written to disk.
    property string artworkSource: ""

    function refreshArtwork() {
        artworkSource = ""
        if (!player || isVoice || mediaKey.length === 0)
            return
        var artwork = player.metaData.value(MediaMetaData.CoverArtImage)
        if (!artwork)
            artwork = player.metaData.value(MediaMetaData.ThumbnailImage)
        if (artwork)
            artworkSource = app.mediaBridge.audioArtworkSource(mediaKey,
                                                                artwork)
    }

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
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function togglePlay() {
        if (!app.mediaBridge.supported) {
            root.openExternalRequested()
            return
        }
        engaged = true // synchronous Loader: player exists after this
        if (playing) {
            player.pause()
            return
        }
        if (fetchState === "failed") {
            // Explicit retry: clear the failure mark first, or playableSource
            // stays blocked by it.
            app.mediaBridge.retry(fetchCacheKey)
            fetchState = "idle"
        }
        if (ready) {
            app.playback.acquire(root.ownerKey)
            player.play()
            return
        }
        var url = app.mediaBridge.playableSource(root.mediaKey)
        if (url.length > 0) {
            fetchState = "idle"
            fetchingKey = ""
            player.source = url
            pinFile()
            app.playback.acquire(root.ownerKey)
            player.play()
        } else {
            fetchState = "fetching"
            fetchingKey = root.mediaKey
        }
    }
    // The key with an outstanding fetch for this card; reset/destruction
    // cancels it so abandoned downloads stop.
    property string fetchingKey: ""
    function cancelFetch() {
        if (fetchingKey.length === 0)
            return
        app.mediaBridge.cancelPlayable(fetchingKey)
        fetchingKey = ""
    }
    function pinFile() {
        // The player holds the materialized file open; the LRU must not delete
        // it (seek/replay would fail).
        if (pinnedKey === mediaKey)
            return
        unpinFile()
        app.mediaBridge.pinPlayable(mediaKey)
        pinnedKey = mediaKey
    }
    function unpinFile() {
        if (pinnedKey.length === 0)
            return
        app.mediaBridge.unpinPlayable(pinnedKey)
        pinnedKey = ""
    }
    function resetPlayback() {
        if (player) {
            player.stop()
            player.source = ""
        }
        engaged = false // unload the backend and its temp-file handle
        unpinFile()
        cancelFetch()
        fetchState = "idle"
        artworkSource = ""
        app.playback.release(root.ownerKey)
    }
    // Bounded speculative prefetch (size-capped, lowest priority, deduplicated
    // by MediaBridge) so Play starts without a download wait.
    function maybePrefetch() {
        // GIF autoplay "never" also means no passive media downloads.
        if (rowOnScreen && prefetchAllowed && mediaKey.length > 0
            && app.mediaBridge.supported
            && app.settings.gifAutoplay !== 2)
            app.mediaBridge.prefetchPlayable(
                mediaKey,
                fileSize || app.settings.knownMediaSizeBytes(mediaKey) || 0)
    }
    onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()
    Component.onCompleted: maybePrefetch()
    onMediaKeyChanged: {
        resumePositionMs = 0 // a different track never inherits a position
        resetPlayback()      // delegate reuse safety
        maybePrefetch()
    }
    onRowOnScreenChanged: {
        if (!rowOnScreen && playing)
            player.pause()
        else if (rowOnScreen)
            maybePrefetch()
    }
    Component.onDestruction: {
        unpinFile()
        cancelFetch()
        app.playback.release(root.ownerKey)
    }
    // A paused, scrolled-away card frees its decoder and audio stream after a
    // grace period. The position is kept and resumed from the reused temp file.
    Timer {
        interval: 45000
        running: root.engaged && !root.rowOnScreen && !root.playing
        onTriggered: {
            root.resumePositionMs = root.player ? root.player.position : 0
            root.resetPlayback()
        }
    }
    // A forced stop (room/account switch, sign-out) unloads the engine: the
    // decrypted temp file is about to be wiped.
    readonly property int stopGen: app.playback.stopGeneration
    onStopGenChanged: resetPlayback()

    Connections {
        target: app.mediaBridge
        // Filter on this card's cache key only.
        function onPlayableMediaReady(cacheKey) {
            if (cacheKey !== root.fetchCacheKey
                || root.fetchState !== "fetching")
                return
            var url = app.mediaBridge.playableSource(root.mediaKey)
            if (url.length === 0 || !root.player) return
            root.fetchState = "idle"
            root.fetchingKey = ""
            root.player.source = url
            root.pinFile()
            app.playback.acquire(root.ownerKey)
            root.player.play()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === root.fetchCacheKey
                && root.fetchState === "fetching") {
                root.fetchState = "failed"
                root.fetchingKey = "" // the fetch is over; nothing to cancel
            }
        }
    }
    Connections {
        target: app.playback
        function onAudibleOwnerChanged() {
            if (!app.playback.owns(root.ownerKey) && root.playing)
                player.pause()
        }
        // Space toggles whatever is audible. Re-check the owner: audibility
        // may have moved since the key press.
        function onTogglePlayPauseRequested(ownerKey) {
            if (ownerKey !== root.ownerKey || !root.engaged)
                return
            root.togglePlay()
        }
    }

    Loader {
        id: engine
        active: root.engaged
        sourceComponent: MediaPlayer {
            audioOutput: AudioOutput {
                id: audioOut
                property bool userUnmuted: false
                muted: false
                // The remembered level. A live binding, so every card follows a
                // change; the slider's own write breaks it only on that card,
                // to the same value.
                volume: app.settings.mediaVolume
            }
            // The remembered speed applies to every card.
            playbackRate: app.settings.mediaPlaybackRate
            onErrorOccurred: root.fetchState = "failed"
            onMetaDataChanged: root.refreshArtwork()
            // Resume after an offscreen unload once the media has loaded; an
            // earlier seek would be dropped.
            onMediaStatusChanged: {
                if (mediaStatus === MediaPlayer.LoadedMedia
                    && root.resumePositionMs > 0) {
                    position = root.resumePositionMs
                    root.resumePositionMs = 0
                }
            }
        }
    }

    implicitWidth: Math.min(
        360, root.hostContentWidth >= 0
             ? root.hostContentWidth
             : (bubble ? bubble.width : 360))
    implicitHeight: cardRow.implicitHeight + 12
                    + (coverArtBox.visible
                       ? coverArtBox.implicitHeight + 6 : 0)
    color: AppTheme.embedSurface
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: 1
    // Legacy width source for standalone use (tests may leave it null). Its
    // outer width includes the bubble's padding, so the delegate passes
    // `hostContentWidth` (the content column's inner width) instead.
    property var bubble: null
    /// The width the host allows; negative means unset. Not named
    /// `availableWidth`, which coverArtBox already uses.
    property real hostContentWidth: -1

    // Embedded cover art, as a box attached below the controls, corner-matched
    // and clipped. RAM-only (see artworkSource).
    Rectangle {
        id: coverArtBox
        objectName: "audioCoverArtBox"
        visible: root.artworkSource.length > 0
        anchors.left: parent.left
        anchors.top: cardRow.bottom
        anchors.leftMargin: 6
        anchors.topMargin: 6
        // The box takes the artwork's own aspect. Both axes are bounded, with
        // the width derived from the capped height, so box and artwork keep the
        // same shape at every size.
        readonly property real maxEdge: 420
        readonly property real availableWidth: Math.max(1, root.width - 12)
        // Height per unit width; a fallback ratio until the image reports its
        // size.
        readonly property real artRatio: {
            var iw = coverArtImage.implicitWidth
            var ih = coverArtImage.implicitHeight
            return (iw > 0 && ih > 0) ? (ih / iw) : 0.62
        }
        implicitWidth: visible
            ? Math.round(Math.min(availableWidth, maxEdge / artRatio)) : 0
        implicitHeight: visible
            ? Math.round(Math.min(availableWidth * artRatio, maxEdge)) : 0
        width: implicitWidth
        height: implicitHeight
        radius: AppTheme.radiusMd
        color: AppTheme.embedSurface
        border.width: 1
        border.color: AppTheme.border
        clip: true
        Image {
            id: coverArtImage
            objectName: "audioCoverArtwork"
            anchors.fill: parent
            source: coverArtBox.visible ? root.artworkSource : ""
            // Width only; constraining both would letterbox the source.
            sourceSize.width: 640
            // The box matches the aspect, so Fit shows the whole cover.
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            Accessible.name: qsTr("Audio cover artwork")
        }
    }

    RowLayout {
        id: cardRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        spacing: 8

        IconButton {
            objectName: "audioPlayPauseButton"
            fill: true
            // Click focus, so Space after Play toggles the clip.
            focusPolicy: Qt.StrongFocus
            implicitWidth: 30; implicitHeight: 30
            // Always a play glyph when idle (a mic read as "record"); the voice
            // label and waveform identify voice messages.
            iconName: root.fetchState === "fetching"
                      ? "schedule"
                      : (root.playing ? "pause" : "play_arrow")
            iconSize: 17
            enabled: root.fetchState !== "fetching"
            readonly property string actionLabel: root.playing
                ? qsTr("Pause %1").arg(root.isVoice
                                       ? qsTr("voice message") : root.filename)
                : qsTr("Play %1").arg(root.isVoice
                                      ? qsTr("voice message") : root.filename)
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.togglePlay()
        }


        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.fetchState === "failed"
                      ? qsTr("This audio cannot be played")
                      : root.isVoice ? qsTr("Voice message")
                                     : (root.filename || qsTr("Audio"))
                color: root.fetchState === "failed"
                       ? AppTheme.danger : AppTheme.text
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
                elide: Label.ElideMiddle
                Layout.fillWidth: true
            }

            // Seek surface: real waveform bars when MSC3245 data exists,
            // otherwise a slim progress slider. Both are seekable.
            Item {
                Layout.fillWidth: true
                implicitHeight: 18
                visible: root.fetchState !== "failed"

                Row {
                    id: waveRow
                    objectName: "audioWaveRow"
                    anchors.fill: parent
                    visible: root.isVoice && root.waveform
                             && root.waveform.length > 0
                    spacing: 1
                    property int barCount: Math.max(
                        1, Math.min(root.waveform ? root.waveform.length : 0,
                                    Math.floor(width / 3)))
                    Repeater {
                        model: waveRow.visible ? waveRow.barCount : 0
                        delegate: Rectangle {
                            required property int index
                            // Buckets are 0..=100 (rust/src/timeline.rs
                            // downsample_waveform; RustTimelineIngest.cpp drops
                            // anything outside), so divide; min() guards a
                            // malformed bucket.
                            readonly property real amp: {
                                var wf = root.waveform
                                var at = Math.floor(
                                    index * wf.length / waveRow.barCount)
                                return Math.max(0.12,
                                                Math.min(1, wf[at] / 100))
                            }
                            readonly property real progress:
                                root.player && root.player.duration > 0
                                ? root.player.position / root.player.duration
                                : 0
                            width: 2
                            anchors.verticalCenter: parent.verticalCenter
                            height: parent.height * amp
                            radius: 1
                            color: (index / waveRow.barCount) <= progress
                                   ? AppTheme.accent : AppTheme.borderStrong
                        }
                    }
                    TapHandler {
                        enabled: root.player ? root.player.seekable : false
                        onTapped: (eventPoint) => {
                            if (!root.player) return
                            root.player.position = root.player.duration
                                * (eventPoint.position.x / waveRow.width)
                        }
                    }
                }

                Slider {
                    id: seekSlider
                    anchors.fill: parent
                    visible: !waveRow.visible
                    from: 0
                    to: Math.max(1, root.player && root.player.duration > 0
                                    ? root.player.duration : root.durationMs)
                    enabled: root.player ? root.player.seekable : false
                    value: pressed ? value
                                   : (root.player ? root.player.position : 0)
                    Accessible.name: qsTr("Seek position")
                    onMoved: if (root.player) root.player.position = value
                    // A slim track and 12px handle; Basic's 28px handle was
                    // clipped by the 18px row.
                    padding: 0
                    background: Rectangle {
                        x: seekSlider.leftPadding
                        y: seekSlider.topPadding
                            + seekSlider.availableHeight / 2 - height / 2
                        width: seekSlider.availableWidth
                        height: 4
                        radius: 2
                        color: AppTheme.borderStrong
                        Rectangle {
                            width: seekSlider.visualPosition * parent.width
                            height: parent.height
                            radius: parent.radius
                            color: AppTheme.accent
                        }
                    }
                    handle: Rectangle {
                        x: seekSlider.leftPadding
                           + seekSlider.visualPosition
                             * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding
                            + seekSlider.availableHeight / 2 - height / 2
                        width: 12
                        height: 12
                        radius: 6
                        color: AppTheme.accent
                        border.width: 2
                        border.color: AppTheme.surfaceElevated
                    }
                }
            }

            Label {
                text: {
                    var pos = root.formatPosition(root.player ? root.player.position
                                                        : 0)
                    var total = root.formatDuration(
                        root.player && root.player.duration > 0
                        ? root.player.duration : root.durationMs)
                    var line = root.ready ? pos + " / " + total : total
                    if (!root.isVoice && root.fileSize > 0) {
                        var kb = root.fileSize / 1024
                        line += " • " + (kb < 1024 ? kb.toFixed(0) + " KB"
                                        : (kb / 1024).toFixed(1) + " MB")
                    }
                    return line
                }
                // Elides inside the card.
                Layout.fillWidth: true
                elide: Label.ElideRight
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        // Playback speed: opens a list, and the choice is remembered
        // (app.settings.mediaPlaybackRate) across cards, rooms and restarts.
        AbstractButton {
            id: audioSpeedButton
            objectName: "audioSpeedButton"
            readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
            readonly property real rate: app.settings.mediaPlaybackRate
            visible: root.ready
            implicitWidth: 34; implicitHeight: 24
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(rate)
            ToolTip.text: qsTr("Playback speed")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: speedMenu.popup()
            AppMenu {
                id: speedMenu
                objectName: "audioSpeedMenu"
                menuWidth: 140
                Repeater {
                    model: audioSpeedButton.rates
                    AppMenuItem {
                        required property real modelData
                        text: modelData + "\u00d7"
                        iconName: Math.abs(modelData - audioSpeedButton.rate)
                                  < 0.001 ? "check" : ""
                        // Write the setting only: playbackRate is bound to it,
                        // and assigning the player too would break that
                        // binding.
                        onTriggered: app.settings.mediaPlaybackRate = modelData
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
                color: Math.abs(audioSpeedButton.rate - 1.0) < 0.001
                       ? AppTheme.textMuted : AppTheme.accent
                font.pixelSize: AppTheme.textMicro
                font.weight: AppTheme.weightBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        MediaVolumeControl {
            objectName: "audioMuteButton"
            audio: root.player ? root.player.audioOutput : null
            sliderObjectName: "audioVolumeSlider"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.ready
        }
        IconButton {
            objectName: "audioSaveButton"
            iconName: "download"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.canSave
            readonly property string actionLabel:
                qsTr("Save %1 as…").arg(root.filename || qsTr("audio"))
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.saveRequested()
        }
    }
}
