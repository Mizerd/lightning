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
    property string colorKey: ""
    property bool isDirect: false
    property bool isSpace: false
    // An accept or decline is in flight; both buttons wait for it.
    property bool pending: false
    property string errorText: ""

    signal acceptRequested()
    signal declineRequested()

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
            mxc: card.avatarUrl
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
                objectName: "invitePreviewAccept"
                kind: "primary"
                text: qsTr("Accept")
                enabled: !card.pending && card.roomId !== ""
                Accessible.name: qsTr("Accept the invitation")
                onClicked: card.acceptRequested()
            }
            AppButton {
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
