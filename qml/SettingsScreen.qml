import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
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

            // ── Connection ────────────────────────────────────────────────
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

                    Label { text: qsTr("Connection"); font.weight: Font.DemiBold; color: AppTheme.text }
                    Label { text: qsTr("Homeserver URL"); color: AppTheme.textSecondary; font.pixelSize: AppTheme.fontSizeS }
                    TextField {
                        Layout.fillWidth: true
                        text: app.settings.homeserverUrl
                        placeholderText: "https://matrix.org"
                        onEditingFinished: app.settings.homeserverUrl = text
                    }
                }
            }

            // ── Appearance ────────────────────────────────────────────────
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

                    Label { text: qsTr("Appearance"); font.weight: Font.DemiBold; color: AppTheme.text }

                    Label { text: qsTr("Theme"); color: AppTheme.textSecondary; font.pixelSize: AppTheme.fontSizeS }
                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                        currentIndex: app.settings.theme
                        onActivated: app.settings.theme = currentIndex
                    }

                    Label { text: qsTr("Language"); color: AppTheme.textSecondary; font.pixelSize: AppTheme.fontSizeS }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["en", "lt"]
                        currentIndex: Math.max(0, model.indexOf(app.settings.language))
                        onActivated: app.settings.language = model[currentIndex]
                    }
                    Label {
                        text: qsTr("Language switching requires an app restart.")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeXS
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
                        text: qsTr("Security & Recovery")
                        font.weight: Font.DemiBold
                        color: AppTheme.text
                    }

                    // ─── Current session ──────────────────────────────
                    Label {
                        text: qsTr("Current session")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontSizeS
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        text: qsTr("Device ID: %1").arg(
                            app.sessionDeviceId !== ""
                                ? app.sessionDeviceId
                                : (app.rustDeviceIdRedacted !== ""
                                    ? app.rustDeviceIdRedacted
                                    : qsTr("(not yet available)")))
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: app.sessionTrustState === "Verified"
                            ? AppTheme.success
                            : (app.sessionTrustState === "Not verified"
                                ? AppTheme.warning
                                : AppTheme.textMuted)
                        text: qsTr("Status: %1").arg(app.sessionTrustState)
                    }

                    // ─── Verify this session ──────────────────────────
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        visible: app.sessionTrustState !== "Verified"
                        text: qsTr(
                            "Verify this session using another session already " +
                            "signed in to this Matrix account. This does not import " +
                            "room keys — key import is a separate action below.")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingS
                        visible: !app.verificationActive
                        Button {
                            text: app.sessionTrustState === "Verified"
                                ? qsTr("Verify again")
                                : qsTr("Verify this session")
                            enabled: app.loggedIn
                            onClicked: app.startOwnVerification()
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            visible: app.sessionTrustState === "Verified"
                            color: AppTheme.success
                            text: qsTr("This Lightning session is verified through Matrix cross-signing.")
                            wrapMode: Text.WordWrap
                        }
                    }

                    // v0.5.0 SAS emoji verification UI. Receive-first —
                    // the Rust bridge listens for verification requests
                    // and surfaces them here. Visible only while a flow
                    // is active.
                    Pane {
                        Layout.fillWidth: true
                        visible: app.verificationActive
                        background: Rectangle {
                            color: AppTheme.surfaceAlt
                            border.color: AppTheme.accent
                            radius: AppTheme.radiusSm
                        }
                        ColumnLayout {
                            width: parent.width
                            spacing: AppTheme.spacingS

                            Label {
                                text: qsTr("Session verification")
                                color: AppTheme.text
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: AppTheme.textMuted
                                text: {
                                    if (app.verificationState === "starting")
                                        return qsTr("Sending verification request…")
                                    if (app.verificationState === "waiting_for_other_session")
                                        return qsTr(
                                            "Verification request sent. Accept it in " +
                                            "another session, such as Element.")
                                    if (app.verificationState === "requested")
                                        return qsTr("Incoming verification request from %1")
                                            .arg(app.verificationOtherUser)
                                    if (app.verificationState === "sas_ready")
                                        return qsTr(
                                            "Compare all seven emojis with the other " +
                                            "session. Confirm only if every emoji matches " +
                                            "in the same order.")
                                    if (app.verificationState === "done")
                                        return qsTr(
                                            "Verification flow complete. Lightning is " +
                                            "querying the SDK for updated trust state.")
                                    if (app.verificationState === "cancelled")
                                        return qsTr("Verification cancelled.")
                                    if (app.verificationState.indexOf("failed") === 0)
                                        return qsTr("Verification failed.")
                                    return qsTr("Waiting…")
                                }
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                visible: app.verificationState === "sas_ready"
                                Repeater {
                                    model: app.verificationEmojis
                                    delegate: Rectangle {
                                        color: AppTheme.surface
                                        border.color: AppTheme.border
                                        radius: AppTheme.radiusSm
                                        implicitWidth: 84
                                        implicitHeight: 78
                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            spacing: 2
                                            Label {
                                                text: modelData.symbol || ""
                                                font.pixelSize: 28
                                                horizontalAlignment: Text.AlignHCenter
                                                Layout.alignment: Qt.AlignHCenter
                                            }
                                            Label {
                                                text: modelData.description || ""
                                                font.pixelSize: AppTheme.fontSizeXS
                                                color: AppTheme.textMuted
                                                horizontalAlignment: Text.AlignHCenter
                                                Layout.alignment: Qt.AlignHCenter
                                            }
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                Button {
                                    text: qsTr("Accept")
                                    visible: app.verificationState === "requested"
                                    highlighted: true
                                    onClicked: app.acceptVerification()
                                }
                                Button {
                                    text: qsTr("They match")
                                    visible: app.verificationState === "sas_ready"
                                    highlighted: true
                                    onClicked: app.confirmVerification()
                                }
                                Button {
                                    text: qsTr("They do not match")
                                    visible: app.verificationState === "sas_ready"
                                    onClicked: app.mismatchVerification()
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    text: qsTr("Cancel verification")
                                    visible: app.verificationState === "requested"
                                            || app.verificationState === "sas_ready"
                                            || app.verificationState === "waiting_for_other_session"
                                            || app.verificationState === "starting"
                                    onClicked: app.cancelVerification()
                                }
                                Button {
                                    text: qsTr("Dismiss")
                                    visible: app.verificationState === "done"
                                            || app.verificationState === "cancelled"
                                            || app.verificationState.indexOf("failed") === 0
                                    onClicked: app.cancelVerification()
                                }
                            }
                        }
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

                    // ─── Encrypted room-key import ────────────────────
                    Label {
                        text: qsTr("Import room keys")
                        font.weight: Font.DemiBold
                        color: AppTheme.text
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        text: qsTr(
                            "Import an encrypted Matrix room-key export from another " +
                            "session. Imported keys may unlock older encrypted messages, " +
                            "but they do not verify this session.")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingS
                        Button {
                            id: importChooseButton
                            text: qsTr("Choose key export")
                            enabled: app.loggedIn && !importPanel.running
                            onClicked: importFileDialog.open()
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            elide: Text.ElideMiddle
                            color: AppTheme.textMuted
                            text: importPanel.selectedFileName === ""
                                ? qsTr("(no file selected)")
                                : importPanel.selectedFileName
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingS
                        visible: importPanel.selectedFileUrl.toString() !== ""
                        TextField {
                            id: importPassphraseField
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: qsTr("Export passphrase")
                            enabled: !importPanel.running
                            onAccepted: {
                                if (text.length > 0)
                                    importStartButton.clicked()
                            }
                        }
                        Button {
                            id: importStartButton
                            text: importPanel.running
                                ? qsTr("Importing…")
                                : qsTr("Import")
                            enabled: !importPanel.running
                                && importPassphraseField.text.length > 0
                            onClicked: {
                                app.importRoomKeys(
                                    importPanel.selectedFileUrl,
                                    importPassphraseField.text)
                                // Wipe the passphrase from the QML field
                                // immediately — never keep it beyond the
                                // dispatch.
                                importPassphraseField.text = ""
                            }
                        }
                        Button {
                            text: qsTr("Clear")
                            enabled: !importPanel.running
                            onClicked: {
                                importPanel.selectedFileUrl = ""
                                importPanel.selectedFileName = ""
                                importPassphraseField.text = ""
                            }
                        }
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: importPanel.running
                        indeterminate: app.roomKeyImportTotalCount === 0
                        from: 0
                        to: Math.max(1, app.roomKeyImportTotalCount)
                        value: app.roomKeyImportImportedCount
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: importPanel.statusText !== ""
                        color: importPanel.statusColor
                        text: importPanel.statusText
                    }

                    QtObject {
                        id: importPanel
                        property url selectedFileUrl: ""
                        property string selectedFileName: ""
                        property bool running: false
                        property string statusText: ""
                        property color statusColor: AppTheme.textMuted
                    }

                    FileDialog {
                        id: importFileDialog
                        title: qsTr("Select encrypted Matrix room-key export")
                        fileMode: FileDialog.OpenFile
                        // Deliberately no `nameFilters` filter on extension —
                        // Element writes .txt exports; users may rename.
                        onAccepted: {
                            importPanel.selectedFileUrl = selectedFile
                            importPanel.selectedFileName =
                                selectedFile.toString().split('/').pop()
                            importPanel.statusText = ""
                        }
                    }

                    Connections {
                        target: app
                        function onRoomKeyImportStateChanged() {
                            var state = app.roomKeyImportState
                            if (state === "importing") {
                                importPanel.running = true
                                importPanel.statusText =
                                    qsTr("Importing room keys…")
                                importPanel.statusColor = AppTheme.textMuted
                            } else if (state === "done") {
                                importPanel.running = false
                                importPanel.statusText = qsTr(
                                    "Room-key import complete.\n" +
                                    "Imported sessions: %1\n" +
                                    "Affected rooms: %2\n" +
                                    "Note: importing keys does not verify this session.")
                                    .arg(app.roomKeyImportImportedCount)
                                    .arg(app.roomKeyImportAffectedRoomCount)
                                importPanel.statusColor = AppTheme.success
                                // Clear file selection for the next round;
                                // keep the completion summary visible.
                                importPanel.selectedFileUrl = ""
                                importPanel.selectedFileName = ""
                                importPassphraseField.text = ""
                            } else if (state === "failed") {
                                importPanel.running = false
                                importPanel.statusText =
                                    app.roomKeyImportLastMessage
                                importPanel.statusColor = AppTheme.error
                                importPassphraseField.text = ""
                            }
                        }
                    }

                    // ─── Recovery distinctions ─────────────────────────
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        text: qsTr(
                            "Verification establishes trust in this session. " +
                            "Secure Backup and room-key imports provide decryption keys " +
                            "for message history. These are separate operations.")
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

                // Sign out — only visible when authenticated
                Button {
                    visible: app.loggedIn
                    text: qsTr("Sign out")
                    onClicked: app.auth.logout()
                    contentItem: Label {
                        text: parent.text
                        color: AppTheme.danger
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: parent.pressed ? Qt.lighter(AppTheme.border, 0.9)
                             : parent.hovered ? AppTheme.hover : AppTheme.surface
                        border.color: AppTheme.danger
                        border.width: 1
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: qsTr("Back")
                    onClicked: app.loggedIn ? app.showMain() : app.showLogin()
                }
            }
        }
    }
}
