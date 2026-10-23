import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Keyboard-driven quick switcher (Ctrl+K): a modal overlay over the local
// room/DM/Space/invite data (app.quickSwitcher). No network search, no
// persisted message text; selecting routes through normal navigation. Two modes
// in one 480px surface. Navigate mode lists rooms, people, spaces and invites
// in sections. Command mode (">" as the first character, or Ctrl+Shift+K) shows
// a bolt tile, an Actions/Rooms/People scope row and a small declarative set of
// executable actions (no leave/mute/file actions, no slash-command registry).
Popup {
    id: switcher
    // Discover / Join is hosted elsewhere (the dialog lives in RoomsPanel); the
    // switcher only announces the intent.
    signal discoverRequested(string startMode)
    // Global message search (dialog hosted by MainScreen), optionally seeded
    // with what was typed.
    signal globalSearchRequested(string query)
    parent: Overlay.overlay
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // One shared width in both modes; min() guards narrow windows.
    width: Math.min(480, parent ? parent.width - 48 : 480)
    height: Math.min(460, parent ? parent.height - 120 : 460)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? Math.max(60, parent.height * 0.12) : 0
    padding: 0

    // Command mode, entered by ">" as the first character (stripped
    // immediately) or Ctrl+Shift+K. commandScope narrows results to Actions /
    // Rooms / People (the latter two filter the same app.quickSwitcher
    // results).
    property bool commandMode: false
    property string commandScope: "actions"
    // Set by openCommandMode() before open(); consumed once in onOpened so the
    // open animation cannot race it.
    property bool _pendingCommandMode: false

    function openCommandMode() {
        switcher._pendingCommandMode = true
        open()
    }

    onOpened: {
        app.quickSwitcher.query = ""
        app.quickSwitcher.refresh()
        queryField.text = ""
        queryField.forceActiveFocus()
        resultList.currentIndex = app.quickSwitcher.count > 0 ? 0 : -1
        commandList.currentIndex = -1
        commandMode = switcher._pendingCommandMode
        switcher._pendingCommandMode = false
        commandScope = "actions"
    }
    onClosed: {
        app.quickSwitcher.reset()
        commandMode = false
        commandScope = "actions"
        if (switcher._labsRowsHeld)
            switcher.refreshLabsAccountRows()
    }

    function activate(row) {
        if (row < 0 || row >= app.quickSwitcher.count) return
        var r = app.quickSwitcher.resultAt(row)
        if (!r.roomId) return
        if (r.isSpace) {
            if (app.spaces) app.spaces.activeSpaceId = r.roomId
        } else {
            // Rooms, DMs and invites open the room; an invite opens its
            // accept/decline view, never auto-accepted.
            app.openRoom(r.roomId)
            // Ctrl+K is a deliberate jump: ready to type (GitHub #29).
            app.requestComposerFocus(r.roomId)
        }
        switcher.close()
    }

    function activateCommandRow(row) {
        var rows = switcher.commandRows
        if (row < 0 || row >= rows.length) return
        var r = rows[row]
        if (r.kind === "action") {
            if (r.enabled === false) return
            if (typeof r.run === "function") r.run()
            switcher.close()
        } else {
            switcher.activate(r.sourceRow)
        }
    }

    // Command-mode actions: Open Settings and each section, switch to another
    // account, and every real theme. Section icons/titles match
    // SettingsScreen's sectionIcon()/sectionTitle().
    function buildCommandActions() {
        var actions = []
        actions.push({
            kind: "action", id: "open-settings",
            label: qsTr("Open Settings"), subtitle: qsTr("Application action"),
            iconName: "settings", keywords: "settings preferences",
            enabled: true, run: function() { app.showSettings() }
        })
        if (app.discovery && app.discovery.supported) {
            actions.push({
                kind: "action", id: "discover-rooms",
                label: qsTr("Discover rooms"),
                subtitle: qsTr("Browse the public room directory"),
                iconName: "explore",
                keywords: "discover explore browse public directory rooms",
                enabled: true,
                run: function() { switcher.discoverRequested("browse") }
            })
            actions.push({
                kind: "action", id: "join-room",
                label: qsTr("Join a room by address…"),
                subtitle: qsTr("#room:server, !roomid or a Matrix link"),
                iconName: "tag",
                keywords: "join room alias address link matrix",
                enabled: true,
                run: function() { switcher.discoverRequested("address") }
            })
        }
        if (app.messageSearch && app.messageSearch.supported) {
            actions.push({
                kind: "action", id: "search-messages",
                label: qsTr("Search messages…"),
                subtitle: qsTr("Server-side history search (Ctrl+Shift+F)"),
                iconName: "search",
                keywords: "search messages history find global",
                enabled: true,
                run: function() { switcher.globalSearchRequested("") }
            })
        }
        // Looks: where people type "background" or "gradient" and expect to
        // land somewhere.
        if (app.backdrops) {
            actions.push({
                kind: "action", id: "chat-background",
                label: qsTr("Chat background…"),
                subtitle: qsTr("Your own picture behind every room"),
                iconName: "image",
                keywords: "chat background wallpaper picture image backdrop",
                enabled: true,
                run: function() { switcherBackgroundDialog.openForDefault() }
            })
            if (app.currentRoomId !== "") {
                actions.push({
                    kind: "action", id: "room-chat-background",
                    label: qsTr("Chat background for this room…"),
                    subtitle: qsTr("Only the room you have open"),
                    iconName: "image",
                    keywords: "room chat background wallpaper picture image",
                    enabled: true,
                    run: function() {
                        var row = app.roomList.findRoom(app.currentRoomId)
                        switcherBackgroundDialog.openForRoom(
                            app.currentRoomId, (row && row.name) || "")
                    }
                })
            }
        }
        actions.push({
            kind: "action", id: "custom-theme",
            label: qsTr("Create your own theme…"),
            subtitle: qsTr("Colours and gradients, in Appearance"),
            iconName: "palette",
            keywords: "custom theme colour color gradient editor create",
            enabled: true,
            run: function() { app.showSettingsSection("appearance") }
        })
        // Every section SettingsScreen has a nav row for, with matching titles
        // and glyphs; SettingsShellQmlTest asserts the two lists agree.
        var sectionDefs = [
            { key: "account", title: qsTr("Account"), icon: "account_circle" },
            { key: "appearance", title: qsTr("Appearance"), icon: "palette" },
            { key: "shortcuts", title: qsTr("Keyboard shortcuts"), icon: "keyboard_return" },
            { key: "notifications", title: qsTr("Notifications"), icon: "notifications" },
            { key: "sound", title: qsTr("Sound & video"), icon: "volume_up" },
            { key: "privacy", title: qsTr("Privacy & security"), icon: "verified_user" },
            { key: "sessions", title: qsTr("Sessions"), icon: "devices" },
            { key: "labs", title: qsTr("Labs"), icon: "science" },
            { key: "updates", title: qsTr("Updates"), icon: "download" },
            { key: "about", title: qsTr("About"), icon: "info" },
        ]
        for (var s = 0; s < sectionDefs.length; ++s) {
            (function(def) {
                actions.push({
                    kind: "action", id: "settings-" + def.key,
                    label: qsTr("Open %1").arg(def.title),
                    subtitle: qsTr("Settings"), iconName: def.icon,
                    keywords: "settings " + def.title,
                    enabled: true,
                    run: function() { app.showSettingsSection(def.key) }
                })
            })(sectionDefs[s])
        }
        // Labs: AccountListModel's rows (no secret read per row), copied by
        // refreshLabsAccountRows(); otherwise AccountManager's list as before.
        var accountList = app.accountList
            ? (switcher.labsAccountRows || [])
            : (app.accounts ? app.accounts.accounts : [])
        for (var a = 0; a < accountList.length; ++a) {
            (function(acc) {
                if (acc.isActive) return
                var label = (acc.displayName && acc.displayName.length > 0)
                    ? acc.displayName : acc.userId
                actions.push({
                    kind: "action", id: "switch-" + acc.userId,
                    label: qsTr("Switch to %1").arg(label),
                    subtitle: acc.userId, iconName: "person",
                    keywords: "switch account " + label + " " + acc.userId,
                    enabled: !app.accountSwitching && !(app.auth && app.auth.logoutPending),
                    accountUserId: acc.userId,
                    run: function() { app.switchToAccount(acc.userId) }
                })
            })(accountList[a])
        }
        actions.push({
            kind: "action", id: "theme-match-system",
            label: qsTr("Theme: Match system"), subtitle: qsTr("Appearance"),
            iconName: "palette", keywords: "theme match system auto",
            enabled: true, run: function() { app.settings.theme = 0 }
        })
        // The custom theme is offered only once it exists (an empty one looks
        // like nothing happened).
        var themes = AppTheme.themeList.filter(
            (t) => t.id !== 12 || (app.customTheme && app.customTheme.exists))
        for (var t = 0; t < themes.length; ++t) {
            (function(theme) {
                actions.push({
                    kind: "action", id: "theme-" + theme.id,
                    label: qsTr("Theme: %1").arg(theme.name),
                    subtitle: qsTr("Appearance"), iconName: "palette",
                    keywords: "theme " + theme.name,
                    enabled: true,
                    run: function() { app.settings.theme = theme.id }
                })
            })(themes[t])
        }
        return actions
    }
    readonly property var commandActions: buildCommandActions()

    // Labs (concurrent accounts). The rows' snapshot is a Q_INVOKABLE, so it is
    // copied here by handlers and never read inside a binding; null with Labs
    // off, when `app.accountList` is null.
    property var labsAccountRows: null
    // A host starting, running or pausing also signals rowsChanged. While the
    // switcher is open that change is held: replacing the rows would rebuild
    // the command list and put the keyboard back on the first entry under the
    // user's hands (the badges follow the feed anyway). Applied on close; any
    // other change is applied at once.
    property bool _labsRowsHeld: false
    function refreshLabsAccountRows() {
        const next = app.accountList ? app.accountList.rowsSnapshot() : null
        if (switcher.opened && switcher.activityFeed
                && switcher.activityFeed.sameAccountsIgnoringLiveState(
                    switcher.labsAccountRows, next)) {
            switcher._labsRowsHeld = true
            return
        }
        switcher._labsRowsHeld = false
        switcher.labsAccountRows = next
    }
    Connections {
        target: app
        // The signal only exists in builds that carry Labs.
        ignoreUnknownSignals: true
        function onLiveAccountsActiveChanged() { switcher.refreshLabsAccountRows() }
    }
    Connections {
        target: app.accountList ? app.accountList : null
        function onRowsChanged() { switcher.refreshLabsAccountRows() }
    }
    // Per-account activity for the "Switch to" entries; reads nothing while
    // `app.accountActivity` is null.
    readonly property AccountActivityFeed activityFeed: AccountActivityFeed {
        source: app.accountActivity ? app.accountActivity : null
        userIds: app.accountActivity
                 ? (switcher.labsAccountRows || []).map(a => a.userId || "") : []
    }
    // The "Chat background…" actions' dialog; a Popup of its own, so it
    // outlives the switcher closing behind it.
    ChatBackgroundDialog {
        id: switcherBackgroundDialog
        objectName: "switcherChatBackgroundDialog"
    }
    // One height per list, shared by the delegates and the paging arithmetic.
    readonly property int resultRowHeight: 48
    readonly property int commandRowHeight: 44
    // Names are untrusted (a room or invite name is chosen by a stranger), so
    // the hover text is drawn by a plain-text Label; the attached tooltip text property
    // would auto-detect rich text and fetch remote <img> resources.
    component PlainTip: ToolTip {
        id: plainTip
        property bool shown: false
        property string plainText: ""
        // A standalone ToolTip ignores `delay`, so the hover delay is a Timer.
        onShownChanged: {
            if (shown) tipDelay.restart()
            else { tipDelay.stop(); hide() }
        }
        Timer {
            id: tipDelay
            interval: 600
            onTriggered: if (plainTip.shown) plainTip.show(plainTip.plainText)
        }
        contentItem: Label {
            objectName: "plainTipLabel"
            text: plainTip.plainText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
    }
    readonly property bool hasOtherAccount: {
        var list = app.accountList ? (switcher.labsAccountRows || [])
                                   : (app.accounts ? app.accounts.accounts : [])
        for (var i = 0; i < list.length; ++i)
            if (!list[i].isActive) return true
        return false
    }
    // Command search folds case, accents and punctuation, and splits the
    // query into words: an action matches when EVERY word occurs somewhere in
    // its label or keywords, in any order ("indigo-nacht design" finds
    // "Design: Indigo-Nacht").
    function foldForSearch(text) {
        var t = String(text || "").toLowerCase()
        if (typeof t.normalize === "function")
            t = t.normalize("NFD").replace(/[\u0300-\u036f]/g, "")
        return t.replace(/[\s.,:;!?&|\/\\()\[\]{}<>"'`´“”„‟‘’‚‹›«»…·•\-–—_+=*#@~^%$]+/g, " ").trim()
    }
    function queryTokens(text) {
        var folded = foldForSearch(text)
        return folded.length === 0 ? [] : folded.split(" ")
    }
    readonly property string commandQueryLower: queryField.text.toLowerCase()
    readonly property var commandQueryTokens: queryTokens(queryField.text)
    function actionMatches(a) {
        var tokens = commandQueryTokens
        if (tokens.length === 0) return true
        var hay = foldForSearch(a.label + " " + (a.keywords || ""))
        for (var i = 0; i < tokens.length; ++i)
            if (hay.indexOf(tokens[i]) === -1) return false
        return true
    }
    // The footer's examples are REAL action labels, never separately
    // translated sentences, so a translation cannot drift from the matcher.
    function commandHintLabels() {
        var theme = "", sw = "", privacy = ""
        for (var i = 0; i < commandActions.length; ++i) {
            var a = commandActions[i]
            if (theme === "" && a.id.indexOf("theme-") === 0
                    && a.id !== "theme-match-system") theme = a.label
            else if (sw === "" && a.id.indexOf("switch-") === 0) sw = a.label
            else if (a.id === "settings-privacy") privacy = a.label
        }
        var out = []
        if (theme !== "") out.push(theme)
        if (sw !== "") out.push(sw)
        if (privacy !== "") out.push(privacy)
        return out
    }
    readonly property var commandHintExamples: commandHintLabels()
    readonly property string commandHintText: commandHintExamples.length === 0 ? ""
        : qsTr("Try %1").arg(commandHintExamples.map(l => "\u201c" + l + "\u201d").join(" \u00b7 "))
    readonly property var filteredCommandActions: commandActions.filter(actionMatches)

    // Rooms/People scope: a filter over the same live results, via the
    // presentation-safe resultAt() fields.
    function scopedEntityRowsFor(wantDm) {
        var rows = []
        const count = app.quickSwitcher.count
        for (var i = 0; i < count; ++i) {
            var r = app.quickSwitcher.resultAt(i)
            const isDm = r.category === "dm"
            if (wantDm !== isDm) continue
            r.kind = "entity"
            r.sourceRow = i
            rows.push(r)
        }
        return rows
    }
    readonly property var scopedEntityRows:
        commandScope === "rooms" ? scopedEntityRowsFor(false)
        : commandScope === "people" ? scopedEntityRowsFor(true)
        : []
    readonly property var commandRows:
        commandScope === "actions" ? filteredCommandActions : scopedEntityRows
    // The ListView renders from a ListModel of plain data: action entries hold
    // run() closures, and Qt 6.11's delegate model can report a count for a raw
    // JS object array yet instantiate no rows. activateCommandRow() dispatches
    // by index into commandRows, kept in the same order.
    ListModel { id: commandRowModel }
    function syncCommandRows() {
        commandRowModel.clear()
        for (var i = 0; i < commandRows.length; ++i) {
            var r = commandRows[i]
            commandRowModel.append({
                kind: r.kind || "", label: r.label || "",
                name: r.name || "", subtitle: r.subtitle || "",
                iconName: r.iconName || "", category: r.category || "",
                roomId: r.roomId || "", avatarUrl: r.avatarUrl || "",
                accountUserId: r.accountUserId || "",
                enabled: r.enabled !== false })
        }
        commandList.currentIndex = commandRows.length > 0 ? 0 : -1
    }
    onCommandRowsChanged: syncCommandRows()
    Component.onCompleted: {
        switcher.refreshLabsAccountRows()
        syncCommandRows()
    }

    function sectionLabelFor(category) {
        if (category === "room") return qsTr("ROOMS")
        if (category === "dm") return qsTr("PEOPLE")
        if (category === "space") return qsTr("SPACES")
        if (category === "invite") return qsTr("INVITES")
        return category.toUpperCase()
    }
    // Section jump (Tab/Shift+Tab, navigate mode): the model sorts by category,
    // so section runs are contiguous.
    function sectionStarts() {
        var starts = []
        var lastCat = null
        const count = app.quickSwitcher.count
        for (var i = 0; i < count; ++i) {
            var cat = app.quickSwitcher.resultAt(i).category
            if (cat !== lastCat) { starts.push(i); lastCat = cat }
        }
        return starts
    }
    // The visible list's selection moves through this one door so every key
    // (arrows, Tab wrap-around, paging) also scrolls the row into view.
    function dialogAction(event) {
        return switcher.opened && app.shortcuts
            ? app.shortcuts.dialogActionForKey(event.key, event.modifiers,
                queryField.activeFocus && queryField.selectedText.length > 0
                    && event.matches(StandardKey.Copy)) : ""
    }
    function handleDialogKey(event) {
        const action = switcher.dialogAction(event)
        if (action === "dialog.close") {
            switcher.close()
        } else if (action === "dialog.previousItem" || action === "dialog.nextItem") {
            const delta = action === "dialog.previousItem" ? -1 : 1
            const current = switcher.activeList().currentIndex
            switcher.selectRow(current < 0 ? 0 : current + delta)
        } else {
            return false
        }
        event.accepted = true
        return true
    }

    function activeList() { return switcher.commandMode ? commandList : resultList }
    function activeCount() {
        return switcher.commandMode ? switcher.commandRows.length
                                    : app.quickSwitcher.count
    }
    function selectRow(index) {
        const cnt = switcher.activeCount()
        if (cnt <= 0) return
        const list = switcher.activeList()
        list.currentIndex = Math.max(0, Math.min(index, cnt - 1))
    }
    function pageRows(list) {
        var rowH = list === commandList ? switcher.commandRowHeight
                                        : switcher.resultRowHeight
        return Math.max(1, Math.floor(list.height / rowH) - 1)
    }
    // Enter. With no match the fallbacks are the keyboard path too: message
    // search first, else the public room directory.
    function acceptCurrent() {
        if (switcher.commandMode) {
            switcher.activateCommandRow(commandList.currentIndex)
        } else if (app.quickSwitcher.count === 0) {
            if (searchMessagesButton.visible) searchMessagesButton.clicked()
            else if (exploreRoomsButton.visible) exploreRoomsButton.clicked()
        } else {
            switcher.activate(resultList.currentIndex)
        }
    }
    function jumpToSection(forward) {
        var starts = switcher.sectionStarts()
        if (starts.length === 0) return
        var cur = resultList.currentIndex < 0 ? 0 : resultList.currentIndex
        if (forward) {
            for (var i = 0; i < starts.length; ++i) {
                if (starts[i] > cur) { resultList.currentIndex = starts[i]; return }
            }
            resultList.currentIndex = starts[0]
        } else {
            for (var j = starts.length - 1; j >= 0; --j) {
                if (starts[j] < cur) { resultList.currentIndex = starts[j]; return }
            }
            resultList.currentIndex = starts[starts.length - 1]
        }
    }

    // HTML-escape untrusted names before any StyledText highlight.
    function escapeHtml(s) {
        return String(s)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function highlightedName(name, query, tintColor) {
        var safe = escapeHtml(name)
        var q = (query || "").trim()
        if (q.length === 0) return safe
        var lowerSafe = safe.toLowerCase()
        var lowerQ = escapeHtml(q).toLowerCase()
        var idx = lowerSafe.indexOf(lowerQ)
        if (idx === -1) return safe
        return safe.slice(0, idx) + "<font color=\"" + tintColor + "\">"
             + safe.slice(idx, idx + lowerQ.length) + "</font>"
             + safe.slice(idx + lowerQ.length)
    }

    Overlay.modal: Rectangle {
        color: AppTheme.modalScrim
    }

    background: Item {
        id: bgWrap
        // One of the design's four shadows (composer pattern).
        MultiEffect {
            source: card
            anchors.fill: card
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
        Rectangle {
            id: card
            anchors.fill: parent
            // Storm chrome: the theme-invariant navy panel.
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.radiusCard
        }
    }

    contentItem: ColumnLayout {
        Keys.onShortcutOverride: (event) => {
            if (switcher.dialogAction(event) !== "")
                event.accepted = true
        }
        Keys.onPressed: (event) => switcher.handleDialogKey(event)
        spacing: 0

        // Header: leading glyph, query field, ESC keycap
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing12
            spacing: AppTheme.spacing8

            Item {
                implicitWidth: 22
                implicitHeight: 22
                Icon {
                    anchors.centerIn: parent
                    visible: !switcher.commandMode
                    name: "search"
                    size: 19
                    color: AppTheme.stormTextMuted
                }
                // Command mode swaps the search glyph for the bolt tile
                // (boltInk glyph).
                Rectangle {
                    anchors.centerIn: parent
                    visible: switcher.commandMode
                    radius: AppTheme.radiusChip + 2
                    color: AppTheme.bolt
                    implicitWidth: 22
                    implicitHeight: 22
                    Icon {
                        anchors.centerIn: parent
                        name: "bolt"
                        size: 13
                        color: AppTheme.boltInk
                    }
                }
            }

            TextField {
                id: queryField
                objectName: "quickSwitcherField"
                Layout.fillWidth: true
                placeholderText: switcher.commandMode
                    ? qsTr("Type a command…")
                    : qsTr("Jump to a room, person, or Space…")
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightMedium
                color: AppTheme.stormText
                placeholderTextColor: AppTheme.placeholderInk
                selectionColor: AppTheme.stormSelection
                selectedTextColor: AppTheme.stormText
                background: Item {}
                onTextChanged: {
                    // ">" as the first character enters command mode and is
                    // stripped immediately (re-entering onTextChanged once,
                    // harmlessly).
                    if (!switcher.commandMode && text.length > 0 && text[0] === ">") {
                        switcher.commandMode = true
                        text = text.slice(1)
                        return
                    }
                    app.quickSwitcher.query = text
                    resultList.currentIndex = app.quickSwitcher.count > 0 ? 0 : -1
                }
                Keys.onShortcutOverride: (event) => {
                    if (switcher.dialogAction(event) !== "")
                        event.accepted = true
                }
                Keys.onPressed: (event) => {
                    if (switcher.handleDialogKey(event))
                        return
                    if (switcher.commandMode && event.key === Qt.Key_Backspace
                        && queryField.text.length === 0) {
                        switcher.commandMode = false
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                        const step = switcher.pageRows(switcher.activeList())
                        const cur = Math.max(0, switcher.activeList().currentIndex)
                        switcher.selectRow(event.key === Qt.Key_PageDown
                                           ? cur + step : cur - step)
                        event.accepted = true
                        return
                    }
                    // Ctrl+Home / Ctrl+End jump to the ends of the list; plain
                    // Home/End stay with the text cursor.
                    if ((event.modifiers & Qt.ControlModifier)
                        && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) {
                        switcher.selectRow(event.key === Qt.Key_Home
                                           ? 0 : switcher.activeCount() - 1)
                        event.accepted = true
                        return
                    }
                    if (!switcher.commandMode
                        && (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)) {
                        switcher.jumpToSection(event.key === Qt.Key_Tab)
                        event.accepted = true
                    }
                }
                Keys.onDownPressed: {
                    if (switcher.commandMode) {
                        const cnt = switcher.commandRows.length
                        if (cnt > 0)
                            commandList.currentIndex =
                                Math.min(commandList.currentIndex + 1, cnt - 1)
                    } else if (app.quickSwitcher.count > 0) {
                        resultList.currentIndex =
                            Math.min(resultList.currentIndex + 1,
                                     app.quickSwitcher.count - 1)
                    }
                }
                Keys.onUpPressed: {
                    if (switcher.commandMode) {
                        if (commandList.currentIndex > 0)
                            commandList.currentIndex -= 1
                    } else if (resultList.currentIndex > 0) {
                        resultList.currentIndex -= 1
                    }
                }
                Keys.onReturnPressed: switcher.acceptCurrent()
                Keys.onEnterPressed: switcher.acceptCurrent()
            }

            MenuKeycap {
                keys: "ESC"
                header: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        // Command-mode scope chips (Actions / Rooms / People)
        Row {
            visible: switcher.commandMode
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            spacing: AppTheme.spacing6

            Repeater {
                model: [
                    { value: "actions", label: qsTr("Actions") },
                    { value: "rooms", label: qsTr("Rooms") },
                    { value: "people", label: qsTr("People") },
                ]
                delegate: AbstractButton {
                    id: scopeChip
                    required property var modelData
                    readonly property bool selected:
                        switcher.commandScope === modelData.value
                    implicitWidth: chipLabel.implicitWidth + AppTheme.spacing12 * 2
                    implicitHeight: 26
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.label
                    Accessible.checked: scopeChip.selected
                    onClicked: switcher.commandScope = modelData.value
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: AppTheme.radiusPill
                        color: "transparent"
                        border.color: AppTheme.bolt
                        border.width: 2
                        visible: scopeChip.visualFocus
                    }
                    // Scope chips: selected is a bolt pill with boltInk;
                    // resting is an outline. A hovered unselected chip
                    // brightens its background so its ink stays AA.
                    contentItem: Label {
                        id: chipLabel
                        // Untrusted text: never markup.
                        textFormat: Text.PlainText
                        text: scopeChip.modelData.label
                        color: scopeChip.selected ? AppTheme.boltInk
                                                  : scopeChip.hovered ? AppTheme.stormText
                                                  : AppTheme.stormTextMuted
                        // The UI face at meta size, like other chips.
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusPill
                        color: scopeChip.selected ? AppTheme.bolt
                               : scopeChip.hovered ? AppTheme.stormSelection
                               : "transparent"
                        border.width: scopeChip.selected ? 0 : 1
                        border.color: AppTheme.stormBorderStrong
                    }
                }
            }
        }
        Rectangle {
            visible: switcher.commandMode
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        // Navigate-mode empty states. Nothing matched is not a dead end: the
        // words may be in a message, in a public room, or be a command.
        ColumnLayout {
            objectName: "quickSwitcherEmptyState"
            visible: !switcher.commandMode && app.quickSwitcher.count === 0
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing16
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: queryField.text.length > 0
                      ? qsTr("No matching rooms")
                      : qsTr("Type to search your rooms")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textBody
            }
            AppButton {
                id: searchMessagesButton
                objectName: "quickSwitcherSearchMessages"
                Layout.alignment: Qt.AlignHCenter
                visible: queryField.text.length > 0
                         && !!(app.messageSearch && app.messageSearch.supported)
                storm: true
                kind: "ghost"
                size: "sm"
                iconName: "search"
                // The user's own query, capped (an AppButton does not elide)
                // and with no "<" so the label can never read as markup.
                readonly property string shownQuery: {
                    var q = queryField.text.replace(/</g, "‹")
                    return q.length > 32 ? q.substring(0, 31) + "…" : q
                }
                text: qsTr("Search messages for “%1”").arg(shownQuery)
                onClicked: {
                    var q = queryField.text
                    switcher.close()
                    switcher.globalSearchRequested(q)
                }
            }
            AppButton {
                id: exploreRoomsButton
                objectName: "quickSwitcherExploreRooms"
                Layout.alignment: Qt.AlignHCenter
                visible: queryField.text.length > 0
                         && !!(app.discovery && app.discovery.supported)
                storm: true
                kind: "ghost"
                size: "sm"
                iconName: "explore"
                text: qsTr("Look for public rooms")
                onClicked: {
                    switcher.close()
                    switcher.discoverRequested("browse")
                }
            }
        }
        Label {
            visible: switcher.commandMode && switcher.commandRows.length === 0
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing16
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No matching actions")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
        }

        // Navigate mode: sectioned rooms/people/spaces/invites list
        Item {
            visible: !switcher.commandMode
            Layout.fillWidth: true
            // Mode-dependent: a plain `true` leaves the column's height
            // distribution stale when the mode flips while closed
            // (openCommandMode() before open()), so the visible list would open
            // at 0 height.
            Layout.fillHeight: !switcher.commandMode
            // The scroller is a sibling of the view, never inside it.
            MiddleClickScroller {
                objectName: "quickSwitcherMiddleClickScroller"
                anchors.fill: parent
                z: 1
                view: resultList
            }
            ListView {
                id: resultList
                objectName: "quickSwitcherList"
                anchors.fill: parent
                clip: true
                model: app.quickSwitcher
                currentIndex: -1
                keyNavigationEnabled: false
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Every selection change scrolls its row into view, including the
                // Tab wrap-around, which sets currentIndex from far away.
                onCurrentIndexChanged: {
                    if (currentIndex === 0) positionViewAtBeginning()
                    else if (currentIndex > 0)
                        positionViewAtIndex(currentIndex, ListView.Contain)
                }

                section.property: "category"
                section.criteria: ViewSection.FullString
                section.delegate: MenuSectionLabel {
                    text: switcher.sectionLabelFor(section)
                    leftPadding: AppTheme.spacing16
                    topPadding: AppTheme.spacing8
                    bottomPadding: AppTheme.spacing4
                }

                delegate: ItemDelegate {
                    id: row
                    width: ListView.view.width
                    height: switcher.resultRowHeight
                    highlighted: ListView.isCurrentItem
                    onClicked: switcher.activate(index)
                    Accessible.name: model.name + " " + (model.subtitle || "")
                    Accessible.selected: row.highlighted
                    // Names elide in a fixed-width dialog; hover shows the rest.
                    PlainTip {
                        objectName: "quickSwitcherRowTip"
                        shown: row.hovered && rowTitle.truncated
                        plainText: model.name || ""
                    }

                    background: Rectangle {
                        color: row.highlighted ? AppTheme.stormSelection : "transparent"
                        // Bolt cursor overhanging the selected row's left edge.
                        Icon {
                            visible: row.highlighted
                            name: "bolt"
                            size: 12
                            color: AppTheme.bolt
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 0
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing16
                        anchors.rightMargin: AppTheme.spacing12
                        spacing: AppTheme.spacing10

                        Avatar {
                            size: 28
                            // Invite pictures follow the account's MSC4278 choice.
                            mxc: (model.category === "invite" && app.accountPrefs
                                  && !app.accountPrefs.inviteAvatars)
                                 ? "" : model.avatarUrl
                            name: model.name
                            colorKey: model.identityColorKey || model.roomId
                            circle: model.category === "dm"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                id: rowTitle
                                Layout.fillWidth: true
                                textFormat: Text.StyledText
                                // The matched fragment is bolt on the selected row
                                // only; other rows brighten it to stormText.
                                text: switcher.highlightedName(
                                    model.name, app.quickSwitcher.query,
                                    row.highlighted ? "" + AppTheme.bolt
                                                    : "" + AppTheme.stormText)
                                color: row.highlighted ? AppTheme.stormText
                                                       : AppTheme.stormTextSecondary
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.textSubtitle
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                visible: (model.subtitle || "").length > 0
                                text: model.subtitle || ""
                                // AA on the selection fill.
                                color: row.highlighted ? AppTheme.stormTextSecondary
                                                     : AppTheme.stormTextMuted
                                // One face down the list; mono is for code and
                                // keycaps.
                                font.family: AppTheme.uiFont
                                // Meta size, not the mono identity size.
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                        }
                        MenuKeycap {
                            visible: row.highlighted
                            iconName: "keyboard_return"
                            tinted: true
                        }
                    }
                }
            }
        }

        // Command mode: actions or the scoped Rooms/People copy
        Item {
            visible: switcher.commandMode
            Layout.fillWidth: true
            // Mode-dependent, as for the navigate list.
            Layout.fillHeight: switcher.commandMode
            MiddleClickScroller {
                objectName: "quickSwitcherCommandMiddleClickScroller"
                anchors.fill: parent
                z: 1
                view: commandList
            }
            ListView {
                id: commandList
                objectName: "quickSwitcherCommandList"
                anchors.fill: parent
                clip: true
                model: commandRowModel
                currentIndex: -1
                keyNavigationEnabled: false
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                onCurrentIndexChanged: {
                    if (currentIndex === 0) positionViewAtBeginning()
                    else if (currentIndex > 0)
                        positionViewAtIndex(currentIndex, ListView.Contain)
                }

                delegate: ItemDelegate {
                    id: cmdRow
                    width: ListView.view.width
                    height: switcher.commandRowHeight
                    highlighted: ListView.isCurrentItem
                    enabled: model.kind !== "action" || model.enabled !== false
                    onClicked: switcher.activateCommandRow(index)
                    Accessible.name: model.kind === "action"
                        ? (model.label + " " + (model.subtitle || ""))
                        : (model.name + " " + (model.subtitle || ""))
                    Accessible.selected: cmdRow.highlighted
                    PlainTip {
                        objectName: "quickSwitcherCommandRowTip"
                        shown: cmdRow.hovered && commandRowTitleLabel.truncated
                        plainText: model.kind === "action" ? (model.label || "")
                                                           : (model.name || "")
                    }

                    background: Rectangle {
                        color: cmdRow.highlighted ? AppTheme.stormSelection : "transparent"
                        Icon {
                            visible: cmdRow.highlighted && cmdRow.enabled
                            name: "bolt"
                            size: 12
                            color: AppTheme.bolt
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 0
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing16
                        anchors.rightMargin: AppTheme.spacing12
                        spacing: AppTheme.spacing10
                        opacity: cmdRow.enabled ? 1.0 : 0.5

                        Rectangle {
                            visible: model.kind === "action"
                            implicitWidth: 30
                            implicitHeight: 30
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            Icon {
                                anchors.centerIn: parent
                                name: model.kind === "action" ? model.iconName : ""
                                size: AppTheme.menuIconSize
                                color: cmdRow.highlighted ? AppTheme.bolt
                                                          : AppTheme.stormTextMuted
                            }
                        }
                        Avatar {
                            visible: model.kind === "entity"
                            size: 28
                            mxc: model.kind === "entity"
                                 && !(model.category === "invite" && app.accountPrefs
                                      && !app.accountPrefs.inviteAvatars)
                                 ? model.avatarUrl : ""
                            name: model.kind === "entity" ? model.name : ""
                            colorKey: model.kind === "entity" ? (model.identityColorKey || model.roomId) : ""
                            circle: model.kind === "entity" && model.category === "dm"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                id: commandRowTitleLabel
                                objectName: "commandRowTitle"
                                Layout.fillWidth: true
                                // Row ink follows the selection for both kinds;
                                // only the matched entity fragment is tinted, via
                                // the same escape+highlight helper.
                                textFormat: model.kind === "entity"
                                            ? Text.StyledText : Text.PlainText
                                text: model.kind === "action"
                                      ? (model.label || "")
                                      : switcher.highlightedName(
                                            model.name || "",
                                            switcher.commandQueryLower,
                                            cmdRow.highlighted ? "" + AppTheme.bolt
                                                               : "" + AppTheme.stormText)
                                color: cmdRow.highlighted ? AppTheme.stormText
                                                          : AppTheme.stormTextSecondary
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.textSubtitle
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                visible: (model.subtitle || "").length > 0
                                text: model.subtitle || ""
                                // Muted ink; faint is reserved for decorative
                                // headers.
                                color: AppTheme.stormTextMuted
                                // One face and one size down the list.
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                        }
                        // A "Switch to" entry's account activity (Labs). Only
                        // inactive accounts get an entry, so the active-row rule
                        // holds by construction.
                        AccountActivityBadge {
                            objectName: "quickSwitcherAccountBadge"
                            readonly property var activity:
                                switcher.activityFeed.entryFor(model.accountUserId || "")
                            visible: (model.accountUserId || "").length > 0 && shown
                            liveState: activity.liveState
                            mentionCount: activity.mentionCount
                            mentionsLowerBound: activity.mentionsLowerBound
                            hasUnread: activity.hasUnread
                        }
                        MenuKeycap {
                            visible: cmdRow.highlighted
                            iconName: "keyboard_return"
                            tinted: true
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        // Footer hint bar
        Item {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing12
            implicitHeight: Math.max(navigateFooter.implicitHeight,
                                     commandFooter.implicitHeight)

            Row {
                id: navigateFooter
                visible: !switcher.commandMode
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing16

                Row {
                    spacing: AppTheme.spacing4
                    Text {
                        text: "↑↓"
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: qsTr("navigate")
                        color: AppTheme.stormTextMuted
                        // The keycap glyph stays mono; the word is prose.
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
                Row {
                    spacing: AppTheme.spacing4
                    Icon {
                        name: "keyboard_return"
                        size: AppTheme.textMeta + 2
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        text: qsTr("open")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
                Row {
                    spacing: AppTheme.spacing4
                    Icon {
                        name: "keyboard_tab"
                        size: AppTheme.textMeta + 2
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        text: qsTr("sections")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
                // ESC already has a keycap in the header; this slot names
                // the command mode, which nothing else on screen mentions.
                Row {
                    objectName: "quickSwitcherCommandHint"
                    spacing: AppTheme.spacing4
                    Text {
                        text: ">"
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: qsTr("commands")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }

            Label {
                id: commandFooter
                visible: switcher.commandMode
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Label.ElideRight
                objectName: "quickSwitcherCommandFooter"
                text: switcher.commandHintText
                color: AppTheme.stormTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
        }
    }
}
