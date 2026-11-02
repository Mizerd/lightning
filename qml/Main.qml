import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

ApplicationWindow {
    id: window

    readonly property rect startupGeometry: app.restorableWindowGeometry
    readonly property bool hasStartupGeometry: startupGeometry.width > 0
                                               && startupGeometry.height > 0
    width: hasStartupGeometry ? startupGeometry.width : 1100
    height: hasStartupGeometry ? startupGeometry.height : 720
    minimumWidth: 640
    minimumHeight: 360

    // Start hidden, compute the position once, assign it imperatively, then
    // show. Bindings on x/y re-centred the window whenever screen metrics or
    // width changed, fighting the user's drags; and Qt shows the window during
    // componentComplete(), so placing it later from onCompleted makes it jump.
    visible: false

    function applyStartupPlacement() {
        if (hasStartupGeometry) {
            x = startupGeometry.x
            y = startupGeometry.y
            app.noteWindowPlacement("restored", x, y, width, height)
            return
        }
        var centred = app.centredWindowRect(width, height)
        if (centred.width > 0 && centred.height > 0) {
            x = centred.x
            y = centred.y
            app.noteWindowPlacement("centred", x, y, width, height)
            return
        }
        app.noteWindowPlacement("platform-default", x, y, width, height)
    }

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

    LayoutMirroring.enabled: app.localization.rightToLeft
    LayoutMirroring.childrenInherit: true

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
    property int lastOnScreenVisibility: Window.Windowed

    onVisibilityChanged: {
        if (window.visibility !== Window.Hidden)
            window.hiddenByClose = false
        if (window.visibility === Window.Maximized
                || window.visibility === Window.Windowed)
            window.lastOnScreenVisibility = window.visibility
        window.syncAutomaticPip()
        if (!geometrySaver.armed || !app.settings)
            return
        if (window.visibility === Window.Maximized)
            app.settings.saveWindowMaximized(true)
        else if (window.visibility === Window.Windowed)
            app.settings.saveWindowMaximized(false)
    }

    function raiseIntoView() {
        if (window.visibility === Window.Hidden)
            window.visible = true
        else if (window.visibility === Window.Minimized)
            window.visibility = window.lastOnScreenVisibility
        window.raise()
        window.requestActivate()
    }

    property bool quitRequested: false
    property bool hiddenByClose: false
    onClosing: (close) => {
        window.flushGeometry()
        // app.closeHidesWindow: close-to-tray where a tray exists; on macOS
        // every close, since Cmd+W is not Cmd+Q (see app/CloseBehavior.h).
        if (app.closeHidesWindow(window.quitRequested)) {
            close.accepted = false
            window.hiddenByClose = true
            window.hide()
        }
    }
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

    // The saved open is recorded before the queued room switch. Land only
    // after Rust's timeline reset has been adopted by the room model.
    property string savedLandingRoom: ""
    property string savedLandingEvent: ""
    Timer {
        id: savedLandingExpiry
        interval: 10000
        onTriggered: {
            window.savedLandingRoom = ""
            window.savedLandingEvent = ""
        }
    }
    Connections {
        target: app.savedMessages
        function onOpenRequested(roomId, eventId, threadRootId) {
            window.savedLandingRoom = eventId && !threadRootId
                                      && roomId !== app.currentRoomId ? roomId : ""
            window.savedLandingEvent = window.savedLandingRoom ? eventId : ""
            if (window.savedLandingEvent) savedLandingExpiry.restart()
        }
    }
    Connections {
        target: app
        function onRoomTimelineReady(roomId) {
            if (window.savedLandingEvent !== ""
                    && window.savedLandingRoom === roomId
                    && roomId === app.currentRoomId) {
                var eventId = window.savedLandingEvent
                window.savedLandingRoom = ""
                window.savedLandingEvent = ""
                savedLandingExpiry.stop()
                app.pagination.jumpToEvent(eventId)
            }
        }
    }
    Shortcut {
        objectName: "streamInfoShortcut"
        sequences: {
            var _rev = app.shortcuts.bindingRevision
            return [app.shortcuts.sequenceFor("call.toggleStreamInfo")]
        }
        context: Qt.ApplicationShortcut
        onActivated: app.groupCall.streamInfoVisible = !app.groupCall.streamInfoVisible
    }

    Shortcut {
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
            if (inThread && app.timeline.threadRepliesInline === true) {
                app.showThread(roomId, threadRootId)
                if (eventId && eventId.length > 0) {
                    Qt.callLater(function() {
                        app.pagination.jumpToEvent(eventId)
                    })
                }
                return
            }
            if (inThread)
                app.thread.openThreadAt(roomId, threadRootId, eventId || "")
            if (inThread) {
                Qt.callLater(function() {
                    app.pagination.revealIfLoaded(threadRootId)
                })
            } else if (eventId && eventId.length > 0) {
                if (window.savedLandingRoom !== roomId
                        || window.savedLandingEvent !== eventId) {
                    Qt.callLater(function() {
                        app.pagination.jumpToEvent(eventId)
                    })
                }
            }
        }
    }

    Binding {
        target: AppTheme
        property: "mode"
        value: app.settings ? app.settings.theme : 0
    }
    Binding {
        target: AppTheme
        property: "systemDark"
        value: app.systemDarkMode
    }
    Binding {
        target: AppTheme
        property: "reducedMotion"
        value: app.settings ? app.settings.reducedMotion : false
    }
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
    Binding {
        target: app.thread
        property: "imageMetadataMode"
        value: app.settings ? app.settings.imageMetadataMode : "location"
    }
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
    Binding {
        target: AppTheme
        property: "customGradients"
        value: app.customTheme ? app.customTheme.gradients : ({})
    }
    Binding {
        target: AppTheme
        property: "surfaceDepth"
        value: app.backdrops ? app.backdrops.surfaceDepth : 0
    }
    Binding {
        target: AppTheme
        property: "textScale"
        value: app.settings ? app.settings.textScale / 100 : 1
    }
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
        value: !app.settings ? 4.5
               : app.settings.nameColorCorrection === 2 ? 1.0
               : app.settings.nameColorCorrection === 1 ? 3.0 : 4.5
    }
    Binding {
        target: AppTheme
        property: "messageSpacing"
        value: app.settings ? app.settings.messageSpacing : 1
    }
    Binding {
        target: AppTheme
        property: "uiFont"
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.uiFamily : "Manrope"
    }
    Binding {
        target: AppTheme
        property: "monoFont"
        value: (typeof fonts !== "undefined" && fonts)
               ? fonts.monospaceFamily : "JetBrains Mono"
    }
    font.family: AppTheme.uiFont

    function toggleAudibleMedia(event) {
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
        anchors.bottomMargin: voiceDockFallback.visible
                              ? voiceDockFallback.height : 0

        Keys.onSpacePressed: (event) => window.toggleAudibleMedia(event)
        visible: app.currentScreen !== 2
        enabled: visible

        function pickComponent() {
            var s = app.currentScreen
            if (s === 1 || s === 2) return mainComponent
            if (s === 3) return bootComponent
            return loginComponent
        }
        sourceComponent: pickComponent()

        Connections {
            target: app
            function onCurrentScreenChanged() {
                pageLoader.sourceComponent = pageLoader.pickComponent()
            }
        }
    }

    Component { id: loginComponent;    LoginScreen {} }
    Component { id: mainComponent;     MainScreen { promptRegion: verifySessionPromptCard } }
    Component {
        id: bootComponent
        Rectangle {
            objectName: "startupRestoreSurface"
            color: AppTheme.background
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingM
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
                    font.pixelSize: AppTheme.scaled(26)
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
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                }
            }
        }
    }

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
    VoiceMiniPlayer {
        id: voiceMiniPlayer
        anchors.fill: parent
        z: 40
        hintProvider: function() {
            var pane = window.voicePane
            if (!pane || !pane.visible || !pane.chatSafeArea)
                return null
            var r = pane.chatSafeArea
            var p = pane.mapToItem(voiceMiniPlayer, r.x, r.y)
            return Qt.rect(p.x, p.y, r.width, r.height)
        }
    }

    Shortcut {
        sequence: "Ctrl+Shift+D"
        enabled: app.screenshotDemoActive
        onActivated: if (app.demo) app.demo.toggleControls()
    }

    function _adjustZoom(delta) {
        var next = delta === 0 ? 100 : app.settings.interfaceZoom + delta
        app.settings.interfaceZoom = next
        zoomNotice.show()
    }
    Shortcut {
        sequences: {
            var _rev = app.shortcuts.bindingRevision
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
    Rectangle {
        id: infoToast
        objectName: "infoToast"
        function show(text) {
            infoToastLabel.text = text
            visible = true
            infoToastTimer.restart()
        }
        visible: false
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: AppTheme.spacing16
        z: 1000
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        width: Math.min(infoToastLabel.implicitWidth + AppTheme.spacing16 * 2,
                        parent.width - AppTheme.spacing16 * 2)
        height: infoToastLabel.implicitHeight + AppTheme.spacing12
        Label {
            id: infoToastLabel
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacing16 * 2
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            // Space names are remote text.
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.pixelSize: AppTheme.fontSecondary
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        Timer {
            id: infoToastTimer
            interval: 3000
            onTriggered: infoToast.visible = false
        }
    }
    Connections {
        target: app
        function onNoticeReported(message) { infoToast.show(message) }
    }
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
    Rectangle {
        id: pinNotice
        objectName: "pinActionNotice"
        property string message: ""
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
        Connections {
            target: app.composer
            function onEditsRemoved(eventId, ok, removed, failed, truncated) {
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
        Connections {
            target: app.savedMessages
            function onActionFailed(message) {
                pinNotice.show(message, true)
            }
            function onSaveSucceeded() {
                pinNotice.show(qsTr("Message saved."), false)
            }
        }
        Connections {
            target: app.moderation
            function onReportFinished(ok, message) {
                if (message.length > 0)
                    pinNotice.show(message, !ok)
            }
        }
        Connections {
            target: app.stickers
            function onSaveFinished(ok, category, shortcode, scope) {
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
                    pinNotice.show(
                        qsTr("You do not have permission to change this "
                             + "room's stickers."), true)
                } else if (category.indexOf("svg_") === 0) {
                    pinNotice.show(
                        app.stickers.svgMessage(category.substring(4)), true)
                } else {
                    pinNotice.show(
                        qsTr("The sticker could not be saved."), true)
                }
            }
        }
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
        Connections {
            target: app.forward
            function onForwardFailed(targetRoomId, message) {
                if (message.length === 0)
                    return
                if (targetRoomId.length > 0
                        && targetRoomId !== app.currentRoomId)
                    return
                pinNotice.show(message)
            }
        }
    }

    VerificationDialog {
        id: verificationDialog
        objectName: "verificationDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    UiaPromptDialog {
        id: uiaPromptDialog
        parent: Overlay.overlay
    }

    ReportMessageDialog {
        id: reportMessageDialog
        parent: Overlay.overlay
    }

    ForwardMessageDialog {
        id: forwardMessageDialog
        parent: Overlay.overlay
    }

    UpdateAvailableDialog {
        id: updateAvailableDialog
        parent: Overlay.overlay
    }

    FileChooserDialog {
        id: fileChooserDialog
    }

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
        interval: 250
        repeat: true
        running: cornerPassiveHost.height > 0 || incomingCallHost.height > 0
        triggeredOnStart: true
        onTriggered: window.refreshComposerClearance()
    }

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
        IndexAllPrompt {
            objectName: "indexAllPromptHost"
            blocked: verifySessionPromptCard.shouldShow
                     || encryptionBrokenPromptCard.shouldShow
        }
        BackgroundSyncPrompt {
            objectName: "backgroundSyncPromptHost"
            blocked: verifySessionPromptCard.shouldShow
                     || encryptionBrokenPromptCard.shouldShow
        }
        EncryptionBrokenPrompt {
            id: encryptionBrokenPromptCard
            objectName: "encryptionBrokenPromptHost"
        }
    }

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
                 && (statusBar.shown !== ""
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
                visible: statusBar.shown !== ""
                text: statusBar.shown
                textFormat: Text.PlainText
                color: AppTheme.error
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
            IconButton {
                objectName: "statusErrorDismiss"
                visible: statusBar.shown !== ""
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
        readonly property string shown:
            app.currentScreen === 0 && lastError === app.auth.lastError
            ? "" : lastError
    }

    Connections {
        target: app
        function onErrorReported(msg) { statusBar.lastError = msg }
    }
}
