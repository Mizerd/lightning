import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The voice-call corner card. It reflects call state, not ring policy: it
// shows whenever a call is live (ringing, dialing, connecting or active) even
// in a muted room. Dismiss hides a ringing card only (the caller and our other
// devices keep ringing); Decline sends the event that stops the ring
// everywhere.
//
// Two lanes ring here and are answered differently:
// * the legacy 1:1 `m.call.*` lane by `app.calls.answer()`, which needs the
//   GStreamer engine (app.calls.mediaBackendAvailable);
// * a MatrixRTC ring by joining the session: `app.groupCall.join()` gated on
//   `app.rtc.joinBlockReason()`, the same gate and action as RoomCallBanner
//   and the timeline's call row.
// The lane decides which button exists, and a legacy refusal is shown rather
// than swallowed.
Rectangle {
    id: root

    // Dismissal is per call; the next call shows again.
    property string dismissedCallId: ""

    readonly property bool ringing:
        app.calls.state === CallController.Ringing
    /// Which lane rang (see the header).
    readonly property bool rtcRing: app.calls.rtcRing
    readonly property string callRoomId: app.calls.activeRoomId
    readonly property bool inCall:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active

    /// True when the top-of-conversation call bar is on screen for this call,
    /// i.e. the user is viewing the call's room in the chat shell.
    readonly property bool barCovers:
        app.currentScreen === 1
        && app.calls.activeRoomId === app.currentRoomId

    readonly property bool shouldShow:
        // One in-call surface at a time: the card appears only where
        // CallHeaderBar can't (another room or screen). It keeps Hang Up, so
        // leaving is reachable from anywhere.
        (inCall && !barCovers)
        || (ringing && app.calls.activeCallId !== dismissedCallId
            && app.currentScreen === 1)

    // ── The MatrixRTC join gate ──
    // One gate and action shared with RoomCallBanner and CallEventDelegate;
    // change them together. Bindings calling into RtcController read
    // `refreshTick`, since Qt can't track a C++ call as a dependency.
    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    // Guarded like CallEventDelegate: without controllers (fixtures) "can't
    // ask" degrades to "can't join", never a reference error.
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
    /// This device is already in that call.
    readonly property bool alreadyInThisCall: {
        var _ = root.refreshTick;
        return root.groupCallReachable && app.groupCall.active
            && app.groupCall.roomId === root.callRoomId;
    }
    readonly property bool canJoinRtc:
        root.ringing && root.rtcRing && root.groupCallReachable
        && root.joinBlockReason.length === 0 && !root.alreadyInThisCall
    /// The legacy lane's Accept, named so a test can read the decision without
    /// depending on effective visibility (an off-screen card would make both
    /// lanes read the same).
    readonly property bool legacyAcceptOffered:
        root.ringing && !root.rtcRing && app.calls.mediaBackendAvailable

    // ── Silence ──
    // Stops Lightning's ringer for this call only; the card stays and the next
    // call rings normally. Offered only while our ringer sounds for the ringing
    // call: `ringingCallId` is empty once silenced, or when the desktop's sound
    // is the ringer (the notification then offers Silence).
    readonly property bool callSoundsReachable:
        typeof app !== "undefined" && app && app.callSounds ? true : false
    readonly property bool silenceOffered:
        root.ringing && root.callSoundsReachable
        && app.callSounds.ringingCallId.length > 0
        && app.callSounds.ringingCallId === app.calls.activeCallId

    /// Human wording for `joinBlockReason`: the closed set from
    /// RtcController::joinBlockReason, never a raw server string. Must match
    /// RoomCallBanner.blockText and CallEventDelegate.blockText;
    /// `everyJoinBlockTokenHasWordingOnEverySurface` enforces it.
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
        case "media_encryption_unavailable":
            // Encrypted room without call media E2EE: joining would expose
            // media to the SFU.
            return qsTr("This room is encrypted, and encrypted calls aren't "
                        + "available in this build.");
        default:
            return qsTr("Joining isn't available.");
        }
    }

    /// Why the legacy Accept refused, from answer()'s return value. Cleared
    /// when the ring changes so one call's failure can't describe the next.
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
        default:
            return qsTr("Couldn't answer this call.");
        }
    }
    onRingingChanged: {
        root.answerRefusal = "";
        // An RTC ring may name a room nobody has asked RtcController about
        // (RoomCallBanner only refreshes the open room).
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
        // Joining from this card must stand the button down. `active`
        // notifies through stateChanged; there is no activeChanged.
        function onStateChanged() {
            root.refresh();
        }
    }

    objectName: "incomingCallPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    // Never narrower than the action row needs: the buttons fill width, and a
    // squeezed AppButton draws its label over its neighbours. Ringing shows
    // three at once. Reading the row's implicit width is safe; it comes from
    // the labels, not the width handed back.
    width: Math.max(316, actionRow.implicitWidth + AppTheme.spacing16 * 2)
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    // Bolt border: a call is an invitation, not a warning.
    border.color: AppTheme.bolt
    border.width: 1

    // One title for the header and the accessible name.
    readonly property string titleText: {
        if (app.calls.state === CallController.Inviting)
            return qsTr("Calling…")
        if (app.calls.state === CallController.Connecting)
            return qsTr("Voice call — connecting…")
        if (app.calls.state === CallController.Active)
            return qsTr("Voice call")
        return qsTr("Incoming voice call")
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
            // In the title row: it's not a decision about the call, and the
            // action row is already full while ringing.
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
                // Read at the press, so it names this card's call; silenceRing
                // refuses any other.
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
            // Localpart only, as in the timeline.
            text: {
                var caller = app.calls.callerUserId
                if (caller.length > 1 && caller.charAt(0) === "@")
                    caller = caller.substring(1).split(":")[0]

                // A MatrixRTC ring invites to a session: "started a call", and
                // any obstacle is the join gate, never the legacy engine.
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
            // ── The MatrixRTC ring: Join ── Absent rather than disabled when
            // the gate refuses (a disabled control gets no hover, so can't
            // explain itself); the reason goes in the body line.
            AppButton {
                objectName: "incomingCallPromptJoin"
                visible: root.canJoinRtc
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Join")
                // Same action as the other Join buttons. The ring clears once
                // the join goes active
                // (CallController::noteAnsweredByOtherLane), with nothing sent
                // on the wire: a decline would tell the caller "no".
                onClicked: app.groupCall.join(root.callRoomId, false)
            }
            // ── The legacy 1:1 ring: Answer ──
            AppButton {
                objectName: "incomingCallPromptAccept"
                // The engine gate applies to this lane only.
                visible: root.legacyAcceptOffered
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Accept")
                // Read the bool return so a refusal is shown.
                onClicked: {
                    if (!app.calls.answer())
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
                onClicked: root.dismissedCallId = app.calls.activeCallId
            }
        }
    }
}
