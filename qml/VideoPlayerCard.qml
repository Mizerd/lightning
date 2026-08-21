import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// v0.7: inline video player surface. Created on explicit user intent (the
// delegate swaps its thumbnail cover for this card on Play), fetches the
// decrypted payload through MediaBridge's validated playable
// materialization, and plays the resulting session-scoped temp file with
// the in-process QMediaPlayer. Audibility is coordinated through
// app.playback (one audible card at a time; room/account switches force a
// stop). Geometry is owned by the parent — this item fills whatever the
// cover reserved, so starting playback never reflows the timeline.
Item {
    id: root
    objectName: "videoPlayerCard"

    // Stable identity + metadata from the delegate (model-bound).
    property string mediaKey: ""
    property string ownerKey: ""
    property string filename: ""
    property bool rowOnScreen: true
    // The card only exists because the user pressed Play on the cover, so
    // inline video starts AUDIBLE at a moderate volume — this is play-on-
    // explicit-intent, not autoplay. Callers that want a silent start (none
    // today) can set startMuted.
    property bool startMuted: false
    // The media key whose materialized file this card has PINNED against
    // LRU eviction; recorded at pin time so delegate reuse unpins the
    // right key (mediaKey changes before resetPlayback runs).
    property string pinnedKey: ""

    signal closeRequested()

    readonly property bool ready: player.source.toString().length > 0
    readonly property bool buffering:
        player.mediaStatus === MediaPlayer.LoadingMedia
        || player.mediaStatus === MediaPlayer.StalledMedia
        || (fetchState === "fetching")
    property string fetchState: "idle" // idle / fetching / failed

    function formatMs(ms) {
        if (!ms || ms < 0) ms = 0
        var total = Math.floor(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    // Stable failure identity: MediaBridge marks/signals by this cache key.
    readonly property string fetchCacheKey: "full:" + mediaKey

    // The media key with a bridge fetch outstanding on this card's behalf;
    // recorded so reset/destruction can cancel the backend download (an
    // abandoned fetch used to keep downloading, starving later media).
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
        // The player holds the materialized temp file open; the LRU must
        // not delete it underneath (seek or overlay reopen would fail).
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
    // Explicit user retry must clear the (possibly permanent) failure mark
    // first — playableSource is otherwise blocked by it and the card would
    // wait forever for a dispatch that never happened.
    function retryFetch() {
        app.mediaBridge.retry(fetchCacheKey)
        fetchState = "idle"
        start()
    }
    function resetPlayback() {
        // A forced reset invalidates the expanded view too — it borrows
        // this card's player and must never outlive its source.
        if (videoOverlay.opened)
            videoOverlay.close()
        player.stop()
        player.source = ""
        unpinFile()
        cancelFetch()
        fetchState = "idle"
        app.playback.release(root.ownerKey)
    }
    // Delegate reuse: a recycled card for another event must never keep
    // the previous event's position, source, or audibility.
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
    // Offscreen resource release: a video paused by scrolling away used to
    // keep its QMediaPlayer, decoder, GPU surfaces and open temp-file
    // handle alive for the rest of the room session — N started videos
    // meant N live decoders. After a grace period the card closes itself
    // exactly like the control bar's close button; the cover (poster +
    // play) returns and a fresh Play reuses the still-materialized file.
    Timer {
        interval: 90000
        // review L2: never while the expanded overlay is open — the
        // underlying ROW can leave the viewport (bottom-pinned appends)
        // while the user is watching full-screen, and reclaiming then
        // would close the overlay under them.
        running: !root.rowOnScreen && !videoOverlay.opened
                 && player.playbackState !== MediaPlayer.PlayingState
        onTriggered: {
            root.resetPlayback()
            root.closeRequested()
        }
    }

    Connections {
        target: app.mediaBridge
        // Both handlers filter on THIS card's cache key — an unrelated
        // avatar/thumbnail failure elsewhere must not flip this fetch.
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
    // One toggle, so the tap handler, the control bar and the Space key all
    // do the same thing (and all of them go through the audibility claim).
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
        // 2026-08-18: Space toggles whatever is audible. A video card holds
        // audibility exactly like an audio card does, so it has to answer
        // this too — without it the key would be swallowed by a player that
        // never reacts.
        function onTogglePlayPauseRequested(ownerKey) {
            if (ownerKey !== root.ownerKey || !root.ready)
                return
            root.togglePlayPause()
        }
    }
    // A forced stop (room/account switch, sign-out) must drop the source —
    // the decrypted temp file is about to be wiped; a paused player holding
    // an open handle would outlive it.
    readonly property int stopGen: app.playback.stopGeneration
    onStopGenChanged: resetPlayback()

    MediaPlayer {
        id: player
        videoOutput: output
        audioOutput: AudioOutput {
            id: audioOut
            muted: root.startMuted && !userUnmuted
            property bool userUnmuted: false
            volume: 0.8
        }
        onErrorOccurred: root.fetchState = "failed"
    }

    Rectangle {
        anchors.fill: parent
        // Black, not a theme surface: the VideoOutput aspect-FITS inside a
        // card whose width is floored by the control bar, so a portrait
        // video leaves side gutters — on a themed fill those read as
        // colored stripes glued to the video (maintainer screenshot,
        // 2026-08-12). Black is the universal letterbox and makes the
        // gutters read as part of the player, exactly like the expanded
        // overlay's scrim. This is one of the SANCTIONED literals: a
        // letterbox is not a themed surface and has no token, by design.
        color: "#000000"
        radius: AppTheme.radiusSm
        border.color: AppTheme.border
        border.width: 1
        clip: true

        // The video owns the whole card; controls overlay its lower edge
        // on a gradient scrim instead of consuming card height.
        VideoOutput {
            id: output
            anchors.fill: parent
            // Match the cover's edge-to-edge presentation: the card's width
            // is floored by the control bar, so a plain aspect-FIT leaves
            // visible letterbox bars for any small card/video mismatch
            // (maintainer screenshot, 2026-08-12) while the poster before it
            // filled by cropping. Fill by cropping whenever the shapes are
            // close (≤20% mismatch — a sliver off the edges, exactly what
            // the cover already cropped); genuinely different shapes (a
            // portrait video in a metadata-less 16:9 card) keep the honest
            // fit + black letterbox rather than amputating real content.
            // The expanded overlay always shows the uncropped frame.
            // ROTATION-TOLERANT: sourceRect reports the CODED frame size,
            // not the displayed one — a phone video encoded 1280x720 with a
            // 90-degree display matrix reports landscape while rendering
            // portrait (measured live on the maintainer's timeline), which
            // made the plain ratio comparison letterbox a video that
            // actually matched its card. The card's shape always comes from
            // display-truthful sources (Matrix thumbnail, extracted poster,
            // declared dimensions), so accept EITHER orientation of the
            // coded ratio; a genuine cross-shape still exceeds the bound in
            // both orientations and letterboxes honestly.
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

        // Tap toggles play/pause IMMEDIATELY; double-tap expands. NOT
        // exclusive signals: exclusivity makes Qt sit out the whole
        // double-click interval (~500 ms) before committing to the single
        // tap, which read as "pause is laggy" in live use (maintainer
        // feedback 2026-08-12). With plain taps the toggle is instant; a
        // double-tap toggles twice — net no state change — and then
        // expands, costing only a one-frame flicker.
        TapHandler {
            id: videoTap
            enabled: root.ready && root.fetchState !== "failed"
            onTapped: root.togglePlayPause()
            onDoubleTapped: videoOverlay.openFor(player, output)
        }

        // Basic's BusyIndicator inks palette.dark (the theme's secondary
        // TEXT colour), which on a black letterbox is barely visible.
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
                // On the black letterbox, not on a theme surface: a light
                // theme's textMuted is a mid grey that all but disappears
                // here.
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

        // Adaptive control bar over the video's lower edge. Visible while
        // paused/idle/failed, on hover, or with keyboard focus inside it;
        // auto-hides during undisturbed playback.
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
