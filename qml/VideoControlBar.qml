import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Shared adaptive video control bar — one implementation for the inline
// card and the expanded overlay. Sits over the video on a bottom gradient
// scrim, so its ink is scrim-constant (accentText on dark), not themed
// surface ink. Width-adaptive: below `tightWidth` the mute and speed
// controls collapse into one overflow menu; the seek slider, time, and the
// expand/exit action are never dropped — expand is the escape hatch to the
// full control set.
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
        if (!audio) return
        audio.userUnmuted = true
        audio.muted = !audio.muted
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#B3000000" }
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
            iconColorOverride: "#FFFFFF"
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
                color: "#59FFFFFF"
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
                color: "#FFFFFF"
                visible: seekSlider.enabled
            }
        }

        Label {
            objectName: "videoTimeLabel"
            text: bar.formatMs(bar.player ? bar.player.position : 0)
                  + " / " + bar.formatMs(bar.player ? bar.player.duration : 0)
            color: "#E6FFFFFF"
            font.pixelSize: 10
            font.weight: Font.DemiBold
            Layout.leftMargin: 2
            Layout.rightMargin: 2
        }

        // Wide layout: dedicated mute + speed controls.
        IconButton {
            objectName: "videoMuteButton"
            visible: !bar.tight
            iconName: bar.audio && bar.audio.muted ? "volume_off" : "volume_up"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: "#FFFFFF"
            Accessible.name: bar.audio && bar.audio.muted
                             ? qsTr("Unmute") : qsTr("Mute")
            onClicked: bar.toggleMute()
            HoverHandler { id: muteHover }
            // Volume popup on hover — keeps the bar uncramped.
            Popup {
                id: volumePopup
                x: Math.round((parent.width - width) / 2)
                y: -height - 4
                width: 36
                height: 110
                padding: 8
                visible: (muteHover.hovered || volumeHover.hovered
                          || volumeSlider.pressed)
                         && bar.audio && !bar.audio.muted
                closePolicy: Popup.NoAutoClose
                background: Rectangle {
                    radius: AppTheme.radiusMd
                    color: "#D9000000"
                }
                contentItem: Slider {
                    id: volumeSlider
                    objectName: "videoVolumeSlider"
                    orientation: Qt.Vertical
                    from: 0; to: 1
                    value: bar.audio ? bar.audio.volume : 0.8
                    onMoved: if (bar.audio) bar.audio.volume = value
                    Accessible.name: qsTr("Volume")
                    HoverHandler { id: volumeHover }
                    background: Rectangle {
                        x: volumeSlider.availableWidth / 2 - 2
                        width: 4
                        height: volumeSlider.availableHeight
                        radius: 2
                        color: "#59FFFFFF"
                        Rectangle {
                            y: parent.height
                               - (1 - volumeSlider.visualPosition) * parent.height
                            width: parent.width
                            height: (1 - volumeSlider.visualPosition) * parent.height
                            radius: 2
                            color: AppTheme.accent
                        }
                    }
                    handle: Rectangle {
                        x: volumeSlider.availableWidth / 2 - width / 2
                           + volumeSlider.leftPadding / 2
                        y: volumeSlider.topPadding
                           + volumeSlider.visualPosition
                             * (volumeSlider.availableHeight - height)
                        width: 12; height: 12; radius: 6
                        color: "#FFFFFF"
                    }
                }
            }
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
                color: speedButton.hovered ? "#33FFFFFF" : "transparent"
                border.width: speedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: bar._rateLabel + "×"
                color: bar._rateIndex === 2 ? "#E6FFFFFF" : AppTheme.accent
                font.pixelSize: 10
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        // Tight layout: one overflow menu holding mute + speed.
        IconButton {
            objectName: "videoOverflowButton"
            visible: bar.tight
            iconName: "more_vert"
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: "#FFFFFF"
            Accessible.name: qsTr("More playback controls")
            onClicked: overflowMenu.popup(this, 0, -overflowMenu.height - 4)
        }

        IconButton {
            objectName: "videoExpandButton"
            visible: bar.showExpand
            iconName: bar.expandIcon
            iconSize: 18
            implicitWidth: 30; implicitHeight: 30
            iconColorOverride: "#FFFFFF"
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
            iconColorOverride: "#FFFFFF"
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
            iconName: bar.audio && bar.audio.muted ? "volume_off" : "volume_up"
            text: bar.audio && bar.audio.muted ? qsTr("Unmute") : qsTr("Mute")
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
