import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// VoicePreviewBar — the review step between finishing a voice recording and
// sending it.
//
// 2026-08-18 tester report: "kai sendini audio messages nera pause arba done
// mygtuko ir preview yra tik send ir delete". The recording pill now has a
// pause and a Done button, and Done lands here: the finished clip can be
// PLAYED BACK before it is sent, discarded, or sent.
//
// The clip is a local file the recorder produced and handed over with
// ready(); it is played straight from disk (nothing here goes near the media
// bridge, which exists for RECEIVED media). Whoever hosts this bar owns the
// file: sending transfers it to the send queue, discarding asks
// AppController to delete it (which only accepts a path the recorder itself
// produced).
Rectangle {
    id: root

    property string filePath: ""
    property string mime: ""
    property real durationMs: 0
    property var waveform: []
    // Smaller chrome for the thread composer, exactly like the pill.
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

    // ── A POSITION FLOORS, A TOTAL ROUNDS, AND THEY ARE NOT ONE CLOCK ──
    //
    // Two different quantities shared one formatter in every player here,
    // which is why they could never be made consistent. They pull opposite
    // ways:
    //
    //   * a POSITION is elapsed time. At 25.7 s you have not reached 0:26,
    //     and a clock that says you have is claiming time that has not
    //     passed. It would also hit the total a half-second before the
    //     audio ends and sit there.
    //   * a TOTAL is a length. 25.7 s of audio IS 26 seconds to the nearest
    //     second, which is what `embedDurationText` on the collapsed
    //     summary line has always said, and a total that floors reads a
    //     second short of the clip.
    //
    // Rounding BOTH (which this file briefly did) fixed the summary-line
    // disagreement and broke the position clock. Flooring both, the state
    // before that, made the card disagree with the one-line summary that
    // opens it. Naming them apart is the only thing that makes every
    // surface agree, and the reason the old comment here was wrong on both
    // counts: the video card does NOT round, and the recording counter
    // floors on purpose.
    //
    // The two RECORDING counters stay floored and are not this rule's
    // business: a counter running while you speak is a position.
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
    // Nothing may keep playing once the clip is gone (sent or discarded).
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
