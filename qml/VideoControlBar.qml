import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Shared adaptive video control bar — one implementation for the inline
// card and the expanded overlay. Sits over the video on a bottom gradient
// scrim, so its ink is scrim-constant (accentText on dark), not themed
// surface ink. Width-adaptive: in tight cards mute and speed collapse into
// an overflow menu; the seek slider, time, and expand/exit action remain
// visible. Expanding the player exposes the complete direct-control set.
FocusScope {
    id: bar

    required property MediaPlayer player
    // The AudioOutput is not reachable through MediaPlayer from QML;
    // callers pass it explicitly.
    required property var audio
    // Re-acquire the one-audible-owner slot when play is pressed here.
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

    function formatMs(ms) {
        if (!ms || ms < 0) ms = 0
        var total = Math.floor(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
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
            // Committed dark on every theme: the bar sits over
            // arbitrary video. scrimSurface is the shared "chrome over
            // media" value — this used to be one of three different black
            // alphas invented across three media files (70/85/90%), which
            // is why the control bar, the volume popup and the image
            // viewer never matched each other.
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
            // Scrim-styled compact track/handle (the Basic style track is
            // surface-themed and vanishes over video).
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
            text: bar.formatMs(bar.player ? bar.player.position : 0)
                  + " / " + bar.formatMs(bar.player ? bar.player.duration : 0)
            color: AppTheme.scrimInkStrong
            font.pixelSize: AppTheme.textMicro
            font.weight: AppTheme.weightStrong
            Layout.leftMargin: 2
            Layout.rightMargin: 2
        }

        // Wide layout exposes the volume slider directly in a compact
        // hover/focus popup. Tight cards retain mute in the overflow menu.
        MediaVolumeControl {
            id: volumeControl
            objectName: "videoMuteButton"
            audio: bar.audio
            scrim: true
            sliderObjectName: "videoVolumeSlider"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            visible: !bar.tight
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

        // Tight layout: mute and speed move into one overflow menu.
        IconButton {
            objectName: "videoOverflowButton"
            visible: bar.tight
            iconName: "more_vert"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("More playback controls")
            onClicked: overflowMenu.popup(this, 0, -overflowMenu.height - 4)
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

    // Speed state shared by both presentations. Session-scoped, never
    // persisted; resets with the bar instance.
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
    AppMenu {
        id: overflowMenu
        AppMenuItem {
            iconName: !bar.audio || bar.audio.muted || bar.audio.volume <= 0
                      ? "volume_off" : "volume_up"
            text: bar.audio && (bar.audio.muted || bar.audio.volume <= 0)
                  ? qsTr("Unmute") : qsTr("Mute")
            onTriggered: bar.toggleMute()
        }
        AppMenuItem {
            iconName: "speed"
            text: qsTr("Speed: %1×").arg(bar._rateLabel)
            onTriggered: {
                var next = (bar._rateIndex + 1) % bar._rates.length
                bar._applyRate(next)
            }
        }
    }
}
