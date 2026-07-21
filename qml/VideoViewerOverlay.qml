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

    function openFor(mediaPlayer, inlineOutput) {
        player = mediaPlayer
        cardOutput = inlineOutput
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
    }

    background: Rectangle {
        // Deliberate scrim, matching ImageViewerOverlay: readable over both
        // themes.
        color: Qt.rgba(0, 0, 0, 0.85)
    }

    contentItem: Item {
        VideoOutput {
            id: expandedOutput
            anchors.fill: parent
            anchors.margins: AppTheme.spacing24
            fillMode: VideoOutput.PreserveAspectFit
        }
        TapHandler {
            onTapped: {
                if (!root.player) return
                root.player.playbackState === MediaPlayer.PlayingState
                    ? root.player.pause() : root.player.play()
            }
        }
        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: AppTheme.spacing16
            spacing: AppTheme.spacing8
            IconButton {
                objectName: "videoOverlayPlayPause"
                iconName: root.player
                          && root.player.playbackState === MediaPlayer.PlayingState
                          ? "pause" : "play_arrow"
                iconSize: 20
                implicitWidth: 34; implicitHeight: 34
                Accessible.name: qsTr("Play or pause")
                onClicked: {
                    if (!root.player) return
                    if (root.player.playbackState === MediaPlayer.PlayingState)
                        root.player.pause()
                    else
                        root.player.play()
                }
            }
            IconButton {
                objectName: "videoOverlayClose"
                iconName: "close_fullscreen"
                iconSize: 20
                implicitWidth: 34; implicitHeight: 34
                Accessible.name: qsTr("Exit expanded video")
                onClicked: root.close()
            }
        }
    }
}
