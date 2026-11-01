import QtQuick
import QtQuick.Controls
import MatrixClient

// The hit surface laid over an audio file's cover art: a click, or Enter /
// Space with focus, asks the host to show the cover enlarged
// (AudioPlayerCard.artworkViewRequested). It draws nothing but a focus ring.
// The tap takes the EXCLUSIVE grab (`WithinBounds`): on the default policy
// the row's own handlers beneath get the same press and select the message.
// Inert (and out of the tab order) when `enabled` is false, which is how the
// placeholder tile of a file with no cover stays a plain tile.
Item {
    id: root
    objectName: "coverArtButton"

    property string label: qsTr("View cover art")
    signal activated()

    activeFocusOnTab: enabled
    // Focus came from a click: no ring then, as for the play button.
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
    ToolTip {
        parent: root
        visible: hover.hovered && root.enabled
        delay: 600
        text: root.label
    }
}
