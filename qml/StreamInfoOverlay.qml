import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    objectName: "streamInfoOverlay"
    property var stats: []
    property var controller: null
    Timer {
        interval: 1000
        repeat: true
        running: root.visible && root.controller !== null
                 && root.controller.streamInfoVisible
        onRunningChanged: {
            if (running) {
                root.controller.resetStreamInfoRates()
                root.controller.refreshStreamInfo()
            }
        }
        onTriggered: root.controller.refreshStreamInfo()
    }
    implicitWidth: 460
    implicitHeight: content.implicitHeight + 2 * AppTheme.spacing12
    color: Qt.rgba(AppTheme.surfaceElevated.r, AppTheme.surfaceElevated.g,
                   AppTheme.surfaceElevated.b, 0.94)
    border.color: AppTheme.borderSubtle
    radius: AppTheme.spacing8
    clip: true

    function measured(value, digits) {
        return value >= 0 ? Number(value).toFixed(digits) : "—"
    }

    function count(value) {
        return value === undefined || value === null ? "—" : String(value)
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: AppTheme.spacing12
        contentWidth: availableWidth
        ColumnLayout {
            id: content
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Stream Info")
                color: AppTheme.textPrimary
                font.bold: true
            }
            Label {
                visible: root.stats.length === 0
                text: qsTr("No video streams")
                color: AppTheme.textSecondary
            }
            Repeater {
                model: root.stats
                delegate: Label {
                    objectName: "streamInfoRow"
                    required property var modelData
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textMeta
                    text: {
                        const s = modelData
                        const heading = s.outgoing
                            ? (s.screen ? qsTr("%1 · outgoing share") : qsTr("%1 · outgoing camera"))
                            : (s.screen ? qsTr("%1 · received share") : qsTr("%1 · received camera"))
                        let lines = [heading.arg(s.participant),
                            qsTr("%1 · %2 × %3 · %4 FPS · %5 kbps")
                                .arg(s.codec).arg(s.width || "—").arg(s.height || "—")
                                .arg(root.measured(s.fps, 1)).arg(root.measured(s.kbps, 0))]
                        lines.push(s.encrypted ? qsTr("Encryption on") : qsTr("Encryption off"))
                        if (s.outgoing) {
                            lines.push(qsTr("Encoder: %1 · Scaling: %2 · Capture: %3")
                                .arg(s.encoder).arg(s.scaling).arg(s.source))
                            lines.push(qsTr("RTP packets sent: %1 · Lost on the way to the server: %2")
                                .arg(root.count(s.packetsSent)).arg(root.count(s.lost)))
                            lines.push(qsTr("Keyframes encoded: %1 · Keyframe requests: %2 · Resend requests: %3")
                                .arg(s.keyframes).arg(s.keyframeRequests).arg(root.count(s.nack)))
                        } else {
                            lines.push(qsTr("Decoder: %1 · RTP packets: %2 · Lost: %3 · Jitter: %4 ms")
                                .arg(s.encoder || "—").arg(root.count(s.packets))
                                .arg(root.count(s.lost)).arg(root.measured(s.jitterMs, 1)))
                            lines.push(qsTr("Keyframes: %1 · Keyframes asked for: %2 · Resend requests: %3")
                                .arg(s.keyframes).arg(s.keyframeRequests).arg(root.count(s.nack)))
                            lines.push(qsTr("Frames decrypted: %1 · Dropped: %2")
                                .arg(s.decrypted).arg(s.dropped))
                        }
                        return lines.join("\n")
                    }
                }
            }
        }
    }
}
