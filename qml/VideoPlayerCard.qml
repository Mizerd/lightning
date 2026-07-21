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
            volume: volumeSlider.value
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

        VideoOutput {
            id: output
            anchors.fill: parent
            anchors.bottomMargin: controls.height
            fillMode: VideoOutput.PreserveAspectFit
        }

        // Click video area toggles play/pause.
        TapHandler {
            enabled: root.ready && root.fetchState !== "failed"
            onTapped: player.playbackState === MediaPlayer.PlayingState
                      ? player.pause()
                      : (app.playback.acquire(root.ownerKey), player.play())
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

        // Control strip.
        Rectangle {
            id: controls
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 36
            color: AppTheme.surface
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing8
                anchors.rightMargin: AppTheme.spacing8
                spacing: AppTheme.spacing8
                IconButton {
                    objectName: "videoPlayPauseButton"
                    iconName: player.playbackState === MediaPlayer.PlayingState
                              ? "pause" : "play_arrow"
                    iconSize: 18
                    implicitWidth: 26; implicitHeight: 26
                    enabled: root.ready
                    Accessible.name:
                        player.playbackState === MediaPlayer.PlayingState
                        ? qsTr("Pause video") : qsTr("Play video")
                    onClicked: {
                        if (player.playbackState === MediaPlayer.PlayingState) {
                            player.pause()
                        } else {
                            app.playback.acquire(root.ownerKey)
                            player.play()
                        }
                    }
                }
                Label {
                    text: root.formatMs(player.position) + " / "
                          + root.formatMs(player.duration)
                    color: AppTheme.textMuted
                    font.pixelSize: 10
                }
                Slider {
                    id: seekSlider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, player.duration)
                    enabled: player.seekable
                    value: pressed ? value : player.position
                    Accessible.name: qsTr("Seek position")
                    onMoved: player.position = value
                }
                IconButton {
                    objectName: "videoMuteButton"
                    iconName: audioOut.muted ? "volume_off" : "volume_up"
                    iconSize: 16
                    implicitWidth: 26; implicitHeight: 26
                    Accessible.name: audioOut.muted
                                     ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: {
                        audioOut.userUnmuted = true
                        audioOut.muted = !audioOut.muted
                    }
                }
                Slider {
                    id: volumeSlider
                    Layout.preferredWidth: 56
                    from: 0; to: 1; value: 0.8
                    Accessible.name: qsTr("Volume")
                }
                // Playback speed: cycles the standard rates; resets to 1x
                // per playback session (never persisted).
                AbstractButton {
                    id: videoSpeedButton
                    objectName: "videoSpeedButton"
                    readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
                    property int rateIndex: 2
                    implicitWidth: 34; implicitHeight: 26
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Playback speed %1x")
                        .arg(rates[rateIndex])
                    onClicked: {
                        rateIndex = (rateIndex + 1) % rates.length
                        player.playbackRate = rates[rateIndex]
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: videoSpeedButton.hovered
                               ? AppTheme.hover : "transparent"
                        border.width: videoSpeedButton.visualFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                    }
                    contentItem: Label {
                        text: videoSpeedButton.rates[videoSpeedButton.rateIndex]
                              + "×"
                        color: videoSpeedButton.rateIndex === 2
                               ? AppTheme.textMuted : AppTheme.accent
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                IconButton {
                    objectName: "videoExpandButton"
                    iconName: "open_in_full"
                    iconSize: 16
                    implicitWidth: 26; implicitHeight: 26
                    Accessible.name: qsTr("Expand video")
                    ToolTip.text: qsTr("Expand video")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    // Hand the overlay the inline VIDEO output — it must be
                    // restored as player.videoOutput on close.
                    onClicked: videoOverlay.openFor(player, output)
                }
                IconButton {
                    objectName: "videoCloseButton"
                    iconName: "close"
                    iconSize: 16
                    implicitWidth: 26; implicitHeight: 26
                    Accessible.name: qsTr("Close player")
                    onClicked: {
                        root.resetPlayback()
                        root.closeRequested()
                    }
                }
            }
        }
    }

    VideoViewerOverlay {
        id: videoOverlay
    }
}
