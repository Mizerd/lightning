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
    // Voice/audio never autoplay with sound; video starts muted unless the
    // user raises the volume — this satisfies the no-autoplay-with-sound
    // rule while still starting on the user's explicit Play press.
    property bool startMuted: false

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

    function start() {
        var url = app.mediaBridge.playableSource(root.mediaKey)
        if (url.length > 0) {
            fetchState = "idle"
            player.source = url
            app.playback.acquire(root.ownerKey)
            player.play()
        } else {
            fetchState = "fetching"
        }
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
    Component.onDestruction: app.playback.release(root.ownerKey)

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
                && root.fetchState === "fetching")
                root.fetchState = "failed"
        }
    }
    Connections {
        target: app.playback
        function onAudibleOwnerChanged() {
            if (!app.playback.owns(root.ownerKey)
                && player.playbackState === MediaPlayer.PlayingState)
                player.pause()
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
        color: AppTheme.surfaceElevated
        radius: AppTheme.radiusSm
        border.color: AppTheme.border
        border.width: 1
        clip: true

        // The video owns the whole card; controls overlay its lower edge
        // on a gradient scrim instead of consuming card height.
        VideoOutput {
            id: output
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }

        HoverHandler { id: cardHover }

        // Tap toggles play/pause; double-tap expands (matching the
        // expanded view, where double-tap exits). Exclusive signals so a
        // double-tap does not first flip the play state as a side effect.
        TapHandler {
            id: videoTap
            enabled: root.ready && root.fetchState !== "failed"
            exclusiveSignals: TapHandler.SingleTap | TapHandler.DoubleTap
            onSingleTapped: {
                player.playbackState === MediaPlayer.PlayingState
                    ? player.pause()
                    : (app.playback.acquire(root.ownerKey), player.play())
            }
            onDoubleTapped: videoOverlay.openFor(player, output)
        }

        BusyIndicator {
            anchors.centerIn: output
            running: root.buffering && root.fetchState !== "failed"
            visible: running
        }
        ColumnLayout {
            anchors.centerIn: output
            visible: root.fetchState === "failed"
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("This video cannot be played")
                color: AppTheme.textMuted
                font.pixelSize: 12
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
