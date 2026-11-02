import QtQuick
import QtQuick.Controls
import MatrixClient

Slider {
    id: root
    objectName: "voiceSeekBar"

    readonly property var voice: app.voicePlayback
    property bool followsPlayer: true
    property real idleDuration: 0
    property bool idleSeekable: false
    signal seekRequested(real ms)
    readonly property real shownPosition:
        root.followsPlayer ? root.voice.position : 0
    readonly property real shownDuration:
        root.followsPlayer && root.voice.duration > 0 ? root.voice.duration
                                                      : root.idleDuration
    function seekTo(ms) {
        if (root.followsPlayer)
            root.voice.seek(ms)
        else
            root.seekRequested(ms)
    }
    property real trackHeight: 3
    property color trackColor: AppTheme.borderStrong
    property color fillColor: AppTheme.accent
    property bool knobAlways: false
    property var waveform: []
    readonly property bool showsWaveform: root.waveform
                                          && root.waveform.length > 0

    function clock(ms, round) {
        if (!ms || ms < 0) ms = 0
        var t = round ? Math.round(ms / 1000) : Math.floor(ms / 1000)
        return AppTheme.clockText(t)
    }

    from: 0
    to: Math.max(1, root.shownDuration)
    // While the pointer holds the knob the bar shows where it is; otherwise
    // it follows the player. Internal (C++) writes during a drag or a key
    // step do not remove this binding.
    value: root.pressed ? root.value : root.shownPosition
    stepSize: 5000
    snapMode: Slider.NoSnap
    live: true
    enabled: root.followsPlayer
             ? (root.voice.active && root.voice.fetchState !== "failed")
             : root.idleSeekable
    focusPolicy: Qt.StrongFocus
    padding: 0
    implicitHeight: 14
    implicitWidth: 120
    hoverEnabled: true

    property real idleTarget: -1
    onMoved: {
        if (root.followsPlayer || !root.pressed)
            root.seekTo(root.value)
        else
            root.idleTarget = root.value
    }
    onPressedChanged: {
        if (root.pressed)
            return
        if (!root.followsPlayer && root.idleTarget >= 0)
            root.seekTo(root.idleTarget)
        root.idleTarget = -1
    }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Home) {
            root.seekTo(0)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            if (root.shownDuration > 0)
                root.seekTo(Math.max(0, root.shownDuration - 1))
            event.accepted = true
        }
    }

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Playback position")

    ToolTip.visible: root.hovered && !root.pressed && root.enabled
    ToolTip.delay: 600
    HoverHandler { id: hoverPoint }
    readonly property real hoverMs: root.availableWidth > 0
        ? Math.max(0, Math.min(1, (hoverPoint.point.position.x
                                   - root.leftPadding) / root.availableWidth))
          * root.shownDuration
        : 0
    ToolTip.text: root.shownDuration > 0
        ? qsTr("Seek to %1").arg(root.clock(root.hoverMs, false))
        : qsTr("Seek")

    background: Item {
        x: root.leftPadding
        y: root.topPadding
        width: root.availableWidth
        height: root.availableHeight
        Row {
            id: waveBars
            objectName: "voiceSeekWave"
            anchors.fill: parent
            visible: root.showsWaveform
            spacing: 1
            readonly property int count: root.showsWaveform
                ? Math.max(1, Math.floor((width + 1) / 3)) : 0
            readonly property real barWidth: count > 0
                ? Math.max(1, (width - (count - 1) * spacing) / count) : 0
            Repeater {
                model: waveBars.visible ? waveBars.count : 0
                delegate: Rectangle {
                    required property int index
                    readonly property real amp: {
                        var wf = root.waveform
                        var at = Math.floor(index * wf.length
                                            / waveBars.count)
                        return Math.max(0.12, Math.min(1, wf[at] / 100))
                    }
                    width: waveBars.barWidth
                    height: waveBars.height * amp
                    anchors.verticalCenter: parent.verticalCenter
                    radius: Math.min(1, width / 2)
                    color: (index / waveBars.count) < root.visualPosition
                           ? root.fillColor : root.trackColor
                }
            }
        }
        Rectangle {
            objectName: "voiceSeekTrack"
            visible: !root.showsWaveform
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: root.hovered || root.pressed || root.visualFocus
                    ? root.trackHeight + 2 : root.trackHeight
            radius: height / 2
            color: root.trackColor
            Rectangle {
                objectName: "voiceSeekFill"
                width: root.visualPosition * parent.width
                height: parent.height
                radius: parent.radius
                color: root.fillColor
            }
        }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 12
        height: 12
        radius: 6
        visible: root.enabled && (root.knobAlways || root.hovered
                                  || root.pressed || root.visualFocus)
        color: root.fillColor
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? AppTheme.focusRing
                                       : AppTheme.surfaceElevated
    }
}
