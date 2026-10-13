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
                enabled: !!section.prefs
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
                           + "downloaded until you do. Private rooms are "
                           + "encrypted rooms and direct messages. The choice "
                           + "is saved in your account, so other apps that "
                           + "support it follow it.")
            }
            CheckBox {
                id: inviteCheck
                objectName: "inviteAvatarsCheck"
                palette.windowText: AppTheme.stormText
                enabled: !!section.prefs
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
