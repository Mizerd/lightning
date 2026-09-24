import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Review step between finishing a voice recording and sending it: play it back,
// discard it, or send it. The clip is a local file from the recorder, played
// from disk (the media bridge is for received media). The host owns the file:
// sending hands it to the send queue; discarding asks AppController to delete
// it (only paths the recorder produced).
Rectangle {
    id: root

    property string filePath: ""
    property string mime: ""
    property real durationMs: 0
    // The recorder's amplitude buckets (0..=100, VoiceRecorder.h), the same
    // list sent as the MSC3245 waveform. Empty when the clip could not be
    // decoded; the strip is then hidden.
    property var waveform: []
    readonly property bool hasWaveform:
        root.waveform !== undefined && root.waveform !== null
        && root.waveform.length > 0
    // Smaller chrome for the thread composer.
    property bool compact: false

    signal sendRequested()
    signal discardRequested()

    readonly property bool playing:
        preview.playbackState === MediaPlayer.PlayingState

    function stopPlayback() {
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
        var m = Math.floor(totalSeconds / 60)
        var s = totalSeconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    MediaPlayer {
        id: preview
        source: root.filePath.length > 0
                ? "file://" + root.filePath : ""
        audioOutput: AudioOutput {
            volume: app.settings.mediaVolume
        }
    }
    // Stop once the clip is gone (sent or discarded).
    onFilePathChanged: preview.stop()

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
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: {
                if (root.playing)
                    preview.pause()
                else
                    preview.play()
            }
        }
        // The waveform, with the played part inked and tap-to-seek, like
        // AudioPlayerCard's received-voice strip.
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
                // One bar per 3px, never more than the buckets.
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
                        // Buckets are 0..=100, not 0..1.
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
                        // The pill's own label inks, not the card's: this sits
                        // on accentSoft, where AudioPlayerCard's
                        // accent/borderStrong pair is invisible on some themes.
                        // theVoicePreviewWaveformReadsOnEveryTheme holds the
                        // floors.
                        color: (index / waveRow.barCount) <= waveRow.progress
                               ? AppTheme.text : AppTheme.textMuted
                    }
                }
            }
            TapHandler {
                // WithinBounds, so a tap seeks and does not reach the
                // composer's handlers under the pill.
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
        Label {
            objectName: "voicePreviewTime"
            text: {
                // While playing, the position; otherwise the full length.
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
                preview.stop()
                root.sendRequested()
            }
        }
    }
}
