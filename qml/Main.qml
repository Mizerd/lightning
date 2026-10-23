import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

ApplicationWindow {
    id: window

    // Window geometry, restored from the last run and saved as the user moves
    // and resizes. AppController::restorableWindowGeometry is constant,
    // already validated against the current display layout and fitted to the
    // screen it lands on; an invalid rect means "do not restore".
    readonly property rect startupGeometry: app.restorableWindowGeometry
    readonly property bool hasStartupGeometry: startupGeometry.width > 0
                                               && startupGeometry.height > 0
    width: hasStartupGeometry ? startupGeometry.width : 1100
    height: hasStartupGeometry ? startupGeometry.height : 720
    minimumWidth: 640
    minimumHeight: 420

    // Start hidden, compute the position once, assign it imperatively, then
    // show. Bindings on x/y re-centred the window whenever screen metrics or
    // width changed, fighting the user's drags; and Qt shows the window during
    // componentComplete(), so placing it later from onCompleted makes it jump.
    visible: false

    function applyStartupPlacement() {
        if (hasStartupGeometry) {
            // Validated in C++ against the current display layout, including
            // legitimate negative coordinates of monitors left of or above the
            // primary.
            x = startupGeometry.x
            y = startupGeometry.y
            app.noteWindowPlacement("restored", x, y, width, height)
            return
        }
        // Centred on one screen's available area
        // (AppController::centredWindowRect); the virtual desktop's width would
        // aim between two monitors. An empty rect means the metrics could not
        // be trusted, and the window manager places it.
        var centred = app.centredWindowRect(width, height)
        if (centred.width > 0 && centred.height > 0) {
            x = centred.x
            y = centred.y
            app.noteWindowPlacement("centred", x, y, width, height)
            return
        }
        // No usable metrics (headless or still settling): leave the platform's
        // placement rather than centring against zeroes.
        app.noteWindowPlacement("platform-default", x, y, width, height)
    }

    // Screenshot demo mode adds a title suffix so a demo window is never taken
    // for a real account; it drops when the demo controls are hidden. The title
    // carries the unread count, since the taskbar is visible while Lightning is
    // not: rooms rather than messages, with mentions separately.
    readonly property string unreadTitleSuffix: {
        if (!app.roomList)
            return ""
        var rooms = app.roomList.unreadRoomCount
        if (rooms <= 0)
            return ""
        var mentions = app.roomList.highlightRoomCount
        if (mentions > 0)
            return qsTr("(%1 unread, %2 ●) ").arg(rooms).arg(mentions)
        return qsTr("(%1 unread) ").arg(rooms)
    }
    title: (app.screenshotDemoActive && (!app.demo || app.demo.controlsVisible))
           ? qsTr("Lightning — Screenshot Demo")
           : unreadTitleSuffix + qsTr("Lightning %1").arg(app.appVersion)

    color: AppTheme.background

    // Right-to-left: LocalizationManager sets the layout direction; anchors and
    // Layouts only mirror where LayoutMirroring says so, so enabling it here
    // with childrenInherit mirrors the whole shell. It never mirrors pixels
    // (images, video, avatars), and the timeline's vertical rotation is
    // unaffected. CodeBlock opts out: code reads left to right.
    LayoutMirroring.enabled: app.localization.rightToLeft
    LayoutMirroring.childrenInherit: true

    // Popups (ComboBox dropdowns, Menus, ToolTips, ScrollBars, Dialogs) resolve
    // their palette from the application palette, which an item palette never
    // reaches; push the same tokens onto QGuiApplication on load and on every
    // theme change.
    function syncControlPalette() {
        app.applyControlPalette({
            "window": AppTheme.background,
            "windowText": AppTheme.textPrimary,
            "base": AppTheme.inputBackground,
            "alternateBase": AppTheme.cardElevated,
            "text": AppTheme.textPrimary,
            "button": AppTheme.cardElevated,
            "buttonText": AppTheme.textPrimary,
            "highlight": AppTheme.paletteHighlight,
            "highlightedText": AppTheme.paletteHighlightText,
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
        function onCustomOverridesChanged() { window.syncControlPalette() }
    }

    palette {
        window: AppTheme.background
        windowText: AppTheme.textPrimary
        base: AppTheme.inputBackground
        alternateBase: AppTheme.cardElevated
        text: AppTheme.textPrimary
        button: AppTheme.cardElevated
        buttonText: AppTheme.textPrimary
        highlight: AppTheme.paletteHighlight
        highlightedText: AppTheme.paletteHighlightText
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

    // The last item that held the keyboard focus while the main screen (not
    // Settings) was current: what closing Settings returns to (C74-F08).
    property Item lastMainFocus: null
    onActiveFocusItemChanged: {
        var f = activeFocusItem
        if (!f || app.currentScreen === 2)
            return
        for (var it = f; it; it = it.parent) {
            if (it === settingsViewLoader)
                return
        }
        lastMainFocus = f
    }
    Component.onCompleted: {
        syncControlPalette()
        // Place first, while still hidden, so the user never sees it move.
        applyStartupPlacement()
        visible = true
        // Maximized is applied here rather than bound to `visibility`, which
        // would fight the user after they un-maximize. It must lose to
        // startMinimized and startInTray, so it comes first.
        if (app.settings && app.settings.initialWindowMaximized)
            window.visibility = Window.Maximized
        if (app.settings && app.settings.startMinimized)
            window.visibility = Window.Minimized
        // Start in the tray only if a tray exists; SettingsManager::startInTray
        // also requires closeToTray, so the window can always be brought back.
        if (app.settings && app.settings.startInTray && app.trayAvailable)
            window.hide()
        geometrySaver.armed = true
    }

    // Saving geometry, debounced. Only the windowed state is recorded (the
    // maximized flag separately). `armed` keeps the startup restore from being
    // written back.
    Timer {
        id: geometrySaver
        property bool armed: false
        interval: 400
        onTriggered: window.flushGeometry()
    }
    function flushGeometry() {
        geometrySaver.stop()
        if (!app.settings || !window.visible)
            return
        if (window.visibility !== Window.Windowed)
            return
        app.settings.saveWindowGeometry(window.x, window.y,
                                        window.width, window.height)
    }
    function noteGeometryChanged() {
        if (geometrySaver.armed)
            geometrySaver.restart()
    }
    onXChanged: noteGeometryChanged()
    onYChanged: noteGeometryChanged()
    onWidthChanged: noteGeometryChanged()
    onHeightChanged: noteGeometryChanged()
    // The last visibility the window had on screen, restored by raiseIntoView()
    // after Minimized/Hidden. initialWindowMaximized is read once at load and
    // cannot answer this mid-session.
    property int lastOnScreenVisibility: Window.Windowed

    onVisibilityChanged: {
        if (window.visibility !== Window.Hidden)
            window.hiddenByClose = false
        if (window.visibility === Window.Maximized
                || window.visibility === Window.Windowed)
            window.lastOnScreenVisibility = window.visibility
        // Picture-in-picture follows the window off screen and back (see
        // syncAutomaticPip); first, so a return from the tray stands it down.
        window.syncAutomaticPip()
        if (!geometrySaver.armed || !app.settings)
            return
        // Minimized and Hidden say nothing about the state the user returns to,
        // so they are not recorded.
        if (window.visibility === Window.Maximized)
            app.settings.saveWindowMaximized(true)
        else if (window.visibility === Window.Windowed)
            app.settings.saveWindowMaximized(false)
    }

    // Bring the window forward without disturbing it. Never show(): it forces
    // the normal state, un-maximizing a maximized window and persisting that.
    // Hidden     — closed to the tray; `visible = true` restores its state.
    // Minimized  — restore the state it had before minimizing. on screen  —
    // raise and focus only; do not write visibility.
    function raiseIntoView() {
        if (window.visibility === Window.Hidden)
            window.visible = true
        else if (window.visibility === Window.Minimized)
            window.visibility = window.lastOnScreenVisibility
        window.raise()
        window.requestActivate()
    }

    // Close to tray: off by default and only where a tray exists; clicking the
    // tray icon restores the window. `quitRequested` lets a real quit through:
    // Qt asks every top-level window to close while quitting, and a refusal
    // aborts the quit.
    property bool quitRequested: false
    // The window hid itself on a close request (not at startup, not minimised):
    // the macOS Dock icon brings it back from here (GitHub #34).
    property bool hiddenByClose: false
    onClosing: (close) => {
        // Flush first: the 400 ms debounce may still be pending, and both
        // branches end this window's useful life.
        window.flushGeometry()
        // app.closeHidesWindow: close-to-tray where a tray exists; on macOS
        // every close, since Cmd+W is not Cmd+Q (see app/CloseBehavior.h).
        if (app.closeHidesWindow(window.quitRequested)) {
            close.accepted = false
            window.hiddenByClose = true
            window.hide()
        }
    }
    // Dock icon click (Qt reports it as a forced ApplicationActive) or Cmd+Tab
    // while the window is closed to the background.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive
                    && app.activationRestoresWindow(
                        window.hiddenByClose,
                        window.visibility === Window.Hidden))
                window.raiseIntoView()
        }
    }
    Connections {
        target: app
        // A deliberate quit from C++ (applying an update) must not be vetoed by
        // close-to-tray.
        function onApplicationQuitIntended() {
            window.quitRequested = true
        }
        function onTrayShowRequested() {
            // `visible = true`, never show(); see raiseIntoView().
            window.raiseIntoView()
        }
    }
    // Ctrl+Q quits for real, as does the tray icon's "Quit Lightning". It sets
    // quitRequested first so the close handler does not veto the quit.
    // Qt.quit() rather than Qt.exit() so aboutToQuit teardown (AppController,
    // UpdateManager apply-on-quit) still runs. ApplicationShortcut so it fires
    // while one of our native dialogs has focus.
    Shortcut {
        // From ShortcutRegistry. bindingRevision is read inside the binding
        // because sequenceFor() creates no dependency.
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("app.quit")]
        }
        context: Qt.ApplicationShortcut
        onActivated: {
            window.quitRequested = true
            Qt.quit()
        }
    }
    // Cmd+W closes the window, which on macOS hides it and leaves Lightning
    // running in the Dock (see onClosing and app/CloseBehavior.h). The app has
    // no native menu bar to carry a Close item, so without this only the red
    // button worked. Window-scoped, and macOS only: elsewhere Ctrl+W means
    // nothing here and a close quits or goes to the tray.
    Shortcut {
        objectName: "macCloseWindowShortcut"
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        enabled: Qt.platform.os === "osx"
        onActivated: window.close()
    }

    // Security, application-wide: every attached ToolTip.text is drawn as
    // plain text (SharedToolTipGuard.qml says why).
    SharedToolTipGuard {
        id: sharedToolTipGuard
    }

    // A clicked notification raises Lightning, selects the room, opens the
    // thread for a thread reply, and locates the event. The payload carries
    // identity only, never tokens.
    Connections {
        target: app
        function onNotificationOpenRequested(roomId, eventId, threadRootId) {
            window.raiseIntoView()
            app.activateMainWindowForNotificationClick()
            if (roomId === "")
                return
            app.showMain()
            app.currentRoomId = roomId
            var inThread = threadRootId && threadRootId.length > 0
            // The thread panel lands on the notified reply once the thread is
            // Ready (bounded thread pagination, highlighted), instead of
            // wherever the panel was last left.
            if (inThread)
                app.thread.openThreadAt(roomId, threadRootId, eventId || "")
            // A thread reply is not in the room timeline (Live with
            // hide_threaded_events), so jumping to it there can only fail; the
            // thread panel opened above is the destination. The thread root
            // does remain in the main timeline (CLAUDE.md §8) and can give
            // context, but must not paginate: jumpToEvent could walk the room
            // back through months of history. revealIfLoaded() takes it only
            // when already loaded.
            if (inThread) {
                // `inThread` is the non-empty test above.
                Qt.callLater(function() {
                    app.pagination.revealIfLoaded(threadRootId)
                })
            } else if (eventId && eventId.length > 0) {
                Qt.callLater(function() {
                    app.pagination.jumpToEvent(eventId)
                })
            }
        }
    }

    // Push the theme selection into the AppTheme singleton.
    Binding {
        target: AppTheme
        property: "mode"
        value: app.settings ? app.settings.theme : 0
    }
    // The platform light/dark preference drives the "System" theme.
    Binding {
        target: AppTheme
        property: "systemDark"
        value: app.systemDarkMode
    }
    // Reduced motion, pushed in from here like `mode` and customOverrides: the
    // AppTheme singleton may be created before the `app` context property
    // exists.
    Binding {
        target: AppTheme
        property: "reducedMotion"
        value: app.settings ? app.settings.reducedMotion : false
    }
    // Composer policy pushed into the C++ composer, which holds no
    // SettingsManager; this is the one place that owns both.
    Binding {
        target: app.composer
        property: "sendTextAsCaption"
        value: app.settings ? app.settings.sendTextAsCaption : false
    }
    Binding {
        target: app.composer
        property: "imageMetadataMode"
        value: app.settings ? app.settings.imageMetadataMode : "location"
    }
    // The thread panel's attachments follow the same policy.
    Binding {
        target: app.thread
        property: "imageMetadataMode"
        value: app.settings ? app.settings.imageMetadataMode : "location"
    }
    // Slash commands only ask for these; the mode is a setting and display-name
    // changes carry AppController's op-id bookkeeping. Wired here, where both
    // sides are owned.
    Connections {
        target: app.composer
        function onComposerModeToggleRequested() {
            if (!app.settings)
                return
            app.settings.composerMode =
                app.settings.composerMode === "rich" ? "markdown" : "rich"
        }
        function onDisplayNameChangeRequested(name) {
            app.submitOwnDisplayName(name)
        }
    }
    // The thread panel's composer issues the same two requests.
    Connections {
        target: app.thread
        function onComposerModeToggleRequested() {
            if (!app.settings)
                return
            app.settings.composerMode =
                app.settings.composerMode === "rich" ? "markdown" : "rich"
        }
        function onDisplayNameChangeRequested(name) {
            app.submitOwnDisplayName(name)
        }
    }
    // The custom palette (Settings → Appearance → Custom theme), pushed in the
    // same way as the theme id. CustomThemeStore has already dropped unknown
    // roles and malformed values.
    Binding {
        target: AppTheme
        property: "customOverrides"
        value: app.customTheme ? app.customTheme.colors : ({})
    }
    Binding {
        target: AppTheme
        property: "customBase"
        value: app.customTheme ? app.customTheme.baseTheme : 11
    }
    // The custom theme's surface gradients, sanitised by CustomThemeStore.
    Binding {
        target: AppTheme
        property: "customGradients"
        value: app.customTheme ? app.customTheme.gradients : ({})
    }
    // Settings → Appearance → Depth.
    Binding {
        target: AppTheme
        property: "surfaceDepth"
        value: app.backdrops ? app.backdrops.surfaceDepth : 0
    }
    // Content text scale (Settings → Appearance → Text size).
    Binding {
        target: AppTheme
        property: "textScale"
        value: app.settings ? app.settings.textScale / 100 : 1
    }
    // Settings > Appearance > Accessibility and message spacing.
    Binding {
        target: AppTheme
        property: "highContrast"
        value: app.settings ? app.settings.highContrast : false
    }
    Binding {
        target: AppTheme
        property: "underlineLinks"
        value: app.settings ? app.settings.underlineLinks : false
    }
    Binding {
        target: AppTheme
        property: "nameColorMinContrast"
        // Strong 4.5:1, Weak 3:1, Off 1 (no correction).
        value: !app.settings ? 4.5
               : app.settings.nameColorCorrection === 2 ? 1.0
               : app.settings.nameColorCorrection === 1 ? 3.0 : 4.5
    }
    Binding {
        target: AppTheme
        property: "messageSpacing"
        value: app.settings ? app.settings.messageSpacing : 1
    }
    // The UI font. Controls inherit through the window font; AppTheme.uiFont
    // covers non-inheriting text. Uses FontManager's resolved family, never a
    // family the host cannot draw; the stored choice is kept, so reinstalling a
    // missing font brings it back.
    Binding {
        target: AppTheme
        property: "uiFont"
        // `typeof`: `fonts` is a context property some suites do not install,
        // and referencing an unresolved name throws.
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.uiFamily : "Manrope"
    }
    // Same for the code/monospace face.
    Binding {
        target: AppTheme
        property: "monoFont"
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.monospaceFamily : "JetBrains Mono"
    }
    font.family: AppTheme.uiFont

    // No global header bar; the shell columns carry their own headers.

    // Space toggles whatever inline media is audible. A Keys handler on an
    // ancestor rather than a Shortcut: a Shortcut would take Space before the
    // focused item (timeline paging, emoji/GIF grids, composer, a settings
    // field), while bubbling keys only reach here if nothing else wanted them.
    // On the chat screen and in Settings alike (design-media B2: a clip kept
    // playing in Settings with nothing able to pause it).
    function toggleAudibleMedia(event) {
        // Held Space would toggle on every auto-repeat.
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

    Loader {
        id: pageLoader
        anchors.fill: parent
        // Above the docked player strip when it runs along the bottom.
        anchors.bottomMargin: voiceDockFallback.visible
                              ? voiceDockFallback.height : 0

        Keys.onSpacePressed: (event) => window.toggleAudibleMedia(event)
        // Hidden, not unloaded, under full-view Settings so chat state
        // survives.
        visible: app.currentScreen !== 2
        enabled: visible

        // AppController::Screen values, kept in sync with
        // src/app/AppController.h. Integer literals rather than
        // `app.LoginScreen`: case expressions reading enum values off a
        // context-property object fell through under some Qt Quick compiler
        // configurations.
        function pickComponent() {
            var s = app.currentScreen
            // Settings (2) keeps MainScreen loaded but hidden, so the selected
            // room, timeline position and drafts survive.
            if (s === 1 || s === 2) return mainComponent
            // 3 = BootScreen: a saved session is restoring; the login form is
            // never created on that path.
            if (s === 3) return bootComponent
            return loginComponent                  // 0 = LoginScreen
        }
        sourceComponent: pickComponent()

        // Re-evaluate on the explicit signal as a fallback; a no-op when the
        // binding tracks the property.
        Connections {
            target: app
            function onCurrentScreenChanged() {
                pageLoader.sourceComponent = pageLoader.pickComponent()
            }
        }
    }

    Component { id: loginComponent;    LoginScreen {} }
    Component { id: mainComponent;     MainScreen { promptRegion: verifySessionPromptCard } }
    // Minimal restoration surface: background, wordmark, one spinner. No
    // credential fields or stale content; the saved theme is already resolved.
    Component {
        id: bootComponent
        Rectangle {
            objectName: "startupRestoreSurface"
            color: AppTheme.background
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingM
                // Brand mark, same bolt-in-tile idiom as the room-list header.
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

    // Full-view Settings, covering the whole content area. Built once and kept:
    // instantiating SettingsScreen on every open blocked the GUI thread for
    // hundreds of milliseconds. The first build starts asynchronously shortly
    // after the main screen shows; an earlier open builds synchronously.
    Loader {
        id: settingsViewLoader
        objectName: "settingsViewLoader"
        anchors.fill: parent
        anchors.bottomMargin: voiceDockFallback.visible
                              ? voiceDockFallback.height : 0
        Keys.onSpacePressed: (event) => window.toggleAudibleMedia(event)
        property bool warm: false
        active: warm || app.currentScreen === 2
        visible: app.currentScreen === 2
        asynchronous: !visible
        z: 5
        sourceComponent: SettingsScreen {}
        onLoaded: {
            warm = true
            if (visible)
                item.forceActiveFocus()
        }
        // Closing Settings (Escape) hands the focus back instead of leaving it
        // on an item that has just been hidden (C74-F08). The item to return to
        // is tracked WHILE the main screen is showing (window.lastMainFocus,
        // below), not read when Settings becomes visible: by then the shell is
        // hiding in the same binding pass and activeFocusItem may already be
        // null.
        property Item returnFocusTo: null
        property bool settingsWasShown: false
        function restoreFocusAfterClose() {
            var target = returnFocusTo
            returnFocusTo = null
            if (visible)
                return
            if (target && target.visible && target.enabled
                    && target.Window.window === window) {
                target.forceActiveFocus(Qt.OtherFocusReason)
                return
            }
            if (pageLoader.item)
                pageLoader.item.forceActiveFocus(Qt.OtherFocusReason)
            else
                pageLoader.forceActiveFocus(Qt.OtherFocusReason)
        }
        onVisibleChanged: {
            if (visible && item) {
                returnFocusTo = window.lastMainFocus
                settingsWasShown = true
                item.forceActiveFocus()
            } else if (!visible && settingsWasShown) {
                // After the page loader has shown again. A drop that follows
                // no show (the loader never displayed Settings) restores nothing.
                settingsWasShown = false
                Qt.callLater(settingsViewLoader.restoreFocusAfterClose)
            }
        }
    }
    Timer {
        interval: 2500
        running: app.currentScreen === 1 && !settingsViewLoader.warm
        onTriggered: settingsViewLoader.warm = true
    }

    // ── The voice/audio player, at window level so it outlives every screen
    // switch (design-media B2: it lived in TimelinePane, which Settings hides,
    // so a clip played on in Settings with nothing showing or stopping it).
    //
    // Docked (the default, app.settings.voicePlayerDocked): the strip lives in
    // TimelinePane under the room header. Where no room column shows it
    // (Settings; a thread taking the whole pane), this one runs along the
    // bottom instead and the screens above stop short of it, so it never
    // covers anything.
    // The chat pane, found once and kept (MainScreen names it).
    property Item voicePane: null
    function findVoicePane() {
        var queue = pageLoader.item ? [pageLoader.item] : []
        for (var depth = 0; depth < 8 && queue.length > 0; ++depth) {
            var next = []
            for (var i = 0; i < queue.length; ++i) {
                var it = queue[i]
                if (it.objectName === "timelinePane")
                    return it
                for (var j = 0; j < it.children.length; ++j)
                    next.push(it.children[j])
            }
            queue = next
        }
        return null
    }
    function refreshVoicePane() {
        if (!window.voicePane && app.voicePlayback.active)
            window.voicePane = window.findVoicePane()
    }
    Connections {
        target: app.voicePlayback
        function onCurrentChanged() { window.refreshVoicePane() }
    }
    Connections {
        target: pageLoader
        function onLoaded() {
            window.voicePane = null
            window.refreshVoicePane()
        }
    }
    VoicePlayerBar {
        id: voiceDockFallback
        objectName: "voicePlayerDockFallback"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        z: 6
        atBottom: true
        shown: app.voicePlayback.active && app.settings.voicePlayerDocked
               && (app.currentScreen === 2
                   || (app.currentScreen === 1
                       && !(window.voicePane
                            && window.voicePane.voiceDockShown)))
    }
    // Keyboard access to the player from anywhere (design-media U5: Tab
    // never reached it from the timeline, and the composer keeps Tab): the
    // rebindable `media.focusPlayer` (ShortcutRegistry). Focus lands on a
    // control where Space plays or pauses (the strip's or the row's
    // play/pause button, or the card, which takes Space itself); Tab then
    // walks the player's controls.
    function focusVoicePlayer() {
        if (voiceMiniPlayer.wanted) {
            voiceMiniPlayer.focusCard()
            return
        }
        if (voiceDockFallback.visible) {
            voiceDockFallback.focusPlayer()
            return
        }
        window.refreshVoicePane()
        if (window.voicePane && window.voicePane.voiceDockShown) {
            window.voicePane.focusVoiceDock()
            return
        }
        // Popped out with the clip's row on screen: that row is the player.
        var row = app.voicePlayback.visibleRow
        if (row && row.focusPlayButton)
            row.focusPlayButton()
    }
    Shortcut {
        objectName: "focusVoicePlayerShortcut"
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("media.focusPlayer")]
        }
        enabled: app.voicePlayback.active
                 && (app.currentScreen === 1 || app.currentScreen === 2)
        onActivated: window.focusVoicePlayer()
    }
    // Popped out: the floating card, over everything but popups.
    VoiceMiniPlayer {
        id: voiceMiniPlayer
        anchors.fill: parent
        z: 40
        // The chat column, for the card's first spot.
        hintProvider: function() {
            var pane = window.voicePane
            if (!pane || !pane.visible || !pane.chatSafeArea)
                return null
            var r = pane.chatSafeArea
            var p = pane.mapToItem(voiceMiniPlayer, r.x, r.y)
            return Qt.rect(p.x, p.y, r.width, r.height)
        }
    }

    // Development-only screenshot-demo control panel, an overlay child of the
    // window (no layout gap). Never loaded in a normal build
    // (app.screenshotDemoActive is false). Ctrl+Shift+D toggles it.
    Shortcut {
        sequence: "Ctrl+Shift+D"
        enabled: app.screenshotDemoActive
        onActivated: if (app.demo) app.demo.toggleControls()
    }

    // Interface zoom. Applied as QT_SCALE_FACTOR, which Qt reads once at
    // startup, so it takes effect on the next launch and the notice says so.
    function _adjustZoom(delta) {
        var next = delta === 0 ? 100 : app.settings.interfaceZoom + delta
        app.settings.interfaceZoom = next
        zoomNotice.show()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            // Ctrl++ stays a hard-coded alternate: keyboards differ on whether
            // Ctrl+= or Ctrl++ is reachable, and the registry stores one
            // sequence per action.
            return [app.shortcuts.sequenceFor("view.zoomIn"), "Ctrl++"]
        }
        onActivated: window._adjustZoom(5)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("view.zoomOut")]
        }
        onActivated: window._adjustZoom(-5)
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("view.zoomReset")]
        }
        onActivated: window._adjustZoom(0)
    }

    Rectangle {
        id: zoomNotice
        function show() { visible = true; zoomNoticeTimer.restart() }
        visible: false
        // In the overlay so it shows above open popups.
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
    // One-time notice (per version) when the GL driver is a CPU rasteriser
    // such as llvmpipe, e.g. the AppImage under appimage-run on NixOS. Not
    // shown when the user asked for software rendering. See
    // AppController::setGlRenderer. The explanation follows
    // app.softwareRendererNoticeContext: NixOS is named only on NixOS and the
    // AppImage only for an AppImage (it said "the AppImage on NixOS" on every
    // package, a .deb in a VM included).
    Rectangle {
        id: softwareRendererNotice
        objectName: "softwareRendererNotice"
        visible: !!app && app.softwareRendererNoticeVisible === true
        readonly property string noticeContext:
            !!app && typeof app.softwareRendererNoticeContext === "string"
                ? app.softwareRendererNoticeContext : "generic"
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: AppTheme.spacing16
        z: 1000
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        width: Math.min(parent.width - AppTheme.spacing16 * 2, 560)
        height: noticeColumn.implicitHeight + AppTheme.spacing16 * 2
        ColumnLayout {
            id: noticeColumn
            anchors.fill: parent
            anchors.margins: AppTheme.spacing16
            spacing: AppTheme.spacing8
            Label {
                objectName: "softwareRendererNoticeText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: {
                    switch (softwareRendererNotice.noticeContext) {
                    case "appimage-nixos":
                        return qsTr("Lightning is drawing on the CPU, not your graphics card, so scrolling may be slow. This is common with the AppImage on NixOS.")
                    case "appimage":
                        return qsTr("Lightning is drawing on the CPU, not your graphics card, so scrolling may be slow. This can happen when the AppImage cannot use your system's graphics driver, or in a virtual machine.")
                    case "nixos":
                        return qsTr("Lightning is drawing on the CPU, not your graphics card, so scrolling may be slow. On NixOS this usually means Lightning cannot reach the system's graphics driver.")
                    default:
                        return qsTr("Lightning is drawing on the CPU, not your graphics card, so scrolling may be slow. This usually means no graphics driver is available to it, for example in a virtual machine or a remote desktop session.")
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                AppButton {
                    storm: true
                    objectName: "softwareRendererNoticeHelpButton"
                    // The page it opens is the NixOS AppImage fix.
                    visible: softwareRendererNotice.noticeContext === "appimage-nixos"
                    text: qsTr("How to fix")
                    onClicked: {
                        app.media.openWebUrl("https://github.com/Mizerd/lightning/blob/main/docs/install.md#nixos-appimage-and-gpu-acceleration")
                    }
                }
                AppButton {
                    storm: true
                    kind: "primary"
                    objectName: "softwareRendererNoticeDismissButton"
                    text: qsTr("Dismiss")
                    onClicked: app.dismissSoftwareRendererNotice()
                }
            }
        }
    }
    // Transient notice for failures of actions taken from menus that have
    // already closed (pin/unpin and others below). Success is usually silent.
    // Overlay-parented like zoomNotice.
    Rectangle {
        id: pinNotice
        objectName: "pinActionNotice"
        property string message: ""
        // Whether this reports a failure. Defaults to true; non-failure callers
        // pass false.
        property bool danger: true
        function show(text, isDanger) {
            message = text
            danger = (isDanger === undefined) ? true : !!isDanger
            visible = true
            pinNoticeTimer.restart()
        }
        visible: false
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: AppTheme.spacing16
        z: 1000
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: pinNotice.danger ? AppTheme.stormDanger
                                       : AppTheme.stormBorder
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
        // "Remove edits": the menu is closed by the time the server answers.
        // Removing nothing is reported as such, not as success.
        Connections {
            target: app.composer
            function onEditsRemoved(eventId, ok, removed, failed, truncated) {
                // ok == false with nothing attempted means the edits could not
                // be read at all; "there are none" would be false.
                if (!ok && removed === 0 && failed === 0) {
                    // One literal: lupdate cannot extract a concatenation.
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
        // Report outcome. The report dialog closes before the server answers,
        // so report both outcomes here (a silent success looks like a dead menu
        // item). The controller supplies the translated sentences.
        Connections {
            target: app.moderation
            function onReportFinished(ok, message) {
                if (message.length > 0)
                    pinNotice.show(message, !ok)
            }
        }
        // "Add to my stickers" outcome, reported here since the menu has
        // closed. Success is reported (the pack is only visible in the picker),
        // naming the shortcode, which may have a numeric suffix.
        Connections {
            target: app.stickers
            function onSaveFinished(ok, category, shortcode, scope) {
                // Distinguish this account's pack from the room's.
                var toRoom = scope === "room"
                if (ok) {
                    if (toRoom) {
                        pinNotice.show(
                            shortcode.length > 0
                                ? qsTr("Added to this room's stickers as :%1:")
                                    .arg(shortcode)
                                : qsTr("Added to this room's stickers"), false)
                    } else {
                        pinNotice.show(
                            shortcode.length > 0
                                ? qsTr("Saved to your stickers as :%1:")
                                    .arg(shortcode)
                                : qsTr("Saved to your stickers"), false)
                    }
                } else if (category === "duplicate") {
                    // Not an error: the sticker is already in the pack.
                    pinNotice.show(
                        toRoom
                            ? qsTr("That sticker is already in this room's stickers")
                            : qsTr("That sticker is already in your stickers"),
                        false)
                } else if (category === "pack_full") {
                    pinNotice.show(
                        toRoom
                            ? qsTr("This room's sticker pack is full.")
                            : qsTr("Your sticker pack is full. Remove some "
                                   + "stickers in another client to add more."),
                        true)
                } else if (category === "forbidden") {
                    // Only the room write can be refused this way (room state,
                    // power-level gated).
                    pinNotice.show(
                        qsTr("You do not have permission to change this "
                             + "room's stickers."), true)
                } else if (category.indexOf("svg_") === 0) {
                    // An SVG that could not be converted: the reason is the
                    // news, not "could not be saved".
                    pinNotice.show(
                        app.stickers.svgMessage(category.substring(4)), true)
                } else {
                    pinNotice.show(
                        qsTr("The sticker could not be saved."), true)
                }
            }
        }
        // "Add custom emoji to my pack" from a message's menu: the menu has
        // closed, so the single report is a notice. Export and import report
        // where they were started (the pack editor, Settings).
        Connections {
            target: app.stickers
            function onTransferFinished(kind, ok, category, added, skipped,
                                        failed, name) {
                if (kind !== "emoji")
                    return
                if (ok && added === 1 && skipped === 0 && failed === 0
                        && name.length > 0) {
                    pinNotice.show(qsTr("Added :%1: to your emoji").arg(name),
                                   false)
                    return
                }
                pinNotice.show(app.stickers.transferMessage(
                    kind, ok, category, added, skipped, failed, name), !ok)
            }
        }
        // A forwarded attachment is a direct upload with no local echo in the
        // target room, so report a failure here.
        Connections {
            target: app.forward
            function onForwardFailed(targetRoomId, message) {
                if (message.length === 0)
                    return
                // Only while the user is still looking at the target room.
                if (targetRoomId.length > 0
                        && targetRoomId !== app.currentRoomId)
                    return
                pinNotice.show(message)
            }
        }
    }

    // One verification dialog for the whole app, following AppController's
    // state, so Settings, the corner prompt and incoming requests share it.
    VerificationDialog {
        id: verificationDialog
        objectName: "verificationDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    // The one UIA prompt, opened by UiaController's challenge state.
    UiaPromptDialog {
        id: uiaPromptDialog
        parent: Overlay.overlay
    }

    // The one report-message prompt (ModerationController's pending report).
    ReportMessageDialog {
        id: reportMessageDialog
        parent: Overlay.overlay
    }

    // The one forward room-picker (ForwardController's `active` state).
    ForwardMessageDialog {
        id: forwardMessageDialog
        parent: Overlay.overlay
    }

    // The one update-available prompt, opened by UpdateManager's state.
    UpdateAvailableDialog {
        id: updateAvailableDialog
        parent: Overlay.overlay
    }

    // Lightning's own file chooser, for Linux without a portal FileChooser:
    // FileChooser (app.files) asks it, and every NativeFileDialog's answer
    // comes back through it there. Its z keeps it above the dialog it was
    // opened from.
    FileChooserDialog {
        id: fileChooserDialog
    }

    // The composer's real top edge lifts the corner cards clear of the
    // send/emoji/GIF/mic controls; a guessed constant was wrong for a
    // multi-line composer, a reply strip or UI scale. The search covers the
    // page only (never the timeline delegates' whole tree on every tick), a
    // MISS is remembered until the room or screen changes, and a cached item
    // is dropped as soon as it is null, parentless or reads invisible-undefined
    // (a destroyed wrapper is not reliably null).
    property var composerProbe: null
    property string composerMissKey: ""
    function findByObjectName(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        var kids = item.children
        for (var i = 0; i < kids.length; ++i) {
            var hit = findByObjectName(kids[i], name)
            if (hit)
                return hit
        }
        return null
    }
    function composerProbeAlive() {
        var c = composerProbe
        return !!c && c.parent !== null && c.parent !== undefined
               && c.visible !== undefined
    }
    /// Pixels from the window bottom up to the composer's top edge, 0 when no
    /// composer is on screen.
    property real composerClearance: 0
    function refreshComposerClearance() {
        var next = 0
        if (app.currentScreen === 1 && app.currentRoomId !== "") {
            var key = app.currentScreen + "|" + app.currentRoomId
            if (!composerProbeAlive()) {
                composerProbe = null
                if (composerMissKey !== key) {
                    composerProbe = findByObjectName(pageLoader.item,
                                                     "composerCard")
                    if (!composerProbe)
                        composerMissKey = key
                }
            }
            var c = composerProbe
            if (c && c.visible && c.height > 0) {
                var top = c.mapToItem(window.contentItem, 0, 0).y
                next = Math.max(0, window.contentItem.height - top)
            }
        } else {
            composerProbe = null
            composerMissKey = ""
        }
        if (Math.abs(next - composerClearance) >= 1)
            composerClearance = next
    }
    Timer {
        // Runs only while a card is showing; the composer moves when it grows.
        interval: 250
        repeat: true
        running: cornerPassiveHost.height > 0 || incomingCallHost.height > 0
        triggeredOnStart: true
        onTriggered: window.refreshComposerClearance()
    }

    // VoiceMiniPlayer and the PiP avoid the object named "cornerPromptHost".
    // That is this invisible proxy spanning BOTH real hosts below, so the
    // ring is avoided as well as the passive cards without touching them.
    Item {
        objectName: "cornerPromptHost"
        parent: Overlay.overlay
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: AppTheme.spacing16
        anchors.bottomMargin: AppTheme.spacing16 + window.composerClearance
        width: Math.max(incomingCallHost.width, cornerPassiveHost.width)
        height: cornerPassiveHost.height + incomingCallHost.height
                + (cornerPassiveHost.height > 0 && incomingCallHost.height > 0
                   ? AppTheme.spacing8 : 0)
        visible: height > 0
        enabled: false
        z: -2
    }

    // A live ring has its own host ABOVE every popup: it is time-critical and
    // must stay answerable while a modal dialog is open.
    Column {
        id: incomingCallHost
        objectName: "incomingCallHost"
        parent: Overlay.overlay
        anchors.right: parent.right
        anchors.bottom: cornerPassiveHost.top
        anchors.rightMargin: AppTheme.spacing16
        anchors.bottomMargin: cornerPassiveHost.height > 0 ? AppTheme.spacing8 : 0
        z: 900

        IncomingCallPrompt {
            objectName: "incomingCallPromptHost"
        }
    }

    // Passive corner prompts share one bottom-right column so they never
    // overlap; hidden prompts reclaim their space. The verification nudge is
    // a card, not a modal: an unverified session still works.
    Column {
        id: cornerPassiveHost
        objectName: "cornerPassiveHost"
        parent: Overlay.overlay
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: AppTheme.spacing16
        anchors.bottomMargin: AppTheme.spacing16 + window.composerClearance
        spacing: AppTheme.spacing8
        // Below every popup (default z 0), so a modal dialog's scrim covers
        // the cards and blocks clicks on them.
        z: -1

        UpdateAvailablePrompt {
            objectName: "updateAvailablePromptHost"
            onDetailsRequested: updateAvailableDialog.open()
        }
        VerifySessionPrompt {
            id: verifySessionPromptCard
            objectName: "verifySessionPromptHost"
        }
        // The one-time "index all messages now?" offer. It waits while a
        // prompt about this session's keys is showing, so it never competes
        // with verification or recovery.
        IndexAllPrompt {
            objectName: "indexAllPromptHost"
            blocked: verifySessionPromptCard.shouldShow
                     || encryptionBrokenPromptCard.shouldShow
        }
        // Personal chat backgrounds on the homeserver: the one-time notice
        // before earlier pictures are uploaded, and "two different
        // backgrounds" when this device and the server disagree.
        BackgroundSyncPrompt {
            objectName: "backgroundSyncPromptHost"
            blocked: verifySessionPromptCard.shouldShow
                     || encryptionBrokenPromptCard.shouldShow
        }
        // Nearest the corner: a session whose published identity key does not
        // match its account cannot decrypt anything it receives.
        EncryptionBrokenPrompt {
            id: encryptionBrokenPromptCard
            objectName: "encryptionBrokenPromptHost"
        }
    }

    // Picture-in-picture: a separate top-level Window, so it keeps carrying the
    // call while this one is minimised or in the tray. Visibility follows
    // CallStageState's flag (see CallPipWindow); dropped when the call ends.
    CallPipWindow {
        objectName: "callPipWindow"
        onRestoreRequested: {
            // Stand the PiP down before raising: the video router gives a
            // track's sink to the last attach, so the departing surface must
            // release first.
            if (app.groupCall && app.groupCall.stageState)
                app.groupCall.stageState.setPictureInPicture(false)
            window.raiseIntoView()
        }
    }
    // Automatic pop-out: only when this window is minimised or in the tray with
    // a live call, and stood down on return. It never opens over a visible
    // window, and only closes a PiP it opened itself.
    property bool pipAutoOpened: false
    readonly property bool pipCallLive:
        (app.groupCall && app.groupCall.active)
        || (app.calls && (app.calls.state === CallController.Active
                          || app.calls.state === CallController.Connecting))
    readonly property bool windowAwayFromView:
        window.visibility === Window.Hidden
        || window.visibility === Window.Minimized
    function syncAutomaticPip() {
        if (!app.settings || !app.settings.callPictureInPicture)
            return
        var state = app.groupCall ? app.groupCall.stageState : null
        if (!state)
            return
        if (window.pipCallLive && window.windowAwayFromView
                && !state.pictureInPicture) {
            state.setPictureInPicture(true)
            window.pipAutoOpened = true
        } else if (window.pipAutoOpened
                   && (!window.windowAwayFromView || !window.pipCallLive)) {
            state.setPictureInPicture(false)
            window.pipAutoOpened = false
        }
    }
    // Only one onVisibilityChanged handler is allowed, so this hangs off the
    // existing one.
    onPipCallLiveChanged: window.syncAutomaticPip()

    Loader {
        active: app.screenshotDemoActive
        anchors.fill: parent
        z: 100
        // A string source resolved at runtime, so non-demo builds never
        // reference the component.
        source: app.screenshotDemoActive ? "DemoControlPanel.qml" : ""
    }

    // Status strip, shown only while something needs attention: an error, or
    // a signed-in session that is not connected. Not on the sign-in page,
    // where "Not connected" is simply true before anyone has signed in and
    // the form says what it is doing. The development backends (mock, HTTP)
    // keep it everywhere, since it is the only place that names them.
    footer: Rectangle {
        color: AppTheme.surface
        readonly property bool developmentBackend: app.backendName !== "rust"
        // Hidden in the screenshot demo (mock backend).
        visible: !app.screenshotDemoActive
                 && (statusBar.lastError !== ""
                     || developmentBackend
                     || (app.loggedIn
                         && app.connectionStatus !== qsTr("Connected")))
        implicitHeight: visible
                        ? statusRow.implicitHeight + AppTheme.spacingS * 2 : 0
        RowLayout {
            id: statusRow
            anchors.fill: parent
            anchors.margins: AppTheme.spacingS
            spacing: AppTheme.spacingM
            // Status dot plus backend label.
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
                // The real backend is not news to anyone using it; only the
                // development backends are named.
                text: {
                    if (app.backendName === "rust")
                        return app.connectionStatus
                    var label = app.backendName === "mock" ? qsTr("Mock backend")
                                                           : qsTr("HTTP backend")
                    return qsTr("%1 • %2").arg(label).arg(app.connectionStatus)
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: statusBar.lastError !== ""
                text: statusBar.lastError
                // Some messages carry server text.
                textFormat: Text.PlainText
                color: AppTheme.error
                font.pixelSize: 12
            }
            // The message also expires on its own and goes when the user
            // moves to another room (AppController::clearStatusError).
            IconButton {
                objectName: "statusErrorDismiss"
                visible: statusBar.lastError !== ""
                Layout.alignment: Qt.AlignVCenter
                size: "sm"
                iconName: "close"
                Accessible.name: qsTr("Dismiss")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Dismiss")
                onClicked: app.dismissStatusError()
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
