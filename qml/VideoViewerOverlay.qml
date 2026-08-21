import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// v0.7: expanded in-application video view. Deliberately NOT an OS
// fullscreen window: a modal overlay inside the application window (the
// ImageViewerOverlay pattern), so account/room context and input focus
// stay under the shell's control. It borrows the inline card's
// MediaPlayer — playback continues seamlessly, audio never doubles, and
// closing hands the video surface back to the card and restores focus.
// Escape closes.
Popup {
    id: root
    objectName: "videoViewerOverlay"

    property var player: null
    property var cardOutput: null
    // The card's AudioOutput, for the shared control bar's mute/volume.
    property var cardAudio: null

    function openFor(mediaPlayer, inlineOutput) {
        player = mediaPlayer
        cardOutput = inlineOutput
        cardAudio = mediaPlayer.audioOutput
        open()
        mediaPlayer.videoOutput = expandedOutput
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape

    onClosed: {
        // Hand the frames back to the inline card; playback state is
        // deliberately untouched (whatever played keeps playing inline).
        if (player && cardOutput)
            player.videoOutput = cardOutput
        player = null
        cardOutput = null
        cardAudio = null
    }

    background: Rectangle {
        // Committed dark on every theme, matching ImageViewerOverlay — the
        // shared scrim token rather than a second copy of the same alpha.
        color: AppTheme.scrimSurface
    }

    contentItem: FocusScope {
        focus: true

        // Playback keyboard: Space toggles, arrows seek ±5s, M mutes,
        // Up/Down adjust volume. Escape closes via the Popup policy.
        Keys.onPressed: (event) => {
            if (!root.player) return
            switch (event.key) {
            case Qt.Key_Space:
                root.player.playbackState === MediaPlayer.PlayingState
                    ? root.player.pause() : root.player.play()
                event.accepted = true
                break
            case Qt.Key_Left:
                if (root.player.seekable)
                    root.player.position =
                        Math.max(0, root.player.position - 5000)
                event.accepted = true
                break
            case Qt.Key_Right:
                if (root.player.seekable)
                    root.player.position = Math.min(
                        root.player.duration, root.player.position + 5000)
                event.accepted = true
                break
            case Qt.Key_M:
                if (root.cardAudio) {
                    root.cardAudio.userUnmuted = true
                    root.cardAudio.muted = !root.cardAudio.muted
                }
                event.accepted = true
                break
            case Qt.Key_Up:
                if (root.cardAudio)
                    root.cardAudio.volume =
                        Math.min(1, root.cardAudio.volume + 0.05)
                event.accepted = true
                break
            case Qt.Key_Down:
                if (root.cardAudio)
                    root.cardAudio.volume =
                        Math.max(0, root.cardAudio.volume - 0.05)
                event.accepted = true
                break
            }
        }

        VideoOutput {
            id: expandedOutput
            anchors.fill: parent
            anchors.margins: AppTheme.spacing24
            anchors.bottomMargin: AppTheme.spacing24 + overlayBar.height
            fillMode: VideoOutput.PreserveAspectFit
        }
        TapHandler {
            id: overlayTap
            // Instant toggle on every tap (exclusive signals delayed the
            // single tap by the whole double-click interval — the same
            // "laggy pause" the inline card had). A double-tap toggles
            // twice — net no state change — then exits.
            onTapped: {
                if (!root.player) return
                root.player.playbackState === MediaPlayer.PlayingState
                    ? root.player.pause() : root.player.play()
            }
            onDoubleTapped: root.close()
        }
        HoverHandler { id: overlayHover }

        // Full shared control set — the expanded view is the escape hatch
        // from the card's width-constrained bar.
        VideoControlBar {
            id: overlayBar
            objectName: "videoOverlayControlBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: AppTheme.spacing24
            anchors.rightMargin: AppTheme.spacing24
            anchors.bottomMargin: AppTheme.spacing8
            player: root.player
            audio: root.cardAudio
            showExpand: true
            showClose: false
            expandIcon: "close_fullscreen"
            onExpandRequested: root.close()
            readonly property bool shown:
                !root.player
                || root.player.playbackState !== MediaPlayer.PlayingState
                || overlayHover.hovered || overlayBar.activeFocus
            visible: opacity > 0
            opacity: shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 160 }
            }
        }
    }
}
