import QtQuick
import QtQuick.Controls
import MatrixClient

// The speed chip of the "now playing" strip and the popped-out card: one
// click steps 1x -> 1.5x -> 2x -> 1x. It shows the clip's own speed
// (app.voicePlayback.rate): the remembered voice speed for a voice message, the
// file's own (1x until changed) for an audio file. Accent while not 1x, so a
// speed left on from earlier is visible at a glance (design-media U3).
AbstractButton {
    id: root
    objectName: "voiceSpeedChip"

    readonly property var voice: app.voicePlayback
    // The surface it sits on, so its ink is guarded to 4.5:1 there.
    property color surface: AppTheme.cardElevated
    readonly property real rate: root.voice.rate
    readonly property bool normal: Math.abs(root.rate - 1.0) < 0.001

    implicitWidth: Math.max(38, chipLabel.implicitWidth + 12)
    implicitHeight: 24
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    enabled: root.voice.active

    VoicePlayerLogic { id: logic }

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Playback speed %1. Change speed")
        .arg(logic.rateText(root.rate))
    // Says what a click does: the speed now and the one it steps to.
    ToolTip.text: qsTr("Playback speed %1, click for %2")
        .arg(logic.rateText(root.rate))
        .arg(logic.rateText(logic.nextRate(root.rate)))
    ToolTip.visible: root.hovered
    ToolTip.delay: 600
    onClicked: logic.cycleRate()
    // Space is the AbstractButton's own; Return and Enter act too, as on
    // every other button (C21-F01).
    Keys.onReturnPressed: (event) => { event.accepted = true; if (root.enabled) root.clicked() }
    Keys.onEnterPressed: (event) => { event.accepted = true; if (root.enabled) root.clicked() }

    background: Rectangle {
        radius: height / 2
        color: root.hovered ? AppTheme.hover : "transparent"
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? AppTheme.focusRing
                      : (root.normal ? AppTheme.border : AppTheme.accent)
    }
    contentItem: Label {
        id: chipLabel
        text: logic.rateText(root.rate)
        color: logic.readable(root.normal ? AppTheme.textSecondary
                                          : AppTheme.link, root.surface)
        font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
        font.weight: AppTheme.weightBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
