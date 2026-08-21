import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.7.x: session verification as a FOCUSED centred modal, the way Element
// presents it — rather than a card at the bottom of a scrolled settings
// page, where the emojis the user is supposed to compare against another
// device could sit off-screen at the exact moment they matter.
//
// Chrome follows the Storm centred-modal idiom already used by
// MemberProfilePopover (SPEC-storm-language §3.1): storm panel, storm
// border, themed scrim, sanctioned shadow. The body is VerificationPanel —
// ONE flow presentation, so Settings and the first-login prompt cannot
// drift apart.
//
// Lifecycle: the dialog follows AppController's verification state rather
// than being opened and closed by hand. It opens whenever a flow exists
// (including an INCOMING request, so an Element-initiated verification
// surfaces here too) and it does not self-close on a terminal state — the
// user reads "Verification complete" or the failure reason and dismisses
// it, exactly as the inline card behaved.
Popup {
    id: root
    modal: true
    width: Math.min(440, (parent ? parent.width : 440) - 2 * AppTheme.spacing16)
    padding: 0
    // Escape must not silently abandon a live flow: closePolicy is
    // deliberately NoAutoClose while a non-terminal flow is running, so the
    // only ways out are Cancel (which tells the SDK) and, once the flow has
    // ended, Dismiss/Escape. Clicking the scrim never abandons it either.
    closePolicy: root.flowIsLive ? Popup.NoAutoClose
                                 : (Popup.CloseOnEscape
                                    | Popup.CloseOnPressOutside)

    // A flow that has not reached a terminal state.
    //
    // Deliberately NOT `app.verificationActive || …`: AppController keeps
    // `m_verificationFlowId` set at done/cancelled/failed (only
    // cancelVerification() clears it), so ORing verificationActive in left
    // this true at every terminal state — the close affordance never
    // appeared and closePolicy stayed NoAutoClose on a finished flow. The
    // state STRING is the authority on liveness; a flow always has one (a
    // request sets "requested", a start sets "starting"), so nothing is
    // missed by not consulting the id.
    readonly property bool flowIsLive:
        app.verificationState !== ""
        && app.verificationState !== "done"
        && app.verificationState !== "cancelled"
        && !app.verificationState.startsWith("failed")
    // Any flow state at all — live, or terminal and not yet read. This is
    // the c259b60 invariant, moved verbatim from the inline card: a failure
    // raised BEFORE a flow id exists (no cross-signing identity, request
    // send failed, not signed in) sets a state without setting
    // verificationActive, and must still be shown rather than vanishing.
    readonly property bool flowPresent:
        app.verificationActive || app.verificationState !== ""

    // NOTE there is deliberately no "open me" entry point. Callers start a
    // flow (app.startOwnVerification()) and the state below brings the
    // dialog up; a second path for "the dialog is showing" could disagree
    // with the state, which is exactly the class of bug the c259b60 fix was
    // about.
    //
    // This MIRRORS flowPresent rather than reacting to its rising edge only.
    // The edge-only form latched: terminal states leave the state string set,
    // so flowPresent stayed true, the handler never fired again, and a
    // dialog the user had closed could never reopen — for the rest of the
    // session, including for an INCOMING request from another client. The
    // else-branch also makes Dismiss actually dismiss: clearing the state
    // now closes the dialog instead of leaving it open on the panel's
    // "Waiting…" fallback.
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

    // Closing a dialog that is showing a FINISHED flow means exactly what the
    // panel's Dismiss button means, so it does the same thing — otherwise the
    // terminal state would linger and latch the dialog shut (above).
    // cancelVerification() is the established dismissal for these states: it
    // is what Dismiss already calls, and it is guarded so a late cancel can
    // never repaint a successful verification.
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

        // Header: title + a close affordance that is only offered when
        // closing is actually allowed (see closePolicy).
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
            // Says what verification is FOR. "Verify this session" alone
            // does not tell anyone why they should bother, and the honest
            // answer is short.
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
