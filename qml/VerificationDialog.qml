import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Session verification as a focused centred modal, so the emojis to compare are
// always on screen. Storm centred-modal chrome (as MemberProfilePopover); the
// body is VerificationPanel, the one flow presentation. It follows
// AppController's verification state rather than being opened by hand: it opens
// whenever a flow exists (including incoming requests) and does not self-close
// on a terminal state, so the result can be read.
Popup {
    id: root
    modal: true
    width: Math.min(440, (parent ? parent.width : 440) - 2 * AppTheme.spacing16)
    padding: 0
    // Escape must not abandon a live flow: NoAutoClose while non-terminal, so
    // the only way out is Cancel (which tells the SDK); once ended,
    // Dismiss/Escape.
    closePolicy: root.flowIsLive ? Popup.NoAutoClose
                                 : (Popup.CloseOnEscape
                                    | Popup.CloseOnPressOutside)

    // A non-terminal flow. Not `app.verificationActive || …`: the flow id stays
    // set at done/cancelled/failed, which would keep this true forever. The
    // state string is the authority on liveness.
    readonly property bool flowIsLive:
        app.verificationState !== ""
        && app.verificationState !== "done"
        && app.verificationState !== "cancelled"
        && !app.verificationState.startsWith("failed")
    // Any flow state, live or terminal and unread. A failure raised before a
    // flow id exists (no cross-signing identity, send failed, signed out) sets
    // a state without verificationActive and must still be shown.
    readonly property bool flowPresent:
        app.verificationActive || app.verificationState !== ""

    // No "open me" entry point: callers start a flow and the state opens the
    // dialog. Mirrors flowPresent (not just its rising edge, which latched and
    // could never reopen); the else-branch makes Dismiss close it.
    function _syncOpenState() {
        if (flowPresent) {
            if (!opened)
                open()
        } else if (opened) {
            close()
        }
    }
    onFlowPresentChanged: _syncOpenState()
    Component.onCompleted: _syncOpenState()

    // Closing a finished flow means the same as Dismiss: cancelVerification(),
    // which is guarded so a late cancel cannot repaint a success.
    onClosed: {
        if (!flowIsLive && app.verificationState !== "")
            app.cancelVerification()
    }

    Overlay.modal: Rectangle { color: AppTheme.modalScrim }

    background: Item {
        Rectangle {
            id: dialogBackground
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.radiusLg
        }
        MultiEffect {
            source: dialogBackground
            anchors.fill: dialogBackground
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
    }

    contentItem: ColumnLayout {
        spacing: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Session verification")

        // Header: title and a close affordance only when closing is allowed.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing16
            Layout.bottomMargin: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                objectName: "verificationDialogTitle"
                text: qsTr("Verify this session")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
                elide: Label.ElideRight
            }
            IconButton {
                objectName: "verificationDialogClose"
                iconName: "close"
                storm: true
                visible: !root.flowIsLive
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.bottomMargin: AppTheme.spacing8
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
            // Says what verification is for.
            text: qsTr("Verifying proves this device is really yours, so "
                       + "your other sessions will share encryption keys "
                       + "with it and encrypted history becomes readable "
                       + "here.")
        }

        VerificationPanel {
            objectName: "verificationPanel"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.bottomMargin: AppTheme.spacing16
        }
    }
}
