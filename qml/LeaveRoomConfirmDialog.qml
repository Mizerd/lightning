import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient
import "LeaveAdminCheck.js" as AdminCheck

Dialog {
    id: root

    signal confirmed(string roomId)

    property string pendingRoomId: ""
    property string pendingRoomName: ""
    property int memberCount: -1
    property string joinRule: ""
    property int invitedCount: -1

    // The user holds the only admin power level in a room with others left in
    // it: nobody could then change the room's settings or its members. Only
    // ever true from a complete roster (see soleAdminOf).
    property bool lastAdmin: false
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

    readonly property int nameLimit: 80
    readonly property string shownRoomName: {
        var name = pendingRoomName
        if (name.length <= nameLimit)
            return name
        var cut = nameLimit
        var code = name.charCodeAt(cut - 1)
        if (code >= 0xD800 && code <= 0xDBFF)
            cut -= 1
        return name.slice(0, cut) + "\u2026"
    }
    readonly property bool openToAnyone: joinRule === "public"

    function soleAdminOf(info, roomId) {
        return AdminCheck.soleAdminOf(info, roomId)
    }

    function openFor(roomId, name, members, rule, invited, soleAdmin) {
        lastAdmin = soleAdmin === true
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
        var shown = shownRoomName
        if (unreachable)
            return named
                ? qsTr("You are the only member of \"%1\". If you leave, the "
                       + "room will be empty and cannot be rejoined, because "
                       + "nobody can invite you back.")
                      .arg(shown)
                : qsTr("You are the only member of this room. If you leave, "
                       + "the room will be empty and cannot be rejoined, "
                       + "because nobody can invite you back.")
        if (inviteOnly)
            return named
                ? qsTr("You will stop receiving messages from \"%1\". Server "
                       + "history is not deleted, but this room is "
                       + "invite-only: you will need a new invitation to come "
                       + "back.").arg(shown)
                : qsTr("You will stop receiving messages from this room. "
                       + "Server history is not deleted, but this room is "
                       + "invite-only: you will need a new invitation to come "
                       + "back.")
        if (openToAnyone)
            return named
                ? qsTr("You will stop receiving messages from \"%1\". Server "
                       + "history is not deleted, and this room is public: "
                       + "you can join it again at any time.").arg(shown)
                : qsTr("You will stop receiving messages from this room. "
                       + "Server history is not deleted, and this room is "
                       + "public: you can join it again at any time.")
        return named
            ? qsTr("You will stop receiving messages from \"%1\". Server "
                   + "history is not deleted, and you can be invited again "
                   + "later.").arg(shown)
            : qsTr("You will stop receiving messages from this room. Server "
                   + "history is not deleted, and you can be invited again "
                   + "later.")
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
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

    onOpened: {
        cancelButton.focus = false
        cancelButton.forceActiveFocus(Qt.TabFocusReason)
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12
        Label {
            objectName: "leaveRoomConfirmBody"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: root.bodyText
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            color: AppTheme.stormText
        }
        Label {
            objectName: "leaveRoomLastAdminNote"
            Layout.fillWidth: true
            visible: root.lastAdmin && !root.soleMember
            textFormat: Text.PlainText
            text: qsTr("You are the only administrator. If you leave, nobody "
                       + "will be able to change this room's settings or "
                       + "manage its members. Make someone else an "
                       + "administrator first.")
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            font.weight: AppTheme.weightStrong
            color: AppTheme.stormText
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                id: cancelButton
                objectName: "leaveRoomConfirmCancel"
                text: qsTr("Cancel")
                focus: true
                onClicked: root.close()
            }
            AppButton {
                objectName: "leaveRoomConfirmButton"
                kind: "danger"
                text: qsTr("Leave room")
                keysNeedVisualFocus: true
                Keys.onPressed: (event) => {
                    if (!visualFocus && event.key === Qt.Key_Space) {
                        event.accepted = true
                        cancelButton.forceActiveFocus(Qt.TabFocusReason)
                    }
                }
                onKeyRefusedWithoutRing: cancelButton.forceActiveFocus(Qt.TabFocusReason)
                onClicked: {
                    var id = root.pendingRoomId
                    root.close()
                    root.confirmed(id)
                }
            }
        }
    }
}
