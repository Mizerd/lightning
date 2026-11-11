import QtQuick
import MatrixClient

// Child of an Avatar: opens its picture full size in the image viewer.
Item {
    id: root
    objectName: "avatarViewAction"

    readonly property string title: avatar ? avatar.name : ""
    readonly property Item avatar: parent
    readonly property string mxc: avatar ? avatar.mxc : ""
    // Initials (no picture, a withheld one, or a failed fetch) offer nothing.
    readonly property bool available: avatar !== null && avatar.hasImage
                                      && avatar.presentationState !== "failed"

    function activate() {
        if (!available)
            return
        viewerLoader.active = true
        viewerLoader.item.openMxc(mxc, title)
    }

    anchors.fill: parent
    visible: available
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("View picture")
    Accessible.onPressAction: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    Keys.onSpacePressed: activate()

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        gesturePolicy: TapHandler.WithinBounds
        onTapped: root.activate()
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -2
        radius: root.avatar ? root.avatar.radius + 2 : 0
        color: "transparent"
        border.color: AppTheme.focusRing
        border.width: 2
        visible: root.activeFocus
    }

    Loader {
        id: viewerLoader
        active: false
        sourceComponent: ImageViewerOverlay {}
    }
}
