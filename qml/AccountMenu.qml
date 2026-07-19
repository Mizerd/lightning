import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 (design 1c): the account switcher popover, opened from the rail
// avatar. Shows the active account, every saved account (avatar, name,
// Matrix id in mono, checkmark on the active one — click to switch), an
// Add account row, then Settings / Security & Recovery / About and — at
// the bottom, danger-styled, behind a confirmation whose safe default is
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
        implicitWidth: 300

        // Identity header (active account).
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Avatar {
                size: 40
                circle: true
                name: root.accountLocalpart
                mxc: {
                    var record = app.accounts
                        ? app.accounts.account(root.accountUserId) : ({})
                    return record.avatarUrl || ""
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: {
                        var record = app.accounts
                            ? app.accounts.account(root.accountUserId) : ({})
                        return record.displayName || root.accountLocalpart
                    }
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontBody
                    font.weight: Font.DemiBold
                    elide: Label.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: root.accountUserId
                    color: AppTheme.textMuted
                    font.family: AppTheme.monoFont
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

        // ── Saved accounts (design 1c): click a row to switch. ───────────
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing8
            Layout.topMargin: AppTheme.spacing4
            text: qsTr("ACCOUNTS")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.fontCaption
            font.weight: Font.ExtraBold
            font.letterSpacing: 1.2
            visible: accountRepeater.count > 0
        }
        Repeater {
            id: accountRepeater
            model: app.accounts ? app.accounts.accounts : []

            ItemDelegate {
                id: accountRow
                required property var modelData
                Layout.fillWidth: true
                enabled: !app.accountSwitching
                Accessible.name: modelData.isActive
                                 ? qsTr("Active account %1").arg(modelData.userId)
                                 : qsTr("Switch to %1").arg(modelData.userId)

                readonly property string rowLocalpart: {
                    var uid = modelData.userId || ""
                    if (uid.startsWith("@")) uid = uid.slice(1)
                    var colon = uid.indexOf(":")
                    return colon > 0 ? uid.slice(0, colon) : uid
                }

                contentItem: RowLayout {
                    spacing: AppTheme.spacing8
                    Avatar {
                        size: 28
                        circle: true
                        name: accountRow.modelData.displayName
                              || accountRow.rowLocalpart
                        mxc: accountRow.modelData.avatarUrl || ""
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: accountRow.modelData.displayName
                                  || accountRow.rowLocalpart
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSecondary
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: accountRow.modelData.userId || ""
                            color: AppTheme.textMuted
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontCaption
                            elide: Label.ElideMiddle
                        }
                    }
                    Label {
                        visible: accountRow.modelData.isActive === true
                        text: "✓"
                        color: AppTheme.accent
                        font.pixelSize: AppTheme.fontBody
                        font.weight: Font.Bold
                    }
                    // Scoped removal of a non-active account (confirmed).
                    ToolButton {
                        id: removeAccountButton
                        visible: accountRow.modelData.isActive !== true
                                 && accountRow.hovered
                        implicitWidth: 24; implicitHeight: 24
                        Accessible.name: qsTr("Remove account %1")
                                         .arg(accountRow.modelData.userId)
                        ToolTip.text: qsTr("Remove from this device")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        contentItem: Label {
                            text: "×"
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.fontBody
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusSm
                            color: removeAccountButton.hovered
                                   ? AppTheme.hover : "transparent"
                        }
                        onClicked: {
                            removeConfirm.targetUserId = accountRow.modelData.userId
                            root.close()
                            removeConfirm.open()
                        }
                    }
                }
                onClicked: {
                    if (modelData.isActive === true)
                        return
                    root.close()
                    app.switchToAccount(modelData.userId)
                }
            }
        }

        ItemDelegate {
            Layout.fillWidth: true
            enabled: !app.accountSwitching
            Accessible.name: qsTr("Add account")
            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                Rectangle {
                    width: 28; height: 28
                    radius: AppTheme.radiusPill
                    color: "transparent"
                    border.color: AppTheme.borderStrong
                    Label {
                        anchors.centerIn: parent
                        text: "＋"
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontSecondary
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add account")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontSecondary
                }
            }
            onClicked: {
                root.close()
                app.showLogin()
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

    // Remove-account confirmation — names exactly which account is removed;
    // Cancel is focused and the default safe action. Only that account's
    // local session, store, and token are deleted.
    Dialog {
        id: removeConfirm
        property string targetUserId: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
        modal: true
        title: qsTr("Remove account?")
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
                text: qsTr("Remove %1 from this device? Its local Lightning "
                           + "data, encryption store, and sign-in are deleted "
                           + "from this computer only. Messages stay on the "
                           + "server, and other accounts are not affected.")
                      .arg(removeConfirm.targetUserId)
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: removeConfirm.close()
                }
                Button {
                    text: qsTr("Remove")
                    Accessible.name: qsTr("Confirm account removal")
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
                        var target = removeConfirm.targetUserId
                        removeConfirm.close()
                        app.removeAccount(target)
                    }
                }
            }
        }
    }

    // Confirmation — Cancel is focused and the default safe action.
    Dialog {
        id: signOutConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        // Bound to the overlay, not to content implicitWidth: the content
        // can wrap without feeding its preferred size back into the Dialog.
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
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
