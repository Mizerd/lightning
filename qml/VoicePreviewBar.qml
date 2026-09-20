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
    // The recorder's own amplitude buckets, 0..=100 (VoiceRecorder.h), the
    // SAME list that goes on the wire as MSC3245 `waveform` — so what the
    // preview draws is what the recipient will see. Empty when the clip
    // could not be decoded, which is a real case the recorder documents;
    // the strip simply does not appear then.
    property var waveform: []
    readonly property bool hasWaveform:
        root.waveform !== undefined && root.waveform !== null
        && root.waveform.length > 0
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
        // THE WAVEFORM, WHICH THIS BAR TOOK AND NEVER DREW.
        //
        // `waveform` has been declared here and bound by both hosts since
        // the preview shipped, and `grep waveform VoicePreviewBar.qml`
        // returned the declaration and nothing else — the same shape as
        // refreshIndexStats() with no caller. Drawn now, from the recorder's
        // own buckets, with the played part inked like AudioPlayerCard's
        // received-voice strip and the same tap-to-seek, because a review
        // step whose whole job is "listen to this before you send it" is
        // exactly where a scrub surface belongs.
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
                // One bar per 3 px, never more than the buckets we have.
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
                        // The buckets are 0..=100, NOT 0..1. Dividing is the
                        // whole difference between a waveform and a solid
                        // block of full-height bars.
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
                        // THE PILL'S OWN INKS, NOT THE CARD'S.
                        //
                        // AudioPlayerCard draws its received-voice strip
                        // `accent` over `borderStrong` — correct there,
                        // because that strip sits on a CARD. This one sits
                        // on the preview pill, whose fill is `accentSoft`,
                        // and measured against that fill across all seven
                        // palettes that define their own accent the card's
                        // pair collapses: borderStrong reaches 1.04:1 on
                        // Nordic and 1.06:1 on Graphite (invisible), and
                        // accent itself only 1.52:1 on Graphite. The pill's
                        // own label inks clear it everywhere — text 5.4 to
                        // 14.3:1, textMuted 2.5 to 5.5:1, and 19 to 33 dL*
                        // apart from each other, which is what makes played
                        // and unplayed tell each other apart.
                        // theVoicePreviewWaveformReadsOnEveryTheme holds
                        // those floors.
                        color: (index / waveRow.barCount) <= waveRow.progress
                               ? AppTheme.text : AppTheme.textMuted
                    }
                }
            }
            TapHandler {
                // WithinBounds, so a tap on the strip seeks and does not
                // also reach whatever sits under the pill (the composer's
                // own handlers) — the recorded TapHandler rule.
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
