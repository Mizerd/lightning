import QtQuick
import QtQuick.Controls
import MatrixClient

// Stand-in for a locally hidden image, Element-style. It fills the media box it
// replaces and has no implicit size, so the row keeps its exact geometry and
// the timeline does not move when an image is hidden. Behind a Loader in its
// host.
Item {
    id: root

    /// Whether the media is hidden right now.
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
            // A quiet surface, reading as "hidden", not "failed to load".
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
                    // Link ink: the one action here, and it behaves like a
                    // link.
                    color: AppTheme.link
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

    // Keyboard reachable: the placeholder is the only way back for a hidden
    // image.
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

    // The focus ring.
    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
