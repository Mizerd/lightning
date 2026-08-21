import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// v0.7: inline audio / voice-message player. A stable compact card:
// pressing Play fetches the decrypted payload through MediaBridge's
// validated playable materialization, then plays the session-scoped temp
// file in-process. Voice messages render their real MSC3245 waveform when
// the event carried one — never a fabricated decoration — and fall back
// to the plain progress slider otherwise. One audible card at a time via
// app.playback; room/account switches force a stop.
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
    // Normalized MSC3245 amplitudes (0..1) — empty when the event had none.
    property var waveform: []
    property bool rowOnScreen: true
    // Speculative prefetch gate, separate from rowOnScreen: a row that
    // merely swept past during a gesture must not pull a payload. Defaults
    // permissive so standalone hosts (fixtures) behave as before.
    property bool prefetchAllowed: true
    property bool canSave: false

    signal saveRequested()
    // Non-bridge backends (plain HTTP media) keep their external-open path;
    // the delegate wires this to app.media.openExternal.
    signal openExternalRequested()

    // The player backend loads ON DEMAND (first Play press) — an audio- or
    // voice-heavy room must not hold one QMediaPlayer per visible row.
    property bool engaged: false
    readonly property var player: engine.item
    readonly property bool playing:
        player ? player.playbackState === MediaPlayer.PlayingState : false
    readonly property bool ready:
        player ? player.source.toString().length > 0 : false
    property string fetchState: "idle" // idle / fetching / failed
    // Stable failure identity: MediaBridge marks/signals by this cache key.
    readonly property string fetchCacheKey: "full:" + mediaKey
    // The media key whose materialized file this card has PINNED against
    // LRU eviction (a live player holds the file open). Recorded at pin
    // time so delegate reuse — which changes mediaKey before resetPlayback
    // runs — still unpins the right key.
    property string pinnedKey: ""
    // Playback position preserved across an offscreen engine unload; the
    // next Play resumes here instead of restarting the track.
    property real resumePositionMs: 0
    // Qt Multimedia exposes embedded cover/thumbnail metadata after the
    // backend has opened the session-scoped playable file. MediaBridge keeps
    // the decoded pixels in a small RAM-only LRU and gives Image a provider
    // URL; no decrypted artwork is written to CacheStore or another file.
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

    function formatMs(ms) {
        if (!ms || ms < 0) ms = 0
        var total = Math.floor(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
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
            // Explicit user retry: clear the (possibly permanent) failure
            // mark first — playableSource is otherwise blocked by it and
            // the card would wait forever for a dispatch that never ran.
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
    // The media key with a bridge fetch outstanding on this card's behalf;
    // reset/destruction cancels it so an abandoned download stops consuming
    // bandwidth and pipeline slots.
    property string fetchingKey: ""
    function cancelFetch() {
        if (fetchingKey.length === 0)
            return
        app.mediaBridge.cancelPlayable(fetchingKey)
        fetchingKey = ""
    }
    function pinFile() {
        // The player now holds the materialized temp file open; the LRU
        // must not delete it underneath (seek/replay would fail).
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
    // Bounded speculative prefetch (size-capped, lowest priority, deduped
    // by MediaBridge) so Play starts from the materialized file instead of
    // a download wait. Voice messages and short tracks fit the cap.
    function maybePrefetch() {
        // Same user preference as GIF autoplay: "never" means no passive
        // downloads of any media class.
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
    // Offscreen resource release: a paused, scrolled-away card frees its
    // decoder and audio backend after a grace period instead of holding a
    // QMediaPlayer (and an open PulseAudio/PipeWire stream) for the rest of
    // the room session. The position survives; the next Play resumes it
    // from the still-materialized (reused) temp file.
    Timer {
        interval: 45000
        running: root.engaged && !root.rowOnScreen && !root.playing
        onTriggered: {
            root.resumePositionMs = root.player ? root.player.position : 0
            root.resetPlayback()
        }
    }
    // A forced stop (room/account switch, sign-out) drops the source and
    // unloads the engine — the decrypted temp file is about to be wiped.
    readonly property int stopGen: app.playback.stopGeneration
    onStopGenChanged: resetPlayback()

    Connections {
        target: app.mediaBridge
        // Both handlers filter on THIS card's cache key — an unrelated
        // avatar/thumbnail failure elsewhere must not flip this fetch.
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
        // Space toggles whatever is currently audible (2026-08-18 tester
        // report "tarpas neveikia pause ir unpause"). The owner key is
        // re-checked here: a card that lost audibility between the key press
        // and this delivery must not react.
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
                // 2026-08-18 tester report ("neatsimena audio preferencu
                // uzdeda default visada"): the remembered level, not a fixed
                // 0.8 every time. This is a live binding, so changing the
                // volume on one card moves every other card with it; the
                // slider's own direct write breaks the binding on THAT card
                // only, to the same value it just stored.
                volume: app.settings.mediaVolume
            }
            // The remembered speed applies to every card, including one
            // opened long after the choice was made.
            playbackRate: app.settings.mediaPlaybackRate
            onErrorOccurred: root.fetchState = "failed"
            onMetaDataChanged: root.refreshArtwork()
            // Resume after an offscreen engine unload: seek once the media
            // is actually loaded — a seek issued straight after setting the
            // source would be dropped.
            onMediaStatusChanged: {
                if (mediaStatus === MediaPlayer.LoadedMedia
                    && root.resumePositionMs > 0) {
                    position = root.resumePositionMs
                    root.resumePositionMs = 0
                }
            }
        }
    }

    implicitWidth: Math.min(360, bubble ? bubble.width : 360)
    implicitHeight: cardRow.implicitHeight + 12
                    + (coverArtBox.visible
                       ? coverArtBox.implicitHeight + 6 : 0)
    color: AppTheme.embedSurface
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: 1
    // The delegate provides `bubble`; standalone use (tests) tolerates null.
    property var bubble: null

    // Embedded cover art, rendered as its own box ATTACHED BELOW the player
    // controls rather than as a thumbnail beside them — the shape a chat
    // client uses for an embed under a message. Square-ish, corner-matched
    // to the card, and clipped so a non-square image cannot spill.
    //
    // Still RAM-only: MediaBridge::audioArtworkSource hands back a
    // provider URL backed by a bounded decoded-image cache, and nothing
    // about moving it in the layout writes artwork to disk.
    Rectangle {
        id: coverArtBox
        objectName: "audioCoverArtBox"
        visible: root.artworkSource.length > 0
        anchors.left: parent.left
        anchors.top: cardRow.bottom
        anchors.leftMargin: 6
        anchors.topMargin: 6
        // The box takes the ARTWORK'S OWN aspect, so a square album cover
        // renders square. A fixed ratio cropped the top and bottom off every
        // square cover, which is most of them.
        //
        // BOTH axes are bounded, and that is the point: capping only the
        // height (the first version of this) silently broke the aspect match
        // the box exists to provide. Any cover wider than the cap got a box
        // that was still full width but only `maxEdge` tall, and
        // PreserveAspectFit then painted the artwork small and centred with
        // dead card surface down either side. Deriving the width from the
        // capped height keeps box and artwork the same shape at every size.
        readonly property real maxEdge: 420
        readonly property real availableWidth: Math.max(1, root.width - 12)
        // Height per unit width. Falls back to a sane ratio until the image
        // reports its size; asynchronous loading means that is transient.
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
            // Width only: constraining both axes would letterbox the source
            // before the box has a chance to take its shape.
            sourceSize.width: 640
            // The box already matches the artwork's aspect, so Fit shows the
            // whole cover instead of cropping it.
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
            // Click focus (IconButton defaults to Tab-only): pressing Play
            // then pressing Space toggles the clip, which is what a desktop
            // player does, and it needs no global key grab to work.
            focusPolicy: Qt.StrongFocus
            implicitWidth: 30; implicitHeight: 30
            // Always a play glyph when idle — the accent-filled button with
            // a mic read as "record", not "play" (maintainer feedback
            // 2026-08-12); the voice identity is already carried by the
            // "Voice message" label and the waveform.
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
                            readonly property real amp: {
                                var wf = root.waveform
                                var at = Math.floor(
                                    index * wf.length / waveRow.barCount)
                                return Math.max(0.12, Math.min(1, wf[at]))
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
                    // 2026-08-18 tester report ("audio slider klipinasi
                    // biski"): the default Basic-style handle is 28px tall
                    // inside an 18px seek row, so its top and bottom were cut
                    // off. A slim track with a 12px handle fits the row it
                    // actually lives in.
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
                    var pos = root.formatMs(root.player ? root.player.position
                                                        : 0)
                    var total = root.formatMs(
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
                // Elides inside the card. Without this the position/duration
                // (plus the file size on a music file) simply ran past the
                // card's right edge and was cut mid-character.
                Layout.fillWidth: true
                elide: Label.ElideRight
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        // Playback speed. 2026-08-18 tester report ("kai keiti audio garso
        // greiti nera kaip grizti ... turi visa rata prasukti"): the button
        // used to CYCLE one way only, so overshooting 1x meant walking the
        // whole list around again. It now opens the list and the choice is
        // remembered (app.settings.mediaPlaybackRate), so it also survives
        // the next card, room and restart.
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
                        // Writing the SETTING is enough: playbackRate is
                        // bound to it above, so this card and every other one
                        // follow. Assigning the player directly as well would
                        // break that binding for this card.
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
