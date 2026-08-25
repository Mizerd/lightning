import QtQuick
import QtQuick.Controls
import MatrixClient

// The stand-in for a locally hidden image, Element-style.
//
// THE POINT OF IT IS THE GEOMETRY. It fills the media box it replaces and
// contributes no implicit size of its own, so the row keeps the exact
// rectangle the picture reserved: the same width, the same height, the same
// reply and thread positions, and the timeline does not move a pixel when the
// reader hides something. Replacing a 360×270 picture with a text row would
// jump every message above it, which for a hide-this-image control is a worse
// outcome than the picture.
//
// Behind a Loader in its host, so a row that is never hidden pays nothing for
// it.
Item {
    id: root

    /// Whether the media is hidden right now.
    property bool hidden: false

    signal revealRequested()

    // `visible` alone would leave the item hit-testable in some stacking
    // orders; `enabled` is what actually stops the tap handler.
    visible: root.hidden
    enabled: root.hidden

    Loader {
        anchors.fill: parent
        active: root.hidden
        visible: active
        sourceComponent: Rectangle {
            objectName: "mediaHiddenPlaceholder"
            radius: AppTheme.radiusSm
            // A quiet surface, not a hole: the reader should read "there is a
            // picture here that I hid", not "something failed to load".
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
                    // The link ink, because it is the one action on the
                    // surface and it behaves like a link: activating it
                    // restores the content.
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

    // Keyboard reach. The placeholder is the ONLY way back for a hidden
    // image, so it has to be operable without a pointer.
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

    // The focus ring, so tabbing to it is visible.
    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
