import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// 2026-08-18 rounds 2-3: the voice-call corner card — Lightning's whole
// call surface. STATE, not policy: it shows whenever a call is live
// (ringing, dialing, connecting or active), independent of the ring-sound
// gates — a muted room silences the sound, it does not hide the fact of
// the call (matching Element). Dismissing hides a RINGING card only; the
// caller keeps ringing on their side and our other devices keep ringing
// too. Decline is the real action: it sends the wire event that stops the
// ring everywhere.
//
// TWO LANES RING HERE, AND THEY ARE ANSWERED BY DIFFERENT CODE.
//
// * The legacy 1:1 `m.call.*` lane is answered by `app.calls.answer()`,
//   which needs a registered media engine (app.calls.mediaBackendAvailable
//   — the GStreamer webrtcbin engine).
// * A MatrixRTC ring announces a SESSION. It is answered by JOINING that
//   session — `app.groupCall.join()`, gated on `app.rtc.joinBlockReason()`
//   — which is the SAME gate and the SAME action RoomCallBanner and the
//   timeline's call row already offer. Not a second join path.
//
// THE DEFECT THIS SHAPE FIXES. The card had ONE button, "Accept", gated on
// `app.calls.mediaBackendAvailable` — a property of the LEGACY engine that
// says nothing about which lane rang. So an Element call (which rings over
// MatrixRTC) showed an Accept that `CallController::answer()` refuses at its
// third guard, returning false into a call site that discarded it. Nothing
// happened, nothing was said, and the maintainer reported exactly that:
// "this accept does nothing".
//
// The lane now decides which affordance exists, and a legacy refusal is
// SHOWN rather than swallowed.
Rectangle {
    id: root

    // One-shot per call: dismissing hides THIS call's card; the next call
    // shows again.
    property string dismissedCallId: ""

    readonly property bool ringing:
        app.calls.state === CallController.Ringing
    /// Which lane rang. See the header comment.
    readonly property bool rtcRing: app.calls.rtcRing
    readonly property string callRoomId: app.calls.activeRoomId
    readonly property bool inCall:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active

    /// True when the top-of-conversation bar is on screen for this call.
    ///
    /// That bar lives in the room's own column, so it is only reachable
    /// while the user is looking at the call's room in the chat shell.
    readonly property bool barCovers:
        app.currentScreen === 1
        && app.calls.activeRoomId === app.currentRoomId

    readonly property bool shouldShow:
        // EXACTLY ONE in-call surface at a time. The card appears only where
        // CallHeaderBar cannot: another room, or another screen (Settings
        // mid-call). Gating on the call's STATE instead was wrong — during
        // Inviting/Connecting both were visible at once, which is what the
        // maintainer reported.
        //
        // The card still carries Hang Up, so leaving a call stays reachable
        // from anywhere; it simply stops duplicating the bar.
        (inCall && !barCovers)
        || (ringing && app.calls.activeCallId !== dismissedCallId
            && app.currentScreen === 1)

    // ── The MatrixRTC join gate ──────────────────────────────────────────
    //
    // ONE gate, one action, three surfaces (this card, RoomCallBanner, and
    // CallEventDelegate's call row). If this changes, those change with it —
    // a second opinion about whether a call is joinable is exactly the drift
    // those two already guard against.
    //
    // Every binding that calls into RtcController must read `refreshTick`:
    // Qt cannot track a C++ function call as a dependency, so a binding
    // without it evaluates once and never again.
    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    // Guarded the way CallEventDelegate guards: fixtures exist that supply no
    // call controllers, and "cannot ask" must degrade to "cannot join", never
    // to a reference error that takes the whole card down.
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
    /// This device is already in that call — the controls are up elsewhere
    /// and joining again is meaningless.
    readonly property bool alreadyInThisCall: {
        var _ = root.refreshTick;
        return root.groupCallReachable && app.groupCall.active
            && app.groupCall.roomId === root.callRoomId;
    }
    readonly property bool canJoinRtc:
        root.ringing && root.rtcRing && root.groupCallReachable
        && root.joinBlockReason.length === 0 && !root.alreadyInThisCall
    /// The LEGACY lane's Accept. Named rather than inlined into the button so
    /// a test can read the decision without depending on scene visibility —
    /// an item's `visible` is EFFECTIVE visibility, so a card that is merely
    /// off screen would make both lanes read the same and a regression test
    /// vacuous.
    readonly property bool legacyAcceptOffered:
        root.ringing && !root.rtcRing && app.calls.mediaBackendAvailable

    /// Human wording for `joinBlockReason`. Closed set from
    /// RtcController::joinBlockReason — a raw server string is never shown.
    /// Kept in step with RoomCallBanner.blockText, which maps the same set.
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
        default:
            return qsTr("Joining isn't available.");
        }
    }

    /// Why the legacy Accept refused, when it did. `answer()` returns a bool
    /// and this card USES it: swallowing that return is what made the button
    /// look dead. Cleared whenever the ring changes so one call's failure
    /// cannot describe the next.
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
        // An RTC ring can name a room the user is not looking at, so nothing
        // else has necessarily asked RtcController about it. RoomCallBanner
        // only refreshes the OPEN room.
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
        // Joining from this very card must make the button stand down.
        // `active` notifies through stateChanged — there is no
        // activeChanged, and a handler named for one would simply never run.
        function onStateChanged() {
            root.refresh();
        }
    }

    objectName: "incomingCallPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    // 316 is the card's SHAPE, not a promise that three buttons fit in it.
    //
    // Every button in the action row is `Layout.fillWidth`, so on a card too
    // narrow for them the RowLayout shrinks each one below its own label —
    // and an AppButton draws its label from the centre of whatever width it
    // is given, so three squeezed buttons write across each other. Ringing
    // shows three at once (Accept/Join, Decline, Dismiss), which is the
    // worst case and also the most urgent surface in the app to get wrong.
    //
    // Whether they fit is a question about TEXT: it depends on the label,
    // the font and the platform's own metrics for it, and it changed under
    // the maintainer without a line of QML changing. So the card asks the
    // row how much it needs and is never narrower than that. Reading
    // `actionRow.implicitWidth` is safe — a button's implicit width comes
    // from its label, never from the width the row hands back.
    width: Math.max(316, actionRow.implicitWidth + AppTheme.spacing16 * 2)
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    // Bolt-accent border: a call is an invitation, not a warning
    // (stormDanger is the verify prompt's tone; bolt is the brand accent).
    border.color: AppTheme.bolt
    border.width: 1

    // One title for the visible header AND the accessible name, so a
    // screen reader follows the state the way sighted users do.
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
        }

        Label {
            Layout.fillWidth: true
            visible: root.ringing
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            // Localpart only — the timeline's own no-bare-MXID restraint.
            text: {
                var caller = app.calls.callerUserId
                if (caller.length > 1 && caller.charAt(0) === "@")
                    caller = caller.substring(1).split(":")[0]

                // A MatrixRTC ring is an invitation to a SESSION, so the
                // wording is "started a call" and the obstacle, when there is
                // one, is the join gate — never the legacy engine, which has
                // nothing to do with this lane.
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
            // ── The MatrixRTC ring: JOIN ──
            //
            // Absent, not disabled, when the gate refuses — a disabled Qt
            // Quick control receives no hover and so cannot explain itself;
            // the reason goes in the card's body line instead. Same rule
            // RoomCallBanner follows.
            AppButton {
                objectName: "incomingCallPromptJoin"
                visible: root.canJoinRtc
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Join")
                // The SAME call RoomCallBanner's and the call row's Join
                // make. The ring clears itself once the join goes active
                // (AppController wires SfuCallController::stateChanged to
                // CallController::noteAnsweredByOtherLane), which puts
                // NOTHING on the wire — emphatically not a decline, which
                // would tell the caller "no" about a call just walked into.
                onClicked: app.groupCall.join(root.callRoomId, false)
            }
            // ── The legacy 1:1 ring: ANSWER ──
            AppButton {
                objectName: "incomingCallPromptAccept"
                // The engine gate applies to THIS lane only. Gating the RTC
                // ring on it was the defect: a legacy-engine property was
                // deciding whether to offer a button `answer()` refuses.
                visible: root.legacyAcceptOffered
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Accept")
                // The bool return is READ. Discarding it is what made a
                // refusal indistinguishable from a dead button.
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
