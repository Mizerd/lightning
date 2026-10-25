import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A pending invite opened as the current room (from Activity or a
// notification): the room's identity, who invited you, and Accept / Decline,
// in place of a timeline and composer that cannot work before you join. The
// room list's invite row offers the same two actions; Element shows the same
// preview. TimelinePane decides when it is shown and wires the actions; this
// card only presents. Every remote string is plain text.
Rectangle {
    id: card
    objectName: "invitePreviewCard"
    color: AppTheme.timelineBackground

    property string roomId: ""
    property string roomName: ""
    property string topic: ""
    property string inviter: ""
    property string avatarUrl: ""
    // MSC4278 invite_avatars for this account (the host passes
    // app.accountPrefs.inviteAvatarsAllowed). Off: the letter tile stands in
    // and no request is made for the stranger-chosen picture.
    property bool showAvatar: false
    property string colorKey: ""
    property bool isDirect: false
    property bool isSpace: false
    // An accept or decline is in flight; both buttons wait for it.
    property bool pending: false
    property string errorText: ""

    signal acceptRequested()
    signal declineRequested()

    // Keyboard: the card is a Tab stop (with a visible ring, WCAG 2.4.7) and
    // Accept and Decline follow it in the ordinary chain, so Tab and
    // Shift+Tab can also LEAVE it (no trap, WCAG 2.1.2). Showing the card
    // never takes focus, so arrowing down the room list onto an invite row
    // keeps working. When the card's own buttons disable under the user (an
    // accept or decline in flight, or an error arriving), the disabled item
    // drops active focus; focus is pulled back to the card ONLY if it was
    // inside the card or already lost, never from wherever else the user has
    // moved (the list's own Accept/Reject also set `pending`; an error
    // arrives asynchronously). The covered timeline must also be out of the
    // Tab chain while the card is up: TimelinePane sets enabled on it
    // (C83-F01; done in TimelinePane).
    activeFocusOnTab: true
    function _focusInsideOrLost() {
        const w = card.Window.window
        if (!w)
            return false
        let f = w.activeFocusItem
        if (f === null || f === w.contentItem)
            return true
        for (; f; f = f.parent)
            if (f === card)
                return true
        return false
    }
    function focusCard() {
        if (visible && _focusInsideOrLost())
            forceActiveFocus(Qt.TabFocusReason)
    }
    onPendingChanged: if (pending) focusCard()
    onErrorTextChanged: if (errorText !== "") focusCard()

    // The card's own Tab stop shows a ring, inside its bounds.
    Rectangle {
        objectName: "invitePreviewFocusRing"
        anchors.fill: parent
        anchors.margins: 2
        radius: AppTheme.radiusLg
        color: "transparent"
        border.color: AppTheme.accent
        border.width: 2
        visible: card.activeFocus
        z: 1
    }

    // Swallow input so nothing reaches the timeline underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - AppTheme.spacing24 * 2, 420)
        spacing: AppTheme.spacing12

        Avatar {
            Layout.alignment: Qt.AlignHCenter
            size: 72
            squareRadius: AppTheme.radiusLg
            name: card.roomName || card.roomId
            objectName: "invitePreviewAvatar"
            mxc: card.showAvatar ? card.avatarUrl : ""
            colorKey: card.colorKey || card.roomId
            circle: card.isDirect
        }
        Label {
            objectName: "invitePreviewName"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: card.roomName || card.roomId
            textFormat: Text.PlainText
            color: AppTheme.text
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textDisplay)
            font.weight: AppTheme.weightDisplay
            elide: Label.ElideRight
        }
        Label {
            objectName: "invitePreviewInviter"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: card.inviter.length > 0
                  ? (card.isSpace
                     ? qsTr("%1 invited you to join this Space").arg(card.inviter)
                     : qsTr("%1 invited you to join").arg(card.inviter))
                  : (card.isSpace
                     ? qsTr("You have been invited to join this Space")
                     : qsTr("You have been invited to join"))
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
        }
        Label {
            Layout.fillWidth: true
            visible: card.topic.length > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: card.topic
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("You can read and send messages here after you accept.")
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: AppTheme.spacingS
            spacing: AppTheme.spacingS
            AppButton {
                id: acceptButton
                objectName: "invitePreviewAccept"
                kind: "primary"
                text: qsTr("Accept")
                enabled: !card.pending && card.roomId !== ""
                Accessible.name: qsTr("Accept the invitation")
                onClicked: card.acceptRequested()
            }
            AppButton {
                id: declineButton
                objectName: "invitePreviewDecline"
                kind: "danger"
                text: qsTr("Decline")
                enabled: !card.pending && card.roomId !== ""
                Accessible.name: qsTr("Decline the invitation")
                onClicked: card.declineRequested()
            }
        }
        Label {
            objectName: "invitePreviewError"
            Layout.fillWidth: true
            visible: card.errorText.length > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: card.errorText
            color: AppTheme.error
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        }
    }
}
