import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: card
    objectName: "invitePreviewCard"
    color: AppTheme.timelineBackground

    property string roomId: ""
    property string roomName: ""
    property string topic: ""
    property string inviter: ""
    property string inviterUserId: ""
    property string commonRoomsText: ""
    property string avatarUrl: ""
    // MSC4278 invite_avatars for this account (the host passes
    // app.accountPrefs.inviteAvatarsAllowed). Off: the letter tile stands in
    // and no request is made for the stranger-chosen picture.
    property bool showAvatar: false
    property string colorKey: ""
    property bool isDirect: false
    property bool isSpace: false
    property bool pending: false
    property string errorText: ""

    signal acceptRequested()
    signal declineRequested()
    signal inviterProfileRequested(Item anchor)

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
            font.underline: inviterMouse.containsMouse
            MouseArea {
                id: inviterMouse
                objectName: "invitePreviewInviterProfileArea"
                anchors.fill: parent
                enabled: card.inviterUserId !== ""
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("View %1's profile")
                    .arg(card.inviter || card.inviterUserId)
                onClicked: card.inviterProfileRequested(parent)
            }
        }
        Label {
            objectName: "invitePreviewCommonRooms"
            Layout.fillWidth: true
            visible: card.commonRoomsText.length > 0
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            text: card.commonRoomsText
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
