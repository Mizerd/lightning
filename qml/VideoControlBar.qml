import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Shared video control bar for the inline card and the expanded overlay, over
// the video on a bottom scrim, so its inks are scrim-constant.
//
// Speed has ONE source of truth: every bar shows the PLAYER's rate. The card
// and the overlay used to keep an index each, so the expanded bar read "1×"
// over a video playing at 1.5×. A choice is also stored as the session's
// video speed (app.playback.videoPlaybackRate), so the next player starts at
// it.
//
// Width ladder, so the seek track keeps a scrubbable width (at least 120 px,
// tests/qml/tst_VideoPlayer.qml) on a 260 px portrait card, where it used to
// be ~45 px:
//   full     (>= 400, and always in the expanded view): time inline, speed,
//            close
//   compact  (< 400): the time moves to its own line above the track, and
//            close leaves the bar (`closeInBar`): the card draws it in its
//            top-right corner
//   tight    (< 280): speed yields too; expanding reaches it
FocusScope {
    id: bar

    // An icon button for a scrim: hover is a pale wash of the scrim itself.
    // IconButton's themed hover fill is a LIGHT plate on a light theme, and
    // the white scrim glyph on it was ~1.3:1. The focus ring is white on the
    // dark scrim: the theme's accent ring measured 1.76:1 against it.
    component ScrimIconButton: IconButton {
        id: sib
        background: Rectangle {
            radius: sib.radius
            color: sib.enabled && (sib.down || sib.hovered)
                   ? AppTheme.scrimSurfaceRaised : sib.restingColor
        }
        Rectangle {
            objectName: "scrimFocusRing"
            anchors.fill: parent
            radius: sib.radius
            color: "transparent"
            border.color: AppTheme.scrimInk
            border.width: 2
            visible: sib.visualFocus
        }
    }

    required property MediaPlayer player
    // The AudioOutput is not reachable through MediaPlayer from QML; passed
    // explicitly.
    required property var audio
    // Re-acquire the one-audible-owner slot on play.
    property string ownerKey: ""
    property bool showExpand: true
    property bool showClose: true
    // The expanded view: larger type and buttons for a stage that can be
    // 1400 px wide, and never compact.
    property bool large: false
    // The length the event declares (info.duration), shown until the player
    // reports its own. 0 when the sender declared none.
    property real declaredDurationMs: 0
    // The player's length once known, else the declared one, else 0.
    readonly property real knownDurationMs:
        player && player.duration > 0 ? player.duration
                                      : Math.max(0, declaredDurationMs || 0)
    // "open_in_full" on the card, "close_fullscreen" in the overlay.
    property string expandIcon: "open_in_full"
    signal expandRequested()
    signal closeRequested()
    // Pulse-backend SIGABRT protection (pwcrash): with the `qtAudio` context
    // property, play only once Qt's PulseAudio connection is ready. A card or
    // bar loaded alone (tests) has no qtAudio and plays directly.
    function gatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.play(p)
        else p.play()
    }
    function cancelGatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.cancelPlay(p)
    }

    readonly property bool playing:
        !!player && player.playbackState === MediaPlayer.PlayingState
    readonly property bool compact: !large && width < 400
    readonly property bool tight: !large && width < 280
    // False in a compact bar: the host shows its own close (the card's
    // top-right corner) so the track keeps its width.
    readonly property bool closeInBar: showClose && !compact
    // A menu or popup of this bar is open. The host keeps the bar shown while
    // it is: the speed menu takes the pointer's hover, and a bar that faded
    // left the menu floating with no visible owner.
    readonly property bool menuOpen:
        speedMenu.visible || volumeControl.wantVolumeOpen

    // The rate the player is really running at.
    readonly property real rate:
        player && player.playbackRate > 0 ? player.playbackRate : 1.0
    readonly property string rateLabel: "" + Math.round(rate * 100) / 100
    readonly property bool rateIsNormal: Math.abs(rate - 1.0) < 0.001
    readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

    readonly property real positionMs: player ? (player.position || 0) : 0
    readonly property string timeText:
        formatPosition(positionMs) + " / " + formatTotal(knownDurationMs)
    // What a screen reader says for the position: "1:04 of 3:00".
    readonly property string timeSpoken:
        knownDurationMs > 0
        ? qsTr("%1 of %2").arg(formatPosition(positionMs))
                          .arg(formatDuration(knownDurationMs))
        : formatPosition(positionMs)

    readonly property int buttonSize: large ? 36 : 30
    readonly property int timePixelSize:
        AppTheme.scaled(large ? AppTheme.textBody : AppTheme.textMeta)

    implicitHeight: 40 + (compact ? compactTime.implicitHeight + 2 : 0)

    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("Video controls")
    Accessible.description: playing ? qsTr("Playing, %1").arg(timeSpoken)
                                    : qsTr("Paused, %1").arg(timeSpoken)

    // A position floors, a total rounds. At 25.7 s you have not reached 0:26,
    // so an elapsed clock floors (rounding would reach the total early); 25.7 s
    // of audio is 26 seconds long, matching embedDurationText. The recording
    // counters are positions and stay floored.
    function formatPosition(ms) {
        return AppTheme.clockPosition(ms)
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return bar.clockText(Math.round(ms / 1000))
    }
    // An unknown length reads "–:–": "0:00" would claim the clip is empty.
    function formatTotal(ms) {
        return ms > 0 ? bar.formatDuration(ms) : "–:–"
    }
    function clockText(totalSeconds) {
        return AppTheme.clockText(totalSeconds)
    }
    function togglePlay() {
        if (!player) return
        if (playing) {
            bar.cancelGatedPlay(player)
            player.pause()
        } else {
            if (ownerKey.length > 0)
                app.playback.acquire(ownerKey)
            // Play after the end starts over rather than at the last frame.
            if (player.mediaStatus === MediaPlayer.EndOfMedia)
                player.position = 0
            bar.gatedPlay(player)
        }
    }
    // Keyboard focus into the bar: the play button once there is something
    // to play, else the bar itself, whose key map works either way.
    function focusPlayButton() {
        if (playButton.enabled)
            playButton.forceActiveFocus(Qt.TabFocusReason)
        else
            bar.forceActiveFocus(Qt.TabFocusReason)
    }
    function toggleMute() {
        volumeControl.toggleMute()
    }
    function seekBy(deltaMs) {
        if (!player || !player.seekable)
            return
        var target = Math.max(0, player.position + deltaMs)
        if (player.duration > 0)
            target = Math.min(player.duration, target)
        player.position = target
    }
    // Keyboard volume: the same path as the slider, so the level is
    // remembered. Up is an explicit unmute (an initially muted card counts as
    // unmuted by the user); Down on a muted card lowers the level and keeps
    // the mute: only Up or M unmutes.
    function stepVolume(delta) {
        if (!audio)
            return
        var v = Math.max(0, Math.min(1,
                    Math.round((audio.volume + delta) * 100) / 100))
        if (delta < 0 && audio.muted) {
            audio.volume = v
            volumeControl.rememberVolume(v)
            return
        }
        volumeControl.markUserVolumeIntent()
        audio.volume = v
        audio.muted = v <= 0
        volumeControl.rememberVolume(v)
    }
    // Speed through the player, and remembered for the session.
    function applyRate(r) {
        if (player)
            player.playbackRate = r
        if (typeof app !== "undefined" && app && app.playback
                && app.playback.videoPlaybackRate !== undefined)
            app.playback.videoPlaybackRate = r
    }
    // One key map for the inline card and the expanded view: Space/K play,
    // Left/Right ±5 s, Home to the start, Up/Down volume, M mute, F expand
    // (or leave the expanded view). Returns whether it took the key. Keys
    // with Ctrl, Alt or Meta stay with the app's shortcuts.
    function handleKey(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                               | Qt.MetaModifier))
            return false
        switch (event.key) {
        case Qt.Key_Space:
        case Qt.Key_K:
            bar.togglePlay()
            return true
        case Qt.Key_Return:
        case Qt.Key_Enter: {
            // Basic's buttons take Space only. Return clicks the focused
            // button, as it does on the cover.
            var f = bar.Window.activeFocusItem
            if (f && f !== bar && typeof f.animateClick === "function"
                    && f.enabled) {
                f.animateClick()
                return true
            }
            return false
        }
        case Qt.Key_Left:
            bar.seekBy(-5000)
            return true
        case Qt.Key_Right:
            bar.seekBy(5000)
            return true
        case Qt.Key_Home:
            if (player && player.seekable)
                player.position = 0
            return true
        case Qt.Key_Up:
            bar.stepVolume(0.05)
            return true
        case Qt.Key_Down:
            bar.stepVolume(-0.05)
            return true
        case Qt.Key_M:
            bar.toggleMute()
            return true
        case Qt.Key_F:
            if (!bar.showExpand)
                return false
            bar.expandRequested()
            return true
        }
        return false
    }
    // Reaches here from any control in the bar that does not use the key
    // itself (buttons take Space and Return as a click).
    Keys.onPressed: (event) => {
        if (bar.handleKey(event))
            event.accepted = true
    }

    // The scrim starts above the bar and is darkest under the inks, so time,
    // speed and glyphs stay readable over a bright frame. The old 40 px ramp
    // from transparent left the label row at roughly 40% over cyan, yellow or
    // magenta video.
    Rectangle {
        objectName: "videoBarScrim"
        // The expanded view sits on an opaque backdrop below the picture:
        // nothing to darken there.
        visible: !bar.large
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: bar.height + 32
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            // Dark on every theme: the bar sits over arbitrary video.
            // scrimSurface is the shared chrome-over-media value.
            GradientStop {
                position: 0.4
                color: Qt.alpha(AppTheme.scrimSurface, 0.45)
            }
            GradientStop { position: 1.0; color: AppTheme.scrimSurface }
        }
    }

    // Compact: the time on its own line, above the track.
    Label {
        id: compactTime
        objectName: bar.compact ? "videoTimeLabel" : ""
        visible: bar.compact
        anchors.left: parent.left
        // Aligned with the start of the seek track: the row's margin, the
        // play button, the row spacing and the slider's own padding.
        anchors.leftMargin: AppTheme.spacing6 + bar.buttonSize + row.spacing
                            + seekSlider.leftPadding
        anchors.bottom: row.top
        anchors.bottomMargin: 2
        text: bar.timeText
        color: AppTheme.scrimInkStrong
        font.pixelSize: bar.timePixelSize
        font.weight: AppTheme.weightStrong
        Accessible.ignored: true
    }

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 40
        anchors.leftMargin: AppTheme.spacing6
        anchors.rightMargin: AppTheme.spacing6
        anchors.bottomMargin: 2
        spacing: 2

        ScrimIconButton {
            id: playButton
            objectName: "videoPlayPauseButton"
            iconName: bar.playing ? "pause" : "play_arrow"
            iconSize: bar.large ? 24 : 20
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            iconColorOverride: AppTheme.scrimInk
            enabled: !!bar.player && !!bar.player.source
                     && bar.player.source.toString().length > 0
            Accessible.name: bar.playing ? qsTr("Pause video")
                                         : qsTr("Play video")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: bar.togglePlay()
        }

        Slider {
            id: seekSlider
            objectName: "videoSeekSlider"
            // Time runs left to right in every locale (as in every player),
            // so Left = -5 s stays true under a right-to-left layout.
            LayoutMirroring.enabled: false
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            from: 0
            to: bar.player && bar.player.duration > 0 ? bar.player.duration : 1
            enabled: !!bar.player && bar.player.seekable === true
            // Not a binding on `value` itself: Slider clamps `value` into
            // [from, to] when `to` changes, and a plain binding on
            // positionMs does not re-run then, so a view opened while PAUSED
            // (duration arrives after the first evaluation, position never
            // changes again) drew the handle at 0 under a clock reading
            // 0:44. The Binding below also depends on `to`.
            Accessible.name: qsTr("Seek position")
            Accessible.description: bar.timeSpoken
            onMoved: if (bar.player) bar.player.position = value
            // Where a click would land, shown while hovering the track, and
            // the destination while dragging. A time only: no frame is
            // decoded for it.
            // Explicit: a Control's default follows the platform's hover
            // hint, which is off on some (offscreen, touch-first) and then
            // takes the handler's hover with it.
            hoverEnabled: true
            HoverHandler { id: seekHover }
            readonly property real hoverMs: {
                if (seekSlider.pressed)
                    return seekSlider.value
                if (!seekHover.hovered || seekSlider.availableWidth <= 0)
                    return -1
                var f = (seekHover.point.position.x - seekSlider.leftPadding)
                        / seekSlider.availableWidth
                return Math.max(0, Math.min(1, f)) * seekSlider.to
            }
            ToolTip {
                id: seekTimeTip
                objectName: "videoSeekTimeTip"
                parent: seekSlider
                visible: seekSlider.enabled && seekSlider.hoverMs >= 0
                         && !!bar.player && bar.player.duration > 0
                text: bar.formatPosition(seekSlider.hoverMs)
                // A clock we compute, drawn plain like every explicit
                // ToolTip whose text is not a literal (ToolTipPlainTextQmlTest).
                contentItem: Text {
                    text: seekTimeTip.text
                    textFormat: Text.PlainText
                    font: seekTimeTip.font
                    color: seekTimeTip.palette.toolTipText
                }
                y: -height - 4
                x: Math.max(0, Math.min(seekSlider.width - width,
                       seekSlider.leftPadding
                       + (seekSlider.hoverMs / Math.max(1, seekSlider.to))
                         * seekSlider.availableWidth - width / 2))
            }
            Binding {
                target: seekSlider
                property: "value"
                when: !seekSlider.pressed
                value: Math.min(bar.positionMs, seekSlider.to)
                restoreMode: Binding.RestoreNone
            }
            // Five-second steps, as in the expanded view; the stock step is
            // a tenth of the clip.
            Keys.onLeftPressed: (event) => {
                bar.seekBy(-5000)
                event.accepted = true
            }
            Keys.onRightPressed: (event) => {
                bar.seekBy(5000)
                event.accepted = true
            }
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
                border.width: seekSlider.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
        }

        Label {
            objectName: bar.compact ? "" : "videoTimeLabel"
            visible: !bar.compact
            text: bar.timeText
            color: AppTheme.scrimInkStrong
            font.pixelSize: bar.timePixelSize
            font.weight: AppTheme.weightStrong
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            Accessible.ignored: true
        }

        MediaVolumeControl {
            id: volumeControl
            objectName: "videoMuteButton"
            audio: bar.audio
            scrim: true
            sliderObjectName: "videoVolumeSlider"
            // The bar's own Up/Down (Up unmutes, Down keeps a mute).
            arrowKeysStepLevel: false
            iconSize: bar.large ? 22 : 18
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            // Tab lands here and opens the level popup; Up/Down (handled by
            // the bar) move the level, so the slider is keyboard-reachable
            // without entering the popup.
            Accessible.description: bar.audio
                ? qsTr("Volume %1%").arg(Math.round(
                      (bar.audio.muted ? 0 : bar.audio.volume) * 100))
                : ""
        }
        AbstractButton {
            id: speedButton
            objectName: "videoSpeedButton"
            visible: !bar.tight
            // Wide enough for its label at any text scale ("1.75×" at 150%).
            implicitWidth: Math.max(bar.large ? 44 : 38,
                                    rateText.implicitWidth + 10)
            implicitHeight: bar.buttonSize
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(bar.rateLabel)
            hoverEnabled: true
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered && !speedMenu.visible
            ToolTip.delay: 600
            onClicked: speedMenu.visible ? speedMenu.close() : speedMenu.open()
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: speedButton.hovered || speedMenu.visible
                       ? AppTheme.scrimSurfaceRaised : "transparent"
                border.width: speedButton.visualFocus ? 2 : 0
                border.color: AppTheme.scrimInk
            }
            contentItem: Item {
                Label {
                    id: rateText
                    objectName: "videoSpeedLabel"
                    anchors.centerIn: parent
                    text: bar.rateLabel + "×"
                    // Scrim ink at every rate: accent text on video was
                    // unreadable on saturated frames. A changed rate is
                    // marked by the accent rule underneath.
                    color: AppTheme.scrimInkStrong
                    font.pixelSize: bar.timePixelSize
                    font.weight: AppTheme.weightBold
                }
                Rectangle {
                    objectName: "videoSpeedChangedMark"
                    visible: !bar.rateIsNormal
                    anchors.horizontalCenter: rateText.horizontalCenter
                    anchors.top: rateText.bottom
                    width: rateText.width
                    height: 2
                    radius: 1
                    color: AppTheme.accent
                }
            }
        }

        ScrimIconButton {
            objectName: "videoExpandButton"
            visible: bar.showExpand
            iconName: bar.expandIcon
            iconSize: bar.large ? 22 : 18
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: bar.expandIcon === "open_in_full"
                             ? qsTr("Expand video") : qsTr("Exit expanded video")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: bar.expandRequested()
        }
        ScrimIconButton {
            objectName: "videoCloseButton"
            visible: bar.closeInBar
            iconName: "close"
            iconSize: 18
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("Close player")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: bar.closeRequested()
        }
    }

    // Opens UPWARD from the speed button, over the video, at its laid-out
    // height. It used to be placed with popup(button, 0, -height - 4), and
    // `height` was read before the menu had ever been laid out, so the offset
    // was a few pixels and the menu dropped over the button and the composer
    // below the card. A binding follows the real height instead, floored by
    // the height the rows will take, so even the first frame is placed right.
    AppMenu {
        id: speedMenu
        objectName: "videoSpeedMenu"
        parent: speedButton
        menuWidth: 120
        // A press on the speed button is not "outside": it would close the
        // menu on press and the click would open it again. The button's own
        // click toggles it instead.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        readonly property real expectedHeight:
            bar.rates.length * AppTheme.menuItemHeight + topPadding
            + bottomPadding
        // Over the video: leftwards from the button, rightwards when the row
        // is mirrored and the button sits at the left end.
        x: speedButton.mirrored ? 0 : Math.round(speedButton.width - width)
        y: -Math.max(height, expectedHeight) - 4
        Repeater {
            model: bar.rates
            AppMenuItem {
                required property var modelData
                text: modelData + "×"
                iconName: Math.abs(modelData - bar.rate) < 0.001 ? "check" : ""
                onTriggered: bar.applyRate(modelData)
            }
        }
    }
}
