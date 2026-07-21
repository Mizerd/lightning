import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// v0.7: inline audio / voice-message player. A stable compact card:
// pressing Play fetches the decrypted payload through MediaBridge's
// validated playable materialization, then plays the session-scoped temp
// file in-process. Voice messages render their real MSC3245 waveform when
// the event carried one — never a fabricated decoration — and fall back
// to the plain progress slider otherwise. One audible card at a time via
// app.playback; room/account switches force a stop.
Rectangle {
    id: root
    objectName: "audioPlayerCard"

    property string mediaKey: ""
    property string ownerKey: ""
    property string filename: ""
    property string mimetype: ""
    property real fileSize: 0
    property real durationMs: 0
    property bool isVoice: false
    // Normalized MSC3245 amplitudes (0..1) — empty when the event had none.
    property var waveform: []
    property bool rowOnScreen: true
    property bool canSave: false

    signal saveRequested()
    // Non-bridge backends (plain HTTP media) keep their external-open path;
    // the delegate wires this to app.media.openExternal.
    signal openExternalRequested()

    readonly property bool playing:
        player.playbackState === MediaPlayer.PlayingState
    readonly property bool ready: player.source.toString().length > 0
    property string fetchState: "idle" // idle / fetching / failed

    function formatMs(ms) {
        if (!ms || ms < 0) ms = 0
        var total = Math.floor(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function togglePlay() {
        if (!app.mediaBridge.supported) {
            root.openExternalRequested()
            return
        }
        if (playing) {
            player.pause()
            return
        }
        if (fetchState === "failed")
            fetchState = "idle" // explicit user retry
        if (ready) {
            app.playback.acquire(root.ownerKey)
            player.play()
            return
        }
        var url = app.mediaBridge.playableSource(root.mediaKey)
        if (url.length > 0) {
            fetchState = "idle"
            player.source = url
            app.playback.acquire(root.ownerKey)
            player.play()
        } else {
            fetchState = "fetching"
        }
    }
    function resetPlayback() {
        player.stop()
        player.source = ""
        fetchState = "idle"
        app.playback.release(root.ownerKey)
    }
    onMediaKeyChanged: resetPlayback() // delegate reuse safety
    onRowOnScreenChanged: if (!rowOnScreen && playing) player.pause()
    Component.onDestruction: app.playback.release(root.ownerKey)

    Connections {
        target: app.mediaBridge
        function onPlayableMediaReady(cacheKey) {
            if (root.fetchState !== "fetching") return
            var url = app.mediaBridge.playableSource(root.mediaKey)
            if (url.length === 0) return
            root.fetchState = "idle"
            player.source = url
            app.playback.acquire(root.ownerKey)
            player.play()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (root.fetchState === "fetching") root.fetchState = "failed"
        }
    }
    Connections {
        target: app.playback
        function onAudibleOwnerChanged() {
            if (!app.playback.owns(root.ownerKey) && root.playing)
                player.pause()
        }
    }

    MediaPlayer {
        id: player
        audioOutput: AudioOutput {
            id: audioOut
            muted: muteButton.mutedState
        }
        onErrorOccurred: root.fetchState = "failed"
    }

    implicitWidth: Math.min(360, bubble ? bubble.width : 360)
    implicitHeight: cardRow.implicitHeight + 12
    color: AppTheme.surfaceElevated
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: 1
    // The delegate provides `bubble`; standalone use (tests) tolerates null.
    property var bubble: null

    RowLayout {
        id: cardRow
        anchors.fill: parent
        anchors.margins: 6
        spacing: 8

        IconButton {
            objectName: "audioPlayPauseButton"
            fill: true
            implicitWidth: 30; implicitHeight: 30
            iconName: root.fetchState === "fetching"
                      ? "schedule"
                      : (root.playing ? "pause"
                         : (root.isVoice && !root.ready ? "mic" : "play_arrow"))
            iconSize: 17
            enabled: root.fetchState !== "fetching"
            Accessible.name: root.playing
                ? qsTr("Pause %1").arg(root.isVoice
                                       ? qsTr("voice message") : root.filename)
                : qsTr("Play %1").arg(root.isVoice
                                      ? qsTr("voice message") : root.filename)
            onClicked: root.togglePlay()
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: root.fetchState === "failed"
                      ? qsTr("This audio cannot be played")
                      : root.isVoice ? qsTr("Voice message")
                                     : (root.filename || qsTr("Audio"))
                color: root.fetchState === "failed"
                       ? AppTheme.danger : AppTheme.text
                font.pixelSize: 12
                font.weight: Font.Medium
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
                            readonly property real amp: {
                                var wf = root.waveform
                                var at = Math.floor(
                                    index * wf.length / waveRow.barCount)
                                return Math.max(0.12, Math.min(1, wf[at]))
                            }
                            readonly property real progress:
                                player.duration > 0
                                ? player.position / player.duration : 0
                            width: 2
                            anchors.verticalCenter: parent.verticalCenter
                            height: parent.height * amp
                            radius: 1
                            color: (index / waveRow.barCount) <= progress
                                   ? AppTheme.accent : AppTheme.borderStrong
                        }
                    }
                    TapHandler {
                        enabled: player.seekable
                        onTapped: (eventPoint) => {
                            player.position = player.duration
                                * (eventPoint.position.x / waveRow.width)
                        }
                    }
                }

                Slider {
                    anchors.fill: parent
                    visible: !waveRow.visible
                    from: 0
                    to: Math.max(1, player.duration > 0 ? player.duration
                                                        : root.durationMs)
                    enabled: player.seekable
                    value: pressed ? value : player.position
                    Accessible.name: qsTr("Seek position")
                    onMoved: player.position = value
                }
            }

            Label {
                text: {
                    var pos = root.formatMs(player.position)
                    var total = root.formatMs(
                        player.duration > 0 ? player.duration : root.durationMs)
                    var line = root.ready ? pos + " / " + total : total
                    if (!root.isVoice && root.fileSize > 0) {
                        var kb = root.fileSize / 1024
                        line += " • " + (kb < 1024 ? kb.toFixed(0) + " KB"
                                        : (kb / 1024).toFixed(1) + " MB")
                    }
                    return line
                }
                color: AppTheme.textMuted
                font.pixelSize: 10
            }
        }

        // Playback speed — voice messages benefit most. Cycles the standard
        // rates; per-session only, resets to 1x with the card.
        AbstractButton {
            id: audioSpeedButton
            objectName: "audioSpeedButton"
            readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
            property int rateIndex: 2
            visible: root.ready
            implicitWidth: 32; implicitHeight: 24
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(rates[rateIndex])
            onClicked: {
                rateIndex = (rateIndex + 1) % rates.length
                player.playbackRate = rates[rateIndex]
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: audioSpeedButton.hovered ? AppTheme.hover : "transparent"
                border.width: audioSpeedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: audioSpeedButton.rates[audioSpeedButton.rateIndex] + "×"
                color: audioSpeedButton.rateIndex === 2
                       ? AppTheme.textMuted : AppTheme.accent
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        IconButton {
            id: muteButton
            objectName: "audioMuteButton"
            property bool mutedState: false
            iconName: mutedState ? "volume_off" : "volume_up"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.ready
            Accessible.name: mutedState ? qsTr("Unmute") : qsTr("Mute")
            onClicked: mutedState = !mutedState
        }
        IconButton {
            objectName: "audioSaveButton"
            iconName: "download"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.canSave
            Accessible.name: qsTr("Save %1 as…")
                .arg(root.filename || qsTr("audio"))
            onClicked: root.saveRequested()
        }
    }
}
