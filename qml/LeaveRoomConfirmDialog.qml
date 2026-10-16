import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The one leave-room confirmation, shared by the room list menu and the room
// information panel. The wording is honest about what leaving costs: a room
// the user is the only member of ends up empty, and an invite-only room needs
// a new invitation to come back to. Callers pass what they know; -1 and ""
// mean "not known", which gets the neutral text and never a promise.
//
// Chrome follows the other confirmations (sign-out): panel surface, explicit
// scrim, padded title, Cancel as the default.
Dialog {
    id: root

    // Emitted with the room id passed to openFor(), after the dialog closed.
    signal confirmed(string roomId)

    property string pendingRoomId: ""
    // A remote string: shown PlainText only.
    property string pendingRoomName: ""
    // Joined members including the user, or -1 when the caller does not know.
    property int memberCount: -1
    // m.room.join_rules value ("public", "invite", ...), or "" when unknown.
    property string joinRule: ""
    // Pending invitations, or -1 when unknown. Invitees can still join, so
    // the room is not empty while any exist.
    property int invitedCount: -1

    readonly property bool soleMember: memberCount === 1
    readonly property bool inviteOnly: joinRule === "invite"
                                       || joinRule === "knock"
                                       || joinRule === "private"
    // Only a rule that is positively closed (invite / private), with nobody
    // invited, makes "cannot be rejoined" true. restricted, knock, public and
    // an unknown ("") rule all leave a way back, so they get the neutral text.
    readonly property bool unreachable: soleMember && invitedCount === 0
                                        && (joinRule === "invite"
                                            || joinRule === "private")

    function openFor(roomId, name, members, rule, invited) {
        pendingRoomId = roomId || ""
        pendingRoomName = name || ""
        memberCount = (members === undefined || members === null)
                      ? -1 : Number(members)
        joinRule = rule || ""
        invitedCount = (invited === undefined || invited === null)
                       ? -1 : Number(invited)
        open()
    }

    readonly property string bodyText: {
        var named = pendingRoomName.length > 0
        if (unreachable)
            return named
                ? qsTr("You are the only member of \"%1\". If you leave, the "
                       + "room will be empty and cannot be rejoined, because "
                       + "nobody can invite you back.")
                      .arg(pendingRoomName)
                : qsTr("You are the only member of this room. If you leave, "
                       + "the room will be empty and cannot be rejoined, "
                       + "because nobody can invite you back.")
        if (inviteOnly)
            return named
                ? qsTr("You will stop receiving messages from \"%1\". Server "
                       + "history is not deleted, but this room is "
                       + "invite-only: you will need a new invitation to come "
                       + "back.").arg(pendingRoomName)
                : qsTr("You will stop receiving messages from this room. "
                       + "Server history is not deleted, but this room is "
                       + "invite-only: you will need a new invitation to come "
                       + "back.")
        return named
            ? qsTr("You will stop receiving messages from \"%1\". Server "
                   + "history is not deleted, and you can be invited again "
                   + "later.").arg(pendingRoomName)
            : qsTr("You will stop receiving messages from this room. Server "
                   + "history is not deleted, and you can be invited again "
                   + "later.")
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    // Bound to the overlay, not to content, so wrapping text cannot feed back
    // into the Dialog width.
    width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    title: qsTr("Leave room?")
    standardButtons: Dialog.NoButton
    focus: true
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorderStrong
        radius: AppTheme.radiusLg
    }

    // Replaces Basic's stock title bar; `title` stays for accessibility.
    header: Label {
        text: root.title
        color: AppTheme.stormText
        font.family: AppTheme.menuFont
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightBold
        elide: Label.ElideRight
        leftPadding: AppTheme.spacing16
        rightPadding: AppTheme.spacing16
        topPadding: AppTheme.spacing16
        bottomPadding: AppTheme.spacing8
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12
        Label {
            objectName: "leaveRoomConfirmBody"
            Layout.fillWidth: true
            // Untrusted text (the room name): never markup.
            textFormat: Text.PlainText
            text: root.bodyText
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormText
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                focus: true
                onClicked: root.close()
            }
            AppButton {
                objectName: "leaveRoomConfirmButton"
                kind: "danger"
                text: qsTr("Leave room")
                onClicked: {
                    var id = root.pendingRoomId
                    root.close()
                    root.confirmed(id)
                }
            }
        }
    }
}
