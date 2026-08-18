import QtQuick
import QtQuick.Controls
import MatrixClient

// Compact shared volume control for audio and video players. The caller owns
// the QAudioOutput; this component only presents its live muted/volume state.
// A hover/focus popup keeps the slider available without spending permanent
// horizontal space in a media card.
IconButton {
    id: root

    property var audio: null
    property bool scrim: false
    property string sliderObjectName: "mediaVolumeSlider"
    property real lastAudibleVolume: 0.8

    iconName: !audio || audio.muted || audio.volume <= 0
              ? "volume_off" : "volume_up"
    iconColorOverride: scrim ? "#FFFFFF" : ""
    enabled: audio !== null
    Accessible.name: audio && (audio.muted || audio.volume <= 0)
                     ? qsTr("Unmute") : qsTr("Mute")
    ToolTip.text: Accessible.name
    ToolTip.visible: hovered && !volumePopup.visible
    ToolTip.delay: 600

    function markUserVolumeIntent() {
        // Video's explicit-intent start policy uses this bit to break its
        // initial-muted binding. Audio outputs expose the same property so
        // both player types follow one code path.
        if (audio)
            audio.userUnmuted = true
    }

    // 2026-08-18 tester report ("neatsimena audio preferencu uzdeda default
    // visada"): a level the user chose is remembered for the next card and
    // the next session. Only an explicit user gesture writes it — never the
    // player's own state changes, so a video card starting muted by policy
    // cannot silently rewrite the stored level.
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
    // Long enough to cross the gap between the button and the popup, short
    // enough not to linger once the pointer has genuinely left.
    Timer { id: closeGrace; interval: 400 }

    Popup {
        id: volumePopup
        x: Math.round((parent.width - width) / 2)
        // Touch the trigger's top edge so the pointer can travel into the
        // popup without crossing a dead gap that would close it mid-motion.
        y: -height
        width: 44
        height: 124
        padding: AppTheme.spacing8
        // Held open by a short grace timer, NOT by raw hover. The popup
        // sits above the button, and moving the pointer from one to the
        // other necessarily leaves both for a frame or two — with a bare
        // "hovered || hovered" binding the popup vanished exactly as the
        // user reached for it, which made the slider unusable.
        visible: root.audio !== null && (root.wantVolumeOpen || closeGrace.running)
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: root.scrim ? "#E6000000" : AppTheme.surfaceElevated
            border.width: root.scrim ? 0 : 1
            border.color: AppTheme.border
        }
        HoverHandler { id: popupHover }
        // Keyboard/pointer must be able to leave without a dead popup: any
        // press or focus inside keeps it open through the same predicate.
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
            // Track and handle MUST use the same horizontal expression or
            // they visibly disagree. The handle previously added
            // `+ leftPadding / 2` that the track did not, so the dot sat off
            // to one side of the groove it was supposed to ride.
            background: Rectangle {
                x: volumeSlider.leftPadding
                   + volumeSlider.availableWidth / 2 - width / 2
                y: volumeSlider.topPadding
                width: 6
                height: volumeSlider.availableHeight
                radius: width / 2
                color: root.scrim ? "#59FFFFFF" : AppTheme.borderStrong
                // Filled portion grows from the BOTTOM: a vertical slider's
                // visualPosition is 0 at the top, so the fill starts at
                // visualPosition and runs to the end.
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
                color: root.scrim ? "#FFFFFF" : AppTheme.accent
                border.width: 2
                border.color: root.scrim ? "#33000000" : AppTheme.surface
            }
        }
    }
}
