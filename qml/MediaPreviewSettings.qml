import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Privacy & security: when media and invite avatars load.
//
// Stored in the account (MSC4278 `m.media_preview_config`), so Element and any
// other client that honours it follow the same choice. A value this build does
// not know reads as Never. See AccountPrefsController.
ColumnLayout {
    id: section
    objectName: "mediaPreviewSettings"
    spacing: AppTheme.spacing8

    readonly property var prefs: typeof app !== "undefined" && app
                                 ? app.accountPrefs : null

    Label {
        text: qsTr("Media previews")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
    }
    Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Label {
                text: qsTr("Load media automatically")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
            }
            AppComboBox {
                id: modeCombo
                objectName: "mediaPreviewModeCombo"
                storm: true
                Layout.fillWidth: true
                enabled: !!section.prefs && section.prefs.mediaPreviewKnown
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("Always"),             value: "on" },
                    { label: qsTr("Private rooms only"), value: "private" },
                    { label: qsTr("Never"),              value: "off" }
                ]
                function syncFromPrefs() {
                    if (section.prefs)
                        syncToValue(section.prefs.mediaPreviews)
                }
                Component.onCompleted: syncFromPrefs()
                Connections {
                    target: section.prefs
                    function onChanged() { modeCombo.syncFromPrefs() }
                }
                onActivated: section.prefs.setMediaPreviews(currentValue)
                Accessible.name: qsTr("Load media automatically")
            }
            Label {
                objectName: "mediaPreviewHint"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing4
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Pictures, GIFs, stickers, video, audio and files "
                           + "start as one line you can open, and are not "
                           + "downloaded until you do. Private rooms are rooms "
                           + "that not everyone can join: invite-only, knock "
                           + "or restricted to a Space. The choice is saved in "
                           + "your account, so other apps that support it "
                           + "follow it.")
            }
            // An account that chose "Private rooms only" when it meant
            // "encrypted rooms and direct messages" keeps that narrower set
            // until the choice is made again (AccountPrefs::collapsesMedia).
            Label {
                objectName: "mediaPreviewPrivateMigration"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing4
                visible: !!section.prefs && section.prefs.mediaPreviewKnown
                         && section.prefs.mediaPreviews === "private"
                         && !section.prefs.privateMeansNonPublic
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Until you choose Private rooms only again, media "
                           + "also needs the room to be encrypted or a direct "
                           + "message, as this setting used to mean.")
            }
            SettingsCheckBox {
                id: inviteCheck
                objectName: "inviteAvatarsCheck"
                palette.windowText: AppTheme.stormText
                enabled: !!section.prefs && section.prefs.mediaPreviewKnown
                text: qsTr("Show avatars in room invitations")
                checked: section.prefs ? section.prefs.inviteAvatars : true
                onToggled: {
                    section.prefs.setInviteAvatars(checked)
                    // Keep following the account's value, not the click: a
                    // refused write must put the box back.
                    checked = Qt.binding(function () {
                        return section.prefs ? section.prefs.inviteAvatars : true
                    })
                }
                Accessible.description: qsTr(
                    "Load the picture a stranger set on a room that invites you")
            }
            // Server-side invitation blocking (spec v1.18
            // m.invite_permission_config, or MSC4380). Offered only when the
            // homeserver says it implements it: on any other server a switch
            // could only hide invitations, while people could still send them.
            // Muting invitation notifications is a separate push rule
            // (Notifications), and ignoring a person is per person.
            SettingsCheckBox {
                id: allowInvitesCheck
                objectName: "allowInvitesCheck"
                palette.windowText: AppTheme.stormText
                readonly property string policyState: section.prefs
                    ? section.prefs.invitePolicyState : "unknown"
                enabled: policyState === "ready" && !section.prefs.invitePolicyPending
                text: qsTr("Allow incoming room invitations")
                checked: section.prefs ? !section.prefs.invitesBlocked : true
                onToggled: {
                    section.prefs.setInvitesBlocked(!checked)
                    // The account's confirmed value, never the click: a refused
                    // write puts the box back.
                    checked = Qt.binding(function () {
                        return section.prefs ? !section.prefs.invitesBlocked : true
                    })
                }
                Accessible.description: allowInvitesHint.text
            }
            Label {
                id: allowInvitesHint
                objectName: "allowInvitesHint"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing4
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: !!section.prefs && section.prefs.invitePolicyWriteFailed
                       ? AppTheme.stormDanger : AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                text: {
                    var st = allowInvitesCheck.policyState
                    if (st === "unsupported")
                        return qsTr("Your homeserver does not support blocking "
                                    + "invitations, so this cannot be changed here. "
                                    + "You can still mute invitation notifications "
                                    + "or ignore a person.")
                    if (st === "offline")
                        return qsTr("Could not ask your homeserver whether it can "
                                    + "block invitations. Try again when you are "
                                    + "connected.")
                    if (st === "unknown")
                        return qsTr("Checking whether your homeserver can block "
                                    + "invitations…")
                    if (section.prefs && section.prefs.invitePolicyPending)
                        return qsTr("Saving…")
                    if (section.prefs && section.prefs.invitePolicyWriteFailed)
                        return qsTr("Could not save this to your account. It "
                                    + "still shows what your account holds.")
                    return section.prefs && section.prefs.invitesBlocked
                           ? qsTr("Your homeserver rejects new invitations to "
                                  + "this account.")
                           : qsTr("Turn off to have your homeserver reject new "
                                  + "invitations to this account. Saved in your "
                                  + "account, so other apps follow it.")
                }
            }
            Label {
                objectName: "mediaPreviewWriteFailed"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing4
                visible: !!section.prefs && section.prefs.writeFailed
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Could not save this to your account. It still "
                           + "shows what your account holds.")
            }
        }
    }
}
