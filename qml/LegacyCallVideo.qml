import QtQuick
import QtMultimedia
import MatrixClient

// The picture of a 1:1 (`m.call.*`) video call: the peer filling the panel,
// our own in the corner. MatrixRTC has CallStage; this lane has one peer and
// needs none of that. The engine hands frames to the two sinks; a VideoOutput
// lives as long as this item, so each attaches once.
Rectangle {
    id: root
    objectName: "legacyCallVideo"
    color: AppTheme.stormInset
    clip: true

    readonly property bool remoteVisible: app.calls.remoteVideoVisible

    VideoOutput {
        id: remoteOutput
        objectName: "legacyCallRemoteVideo"
        anchors.fill: parent
        visible: root.remoteVisible
        fillMode: VideoOutput.PreserveAspectFit
        Component.onCompleted: app.calls.attachVideoSink(false, videoSink)
    }

    Text {
        objectName: "legacyCallNoPicture"
        anchors.centerIn: parent
        width: Math.min(implicitWidth, parent.width - AppTheme.spacing16 * 2)
        visible: !root.remoteVisible
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Their camera is off")
    }

    Rectangle {
        objectName: "legacyCallSelfView"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: AppTheme.spacing12
        width: Math.max(96, Math.round(root.width * 0.2))
        height: Math.round(width * 9 / 16)
        radius: AppTheme.radiusSm
        clip: true
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder
        visible: app.calls.cameraOn

        VideoOutput {
            id: selfOutput
            objectName: "legacyCallSelfVideo"
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectCrop
            Component.onCompleted: app.calls.attachVideoSink(true, videoSink)
        }
    }
}
