import QtQuick
import MatrixClient

Item {
    id: root

    property real level: 0
    property bool active: true
    property int segments: 24

    implicitWidth: 200
    implicitHeight: 8

    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Microphone level")
    // Qt Quick has no Accessible.value; the description carries the reading.
    Accessible.description: !root.active
        ? qsTr("Not listening")
        : (root.shown <= 0 ? qsTr("Nothing heard")
                           : qsTr("%1 percent").arg(Math.round(root.shown * 10) * 10))

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
