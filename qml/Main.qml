import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

ApplicationWindow {
    id: window
    width: 1100
    height: 720
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    title: qsTr("matrix-client %1").arg(app.appVersion)

    color: AppTheme.background

    Component.onCompleted: {
        if (app.settings && app.settings.startMinimized)
            window.visibility = Window.Minimized
    }

    // Push the current theme selection into the AppTheme singleton so all
    // consumers repaint on change.
    Binding {
        target: AppTheme
        property: "mode"
        value: app.settings ? app.settings.theme : 0
    }

    header: ToolBar {
        background: Rectangle { color: AppTheme.surface; border.width: 0 }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: AppTheme.spacingM
            anchors.rightMargin: AppTheme.spacingM
            spacing: AppTheme.spacingM

            Label {
                text: qsTr("Matrix")
                color: AppTheme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Label {
                text: app.accounts && app.accounts.hasActiveAccount
                      ? app.accounts.activeUserId
                      : qsTr("Not signed in")
                color: AppTheme.textMuted
                Layout.leftMargin: AppTheme.spacingS
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                text: qsTr("Rooms")
                enabled: app.loggedIn && app.currentScreen !== app.MainScreen
                visible: app.loggedIn
                onClicked: app.showMain()
            }
            ToolButton {
                text: qsTr("Settings")
                onClicked: app.showSettings()
            }
            ToolButton {
                text: qsTr("Sign out")
                visible: app.loggedIn
                onClicked: app.auth.logout()
            }
        }
    }

    Loader {
        id: pageLoader
        anchors.fill: parent
        sourceComponent: {
            switch (app.currentScreen) {
            case app.LoginScreen:    return loginComponent
            case app.MainScreen:     return mainComponent
            case app.SettingsScreen: return settingsComponent
            }
            return loginComponent
        }
    }

    Component { id: loginComponent;    LoginScreen {} }
    Component { id: mainComponent;     MainScreen {} }
    Component { id: settingsComponent; SettingsScreen {} }

    footer: Rectangle {
        color: AppTheme.surface
        implicitHeight: statusRow.implicitHeight + AppTheme.spacingS * 2
        RowLayout {
            id: statusRow
            anchors.fill: parent
            anchors.margins: AppTheme.spacingS
            spacing: AppTheme.spacingM
            Label {
                text: app.backendName === "mock"
                      ? qsTr("Mock backend • %1").arg(app.connectionStatus)
                      : qsTr("HTTP backend • %1").arg(app.connectionStatus)
                color: AppTheme.textMuted
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: statusBar.lastError !== ""
                text: statusBar.lastError
                color: AppTheme.error
                font.pixelSize: 12
            }
        }
    }

    QtObject {
        id: statusBar
        property string lastError: ""
    }

    Connections {
        target: app
        function onErrorReported(msg) { statusBar.lastError = msg }
    }
}
