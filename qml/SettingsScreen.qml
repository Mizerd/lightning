import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    Rectangle { anchors.fill: parent; color: AppTheme.background }

    Flickable {
        anchors.fill: parent
        anchors.margins: AppTheme.spacingXL
        contentHeight: content.implicitHeight
        clip: true

        ColumnLayout {
            id: content
            width: parent.width
            spacing: AppTheme.spacingL

            Label {
                text: qsTr("Settings")
                color: AppTheme.text
                font.pixelSize: 22
                font.weight: Font.DemiBold
            }

            Pane {
                Layout.fillWidth: true
                background: Rectangle {
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    radius: AppTheme.radius
                }
                ColumnLayout {
                    width: parent.width
                    spacing: AppTheme.spacingM

                    Label { text: qsTr("Homeserver"); font.weight: Font.DemiBold; color: AppTheme.text }
                    TextField {
                        Layout.fillWidth: true
                        text: app.settings.homeserverUrl
                        placeholderText: "https://matrix.org"
                        onEditingFinished: app.settings.homeserverUrl = text
                    }

                    Label { text: qsTr("Appearance"); font.weight: Font.DemiBold; color: AppTheme.text }
                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                        currentIndex: app.settings.theme
                        onActivated: app.settings.theme = currentIndex
                    }

                    Label { text: qsTr("Language"); font.weight: Font.DemiBold; color: AppTheme.text }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["en", "lt"]
                        currentIndex: Math.max(0, model.indexOf(app.settings.language))
                        onActivated: app.settings.language = model[currentIndex]
                    }
                    Label {
                        text: qsTr("Language switching requires an app restart in v0.1.")
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }

                    CheckBox {
                        text: qsTr("Start minimized")
                        checked: app.settings.startMinimized
                        onToggled: app.settings.startMinimized = checked
                    }
                    CheckBox {
                        text: qsTr("Desktop notifications")
                        checked: app.settings.notificationsEnabled
                        onToggled: app.settings.notificationsEnabled = checked
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                background: Rectangle {
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    radius: AppTheme.radius
                }
                ColumnLayout {
                    width: parent.width
                    spacing: AppTheme.spacingS

                    Label { text: qsTr("Security"); font.weight: Font.DemiBold; color: AppTheme.text }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: app.settings.secretsAreSecure
                        color: AppTheme.success
                        text: qsTr("Access tokens are stored via the system Secret Service. Logout clears them.")
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: !app.settings.secretsAreSecure
                        color: AppTheme.error
                        text: qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        text: qsTr("Crypto backend: %1").arg(app.crypto.backendDescription)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: app.crypto.supportsE2ee ? AppTheme.success : AppTheme.textMuted
                        text: qsTr("E2EE status: %1").arg(app.crypto.statusString)
                    }
                }
            }

            // v0.5.0-prep+10: Rust E2EE controls. Visible only on the
            // Rust backend; the section is a no-op on http/mock.
            Pane {
                Layout.fillWidth: true
                visible: app.backendName === "rust"
                background: Rectangle {
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    radius: AppTheme.radius
                }
                ColumnLayout {
                    width: parent.width
                    spacing: AppTheme.spacingS

                    Label {
                        text: qsTr("Encryption (Matrix Rust SDK)")
                        font.weight: Font.DemiBold
                        color: AppTheme.text
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        text: qsTr(
                            "Lightning session device: %1")
                            .arg(app.rustDeviceIdRedacted !== ""
                                 ? app.rustDeviceIdRedacted
                                 : qsTr("(not yet available)"))
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        text: qsTr(
                            "Encrypted send: initial verified · Encrypted receive: initial verified · " +
                            "Key backup restore: available below · " +
                            "Session (SAS emoji) verification UI: not implemented yet · " +
                            "Cross-signing UI: not implemented yet")
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        text: qsTr(
                            "Some old messages may show \"[unable to decrypt yet]\" until " +
                            "you restore your recovery key here, or until another " +
                            "verified device shares the room keys.")
                    }

                    // Recovery-key restore.
                    Label {
                        text: qsTr("Recovery key")
                        font.weight: Font.DemiBold
                        color: AppTheme.text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: recoveryField
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: qsTr("Paste the Element recovery key")
                            enabled: !recoveryPanel.running
                        }
                        Button {
                            text: recoveryPanel.running
                                ? qsTr("Restoring…")
                                : qsTr("Restore keys")
                            enabled: !recoveryPanel.running
                                && recoveryField.text.length > 0
                            onClicked: {
                                recoveryPanel.running = true
                                recoveryPanel.statusText = qsTr("Recovery started")
                                recoveryPanel.statusColor = AppTheme.textMuted
                                app.requestRecoverFromBackup(recoveryField.text)
                                // Wipe local copy immediately — recovery key
                                // never sits in a QML property beyond this
                                // call.
                                recoveryField.text = ""
                            }
                        }
                    }
                    Label {
                        id: recoveryStatusLabel
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: recoveryPanel.statusText !== ""
                        color: recoveryPanel.statusColor
                        text: recoveryPanel.statusText
                    }

                    QtObject {
                        id: recoveryPanel
                        property bool running: false
                        property string statusText: ""
                        property color statusColor: AppTheme.textMuted
                    }

                    Connections {
                        target: app
                        function onRecoveryStateChanged(state, message) {
                            if (state === "attempted") {
                                recoveryPanel.running = true
                                recoveryPanel.statusText = qsTr("Recovery started")
                                recoveryPanel.statusColor = AppTheme.textMuted
                            } else if (state === "ok") {
                                recoveryPanel.running = false
                                recoveryPanel.statusText = qsTr(
                                    "Recovery complete. New messages should " +
                                    "decrypt as keys arrive. Some old messages may " +
                                    "still require another verified device to share " +
                                    "keys.")
                                recoveryPanel.statusColor = AppTheme.success
                            } else if (state === "failed") {
                                recoveryPanel.running = false
                                recoveryPanel.statusText = qsTr(
                                    "Recovery failed: %1").arg(message)
                                recoveryPanel.statusColor = AppTheme.error
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        text: qsTr(
                            "You can also reset the local Lightning Rust SDK store " +
                            "for this account below, or from a terminal: " +
                            "matrix-client --reset-crypto-store. Reset deletes only " +
                            "Lightning's local store; it does not touch server messages " +
                            "or Element data.")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: qsTr("Refresh current room")
                            enabled: app.currentRoomId !== ""
                            onClicked: app.reloadCurrentRoomTimeline(50)
                        }
                        Item { Layout.fillWidth: true }
                        // v0.5.0-prep+12: destructive-styled reset button.
                        // Foreground white on danger red so it stands
                        // clearly apart from the refresh button.
                        Button {
                            id: resetDangerButton
                            text: qsTr("Reset local Lightning session")
                            onClicked: resetConfirmDialog.open()
                            contentItem: Label {
                                text: resetDangerButton.text
                                color: AppTheme.dangerText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.weight: Font.DemiBold
                                font.pixelSize: AppTheme.fontSizeS
                            }
                            background: Rectangle {
                                radius: AppTheme.radiusSm
                                color: resetDangerButton.pressed
                                        ? Qt.darker(AppTheme.danger, 1.2)
                                        : (resetDangerButton.hovered
                                            ? Qt.darker(AppTheme.danger, 1.1)
                                            : AppTheme.danger)
                                border.color: Qt.darker(AppTheme.danger, 1.3)
                                border.width: 1
                            }
                        }
                    }
                    Label {
                        id: resetStatusLabel
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: resetStatus.text !== ""
                        color: resetStatus.ok ? AppTheme.success : AppTheme.error
                        text: resetStatus.text
                    }
                    QtObject {
                        id: resetStatus
                        property bool ok: false
                        property string text: ""
                    }
                    Connections {
                        target: app
                        function onLocalRustStoreResetResult(ok, message) {
                            resetStatus.ok = ok
                            resetStatus.text = message
                        }
                    }
                    Dialog {
                        id: resetConfirmDialog
                        title: qsTr("Reset local Lightning session?")
                        standardButtons: Dialog.Ok | Dialog.Cancel
                        modal: true
                        Label {
                            width: 380
                            wrapMode: Text.WordWrap
                            text: qsTr(
                                "This deletes Lightning's local Matrix Rust SDK " +
                                "store and any saved smoke session for this " +
                                "account. Server messages, Element data, and " +
                                "other accounts are untouched. You will need to " +
                                "sign in again after this.")
                        }
                        onAccepted: app.resetLocalRustStore()
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                background: Rectangle {
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    radius: AppTheme.radius
                }
                ColumnLayout {
                    width: parent.width
                    spacing: AppTheme.spacingS
                    Label { text: qsTr("About"); font.weight: Font.DemiBold; color: AppTheme.text }
                    Label {
                        text: qsTr("matrix-client %1 — native Qt/QML Matrix desktop client.").arg(app.appVersion)
                        color: AppTheme.textMuted
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Back")
                    onClicked: app.loggedIn ? app.showMain() : app.showLogin()
                }
            }
        }
    }
}
