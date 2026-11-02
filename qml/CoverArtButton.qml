import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root
    objectName: "coverArtButton"

    property string label: qsTr("View cover art")
    signal activated()

    activeFocusOnTab: enabled
    property bool byPointer: false
    onActiveFocusChanged: if (!activeFocus) byPointer = false
    Accessible.role: Accessible.Button
    Accessible.name: root.label
    Accessible.onPressAction: root.activated()

    HoverHandler {
        id: hover
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        enabled: root.enabled
        gesturePolicy: TapHandler.WithinBounds
        onTapped: {
            root.byPointer = true
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.activated()
        }
    }
    Keys.onReturnPressed: (event) => { event.accepted = true; root.activated() }
    Keys.onEnterPressed: (event) => { event.accepted = true; root.activated() }
    Keys.onSpacePressed: (event) => { event.accepted = true; root.activated() }

    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: "transparent"
        visible: root.activeFocus && !root.byPointer
        border.width: 2
        border.color: AppTheme.focusRing
    }
    PlainToolTip {
        parent: root
        visible: hover.hovered && root.enabled
        delay: 600
        text: root.label
    }
}
