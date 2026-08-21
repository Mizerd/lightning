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
    // window is never mistaken for a real account. It drops automatically when
    // the demo controls are hidden, for a fully clean final screenshot.
    title: (app.screenshotDemoActive && (!app.demo || app.demo.controlsVisible))
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

    // SECURITY, application-wide. Qt Quick Controls uses ONE shared ToolTip
    // instance for every attached `ToolTip.text`, and the Basic style builds
    // its contentItem as a Text with the default AutoText format. AutoText
    // runs mightBeRichText() over the string, so any tooltip whose text can
    // begin with markup is promoted to StyledText — and several of ours carry
    // remote-chosen member display names (the reaction reactor list, the
    // read-receipt strip). A display name containing an <img src="https://…">
    // would then make every viewer who merely HOVERS fetch that URL: an
    // unconsented remote beacon reporting IP and timing, inside rooms where
    // link previews are deliberately off by default.
    //
    // Fixing it once on the shared instance is what makes it a property of
    // the application rather than of whichever call site someone remembered.
    // Declaring a per-chip ToolTip with a plain-text contentItem would also
    // work but costs a Popup + background + Label PER CHIP, which is the
    // eager per-row instantiation this timeline has already un-done twice.
    //
    // It lives on an Item, not on the window: ToolTip is an attached property
    // of Item, and attaching it to an ApplicationWindow warns
    // "ToolTip attached property must be attached to an object deriving from
    // Item" — which the QML-warning suites correctly fail on.
    Item {
        id: sharedToolTipGuard
        objectName: "sharedToolTipPlainTextGuard"

        // The same shared instance also carries the app's most-seen popup
        // chrome, and it was the only popup in the product that was not
        // rounded: Basic's ToolTip background is a square Rectangle outlined
        // in `palette.dark`, which Main.qml maps to the theme's secondary
        // TEXT colour — a text-weight grey hairline around every tip, on a
        // UI whose hairlines are AppTheme.border.
        //
        // These are imperative assignments rather than bindings because the
        // instance is created by the style, not declared here — so they must
        // be re-applied whenever the palette moves, which is what the
        // Connections below is for. Fill and ink already follow the palette.
        function applyToolTipChrome() {
            const shared = ToolTip.toolTip
            if (!shared)
                return
            const bg = shared.background
            if (!bg)
                return
            if (bg.radius !== undefined)
                bg.radius = AppTheme.radiusMd
            if (bg.border !== undefined)
                bg.border.color = AppTheme.border
        }

        Component.onCompleted: {
            // Touching `contentItem` forces the lazy instance to exist.
            // Guarded: a style whose tooltip content is not a Text simply has
            // no textFormat, and must not throw here.
            const shared = ToolTip.toolTip
            if (shared && shared.contentItem
                    && shared.contentItem.textFormat !== undefined)
                shared.contentItem.textFormat = Text.PlainText
            applyToolTipChrome()
        }

        Connections {
            target: AppTheme
            function onEffectiveThemeChanged() {
                sharedToolTipGuard.applyToolTipChrome()
            }
        }
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

        // 2026-08-18 tester report ("tarpas neveikia pause ir unpause"):
        // Space toggles whatever inline media is currently audible.
        //
        // Deliberately a Keys handler on an ANCESTOR, not a window
        // Shortcut. A Shortcut is consumed before the focused item ever
        // sees the key, which would silently take Space away from every
        // control that already uses it — the timeline's page-down, the
        // emoji and GIF grids, the focused player button itself. Key
        // events instead bubble UP the parent chain, so this only ever
        // sees a Space that nothing else wanted, and typing a space in
        // the composer is untouched.
        Keys.onSpacePressed: (event) => {
            // Held Space would otherwise toggle on every auto-repeat.
            if (event.isAutoRepeat) {
                event.accepted = false
                return
            }
            if (app.playback.audibleOwner.length > 0) {
                app.playback.requestTogglePlayPause()
                event.accepted = true
            } else {
                event.accepted = false
            }
        }
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
                // Brand mark above the wordmark — same bolt-in-tile idiom as
                // the room-list workspace header.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 44
                    height: 44
                    radius: AppTheme.radiusLg
                    color: AppTheme.accentSoft
                    Icon {
                        anchors.centerIn: parent
                        name: "bolt"
                        size: 28
                        color: AppTheme.accent
                    }
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Lightning")
                    color: AppTheme.text
                    font.pixelSize: 26
                    font.weight: Font.ExtraBold
                }
                AppBusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 26
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

    // ── Development-only screenshot-demo control panel ───────────────────
    // A floating control panel (scenario/account/room/theme/appearance/size
    // selectors, toggles, reset) that also identifies the fake data. It is an
    // overlay child of the window (not in any layout), so hiding it leaves NO
    // gap. `app.screenshotDemoActive` is always false in a normal/release
    // binary, so the panel component is never even loaded in production.
    // Ctrl+Shift+D hides/restores it (app.demo.controlsVisible).
    Shortcut {
        sequence: "Ctrl+Shift+D"
        enabled: app.screenshotDemoActive
        onActivated: if (app.demo) app.demo.toggleControls()
    }

    // Interface zoom (Discord-style shortcuts). The value is applied as
    // QT_SCALE_FACTOR at startup — Qt reads it exactly once — so changes
    // take effect on the next launch; the transient notice says so
    // honestly instead of silently doing nothing.
    function _adjustZoom(delta) {
        var next = delta === 0 ? 100 : app.settings.interfaceZoom + delta
        app.settings.interfaceZoom = next
        zoomNotice.show()
    }
    Shortcut {
        sequences: ["Ctrl+=", "Ctrl++"]
        onActivated: window._adjustZoom(5)
    }
    Shortcut {
        sequences: ["Ctrl+-"]
        onActivated: window._adjustZoom(-5)
    }
    Shortcut {
        sequences: ["Ctrl+0"]
        onActivated: window._adjustZoom(0)
    }

    Rectangle {
        id: zoomNotice
        function show() { visible = true; zoomNoticeTimer.restart() }
        visible: false
        // In the OVERLAY, above any open popup — a plain window child
        // renders below Popups and the notice would be invisible with
        // Settings or a picker open (review nit).
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: AppTheme.spacing16
        z: 1000
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        width: zoomNoticeLabel.implicitWidth + AppTheme.spacing16 * 2
        height: zoomNoticeLabel.implicitHeight + AppTheme.spacing12
        Label {
            id: zoomNoticeLabel
            anchors.centerIn: parent
            text: qsTr("Interface zoom %1% — takes effect after restart")
                  .arg(app.settings.interfaceZoom)
            color: AppTheme.stormText
            font.pixelSize: AppTheme.fontSecondary
        }
        Timer {
            id: zoomNoticeTimer
            interval: 2500
            onTriggered: zoomNotice.visible = false
        }
    }
    // v0.7.x pinned messages: a pin/unpin FAILURE has to be visible where the
    // action was taken. The common path is the message context menu, which
    // closes on trigger — without this the only report was app.pinned.error
    // inside Room Information → Pinned, a surface the user is usually not
    // looking at. Same overlay-parented transient shape as zoomNotice above
    // (it must sit above any open popup, for the same reason).
    // Success is silent on purpose: the pinned list updating IS the feedback.
    Rectangle {
        id: pinNotice
        objectName: "pinActionNotice"
        property string message: ""
        function show(text) { message = text; visible = true; pinNoticeTimer.restart() }
        visible: false
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: AppTheme.spacing16
        z: 1000
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormDanger
        border.width: 1
        width: Math.min(pinNoticeLabel.implicitWidth + AppTheme.spacing16 * 2,
                        window.width - AppTheme.spacing16 * 2)
        height: pinNoticeLabel.implicitHeight + AppTheme.spacing12
        Accessible.role: Accessible.AlertMessage
        Accessible.name: pinNotice.message
        Label {
            id: pinNoticeLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, window.width - AppTheme.spacing16 * 4)
            text: pinNotice.message
            color: AppTheme.stormText
            font.pixelSize: AppTheme.fontSecondary
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
        Timer {
            id: pinNoticeTimer
            interval: 4000
            onTriggered: pinNotice.visible = false
        }
        // 2026-08-18 "Remove edits": the message menu is closed by the time
        // the server answers, so the outcome is reported here. Removing
        // nothing is a real outcome and is stated as such rather than being
        // passed off as success.
        Connections {
            target: app.composer
            function onEditsRemoved(eventId, ok, removed, failed, truncated) {
                // ok == false with nothing attempted means the edits could
                // not be READ at all (offline, cache miss). Saying "there
                // are none" there would be a lie about the message.
                if (!ok && removed === 0 && failed === 0) {
                    // One literal: qsTr() on a concatenation is not
                    // extractable by lupdate, so a split string would never
                    // be translatable.
                    pinNotice.show(qsTr("This message's edits could not be read. Check your connection and try again."))
                } else if (failed > 0) {
                    pinNotice.show(
                        qsTr("Removed %1 edit(s); %2 could not be removed.")
                            .arg(removed).arg(failed))
                } else if (removed === 0) {
                    pinNotice.show(qsTr("No edits could be found to remove."))
                } else if (truncated) {
                    pinNotice.show(
                        qsTr("Removed %1 edits. More remain — run it again.")
                            .arg(removed))
                }
            }
        }
        Connections {
            target: app.pinned
            function onPinActionFinished(roomId, eventId, pin, ok, message) {
                if (!ok && message.length > 0)
                    pinNotice.show(message)
            }
        }
        // v0.7.x forwarding: a forwarded ATTACHMENT is a direct upload, not
        // a queued send, and the target timeline was not open when it was
        // dispatched — so there is no local echo to fail visibly. Without
        // this the user arrives in the target room, sees nothing, and
        // believes the forward worked. Reuses this notice rather than adding
        // a second transient banner class.
        Connections {
            target: app.forward
            function onForwardFailed(targetRoomId, message) {
                if (message.length === 0)
                    return
                // Only while the user is still looking at the room it was
                // aimed at — the same decision the voice-send path makes.
                // A context-free "could not be forwarded" about a room they
                // have since left names nothing and helps nobody.
                if (targetRoomId.length > 0
                        && targetRoomId !== app.currentRoomId)
                    return
                pinNotice.show(message)
            }
        }
    }

    // v0.7.x session verification. ONE dialog for the whole app: it follows
    // AppController's verification state and opens itself, so Settings, the
    // corner prompt and an INCOMING request from another client all surface
    // through the same presentation. Declaring it per-page would give two
    // instances that both react to the same state.
    VerificationDialog {
        id: verificationDialog
        objectName: "verificationDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    // v0.7.x: the ONE UIA prompt. Opens itself off UiaController's
    // challenge state, so every privileged operation that hits a server
    // challenge surfaces through the same presentation — never a per-page
    // password dialog.
    UiaPromptDialog {
        id: uiaPromptDialog
        parent: Overlay.overlay
    }

    // v0.7.x: the ONE report-message prompt (opens itself off
    // ModerationController's pending-report state).
    ReportMessageDialog {
        id: reportMessageDialog
        parent: Overlay.overlay
    }

    // v0.7.x message forwarding (task #14): the ONE forward room-picker
    // (opens itself off ForwardController's `active` state).
    ForwardMessageDialog {
        id: forwardMessageDialog
        parent: Overlay.overlay
    }

    // The ONE update-available prompt. Like the dialogs above it opens itself
    // off UpdateManager's state and closes when the version is dismissed, so
    // it needs no wiring here beyond existing — but it does need to exist:
    // without this instance the update-available state has no prompt at all
    // and the only way to learn about an update is to open Settings.
    UpdateAvailableDialog {
        id: updateAvailableDialog
        parent: Overlay.overlay
    }

    // First-run nudge. Deliberately a corner card rather than a modal: an
    // unverified session still works, so this must not block the app.
    // Both corner prompts share ONE bottom-right column so they can never
    // draw on top of each other: an unverified session and a pending update
    // are independent conditions and are routinely true at the same time.
    // A hidden prompt sets visible:false, so Column reclaims its space and
    // no gap is left behind when only one is showing.
    Column {
        objectName: "cornerPromptHost"
        parent: Overlay.overlay
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing8
        z: 900

        // Update first (above), verification nearest the corner: the
        // security prompt is the more important of the two and keeps the
        // anchored position it already had.
        // A live ring outranks the passive prompts: first in the column,
        // so it renders above them while they keep their corner spots.
        IncomingCallPrompt {
            objectName: "incomingCallPromptHost"
        }
        UpdateAvailablePrompt {
            objectName: "updateAvailablePromptHost"
            onDetailsRequested: updateAvailableDialog.open()
        }
        VerifySessionPrompt {
            objectName: "verifySessionPromptHost"
        }
    }

    Loader {
        active: app.screenshotDemoActive
        anchors.fill: parent
        z: 100
        // String source is resolved at runtime, so a non-demo build (where the
        // component is not in the module and active is always false) never
        // references it.
        source: app.screenshotDemoActive ? "DemoControlPanel.qml" : ""
    }

    // Slim status strip: shown only while something needs attention
    // (connecting, offline, error) or on the login screen; the steady
    // "Connected" state stays quiet per the design's low-noise shell.
    footer: Rectangle {
        color: AppTheme.surface
        // The screenshot demo runs on the mock backend; its connection footer
        // ("Mock backend • …") is meaningless there and only clutters clean
        // promotional screenshots, so it is suppressed in demo mode only.
        visible: !app.screenshotDemoActive
                 && (app.currentScreen !== 1
                     || app.connectionStatus !== qsTr("Connected")
                     || statusBar.lastError !== "")
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
