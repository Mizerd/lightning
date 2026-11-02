import QtQuick
import QtQuick.Controls
import MatrixClient

IconButton {
    id: root

    property var audio: null
    property bool scrim: false
    property string sliderObjectName: "mediaVolumeSlider"
    property real lastAudibleVolume: 0.8
    property bool arrowKeysStepLevel: true

    iconName: audio && (audio.muted || audio.volume <= 0)
              ? "volume_off" : "volume_up"
    iconColorOverride: scrim ? AppTheme.scrimInk : ""
    enabled: audio !== null
    readonly property int levelPercent: !audio || audio.muted ? 0
        : Math.round(audio.volume * 100)
    Accessible.name: audio && (audio.muted || audio.volume <= 0)
                     ? qsTr("Unmute") : qsTr("Mute")
    Accessible.description: qsTr("Volume %1%").arg(root.levelPercent)
    ToolTip.text: Accessible.name
    ToolTip.visible: hovered && !volumePopup.visible
    ToolTip.delay: 600

    function markUserVolumeIntent() {
        if (audio)
            audio.userUnmuted = true
    }

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
            volumeSlider.forceActiveFocus(Qt.TabFocusReason)
        } else if (event.key === Qt.Key_Escape && volumePopup.visible) {
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

    readonly property bool wantVolumeOpen: !root.dismissed && (
        buttonHover.hovered || popupHover.hovered
        || volumeSlider.pressed || volumeSlider.activeFocus || root.visualFocus)
    onWantVolumeOpenChanged: {
        if (wantVolumeOpen)
            closeGrace.stop()
        else
            closeGrace.restart()
    }
    Timer { id: closeGrace; interval: 400 }

    Popup {
        id: volumePopup
        x: Math.round((parent.width - width) / 2)
        y: openDown ? root.height : -height
        width: 44
        height: 124
        padding: AppTheme.spacing8
        visible: root.audio !== null && !root.dismissed
                 && (root.wantVolumeOpen || closeGrace.running)
        closePolicy: Popup.NoAutoClose
        property bool openDown: false
        onAboutToShow: {
            var ref = Overlay.overlay
            if (!ref)
                return
            var top = root.mapToItem(ref, 0, 0).y
            volumePopup.openDown = top < volumePopup.height
        }
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: root.scrim ? AppTheme.scrimSurface : AppTheme.surfaceElevated
            border.width: 1
            border.color: root.scrim ? AppTheme.scrimBorder : AppTheme.border
        }
        HoverHandler { id: popupHover }
        contentItem: Slider {
            id: volumeSlider
            objectName: root.sliderObjectName
            orientation: Qt.Vertical
            from: 0
            to: 1
            stepSize: 0.05
            value: root.audio ? root.audio.volume : 0.8
            focusPolicy: Qt.StrongFocus
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
            background: Rectangle {
                x: volumeSlider.leftPadding
                   + volumeSlider.availableWidth / 2 - width / 2
                y: volumeSlider.topPadding
                width: 6
                height: volumeSlider.availableHeight
                radius: width / 2
                color: root.scrim ? AppTheme.scrimSurfaceHover : AppTheme.borderStrong
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
