import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Inline audio / voice-message player. A VIEW of the one app-owned player
// (app.voicePlayback): this card holds no MediaPlayer of its own, because rows
// are destroyed and rebuilt on every room switch and a clip must keep playing
// through one. The card whose eventId is the player's current clip shows the
// live state (playing, position, waveform progress) and drives it; every other
// card shows idle. Play fetches the decrypted payload through MediaBridge's
// validated playable materialization (inside the controller). Voice messages
// show their real MSC3245 waveform when the event has one (never a fabricated
// one), else a progress slider.
Rectangle {
    id: root
    objectName: "audioPlayerCard"

    property string mediaKey: ""
    // The event this card plays. The player is keyed by it, so a rebuilt row
    // (room switch, scroll, thread panel) finds its live state again.
    property string eventId: ""
    // The room the event is in, and where the mini-player takes the reader
    // back to.
    property string roomId: ""
    property string senderName: ""
    property string senderId: ""
    property string senderAvatarMxc: ""
    // The thread the row is shown in ("" for the main timeline).
    property string threadRootId: ""
    property string filename: ""
    property string mimetype: ""
    property real fileSize: 0
    property real durationMs: 0
    property bool isVoice: false
    // Normalized MSC3245 amplitudes; empty when the event had none.
    property var waveform: []
    property bool rowOnScreen: true
    // Speculative prefetch gate, separate from rowOnScreen: a row that only
    // swept past during a gesture must not pull a payload. Permissive default
    // for standalone hosts.
    property bool prefetchAllowed: true
    property bool canSave: false

    signal saveRequested()
    // Non-bridge backends (plain HTTP media) open externally
    // (app.media.openExternal).
    signal openExternalRequested()

    readonly property var voice: app.voicePlayback
    // This card is the player's current clip.
    readonly property bool isCurrent:
        root.eventId.length > 0 && root.voice.active
        && root.voice.eventId === root.eventId
    // The shared player while this card is current; null otherwise, so the
    // metadata and volume bindings below read nothing for an idle card.
    readonly property var player: root.isCurrent ? root.voice.player : null
    readonly property bool playing: root.isCurrent && root.voice.playing
    readonly property bool ready: root.isCurrent && root.voice.loaded
    readonly property string fetchState:
        root.isCurrent ? root.voice.fetchState : "idle"
    readonly property real livePosition:
        root.ready ? root.voice.position : 0
    readonly property real liveDuration:
        root.ready && root.voice.duration > 0 ? root.voice.duration
                                              : root.durationMs
    readonly property bool seekable: root.ready && root.voice.seekable
    // Embedded cover art, available once the backend has opened the file.
    // MediaBridge keeps the decoded pixels in a small RAM-only LRU; no
    // decrypted artwork is written to disk.
    property string artworkSource: ""

    function refreshArtwork() {
        artworkSource = ""
        if (!player || isVoice || mediaKey.length === 0)
            return
        var artwork = player.metaData.value(MediaMetaData.CoverArtImage)
        if (!artwork)
            artwork = player.metaData.value(MediaMetaData.ThumbnailImage)
        if (artwork)
            artworkSource = app.mediaBridge.audioArtworkSource(mediaKey,
                                                                artwork)
    }
    onPlayerChanged: refreshArtwork()
    Connections {
        target: root.player
        enabled: root.player !== null
        function onMetaDataChanged() { root.refreshArtwork() }
    }

    // Position floors, total rounds. At 25.7 s you haven't reached 0:26, but a
    // 25.7 s clip is 26 s long (matching `embedDurationText`). Recording
    // counters are positions and floor too.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function togglePlay() {
        if (!app.mediaBridge.supported) {
            root.openExternalRequested()
            return
        }
        // Another clip stops first; this one resumes where it was.
        root.voice.toggle(root.eventId, root.roomId, root.mediaKey, {
            "senderName": root.senderName,
            "senderId": root.senderId,
            "senderAvatarMxc": root.senderAvatarMxc,
            "filename": root.filename,
            "isVoice": root.isVoice,
            "durationMs": root.durationMs,
            "threadRootId": root.threadRootId
        })
    }
    function seekTo(ms) {
        if (root.isCurrent)
            root.voice.seek(ms)
    }
    // Bounded speculative prefetch (size-capped, lowest priority, deduplicated
    // by MediaBridge) so Play starts without a download wait.
    function maybePrefetch() {
        // GIF autoplay "never" also means no passive media downloads.
        if (rowOnScreen && prefetchAllowed && mediaKey.length > 0
            && app.mediaBridge.supported
            && app.settings.gifAutoplay !== 2)
            app.mediaBridge.prefetchPlayable(
                mediaKey,
                fileSize || app.settings.knownMediaSizeBytes(mediaKey) || 0)
    }
    onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()
    Component.onCompleted: maybePrefetch()
    onMediaKeyChanged: maybePrefetch()
    // Scrolling a playing row away no longer pauses it: the clip belongs to
    // the app; the row shows it again when scrolled back, and the floating
    // mini-player carries it while the room is elsewhere.
    onRowOnScreenChanged: if (rowOnScreen) maybePrefetch()

    implicitWidth: Math.min(
        360, root.hostContentWidth >= 0
             ? root.hostContentWidth
             : (bubble ? bubble.width : 360))
    implicitHeight: cardRow.implicitHeight + 12
                    + (coverArtBox.visible
                       ? coverArtBox.implicitHeight + 6 : 0)
    color: AppTheme.embedSurface
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: 1
    // Legacy width source for standalone use (tests may leave it null). Its
    // outer width includes the bubble's padding, so the delegate passes
    // `hostContentWidth` (the content column's inner width) instead.
    property var bubble: null
    /// The width the host allows; negative means unset. Not named
    /// `availableWidth`, which coverArtBox already uses.
    property real hostContentWidth: -1

    // Embedded cover art, as a box attached below the controls, corner-matched
    // and clipped. RAM-only (see artworkSource).
    Rectangle {
        id: coverArtBox
        objectName: "audioCoverArtBox"
        visible: root.artworkSource.length > 0
        anchors.left: parent.left
        anchors.top: cardRow.bottom
        anchors.leftMargin: 6
        anchors.topMargin: 6
        // The box takes the artwork's own aspect. Both axes are bounded, with
        // the width derived from the capped height, so box and artwork keep the
        // same shape at every size.
        readonly property real maxEdge: 420
        readonly property real availableWidth: Math.max(1, root.width - 12)
        // Height per unit width; a fallback ratio until the image reports its
        // size.
        readonly property real artRatio: {
            var iw = coverArtImage.implicitWidth
            var ih = coverArtImage.implicitHeight
            return (iw > 0 && ih > 0) ? (ih / iw) : 0.62
        }
        implicitWidth: visible
            ? Math.round(Math.min(availableWidth, maxEdge / artRatio)) : 0
        implicitHeight: visible
            ? Math.round(Math.min(availableWidth * artRatio, maxEdge)) : 0
        width: implicitWidth
        height: implicitHeight
        radius: AppTheme.radiusMd
        color: AppTheme.embedSurface
        border.width: 1
        border.color: AppTheme.border
        clip: true
        Image {
            id: coverArtImage
            objectName: "audioCoverArtwork"
            anchors.fill: parent
            source: coverArtBox.visible ? root.artworkSource : ""
            // Width only; constraining both would letterbox the source.
            sourceSize.width: 640
            // The box matches the aspect, so Fit shows the whole cover.
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            Accessible.name: qsTr("Audio cover artwork")
        }
    }

    RowLayout {
        id: cardRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        spacing: 8

        IconButton {
            objectName: "audioPlayPauseButton"
            fill: true
            // Click focus, so Space after Play toggles the clip.
            focusPolicy: Qt.StrongFocus
            implicitWidth: 30; implicitHeight: 30
            // Always a play glyph when idle (a mic read as "record"); the voice
            // label and waveform identify voice messages.
            iconName: root.fetchState === "fetching"
                      ? "schedule"
                      : (root.playing ? "pause" : "play_arrow")
            iconSize: 17
            enabled: root.fetchState !== "fetching"
            readonly property string actionLabel: root.playing
                ? qsTr("Pause %1").arg(root.isVoice
                                       ? qsTr("voice message") : root.filename)
                : qsTr("Play %1").arg(root.isVoice
                                      ? qsTr("voice message") : root.filename)
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.togglePlay()
        }


        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.fetchState === "failed"
                      ? qsTr("This audio cannot be played")
                      : root.isVoice ? qsTr("Voice message")
                                     : (root.filename || qsTr("Audio"))
                color: root.fetchState === "failed"
                       ? AppTheme.danger : AppTheme.text
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
                elide: Label.ElideMiddle
                Layout.fillWidth: true
            }

            // Seek surface: real waveform bars when MSC3245 data exists,
            // otherwise a slim progress slider. Both are seekable.
            Item {
                Layout.fillWidth: true
                implicitHeight: 18
                visible: root.fetchState !== "failed"

                Row {
                    id: waveRow
                    objectName: "audioWaveRow"
                    anchors.fill: parent
                    visible: root.isVoice && root.waveform
                             && root.waveform.length > 0
                    spacing: 1
                    property int barCount: Math.max(
                        1, Math.min(root.waveform ? root.waveform.length : 0,
                                    Math.floor(width / 3)))
                    Repeater {
                        model: waveRow.visible ? waveRow.barCount : 0
                        delegate: Rectangle {
                            required property int index
                            // Buckets are 0..=100 (rust/src/timeline.rs
                            // downsample_waveform; RustTimelineIngest.cpp drops
                            // anything outside), so divide; min() guards a
                            // malformed bucket.
                            readonly property real amp: {
                                var wf = root.waveform
                                var at = Math.floor(
                                    index * wf.length / waveRow.barCount)
                                return Math.max(0.12,
                                                Math.min(1, wf[at] / 100))
                            }
                            readonly property real progress:
                                root.ready && root.liveDuration > 0
                                ? root.livePosition / root.liveDuration
                                : 0
                            width: 2
                            anchors.verticalCenter: parent.verticalCenter
                            height: parent.height * amp
                            radius: 1
                            color: (index / waveRow.barCount) <= progress
                                   ? AppTheme.accent : AppTheme.borderStrong
                        }
                    }
                    TapHandler {
                        enabled: root.seekable
                        onTapped: (eventPoint) => {
                            root.seekTo(root.liveDuration
                                        * (eventPoint.position.x
                                           / waveRow.width))
                        }
                    }
                }

                Slider {
                    id: seekSlider
                    anchors.fill: parent
                    visible: !waveRow.visible
                    from: 0
                    to: Math.max(1, root.liveDuration)
                    enabled: root.seekable
                    value: pressed ? value : root.livePosition
                    Accessible.name: qsTr("Seek position")
                    onMoved: root.seekTo(value)
                    // A slim track and 12px handle; Basic's 28px handle was
                    // clipped by the 18px row.
                    padding: 0
                    background: Rectangle {
                        x: seekSlider.leftPadding
                        y: seekSlider.topPadding
                            + seekSlider.availableHeight / 2 - height / 2
                        width: seekSlider.availableWidth
                        height: 4
                        radius: 2
                        color: AppTheme.borderStrong
                        Rectangle {
                            width: seekSlider.visualPosition * parent.width
                            height: parent.height
                            radius: parent.radius
                            color: AppTheme.accent
                        }
                    }
                    handle: Rectangle {
                        x: seekSlider.leftPadding
                           + seekSlider.visualPosition
                             * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding
                            + seekSlider.availableHeight / 2 - height / 2
                        width: 12
                        height: 12
                        radius: 6
                        color: AppTheme.accent
                        border.width: 2
                        border.color: AppTheme.surfaceElevated
                    }
                }
            }

            Label {
                text: {
                    var pos = root.formatPosition(root.livePosition)
                    var total = root.formatDuration(root.liveDuration)
                    var line = root.ready ? pos + " / " + total : total
                    if (!root.isVoice && root.fileSize > 0) {
                        var kb = root.fileSize / 1024
                        line += " • " + (kb < 1024 ? kb.toFixed(0) + " KB"
                                        : (kb / 1024).toFixed(1) + " MB")
                    }
                    return line
                }
                // Elides inside the card.
                Layout.fillWidth: true
                elide: Label.ElideRight
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        // Playback speed: opens a list, and the choice is remembered
        // (app.settings.mediaPlaybackRate) across cards, rooms and restarts.
        AbstractButton {
            id: audioSpeedButton
            objectName: "audioSpeedButton"
            readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
            readonly property real rate: app.settings.mediaPlaybackRate
            visible: root.ready
            implicitWidth: 34; implicitHeight: 24
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(rate)
            ToolTip.text: qsTr("Playback speed")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: speedMenu.popup()
            AppMenu {
                id: speedMenu
                objectName: "audioSpeedMenu"
                menuWidth: 140
                Repeater {
                    model: audioSpeedButton.rates
                    AppMenuItem {
                        required property real modelData
                        text: modelData + "\u00d7"
                        iconName: Math.abs(modelData - audioSpeedButton.rate)
                                  < 0.001 ? "check" : ""
                        // Write the setting only: the app-owned player
                        // follows it (VoicePlaybackController).
                        onTriggered: app.settings.mediaPlaybackRate = modelData
                    }
                }
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: audioSpeedButton.hovered ? AppTheme.hover : "transparent"
                border.width: audioSpeedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: audioSpeedButton.rate + "×"
                color: Math.abs(audioSpeedButton.rate - 1.0) < 0.001
                       ? AppTheme.textMuted : AppTheme.accent
                font.pixelSize: AppTheme.textMicro
                font.weight: AppTheme.weightBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        MediaVolumeControl {
            objectName: "audioMuteButton"
            audio: root.isCurrent ? root.voice.audioOutput : null
            sliderObjectName: "audioVolumeSlider"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.ready
        }
        IconButton {
            objectName: "audioSaveButton"
            iconName: "download"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.canSave
            readonly property string actionLabel:
                qsTr("Download %1").arg(root.filename || qsTr("audio"))
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.saveRequested()
        }
    }
}
