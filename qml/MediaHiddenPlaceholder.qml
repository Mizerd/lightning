import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property bool hidden: false

    signal revealRequested()

    // `enabled` is what stops the tap handler; `visible` alone can leave it
    // hit-testable.
    visible: root.hidden
    enabled: root.hidden

    Loader {
        anchors.fill: parent
        active: root.hidden
        visible: active
        sourceComponent: Rectangle {
            objectName: "mediaHiddenPlaceholder"
            radius: AppTheme.radiusSm
            color: AppTheme.cardElevated
            border.width: 1
            border.color: AppTheme.border

            Row {
                anchors.centerIn: parent
                spacing: AppTheme.spacing6

                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "visibility"
                    size: 16
                    color: AppTheme.link
                }
                Label {
                    objectName: "mediaShowImageLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Show image")
                    font.pixelSize: AppTheme.textBody
                    color: AppTheme.link
                    font.underline: AppTheme.underlineLinks
                }
            }

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                objectName: "mediaShowImageTap"
                onTapped: root.revealRequested()
            }
        }
    }

    activeFocusOnTab: root.hidden
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Show image")
    Accessible.description: qsTr("This image is hidden on this device only")
    Accessible.onPressAction: root.revealRequested()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
            || event.key === Qt.Key_Enter) {
            root.revealRequested();
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
