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
