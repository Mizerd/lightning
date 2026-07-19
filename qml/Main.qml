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
    title: qsTr("Lightning %1").arg(app.appVersion)

    color: AppTheme.background

    // Theme correctness for every native Qt Quick Controls surface: the
    // Fusion style paints ComboBox popups, Menus, ToolTips, ScrollBars,
    // and button chrome from the control palette, not from our tokens.
    // Binding the window palette to the tokens propagates the active theme
    // into all of them (including popups) and makes switching instant —
    // this is what previously left dropdowns black/wrong after a change.
    palette {
        window: AppTheme.background
        windowText: AppTheme.textPrimary
        base: AppTheme.inputBackground
        alternateBase: AppTheme.cardElevated
        text: AppTheme.textPrimary
        button: AppTheme.cardElevated
        buttonText: AppTheme.textPrimary
        highlight: AppTheme.selected
        highlightedText: AppTheme.selectedText
        placeholderText: AppTheme.textMuted
        toolTipBase: AppTheme.cardElevated
        toolTipText: AppTheme.textPrimary
        light: AppTheme.hover
        midlight: AppTheme.border
        mid: AppTheme.borderStrong
        dark: AppTheme.textSecondary
        brightText: AppTheme.accentText
        link: AppTheme.link
        disabled {
            text: AppTheme.textDisabled
            buttonText: AppTheme.textDisabled
            windowText: AppTheme.textDisabled
        }
    }

    Component.onCompleted: {
        if (app.settings && app.settings.startMinimized)
            window.visibility = Window.Minimized
    }

    // v0.6.0 checkpoint 11: a clicked notification raises Lightning, selects
    // the room, opens the thread when it was a thread reply, and locates the
    // event (the existing navigation shows a safe message when the target is
    // unavailable). Identity only — the payload never carries tokens.
    Connections {
        target: app
        function onNotificationOpenRequested(roomId, eventId, threadRootId) {
            window.show()
            window.raise()
            window.requestActivate()
            if (roomId === "")
                return
            app.showMain()
            app.currentRoomId = roomId
            if (threadRootId && threadRootId.length > 0)
                app.thread.openThread(roomId, threadRootId)
            if (eventId && eventId.length > 0)
                Qt.callLater(function() {
                    app.pagination.jumpToEvent(eventId)
                })
        }
    }

    // Push the current theme selection into the AppTheme singleton so all
    // consumers repaint on change.
    Binding {
        target: AppTheme
        property: "mode"
        value: app.settings ? app.settings.theme : 0
    }
    // v0.5.11: the platform light/dark preference drives the "System" theme.
    Binding {
        target: AppTheme
        property: "systemDark"
        value: app.systemDarkMode
    }

    // v0.7 design shell: no global header bar — the shell columns carry
    // their own headers (room-list workspace header, room header).

    Loader {
        id: pageLoader
        anchors.fill: parent

        // AppController::Screen enum ordering — kept in sync with
        // src/app/AppController.h. We hard-code the integers here
        // instead of `case app.LoginScreen:` because a switch whose
        // case expressions read enum values on a context-property-
        // exposed QObject was falling through under some Qt Quick
        // compiler configurations, which kept HTTP login stuck on the
        // login screen even after `loginSucceeded` fired (v0.4.4 bug).
        // Integer literals against the notify-tracked
        // `app.currentScreen` property are unambiguous.
        function pickComponent() {
            var s = app.currentScreen
            if (s === 1) return mainComponent      // MainScreen
            if (s === 2) return settingsComponent  // SettingsScreen
            return loginComponent                  // 0 = LoginScreen
        }
        sourceComponent: pickComponent()

        // Belt-and-braces re-eval on the explicit signal. If the binding
        // above tracks the property correctly this is a no-op; if it
        // doesn't (as in the v0.4.4 bug), this closes the gap.
        Connections {
            target: app
            function onCurrentScreenChanged() {
                pageLoader.sourceComponent = pageLoader.pickComponent()
            }
        }
    }

    Component { id: loginComponent;    LoginScreen {} }
    Component { id: mainComponent;     MainScreen {} }
    Component { id: settingsComponent; SettingsScreen {} }

    // Slim status strip: shown only while something needs attention
    // (connecting, offline, error) or on the login screen; the steady
    // "Connected" state stays quiet per the design's low-noise shell.
    footer: Rectangle {
        color: AppTheme.surface
        visible: app.currentScreen !== 1
                 || app.connectionStatus !== qsTr("Connected")
                 || statusBar.lastError !== ""
        implicitHeight: visible
                        ? statusRow.implicitHeight + AppTheme.spacingS * 2 : 0
        RowLayout {
            id: statusRow
            anchors.fill: parent
            anchors.margins: AppTheme.spacingS
            spacing: AppTheme.spacingM
            // v0.5.0-prep+12: coloured status dot + backend label so
            // "Connected" / "Error" is legible at a glance.
            Rectangle {
                id: statusDot
                Layout.alignment: Qt.AlignVCenter
                width: 8; height: 8
                radius: width / 2
                color: {
                    var s = app.connectionStatus
                    if (s === qsTr("Connected"))    return AppTheme.success
                    if (s === qsTr("Error"))        return AppTheme.error
                    if (s === qsTr("Offline — retrying")) return AppTheme.warning
                    if (s === qsTr("Connecting…") ||
                        s === qsTr("Syncing")   ||
                        s === qsTr("Loading rooms…")) return AppTheme.warning
                    return AppTheme.muted
                }
            }
            Label {
                text: {
                    var label = qsTr("HTTP backend")
                    if (app.backendName === "mock")
                        label = qsTr("Mock backend")
                    else if (app.backendName === "rust")
                        label = qsTr("Matrix Rust SDK")
                    return qsTr("%1 • %2").arg(label).arg(app.connectionStatus)
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
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
