import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// 2026-08-18 round 2: the incoming voice-call corner card. STATE surface,
// not policy: it shows whenever a call is genuinely ringing
// (app.calls.ringing), independent of the ring-sound gates — a muted room
// silences the sound, it does not hide the fact of the call (matching
// Element). Dismissing hides the card only; the caller keeps ringing on
// their side and our other devices keep ringing too. Decline is the real
// action: it sends the wire event that stops the ring everywhere.
//
// Honesty: this device cannot ANSWER a call — there is no media engine in
// the tree (see docs/voice-calls.md) — and the card says so rather than
// offering a button that could not work. When a media backend lands,
// app.calls.mediaBackendAvailable flips and an Accept button can be added
// against the already-plumbed answer() path.
Rectangle {
    id: root

    // One-shot per call: dismissing hides THIS call's card; the next call
    // shows again.
    property string dismissedCallId: ""

    readonly property bool shouldShow:
        app.calls.ringing
        && app.calls.activeCallId !== dismissedCallId
        // The chat shell only — never over login/boot/Settings.
        && app.currentScreen === 1

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

    Accessible.role: Accessible.AlertMessage
    Accessible.name: qsTr("Incoming voice call")

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
                text: qsTr("Incoming voice call")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.fontSecondary
                font.weight: Font.Bold
                elide: Label.ElideRight
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.fontCaption
            // Localpart only — the timeline's own no-bare-MXID restraint.
            text: {
                var caller = app.calls.callerUserId
                if (caller.length > 1 && caller.charAt(0) === "@")
                    caller = caller.substring(1).split(":")[0]
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
                objectName: "incomingCallPromptDecline"
                storm: true
                kind: "danger"
                Layout.fillWidth: true
                text: qsTr("Decline")
                onClicked: app.calls.rejectIncoming()
            }
            AppButton {
                objectName: "incomingCallPromptDismiss"
                storm: true
                Layout.fillWidth: true
                text: qsTr("Dismiss")
                onClicked: root.dismissedCallId = app.calls.activeCallId
            }
        }
    }
}
