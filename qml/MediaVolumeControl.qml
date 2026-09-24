import QtQuick
import QtQuick.Controls
import MatrixClient

// Compact shared volume control for audio and video. The caller owns the
// QAudioOutput; this presents its muted/volume state, with the slider in a
// hover/focus popup.
IconButton {
    id: root

    property var audio: null
    property bool scrim: false
    property string sliderObjectName: "mediaVolumeSlider"
    property real lastAudibleVolume: 0.8

    iconName: !audio || audio.muted || audio.volume <= 0
              ? "volume_off" : "volume_up"
    iconColorOverride: scrim ? AppTheme.scrimInk : ""
    enabled: audio !== null
    Accessible.name: audio && (audio.muted || audio.volume <= 0)
                     ? qsTr("Unmute") : qsTr("Mute")
    ToolTip.text: Accessible.name
    ToolTip.visible: hovered && !volumePopup.visible
    ToolTip.delay: 600

    function markUserVolumeIntent() {
        // Video uses this to break its initial-muted binding; audio outputs
        // share the property, so both follow one path.
        if (audio)
            audio.userUnmuted = true
    }

    // Remember the chosen level for the next card and session. Only explicit
    // user gestures write it, never player state changes.
    function rememberVolume(v) {
        if (v > 0)
            app.settings.mediaVolume = v
    }

    function toggleMute() {
        if (!audio)
            return
        markUserVolumeIntent()
        if (audio.muted || audio.volume <= 0) {
            if (audio.volume <= 0)
                audio.volume = Math.max(0.05, lastAudibleVolume)
            audio.muted = false
        } else {
            lastAudibleVolume = audio.volume
            audio.muted = true
        }
    }

    onClicked: toggleMute()

    Connections {
        target: root.audio
        enabled: root.audio !== null
        function onVolumeChanged() {
            if (root.audio.volume > 0)
                root.lastAudibleVolume = root.audio.volume
        }
    }

    HoverHandler { id: buttonHover }

    // One predicate for "the user is engaging with the volume control".
    readonly property bool wantVolumeOpen:
        buttonHover.hovered || popupHover.hovered
        || volumeSlider.pressed || volumeSlider.activeFocus || root.visualFocus
    onWantVolumeOpenChanged: {
        if (wantVolumeOpen)
            closeGrace.stop()
        else
            closeGrace.restart()
    }
    // Long enough to cross from the button to the popup.
    Timer { id: closeGrace; interval: 400 }

    Popup {
        id: volumePopup
        x: Math.round((parent.width - width) / 2)
        // Touch the button's top edge so there is no dead gap.
        y: -height
        width: 44
        height: 124
        padding: AppTheme.spacing8
        // Held open by a grace timer, not raw hover: moving between button and
        // popup leaves both for a frame or two.
        visible: root.audio !== null && (root.wantVolumeOpen || closeGrace.running)
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: root.scrim ? AppTheme.scrimSurface : AppTheme.surfaceElevated
            // A hairline so the dark panel separates from dark video.
            border.width: 1
            border.color: root.scrim ? AppTheme.scrimBorder : AppTheme.border
        }
        HoverHandler { id: popupHover }
        // Any press or focus inside keeps the popup open (same predicate).
        contentItem: Slider {
            id: volumeSlider
            objectName: root.sliderObjectName
            orientation: Qt.Vertical
            from: 0
            to: 1
            stepSize: 0.05
            value: root.audio ? root.audio.volume : 0.8
            Accessible.name: qsTr("Volume")
            Accessible.description: qsTr("Playback volume")
            onMoved: {
                if (!root.audio)
                    return
                root.markUserVolumeIntent()
                root.audio.volume = value
                root.audio.muted = value <= 0
                if (value > 0)
                    root.lastAudibleVolume = value
                root.rememberVolume(value)
            }
            // Track and handle must use the same horizontal expression.
            background: Rectangle {
                x: volumeSlider.leftPadding
                   + volumeSlider.availableWidth / 2 - width / 2
                y: volumeSlider.topPadding
                width: 6
                height: volumeSlider.availableHeight
                radius: width / 2
                color: root.scrim ? AppTheme.scrimSurfaceHover : AppTheme.borderStrong
                // The fill grows from the bottom: a vertical slider's
                // visualPosition is 0 at the top.
                Rectangle {
                    y: volumeSlider.visualPosition * parent.height
                    width: parent.width
                    height: parent.height - y
                    radius: parent.radius
                    color: AppTheme.accent
                }
            }
            handle: Rectangle {
                x: volumeSlider.leftPadding
                   + volumeSlider.availableWidth / 2 - width / 2
                y: volumeSlider.topPadding
                   + volumeSlider.visualPosition
                     * (volumeSlider.availableHeight - height)
                width: 14
                height: 14
                radius: width / 2
                color: root.scrim ? AppTheme.scrimInk : AppTheme.accent
                border.width: 2
                border.color: root.scrim ? AppTheme.scrimBackdrop : AppTheme.surface
            }
        }
    }
}
