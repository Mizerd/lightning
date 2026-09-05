import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

ApplicationWindow {
    id: window

    // ── Window geometry ──────────────────────────────────────────────────
    // Restored from the last run, and saved as the user moves and resizes.
    // Both halves used to be missing: the window opened at 1100x720 wherever
    // the platform felt like putting it, every launch.
    //
    // The restore is DECLARATIVE, in these four bindings, and that is not a
    // style choice. Qt shows the window during the component's own
    // componentComplete(), which runs BEFORE any Component.onCompleted — so
    // geometry applied from a completion handler lands after the window is
    // already on screen and the user watches it jump.
    //
    // AppController::restorableWindowGeometry is CONSTANT, so these bindings
    // evaluate once. Qt overwrites x/y/width/height directly when the user
    // drags the frame, which breaks each binding, which is exactly right: the
    // stored value is a starting point, not a leash. Nothing here reads a
    // notifying property, so saving cannot feed back into restoring.
    //
    // It comes from AppController rather than straight from the settings
    // because it is already filtered for a display layout that may have
    // changed since — an invalid rect here means "do not restore", never
    // "nothing was stored".
    readonly property rect startupGeometry: app.restorableWindowGeometry
    readonly property bool hasStartupGeometry: startupGeometry.width > 0
                                               && startupGeometry.height > 0
    width: hasStartupGeometry ? startupGeometry.width : 1100
    height: hasStartupGeometry ? startupGeometry.height : 720
    minimumWidth: 640
    minimumHeight: 420

    // THE POSITION IS APPLIED ONCE AND THEN LET GO OF.
    //
    // x and y used to be BINDINGS, and on the fresh-launch branch they read
    // Screen.desktopAvailableWidth/Height — which notify — and `width`, which
    // changes whenever the user resizes. So the window re-centred itself
    // under its own user: drag it toward another monitor and the Screen
    // attached property changes, the binding fires, and the window is pulled
    // back to the middle of wherever it now thinks it is. That is the
    // reported "trying to move the window makes it fight the movement", and
    // it is also how a window can end up half off screen — the metrics are
    // not final when an ApplicationWindow is first created, particularly on
    // macOS, so the first centring runs against numbers that are about to
    // change.
    //
    // The comment that used to sit here claimed these bindings evaluate once
    // because restorableWindowGeometry is constant. That is true of the
    // RESTORE branch and false of the centring branch beside it.
    //
    // So: start hidden, compute once, assign imperatively (which breaks the
    // binding for good), then show. Hidden-until-placed also removes the jump
    // the previous round hit when it tried to do this from
    // Component.onCompleted while the window was already visible — Qt shows
    // during componentComplete(), so the user watched it move. Nothing
    // watches it move if it was never on screen.
    visible: false

    function applyStartupPlacement() {
        if (hasStartupGeometry) {
            // Already validated in C++ against the CURRENT display layout: a
            // rect whose grab band no longer meets any screen was dropped
            // there, so reaching here means it is usable, including the
            // legitimate negative coordinates of a monitor left of or above
            // the primary.
            x = startupGeometry.x
            y = startupGeometry.y
            app.noteWindowPlacement("restored", x, y, width, height)
            return
        }
        // Centred on ONE SCREEN's available area, computed and validated in
        // C++ (AppController::centredWindowRect, which carries the argument
        // and the measurement).
        //
        // This used to be arithmetic on `Screen.desktopAvailableWidth` — the
        // width of the whole VIRTUAL DESKTOP — so with two monitors it aimed
        // at the middle of the pair, and on the maintainer's layout landed
        // 90px past the desktop's right edge: the window opened invisible.
        // An empty rect means the answer could not be trusted, and then the
        // window manager places the window, which it does well.
        var centred = app.centredWindowRect(width, height)
        if (centred.width > 0 && centred.height > 0) {
            x = centred.x
            y = centred.y
            app.noteWindowPlacement("centred", x, y, width, height)
            return
        }
        // No usable metrics — a headless or still-settling display. Leave the
        // platform's own placement rather than centring against zeroes, which
        // is what puts a window in the top-left corner or half off screen.
        app.noteWindowPlacement("platform-default", x, y, width, height)
    }

    // Development screenshot mode gets a clear window-title suffix so a demo
    // window is never mistaken for a real account. It drops automatically when
    // the demo controls are hidden, for a fully clean final screenshot.
    // THE TITLE CARRIES THE UNREAD COUNT, because the taskbar is the one
    // surface visible while Lightning is not. Reported: messages arrive and
    // go unnoticed until the same account is opened in another client.
    //
    // Rooms, not messages, and mentions called out separately — "2 unread,
    // 1 mention" is something a person acts on, where a summed message
    // count is a number nobody can do anything with.
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

    // ── Right-to-left ────────────────────────────────────────────────────
    // LocalizationManager sets the process-wide layout direction when the
    // language changes; Qt turns that into text alignment on its own, but
    // ANCHORS and Layouts only mirror where LayoutMirroring says so. Enabling
    // it here with childrenInherit mirrors the whole shell from one place:
    // the rail moves to the right, the room list follows it, panels swap
    // sides, and every leftMargin behaves as a leading margin.
    //
    // What this does NOT do, so nobody goes looking for it: LayoutMirroring
    // resolves anchors and layout order. It never mirrors PIXELS, so images,
    // video and avatars are untouched by construction. The timeline's
    // `rotation: 180` Flickable is likewise unaffected — its scroll axis is
    // vertical and mirroring is horizontal — so the timeline mirrors like any
    // other pane and the scroll machinery is not in the blast radius.
    //
    // The one deliberate opt-out is CodeBlock: source code reads
    // left-to-right in every language.
    LayoutMirroring.enabled: app.localization.rightToLeft
    LayoutMirroring.childrenInherit: true

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
        // Placement FIRST, and while the window is still hidden: everything
        // below decides how to SHOW it, and a window that is shown before it
        // has been positioned is one the user watches jump.
        applyStartupPlacement()
        visible = true
        // Maximized is applied here rather than in a `visibility` binding: a
        // binding would be broken the first time the user un-maximizes, and
        // then re-established by the save below, fighting them. It also has
        // to lose to startMinimized and startInTray, which is why it is
        // first.
        if (app.settings && app.settings.initialWindowMaximized)
            window.visibility = Window.Maximized
        if (app.settings && app.settings.startMinimized)
            window.visibility = Window.Minimized
        // Start straight into the tray. Guarded on the tray actually
        // existing, and SettingsManager::startInTray already refuses unless
        // closeToTray is on — starting invisibly with no way back would be
        // the worst possible failure of this feature.
        if (app.settings && app.settings.startInTray && app.trayAvailable)
            window.hide()
        geometrySaver.armed = true
    }

    // ── Saving the geometry back ──────────────────────────────────────────
    // Debounced: a drag reports every pixel, and one QSettings write per
    // mouse move is not a thing to do.
    //
    // Only the WINDOWED state is recorded. A maximized window's frame is the
    // screen and a minimized one has no useful frame at all, so storing
    // either as "the size the user chose" would throw that size away. The
    // maximized FLAG is stored separately, which is how both survive.
    //
    // `armed` keeps startup out of it: applying the restore above changes x,
    // y, width and height, and saving those back would be writing our own
    // input, plus one write per launch for nothing.
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
    // The last visibility the window actually had ON SCREEN. Minimized and
    // Hidden are transient states the user comes BACK from, so raiseIntoView()
    // restores this rather than guessing Windowed. `initialWindowMaximized` is
    // CONSTANT in SettingsManager (read once at load), so it cannot answer this
    // question after the user has maximized or restored during the session.
    property int lastOnScreenVisibility: Window.Windowed

    onVisibilityChanged: {
        if (window.visibility === Window.Maximized
                || window.visibility === Window.Windowed)
            window.lastOnScreenVisibility = window.visibility
        // Picture-in-picture follows the window off screen and back (see
        // syncAutomaticPip below). Placed FIRST so a return from the tray
        // stands the floating window down before anything else runs.
        window.syncAutomaticPip()
        if (!geometrySaver.armed || !app.settings)
            return
        // Minimized and Hidden say nothing about which of maximized or
        // windowed the user will come back to, so they are not recorded —
        // otherwise closing to tray from a maximized window would remember
        // "not maximized".
        if (window.visibility === Window.Maximized)
            app.settings.saveWindowMaximized(true)
        else if (window.visibility === Window.Windowed)
            app.settings.saveWindowMaximized(false)
    }

    // ── Bringing the window forward without disturbing it ────────────────
    //
    // NEVER show(). QWindow::show() forces the NORMAL state, so calling it on
    // a window that is already on screen and MAXIMIZED un-maximizes it — and
    // onVisibilityChanged above then persists saveWindowMaximized(false), so
    // the user's window preference is silently rewritten as a side effect of
    // a click. Reported by a tester as "clicking a notification in the bell
    // menu minimizes Lightning": the maximized window snapping back to its
    // small remembered frame is what that looks like.
    //
    // The three states are genuinely different and only two of them may touch
    // visibility at all:
    //   Hidden     — closed to the tray; `visible = true` brings it back in
    //                the state it had, which is why the tray path uses that
    //                and not show().
    //   Minimized  — a real desktop-notification click; restore the state the
    //                window was in before it was minimized.
    //   on screen  — raise and focus ONLY. Do not write visibility.
    function raiseIntoView() {
        if (window.visibility === Window.Hidden)
            window.visible = true
        else if (window.visibility === Window.Minimized)
            window.visibility = window.lastOnScreenVisibility
        window.raise()
        window.requestActivate()
    }

    // ── Close to tray ────────────────────────────────────────────────────
    // Requested by a tester on Windows. Off by default, and gated on the
    // platform having a tray at all: hiding the window into a tray that does
    // not exist would leave no way to get it back. Clicking the tray icon —
    // any button — restores it.
    //
    // `quitRequested` is what makes Ctrl+Q work at all while this is on, and
    // it is not a convenience flag — see the Shortcut below. Qt asks every
    // top-level window to close as part of quitting, and a window that
    // REFUSES stops the quit; without this the close-to-tray branch answered
    // that request too, so Ctrl+Q silently hid the window instead of exiting.
    property bool quitRequested: false
    onClosing: (close) => {
        // Flush first, and unconditionally: the 400 ms debounce may still be
        // running, and both branches below end this window's useful life —
        // quitting outright, or hiding it, after which `visible` is false and
        // the timer would decline to save. Losing the last resize because the
        // user closed promptly after it is exactly the reported symptom.
        window.flushGeometry()
        if (!window.quitRequested && app.settings
                && app.settings.closeToTray && app.trayAvailable) {
            close.accepted = false
            window.hide()
        }
    }
    Connections {
        target: app
        function onTrayShowRequested() {
            // See raiseIntoView(): `visible = true`, never show(). This is the
            // path that first learned it, and the notification path now shares
            // the same helper rather than repeating the mistake.
            window.raiseIntoView()
        }
    }
    // Ctrl+Q quits for real. It exists because the tray icon deliberately
    // carries no context menu (QSystemTrayIcon takes a QtWidgets QMenu and
    // this process is a QGuiApplication), so this is the way out once the
    // window has been closed into the tray and brought back.
    //
    // "For real" needs the flag. Qt.quit() posts QEvent::Quit, and
    // QGuiApplication answers it by asking every top-level window to close
    // FIRST — one that refuses aborts the whole quit (`e->ignore()`). With
    // close-to-tray on, onClosing above was that refusal, so Ctrl+Q merely
    // hid the window: the one documented way out of the tray did not work in
    // exactly the mode that puts you there. Announcing the intent before
    // asking lets the close handler stand aside.
    //
    // Deliberately still Qt.quit() and not Qt.exit(): the real shutdown work
    // hangs off QCoreApplication::aboutToQuit (AppController's teardown, and
    // UpdateManager's apply-on-quit), and tearing the event loop down under
    // it would skip both.
    //
    // ApplicationShortcut, not the default WindowShortcut, so it still fires
    // while a native dialog of ours holds focus. Nothing can reach it while
    // the window is hidden in the tray — a hidden window has no focus — which
    // is why restoring from the tray comes first.
    Shortcut {
        // Sequence from ShortcutRegistry (Settings -> Keyboard shortcuts).
        // bindingRevision is read INSIDE the binding on purpose: sequenceFor()
        // is a function call and creates no dependency Qt can track, so
        // without it a rebind would not apply until this component was next
        // created.
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
            window.raiseIntoView()
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
    // Reduced motion, assigned at last. AppTheme has DECLARED `reducedMotion`
    // since the design round and ~20 branches across ten QML files consume it
    // — and nothing ever wrote it, so every one of those branches was dead.
    //
    // PUSHED IN from here rather than read inside the singleton, exactly like
    // `mode` and `customOverrides` above: AppTheme is a `pragma Singleton`
    // that may be created before the `app` context property exists, and an
    // `app.` dereference inside it would be resolved at whatever moment the
    // singleton happens to be built.
    Binding {
        target: AppTheme
        property: "reducedMotion"
        value: app.settings ? app.settings.reducedMotion : false
    }
    // Composer policy, pushed into the C++ composer the same way the theme
    // settings are pushed into AppTheme. MessageComposer holds no
    // SettingsManager (its collaborators are injected), and a setting nothing
    // ever writes is the dead-branch failure `reducedMotion` just cost us —
    // so the one place that owns both objects does the assignment.
    Binding {
        target: app.composer
        property: "sendTextAsCaption"
        value: app.settings ? app.settings.sendTextAsCaption : false
    }
    // v0.9 slash commands: the composer only ASKS for these — mode is a
    // setting, and display-name changes carry AppController's op-id
    // bookkeeping. Wired here because this is the one place that owns both
    // sides, same rationale as the caption binding above.
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
    // The user-authored palette (Settings → Appearance → Custom theme).
    // Pushed in the same way as the theme id, so selecting Custom and editing
    // a colour repaint through exactly one path. CustomThemeStore has already
    // dropped unknown roles and malformed values.
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
    // Content text scale (Settings → Appearance → Text size).
    Binding {
        target: AppTheme
        property: "textScale"
        value: app.settings ? app.settings.textScale / 100 : 1
    }
    // The selected UI font follows the per-account Appearance setting.
    // Controls inherit through the window font; explicit AppTheme.uiFont
    // bindings cover non-inheriting text items.
    //
    // The value is FontManager's RESOLVED family, not the raw setting: the
    // user may have chosen a system font that has since been uninstalled, or
    // an imported one whose file is gone, and this token must never carry a
    // family the host cannot draw. FontManager falls back to the bundled face
    // WITHOUT rewriting the stored choice, so re-installing the font brings
    // it back on its own.
    Binding {
        target: AppTheme
        property: "uiFont"
        // `typeof` and not a plain truth test: `fonts` is a CONTEXT
        // property, and several QML suites load this shell without one.
        // Referencing an unresolved name throws a ReferenceError and kills
        // the binding; `typeof` on it does not.
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.uiFamily : "Manrope"
    }
    // Same contract for the code/monospace face.
    Binding {
        target: AppTheme
        property: "monoFont"
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.monospaceFamily : "JetBrains Mono"
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
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            // Ctrl++ stays a hard-coded ALTERNATE. Keyboards differ on
            // whether Ctrl+= or Ctrl++ is reachable, and the registry stores
            // ONE sequence per action — dropping the alternate would silently
            // remove zoom-in on some layouts. It is deliberately not a
            // registry row: it is the same action, not a second one.
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
        // Whether this notice is reporting a FAILURE. Every original caller
        // is an error report and passes nothing, so an omitted argument
        // stays danger — the sticker round is the first non-failure user of
        // this banner and passes false explicitly.
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
        // "Add to my stickers": the message context menu has closed by the
        // time the account-data write answers, so the outcome is reported
        // here, on the same transient notice.
        //
        // SUCCESS is reported, unlike a pin — nothing visible changes when a
        // sticker lands in your pack (the pack is only seen inside the
        // picker), so a silent success is indistinguishable from a dead
        // menu item. The shortcode the image actually got is named, because
        // it may carry a numeric suffix the user did not ask for.
        Connections {
            target: app.stickers
            function onSaveFinished(ok, category, shortcode, scope) {
                // Two destinations report here — this account's own pack and
                // the ROOM's — and a notice that could not tell them apart
                // would tell the user their sticker went somewhere it did
                // not.
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
                    // Not an error: the sticker IS in the pack, which is what
                    // the user wanted. Saying "failed" here would send them
                    // looking for a problem that does not exist.
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
                    // Only the ROOM write can be refused this way: it is a
                    // state event and the room decides who may send it.
                    pinNotice.show(
                        qsTr("You do not have permission to change this "
                             + "room's stickers."), true)
                } else {
                    pinNotice.show(
                        qsTr("The sticker could not be saved."), true)
                }
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

    // ── Picture-in-picture ───────────────────────────────────────────────
    //
    // A separate top-level Window, so it lives outside this window's scene
    // entirely and keeps carrying the call when this one is minimised or
    // closed to the tray. Its visibility is driven by CallStageState's flag
    // (see CallPipWindow's header for why that is imperative rather than a
    // binding), and the flag is dropped when the call ends.
    CallPipWindow {
        objectName: "callPipWindow"
        onRestoreRequested: {
            // Standing the PiP down BEFORE raising is deliberate: the window
            // this is going back to will rebuild the call stage, and the
            // video router hands a track's sink to whoever attached last —
            // so the surface that is going away has to let go first.
            if (app.groupCall && app.groupCall.stageState)
                app.groupCall.stageState.setPictureInPicture(false)
            window.raiseIntoView()
        }
    }
    // Automatic pop-out. A floating window is worth having exactly when this
    // one is not on screen, and that is the only case it fires in: minimised
    // or closed to the tray, with a call actually live. Coming back on screen
    // stands it down again, so the two are never both showing the call.
    //
    // It never opens by itself while this window is visible — that would be a
    // window appearing over the user's work for no reason they asked for —
    // and it does not close a PiP the user opened deliberately from the call
    // bar, which is why it only writes the flag on a transition it caused.
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
    // NOT a second onVisibilityChanged — QML permits exactly one handler per
    // signal and the second is a compile error ("Property value set multiple
    // times"), so this hangs off the existing one at the top of the file.
    onPipCallLiveChanged: window.syncAutomaticPip()

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
