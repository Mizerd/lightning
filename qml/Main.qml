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
    // Development screenshot mode gets a clear window-title suffix so a demo
    // window is never mistaken for a real account. `demoTitleSuffix` lets the
    // developer drop it for a final clean screenshot (see the demo badge).
    property bool demoTitleSuffix: true
    title: (app.screenshotDemoActive && demoTitleSuffix)
           ? qsTr("Lightning — Screenshot Demo")
           : qsTr("Lightning %1").arg(app.appVersion)

    color: AppTheme.background

    // Theme correctness for every native Qt Quick Controls surface. The
    // window item palette below covers in-window chrome (buttons, fields),
    // but Fusion resolves POPUPS (ComboBox dropdowns, Menus, ToolTips,
    // ScrollBars, Dialogs) from the *application* palette, which an item
    // palette never reaches — that is what left dropdowns the default light
    // colour on a dark theme. syncControlPalette() pushes the same tokens
    // onto QGuiApplication so popups follow the theme too; it runs on load
    // and on every theme change.
    function syncControlPalette() {
        app.applyControlPalette({
            "window": AppTheme.background,
            "windowText": AppTheme.textPrimary,
            "base": AppTheme.inputBackground,
            "alternateBase": AppTheme.cardElevated,
            "text": AppTheme.textPrimary,
            "button": AppTheme.cardElevated,
            "buttonText": AppTheme.textPrimary,
            "highlight": AppTheme.selected,
            "highlightedText": AppTheme.selectedText,
            "placeholderText": AppTheme.textMuted,
            "toolTipBase": AppTheme.cardElevated,
            "toolTipText": AppTheme.textPrimary,
            "light": AppTheme.hover,
            "midlight": AppTheme.border,
            "mid": AppTheme.borderStrong,
            "dark": AppTheme.textSecondary,
            "brightText": AppTheme.accentText,
            "link": AppTheme.link,
            "disabledText": AppTheme.textDisabled,
            "disabledButtonText": AppTheme.textDisabled,
            "disabledWindowText": AppTheme.textDisabled
        })
    }
    Connections {
        target: AppTheme
        function onEffectiveThemeChanged() { window.syncControlPalette() }
    }

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
        syncControlPalette()
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
    // Content text scale (Settings → Appearance → Text size).
    Binding {
        target: AppTheme
        property: "textScale"
        value: app.settings ? app.settings.textScale / 100 : 1
    }
    // v0.7: the selected UI font follows the per-account Appearance
    // setting. Controls inherit through the window font; explicit
    // AppTheme.uiFont bindings cover non-inheriting text items.
    Binding {
        target: AppTheme
        property: "uiFont"
        value: app.settings && app.settings.uiFont.length > 0
               ? app.settings.uiFont : "Manrope"
    }
    font.family: AppTheme.uiFont

    // v0.7 design shell: no global header bar — the shell columns carry
    // their own headers (room-list workspace header, room header).

    Loader {
        id: pageLoader
        anchors.fill: parent
        // Hidden (not unloaded) while the full-view Settings covers the
        // content area: chat state survives without being visible,
        // interactive, or part of active layout.
        visible: app.currentScreen !== 2
        enabled: visible

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
            // Settings (2) keeps MainScreen LOADED (so the selected room,
            // timeline position, and drafts survive) but hidden — the
            // full-view Settings loader below covers the entire content
            // area.
            if (s === 1 || s === 2) return mainComponent
            // 3 = BootScreen: a saved session is restoring. The login form
            // is never instantiated in this state — a valid-session launch
            // goes Boot -> Main without the form ever existing.
            if (s === 3) return bootComponent
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
    // Minimal branded restoration surface: theme background, wordmark, one
    // quiet spinner. No credentials fields, no stale room content, and the
    // saved theme already resolved (AppTheme.mode binds before load).
    Component {
        id: bootComponent
        Rectangle {
            objectName: "startupRestoreSurface"
            color: AppTheme.background
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingM
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Lightning")
                    color: AppTheme.text
                    font.pixelSize: 26
                    font.weight: Font.ExtraBold
                }
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 26; height: 26
                    running: app.currentScreen === 3
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Restoring your session…")
                    color: AppTheme.textMuted
                    font.pixelSize: 13
                }
            }
        }
    }

    // Full application-view Settings: occupies the entire content area
    // below the window title bar. The spaces rail, room list, timeline,
    // composer, and any right-side panel are hidden while it is open.
    Loader {
        objectName: "settingsViewLoader"
        anchors.fill: parent
        active: app.currentScreen === 2
        visible: active
        z: 5
        sourceComponent: SettingsScreen {}
        onLoaded: item.forceActiveFocus()
    }

    // ── Development-only screenshot-demo badge ───────────────────────────
    // A floating pill that identifies demo data so a demo window is never
    // confused with a real account. It is an overlay child of the window (not
    // in any layout), so hiding it leaves NO gap. `app.screenshotDemoActive` is
    // always false in a normal/release binary, so this never appears in
    // production. Hide it for a clean final screenshot; Ctrl+Shift+D restores.
    property bool demoControlsVisible: true
    Shortcut {
        sequence: "Ctrl+Shift+D"
        enabled: app.screenshotDemoActive
        onActivated: window.demoControlsVisible = !window.demoControlsVisible
    }
    Rectangle {
        id: demoBadge
        visible: app.screenshotDemoActive && window.demoControlsVisible
        z: 100
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 10
        width: demoBadgeRow.implicitWidth + 24
        height: 30
        radius: 15
        color: AppTheme.accent
        Row {
            id: demoBadgeRow
            anchors.centerIn: parent
            spacing: 10
            Label {
                text: qsTr("● Screenshot Demo — fake data")
                color: "#FFFFFF"
                font.family: AppTheme.uiFont
                font.pixelSize: 12
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Label {
                text: qsTr("Hide")
                color: "#FFFFFF"
                opacity: 0.9
                font.family: AppTheme.uiFont
                font.pixelSize: 11
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.demoControlsVisible = false
                }
            }
        }
    }

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
