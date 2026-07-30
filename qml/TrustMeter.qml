import QtQuick
import QtQuick.Layouts
import MatrixClient

// Storm compact trust meter (SPEC-storm-language §3.4): mono TRUST label,
// 3px progress track, and a mono N/M counter — stormSuccess with a check at
// full, muted otherwise. Purely presentational; the caller supplies real
// completed/total from SDK-derived state and must never invent trust.
RowLayout {
    id: root

    property int completed: 0
    property int total: 3

    readonly property bool full: total > 0 && completed >= total

    spacing: AppTheme.spacing6

    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Trust %1 of %2").arg(completed).arg(total)

    Text {
        text: qsTr("TRUST")
        font.family: AppTheme.monoFont
        font.pixelSize: AppTheme.fontMicro
        font.weight: Font.DemiBold
        font.letterSpacing: 1.0
        color: AppTheme.stormTextFaint
    }

    Rectangle {
        id: track
        Layout.fillWidth: true
        Layout.minimumWidth: 40
        implicitHeight: 3
        radius: 1.5
        color: AppTheme.stormBorder

        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: root.total > 0
                   ? track.width * Math.max(0, Math.min(root.completed, root.total))
                     / root.total
                   : 0
            height: parent.height
            radius: parent.radius
            color: root.full ? AppTheme.bolt : AppTheme.stormTextMuted
        }
    }

    Text {
        text: root.full ? qsTr("%1/%2 ✓").arg(root.completed).arg(root.total)
                        : qsTr("%1/%2").arg(root.completed).arg(root.total)
        font.family: AppTheme.monoFont
        font.pixelSize: AppTheme.fontMicro
        font.weight: Font.DemiBold
        color: root.full ? AppTheme.stormSuccess : AppTheme.stormTextMuted
    }
}
