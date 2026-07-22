import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // Located by the startup-state suite: the login form must never be
    // instantiated during a valid-session launch.
    objectName: "loginScreen"

    function submit() {
        if (!app.auth.isLoggingIn)
            app.auth.login(homeserverField.text, userField.text,
                           passField.text)
    }

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

        Rectangle {
            id: panel
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(AppTheme.spacingXL,
                        (loginFlick.height - implicitHeight) / 2)
            width: Math.max(300, Math.min(loginFlick.width - AppTheme.spacingXL * 2, 420))
            implicitHeight: loginForm.implicitHeight + AppTheme.spacingXL * 2
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1

            ColumnLayout {
                id: loginForm
                x: AppTheme.spacingXL
                y: AppTheme.spacingXL
                width: parent.width - AppTheme.spacingXL * 2
                spacing: AppTheme.spacingM

                // v0.7 add-account flow: reached from the account switcher
                // while another account stays signed in — offer a way back
                // that does not touch the existing session. Bound to the
                // persisted active account (NOT app.loggedIn): a failed
                // add-account attempt releases the shared client's session,
                // and the Back button must survive that so the user can
                // return; showMain() self-heals.
                AppButton {
                    id: backToAppButton
                    visible: app.accounts && app.accounts.hasActiveAccount
                    Accessible.name: qsTr("Back to the app")
                    text: qsTr("← Back")
                    onClicked: app.showMain()
                }

                // Brand mark.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    Rectangle {
                        width: 10; height: 10; radius: 3
                        color: AppTheme.accent
                    }
                    Label {
                        text: "Lightning"
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: 15
                        font.weight: Font.ExtraBold
                        font.letterSpacing: 0.3
                    }
                }

                Label {
                    text: (app.accounts && app.accounts.hasActiveAccount)
                          ? qsTr("Add another account")
                          : qsTr("Sign in")
                    color: AppTheme.text
                    font.family: AppTheme.uiFont
                    font.pixelSize: 22
                    font.weight: Font.ExtraBold
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
                            return qsTr("Native Matrix backend with end-to-end encryption")
                        return qsTr("Sign in with your Matrix account")
                    }
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: AppTheme.spacingXS
                }

                Label {
                    text: qsTr("Homeserver URL")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                }
                AppTextField {
                    id: homeserverField
                    objectName: "homeserverField"
                    Layout.fillWidth: true
                    // Prefill ONCE from the account-independent login prefill,
                    // then let the user edit freely — no live binding to a
                    // settings getter. Binding text to homeserverUrl (which
                    // returns the ACTIVE account's server during the
                    // add-account flow) re-asserted itself and reverted typed
                    // input back to "your own" homeserver, making it
                    // impossible to point the field at a different one.
                    Component.onCompleted: text = app.settings.loginHomeserverPrefill
                    placeholderText: "https://matrix.org"
                    Accessible.name: qsTr("Homeserver URL")
                    onEditingFinished: app.settings.loginHomeserverPrefill = text
                    KeyNavigation.tab: userField
                }

                Label {
                    text: qsTr("User")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                }
                AppTextField {
                    id: userField
                    Layout.fillWidth: true
                    placeholderText: "@alice:matrix.org"
                    Accessible.name: qsTr("User")
                    KeyNavigation.tab: passField
                }

                Label {
                    text: qsTr("Password")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                }
                // Password field + reveal toggle share one row.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    AppTextField {
                        id: passField
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Password")
                        echoMode: passReveal.checked ? TextInput.Normal
                                                     : TextInput.Password
                        // Enter submits from the password field.
                        onAccepted: root.submit()
                    }
                    IconButton {
                        id: passReveal
                        checkable: true
                        implicitWidth: 34; implicitHeight: 34
                        radius: AppTheme.radiusMd
                        iconName: passReveal.checked ? "visibility_off"
                                                     : "visibility"
                        iconSize: 18
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
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                AppButton {
                    id: loginBtn
                    kind: "primary"
                    text: app.auth.isLoggingIn ? qsTr("Signing in…") : qsTr("Sign in")
                    enabled: !app.auth.isLoggingIn
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    onClicked: root.submit()
                }

                // v0.5.0-prep+11: Rust SDK store/device mismatch recovery.
                // Only visible when AppController reports the "account in the
                // store doesn't match" SDK error, and only on the Rust backend.
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
                    Layout.topMargin: AppTheme.spacingS
                    wrapMode: Text.WordWrap
                    color: AppTheme.error
                    font.family: AppTheme.uiFont
                    font.pixelSize: 12
                    text: qsTr(
                        "This local Lightning Rust SDK store belongs to a different "
                        + "Matrix session or device. Reset the local Lightning session "
                        + "for this account, then sign in again. This does not delete "
                        + "server messages or Element data.")
                }
                AppButton {
                    visible: app.localRustResetRequired
                    kind: "danger"
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
                    font.family: AppTheme.uiFont
                    font.pixelSize: 11
                    text: mismatchPanel.message
                }
            }
        }
    } // Flickable
}
