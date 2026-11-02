import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

FocusScope {
    id: root
    objectName: "videoPlayerCard"
    Accessible.role: Accessible.Pane
    Accessible.name: filename.length > 0 ? qsTr("Video player: %1").arg(filename)
                                         : qsTr("Video player")

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
    property bool hadFocusBeforeOverlay: false
    property double createdAt: 0

    property string mediaKey: ""
    property string ownerKey: ""
    property string filename: ""
    property string senderName: ""
    property var timestamp: null
    property string posterSource: ""
    property bool canSave: false
    signal saveRequested()
    property real declaredDurationMs: 0
    property bool rowOnScreen: true
    property bool startMuted: false
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
    property string fetchState: "idle"
    readonly property string loadPhase:
        fetchState === "failed" ? ""
        : fetchState === "fetching" ? "downloading"
        : player.mediaStatus === MediaPlayer.LoadingMedia ? "preparing"
        : player.mediaStatus === MediaPlayer.StalledMedia ? "stalled"
        : ""

    readonly property string fetchCacheKey: "full:" + mediaKey

    property string fetchingKey: ""
    property string sourceKey: ""
    property real pendingResumeMs: 0
    property string failureReason: ""
    function reasonForCategory(category) {
        switch (category) {
        case "network": case "timeout": case "server_unreachable":
        case "server_error": case "rate_limited": case "request_failure":
            return "network"
        case "not_found":
            return "unavailable"
        case "too_large":
            return "too_large"
        case "rejected":
        case "unsupported_mime": case "invalid": case "unrecognized":
            return "format"
        }
        return ""
    }
    readonly property bool failureRetryable:
        failureReason !== "unavailable" && failureReason !== "format"
        && failureReason !== "too_large"
    readonly property bool failureOffersDownload:
        canSave && (failureReason === "format" || failureReason === "too_large")
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
            if (root.playbackVisible) {
                app.playback.acquire(root.ownerKey)
                root.gatedPlay(player)
            }
            root.applyPendingResume()
        } else if (app.mediaBridge.failureCategory(root.fetchCacheKey)
                   .length > 0) {
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
    function retryFetch() {
        failureReason = ""
        app.mediaBridge.retry(fetchCacheKey)
        fetchState = "idle"
        start()
    }
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
    function rememberPosition() {
        if (sourceKey.length === 0 || ended || pendingResumeMs > 0
                || fetchState === "failed" || !app.playback.rememberVideoPosition)
            return
        app.playback.rememberVideoPosition(sourceKey, player.position,
                                           player.duration)
    }
    function resetPlayback() {
        rememberPosition()
        ended = false
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
    onMediaKeyChanged: resetPlayback()
    readonly property bool playbackVisible: rowOnScreen || videoOverlay.opened
    onRowOnScreenChanged: {
        if (rowOnScreen || videoOverlay.opened)
            return
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
    Timer {
        interval: 90000
        running: !root.rowOnScreen && !videoOverlay.opened
                 && player.playbackState !== MediaPlayer.PlayingState
        onTriggered: {
            root.resetPlayback()
            root.closeRequested()
        }
    }

    Connections {
        target: app.mediaBridge
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
                root.fetchingKey = ""
            }
        }
    }
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
            if (!app.playback.owns(root.ownerKey)) {
                root.cancelGatedPlay(player)
                if (player.playbackState === MediaPlayer.PlayingState)
                    player.pause()
            }
        }
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
    Connections {
        target: typeof qtAudio !== "undefined" ? qtAudio : null
        function onPlayGivenUp(p) {
            if (p !== player)
                return
            root.failureReason = "audio"
            root.fetchState = "failed"
        }
    }
    readonly property int stopGen: app.playback.stopGeneration
    onStopGenChanged: resetPlayback()

    MediaPlayer {
        id: player
        videoOutput: output
        audioOutput: AudioOutput {
            id: audioOut
            muted: root.startMuted && !userUnmuted
            property bool userUnmuted: false
            volume: app.settings.mediaVolume
        }
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
        color: "#000000"
        radius: AppTheme.radiusSm
        border.color: AppTheme.border
        border.width: 1
        clip: true

        VideoOutput {
            id: output
            anchors.fill: parent
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

        SoftwareVideoNotice {
            objectName: "videoCardSoftwareNotice"
            anchors.fill: output
            shown: root.pictureExpected
        }

        HoverHandler { id: cardHover }

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

        VideoControlBar.ScrimIconButton {
            id: cornerClose
            objectName: "videoCardCloseButton"
            anchors.top: parent.top
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
    Connections {
        target: videoOverlay
        function onAboutToShow() {
            root.hadFocusBeforeOverlay = root.activeFocus
        }
        function onClosed() {
            if (root.hadFocusBeforeOverlay)
                root.forceActiveFocus(Qt.TabFocusReason)
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
