import QtQuick
import QtQuick.Controls
import MatrixClient

// How a call is placed, on the chevron beside the room header's call button:
// start one now, and which call type the button uses. "Automatic" is what the
// button has always done. A type this room cannot carry is disabled, not
// hidden, so the choice reads the same in every room.
AppMenu {
    id: root
    objectName: "callFormatMenu"

    property string roomId: ""
    menuWidth: 268

    // callFormats() is an invokable: read when the menu opens.
    property var formats: ({})
    onAboutToShow: root.formats = app.callFormats(root.roomId)

    function laneName(lane) {
        if (lane === "matrixrtc")
            return qsTr("MatrixRTC")
        if (lane === "legacy")
            return qsTr("Direct")
        return qsTr("not available here")
    }

    AppMenuItem {
        objectName: "callFormatMenuVoice"
        iconName: "call"
        text: qsTr("Voice call")
        onTriggered: app.startCall(root.roomId, false)
    }
    AppMenuItem {
        objectName: "callFormatMenuVideo"
        iconName: "videocam"
        text: qsTr("Video call")
        enabled: root.formats.video === true
        onTriggered: app.startCall(root.roomId, true)
    }

    AppMenuSeparator {}

    MenuSectionLabel {
        leftPadding: AppTheme.spacing12
        text: qsTr("Call type")
    }

    AppMenuItem {
        objectName: "callFormatMenuAuto"
        radio: true
        radioSelected: app.settings.callFormat === "auto"
        text: qsTr("Automatic (%1)").arg(root.laneName(root.formats.autoLane))
        onTriggered: app.settings.callFormat = "auto"
    }
    AppMenuItem {
        objectName: "callFormatMenuMatrixRtc"
        radio: true
        radioSelected: app.settings.callFormat === "matrixrtc"
        enabled: root.formats.matrixrtc === true
        text: qsTr("MatrixRTC (through a call server)")
        onTriggered: app.settings.callFormat = "matrixrtc"
    }
    AppMenuItem {
        objectName: "callFormatMenuLegacy"
        radio: true
        radioSelected: app.settings.callFormat === "legacy"
        enabled: root.formats.legacy === true
        text: qsTr("Direct (1:1 peer-to-peer)")
        onTriggered: app.settings.callFormat = "legacy"
    }
}
