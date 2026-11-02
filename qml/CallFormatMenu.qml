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
    // Why MatrixRTC is greyed (RtcController::joinBlockReason's closed set),
    // read with the formats: a disabled row alone said nothing.
    property string rtcBlock: ""
    onAboutToShow: {
        root.formats = app.callFormats(root.roomId)
        root.rtcBlock = (root.roomId.length > 0 && app.rtc)
                        ? app.rtc.joinBlockReason(root.roomId) : "unsupported"
    }
    readonly property string rtcReason: {
        if (root.formats.matrixrtc === true)
            return ""
        switch (root.rtcBlock) {
        case "no_transport":
            return qsTr("This homeserver has no MatrixRTC call service.")
        case "discovery_failed":
            return qsTr("The homeserver's call service did not answer.")
        case "undiscovered":
            return qsTr("Checking whether calling is available…")
        case "no_permission":
            return qsTr("You can't start calls in this room.")
        case "network_route":
            return qsTr("Calls aren't available while this account uses a proxy.")
        case "media_encryption_unavailable":
            return qsTr("Encrypted calls aren't available in this build.")
        case "":
            return ""
        default:
            return qsTr("MatrixRTC calls aren't available here.")
        }
    }

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
    // The reason a type is greyed, under the list, in the room's own words.
    MenuSectionLabel {
        objectName: "callFormatMenuReason"
        readonly property string note: {
            var parts = []
            if (root.rtcReason.length > 0)
                parts.push(qsTr("MatrixRTC: %1").arg(root.rtcReason))
            if (root.formats.legacy !== true)
                parts.push(qsTr("Direct: only in a chat with one other person."))
            return parts.join("\n")
        }
        visible: note.length > 0
        text: note
        width: root.availableWidth
        leftPadding: AppTheme.spacing12
        rightPadding: AppTheme.spacing12
        topPadding: AppTheme.spacing4
        bottomPadding: AppTheme.spacing6
        wrapMode: Text.WordWrap
        elide: Text.ElideNone
        font.weight: Font.Normal
    }
}
