import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

Rectangle {
    id: root

    property string filePath: ""
    property string mime: ""
    property real durationMs: 0
    property var waveform: []
    readonly property bool hasWaveform:
        root.waveform !== undefined && root.waveform !== null
        && root.waveform.length > 0
    property bool compact: false

    signal sendRequested()
    signal discardRequested()

    readonly property bool playing:
        preview.playbackState === MediaPlayer.PlayingState

    function gatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio)
            qtAudio.play(p)
        else
            p.play()
    }
    function cancelGatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio)
            qtAudio.cancelPlay(p)
    }

    function stopPlayback() {
        root.cancelGatedPlay(preview)
        preview.stop()
    }

    implicitHeight: compact ? 24 : 28
    implicitWidth: previewRow.implicitWidth + (compact ? 14 : 16)
    radius: AppTheme.radiusPill
    color: AppTheme.accentSoft
    border.color: AppTheme.accent
    border.width: 1

    // A position floors, a total rounds. At 25.7 s you have not reached 0:26,
    // so an elapsed clock floors (rounding would reach the total early); 25.7 s
    // of audio is 26 seconds long, matching embedDurationText. The recording
    // counters are positions and stay floored.
    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.round(ms / 1000))
    }
    function clockText(totalSeconds) {
        return AppTheme.clockText(totalSeconds)
    }

    MediaPlayer {
        id: preview
        source: root.filePath.length > 0
                ? "file://" + root.filePath : ""
        audioOutput: AudioOutput {
            volume: app.settings.mediaVolume
        }
    }
    onFilePathChanged: {
        root.cancelGatedPlay(preview)
        preview.stop()
    }

    RowLayout {
        id: previewRow
        anchors.centerIn: parent
        spacing: root.compact ? AppTheme.spacing6 : AppTheme.spacing8

        IconButton {
            objectName: "voicePreviewPlayButton"
            implicitWidth: root.compact ? 22 : 24
            implicitHeight: root.compact ? 22 : 24
            iconName: root.playing ? "pause" : "play_arrow"
            iconSize: root.compact ? 14 : 15
            Accessible.name: root.playing ? qsTr("Pause the preview")
                                          : qsTr("Play the recording back")
            ToolTip.text: root.playing ? qsTr("Pause the preview")
                                       : qsTr("Play the recording back")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: {
                if (root.playing) {
                    root.cancelGatedPlay(preview)
                    preview.pause()
                } else {
                    root.gatedPlay(preview)
                }
            }
        }
        Item {
            objectName: "voicePreviewWave"
            visible: root.hasWaveform
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: root.compact ? 44 : 56
            implicitHeight: root.compact ? 12 : 14

            Row {
                id: waveRow
                objectName: "voicePreviewWaveRow"
                anchors.fill: parent
                spacing: 1
                readonly property int barCount: Math.max(
                    1, Math.min(root.hasWaveform ? root.waveform.length : 1,
                                Math.floor(width / 3)))
                readonly property real progress:
                    preview.duration > 0
                    ? preview.position / preview.duration : 0
                Repeater {
                    model: waveRow.barCount
                    delegate: Rectangle {
                        required property int index
                        readonly property real amp: {
                            var wf = root.waveform
                            if (!wf || wf.length === 0)
                                return 0.12
                            var at = Math.floor(
                                index * wf.length / waveRow.barCount)
                            return Math.max(0.12,
                                            Math.min(1, wf[at] / 100))
                        }
                        width: 2
                        anchors.verticalCenter: parent.verticalCenter
                        height: Math.max(1, parent.height * amp)
                        radius: 1
                        color: (index / waveRow.barCount) <= waveRow.progress
                               ? AppTheme.text : AppTheme.textMuted
                    }
                }
            }
            TapHandler {
                gesturePolicy: TapHandler.WithinBounds
                enabled: preview.seekable && preview.duration > 0
                onTapped: (eventPoint) => {
                    preview.position = preview.duration
                        * Math.max(0, Math.min(1, eventPoint.position.x
                                                  / waveRow.width))
                }
            }
            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("Recording waveform")
        }
        TextMetrics {
            id: timeWidest
            font: timeLabel.font
            text: {
                var total = root.durationMs > 0 ? root.durationMs
                                                : preview.duration
                var t = root.formatDuration(total).replace(/[0-9]/g, "8")
                return t + " / " + t
            }
        }
        Label {
            id: timeLabel
            objectName: "voicePreviewTime"
            Layout.preferredWidth: Math.ceil(timeWidest.advanceWidth)
            horizontalAlignment: Text.AlignRight
            text: {
                var total = root.durationMs > 0 ? root.durationMs
                                                : preview.duration
                if (preview.position > 0 && preview.position < total)
                    return root.formatPosition(preview.position) + " / "
                           + root.formatDuration(total)
                return root.formatDuration(total)
            }
            color: AppTheme.text
            font.pixelSize: root.compact ? 11 : 12
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
        }
        IconButton {
            objectName: "voicePreviewDiscardButton"
            implicitWidth: root.compact ? 22 : 24
            implicitHeight: root.compact ? 22 : 24
            iconName: "delete"
            iconSize: root.compact ? 14 : 15
            Accessible.name: qsTr("Discard the recording")
            ToolTip.text: qsTr("Discard")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: {
                root.cancelGatedPlay(preview)
                preview.stop()
                root.discardRequested()
            }
        }
        IconButton {
            objectName: "voicePreviewSendButton"
            implicitWidth: root.compact ? 22 : 24
            implicitHeight: root.compact ? 22 : 24
            fill: true
            iconName: "send"
            iconSize: root.compact ? 13 : 14
            Accessible.name: qsTr("Send the voice message")
            ToolTip.text: qsTr("Send")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: {
                root.cancelGatedPlay(preview)
                preview.stop()
                root.sendRequested()
            }
        }
    }
}
