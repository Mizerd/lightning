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

    Pane {
        anchors.centerIn: parent
        padding: AppTheme.spacingXL
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1
            radius: AppTheme.radius
        }

        ColumnLayout {
            spacing: AppTheme.spacingM
            width: 360

            Label {
                text: qsTr("Sign in to Lightning")
                color: AppTheme.text
                font.pixelSize: 22
                font.weight: Font.DemiBold
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
            }

            Label { text: qsTr("User"); color: AppTheme.textMuted; font.pixelSize: 12 }
            TextField {
                id: userField
                Layout.fillWidth: true
                placeholderText: "@alice:matrix.org"
                text: "alice"
            }

            Label { text: qsTr("Password"); color: AppTheme.textMuted; font.pixelSize: 12 }
            TextField {
                id: passField
                Layout.fillWidth: true
                echoMode: TextInput.Password
                text: "hunter2"
            }

            Label {
                visible: app.auth.lastError !== ""
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
        }
    }
}
