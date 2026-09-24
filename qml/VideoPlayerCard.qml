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
Item {
    id: root
    objectName: "videoPlayerCard"

    // Stable identity and metadata from the delegate.
    property string mediaKey: ""
    property string ownerKey: ""
    property string filename: ""
    property bool rowOnScreen: true
    // Starts audible: the card exists only because the user pressed Play.
    property bool startMuted: false
    // The media key this card pinned against LRU eviction, recorded so reuse
    // unpins the right key (mediaKey changes before resetPlayback runs).
    property string pinnedKey: ""

    signal closeRequested()

    readonly property bool ready: player.source.toString().length > 0
    readonly property bool buffering:
        player.mediaStatus === MediaPlayer.LoadingMedia
        || player.mediaStatus === MediaPlayer.StalledMedia
        || (fetchState === "fetching")
    property string fetchState: "idle" // idle / fetching / failed

    // MediaBridge marks and signals failures by this cache key.
    readonly property string fetchCacheKey: "full:" + mediaKey

    // The key with a fetch outstanding for this card, so reset/destruction can
    // cancel the download.
    property string fetchingKey: ""

    function start() {
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
        app.mediaBridge.retry(fetchCacheKey)
        fetchState = "idle"
        start()
    }
    function resetPlayback() {
        // A forced reset also closes the expanded view, which borrows this
        // player.
        if (videoOverlay.opened)
            videoOverlay.close()
        player.stop()
        player.source = ""
        unpinFile()
        cancelFetch()
        fetchState = "idle"
        app.playback.release(root.ownerKey)
    }
    // Delegate reuse: never keep the previous event's position, source or
    // audibility.
    onMediaKeyChanged: resetPlayback()
    onRowOnScreenChanged: {
        if (!rowOnScreen && player.playbackState === MediaPlayer.PlayingState)
            player.pause()
    }
    Component.onCompleted: start()
    Component.onDestruction: {
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
                root.fetchState = "failed"
                root.fetchingKey = "" // the fetch is over; nothing to cancel
            }
        }
    }
    // One toggle for tap, control bar and Space key, all through the audibility
    // claim.
    function togglePlayPause() {
        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause()
            return
        }
        app.playback.acquire(root.ownerKey)
        player.play()
    }

    Connections {
        target: app.playback
        function onAudibleOwnerChanged() {
            if (!app.playback.owns(root.ownerKey)
                && player.playbackState === MediaPlayer.PlayingState)
                player.pause()
        }
        // Space toggles whatever is audible, which includes video cards.
        function onTogglePlayPauseRequested(ownerKey) {
            if (ownerKey !== root.ownerKey || !root.ready)
                return
            root.togglePlayPause()
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
        onErrorOccurred: root.fetchState = "failed"
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

        HoverHandler { id: cardHover }

        // Tap toggles immediately; double-tap expands. Not exclusive:
        // exclusivity makes Qt wait the double-click interval before the single
        // tap. A double tap toggles twice (no net change) and then expands.
        TapHandler {
            id: videoTap
            enabled: root.ready && root.fetchState !== "failed"
            onTapped: root.togglePlayPause()
            onDoubleTapped: videoOverlay.openFor(player, output)
        }

        // Basic's BusyIndicator uses a text colour barely visible on black.
        Item {
            id: cardSpinner
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
        ColumnLayout {
            anchors.centerIn: output
            visible: root.fetchState === "failed"
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("This video cannot be played")
                // On the black letterbox, not a theme surface.
                color: AppTheme.scrimInkMuted
                font.pixelSize: AppTheme.textMeta
                Layout.alignment: Qt.AlignHCenter
            }
            AppButton {
                text: qsTr("Retry")
                Layout.alignment: Qt.AlignHCenter
                onClicked: root.retryFetch()
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
            expandIcon: "open_in_full"
            onExpandRequested: videoOverlay.openFor(player, output)
            onCloseRequested: {
                root.resetPlayback()
                root.closeRequested()
            }
            readonly property bool shown:
                player.playbackState !== MediaPlayer.PlayingState
                || cardHover.hovered || controls.activeFocus
            visible: opacity > 0
            opacity: shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 160 }
            }
        }
    }

    VideoViewerOverlay {
        id: videoOverlay
    }
}
