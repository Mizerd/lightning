import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Expanded in-app video view: a modal overlay (like ImageViewerOverlay), not an
// OS fullscreen window, so account/room context and focus stay with the shell.
// It borrows the inline card's MediaPlayer, so playback continues without
// doubling audio; closing hands the surface back and restores focus. Escape
// closes.
Popup {
    id: root
    objectName: "videoViewerOverlay"

    property var player: null
    property var cardOutput: null
    // The card's AudioOutput, for the control bar's mute/volume.
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
        // Hand the frames back to the card; playback state is untouched.
        if (player && cardOutput)
            player.videoOutput = cardOutput
        player = null
        cardOutput = null
        cardAudio = null
    }

    background: Rectangle {
        // Dark on every theme, like ImageViewerOverlay (the shared scrim
        // token).
        color: AppTheme.scrimSurface
    }

    contentItem: FocusScope {
        focus: true

        // Space toggles, arrows seek ±5s, M mutes, Up/Down adjust volume.
        // Escape closes via the Popup policy.
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
            // Taps toggle instantly (exclusive tap signals would delay the
            // single tap); a double tap toggles twice and exits. Not over the
            // control bar: this handler is its sibling and covers its gaps and
            // label, where a near-miss double click would close the overlay
            // (the same band check the image viewer uses).
            function onTheBar(y) {
                return overlayBar.visible && y >= overlayBar.y
            }
            onTapped: (eventPoint) => {
                if (onTheBar(eventPoint.position.y))
                    return
                if (!root.player) return
                root.player.playbackState === MediaPlayer.PlayingState
                    ? root.player.pause() : root.player.play()
            }
            onDoubleTapped: (eventPoint) => {
                if (onTheBar(eventPoint.position.y))
                    return
                root.close()
            }
        }
        HoverHandler { id: overlayHover }

        // The full control set; the expanded view is the escape hatch from the
        // card's narrow bar.
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
