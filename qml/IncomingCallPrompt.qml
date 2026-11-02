import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    property string dismissedCallId: ""

    // Read at the press, so it names this card's call. The ringer stops here
    // too (live 2026-10-07: it rang on for 87.8 s with the card gone and no
    // control left to stop it); dismissRing refuses any call but the ringing
    // one.
    function dismiss() {
        var id = app.calls.activeCallId
        root.dismissedCallId = id
        if (root.callSoundsReachable)
            app.callSounds.dismissRing(id)
    }

    readonly property bool ringing:
        app.calls.state === CallController.Ringing
    readonly property bool rtcRing: app.calls.rtcRing
    readonly property string callRoomId: app.calls.activeRoomId
    readonly property bool inCall:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active

    readonly property bool barCovers:
        app.currentScreen === 1
        && app.calls.activeRoomId === app.currentRoomId

    readonly property bool shouldShow:
        (inCall && !barCovers)
        || (ringing && app.calls.activeCallId !== dismissedCallId
            && app.currentScreen === 1)

    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc
        && root.callRoomId.length > 0
    readonly property bool groupCallReachable:
        typeof app !== "undefined" && app && app.groupCall
    readonly property string joinBlockReason: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.joinBlockReason(root.callRoomId)
                                 : "unsupported";
    }
    readonly property bool alreadyInThisCall: {
        var _ = root.refreshTick;
        return root.groupCallReachable && app.groupCall.active
            && app.groupCall.roomId === root.callRoomId;
    }
    readonly property bool canJoinRtc:
        root.ringing && root.rtcRing && root.groupCallReachable
        && root.joinBlockReason.length === 0 && !root.alreadyInThisCall
    readonly property bool legacyAcceptOffered:
        root.ringing && !root.rtcRing && app.calls.mediaBackendAvailable

    readonly property bool callSoundsReachable:
        typeof app !== "undefined" && app && app.callSounds ? true : false
    readonly property bool silenceOffered:
        root.ringing && root.callSoundsReachable
        && app.callSounds.ringingCallId.length > 0
        && app.callSounds.ringingCallId === app.calls.activeCallId

    readonly property string joinBlockText: {
        switch (root.joinBlockReason) {
        case "":
            return "";
        case "unsupported":
            return qsTr("This build can't join Matrix calls.");
        case "undiscovered":
            return qsTr("Checking whether calling is available…");
        case "no_transport":
            return qsTr("No MatrixRTC service on this homeserver.");
        case "discovery_failed":
            return qsTr("Couldn't check whether calling is available.");
        case "session_closed":
            return qsTr("This call has ended.");
        case "no_media_transport":
            return qsTr("Joining calls isn't supported yet in this build.");
        case "no_permission":
            return qsTr("You can't join calls in this room. A room admin "
                        + "can raise your power level in it.");
        case "network_route":
            return qsTr("Calls aren't available while this account uses a proxy");
        case "media_encryption_unavailable":
            // Encrypted room without call media E2EE: joining would expose
            // media to the SFU.
            return qsTr("This room is encrypted, and encrypted calls aren't "
                        + "available in this build.");
        default:
            return qsTr("Joining isn't available.");
        }
    }

    property string answerRefusal: ""
    readonly property string answerRefusalText: {
        switch (root.answerRefusal) {
        case "":
            return "";
        case "no_media_backend":
            return qsTr("Answering needs a media engine this build doesn't "
                        + "have.");
        case "no_remote_offer":
            return qsTr("The caller's connection details didn't arrive.");
        case "not_ringing":
            return qsTr("That call is no longer ringing.");
        case "network_route":
            return qsTr("Calls aren't available while this account uses a proxy.");
        default:
            return qsTr("Couldn't answer this call.");
        }
    }
    onRingingChanged: {
        root.answerRefusal = "";
        if (root.ringing && root.rtcRing && root.rtcReachable)
            app.rtc.refresh(root.callRoomId);
    }

    Connections {
        enabled: root.rtcReachable
        target: (typeof app !== "undefined" && app) ? app.rtc : null
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.callRoomId)
                root.refresh();
        }
        function onAvailabilityChanged() {
            root.refresh();
        }
    }
    Connections {
        enabled: root.groupCallReachable
        target: (typeof app !== "undefined" && app) ? app.groupCall : null
        function onStateChanged() {
            root.refresh();
        }
    }

    objectName: "incomingCallPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: Math.max(316, actionRow.implicitWidth + AppTheme.spacing16 * 2)
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    border.color: AppTheme.bolt
    border.width: 1

    readonly property string titleText: {
        if (app.calls.state === CallController.Inviting)
            return qsTr("Calling…")
        if (app.calls.state === CallController.Connecting)
            return app.calls.videoCall ? qsTr("Video call — connecting…")
                                       : qsTr("Voice call — connecting…")
        if (app.calls.state === CallController.Active)
            return app.calls.videoCall ? qsTr("Video call") : qsTr("Voice call")
        return app.calls.videoCall ? qsTr("Incoming video call")
                                   : qsTr("Incoming voice call")
    }

    Accessible.role: Accessible.AlertMessage
    Accessible.name: titleText

    ColumnLayout {
        id: promptColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: AppTheme.spacing16
        anchors.rightMargin: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "call"
                size: 18
                color: AppTheme.bolt
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                Layout.fillWidth: true
                text: root.titleText
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                elide: Label.ElideRight
            }
            IconButton {
                objectName: "incomingCallPromptSilence"
                visible: root.silenceOffered
                storm: true
                size: "sm"
                iconName: "volume_off"
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: qsTr("Silence ringer")
                ToolTip.text: qsTr("Silence the ringer for this call")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: app.callSounds.silenceRing(app.calls.activeCallId)
            }
        }

        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: root.ringing
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: {
                var caller = app.calls.callerDisplayName || ""

                if (root.rtcRing) {
                    var opened = caller.length > 0
                        ? qsTr("%1 started a call.").arg(caller)
                        : qsTr("Someone started a call.")
                    if (root.alreadyInThisCall)
                        return qsTr("You're already in this call.")
                    if (root.joinBlockText.length > 0)
                        return opened + " " + root.joinBlockText
                    return opened
                }

                if (root.answerRefusalText.length > 0)
                    return root.answerRefusalText
                if (app.calls.videoCall && app.calls.mediaBackendAvailable)
                    return caller.length > 0
                        ? qsTr("%1 is calling with video.").arg(caller)
                        : qsTr("Incoming video call.")
                if (app.calls.mediaBackendAvailable)
                    return caller.length > 0
                        ? qsTr("%1 is calling.").arg(caller)
                        : qsTr("Incoming voice call.")
                return caller.length > 0
                    ? qsTr("%1 is calling. Answering on this device isn't "
                           + "supported yet — decline to stop the ring "
                           + "everywhere, or answer on another device.")
                          .arg(caller)
                    : qsTr("Answering on this device isn't supported yet.")
            }
        }

        RowLayout {
            id: actionRow
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppButton {
                objectName: "incomingCallPromptJoin"
                visible: root.canJoinRtc
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Join")
                onClicked: app.groupCall.join(root.callRoomId, false)
            }
            AppButton {
                objectName: "incomingCallPromptAccept"
                visible: root.legacyAcceptOffered
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Accept")
                onClicked: {
                    if (!app.calls.answer(true))
                        root.answerRefusal = app.calls.lastRefusal()
                    else
                        root.answerRefusal = ""
                }
            }
            AppButton {
                objectName: "incomingCallPromptAcceptAudio"
                visible: root.legacyAcceptOffered && app.calls.videoCall
                storm: true
                Layout.fillWidth: true
                text: qsTr("Audio only")
                onClicked: {
                    if (!app.calls.answer(false))
                        root.answerRefusal = app.calls.lastRefusal()
                    else
                        root.answerRefusal = ""
                }
            }
            AppButton {
                objectName: "incomingCallPromptDecline"
                visible: root.ringing
                storm: true
                kind: "danger"
                Layout.fillWidth: true
                text: qsTr("Decline")
                onClicked: app.calls.rejectIncoming()
            }
            AppButton {
                objectName: "incomingCallPromptHangup"
                visible: root.inCall
                storm: true
                kind: "danger"
                Layout.fillWidth: true
                text: qsTr("Hang up")
                onClicked: app.calls.hangup()
            }
            AppButton {
                objectName: "incomingCallPromptDismiss"
                visible: root.ringing
                storm: true
                Layout.fillWidth: true
                text: qsTr("Dismiss")
                onClicked: root.dismiss()
            }
        }
    }
}
