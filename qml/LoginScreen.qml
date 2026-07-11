import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: AppTheme.background
    }

    // v0.5.11: the login panel is a Flickable so an overflowing form (long
    // errors, high-DPI scaling, short windows) scrolls instead of clipping,
    // and the panel width tracks the window between a sensible min and max.
    Flickable {
        id: loginFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: panel.implicitHeight + AppTheme.spacingXL * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    Pane {
        id: panel
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(AppTheme.spacingXL,
                    (loginFlick.height - implicitHeight) / 2)
        padding: AppTheme.spacingXL
        // Track the window width but stay readable at both extremes.
        width: Math.max(300, Math.min(loginFlick.width - AppTheme.spacingXL * 2, 420))
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1
            radius: AppTheme.radius
        }

        ColumnLayout {
            id: loginForm
            width: parent.availableWidth
            spacing: AppTheme.spacingM

            Label {
                text: qsTr("Sign in to Lightning")
                color: AppTheme.text
                font.pixelSize: 22
                font.weight: Font.DemiBold
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: {
                    // Backend-aware sub-heading so the user knows what
                    // backend they're signing into.
                    if (app.backendName === "mock")
                        return qsTr("Mock backend — any credentials work")
                    if (app.backendName === "rust")
                        return qsTr("Rust SDK backend — E2EE is not verified yet")
                    return qsTr("HTTP backend — sign in with your Matrix account")
                }
                color: AppTheme.textMuted
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label { text: qsTr("Homeserver URL"); color: AppTheme.textMuted; font.pixelSize: 12 }
            TextField {
                id: homeserverField
                Layout.fillWidth: true
                text: app.settings.homeserverUrl
                placeholderText: "https://matrix.org"
                onEditingFinished: app.settings.homeserverUrl = text
                KeyNavigation.tab: userField
            }

            Label { text: qsTr("User"); color: AppTheme.textMuted; font.pixelSize: 12 }
            TextField {
                id: userField
                Layout.fillWidth: true
                placeholderText: "@alice:matrix.org"
                KeyNavigation.tab: passField
            }

            Label { text: qsTr("Password"); color: AppTheme.textMuted; font.pixelSize: 12 }
            // Password field + reveal toggle share one row.
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacingXS
                TextField {
                    id: passField
                    Layout.fillWidth: true
                    echoMode: passReveal.checked ? TextInput.Normal
                                                 : TextInput.Password
                    // Enter submits from the password field.
                    onAccepted: if (!app.auth.isLoggingIn)
                        app.auth.login(homeserverField.text, userField.text,
                                       passField.text)
                }
                ToolButton {
                    id: passReveal
                    checkable: true
                    text: checked ? "🙈" : "👁"
                    Accessible.name: checked ? qsTr("Hide password")
                                             : qsTr("Show password")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                }
            }

            Label {
                visible: app.auth.lastError !== "" && !app.localRustResetRequired
                text: app.auth.lastError
                color: AppTheme.error
                font.pixelSize: 12
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Button {
                id: loginBtn
                text: app.auth.isLoggingIn ? qsTr("Signing in…") : qsTr("Sign in")
                enabled: !app.auth.isLoggingIn
                Layout.fillWidth: true
                highlighted: true
                onClicked: app.auth.login(homeserverField.text, userField.text, passField.text)
            }

            Label {
                text: qsTr("SSO / OIDC login arrives in v0.5.")
                color: AppTheme.textMuted
                font.pixelSize: 11
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }

            // v0.5.0-prep+11: Rust SDK store/device mismatch recovery.
            // Only visible when AppController reports the "account in
            // the store doesn't match" SDK error, and only on the Rust
            // backend.
            QtObject {
                id: mismatchPanel
                property string message: ""
                property bool ok: false
            }
            Connections {
                target: app
                function onStoreDeviceMismatchDetected(displayMessage) {
                    mismatchPanel.message = ""
                    mismatchPanel.ok = false
                }
                function onLocalRustStoreResetResult(ok, message) {
                    mismatchPanel.ok = ok
                    mismatchPanel.message = message
                }
            }
            Label {
                visible: app.localRustResetRequired
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.error
                text: qsTr(
                    "This local Lightning Rust SDK store belongs to a different "
                    + "Matrix session or device. Reset the local Lightning session "
                    + "for this account, then sign in again. This does not delete "
                    + "server messages or Element data.")
            }
            Button {
                visible: app.localRustResetRequired
                Layout.fillWidth: true
                text: qsTr("Reset local Lightning session")
                onClicked: app.resetLocalRustSession(
                    homeserverField.text, userField.text)
            }
            Label {
                visible: mismatchPanel.message !== ""
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: mismatchPanel.ok ? AppTheme.success : AppTheme.error
                font.pixelSize: 11
                text: mismatchPanel.message
            }
        }
    }
    } // Flickable
}
