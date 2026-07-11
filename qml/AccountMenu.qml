import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 3): the account popover. Opened from the sidebar account
// button; contains Settings, Security & Recovery, About and — at the
// bottom, danger-styled, behind a confirmation whose safe default is
// Cancel — the only Sign out in the application. No access token, device
// secret, or local path is ever displayed.
Popup {
    id: root
    modal: true
    padding: AppTheme.spacing8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property string accountUserId:
        app.accounts ? (app.accounts.activeUserId || "") : ""
    readonly property string accountLocalpart: {
        var uid = accountUserId
        if (uid.startsWith("@")) uid = uid.slice(1)
        var colon = uid.indexOf(":")
        return colon > 0 ? uid.slice(0, colon) : uid
    }
    readonly property string accountServer: {
        var colon = accountUserId.indexOf(":")
        return colon > 0 ? accountUserId.slice(colon + 1) : ""
    }
    readonly property bool connected:
        app.connectionStatus === qsTr("Connected")

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        border.width: 1
        radius: AppTheme.radiusMd
    }

    contentItem: ColumnLayout {
        spacing: 2
        implicitWidth: 260

        // Identity header.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Rectangle {
                width: 40; height: 40
                radius: AppTheme.radiusPill
                color: AppTheme.accent
                Label {
                    anchors.centerIn: parent
                    text: root.accountLocalpart.length > 0
                          ? root.accountLocalpart[0].toUpperCase() : "?"
                    color: AppTheme.accentText
                    font.pixelSize: AppTheme.fontRoomTitle
                    font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: root.accountLocalpart
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontBody
                    font.weight: Font.DemiBold
                    elide: Label.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: root.accountUserId
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontCaption
                    elide: Label.ElideMiddle
                }
                RowLayout {
                    spacing: AppTheme.spacing4
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: root.connected ? AppTheme.success : AppTheme.warning
                    }
                    Label {
                        text: app.connectionStatus
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontCaption
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.separator }

        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("Settings")
            Accessible.name: text
            onClicked: { root.close(); app.showSettingsSection("general") }
        }
        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("Security & Recovery")
            Accessible.name: text
            onClicked: { root.close(); app.showSettingsSection("security") }
        }
        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("About Lightning")
            Accessible.name: text
            onClicked: { root.close(); app.showSettingsSection("about") }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.separator }

        ItemDelegate {
            id: signOutItem
            Layout.fillWidth: true
            Accessible.name: qsTr("Sign out")
            contentItem: Label {
                text: qsTr("Sign out")
                color: AppTheme.danger
                font.pixelSize: AppTheme.fontBody
            }
            onClicked: {
                root.close()
                signOutConfirm.open()
            }
        }
    }

    // Confirmation — Cancel is focused and the default safe action.
    Dialog {
        id: signOutConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Sign out?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 380
                text: qsTr("You will be signed out of this session. "
                           + "Lightning's local data for this account is "
                           + "removed from this computer; your messages stay "
                           + "on the server, and encrypted history may need "
                           + "your recovery key after the next sign-in.")
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: signOutConfirm.close()
                }
                Button {
                    text: qsTr("Sign out")
                    Accessible.name: qsTr("Confirm sign out")
                    contentItem: Label {
                        text: parent.text
                        color: AppTheme.dangerText
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.down ? Qt.darker(AppTheme.danger, 1.2)
                                           : AppTheme.danger
                        radius: AppTheme.radiusSm
                    }
                    onClicked: {
                        signOutConfirm.close()
                        app.auth.logout()
                    }
                }
            }
        }
    }
}
