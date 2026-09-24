import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Shared video control bar for the inline card and the expanded overlay, over
// the video on a bottom scrim, so its inks are scrim-constant. A tight card
// drops the speed button; play, seek, time, volume and expand stay at every
// width. The expanded player shows the full set.
FocusScope {
    id: bar

    required property MediaPlayer player
    // The AudioOutput is not reachable through MediaPlayer from QML; passed
    // explicitly.
    required property var audio
    // Re-acquire the one-audible-owner slot on play.
    property string ownerKey: ""
    property bool showExpand: true
    property bool showClose: true
    // "open_in_full" on the card, "close_fullscreen" in the overlay.
    property string expandIcon: "open_in_full"
    signal expandRequested()
    signal closeRequested()

    readonly property bool playing:
        player && player.playbackState === MediaPlayer.PlayingState
    readonly property bool tight: width < 340

    implicitHeight: 40

    // A position floors, a total rounds. At 25.7 s you have not reached 0:26,
    // so an elapsed clock floors (rounding would reach the total early); 25.7 s
    // of audio is 26 seconds long, matching embedDurationText. The recording
    // counters are positions and stay floored.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return bar.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return bar.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function togglePlay() {
        if (!player) return
        if (playing) {
            player.pause()
        } else {
            if (ownerKey.length > 0)
                app.playback.acquire(ownerKey)
            player.play()
        }
    }
    function toggleMute() {
        volumeControl.toggleMute()
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            // Dark on every theme: the bar sits over arbitrary video.
            // scrimSurface is the shared chrome-over-media value.
            GradientStop { position: 1.0; color: AppTheme.scrimSurface }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing6
        anchors.rightMargin: AppTheme.spacing6
        anchors.bottomMargin: 2
        spacing: 2

        IconButton {
            objectName: "videoPlayPauseButton"
            iconName: bar.playing ? "pause" : "play_arrow"
            iconSize: 20
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: AppTheme.scrimInk
            enabled: bar.player
                     && bar.player.source.toString().length > 0
            Accessible.name: bar.playing ? qsTr("Pause video")
                                         : qsTr("Play video")
            onClicked: bar.togglePlay()
        }

        Slider {
            id: seekSlider
            objectName: "videoSeekSlider"
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            from: 0
            to: bar.player ? Math.max(1, bar.player.duration) : 1
            enabled: bar.player && bar.player.seekable
            value: pressed ? value : (bar.player ? bar.player.position : 0)
            Accessible.name: qsTr("Seek position")
            onMoved: if (bar.player) bar.player.position = value
            // Scrim-styled track and handle; the Basic track vanishes over
            // video.
            background: Rectangle {
                x: seekSlider.leftPadding
                y: seekSlider.topPadding + seekSlider.availableHeight / 2 - 2
                width: seekSlider.availableWidth
                height: 4
                radius: 2
                color: AppTheme.scrimSurfaceHover
                Rectangle {
                    width: seekSlider.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: AppTheme.accent
                }
            }
            handle: Rectangle {
                x: seekSlider.leftPadding
                   + seekSlider.visualPosition
                     * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding
                   + seekSlider.availableHeight / 2 - height / 2
                width: 12; height: 12; radius: 6
                color: AppTheme.scrimInk
                visible: seekSlider.enabled
            }
        }

        Label {
            objectName: "videoTimeLabel"
            text: bar.formatPosition(bar.player ? bar.player.position : 0)
                  + " / "
                  + bar.formatDuration(bar.player ? bar.player.duration : 0)
            color: AppTheme.scrimInkStrong
            font.pixelSize: AppTheme.textMicro
            font.weight: AppTheme.weightStrong
            Layout.leftMargin: 2
            Layout.rightMargin: 2
        }

        // Volume stays at every width, taking the old overflow button's slot;
        // speed is what a tight card gives up (reachable by expanding). Keeping
        // both pushed the close button out of a 260px card
        // (portraitVideoControlsRemainReachable).
        MediaVolumeControl {
            id: volumeControl
            objectName: "videoMuteButton"
            audio: bar.audio
            scrim: true
            sliderObjectName: "videoVolumeSlider"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
        }
        AbstractButton {
            id: speedButton
            objectName: "videoSpeedButton"
            visible: !bar.tight
            implicitWidth: 34; implicitHeight: 30
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(bar._rateLabel)
            onClicked: speedMenu.popup(speedButton, 0, -speedMenu.height - 4)
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: speedButton.hovered ? AppTheme.scrimSurfaceRaised
                                           : "transparent"
                border.width: speedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: bar._rateLabel + "×"
                color: bar._rateIndex === 2 ? AppTheme.scrimInkStrong
                                            : AppTheme.accent
                font.pixelSize: AppTheme.textMicro
                font.weight: AppTheme.weightBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        IconButton {
            objectName: "videoExpandButton"
            visible: bar.showExpand
            iconName: bar.expandIcon
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: bar.expandIcon === "open_in_full"
                             ? qsTr("Expand video") : qsTr("Exit expanded video")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: bar.expandRequested()
        }
        IconButton {
            objectName: "videoCloseButton"
            visible: bar.showClose
            iconName: "close"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("Close player")
            onClicked: bar.closeRequested()
        }
    }

    // Speed state for both presentations; session-scoped, never persisted.
    readonly property var _rates: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
    property int _rateIndex: 2
    readonly property string _rateLabel: "" + _rates[_rateIndex]
    function _applyRate(index) {
        _rateIndex = index
        if (player) player.playbackRate = _rates[index]
    }

    AppMenu {
        id: speedMenu
        Repeater {
            model: bar._rates
            AppMenuItem {
                required property int index
                required property var modelData
                text: modelData + "×"
                iconName: index === bar._rateIndex ? "check" : ""
                onTriggered: bar._applyRate(index)
            }
        }
    }
}
