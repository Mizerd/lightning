import QtQuick
import MatrixClient

// A microphone level bar: the in-call device menu (Teams' bar under the
// microphone) and the Settings microphone test (Discord's "Let's check").
//
// `level` is 0..1 on the scale of src/calls/AudioLevelMeter.h (-60..0 dBFS).
// Drawn as segments so a still bar reads as "nothing heard" rather than as a
// progress bar, with the top segments in the warning colour, where the voice
// is close to clipping.
//
// Presentation only: the value comes from C++ (app.groupCall.microphoneLevel
// or app.audioTester.level), already rate-limited there.
Item {
    id: root

    /// 0..1.
    property real level: 0
    /// False draws every segment unlit (muted, not running).
    property bool active: true
    property int segments: 24

    implicitWidth: 200
    implicitHeight: 8

    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Microphone level")

    // Decays rather than snapping down, so a single word stays visible; rises
    // at once, so a word is never late.
    property real shown: 0
    readonly property real target: root.active
                                   ? Math.max(0, Math.min(1, root.level)) : 0
    onTargetChanged: {
        if (root.target >= root.shown)
            root.shown = root.target
        else if (!decay.running)
            decay.start()
    }
    Timer {
        id: decay
        interval: 33
        repeat: true
        onTriggered: {
            // About 0.9 of the bar per second.
            root.shown = Math.max(root.target, root.shown - 0.03)
            if (root.shown <= root.target)
                stop()
        }
    }

    Row {
        anchors.fill: parent
        spacing: 2

        Repeater {
            model: root.segments
            delegate: Rectangle {
                required property int index
                readonly property real threshold: (index + 0.5) / root.segments
                readonly property bool lit: root.shown >= threshold
                // The top sixth is close to 0 dBFS.
                readonly property bool hot: index >= Math.round(root.segments * 5 / 6)

                width: (root.width - (root.segments - 1) * 2) / root.segments
                height: root.height
                radius: Math.min(2, height / 2)
                color: lit ? (hot ? AppTheme.warning : AppTheme.success)
                           : AppTheme.stormInset
            }
        }
    }
}
