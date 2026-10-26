import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Inline video player, created when the user presses Play on the cover. It
// fetches the decrypted payload through MediaBridge's validated playable
// materialization and plays the session-scoped temp file with QMediaPlayer.
// Audibility goes through app.playback (one audible card at a time; room or
// account switches force a stop). Fills the geometry the cover reserved, so
// playback never reflows the timeline.
//
// A forced stop is not a lost place: the position is remembered for the
// session (app.playback.rememberVideoPosition, by media key) and the next Play
// resumes there, as Telegram and Discord do. The expanded view stays open at
// the end with a Replay; inline, the end returns to the cover, which then
// offers Replay.
//
// A FocusScope, so `activeFocus` on the card says whether keyboard focus is
// anywhere inside it: the host reads that on close to hand focus back to the
// cover (a plain Item reports only its own focus, never a descendant's).
FocusScope {
    id: root
    objectName: "videoPlayerCard"
    Accessible.role: Accessible.Pane
    Accessible.name: filename.length > 0 ? qsTr("Video player: %1").arg(filename)
                                         : qsTr("Video player")

    // The player exists because the user pressed Play, so it takes keyboard
    // focus: a mouse click on the cover left focus on the timeline and Space
    // paged it instead of pausing. The key map runs here when no control
    // inside has focus. Only the keys that mean something to a player the
    // card itself holds: Up/Down/Home/F stay with the timeline, so keyboard
    // scrolling still works after a click on Play.
    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Space:
        case Qt.Key_K:
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_M:
            if (controls.handleKey(event))
                event.accepted = true
            break
        }
    }
    // Whether the card held focus when the expanded view opened.
    property bool hadFocusBeforeOverlay: false
    // Set when the card is created; taps in the first moments belong to the
    // click that created it (see videoTap).
    property double createdAt: 0

    // Stable identity and metadata from the delegate.
    property string mediaKey: ""
    property string ownerKey: ""
    property string filename: ""
    // For the expanded view's header: who sent it and when. Plain text.
    property string senderName: ""
    property var timestamp: null
    // The cover's picture, shown by the expanded view once the video ends
    // (some backends clear the frame at the end). Empty when there is none.
    property string posterSource: ""
    // Whether the expanded view offers Download (the host performs it).
    property bool canSave: false
    signal saveRequested()
    // The event's declared length (info.duration), 0 when absent; the bar
    // shows it until the player knows its own.
    property real declaredDurationMs: 0
    property bool rowOnScreen: true
    // Starts audible: the card exists only because the user pressed Play.
    property bool startMuted: false
    // The media key this card pinned against LRU eviction, recorded so reuse
    // unpins the right key (mediaKey changes before resetPlayback runs).
    property string pinnedKey: ""

    signal closeRequested()

    readonly property bool ready: player.source.toString().length > 0
    // A picture belongs on the card now: a source, not failed, not still
    // loading (the spinner has that). On Qt Quick's software renderer the
    // VideoOutput draws nothing, and SoftwareVideoNotice says why.
    readonly property bool pictureExpected:
        ready && fetchState !== "failed" && !buffering
    readonly property bool buffering:
        player.mediaStatus === MediaPlayer.LoadingMedia
        || player.mediaStatus === MediaPlayer.StalledMedia
        || (fetchState === "fetching")
    property string fetchState: "idle" // idle / fetching / failed
    // Which wait the spinner stands for (M-F07). The payload is fetched whole
    // before the player sees a local file, so the network download and the
    // decoder's buffering are different stages and say so; there is no byte
    // count to show, so both stay indeterminate. BufferingMedia (playing on
    // with data in hand) is deliberately not a wait.
    //   "downloading"  MediaBridge is still fetching the file
    //   "preparing"    the local file is being opened by the player
    //   "stalled"      the player ran out of data mid-playback
    //   ""             nothing to wait for
    readonly property string loadPhase:
        fetchState === "failed" ? ""
        : fetchState === "fetching" ? "downloading"
        : player.mediaStatus === MediaPlayer.LoadingMedia ? "preparing"
        : player.mediaStatus === MediaPlayer.StalledMedia ? "stalled"
        : ""

    // MediaBridge marks and signals failures by this cache key.
    readonly property string fetchCacheKey: "full:" + mediaKey

    // The key with a fetch outstanding for this card, so reset/destruction can
    // cancel the download.
    property string fetchingKey: ""
    // The media key the player's source belongs to, recorded when the source
    // is set: delegate reuse changes mediaKey BEFORE resetPlayback runs, and
    // the position must be remembered under the video it belongs to.
    property string sourceKey: ""
    // Where to resume once the player can seek; 0 when nothing is pending.
    property real pendingResumeMs: 0
    // Why the failure panel shows, from facts the pipeline knows (never a Qt
    // error string, which can carry a local path):
    //   ""            unknown: the generic message and Retry
    //   "audio"       the play gate gave up (sound system never ready)
    //   "network"     the download failed in a way a retry can fix
    //   "unavailable" the server no longer serves it (gone, forbidden, refused)
    //   "too_large"   over the playable-file limit: Download instead
    //   "format"      the bytes arrived but this device cannot decode them:
    //                 retrying the same bytes cannot help; Download instead
    property string failureReason: ""
    function reasonForCategory(category) {
        switch (category) {
        case "network": case "timeout": case "server_unreachable":
        case "server_error": case "rate_limited": case "request_failure":
            return "network"
        // The server has said the file does not exist. MediaBridge's own
        // "unavailable" is a transient local dispatch refusal (no client
        // during a restore or switch) and keeps the generic Retry.
        case "not_found":
            return "unavailable"
        case "too_large":
            return "too_large"
        // "rejected" on a playable fetch is MediaBridge refusing the BYTES
        // (an unknown container, a payload that sniffs as markup), not the
        // server refusing the file: live, a non-media "video" read "no longer
        // available" while its download had succeeded.
        case "rejected":
        case "unsupported_mime": case "invalid": case "unrecognized":
            return "format"
        }
        return ""
    }
    // Retrying is offered only where it can change the outcome.
    readonly property bool failureRetryable:
        failureReason !== "unavailable" && failureReason !== "format"
        && failureReason !== "too_large"
    readonly property bool failureOffersDownload:
        canSave && (failureReason === "format" || failureReason === "too_large")
    // The video reached its end while the expanded view was open: the view
    // stays, holding the poster and a Replay.
    property bool ended: false
    // Pulse-backend SIGABRT protection (pwcrash): with the `qtAudio` context
    // property, play only once Qt's PulseAudio connection is ready. A card or
    // bar loaded alone (tests) has no qtAudio and plays directly.
    function gatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.play(p)
        else p.play()
    }
    function cancelGatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.cancelPlay(p)
    }

    function start() {
        var url = app.mediaBridge.playableSource(root.mediaKey)
        if (url.length > 0) {
            fetchState = "idle"
            failureReason = ""
            fetchingKey = ""
            sourceKey = root.mediaKey
            pendingResumeMs = app.playback.videoResumePosition
                    ? app.playback.videoResumePosition(root.mediaKey) : 0
            player.source = url
            root.applySessionRate()
            pinFile()
            // The Play that created this card may be seconds old by now (a
            // slow fetch): a row the reader has since scrolled away from
            // gets a ready, paused card, never sound from off screen. The
            // expanded view is the exception, because then the overlay and
            // not the row is what the user is looking at.
            if (root.playbackVisible) {
                app.playback.acquire(root.ownerKey)
                root.gatedPlay(player)
            }
            root.applyPendingResume()
        } else if (app.mediaBridge.failureCategory(root.fetchCacheKey)
                   .length > 0) {
            // A failure mark means no fetch was dispatched (an earlier
            // failure is still in its retry interval) or it failed at once;
            // no signal will follow, so show the failure and its Retry.
            failureReason = reasonForCategory(
                        app.mediaBridge.failureCategory(root.fetchCacheKey))
            fetchState = "failed"
            fetchingKey = ""
        } else {
            fetchState = "fetching"
            fetchingKey = root.mediaKey
        }
    }
    function cancelFetch() {
        if (fetchingKey.length === 0)
            return
        app.mediaBridge.cancelPlayable(fetchingKey)
        fetchingKey = ""
    }
    function pinFile() {
        // The player holds the temp file open; the LRU must not delete it.
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
    // An explicit retry clears the failure mark first, or playableSource stays
    // blocked.
    function retryFetch() {
        failureReason = ""
        app.mediaBridge.retry(fetchCacheKey)
        fetchState = "idle"
        start()
    }
    // The session's video speed: a resumed or new player starts at the
    // speed last chosen, and the bars show the player's real rate.
    function applySessionRate() {
        var r = app.playback.videoPlaybackRate
        if (r !== undefined && r > 0 && player.playbackRate !== r)
            player.playbackRate = r
    }
    function applyPendingResume() {
        if (pendingResumeMs <= 0 || !player.seekable)
            return
        player.position = pendingResumeMs
        pendingResumeMs = 0
    }
    // Remember where this video stopped, under the key its source belongs
    // to. Nothing while a resume is still pending (the player never got
    // there, and the stored place stays), after the end (markVideoFinished
    // has spoken), or after a failure.
    function rememberPosition() {
        if (sourceKey.length === 0 || ended || pendingResumeMs > 0
                || fetchState === "failed" || !app.playback.rememberVideoPosition)
            return
        app.playback.rememberVideoPosition(sourceKey, player.position,
                                           player.duration)
    }
    function resetPlayback() {
        rememberPosition()
        // Cleared before the expanded view closes, so its closed handler
        // (which finishes an ENDED video inline) does not run a second reset
        // from inside this one.
        ended = false
        // A forced reset also closes the expanded view, which borrows this
        // player.
        if (videoOverlay.opened)
            videoOverlay.close()
        root.cancelGatedPlay(player)
        player.stop()
        player.source = ""
        sourceKey = ""
        pendingResumeMs = 0
        unpinFile()
        cancelFetch()
        fetchState = "idle"
        failureReason = ""
        app.playback.release(root.ownerKey)
    }
    // Reached at end of playback. Some backends clear the video sink once the
    // stream ends rather than holding the last decoded frame, which is the
    // literal black card this was reported as; nothing here can rely on
    // VideoOutput still showing anything. Inline, closing the player exactly
    // like the explicit close button reveals MessageDelegate's own cover —
    // thumbnail and, since the video is marked watched, a Replay glyph. In the
    // expanded view the view STAYS: it used to snap shut and drop the user
    // back in the timeline; now it holds the poster and a Replay, as every
    // mainstream player does. Its own name (not inlined into
    // onMediaStatusChanged) so a test can drive it directly: a real decoder
    // reaching EndOfMedia needs a real codec, which this test tree cannot
    // assume.
    function handleEndOfMedia() {
        if (sourceKey.length > 0 && app.playback.markVideoFinished)
            app.playback.markVideoFinished(sourceKey)
        ended = true
        if (videoOverlay.opened)
            return
        resetPlayback()
        closeRequested()
    }
    function replay() {
        ended = false
        app.playback.acquire(root.ownerKey)
        player.position = 0
        root.gatedPlay(player)
    }
    // Delegate reuse: never keep the previous event's position, source or
    // audibility.
    onMediaKeyChanged: resetPlayback()
    // Whether anything showing this player is on screen: the row inline, or
    // the expanded view, which keeps its own visibility while the row under
    // it scrolls, reflows or leaves the activation band.
    readonly property bool playbackVisible: rowOnScreen || videoOverlay.opened
    onRowOnScreenChanged: {
        if (rowOnScreen || videoOverlay.opened)
            return
        // A play still waiting on the gate must not start off screen either.
        root.cancelGatedPlay(player)
        if (player.playbackState === MediaPlayer.PlayingState)
            player.pause()
    }
    Component.onCompleted: {
        createdAt = Date.now()
        start()
        root.forceActiveFocus(Qt.MouseFocusReason)
    }
    Component.onDestruction: {
        rememberPosition()
        cancelGatedPlay(player)
        unpinFile()
        cancelFetch()
        app.playback.release(root.ownerKey)
    }
    // After a grace period off screen, close like the control bar's close
    // button so the decoder, GPU surfaces and file handle are released. The
    // cover returns; Play reuses the materialized file.
    Timer {
        interval: 90000
        // Never while the expanded overlay is open: the row can leave the
        // viewport while the user watches full-screen.
        running: !root.rowOnScreen && !videoOverlay.opened
                 && player.playbackState !== MediaPlayer.PlayingState
        onTriggered: {
            root.resetPlayback()
            root.closeRequested()
        }
    }

    Connections {
        target: app.mediaBridge
        // Filter on this card's cache key only.
        function onPlayableMediaReady(cacheKey) {
            if (cacheKey === root.fetchCacheKey
                && root.fetchState === "fetching")
                root.start()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === root.fetchCacheKey
                && root.fetchState === "fetching") {
                root.failureReason = root.reasonForCategory(category)
                root.fetchState = "failed"
                root.fetchingKey = "" // the fetch is over; nothing to cancel
            }
        }
    }
    // One toggle for tap, control bar and Space key, all through the audibility
    // claim.
    // Keyboard focus into the controls (a key opened the player: the key map
    // must work at once, and the cover that had focus is gone).
    function focusControls() {
        controls.focusPlayButton()
    }
    function togglePlayPause() {
        if (player.playbackState === MediaPlayer.PlayingState) {
            root.cancelGatedPlay(player)
            player.pause()
            return
        }
        app.playback.acquire(root.ownerKey)
        root.gatedPlay(player)
    }

    Connections {
        target: app.playback
        function onAudibleOwnerChanged() {
            // Also a play still waiting on the gate: it lost audibility.
            if (!app.playback.owns(root.ownerKey)) {
                root.cancelGatedPlay(player)
                if (player.playbackState === MediaPlayer.PlayingState)
                    player.pause()
            }
        }
        // Space toggles whatever is audible, which includes video cards.
        function onTogglePlayPauseRequested(ownerKey) {
            if (ownerKey !== root.ownerKey || !root.ready)
                return
            if (root.ended) {
                root.replay()
                return
            }
            root.togglePlayPause()
        }
    }
    // The gate gave up on a play (the audio backend never became ready):
    // show the failure and its Retry rather than a card that never starts.
    Connections {
        target: typeof qtAudio !== "undefined" ? qtAudio : null
        function onPlayGivenUp(p) {
            if (p !== player)
                return
            root.failureReason = "audio"
            root.fetchState = "failed"
        }
    }
    // A forced stop (room/account switch, sign-out) drops the source: the temp
    // file is about to be wiped.
    readonly property int stopGen: app.playback.stopGeneration
    onStopGenChanged: resetPlayback()

    MediaPlayer {
        id: player
        videoOutput: output
        audioOutput: AudioOutput {
            id: audioOut
            muted: root.startMuted && !userUnmuted
            property bool userUnmuted: false
            // The remembered level (as AudioPlayerCard and VoicePreviewBar). A
            // live binding, so all cards follow; the slider's own write breaks
            // it only on this card, to the value it just stored.
            volume: app.settings.mediaVolume
        }
        // The file is local by now, so a player error is about decoding it
        // (format, codec, a broken file), not about the download.
        onErrorOccurred: {
            if (root.failureReason === "")
                root.failureReason = "format"
            root.fetchState = "failed"
        }
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.EndOfMedia)
                root.handleEndOfMedia()
            else if (mediaStatus === MediaPlayer.LoadedMedia
                     || mediaStatus === MediaPlayer.BufferedMedia)
                root.applyPendingResume()
        }
        onSeekableChanged: root.applyPendingResume()
        onPlaybackStateChanged: {
            if (playbackState === MediaPlayer.PlayingState)
                root.ended = false
        }
    }

    Rectangle {
        anchors.fill: parent
        // Black, not a theme surface: the universal letterbox, so aspect-fit
        // gutters read as part of the player. A sanctioned literal with no
        // token.
        color: "#000000"
        radius: AppTheme.radiusSm
        border.color: AppTheme.border
        border.width: 1
        clip: true

        // The video fills the card; controls overlay its lower edge on a scrim.
        VideoOutput {
            id: output
            anchors.fill: parent
            // Crop to fill when the card and video shapes are within 20% (as
            // the cover did); otherwise fit with a black letterbox rather than
            // cut real content. sourceRect reports the coded frame size, which
            // ignores rotation metadata, so accept either orientation of the
            // ratio. The expanded overlay always shows the uncropped frame.
            readonly property real videoRatio:
                sourceRect.height > 0 && sourceRect.width > 0
                ? sourceRect.width / sourceRect.height : 0
            readonly property real cardRatio:
                height > 0 ? width / height : 0
            function ratioMismatch(a, b) {
                return a > 0 && b > 0
                       ? Math.max(a, b) / Math.min(a, b) : 999
            }
            readonly property real mismatch:
                Math.min(ratioMismatch(videoRatio, cardRatio),
                         ratioMismatch(videoRatio > 0 ? 1 / videoRatio : 0,
                                       cardRatio))
            fillMode: mismatch <= 1.2 ? VideoOutput.PreserveAspectCrop
                                      : VideoOutput.PreserveAspectFit
        }

        // Why the output above stays black on the software renderer. Before
        // the controls, so they stay on top of it.
        SoftwareVideoNotice {
            objectName: "videoCardSoftwareNotice"
            anchors.fill: output
            shown: root.pictureExpected
        }

        HoverHandler { id: cardHover }

        // Tap toggles immediately; double-tap expands. Not exclusive:
        // exclusivity makes Qt wait the double-click interval before the single
        // tap. A double tap toggles twice (no net change) and then expands.
        TapHandler {
            id: videoTap
            enabled: root.ready && root.fetchState !== "failed"
            onTapped: {
                // The second click of a double-click on the cover lands on
                // the card that the first one created, and paused it before
                // its first frame: a black card at 0:00.
                // A heuristic guard, not a cause fix: it keys on the
                // system's double-click interval.
                if (Date.now() - root.createdAt
                        < Qt.styleHints.mouseDoubleClickInterval)
                    return
                root.forceActiveFocus(Qt.MouseFocusReason)
                root.togglePlayPause()
            }
            onDoubleTapped: videoOverlay.openFor(player, output)
        }

        // Basic's BusyIndicator uses a text colour barely visible on black.
        Item {
            id: cardSpinner
            objectName: "videoCardSpinner"
            anchors.centerIn: output
            implicitWidth: 28
            implicitHeight: 28
            width: implicitWidth
            height: implicitHeight
            visible: root.buffering && root.fetchState !== "failed"
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "transparent"
                border.width: 3
                border.color: AppTheme.scrimSurfaceRaised
            }
            Item {
                anchors.fill: parent
                transformOrigin: Item.Center
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: AppTheme.scrimInk
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: -2.5
                }
                RotationAnimator on rotation {
                    running: cardSpinner.visible && !AppTheme.reducedMotion
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                }
            }
        }
        // Says which wait the spinner is, under it, on the letterbox.
        Label {
            objectName: "videoLoadPhaseLabel"
            anchors.top: cardSpinner.bottom
            anchors.topMargin: AppTheme.spacing6
            anchors.horizontalCenter: cardSpinner.horizontalCenter
            visible: cardSpinner.visible && text.length > 0
            text: root.loadPhase === "downloading" ? qsTr("Downloading…")
                  : root.loadPhase === "preparing" ? qsTr("Preparing…")
                  : root.loadPhase === "stalled" ? qsTr("Buffering…")
                  : ""
            color: AppTheme.scrimInkMuted
            font.pixelSize: AppTheme.textMeta
        }
        ColumnLayout {
            objectName: "videoFailedPanel"
            anchors.centerIn: output
            visible: root.fetchState === "failed"
            spacing: AppTheme.spacing8
            Label {
                objectName: "videoFailedText"
                text: {
                    switch (root.failureReason) {
                    case "audio":
                        return qsTr("The sound system is not ready, so the video cannot start yet")
                    case "network":
                        return qsTr("The video could not be downloaded. Check the connection and try again.")
                    case "unavailable":
                        return qsTr("This video is no longer available")
                    case "too_large":
                        return qsTr("This video is too large to play here")
                    case "format":
                        return qsTr("This video's format cannot be played on this device")
                    }
                    return qsTr("This video cannot be played")
                }
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.maximumWidth: Math.max(80, root.width - 24)
                // On the black letterbox, not a theme surface.
                color: AppTheme.scrimInkMuted
                font.pixelSize: AppTheme.textMeta
                Layout.alignment: Qt.AlignHCenter
            }
            AppButton {
                objectName: "videoRetryButton"
                text: qsTr("Retry")
                visible: root.failureRetryable
                Layout.alignment: Qt.AlignHCenter
                onClicked: root.retryFetch()
            }
            AppButton {
                objectName: "videoFailedDownloadButton"
                text: qsTr("Download")
                visible: root.failureOffersDownload
                Layout.alignment: Qt.AlignHCenter
                onClicked: root.saveRequested()
            }
        }

        // Control bar over the lower edge: visible while paused/idle/failed, on
        // hover, or with focus inside; hides during undisturbed playback.
        VideoControlBar {
            id: controls
            objectName: "videoControlBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            player: player
            audio: audioOut
            ownerKey: root.ownerKey
            declaredDurationMs: root.declaredDurationMs
            expandIcon: "open_in_full"
            onExpandRequested: videoOverlay.openFor(player, output)
            onCloseRequested: {
                root.resetPlayback()
                root.closeRequested()
            }
            // An open speed menu or volume popup counts as engagement: the
            // menu takes the hover, and the bar faded out from under it.
            readonly property bool shown:
                player.playbackState !== MediaPlayer.PlayingState
                || cardHover.hovered || controls.activeFocus
                || controls.menuOpen || cornerClose.activeFocus
            visible: opacity > 0
            opacity: shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 160 }
            }
        }

        // A compact bar (narrow, e.g. a portrait video) gives its close
        // button up so the seek track keeps its width; it lives in the
        // card's top-right corner, shown together with the bar.
        VideoControlBar.ScrimIconButton {
            id: cornerClose
            objectName: "videoCardCloseButton"
            anchors.top: parent.top
            // The top-LEFT corner: the row's hover action bar is anchored to
            // the row's top-right and covered a close button there on a
            // narrow card.
            anchors.left: parent.left
            anchors.margins: AppTheme.spacing6
            visible: controls.compact && controls.visible
            opacity: controls.opacity
            iconName: "close"
            iconSize: 16
            implicitWidth: 28; implicitHeight: 28
            radius: 14
            restingColor: AppTheme.overlayScrim
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("Close player")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: {
                root.resetPlayback()
                root.closeRequested()
            }
        }
    }

    VideoViewerOverlay {
        id: videoOverlay
        declaredDurationMs: root.declaredDurationMs
        title: root.filename
        senderName: root.senderName
        timestamp: root.timestamp
        canSave: root.canSave
        mediaKey: root.mediaKey
        posterSource: root.posterSource
        ended: root.ended
        onSaveRequested: root.saveRequested()
        onReplayRequested: root.replay()
    }
    // Closing the expanded view after the end finishes inline, as a video
    // that ended inline does: back to the cover, which offers Replay.
    Connections {
        target: videoOverlay
        function onAboutToShow() {
            root.hadFocusBeforeOverlay = root.activeFocus
        }
        function onClosed() {
            // Focus back on the card, only if it held it before.
            if (root.hadFocusBeforeOverlay)
                root.forceActiveFocus(Qt.TabFocusReason)
            // The row left the screen while the expanded view held the
            // playback: closing the view hands it back to a row nobody can
            // see, so it pauses as it would have had the view never opened.
            if (!root.ended && !root.rowOnScreen) {
                root.cancelGatedPlay(player)
                if (player.playbackState === MediaPlayer.PlayingState)
                    player.pause()
            }
            if (!root.ended)
                return
            root.resetPlayback()
            root.closeRequested()
        }
    }
}
