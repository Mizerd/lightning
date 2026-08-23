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
// Accept exists only when a media engine is registered
// (app.calls.mediaBackendAvailable — the GStreamer webrtcbin engine,
// round 3); without one the card honestly says answering is unsupported
// rather than offering a button that could not work.
Rectangle {
    id: root

    // One-shot per call: dismissing hides THIS call's card; the next call
    // shows again.
    property string dismissedCallId: ""

    readonly property bool ringing:
        app.calls.state === CallController.Ringing
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

    objectName: "incomingCallPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 316
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
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppButton {
                objectName: "incomingCallPromptAccept"
                // Only with a real media engine — the honest gate.
                visible: root.ringing && app.calls.mediaBackendAvailable
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Accept")
                onClicked: app.calls.answer()
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
