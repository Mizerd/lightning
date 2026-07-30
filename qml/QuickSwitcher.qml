import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.6.1: keyboard-driven quick switcher (Ctrl+K). A modal overlay over the
// already-available local room/DM/Space/invite presentation data
// (app.quickSwitcher). No network search, no persisted message text.
// Selecting a result routes through the app's normal navigation.
//
// v0.6.5 (SPEC §1j+1k): one 480px-wide surface, two modes. Navigate mode
// (default, Ctrl+K) lists rooms/people/spaces/invites grouped into sections.
// Command mode (typed ">" as the first character, or Ctrl+Shift+K) swaps the
// header glyph for a bolt tile (storm 2d), adds an Actions/Rooms/People
// scope row, and lists a small declarative set of honestly-executable
// application actions — never leave/mute/file actions, never a
// slash-command registry.
Popup {
    id: switcher
    parent: Overlay.overlay
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // SPEC R6: one shared width (480) in both modes; the min() only guards
    // against a window narrower than the design width.
    width: Math.min(480, parent ? parent.width - 48 : 480)
    height: Math.min(460, parent ? parent.height - 120 : 460)
    x: parent ? (parent.width - width) / 2 : 0
    // Top-aligned, as before v0.6.5.
    y: parent ? Math.max(60, parent.height * 0.12) : 0
    padding: 0

    // Command mode state. Entered by typing ">" as the very first character
    // of the query (stripped immediately so the field never shows it) or by
    // Ctrl+Shift+K (MainScreen.qml). commandScope narrows the command-mode
    // result list to Actions (the declarative action set) / Rooms / People
    // (both simply filter the SAME live app.quickSwitcher results).
    property bool commandMode: false
    property string commandScope: "actions"
    // Set by openCommandMode() just before open(); onOpened consumes it
    // exactly once so timing of the Popup's own open animation cannot race it.
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
    }

    function activate(row) {
        if (row < 0 || row >= app.quickSwitcher.count) return
        var r = app.quickSwitcher.resultAt(row)
        if (!r.roomId) return
        if (r.isSpace) {
            if (app.spaces) app.spaces.activeSpaceId = r.roomId
        } else {
            // Rooms, DMs, and invites all open the room context; an invite
            // opens its accept/decline view — never auto-accepted here.
            app.openRoom(r.roomId)
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

    // ---- Command-mode declarative action list (SPEC 1k). Honestly
    // executable actions only: Open Settings + each section, switch to a
    // non-active account, and every real theme. No leave/mute/files/slash
    // registry. Section icons/titles are duplicated from SettingsScreen.qml's
    // sectionIcon()/sectionTitle() — keep them in sync if either changes.
    function buildCommandActions() {
        var actions = []
        actions.push({
            kind: "action", id: "open-settings",
            label: qsTr("Open Settings"), subtitle: qsTr("Application action"),
            iconName: "settings", keywords: "settings preferences",
            enabled: true, run: function() { app.showSettings() }
        })
        var sectionDefs = [
            { key: "account", title: qsTr("Account"), icon: "account_circle" },
            { key: "appearance", title: qsTr("Appearance"), icon: "palette" },
            { key: "notifications", title: qsTr("Notifications"), icon: "notifications" },
            { key: "privacy", title: qsTr("Privacy & security"), icon: "verified_user" },
            { key: "sessions", title: qsTr("Sessions"), icon: "devices" },
            { key: "labs", title: qsTr("Labs"), icon: "science" },
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
        var accountList = app.accounts ? app.accounts.accounts : []
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
                    enabled: !app.accountSwitching,
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
        var themes = AppTheme.themeList
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
    readonly property string commandQueryLower: queryField.text.toLowerCase()
    function actionMatches(a) {
        if (commandQueryLower.length === 0) return true
        var hay = (a.label + " " + (a.keywords || "")).toLowerCase()
        return hay.indexOf(commandQueryLower) !== -1
    }
    readonly property var filteredCommandActions: commandActions.filter(actionMatches)

    // Rooms/People scope chips: a genuine filter of the SAME live
    // app.quickSwitcher results (not a separate search), read via the
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
    // The ListView renders from a ListModel kept in sync with commandRows,
    // carrying PLAIN DATA only. Two reasons: action entries hold their
    // run() closures (never model material), and Qt 6.11's delegate-model
    // adapter can report count for a raw QJSValue object-array yet
    // instantiate ZERO rows — a ListModel sidesteps that entirely.
    // activateCommandRow() dispatches by index into commandRows, which
    // stays the closure-carrying source in identical order.
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
                enabled: r.enabled !== false })
        }
        commandList.currentIndex = commandRows.length > 0 ? 0 : -1
    }
    onCommandRowsChanged: syncCommandRows()
    Component.onCompleted: syncCommandRows()

    function sectionLabelFor(category) {
        if (category === "room") return qsTr("ROOMS")
        if (category === "dm") return qsTr("PEOPLE")
        if (category === "space") return qsTr("SPACES")
        if (category === "invite") return qsTr("INVITES")
        return category.toUpperCase()
    }
    // Section-jump (Tab/Shift+Tab, navigate mode only): the model sorts by
    // category first (room, dm, space, invite), so section runs are
    // contiguous and their starts are found in one pass.
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

    // HTML-escape untrusted display names before any StyledText highlight
    // (room/DM names are user-controlled).
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
        // One of the design's four sanctioned shadows (composer pattern).
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
            // Storm chrome (SPEC-storm-language §3.1 / mock 2d): the
            // theme-invariant navy panel.
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.radiusCard
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // ── Header: leading glyph, query field, ESC keycap ────────────────
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
                // Storm 2d: command mode swaps the search glyph for the
                // yellow bolt square (bolt tile, panel-ink bolt glyph).
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
                        color: AppTheme.stormPanel
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
                font.pixelSize: AppTheme.fontQuery
                font.weight: Font.Medium
                color: AppTheme.stormText
                placeholderTextColor: AppTheme.stormTextMuted
                selectionColor: AppTheme.stormSelection
                selectedTextColor: AppTheme.stormText
                background: Item {}
                onTextChanged: {
                    // Typing ">" as the very first character of the query
                    // enters command mode; the trigger character is stripped
                    // immediately so it never becomes part of the query text
                    // (this re-enters onTextChanged once, harmlessly).
                    if (!switcher.commandMode && text.length > 0 && text[0] === ">") {
                        switcher.commandMode = true
                        text = text.slice(1)
                        return
                    }
                    app.quickSwitcher.query = text
                    resultList.currentIndex = app.quickSwitcher.count > 0 ? 0 : -1
                }
                Keys.onPressed: (event) => {
                    if (switcher.commandMode && event.key === Qt.Key_Backspace
                        && queryField.text.length === 0) {
                        switcher.commandMode = false
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
                Keys.onReturnPressed: switcher.commandMode
                    ? switcher.activateCommandRow(commandList.currentIndex)
                    : switcher.activate(resultList.currentIndex)
                Keys.onEnterPressed: switcher.commandMode
                    ? switcher.activateCommandRow(commandList.currentIndex)
                    : switcher.activate(resultList.currentIndex)
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

        // ── Command-mode scope chips (Actions / Rooms / People) ───────────
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
                    // Storm §3.7 scope chips: selected = bolt pill with
                    // panel ink; resting = stormBorderStrong outline, muted
                    // mono UPPERCASE.
                    contentItem: Label {
                        id: chipLabel
                        text: scopeChip.modelData.label
                        color: scopeChip.selected ? AppTheme.stormPanel
                                                  : AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontChip
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
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

        // ── Navigate-mode empty states ─────────────────────────────────────
        Label {
            visible: !switcher.commandMode && app.quickSwitcher.count === 0
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing16
            horizontalAlignment: Text.AlignHCenter
            text: queryField.text.length > 0
                  ? qsTr("No matching rooms")
                  : qsTr("Type to search your rooms")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.fontSecondary
        }
        Label {
            visible: switcher.commandMode && switcher.commandRows.length === 0
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing16
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No matching actions")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.fontSecondary
        }

        // ── Navigate mode: sectioned rooms/people/spaces/invites list ─────
        ListView {
            id: resultList
            objectName: "quickSwitcherList"
            visible: !switcher.commandMode
            Layout.fillWidth: true
            // Mode-dependent on purpose: a plain `true` leaves the column's
            // height distribution STALE when the mode flips while the popup
            // is closed (openCommandMode() before open()) — the invisible
            // list keeps the fill space and its sibling opens at 0 height.
            // Changing the attached property invalidates the layout at flip
            // time, forcing a correct redistribution on show.
            Layout.fillHeight: !switcher.commandMode
            clip: true
            model: app.quickSwitcher
            currentIndex: -1
            keyNavigationEnabled: false
            ScrollBar.vertical: ScrollBar {}

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
                height: 48
                highlighted: ListView.isCurrentItem
                onClicked: switcher.activate(index)
                Accessible.name: model.name + " " + (model.subtitle || "")
                Accessible.selected: row.highlighted

                background: Rectangle {
                    color: row.highlighted ? AppTheme.stormSelection : "transparent"
                    // Storm §3.2 signature cursor: a small bolt overhanging
                    // the selected row's left edge.
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
                        mxc: model.avatarUrl
                        name: model.name
                        colorKey: model.roomId
                        circle: model.category === "dm"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            // Matched fragment: bolt on the selected row
                            // only; resting rows brighten it to stormText
                            // (mock 2d — one yellow row per surface).
                            text: switcher.highlightedName(
                                model.name, app.quickSwitcher.query,
                                row.highlighted ? "" + AppTheme.bolt
                                                : "" + AppTheme.stormText)
                            color: row.highlighted ? AppTheme.stormText
                                                   : AppTheme.stormTextSecondary
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.fontResult
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: (model.subtitle || "").length > 0
                            text: model.subtitle || ""
                            // AA on the selection fill: the highlighted
                            // row's subtitle brightens one step.
                            color: row.highlighted ? AppTheme.stormTextSecondary
                                                 : AppTheme.stormTextMuted
                            // §0 reserves mono for identities; category
                            // words ("Invitation", "Space") stay in the UI
                            // face. Every real identity starts @/#/!.
                            font.family: /^[@#!]/.test(model.subtitle || "")
                                         ? AppTheme.monoFont : AppTheme.uiFont
                            font.pixelSize: AppTheme.fontMonoXS
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

        // ── Command mode: actions or the Rooms/People scoped copy ─────────
        ListView {
            id: commandList
            objectName: "quickSwitcherCommandList"
            visible: switcher.commandMode
            Layout.fillWidth: true
            // Mode-dependent for the same stale-distribution reason as the
            // navigate list above.
            Layout.fillHeight: switcher.commandMode
            clip: true
            model: commandRowModel
            currentIndex: -1
            keyNavigationEnabled: false
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: cmdRow
                width: ListView.view.width
                height: 44
                highlighted: ListView.isCurrentItem
                enabled: model.kind !== "action" || model.enabled !== false
                onClicked: switcher.activateCommandRow(index)
                Accessible.name: model.kind === "action"
                    ? (model.label + " " + (model.subtitle || ""))
                    : (model.name + " " + (model.subtitle || ""))
                Accessible.selected: cmdRow.highlighted

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
                        mxc: model.kind === "entity" ? model.avatarUrl : ""
                        name: model.kind === "entity" ? model.name : ""
                        colorKey: model.kind === "entity" ? model.roomId : ""
                        circle: model.kind === "entity" && model.category === "dm"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            objectName: "commandRowTitle"
                            Layout.fillWidth: true
                            // R2: the row's ink follows the selection state
                            // for BOTH kinds; only the MATCHED entity
                            // fragment carries the accent tint (SPEC 1k),
                            // via the same escape+highlight helper the
                            // navigate list uses.
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
                            font.pixelSize: AppTheme.fontResult
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: (model.subtitle || "").length > 0
                            text: model.subtitle || ""
                            // Identities and category words both ride the
                            // muted ink — faint is reserved for decorative
                            // mono headers (AA note in AppTheme).
                            color: AppTheme.stormTextMuted
                            // Same §0 rule as the navigate list: mono is
                            // reserved for identities (@/#/!), never the
                            // category words some entity rows carry.
                            font.family: /^[@#!]/.test(model.subtitle || "")
                                ? AppTheme.monoFont : AppTheme.uiFont
                            font.pixelSize: model.kind === "entity"
                                ? AppTheme.fontMonoXS : AppTheme.fontMonoSm
                            elide: Label.ElideRight
                        }
                    }
                    MenuKeycap {
                        visible: cmdRow.highlighted
                        iconName: "keyboard_return"
                        tinted: true
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        // ── Footer hint bar ────────────────────────────────────────────────
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
                        font.pixelSize: AppTheme.fontMonoSm
                        font.weight: Font.Bold
                    }
                    Label {
                        text: qsTr("navigate")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoSm
                    }
                }
                Row {
                    spacing: AppTheme.spacing4
                    Icon {
                        name: "keyboard_return"
                        size: AppTheme.fontMonoSm + 2
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        text: qsTr("open")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoSm
                    }
                }
                Row {
                    spacing: AppTheme.spacing4
                    Icon {
                        name: "keyboard_tab"
                        size: AppTheme.fontMonoSm + 2
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        text: qsTr("sections")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoSm
                    }
                }
                Row {
                    spacing: AppTheme.spacing4
                    Text {
                        text: "ESC"
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoSm
                        font.weight: Font.Bold
                    }
                    Label {
                        text: qsTr("dismiss")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoSm
                    }
                }
            }

            Label {
                id: commandFooter
                visible: switcher.commandMode
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                elide: Label.ElideRight
                text: qsTr("Try “theme indigo night” · “switch to alice” · “open privacy”")
                color: AppTheme.stormTextMuted
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontMonoSm
            }
        }
    }
}
