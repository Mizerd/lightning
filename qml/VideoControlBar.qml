import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

FocusScope {
    id: bar

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
    property string ownerKey: ""
    property var trackIntent: ({ audio: -1, subtitle: -2 })
    property bool showExpand: true
    property bool showClose: true
    property bool large: false
    property real declaredDurationMs: 0
    readonly property real knownDurationMs:
        player && player.duration > 0 ? player.duration
                                      : Math.max(0, declaredDurationMs || 0)
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
    readonly property bool closeInBar: showClose && !compact
    readonly property bool menuOpen:
        speedMenu.visible || volumeControl.wantVolumeOpen || trackMenu.visible

    readonly property var audioTracks:
        player && player.audioTracks ? player.audioTracks : []
    readonly property var subtitleTracks:
        player && player.subtitleTracks ? player.subtitleTracks : []
    readonly property bool hasTrackChoice:
        audioTracks.length > 1 || subtitleTracks.length > 0
    // The FFmpeg backend emits no activeTracksChanged, neither for a pick nor
    // on load, so the menu reads the pick and re-reads on tracksChanged.
    readonly property int shownAudioTrack: trackIntent.audio >= 0
        ? trackIntent.audio
        : (player && audioTracks.length > 0 ? player.activeAudioTrack : -1)
    readonly property int shownSubtitleTrack: trackIntent.subtitle >= -1
        ? trackIntent.subtitle
        : (player && subtitleTracks.length >= 0 ? player.activeSubtitleTrack : -1)
    property int reapplyBudget: 0
    function reapplyTracks() {
        if (!player || player.mediaStatus === MediaPlayer.NoMedia
                || player.mediaStatus === MediaPlayer.LoadingMedia
                || player.mediaStatus === MediaPlayer.InvalidMedia)
            return
        var want = trackIntent
        // Before the tracks are known a pick cannot land and only spends the budget.
        if (want.audio >= 0 && want.audio < audioTracks.length
                && player.activeAudioTrack !== want.audio
                && reapplyBudget < 8) {
            ++reapplyBudget
            console.log("video track pick reverted audio want=" + want.audio
                        + " got=" + player.activeAudioTrack)
            player.activeAudioTrack = want.audio
        }
        if (want.subtitle >= -1 && want.subtitle < subtitleTracks.length
                && player.activeSubtitleTrack !== want.subtitle
                && reapplyBudget < 8) {
            ++reapplyBudget
            console.log("video track pick reverted subtitle want="
                        + want.subtitle + " got=" + player.activeSubtitleTrack)
            player.activeSubtitleTrack = want.subtitle
        }
    }
    Connections {
        target: bar.player
        ignoreUnknownSignals: true
        function onActiveTracksChanged() { bar.reapplyTracks() }
        function onTracksChanged() { bar.reapplyTracks() }
        function onPlaybackStateChanged() { bar.reapplyTracks() }
        function onMediaStatusChanged() { bar.reapplyTracks() }
        function onSourceChanged() { bar.reapplyBudget = 0 }
    }
    function trackLabel(meta, index) {
        var parts = []
        if (meta) {
            var lang = meta.stringValue(MediaMetaData.Language)
            var title = meta.stringValue(MediaMetaData.Title)
            if (lang && lang.length > 0)
                parts.push(lang)
            if (title && title.length > 0 && title !== lang)
                parts.push(title)
        }
        return parts.length > 0 ? parts.join(" — ")
                                : qsTr("Track %1").arg(index + 1)
    }

    readonly property real rate:
        player && player.playbackRate > 0 ? player.playbackRate : 1.0
    readonly property string rateLabel: "" + Math.round(rate * 100) / 100
    readonly property bool rateIsNormal: Math.abs(rate - 1.0) < 0.001
    readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

    readonly property real positionMs: player ? (player.position || 0) : 0
    readonly property string timeText:
        formatPosition(positionMs) + " / " + formatTotal(knownDurationMs)
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
            if (player.mediaStatus === MediaPlayer.EndOfMedia)
                player.position = 0
            bar.gatedPlay(player)
        }
    }
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
    function applyRate(r) {
        if (player)
            player.playbackRate = r
        if (typeof app !== "undefined" && app && app.playback
                && app.playback.videoPlaybackRate !== undefined)
            app.playback.videoPlaybackRate = r
    }
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
    Keys.onPressed: (event) => {
        if (bar.handleKey(event))
            event.accepted = true
    }

    Rectangle {
        objectName: "videoBarScrim"
        visible: !bar.large
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: bar.height + 32
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop {
                position: 0.4
                color: Qt.alpha(AppTheme.scrimSurface, 0.45)
            }
            GradientStop { position: 1.0; color: AppTheme.scrimSurface }
        }
    }

    Label {
        id: compactTime
        objectName: bar.compact ? "videoTimeLabel" : ""
        visible: bar.compact
        anchors.left: parent.left
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
            Keys.onLeftPressed: (event) => {
                bar.seekBy(-5000)
                event.accepted = true
            }
            Keys.onRightPressed: (event) => {
                bar.seekBy(5000)
                event.accepted = true
            }
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
            arrowKeysStepLevel: false
            iconSize: bar.large ? 22 : 18
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            Accessible.description: bar.audio
                ? qsTr("Volume %1%").arg(Math.round(
                      (bar.audio.muted ? 0 : bar.audio.volume) * 100))
                : ""
        }
        AbstractButton {
            id: speedButton
            objectName: "videoSpeedButton"
            visible: !bar.tight
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
            id: trackButton
            objectName: "videoTracksButton"
            visible: bar.hasTrackChoice && !bar.tight
            iconName: "format_list_bulleted"
            iconSize: bar.large ? 22 : 18
            implicitWidth: bar.buttonSize; implicitHeight: bar.buttonSize
            iconColorOverride: AppTheme.scrimInk
            Accessible.name: qsTr("Audio and subtitles")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered && !trackMenu.visible
            ToolTip.delay: 600
            onClicked: trackMenu.visible ? trackMenu.close() : trackMenu.open()
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

    AppMenu {
        id: trackMenu
        objectName: "videoTracksMenu"
        parent: trackButton
        menuWidth: 220
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        readonly property int rows:
            (bar.audioTracks.length > 1 ? bar.audioTracks.length + 1 : 0)
            + (bar.subtitleTracks.length > 0 ? bar.subtitleTracks.length + 2 : 0)
        x: trackButton.mirrored ? 0 : Math.round(trackButton.width - width)
        y: -Math.max(height, rows * AppTheme.menuItemHeight + topPadding
                                + bottomPadding) - 4
        Repeater {
            model: {
                var rows = []
                if (bar.audioTracks.length > 1) {
                    rows.push({ kind: "heading", label: qsTr("Audio") })
                    for (var i = 0; i < bar.audioTracks.length; ++i)
                        rows.push({ kind: "audio", index: i,
                                    meta: bar.audioTracks[i] })
                }
                if (bar.subtitleTracks.length > 0) {
                    rows.push({ kind: "heading", label: qsTr("Subtitles") })
                    rows.push({ kind: "subtitleOff" })
                    for (var j = 0; j < bar.subtitleTracks.length; ++j)
                        rows.push({ kind: "subtitle", index: j,
                                    meta: bar.subtitleTracks[j] })
                }
                return rows
            }
            AppMenuItem {
                required property var modelData
                readonly property bool heading: modelData.kind === "heading"
                objectName: modelData.kind === "audio"
                            ? "videoAudioTrack" + modelData.index
                            : modelData.kind === "subtitle"
                              ? "videoSubtitleTrack" + modelData.index
                              : modelData.kind === "subtitleOff"
                                ? "videoSubtitleOff" : "videoTracksHeading"
                enabled: !heading
                text: heading ? modelData.label
                      : modelData.kind === "subtitleOff" ? qsTr("Off")
                      : bar.trackLabel(modelData.meta, modelData.index)
                iconName: {
                    if (!bar.player) return ""
                    if (modelData.kind === "audio")
                        return bar.shownAudioTrack === modelData.index
                               ? "check" : ""
                    if (modelData.kind === "subtitleOff")
                        return bar.shownSubtitleTrack < 0 ? "check" : ""
                    if (modelData.kind === "subtitle")
                        return bar.shownSubtitleTrack === modelData.index
                               ? "check" : ""
                    return ""
                }
                onTriggered: {
                    if (!bar.player) return
                    bar.reapplyBudget = 0
                    if (modelData.kind === "audio") {
                        bar.trackIntent.audio = modelData.index
                        bar.player.activeAudioTrack = modelData.index
                    } else if (modelData.kind === "subtitleOff") {
                        bar.trackIntent.subtitle = -1
                        bar.player.activeSubtitleTrack = -1
                    } else if (modelData.kind === "subtitle") {
                        bar.trackIntent.subtitle = modelData.index
                        bar.player.activeSubtitleTrack = modelData.index
                    }
                    bar.reapplyTracks()
                }
            }
        }
    }
    AppMenu {
        id: speedMenu
        objectName: "videoSpeedMenu"
        parent: speedButton
        menuWidth: 120
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        readonly property real expectedHeight:
            bar.rates.length * AppTheme.menuItemHeight + topPadding
            + bottomPadding
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
