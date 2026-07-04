import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 1
            color: AppTheme.border
        }

        RoomListPane {
            SplitView.preferredWidth: 300
            SplitView.minimumWidth: 220
        }

        TimelinePane {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
        }
    }
}
