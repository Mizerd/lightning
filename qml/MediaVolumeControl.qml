import QtQuick
import QtQuick.Controls
import MatrixClient

// Compact shared volume control for audio and video. The caller owns the
// QAudioOutput; this presents its muted/volume state, with the slider in a
// hover/focus popup.
//
// Keyboard: the button is a Tab stop (Space/Enter mutes); focusing it opens
// the slider, and Tab moves into the slider, a Tab stop of its own: Up/Down
// (or Right/Left) step 5 %, Home is silent, End is full, and Tab/Shift+Tab
// leave it again. Up/Down on the button itself step the level too. Both carry
// the level in their accessible text (design-media U5).
IconButton {
    id: root

    property var audio: null
    property bool scrim: false
    property string sliderObjectName: "mediaVolumeSlider"
    property real lastAudibleVolume: 0.8
    // Up/Down on the button step the level. A host with its own volume keys
    // (the video bar: Up unmutes, Down keeps a mute) turns this off and gets
    // them instead.
    property bool arrowKeysStepLevel: true

    // Muted reads as muted; no player yet (an idle row) is not muted.
    iconName: audio && (audio.muted || audio.volume <= 0)
              ? "volume_off" : "volume_up"
    iconColorOverride: scrim ? AppTheme.scrimInk : ""
    enabled: audio !== null
    // The level in percent, 0 while muted.
    readonly property int levelPercent: !audio || audio.muted ? 0
        : Math.round(audio.volume * 100)
    Accessible.name: audio && (audio.muted || audio.volume <= 0)
                     ? qsTr("Unmute") : qsTr("Mute")
    Accessible.description: qsTr("Volume %1%").arg(root.levelPercent)
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

    // A keyboard step of the level, as a slider move would make it.
    function setLevel(v) {
        if (!audio)
            return
        v = Math.max(0, Math.min(1, Math.round(v * 20) / 20))
        markUserVolumeIntent()
        audio.volume = v
        audio.muted = v <= 0
        if (v > 0)
            lastAudibleVolume = v
        rememberVolume(v)
    }
    Keys.onPressed: (event) => {
        if (!root.audio)
            return
        var level = root.audio.muted ? 0 : root.audio.volume
        if ((event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                && !root.arrowKeysStepLevel) {
            return
        } else if (event.key === Qt.Key_Up) {
            root.setLevel(level + 0.05)
        } else if (event.key === Qt.Key_Down) {
            root.setLevel(level - 0.05)
        } else if (event.key === Qt.Key_Tab && volumePopup.visible) {
            // Into the slider: a Tab stop of its own.
            volumeSlider.forceActiveFocus(Qt.TabFocusReason)
        } else if (event.key === Qt.Key_Escape && volumePopup.visible) {
            // Closes the popup and nothing else: an unhandled Escape goes on
            // to the window, which leaves the room (C21-F03).
            root.dismissPopup()
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.toggleMute()
        } else {
            return
        }
        event.accepted = true
    }

    Connections {
        target: root.audio
        enabled: root.audio !== null
        function onVolumeChanged() {
            if (root.audio.volume > 0)
                root.lastAudibleVolume = root.audio.volume
        }
    }

    HoverHandler { id: buttonHover }

    // Escape for a popup the pointer opened: nothing holds the keyboard
    // focus there, so no Keys handler sees the key and it would go on to the
    // window and leave the room (C21-F03). Enabled only while the popup is
    // up, so it never competes with another Escape (two enabled Shortcuts
    // make Qt fire neither, only an ambiguous activation, which also closes).
    Shortcut {
        sequence: "Escape"
        enabled: volumePopup.visible
        onActivated: root.dismissPopup()
        onActivatedAmbiguously: root.dismissPopup()
    }

    // Escape closed the popup; it stays closed until the pointer or the
    // keyboard comes back to the control. Plain property, never bound.
    property bool dismissed: false
    function dismissPopup() {
        root.dismissed = true
        closeGrace.stop()
        if (volumeSlider.activeFocus)
            root.forceActiveFocus(Qt.OtherFocusReason)
    }
    Connections {
        target: buttonHover
        function onHoveredChanged() {
            if (buttonHover.hovered)
                root.dismissed = false
        }
    }
    onVisualFocusChanged: if (visualFocus) root.dismissed = false

    // One predicate for "the user is engaging with the volume control".
    readonly property bool wantVolumeOpen: !root.dismissed && (
        buttonHover.hovered || popupHover.hovered
        || volumeSlider.pressed || volumeSlider.activeFocus || root.visualFocus)
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
        visible: root.audio !== null && !root.dismissed
                 && (root.wantVolumeOpen || closeGrace.running)
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
            focusPolicy: Qt.StrongFocus
            // The wheel over the open popup steps the level (C21-F06).
            wheelEnabled: true
            Accessible.name: qsTr("Volume %1%").arg(root.levelPercent)
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
            // Up/Down/Left/Right are the Slider's own steps (they emit
            // moved()); Home and End, and leaving with Tab, are added here.
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Home) {
                    root.setLevel(0)
                } else if (event.key === Qt.Key_End) {
                    root.setLevel(1)
                } else if (event.key === Qt.Key_Backtab
                           || (event.key === Qt.Key_Tab
                               && (event.modifiers & Qt.ShiftModifier))) {
                    root.forceActiveFocus(Qt.BacktabFocusReason)
                } else if (event.key === Qt.Key_Tab) {
                    var next = root.nextItemInFocusChain(true)
                    if (next)
                        next.forceActiveFocus(Qt.TabFocusReason)
                } else if (event.key === Qt.Key_Escape) {
                    root.dismissPopup()
                } else {
                    return
                }
                event.accepted = true
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
