import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// The in-shell Settings pane (correction spec §3): it replaces ONLY the
// timeline region — MainScreen keeps the spaces rail and room list visible.
// A 60px header ("Settings — <section>" with the section icon in accent and
// a bare close X) sits above the 260px navigation column (Account,
// Appearance, Notifications, Privacy & security, Sessions, Labs; About
// pinned at the bottom) and the per-category panes. Appearance carries the
// three featured design theme cards with their fixed preview palettes, the
// match-system switch, the functional message-layout selector, and the text
// size slider. Every security control (SAS verification, recovery key,
// encrypted room-key import, local reset) is preserved under Privacy &
// security / Sessions, with the destructive reset in a separated Danger
// Zone. Sign out lives ONLY in the account menu.
//
// Category panes are toggled by visibility (never Loader), so in-flight
// verification/import state survives switching categories.
Item {
    id: root

    // v0.7.2: whether the sanitized E2EE recovery diagnostics are expanded.
    property bool showRecoveryDiagnostics: false

    // ── SPEC 1v: client-side settings search (no new C++ index) ───────────
    // Declarative {title, keywords, section, breadcrumb, control?} entries.
    // Indexed by SECTION KEY, not layout position — Privacy & security is
    // three non-contiguous ColumnLayout blocks in this file (search must
    // still resolve to the one "privacy" section). `control` names one of
    // the 5 settings picked for an inline live control in the results
    // panel below — each binds directly to the SAME SettingsManager
    // property its real control in the section pane uses, two-way, exactly
    // like that control.
    property string settingsSearchQuery: ""
    readonly property var searchIndex: [
        { title: qsTr("Account"), keywords: qsTr("account profile"),
          section: "account", breadcrumb: qsTr("Account") },
        { title: qsTr("Homeserver"), keywords: qsTr("homeserver server url"),
          section: "account", breadcrumb: qsTr("Account") },
        { title: qsTr("Start minimized"), keywords: qsTr("startup minimized"),
          section: "account", breadcrumb: qsTr("Account · Startup") },

        { title: qsTr("Theme"),
          keywords: qsTr("theme moss indigo teal light dark graphite midnight nordic purple warm"),
          section: "appearance", breadcrumb: qsTr("Appearance") },
        { title: qsTr("Match system light/dark"),
          keywords: qsTr("match system auto theme"), section: "appearance",
          breadcrumb: qsTr("Appearance · Theme"), control: "matchSystem" },
        { title: qsTr("Message layout"),
          keywords: qsTr("message layout modern bubbles compact"),
          section: "appearance", breadcrumb: qsTr("Appearance"),
          control: "messageLayout" },
        { title: qsTr("Text size"), keywords: qsTr("text size font scale"),
          section: "appearance", breadcrumb: qsTr("Appearance") },
        { title: qsTr("Font"), keywords: qsTr("font family typeface"),
          section: "appearance", breadcrumb: qsTr("Appearance") },
        { title: qsTr("Language"), keywords: qsTr("language locale"),
          section: "appearance", breadcrumb: qsTr("Appearance") },
        { title: qsTr("Show room activity"),
          keywords: qsTr("room activity membership joins leaves profile"),
          section: "appearance", breadcrumb: qsTr("Appearance · Timeline"),
          control: "showRoomActivity" },
        { title: qsTr("Mouse-wheel speed"),
          keywords: qsTr("wheel speed scroll timeline"), section: "appearance",
          breadcrumb: qsTr("Appearance · Timeline") },

        { title: qsTr("Desktop notifications"),
          keywords: qsTr("notifications desktop enable"),
          section: "notifications", breadcrumb: qsTr("Notifications"),
          control: "notificationsEnabled" },
        { title: qsTr("Notification preview"),
          keywords: qsTr("notification preview privacy sender message"),
          section: "notifications", breadcrumb: qsTr("Notifications") },
        { title: qsTr("Notification sound"),
          keywords: qsTr("notification sound mute"), section: "notifications",
          breadcrumb: qsTr("Notifications") },

        { title: qsTr("Automatically load previews in unencrypted rooms"),
          keywords: qsTr("link preview privacy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          control: "autoLoadLinkPreviews" },
        { title: qsTr("Load previews in encrypted rooms"),
          keywords: qsTr("link preview encrypted"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews") },
        { title: qsTr("Autoplay GIFs"), keywords: qsTr("gif autoplay"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · GIFs") },
        { title: qsTr("GIF safe search"),
          keywords: qsTr("gif safe search rating"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs") },
        { title: qsTr("Preferred GIF provider"),
          keywords: qsTr("gif provider giphy klipy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs") },
        { title: qsTr("Store recently used GIFs"),
          keywords: qsTr("gif recents store"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · GIFs") },
        { title: qsTr("Security status"),
          keywords: qsTr("e2ee encryption status cross-signing backup"),
          section: "privacy", breadcrumb: qsTr("Privacy & security") },
        { title: qsTr("Recovery key or passphrase"),
          keywords: qsTr("recovery key passphrase backup restore"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Recovery") },
        { title: qsTr("Import room keys"),
          keywords: qsTr("import room keys export"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery") },
        { title: qsTr("Danger Zone"),
          keywords: qsTr("reset danger local session"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Recovery") },

        { title: qsTr("Sessions"), keywords: qsTr("sessions devices"),
          section: "sessions", breadcrumb: qsTr("Sessions") },
        { title: qsTr("Current session"),
          keywords: qsTr("device id session status"), section: "sessions",
          breadcrumb: qsTr("Sessions") },
        { title: qsTr("Verify this session"),
          keywords: qsTr("verify verification sas cross-signing"),
          section: "sessions", breadcrumb: qsTr("Sessions") },

        { title: qsTr("Backend"), keywords: qsTr("backend rust http mock"),
          section: "labs", breadcrumb: qsTr("Labs") },
        { title: qsTr("Sync mode"), keywords: qsTr("sync sliding"),
          section: "labs", breadcrumb: qsTr("Labs") },
        { title: qsTr("Connection"), keywords: qsTr("connection status"),
          section: "labs", breadcrumb: qsTr("Labs") },
        { title: qsTr("Refresh current room"),
          keywords: qsTr("refresh reload timeline"), section: "labs",
          breadcrumb: qsTr("Labs") },

        { title: qsTr("About"), keywords: qsTr("about version license"),
          section: "about", breadcrumb: qsTr("About") },
    ]
    readonly property var matchedSearchResults: {
        var q = root.settingsSearchQuery.trim().toLowerCase()
        if (q.length === 0) return []
        return root.searchIndex.filter(function(e) {
            return (e.title + " " + e.keywords).toLowerCase().indexOf(q) !== -1
        })
    }
    readonly property var matchedSearchSections: {
        var s = {}
        for (var i = 0; i < root.matchedSearchResults.length; ++i)
            s[root.matchedSearchResults[i].section] = true
        return s
    }
    function escapeHtml(s) {
        return String(s)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function highlightedTitle(title, query) {
        var safe = escapeHtml(title)
        var q = (query || "").trim()
        if (q.length === 0) return safe
        var lowerSafe = safe.toLowerCase()
        var lowerQ = escapeHtml(q).toLowerCase()
        var idx = lowerSafe.indexOf(lowerQ)
        if (idx === -1) return safe
        return safe.slice(0, idx) + "<font color=\"" + AppTheme.bolt + "\">"
             + safe.slice(idx, idx + lowerQ.length) + "</font>"
             + safe.slice(idx + lowerQ.length)
    }

    // Reusable settings card (grouped-controls surface).
    component SettingsCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormCanvas
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    // Storm §4 2f nav row: 32px, radiusTile; the active row fills
    // stormSelection, brightens icon (bolt) and label (stormText), and
    // carries the signature edge-bolt caret overhanging its left edge.
    component SettingsNavRow: ItemDelegate {
        id: navRow
        property string sectionKey: ""
        property string iconName: ""
        property string navLabel: ""
        objectName: "settingsNavRow_" + sectionKey
        Layout.fillWidth: true
        implicitHeight: 32
        // Constant content inset clearing the caret gutter (§3.2 — never
        // active-only, so rows don't shift as the selection moves).
        leftPadding: padding + 4
        // v0.6.5 live-feedback: the Basic-style ItemDelegate default
        // (padding: 12) survives even though implicitHeight is forced to
        // 32, leaving contentItem only 8px of availableHeight — nowhere
        // near enough for the icon/label content, so cross-axis centering
        // (even with Layout.alignment set, above) clamps against that
        // undersized box instead of the row's real bounds. Same fix as
        // AppMenuItem.qml's topPadding/bottomPadding: 0 for the identical
        // forced-implicitHeight shape — give contentItem the full row.
        topPadding: 0
        bottomPadding: 0
        // SPEC 1v: typing in the search field narrows the nav to sections
        // with at least one matching result.
        visible: root.settingsSearchQuery.trim().length === 0
                 || root.matchedSearchSections[sectionKey] === true
        highlighted: root.section === sectionKey
        Accessible.name: navLabel
        Accessible.selected: navRow.highlighted
        onClicked: root.section = sectionKey
        contentItem: RowLayout {
            spacing: AppTheme.spacing8
            // v0.6.5 live-feedback: on a real desktop the icon and label
            // read as vertically offset from each other (DPR-dependent —
            // worse at fractional scale factors). Root cause: Icon is a
            // bare Text glyph with no explicit height, so its
            // implicitHeight comes from the ICON FONT's own ascent/
            // descent metrics; navLabel's implicitHeight comes from the UI
            // text font's own, very differently-proportioned metrics.
            // RowLayout centers each child's bounding box independently,
            // so two boxes of different height and different internal
            // ink-to-box-center offset land their VISIBLE glyphs at
            // slightly different y — a sub-pixel gap that rounds/hints
            // differently (and becomes visible) at different DPRs. Pinning
            // the icon's Layout.preferredHeight to the label's own
            // implicitHeight makes both boxes IDENTICAL, so there is
            // nothing left for cross-axis centering to disagree about.
            Icon {
                name: navRow.iconName
                size: 16
                Layout.alignment: Qt.AlignVCenter
                // (id navRowText, NOT navLabel — that name is the row's
                // string property, and an id here would shadow it, feeding
                // the Label OBJECT into Accessible.name above.)
                Layout.preferredHeight: navRowText.implicitHeight
                color: navRow.highlighted ? AppTheme.bolt
                                          : AppTheme.stormTextMuted
            }
            Label {
                id: navRowText
                text: navRow.navLabel
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                color: navRow.highlighted ? AppTheme.stormText
                                          : AppTheme.stormTextSecondary
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.fontSecondary
                font.weight: navRow.highlighted ? Font.Bold : Font.DemiBold
                elide: Label.ElideRight
            }
        }
        background: Rectangle {
            radius: AppTheme.radiusTile
            color: navRow.highlighted ? AppTheme.stormSelection
                 : navRow.hovered ? Qt.alpha(AppTheme.stormSelection, 0.55)
                 : "transparent"
            Icon {
                visible: navRow.highlighted
                name: "bolt"
                size: 11
                color: AppTheme.bolt
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: -2
            }
        }
    }

    // Design-1d sections: "account" | "appearance" | "notifications"
    // | "privacy" | "sessions" | "labs" | "about". The design opens on
    // Appearance; deep links still land on their own section.
    property string section: "appearance"

    function sectionTitle(key) {
        if (key === "account") return qsTr("Account")
        if (key === "appearance") return qsTr("Appearance")
        if (key === "notifications") return qsTr("Notifications")
        if (key === "privacy") return qsTr("Privacy & security")
        if (key === "sessions") return qsTr("Sessions")
        if (key === "labs") return qsTr("Labs")
        if (key === "about") return qsTr("About")
        return key
    }
    function sectionIcon(key) {
        if (key === "account") return "account_circle"
        if (key === "appearance") return "palette"
        if (key === "notifications") return "notifications"
        if (key === "privacy") return "verified_user"
        if (key === "sessions") return "devices"
        if (key === "labs") return "science"
        if (key === "about") return "info"
        return "settings"
    }

    // Deep links from older code paths (message rows jump to "security",
    // etc.) keep working through this mapping.
    function mapLegacySection(key) {
        if (key === "general") return "appearance"
        if (key === "security") return "privacy"
        if (key === "advanced") return "labs"
        return key
    }

    Component.onCompleted: {
        var requested = app.takeRequestedSettingsSection()
        if (requested.length > 0)
            section = mapLegacySection(requested)
    }

    function goBack() {
        app.loggedIn ? app.showMain() : app.showLogin()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: root.goBack()
    }
    // SPEC 1v: Ctrl+, focuses the settings search field. Scoped to this Item
    // (a Loader-hosted view — see settingsViewLoader), so it only exists
    // while Settings is actually open.
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsSearchField.forceActiveFocus()
    }

    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op. demoOpenTrustCard needs no handler
    // here — sessionsTrustCard (SPEC 1r) already renders whenever section
    // is "sessions", which activateScenario's page navigation sets up on
    // its own; see docs/screenshot-demo.md for the mock-backend crypto-
    // gating caveat.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoFocusSettingsSearch(query) {
            settingsSearchField.text = query
            settingsSearchField.forceActiveFocus()
        }
    }

    // Confirmation before clearing local GIF collections (Favorites are the
    // destructive one; Recents too for parity).
    Dialog {
        id: gifClearConfirm
        property string kind: ""
        function open(k) { kind = k; title = k === "favorites"
            ? qsTr("Clear GIF favorites?") : qsTr("Clear recent GIFs?"); visible = true }
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: 280
            wrapMode: Text.WordWrap
            text: gifClearConfirm.kind === "favorites"
                ? qsTr("Remove all saved GIF favorites from this device? This "
                       + "cannot be undone.")
                : qsTr("Clear the list of recently used GIFs on this device?")
        }
        onAccepted: {
            if (kind === "favorites") app.gif.favorites.clearAll()
            else if (kind === "recent") app.gif.recent.clearAll()
        }
    }

    Rectangle { anchors.fill: parent; color: AppTheme.stormDeep }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Settings header (SPEC 1v: 44px title-bar treatment) — section
        // icon in accent, "Settings — <section>", bare close X ────────────
        Rectangle {
            objectName: "settingsHeaderBar"
            Layout.fillWidth: true
            implicitHeight: 44
            color: AppTheme.stormDeep
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing24
                anchors.rightMargin: AppTheme.spacing24
                spacing: AppTheme.spacing8 + 2
                Icon {
                    name: root.sectionIcon(root.section)
                    size: 22
                    color: AppTheme.bolt
                }
                Label {
                    objectName: "settingsHeaderTitle"
                    text: qsTr("Settings — %1").arg(root.sectionTitle(root.section))
                    color: AppTheme.stormText
                    font.family: AppTheme.menuFont
                    font.pixelSize: 15
                    font.weight: Font.ExtraBold
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                IconButton {
                    storm: true
                    objectName: "settingsCloseButton"
                    iconName: "close"
                    iconSize: 20
                    Accessible.name: qsTr("Close settings")
                    ToolTip.text: qsTr("Close settings")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.goBack()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.stormBorder }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Left navigation (design 1d: 260 px, Settings title, icon
            // rows, About pinned at the bottom) ──────────────────────────
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 260
                Layout.minimumWidth: 200
                color: AppTheme.stormCanvas
                // Structural containment: nothing hosted in this column
                // (search results, inline controls) may ever paint across
                // the divider into the content pane, whatever its width.
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: 2

                    // Storm §4 2f pane title: filled-look bolt + Space
                    // Grotesk 16-17/700, tight 12px gap to the search field.
                    RowLayout {
                        spacing: AppTheme.spacing8
                        Layout.leftMargin: AppTheme.spacing8
                        Layout.topMargin: AppTheme.spacing4
                        Layout.bottomMargin: AppTheme.spacing4
                        Icon {
                            name: "bolt"
                            size: 15
                            color: AppTheme.bolt
                        }
                        Label {
                            text: qsTr("Settings")
                            color: AppTheme.stormText
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.fontNavTitle
                            font.weight: Font.Bold
                        }
                    }

                    // SPEC 1v: search field directly under the title, with a
                    // trailing Ctrl+, keycap.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.bottomMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing6
                        AppTextField {
                            id: settingsSearchField
                            objectName: "settingsSearchField"
                            Layout.fillWidth: true
                            searchIcon: true
                            clearButton: true
                            storm: true
                            placeholderText: qsTr("Search settings…")
                            Accessible.name: qsTr("Search settings")
                            onTextChanged: root.settingsSearchQuery = text
                            onAccepted: {
                                if (root.matchedSearchResults.length > 0)
                                    root.section = root.matchedSearchResults[0].section
                            }
                        }
                        MenuKeycap { keys: "Ctrl+," }
                    }

                    // ── Search results panel (replaces the nav list while
                    // searching; SPEC 1v — no "Browse all" affordance). ────
                    ColumnLayout {
                        objectName: "settingsSearchResults"
                        visible: root.settingsSearchQuery.trim().length > 0
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing4

                        Label {
                            objectName: "settingsSearchNoResults"
                            visible: root.matchedSearchResults.length === 0
                            Layout.fillWidth: true
                            text: qsTr("No matching settings")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontSizeXS
                        }

                        Repeater {
                            model: root.matchedSearchResults
                            delegate: Rectangle {
                                id: resultRow
                                required property var modelData
                                required property int index
                                objectName: "settingsSearchResult_" + index
                                Layout.fillWidth: true
                                radius: AppTheme.radiusLg
                                color: resultHover.hovered ? AppTheme.stormSelection : "transparent"
                                implicitHeight: resultContent.implicitHeight
                                                + AppTheme.spacing8

                                RowLayout {
                                    id: resultContent
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.margins: AppTheme.spacing8
                                    spacing: AppTheme.spacing8

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            Layout.fillWidth: true
                                            textFormat: Text.StyledText
                                            text: root.highlightedTitle(
                                                resultRow.modelData.title,
                                                root.settingsSearchQuery)
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.fontSecondary
                                            font.weight: Font.DemiBold
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: resultRow.modelData.breadcrumb
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.fontMonoSm
                                            elide: Label.ElideRight
                                        }
                                        // A click anywhere in this text
                                        // column navigates — scoped away
                                        // from the inline controls below so
                                        // the two never fight for the tap.
                                        TapHandler {
                                            onTapped: root.section = resultRow.modelData.section
                                        }

                                        // The three-segment layout control is
                                        // wider than the 220px the nav column
                                        // leaves beside the text — inline it
                                        // BELOW the title as a dense row
                                        // instead of letting it paint across
                                        // the nav divider into the pane.
                                        SegmentedControl {
                                            storm: true
                                            objectName: "settingsSearchInlineMessageLayout_" + resultRow.index
                                            visible: resultRow.modelData.control === "messageLayout"
                                            dense: true
                                            Layout.topMargin: AppTheme.spacing4
                                            model: [
                                                { label: qsTr("Modern"), value: 0 },
                                                { label: qsTr("Bubbles"), value: 1 },
                                                { label: qsTr("Compact"), value: 2 },
                                            ]
                                            current: app.settings.messageLayout
                                            onActivated: (value) =>
                                                app.settings.messageLayout = value
                                        }
                                    }

                                    // ── Inline live controls (SPEC 1v):
                                    // exactly 5 entries, each bound two-way
                                    // to the SAME SettingsManager property
                                    // its real section control uses. ──────
                                    AppSwitch {
                                        objectName: "settingsSearchInlineMatchSystem_" + resultRow.index
                                        visible: resultRow.modelData.control === "matchSystem"
                                        checked: app.settings.theme === 0
                                        Accessible.name: qsTr("Match system light/dark")
                                        onToggled: app.settings.theme =
                                            app.settings.theme === 0
                                                ? AppTheme.effectiveTheme : 0
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineShowRoomActivity_" + resultRow.index
                                        visible: resultRow.modelData.control === "showRoomActivity"
                                        checked: app.settings.showRoomActivity
                                        Accessible.name: qsTr("Show room activity")
                                        onToggled: app.settings.showRoomActivity =
                                            !app.settings.showRoomActivity
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineAutoLoadPreviews_" + resultRow.index
                                        visible: resultRow.modelData.control === "autoLoadLinkPreviews"
                                        checked: app.settings.autoLoadLinkPreviews
                                        Accessible.name: qsTr(
                                            "Automatically load previews in unencrypted rooms")
                                        onToggled: app.settings.autoLoadLinkPreviews =
                                            !app.settings.autoLoadLinkPreviews
                                    }
                                    AppSwitch {
                                        objectName: "settingsSearchInlineNotificationsEnabled_" + resultRow.index
                                        visible: resultRow.modelData.control === "notificationsEnabled"
                                        checked: app.settings.notificationsEnabled
                                        Accessible.name: qsTr("Desktop notifications")
                                        onToggled: app.settings.notificationsEnabled =
                                            !app.settings.notificationsEnabled
                                    }
                                }
                                HoverHandler { id: resultHover }
                                Accessible.role: Accessible.Button
                                Accessible.name: resultRow.modelData.title + " "
                                    + resultRow.modelData.breadcrumb
                            }
                        }

                        // Quick-filter chips seeding example queries.
                        Flow {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing4
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: [
                                    qsTr("theme"), qsTr("notifications"),
                                    qsTr("privacy"), qsTr("sessions"),
                                ]
                                delegate: Rectangle {
                                    id: quickChip
                                    required property string modelData
                                    radius: AppTheme.radiusPill
                                    color: quickChipHover.hovered
                                           ? AppTheme.stormSelection : AppTheme.stormInset
                                    implicitWidth: quickChipLabel.implicitWidth
                                                   + AppTheme.spacing12
                                    implicitHeight: quickChipLabel.implicitHeight
                                                    + AppTheme.spacing6
                                    Label {
                                        id: quickChipLabel
                                        anchors.centerIn: parent
                                        text: quickChip.modelData
                                        font.family: AppTheme.monoFont
                                        font.pixelSize: AppTheme.fontChip
                                        color: AppTheme.stormTextSecondary
                                    }
                                    HoverHandler { id: quickChipHover }
                                    TapHandler {
                                        onTapped: {
                                            settingsSearchField.text = quickChip.modelData
                                            settingsSearchField.forceActiveFocus()
                                        }
                                    }
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("Search for %1")
                                        .arg(quickChip.modelData)
                                }
                            }
                        }
                    }

                    SettingsNavRow {
                        sectionKey: "account"
                        iconName: "account_circle"
                        navLabel: qsTr("Account")
                    }
                    SettingsNavRow {
                        sectionKey: "appearance"
                        iconName: "palette"
                        navLabel: qsTr("Appearance")
                    }
                    SettingsNavRow {
                        sectionKey: "notifications"
                        iconName: "notifications"
                        navLabel: qsTr("Notifications")
                    }
                    SettingsNavRow {
                        sectionKey: "privacy"
                        iconName: "verified_user"
                        navLabel: qsTr("Privacy & security")
                    }
                    SettingsNavRow {
                        sectionKey: "sessions"
                        iconName: "devices"
                        navLabel: qsTr("Sessions")
                    }
                    SettingsNavRow {
                        sectionKey: "labs"
                        iconName: "science"
                        navLabel: qsTr("Labs")
                    }
                    Item { Layout.fillHeight: true }

                    // Storm §4 2f: mono match counter pinned at the nav
                    // bottom while a search narrows the rows.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        visible: root.settingsSearchQuery.trim().length > 0
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: AppTheme.stormBorder
                        }
                        Label {
                            objectName: "settingsSearchMatchCounter"
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing8
                            Layout.bottomMargin: AppTheme.spacing4
                            Layout.leftMargin: AppTheme.spacing8
                            text: {
                                var sections = 0
                                for (var k in root.matchedSearchSections) {
                                    if (root.matchedSearchSections[k] === true)
                                        sections++
                                }
                                // Mock-style always-plural mono counter.
                                return qsTr("Matches · %1 sections · %2 settings")
                                       .arg(sections)
                                       .arg(root.matchedSearchResults.length)
                            }
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontMicro
                            font.weight: Font.Medium
                            font.letterSpacing: 1.0
                            font.capitalization: Font.AllUppercase
                            color: AppTheme.stormTextFaint
                            elide: Label.ElideRight
                        }
                    }

                    SettingsNavRow {
                        sectionKey: "about"
                        iconName: "info"
                        navLabel: qsTr("About")
                    }
                }
            }
            Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: AppTheme.stormBorder }

            // ── Right content pane ───────────────────────────────────────
            Flickable {
                id: contentFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentHeight: contentColumn.implicitHeight + AppTheme.spacing24 * 2
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // Jump to the top when switching categories.
                Connections {
                    target: root
                    function onSectionChanged() { contentFlick.contentY = 0 }
                }

                ColumnLayout {
                    id: contentColumn
                    x: AppTheme.spacing24
                    y: AppTheme.spacing24
                    width: Math.min(860, contentFlick.width - AppTheme.spacing24 * 2)
                    spacing: AppTheme.spacing16

                    // ════════════ Appearance (design 1d) ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Appearance")
                            color: AppTheme.stormText
                            font.pixelSize: 19
                            font.weight: Font.ExtraBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("Theme, message layout and text size — per account.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.topMargin: AppTheme.spacing8
                            text: qsTr("THEME")
                            color: AppTheme.stormTextFaint
                            font.pixelSize: AppTheme.fontChip
                            font.family: AppTheme.monoFont
                            font.weight: Font.DemiBold
                            font.letterSpacing: AppTheme.trackingStorm
                        }
                        // Four featured design themes with FIXED preview
                        // palettes (the one place the design allows
                        // hard-coded colors — each card always paints its
                        // own theme regardless of the active one). Storm is
                        // the 0.6.5 brand theme and sorts first/primary; its
                        // preview swatches are NOT new fixed hex literals —
                        // they read from AppTheme.paletteForTheme(11) (via
                        // stormPreview below) exactly like the mini theme
                        // cards already do, so the card can never drift from
                        // the real Storm palette values AppTheme.qml owns.
                        Flow {
                            id: featuredThemeFlow
                            objectName: "featuredThemeFlow"
                            Layout.fillWidth: true
                            spacing: 14
                            readonly property var stormPreview:
                                AppTheme.paletteForTheme(11)
                            Repeater {
                                model: [
                                    { id: 11, name: qsTr("Storm"),
                                      frame: featuredThemeFlow.stormPreview.background,
                                      rail: featuredThemeFlow.stormPreview.sidebar,
                                      bar1: featuredThemeFlow.stormPreview.border,
                                      bar2: featuredThemeFlow.stormPreview.surface,
                                      accent: featuredThemeFlow.stormPreview.accent },
                                    { id: 8,  name: qsTr("Moss Light"),
                                      frame: "#f7f7f5", rail: "#eceded",
                                      bar1: "#dcdedc", bar2: "#e6e8e6",
                                      accent: "#12a67f" },
                                    { id: 9,  name: qsTr("Indigo Night"),
                                      frame: "#101016", rail: "#1d1d26",
                                      bar1: "#2a2a36", bar2: "#23232d",
                                      accent: "#7c7ff2" },
                                    { id: 10, name: qsTr("Deep Teal"),
                                      frame: "#0e1416", rail: "#182428",
                                      bar1: "#1d2b30", bar2: "#152023",
                                      accent: "#27c2ad" },
                                ]
                                delegate: Rectangle {
                                    id: themeCard
                                    required property var modelData
                                    objectName: "featuredThemeCard_" + modelData.id
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    // SPEC 1v: three 150px preview cards.
                                    implicitWidth: 150
                                    // Integral height keeps the card edge on
                                    // device pixels (fractional text metrics
                                    // otherwise bleed one-device-pixel ring
                                    // slivers under fractional scaling).
                                    implicitHeight: previewTop.height
                                                    + Math.ceil(cardFoot.height)
                                    radius: 12
                                    // NO clip here: the selection glow and
                                    // focus ring are drawn OUTSIDE the card
                                    // (negative margins below). Item.clip is a
                                    // rectangular scissor, so it cannot round
                                    // the preview's corners anyway — all it
                                    // did was shave the rings to corner
                                    // crescents and a protruding edge sliver.
                                    color: AppTheme.stormCanvas
                                    // The outline is drawn as an overlay
                                    // sibling BELOW (z above the children):
                                    // previewTop/cardFoot fill to the edges
                                    // and would occlude a border painted on
                                    // this base rectangle.
                                    border.width: 0
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: modelData.name
                                    Accessible.focusable: true
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: app.settings.theme = modelData.id
                                    Keys.onSpacePressed: app.settings.theme = modelData.id

                                    // Selected affordance (R9): 3px glow of
                                    // the accent at 18% alpha — theme-derived,
                                    // not the accentSoft surface token.
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        radius: parent.radius + 3
                                        z: -1
                                        visible: themeCard.selectedTheme
                                        color: "transparent"
                                        border.width: 3
                                        border.color: Qt.alpha(AppTheme.bolt,
                                                               0.18)
                                    }
                                    // Keyboard focus ring (shared treatment).
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -6
                                        radius: parent.radius + 6
                                        z: -1
                                        visible: themeCard.activeFocus
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                    }

                                    // Preview top: 96px painted in the
                                    // previewed theme's exact colors.
                                    Rectangle {
                                        id: previewTop
                                        objectName: "themeCardPreview_" + themeCard.modelData.id
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: 96
                                        color: themeCard.modelData.frame
                                        // Follow the card's rounded top; the
                                        // old rectangular clip never did this
                                        // — the preview overdrew the corner
                                        // arcs squarely.
                                        topLeftRadius: 11
                                        topRightRadius: 11
                                        // 10px padding, 26px mini rail, three
                                        // rounded bars at 70/50/60% width —
                                        // the last in the theme's accent.
                                        Rectangle {
                                            x: 10; y: 10
                                            width: 26
                                            height: parent.height - 20
                                            radius: 6
                                            color: themeCard.modelData.rail
                                        }
                                        Column {
                                            id: previewBars
                                            x: 10 + 26 + 6
                                            y: 10
                                            spacing: 6
                                            readonly property real barSpan:
                                                previewTop.width - x - 10
                                            Rectangle {
                                                width: previewBars.barSpan * 0.7
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.bar1
                                            }
                                            Rectangle {
                                                width: previewBars.barSpan * 0.5
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.bar2
                                            }
                                            Rectangle {
                                                objectName: "themeCardAccentBar_" + themeCard.modelData.id
                                                width: previewBars.barSpan * 0.6
                                                height: 8
                                                radius: 4
                                                color: themeCard.modelData.accent
                                            }
                                        }
                                    }

                                    // Card bottom: raised background, radio,
                                    // theme name.
                                    Rectangle {
                                        id: cardFoot
                                        anchors.top: previewTop.bottom
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: footRow.implicitHeight + 20
                                        color: AppTheme.stormCanvas
                                        // Follow the card's rounded bottom,
                                        // mirroring previewTop's top arcs.
                                        bottomLeftRadius: 11
                                        bottomRightRadius: 11
                                        RowLayout {
                                            id: footRow
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            spacing: 8
                                            Rectangle {
                                                implicitWidth: 14
                                                implicitHeight: 14
                                                radius: 7
                                                color: themeCard.selectedTheme
                                                       ? AppTheme.bolt : "transparent"
                                                border.width: 2
                                                border.color: themeCard.selectedTheme
                                                              ? AppTheme.bolt
                                                              : AppTheme.stormTextFaint
                                                Rectangle {
                                                    anchors.centerIn: parent
                                                    width: 5; height: 5; radius: 2.5
                                                    visible: themeCard.selectedTheme
                                                    color: AppTheme.stormText
                                                }
                                            }
                                            Label {
                                                text: themeCard.modelData.name
                                                color: AppTheme.stormText
                                                font.pixelSize: 13
                                                font.weight: Font.Bold
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }

                                    // The card outline, above the edge-
                                    // filling children so it always renders
                                    // (SPEC 1v: 1.5px accent when selected).
                                    Rectangle {
                                        anchors.fill: parent
                                        z: 5
                                        radius: 12
                                        color: "transparent"
                                        border.width: 1.5
                                        border.color: themeCard.selectedTheme
                                                      ? AppTheme.bolt
                                                      : AppTheme.stormBorder
                                    }

                                    TapHandler {
                                        onTapped: app.settings.theme =
                                            themeCard.modelData.id
                                    }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                }
                            }
                        }

                        // Secondary access to the remaining presets — a
                        // compact row that never disturbs the featured
                        // composition above.
                        Label {
                            Layout.topMargin: AppTheme.spacing8
                            text: qsTr("MORE THEMES")
                            color: AppTheme.stormTextFaint
                            font.pixelSize: AppTheme.fontChip
                            font.family: AppTheme.monoFont
                            font.weight: Font.DemiBold
                            font.letterSpacing: AppTheme.trackingStorm
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Repeater {
                                model: AppTheme.themeList.filter(
                                    (t) => t.id !== 8 && t.id !== 9 && t.id !== 10
                                           && t.id !== 11)
                                delegate: Rectangle {
                                    id: miniThemeCard
                                    required property var modelData
                                    objectName: "miniThemeCard_" + modelData.id
                                    readonly property var pal:
                                        AppTheme.paletteForTheme(modelData.id)
                                    readonly property bool selectedTheme:
                                        app.settings.theme === modelData.id
                                    implicitWidth: miniRow.implicitWidth + 24
                                    implicitHeight: 34
                                    radius: 9
                                    color: selectedTheme ? AppTheme.stormSelection
                                           : miniHover.hovered ? AppTheme.stormSelection
                                                               : AppTheme.stormInset
                                    border.width: selectedTheme ? 1.5 : 0
                                    border.color: AppTheme.bolt
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: modelData.name
                                    Accessible.focusable: true
                                    activeFocusOnTab: true
                                    Keys.onReturnPressed: app.settings.theme = modelData.id
                                    Keys.onSpacePressed: app.settings.theme = modelData.id
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -3
                                        radius: parent.radius + 3
                                        visible: miniThemeCard.activeFocus
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                    }
                                    RowLayout {
                                        id: miniRow
                                        anchors.centerIn: parent
                                        spacing: 6
                                        Rectangle {
                                            implicitWidth: 12; implicitHeight: 12
                                            radius: 4
                                            color: miniThemeCard.pal.background
                                            border.color: miniThemeCard.pal.border
                                            Rectangle {
                                                anchors.right: parent.right
                                                anchors.bottom: parent.bottom
                                                width: 5; height: 5; radius: 2
                                                color: miniThemeCard.pal.accent
                                            }
                                        }
                                        Label {
                                            text: miniThemeCard.modelData.name
                                            color: miniThemeCard.selectedTheme
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextSecondary
                                            font.pixelSize: 12
                                            font.weight: Font.Bold
                                        }
                                    }
                                    TapHandler {
                                        onTapped: app.settings.theme =
                                            miniThemeCard.modelData.id
                                    }
                                    HoverHandler {
                                        id: miniHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                }
                            }
                        }

                        // Match-system row (spec: 36×20 switch, 16px white
                        // thumb, 150ms travel; the WHOLE row is clickable).
                        AbstractButton {
                            id: matchSystemSwitch
                            objectName: "matchSystemSwitch"
                            Layout.topMargin: AppTheme.spacing8
                            implicitWidth: matchRow.implicitWidth
                            implicitHeight: 24
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            checkable: true
                            checked: app.settings.theme === 0
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: qsTr("Match system light/dark")
                            onClicked: app.settings.theme =
                                app.settings.theme === 0 ? AppTheme.effectiveTheme : 0
                            contentItem: RowLayout {
                                id: matchRow
                                spacing: 10
                                Rectangle {
                                    objectName: "matchSystemTrack"
                                    implicitWidth: 36
                                    implicitHeight: 20
                                    radius: 99
                                    color: app.settings.theme === 0
                                           ? AppTheme.bolt : AppTheme.stormTextFaint
                                    Rectangle {
                                        width: 16; height: 16; radius: 8
                                        // v0.6.5 live-feedback: the checked
                                        // track fills AppTheme.bolt — under
                                        // Storm that's the literal bolt
                                        // yellow, and a white thumb on it is
                                        // illegible. boltInk is the ink
                                        // that's DESIGNED to sit on a bolt
                                        // fill (navy under Storm, accentText
                                        // under legacy — which is white for
                                        // every legacy theme except Deep
                                        // Teal, so this is a no-op change
                                        // for legacy themes other than that
                                        // one, where it's a latent-bug fix
                                        // too). The unchecked track never
                                        // carries bolt, so its thumb keeps
                                        // the plain white literal.
                                        color: app.settings.theme === 0
                                               ? AppTheme.boltInk : "#FFFFFF"
                                        y: 2
                                        x: app.settings.theme === 0 ? 18 : 2
                                        Behavior on x {
                                            NumberAnimation { duration: 150 }
                                        }
                                    }
                                }
                                Label {
                                    text: qsTr("Match system light/dark")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: 13
                                }
                            }
                            background: Item {}
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -4
                                radius: 8
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.bolt
                                visible: matchSystemSwitch.visualFocus
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontCaption
                            text: qsTr("When on, Lightning follows the system scheme: "
                                       + "Moss Light in light mode, Storm in dark mode.")
                        }

                        Label {
                            Layout.topMargin: AppTheme.spacing8
                            text: qsTr("MESSAGE LAYOUT")
                            color: AppTheme.stormTextFaint
                            font.pixelSize: AppTheme.fontChip
                            font.family: AppTheme.monoFont
                            font.weight: Font.DemiBold
                            font.letterSpacing: AppTheme.trackingStorm
                        }
                        SegmentedControl {
                            storm: true
                            objectName: "messageLayoutControl"
                            model: [
                                { label: qsTr("Modern"), value: 0 },
                                { label: qsTr("Bubbles"), value: 1 },
                                { label: qsTr("Compact"), value: 2 },
                            ]
                            current: app.settings.messageLayout
                            onActivated: (value) =>
                                app.settings.messageLayout = value
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontCaption
                            text: qsTr("Bubbles applies to direct messages; rooms keep "
                                       + "the Modern rows. Compact tightens every timeline.")
                        }

                        Label {
                            Layout.topMargin: AppTheme.spacing8
                            text: qsTr("TEXT SIZE")
                            color: AppTheme.stormTextFaint
                            font.pixelSize: AppTheme.fontChip
                            font.family: AppTheme.monoFont
                            font.weight: Font.DemiBold
                            font.letterSpacing: AppTheme.trackingStorm
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 12
                            }
                            Slider {
                                id: textScaleSlider
                                objectName: "textScaleSlider"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 90
                                to: 140
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.textScale
                                onMoved: app.settings.textScale = Math.round(value)
                                Accessible.name: qsTr("Message text size")
                                background: Rectangle {
                                    x: textScaleSlider.leftPadding
                                    y: textScaleSlider.topPadding
                                       + textScaleSlider.availableHeight / 2 - 2
                                    width: textScaleSlider.availableWidth
                                    height: 4
                                    radius: 99
                                    color: AppTheme.stormInset
                                    Rectangle {
                                        width: textScaleSlider.visualPosition
                                               * parent.width
                                        height: parent.height
                                        radius: 99
                                        color: AppTheme.bolt
                                    }
                                }
                                handle: Rectangle {
                                    x: textScaleSlider.leftPadding
                                       + textScaleSlider.visualPosition
                                         * (textScaleSlider.availableWidth - width)
                                    y: textScaleSlider.topPadding
                                       + textScaleSlider.availableHeight / 2
                                       - height / 2
                                    width: 16; height: 16; radius: 8
                                    // v0.6.5 live-feedback: unlike the
                                    // switch thumb, this one sits ON the
                                    // groove, not on a single checked/
                                    // unchecked fill — so which ink it
                                    // needs depends on where the fill's
                                    // right edge (visualPosition *
                                    // availableWidth, background above)
                                    // actually lands relative to the
                                    // thumb's own centre (leftPadding +
                                    // visualPosition * (availableWidth -
                                    // width) + width/2). Solving fill-edge
                                    // > thumb-centre for these two
                                    // expressions reduces to
                                    // visualPosition > 0.5 exactly — below
                                    // half the range the thumb sits mostly
                                    // on the unfilled stormInset groove
                                    // (white reads fine there); above half
                                    // it sits mostly on the bolt fill and
                                    // needs boltInk for the same reason as
                                    // the switch thumb above.
                                    color: textScaleSlider.visualPosition > 0.5
                                           ? AppTheme.boltInk : "#FFFFFF"
                                    // The slider thumb's shadow is one of the
                                    // four the design budget allows.
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.topMargin: 1
                                        anchors.bottomMargin: -1
                                        radius: 8
                                        z: -1
                                        color: "#40000000"
                                    }
                                    border.width: textScaleSlider.visualFocus ? 2 : 0
                                    border.color: AppTheme.bolt
                                }
                            }
                            Label {
                                text: "A"
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 18
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontCaption
                            text: qsTr("Scales message and list text. Interface chrome "
                                       + "and icons keep their size.")
                        }

                        // ── v0.7: UI font (bundled OFL families) ────────
                        Label {
                            Layout.topMargin: AppTheme.spacing8
                            text: qsTr("FONT")
                            color: AppTheme.stormTextFaint
                            font.pixelSize: AppTheme.fontChip
                            font.family: AppTheme.monoFont
                            font.weight: Font.DemiBold
                            font.letterSpacing: AppTheme.trackingStorm
                        }
                        ColumnLayout {
                            objectName: "uiFontSelector"
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing4
                            Repeater {
                                model: app.settings.uiFontChoices()
                                ItemDelegate {
                                    id: fontRow
                                    required property string modelData
                                    readonly property bool selected:
                                        app.settings.uiFont === modelData
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 420
                                    implicitHeight: 56
                                    // Same forced-implicitHeight squeeze as
                                    // SettingsNavRow above (Basic-style
                                    // ItemDelegate padding: 12 survives a
                                    // taller forced row too) — AppMenuItem's
                                    // topPadding/bottomPadding: 0 pattern.
                                    topPadding: 0
                                    bottomPadding: 0
                                    Accessible.name:
                                        qsTr("Use the %1 font").arg(modelData)
                                    onClicked:
                                        app.settings.uiFont = modelData
                                    background: Rectangle {
                                        radius: AppTheme.radiusMd
                                        color: fontRow.selected
                                               ? AppTheme.stormSelection
                                               : fontRow.hovered
                                                 ? AppTheme.stormSelection
                                                 : AppTheme.stormInset
                                        border.width: 1
                                        border.color: fontRow.selected
                                                      ? AppTheme.stormBorderStrong
                                                      : fontRow.visualFocus
                                                        ? AppTheme.bolt
                                                        : AppTheme.stormBorder
                                    }
                                    contentItem: RowLayout {
                                        spacing: AppTheme.spacing12
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Label {
                                                text: fontRow.modelData
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.fontBody
                                                font.weight: Font.DemiBold
                                                color: AppTheme.stormText
                                            }
                                            // The sample previews the actual
                                            // family being offered.
                                            Label {
                                                text: qsTr("Messages, rooms and settings")
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.fontSecondary
                                                color: AppTheme.stormTextMuted
                                            }
                                        }
                                        Icon {
                                            visible: fontRow.selected
                                            name: "check"
                                            size: 16
                                            color: AppTheme.bolt
                                        }
                                    }
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontCaption
                            text: qsTr("Applies to the whole interface. Code, Matrix "
                                       + "IDs, icons, and emoji keep their own fonts.")
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Timeline")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showRoomActivityCheck"
                                    text: qsTr("Show room activity")
                                    checked: app.settings.showRoomActivity
                                    onToggled: app.settings.showRoomActivity = checked
                                    Accessible.description: qsTr(
                                        "Show membership, profile, and room setting updates in timelines")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Hide routine joins, leaves, profile changes, "
                                               + "and room setting updates. Messages and "
                                               + "decryption warnings remain visible.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Mouse-wheel speed")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                }
                                AppComboBox {
                                    storm: true
                                    id: wheelSpeedCombo
                                    objectName: "timelineWheelSpeedCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"
                                    valueRole: "value"
                                    // Values map to TimelineScrollController::WheelSpeed.
                                    model: [
                                        { label: qsTr("Standard"),  value: 0 },
                                        { label: qsTr("Fast"),      value: 1 },
                                        { label: qsTr("Very fast"), value: 2 }
                                    ]
                                    // indexOfValue() only resolves once the
                                    // model is ready, so set it on completion
                                    // and whenever the persisted value changes
                                    // rather than in a one-shot binding.
                                    function syncFromSetting() {
                                        currentIndex = Math.max(0, indexOfValue(
                                            app.settings.timelineWheelSpeed))
                                    }
                                    Component.onCompleted: syncFromSetting()
                                    Connections {
                                        target: app.settings
                                        function onTimelineWheelSpeedChanged() {
                                            wheelSpeedCombo.syncFromSetting()
                                        }
                                    }
                                    onActivated: app.settings.timelineWheelSpeed = currentValue
                                    Accessible.description: qsTr(
                                        "How far one physical mouse-wheel notch scrolls the timeline")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("How far one physical mouse-wheel notch moves "
                                               + "the timeline. Touchpad and precision scrolling "
                                               + "stay fine-grained regardless of this setting.")
                                }
                            }
                        }
                    }

                    // ════════════ Appearance (continued: language) ════════════
                    ColumnLayout {
                        visible: root.section === "appearance"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Language")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                }
                                AppComboBox {
                                    storm: true
                                    Layout.fillWidth: true
                                    model: ["en", "lt"]
                                    currentIndex: Math.max(0, model.indexOf(app.settings.language))
                                    onActivated: app.settings.language = model[currentIndex]
                                }
                                Label {
                                    text: qsTr("Language switching requires an app restart.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                }
                            }
                        }

                    }

                    // ════════════ Privacy & security (design 1d) ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Privacy & security")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.ExtraBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Network privacy, encryption health, and recovery.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }

                        // v0.5.11: link-preview and GIF policy.
                        Label {
                            text: qsTr("Link previews & media")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    id: autoPreviewCheck
                                    text: qsTr("Automatically load previews in unencrypted rooms")
                                    checked: app.settings.autoLoadLinkPreviews
                                    onToggled: app.settings.autoLoadLinkPreviews = checked
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Load previews in encrypted rooms")
                                    checked: app.settings.loadPreviewsInEncryptedRooms
                                    onToggled: app.settings.loadPreviewsInEncryptedRooms = checked
                                }
                                // Privacy caution: a danger-ruled callout —
                                // the RULE carries the caution semantics so
                                // the copy itself stays readable body ink
                                // (§1 keeps red text for live danger states).
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    spacing: AppTheme.spacing8
                                    Rectangle {
                                        Layout.fillHeight: true
                                        implicitWidth: 2
                                        radius: 1
                                        color: AppTheme.stormDangerBorder
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.fontCaption
                                        text: qsTr("Loading a preview contacts the linked website "
                                                   + "directly and may reveal your IP address and "
                                                   + "request timing. No JavaScript is executed. "
                                                   + "Encrypted-room previews are off by "
                                                   + "default; otherwise use each message's "
                                                   + "“Load link preview” action.")
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.stormBorder
                                }

                                // ─────────── GIFs ───────────
                                Label {
                                    text: qsTr("GIFs")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.fontBody
                                    font.weight: Font.DemiBold
                                    Layout.topMargin: AppTheme.spacing4
                                }

                                Label { text: qsTr("Autoplay GIFs"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifAutoplayCombo
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Autoplay GIFs")
                                    model: [
                                        { label: qsTr("Always"),   value: 0 },
                                        { label: qsTr("On hover"), value: 1 },
                                        { label: qsTr("Never"),    value: 2 },
                                    ]
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifAutoplay))
                                    onActivated: app.settings.gifAutoplay = currentValue
                                }

                                Label { text: qsTr("GIF safe search"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifRatingCombo
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("GIF safe search rating")
                                    // Values map to gif::Rating (0=g … 3=r).
                                    model: [
                                        { label: qsTr("G — strict"),  value: 0 },
                                        { label: qsTr("PG"),          value: 1 },
                                        { label: qsTr("PG-13"),       value: 2 },
                                        { label: qsTr("R — all"),     value: 3 },
                                    ]
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifSafeSearch))
                                    onActivated: app.settings.gifSafeSearch = currentValue
                                }

                                Label { text: qsTr("Preferred GIF provider"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifProviderCombo
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Preferred GIF provider")
                                    model: [
                                        { label: "GIPHY", value: "giphy" },
                                        { label: "KLIPY", value: "klipy" },
                                    ]
                                    currentIndex: Math.max(0, indexOfValue(app.settings.gifPreferredProvider))
                                    onActivated: app.settings.gifPreferredProvider = currentValue
                                }
                                // Honest per-provider availability.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("GIPHY: %1 · KLIPY: %2")
                                        .arg(app.gif.providerConfigured("giphy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                        .arg(app.gif.providerConfigured("klipy")
                                             ? qsTr("configured") : qsTr("no API key"))
                                }

                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Store recently used GIFs")
                                    checked: app.settings.storeRecentGifs
                                    onToggled: app.settings.storeRecentGifs = checked
                                    Accessible.name: qsTr("Store recently used GIFs")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("GIF searches are sent directly to the "
                                               + "selected provider. Favorites and recent "
                                               + "GIFs are stored locally on this device "
                                               + "and are not synchronized; search terms "
                                               + "are not saved.")
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Clear recent GIFs")
                                        enabled: app.gif.recent.count > 0
                                        onClicked: gifClearConfirm.open("recent")
                                    }
                                    AppButton {
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Clear GIF favorites")
                                        enabled: app.gif.favorites.count > 0
                                        onClicked: gifClearConfirm.open("favorites")
                                    }
                                }
                            }
                        }
                    }

                    // ════════════ Notifications ════════════
                    ColumnLayout {
                        visible: root.section === "notifications"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Notifications")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Desktop notifications")
                                    checked: app.settings.notificationsEnabled
                                    onToggled: app.settings.notificationsEnabled = checked
                                }
                                // v0.6.0 checkpoint 11: notification privacy.
                                Label {
                                    text: qsTr("Notification preview")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationPreviewCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    model: [
                                        qsTr("Sender and message"),
                                        qsTr("Sender only"),
                                        qsTr("Private")
                                    ]
                                    currentIndex: app.settings.notificationPreview
                                    onActivated: (index) =>
                                        app.settings.notificationPreview = index
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Sender only (the default) never shows "
                                               + "message text in notifications. "
                                               + "Encrypted messages that cannot be "
                                               + "decrypted always show a generic "
                                               + "notification. Notifications are "
                                               + "suppressed while the room is open, "
                                               + "focused, and at the latest message.")
                                }
                                // v0.6.1: notification sound.
                                Label {
                                    text: qsTr("Notification sound")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                AppComboBox {
                                    storm: true
                                    objectName: "notificationSoundCombo"
                                    Layout.fillWidth: true
                                    enabled: app.settings.notificationsEnabled
                                    model: [
                                        qsTr("Off"),
                                        qsTr("Mentions and direct messages"),
                                        qsTr("All notifications")
                                    ]
                                    currentIndex: app.settings.notificationSound
                                    onActivated: (index) =>
                                        app.settings.notificationSound = index
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("The sound plays only when a "
                                               + "notification is shown, so muted and "
                                               + "active rooms stay silent. Bursts are "
                                               + "coalesced into a single alert.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Per-room notification modes (set from "
                                               + "Room information) apply to this "
                                               + "device only — they are not server "
                                               + "push rules. Push registration for "
                                               + "mobile-style notifications is not "
                                               + "implemented.")
                                }
                            }
                        }
                    }

                    // ════════════ Account ════════════
                    ColumnLayout {
                        visible: root.section === "account"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Account")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        // v0.6.5 polish: the account header carries the
                        // identity-card idiom (real Avatar, bold display
                        // name, mono MXID, status chip) in the ACTIVE theme
                        // — the brand navy/yellow stays exclusive to the
                        // trust card.
                        SettingsCard {
                            id: accountIdentityCard
                            // Invokable results do not re-evaluate on
                            // signals; refresh the record whenever the
                            // registry or selection changes (the SpacesRail
                            // idiom) — a profile landing after this screen
                            // opened must not leave a stale MXID-as-name.
                            property var accountRecord: ({})
                            function refreshAccountRecord() {
                                accountRecord =
                                    (app.accounts && app.accounts.activeUserId)
                                    ? app.accounts.account(app.accounts.activeUserId)
                                    : ({})
                            }
                            Component.onCompleted: refreshAccountRecord()
                            Connections {
                                target: app.accounts
                                function onAccountsChanged() {
                                    accountIdentityCard.refreshAccountRecord()
                                }
                                function onActiveUserIdChanged() {
                                    accountIdentityCard.refreshAccountRecord()
                                }
                            }
                            readonly property string accountDisplayName:
                                accountRecord && accountRecord.displayName
                                ? accountRecord.displayName : ""
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Avatar {
                                        size: 48
                                        circle: false
                                        squareRadius: 14
                                        mxc: accountIdentityCard.accountRecord
                                             && accountIdentityCard.accountRecord.avatarUrl
                                             ? accountIdentityCard.accountRecord.avatarUrl : ""
                                        name: accountIdentityCard.accountDisplayName.length > 0
                                              ? accountIdentityCard.accountDisplayName
                                              : (app.accounts ? (app.accounts.activeUserId || "") : "")
                                        colorKey: app.accounts ? (app.accounts.activeUserId || "") : ""
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            Label {
                                                text: accountIdentityCard.accountDisplayName.length > 0
                                                      ? accountIdentityCard.accountDisplayName
                                                      : (app.accounts
                                                         ? (app.accounts.activeUserId || qsTr("(signed out)"))
                                                         : "")
                                                color: AppTheme.stormText
                                                font.pixelSize: AppTheme.fontSizeL
                                                font.weight: Font.Bold
                                                elide: Label.ElideRight
                                                Layout.maximumWidth: 300
                                            }
                                            StatusChip {
                                                storm: true
                                                visible: app.backendName === "rust"
                                                label: app.sessionTrustState
                                                iconName: app.sessionTrustState === "Verified"
                                                          ? "verified_user" : ""
                                                // Same mapping as the Sessions
                                                // "Current session" chip — one
                                                // trust state, one ink.
                                                tone: app.sessionTrustState === "Verified"
                                                      ? "success"
                                                      : app.sessionTrustState === "Not verified"
                                                        ? "danger" : "neutral"
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: app.accounts ? (app.accounts.activeUserId || "") : ""
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.fontMonoXS
                                            elide: Label.ElideMiddle
                                        }
                                        Label {
                                            visible: app.backendName === "rust" && app.sessionDeviceId !== ""
                                            text: qsTr("Device %1").arg(app.sessionDeviceId)
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.fontMonoXS
                                        }
                                    }
                                }
                                AppButton {
                                    storm: true
                                    text: qsTr("Open Privacy & security")
                                    onClicked: root.section = "privacy"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("To sign out, use the account menu at the "
                                               + "bottom of the sidebar.")
                                }
                            }
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Homeserver")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                AppTextField {
                                    storm: true
                                    Layout.fillWidth: true
                                    text: app.settings.homeserverUrl
                                    placeholderText: "https://matrix.org"
                                    onEditingFinished: app.settings.homeserverUrl = text
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Changing the homeserver takes effect at the next sign-in.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Startup")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.fontSecondary
                                    font.weight: Font.DemiBold
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Start minimized")
                                    checked: app.settings.startMinimized
                                    onToggled: app.settings.startMinimized = checked
                                }
                            }
                        }
                    }

                    // ════════════ Privacy & security (encryption) ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        // Storage / crypto backend facts (all backends).
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: app.settings.secretsAreSecure
                                    color: AppTheme.stormSuccess
                                    text: qsTr("Access tokens are stored via the system Secret Service. Logout clears them.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: !app.settings.secretsAreSecure
                                    color: AppTheme.stormDanger
                                    text: qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Crypto backend: %1").arg(app.crypto.backendDescription)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: app.crypto.supportsE2ee ? AppTheme.stormSuccess : AppTheme.stormTextMuted
                                    text: qsTr("E2EE status: %1").arg(app.crypto.statusString)
                                }
                            }
                        }

                        // v0.6.0 checkpoint 7: read-only E2EE health from
                        // the Rust SDK (app.cryptoHealth). Unsupported
                        // capabilities show as informative state, never as
                        // errors.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Security status")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.fontSecondary
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "cryptoHealthRefresh"
                                        text: qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.fontSecondary
                                        font.underline: true
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.refreshCryptoHealth()
                                        }
                                    }
                                }
                                Label {
                                    objectName: "cryptoHealthSummary"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormText
                                    text: app.cryptoHealth.statusSummary
                                }
                                // v0.7: live verified-session bootstrap
                                // status — the SDK-owned secret request /
                                // backup restore progress after verifying
                                // this session from a trusted one. Manual
                                // recovery-key entry below stays the
                                // fallback, never the first step.
                                RowLayout {
                                    objectName: "cryptoBootstrapStatus"
                                    visible: app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    BusyIndicator {
                                        width: 14; height: 14
                                        visible: running
                                        running: app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.WaitingForKeys
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.SecretsPending
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.SecretReceived
                                                 || app.cryptoBootstrap.phase
                                                     === CryptoBootstrapModel.RestoringHistory
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: app.cryptoBootstrap.phase
                                                   === CryptoBootstrapModel.Ready
                                               ? AppTheme.stormSuccess
                                               : app.cryptoBootstrap.needsRecoveryKey
                                                 ? AppTheme.bolt
                                                 : AppTheme.stormText
                                        font.pixelSize: AppTheme.fontSecondary
                                        text: app.cryptoBootstrap.statusMessage
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: text
                                    }
                                }
                                // v0.7.2: standards-based key re-request.
                                // The Rust recovery coordinator issues a
                                // FRESH m.secret.request round through the
                                // SDK's gossip machinery (new request IDs,
                                // full trust validation on the answers) and
                                // keeps re-trying on a bounded ladder. Shown
                                // only when a new request is genuinely
                                // useful (session verified, identity
                                // trusted, secrets still missing).
                                AppButton {
                                    storm: true
                                    objectName: "requestKeysAgain"
                                    visible: app.cryptoBootstrap.canRequestKeys
                                    enabled: app.loggedIn
                                    text: qsTr("Request keys again")
                                    Accessible.name: text
                                    onClicked: app.requestEncryptionKeys()
                                }
                                // When this session does not itself trust
                                // the account identity, a gossiped answer
                                // could not be accepted — only a repeated
                                // interactive verification (or the recovery
                                // key below) can complete the trust chain.
                                // No new crypto: this is the existing
                                // startOwnVerification path.
                                AppButton {
                                    storm: true
                                    objectName: "verifyAgainForKeys"
                                    visible: app.cryptoBootstrap.phase
                                                 === CryptoBootstrapModel.IdentityIncomplete
                                             || app.cryptoBootstrap.phase
                                                 === CryptoBootstrapModel.ManualRecoveryRequired
                                    enabled: app.loggedIn
                                             && (!app.verificationActive
                                                 || app.verificationState === "done"
                                                 || app.verificationState === "cancelled"
                                                 || app.verificationState.indexOf("failed") === 0)
                                    text: app.cryptoBootstrap.phase
                                              === CryptoBootstrapModel.IdentityIncomplete
                                          ? qsTr("Verify this session again")
                                          : qsTr("Verify another session to request keys")
                                    Accessible.name: text
                                    onClicked: {
                                        // The live flow card renders in the
                                        // Sessions section — bring it into
                                        // view alongside starting the flow.
                                        root.section = "sessions"
                                        app.startOwnVerification()
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: {
                                        var device = app.cryptoHealth.currentDeviceVerified
                                        var deviceText = device === CryptoHealthModel.Yes
                                            ? qsTr("Yes")
                                            : device === CryptoHealthModel.No
                                              ? qsTr("No") : qsTr("Unknown")
                                        return qsTr("Current session verified: %1").arg(deviceText)
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.crossSigningReady
                                          ? qsTr("Cross-signing: ready")
                                          : app.cryptoHealth.crossSigningAvailable
                                            ? qsTr("Cross-signing: not complete on this session")
                                            : qsTr("Cross-signing: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.keyBackupUsable
                                          ? qsTr("Key backup: active on this session")
                                          : app.cryptoHealth.keyBackupAvailable
                                            ? qsTr("Key backup: exists, but this session cannot use it yet")
                                            : qsTr("Key backup: none found")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.recoveryAvailable
                                          ? qsTr("Recovery: set up")
                                          : app.cryptoHealth.recoveryRequired
                                            ? qsTr("Recovery: set up, but secrets are missing here")
                                            : qsTr("Recovery: not set up")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: app.cryptoHealth.cryptoSyncing
                                          ? qsTr("Encryption sync: active")
                                          : app.cryptoHealth.cryptoReady
                                            ? qsTr("Encryption sync: ready")
                                            : qsTr("Encryption sync: waiting")
                                }
                                // v0.7.2 sanitized recovery diagnostics —
                                // fixed tokens and counts only, expandable
                                // so the primary status stays concise.
                                // v0.6.5 (C8): a real disclosure row instead
                                // of a bare underlined Label — the plain-text
                                // link read as inert body copy, so clicking
                                // it could be mistaken for an unrelated
                                // geometry defect rather than the intentional
                                // expander it is. Row chrome plus a rotating
                                // chevron (the same treatment AppComboBox's
                                // indicator already uses) make the
                                // expand/collapse affordance visible; the
                                // toggled property, its target block, and the
                                // click behavior are unchanged. implicitHeight
                                // is a hard constant — hover/press only paint
                                // the background, they never resize the row.
                                AbstractButton {
                                    id: recoveryDiagnosticsToggle
                                    objectName: "recoveryDiagnosticsToggle"
                                    visible: app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    implicitHeight: 28
                                    hoverEnabled: true
                                    focusPolicy: Qt.TabFocus
                                    Accessible.role: Accessible.Button
                                    Accessible.name: root.showRecoveryDiagnostics
                                        ? qsTr("Hide recovery diagnostics")
                                        : qsTr("Recovery diagnostics")
                                    onClicked: root.showRecoveryDiagnostics
                                        = !root.showRecoveryDiagnostics
                                    contentItem: RowLayout {
                                        spacing: AppTheme.spacing4
                                        Label {
                                            text: root.showRecoveryDiagnostics
                                                  ? qsTr("Hide recovery diagnostics")
                                                  : qsTr("Recovery diagnostics")
                                            color: AppTheme.stormLink
                                            font.pixelSize: AppTheme.fontSecondary
                                        }
                                        Icon {
                                            name: "expand_more"
                                            size: 16
                                            color: AppTheme.stormLink
                                            rotation: root.showRecoveryDiagnostics ? 180 : 0
                                            Behavior on rotation {
                                                enabled: !AppTheme.reducedMotion
                                                NumberAnimation { duration: 120 }
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    background: Rectangle {
                                        radius: AppTheme.radiusMd
                                        color: (recoveryDiagnosticsToggle.hovered
                                                || recoveryDiagnosticsToggle.down)
                                               ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                               : "transparent"
                                    }
                                    // Keyboard focus ring — the same
                                    // absolute-overlay idiom every other
                                    // AbstractButton-based control in this
                                    // file uses (matchSystemSwitch above;
                                    // IconButton/AppButton/AppTextField
                                    // shell-wide): anchors.fill + negative
                                    // margins, so it paints outside the
                                    // row's own bounds and never feeds back
                                    // into implicitHeight. visualFocus (not
                                    // activeFocus) because this IS an
                                    // AbstractButton — it only lights on
                                    // keyboard focus, not a plain click.
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        radius: AppTheme.radiusMd + 4
                                        color: "transparent"
                                        border.width: 2
                                        border.color: AppTheme.bolt
                                        visible: recoveryDiagnosticsToggle.visualFocus
                                    }
                                }
                                ColumnLayout {
                                    objectName: "recoveryDiagnostics"
                                    visible: root.showRecoveryDiagnostics
                                             && app.cryptoBootstrap.active
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing4
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        text: qsTr("Own identity: %1").arg(
                                            app.cryptoBootstrap.ownIdentity === "verified"
                                                ? qsTr("verified")
                                                : app.cryptoBootstrap.ownIdentity === "unverified"
                                                  ? qsTr("not verified on this session")
                                                  : qsTr("not checked yet"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        text: qsTr("Cross-signing private keys: %1").arg(
                                            app.cryptoBootstrap.crossSigningSecrets === "complete"
                                                ? qsTr("present")
                                                : app.cryptoBootstrap.crossSigningSecrets === "incomplete"
                                                  ? qsTr("missing on this session")
                                                  : qsTr("not checked yet"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        text: {
                                            var s = app.cryptoBootstrap.requestState
                                            var label = s === "requested"
                                                ? qsTr("sent")
                                                : s === "already_pending"
                                                  ? qsTr("pending")
                                                  : s === "none_missing"
                                                    ? qsTr("nothing missing")
                                                    : s === "identity_unverified"
                                                      ? qsTr("blocked — identity not verified")
                                                      : s === "no_eligible_devices"
                                                        ? qsTr("no verified session to ask")
                                                        : s === "unavailable"
                                                          ? qsTr("could not be created")
                                                          : qsTr("not sent yet")
                                            return qsTr("Secret request: %1 (%n attempt(s))",
                                                        "",
                                                        app.cryptoBootstrap.requestAttempts)
                                                .arg(label)
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        visible: app.cryptoBootstrap.requestAttempts > 0
                                        text: qsTr("Verified sessions available: %1")
                                            .arg(app.cryptoBootstrap.eligibleDevices)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        text: qsTr("Backup key usable: %1").arg(
                                            app.cryptoHealth.keyBackupUsable
                                                ? qsTr("yes") : qsTr("no"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontSecondary
                                        visible: app.cryptoBootstrap.keysReceived > 0
                                        text: qsTr("Room keys imported: %1")
                                            .arg(app.cryptoBootstrap.keysReceived)
                                    }
                                }
                            }
                        }

                    }

                    // ════════════ Sessions (design 1d) ════════════
                    ColumnLayout {
                        visible: root.section === "sessions"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        // The trust chain's DEVICES step reads the session
                        // list — populate it when the section opens (same
                        // data the manual Refresh below fetches).
                        onVisibleChanged: {
                            if (visible && app.backendName === "rust")
                                app.refreshSessionDevices()
                        }

                        Label {
                            text: qsTr("Sessions")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.ExtraBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("This account's Matrix sessions and device "
                                       + "verification.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }

                        // v0.6.5 (SPEC 1r): the own-account trust chain,
                        // driven by REAL crypto state only — never another
                        // user's, never optimistic. Brand-fixed by design
                        // (the card deliberately ignores the theme). Verify
                        // routes to the existing SAS flow; its visibility
                        // mirrors the start-row complement further below so
                        // the two controls never coexist.
                        TrustCard {
                            id: sessionsTrustCard
                            objectName: "sessionsTrustCard"
                            Layout.fillWidth: true
                            visible: app.cryptoHealth
                                     && app.cryptoHealth.cryptoSupported
                            // Same invokable-staleness guard as the Account
                            // header above: refresh on registry/selection
                            // changes instead of binding a Q_INVOKABLE.
                            property var accountRecord: ({})
                            function refreshAccountRecord() {
                                accountRecord =
                                    (app.accounts && app.accounts.activeUserId)
                                    ? app.accounts.account(app.accounts.activeUserId)
                                    : ({})
                            }
                            Component.onCompleted: refreshAccountRecord()
                            Connections {
                                target: app.accounts
                                function onAccountsChanged() {
                                    sessionsTrustCard.refreshAccountRecord()
                                }
                                function onActiveUserIdChanged() {
                                    sessionsTrustCard.refreshAccountRecord()
                                }
                            }
                            readonly property bool devicesVerified:
                                app.sessionDevices.length > 0
                                ? app.sessionDevices.every(
                                      d => d.verified === true
                                           || d.crossSigned === true)
                                : app.cryptoHealth.currentDeviceVerified
                                  === CryptoHealthModel.Yes
                            readonly property var chainSteps: [
                                { label: qsTr("IDENTITY"), iconName: "person",
                                  complete: app.cryptoHealth.ownIdentityVerified
                                            === CryptoHealthModel.Yes },
                                { label: qsTr("%1 DEVICES")
                                        .arg(app.sessionDevices.length),
                                  iconName: "devices",
                                  complete: devicesVerified },
                                { label: qsTr("CROSS-SIGN"), iconName: "key",
                                  complete: app.cryptoHealth.crossSigningReady
                                            === true }
                            ]
                            displayName: accountRecord
                                         && accountRecord.displayName
                                         ? accountRecord.displayName
                                         : (app.accounts
                                            ? app.accounts.activeUserId : "")
                            userId: app.accounts ? app.accounts.activeUserId
                                                 : ""
                            avatarMxc: accountRecord && accountRecord.avatarUrl
                                       ? accountRecord.avatarUrl : ""
                            steps: chainSteps
                            statusText: {
                                var complete = 0
                                for (var i = 0; i < chainSteps.length; ++i) {
                                    if (chainSteps[i].complete)
                                        ++complete
                                }
                                return qsTr("%1 of 3 checks complete")
                                       .arg(complete)
                            }
                            showVerify: !app.verificationActive
                                        && app.verificationState === ""
                            onVerifyRequested: app.startOwnVerification()
                        }

                        // v0.6.0 checkpoint 9: the account's Matrix
                        // devices/sessions — server metadata merged with SDK
                        // crypto trust. Read-only: removing other sessions
                        // requires interactive re-authentication, which
                        // Lightning does not implement yet (limitation shown
                        // honestly below).
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    Layout.fillWidth: true
                                    // Mono-caption module header — the trust
                                    // card's "TRUST CHAIN" idiom, in theme
                                    // ink.
                                    RowLayout {
                                        spacing: AppTheme.spacing6
                                        Icon {
                                            name: "devices"
                                            size: 13
                                            color: AppTheme.stormTextFaint
                                        }
                                        MenuSectionLabel { text: qsTr("Sessions") }
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "sessionDevicesRefresh"
                                        text: app.sessionDevicesLoading
                                              ? qsTr("Loading…") : qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.fontSecondary
                                        font.underline: !app.sessionDevicesLoading
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            enabled: !app.sessionDevicesLoading
                                            onClicked: app.refreshSessionDevices()
                                        }
                                    }
                                }
                                Label {
                                    visible: app.sessionDevicesFailed
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("The session list could not be loaded.")
                                }
                                Label {
                                    visible: !app.sessionDevicesFailed
                                             && !app.sessionDevicesLoading
                                             && app.sessionDevices.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("Press Refresh to load this account's sessions.")
                                }
                                // v0.6.5 polish: each session is a small
                                // elevated tile (device icon, name, status
                                // chips, mono metadata) — the premium card
                                // language of the trust surface, in theme
                                // ink. Trust values still come straight from
                                // SDK state; nothing here invents trust.
                                Repeater {
                                    model: app.sessionDevices
                                    delegate: Rectangle {
                                        Layout.fillWidth: true
                                        radius: AppTheme.radiusTile
                                        color: AppTheme.stormInset
                                        border.width: 1
                                        border.color: modelData.isCurrent === true
                                                      ? AppTheme.stormBorderStrong
                                                      : AppTheme.stormBorder
                                        implicitHeight: sessionTileRow.implicitHeight
                                                        + AppTheme.spacing8 * 2

                                        RowLayout {
                                            id: sessionTileRow
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: AppTheme.spacing8 + 2
                                            anchors.rightMargin: AppTheme.spacing8 + 2
                                            spacing: AppTheme.spacing8 + 2

                                            Rectangle {
                                                implicitWidth: 30
                                                implicitHeight: 30
                                                radius: AppTheme.radiusTile
                                                color: AppTheme.stormCanvas
                                                border.width: 1
                                                border.color: AppTheme.stormBorder
                                                Icon {
                                                    anchors.centerIn: parent
                                                    name: "devices"
                                                    size: 16
                                                    color: modelData.isCurrent === true
                                                           ? AppTheme.bolt
                                                           : AppTheme.stormTextSecondary
                                                }
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: AppTheme.spacing6
                                                    Label {
                                                        text: (modelData.displayName
                                                               && modelData.displayName.length > 0)
                                                              ? modelData.displayName
                                                              : modelData.deviceId
                                                        color: AppTheme.stormText
                                                        font.pixelSize: AppTheme.fontSecondary
                                                        font.weight: Font.DemiBold
                                                        elide: Label.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                    StatusChip {
                                                        storm: true
                                                        visible: modelData.isCurrent === true
                                                        label: qsTr("This session")
                                                        tone: "accent"
                                                    }
                                                    StatusChip {
                                                        storm: true
                                                        label: modelData.crossSigned === true
                                                              ? qsTr("Verified")
                                                              : modelData.hasCryptoIdentity === true
                                                                ? qsTr("Not verified")
                                                                : qsTr("No encryption")
                                                        iconName: modelData.crossSigned === true
                                                                  ? "verified_user" : ""
                                                        tone: modelData.crossSigned === true
                                                              ? "success" : "neutral"
                                                    }
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.fontMonoXS
                                                    elide: Label.ElideRight
                                                    text: {
                                                        var parts = [ modelData.deviceId ]
                                                        if (modelData.lastSeen
                                                            && !isNaN(modelData.lastSeen.getTime()))
                                                            parts.push(qsTr("last seen %1").arg(
                                                                Qt.formatDateTime(modelData.lastSeen,
                                                                                  "d MMM yyyy hh:mm")))
                                                        if (modelData.lastSeenIp
                                                            && modelData.lastSeenIp.length > 0)
                                                            parts.push(modelData.lastSeenIp)
                                                        return parts.join(" · ")
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontSecondary
                                    text: qsTr("Signing out other sessions from Lightning "
                                               + "is not supported yet — use another "
                                               + "client for that. Verification below "
                                               + "always requires explicit confirmation "
                                               + "on both sessions.")
                                }
                            }
                        }

                        // Rust-only: session verification.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing6
                                    Icon {
                                        name: "key"
                                        size: 13
                                        color: AppTheme.stormTextFaint
                                    }
                                    MenuSectionLabel { text: qsTr("Current session") }
                                    Item { Layout.fillWidth: true }
                                    StatusChip {
                                        storm: true
                                        label: app.sessionTrustState
                                        iconName: app.sessionTrustState === "Verified"
                                                  ? "verified_user" : ""
                                        tone: app.sessionTrustState === "Verified"
                                              ? "success"
                                              : app.sessionTrustState === "Not verified"
                                                ? "danger" : "neutral"
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.fontMonoXS
                                    text: qsTr("Device ID: %1").arg(
                                        app.sessionDeviceId !== ""
                                            ? app.sessionDeviceId
                                            : (app.rustDeviceIdRedacted !== ""
                                                ? app.rustDeviceIdRedacted
                                                : qsTr("(not yet available)")))
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    visible: app.sessionTrustState !== "Verified"
                                    text: qsTr(
                                        "Verify this session using another session already " +
                                        "signed in to this Matrix account. This does not import " +
                                        "room keys — key import is a separate action below.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    // Mirror of the active-flow card's
                                    // condition, so the start row and the flow
                                    // card are never both shown. When the
                                    // trust card renders (crypto-supported
                                    // backends), ITS Verify button is the one
                                    // start affordance — this legacy row only
                                    // covers backends without the card, so
                                    // Sessions never shows two identical
                                    // Verify triggers at once (v0.6.5).
                                    visible: !app.verificationActive
                                            && app.verificationState === ""
                                            && !(app.cryptoHealth
                                                 && app.cryptoHealth.cryptoSupported)
                                    AppButton {
                                        storm: true
                                        text: app.sessionTrustState === "Verified"
                                            ? qsTr("Verify again")
                                            : qsTr("Verify this session")
                                        enabled: app.loggedIn
                                        onClicked: app.startOwnVerification()
                                    }
                                    Label {
                                        visible: app.sessionTrustState === "Verified"
                                        Layout.fillWidth: true
                                        color: AppTheme.stormSuccess
                                        text: qsTr("This Lightning session is verified through Matrix cross-signing.")
                                        wrapMode: Text.WordWrap
                                    }
                                    Item {
                                        visible: app.sessionTrustState !== "Verified"
                                        Layout.fillWidth: true
                                    }
                                }

                                // Active SAS flow card.
                                Pane {
                                    Layout.fillWidth: true
                                    // NOT just verificationActive: that is
                                    // !flowId.isEmpty(), and startOwnVerification()
                                    // clears the flow id before setting
                                    // "starting". Every failure raised before a
                                    // flow id exists — no cross-signing
                                    // identity, request send failed, not signed
                                    // in — therefore landed in a hidden card,
                                    // so clicking "Verify this session" did
                                    // visibly nothing at all. Any non-empty
                                    // state must show the card.
                                    visible: app.verificationActive
                                            || app.verificationState !== ""
                                    background: Rectangle {
                                        color: AppTheme.stormPanel
                                        border.color: AppTheme.bolt
                                        radius: AppTheme.radiusSm
                                    }
                                    ColumnLayout {
                                        width: parent.width
                                        spacing: AppTheme.spacing8

                                        Label {
                                            text: qsTr("Session verification")
                                            color: AppTheme.stormText
                                            font.weight: Font.DemiBold
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            // v0.7.1: live progress feedback
                                            // for the two post-"They match"
                                            // states — the press is
                                            // acknowledged immediately and
                                            // the peer wait is named.
                                            BusyIndicator {
                                                implicitWidth: 18
                                                implicitHeight: 18
                                                visible: running
                                                running: app.verificationState === "confirming"
                                                         || app.verificationState === "waiting_for_peer"
                                                         || app.verificationState === "ready"
                                            }
                                            Label {
                                                objectName: "verificationStatusLabel"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                                color: app.verificationState === "done"
                                                       ? AppTheme.stormSuccess
                                                       : AppTheme.stormTextMuted
                                                Accessible.role: Accessible.StaticText
                                                Accessible.name: text
                                                text: {
                                                    if (app.verificationState === "starting")
                                                        return qsTr("Sending verification request…")
                                                    if (app.verificationState === "waiting_for_other_session")
                                                        return qsTr(
                                                            "Verification request sent. Accept it in " +
                                                            "another session, such as Element.")
                                                    if (app.verificationState === "requested")
                                                        return qsTr("Incoming verification request from %1")
                                                            .arg(app.verificationOtherUser)
                                                    if (app.verificationState === "ready")
                                                        return qsTr(
                                                            "Both sessions accepted. Exchanging keys — " +
                                                            "the emojis will appear here shortly.")
                                                    if (app.verificationState === "sas_ready")
                                                        return qsTr(
                                                            "Compare all seven emojis with the other " +
                                                            "session. Confirm only if every emoji matches " +
                                                            "in the same order.")
                                                    if (app.verificationState === "confirming")
                                                        return qsTr("Confirming verification…")
                                                    if (app.verificationState === "waiting_for_peer")
                                                        return qsTr(
                                                            "Waiting for your other device to confirm…")
                                                    if (app.verificationState === "done")
                                                        return qsTr(
                                                            "Verification complete. This session is now " +
                                                            "verified; Lightning is refreshing the trust " +
                                                            "state and requesting encryption keys.")
                                                    if (app.verificationState === "cancelled")
                                                        return qsTr("Verification cancelled.")
                                                    if (app.verificationState.indexOf("failed") === 0) {
                                                        // The reason is the
                                                        // whole value of this
                                                        // line: "no
                                                        // cross-signing
                                                        // identity" tells the
                                                        // user what to do,
                                                        // "Verification
                                                        // failed." does not.
                                                        // AppController stores
                                                        // it as "failed:<msg>".
                                                        var reason =
                                                            app.verificationState
                                                                .substring(7)
                                                        if (reason.length > 0)
                                                            return qsTr(
                                                                "Verification failed: %1")
                                                                .arg(reason)
                                                        return qsTr("Verification failed.")
                                                    }
                                                    return qsTr("Waiting…")
                                                }
                                            }
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            // Emoji stay on screen through
                                            // confirming/waiting so users can
                                            // keep comparing while the flow
                                            // settles.
                                            visible: app.verificationState === "sas_ready"
                                                     || app.verificationState === "confirming"
                                                     || app.verificationState === "waiting_for_peer"
                                            Repeater {
                                                model: app.verificationEmojis
                                                delegate: Rectangle {
                                                    color: AppTheme.stormCanvas
                                                    border.color: AppTheme.stormBorder
                                                    radius: AppTheme.radiusSm
                                                    implicitWidth: 84
                                                    implicitHeight: 78
                                                    ColumnLayout {
                                                        // Width-bound, not
                                                        // centerIn: a long SAS
                                                        // word ("Headphones")
                                                        // otherwise widens the
                                                        // layout past the 84px
                                                        // tile and bleeds over
                                                        // its neighbours.
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        anchors.left: parent.left
                                                        anchors.right: parent.right
                                                        anchors.margins: AppTheme.spacing4
                                                        spacing: 2
                                                        Label {
                                                            text: modelData.symbol || ""
                                                            font.pixelSize: 28
                                                            horizontalAlignment: Text.AlignHCenter
                                                            Layout.alignment: Qt.AlignHCenter
                                                        }
                                                        Label {
                                                            text: modelData.description || ""
                                                            font.pixelSize: AppTheme.fontCaption
                                                            color: AppTheme.stormTextMuted
                                                            horizontalAlignment: Text.AlignHCenter
                                                            // Wrap, never elide: the user is
                                                            // asked to COMPARE this word across
                                                            // devices — a truncated word is
                                                            // worse than a two-line caption.
                                                            wrapMode: Text.Wrap
                                                            maximumLineCount: 2
                                                            Layout.fillWidth: true
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            AppButton {
                                                storm: true
                                                text: qsTr("Accept")
                                                visible: app.verificationState === "requested"
                                                kind: "primary"
                                                onClicked: app.acceptVerification()
                                            }
                                            AppButton {
                                                storm: true
                                                text: qsTr("They match")
                                                // v0.7.1: stays visible (but
                                                // disabled) through the
                                                // confirm/peer wait so the
                                                // card does not jump; only
                                                // sas_ready accepts the press
                                                // (AppController enforces the
                                                // same guard).
                                                visible: app.verificationState === "sas_ready"
                                                        || app.verificationState === "confirming"
                                                        || app.verificationState === "waiting_for_peer"
                                                enabled: app.verificationState === "sas_ready"
                                                kind: "primary"
                                                onClicked: app.confirmVerification()
                                            }
                                            AppButton {
                                                storm: true
                                                text: qsTr("They do not match")
                                                visible: app.verificationState === "sas_ready"
                                                        || app.verificationState === "confirming"
                                                        || app.verificationState === "waiting_for_peer"
                                                enabled: app.verificationState === "sas_ready"
                                                onClicked: app.mismatchVerification()
                                            }
                                            AppButton {
                                                storm: true
                                                text: qsTr("Cancel verification")
                                                // Cancel must exist in EVERY
                                                // non-terminal state. Listing
                                                // states positively meant a
                                                // newly added one ("ready")
                                                // silently lost the only way
                                                // out: that card shows a
                                                // spinner and no buttons at
                                                // all, so a peer that never
                                                // advertised m.sas.v1 pinned
                                                // the user to it. Inverted so
                                                // the next added state cannot
                                                // drop the escape hatch again.
                                                visible: app.verificationState !== ""
                                                        && app.verificationState !== "done"
                                                        && app.verificationState !== "cancelled"
                                                        && !app.verificationState.startsWith("failed")
                                                onClicked: app.cancelVerification()
                                            }
                                            AppButton {
                                                storm: true
                                                text: qsTr("Dismiss")
                                                visible: app.verificationState === "done"
                                                        || app.verificationState === "cancelled"
                                                        || app.verificationState.indexOf("failed") === 0
                                                onClicked: app.cancelVerification()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                    }

                    // ════════════ Privacy & security (recovery) ════════════
                    ColumnLayout {
                        visible: root.section === "privacy"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        // Rust-only: recovery key/passphrase + room-key
                        // import. Restoring recovery also restores
                        // cross-signing secrets where they are stored in 4S
                        // (the SDK imports them and can sign this session).
                        // Honest limitations: SETTING UP new cross-signing or
                        // a new key backup requires interactive
                        // re-authentication / full 4S bootstrap, which
                        // Lightning does not implement in 0.6.0; secrets from
                        // other sessions arrive via the SDK's automatic
                        // secret gossip after verification (no manual
                        // request button is faked).
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Some old messages may show \"[unable to decrypt yet]\" until " +
                                        "you restore your recovery key here, or until another " +
                                        "verified device shares the room keys.")
                                }

                                Label {
                                    text: qsTr("Recovery key or passphrase")
                                    font.weight: Font.DemiBold
                                    color: AppTheme.stormText
                                }
                                GridLayout {
                                    id: recoveryRow
                                    Layout.fillWidth: true
                                    columnSpacing: AppTheme.spacing8
                                    rowSpacing: AppTheme.spacing8
                                    columns: width < 360 ? 1 : 2
                                    AppTextField {
                                        storm: true
                                        id: recoveryField
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 160
                                        objectName: "recoveryInputField"
                                        echoMode: TextInput.Password
                                        // The SDK's recover() accepts both a
                                        // recovery key and a passphrase.
                                        placeholderText: qsTr("Recovery key or passphrase")
                                        enabled: !recoveryPanel.running
                                    }
                                    AppButton {
                                        storm: true
                                        text: recoveryPanel.running
                                            ? qsTr("Restoring…")
                                            : qsTr("Restore keys")
                                        enabled: !recoveryPanel.running
                                            && recoveryField.text.length > 0
                                        onClicked: {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.stormTextMuted
                                            app.requestRecoverFromBackup(recoveryField.text)
                                            // Wipe local copy immediately — the recovery
                                            // key never sits in a QML property beyond
                                            // this call.
                                            recoveryField.text = ""
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: recoveryPanel.statusText !== ""
                                    color: recoveryPanel.statusColor
                                    text: recoveryPanel.statusText
                                }
                                QtObject {
                                    id: recoveryPanel
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.stormTextMuted
                                }
                                Connections {
                                    target: app
                                    function onRecoveryStateChanged(state, message) {
                                        if (state === "attempted") {
                                            recoveryPanel.running = true
                                            recoveryPanel.statusText = qsTr("Recovery started")
                                            recoveryPanel.statusColor = AppTheme.stormTextMuted
                                        } else if (state === "ok") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery complete. New messages should " +
                                                "decrypt as keys arrive. Some old messages may " +
                                                "still require another verified device to share " +
                                                "keys.")
                                            // v0.6.0 checkpoint 10: recovered
                                            // secrets change trust/backup
                                            // state — re-read it from the SDK.
                                            app.refreshCryptoHealth()
                                            app.refreshSessionTrustState()
                                            recoveryPanel.statusColor = AppTheme.stormSuccess
                                        } else if (state === "failed") {
                                            recoveryPanel.running = false
                                            recoveryPanel.statusText = qsTr(
                                                "Recovery failed: %1").arg(message)
                                            recoveryPanel.statusColor = AppTheme.stormDanger
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Import room keys")
                                    font.weight: Font.DemiBold
                                    color: AppTheme.stormText
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Import an encrypted Matrix room-key export from another " +
                                        "session. Imported keys may unlock older encrypted messages, " +
                                        "but they do not verify this session.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        storm: true
                                        text: qsTr("Choose key export")
                                        enabled: app.loggedIn && !importPanel.running
                                        onClicked: importFileDialog.open()
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        elide: Text.ElideMiddle
                                        color: AppTheme.stormTextMuted
                                        text: importPanel.selectedFileName === ""
                                            ? qsTr("(no file selected)")
                                            : importPanel.selectedFileName
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: importPanel.selectedFileUrl.toString() !== ""
                                    AppTextField {
                                        storm: true
                                        id: importPassphraseField
                                        Layout.fillWidth: true
                                        echoMode: TextInput.Password
                                        placeholderText: qsTr("Export passphrase")
                                        enabled: !importPanel.running
                                        onAccepted: {
                                            if (text.length > 0)
                                                importStartButton.clicked()
                                        }
                                    }
                                    AppButton {
                                        storm: true
                                        id: importStartButton
                                        text: importPanel.running
                                            ? qsTr("Importing…")
                                            : qsTr("Import")
                                        enabled: !importPanel.running
                                            && importPassphraseField.text.length > 0
                                        onClicked: {
                                            app.importRoomKeys(
                                                importPanel.selectedFileUrl,
                                                importPassphraseField.text)
                                            // Wipe the passphrase from the QML field
                                            // immediately — never keep it beyond the
                                            // dispatch.
                                            importPassphraseField.text = ""
                                        }
                                    }
                                    AppButton {
                                        storm: true
                                        text: qsTr("Clear")
                                        enabled: !importPanel.running
                                        onClicked: {
                                            importPanel.selectedFileUrl = ""
                                            importPanel.selectedFileName = ""
                                            importPassphraseField.text = ""
                                        }
                                    }
                                }
                                ProgressBar {
                                    Layout.fillWidth: true
                                    visible: importPanel.running
                                    indeterminate: app.roomKeyImportTotalCount === 0
                                    from: 0
                                    to: Math.max(1, app.roomKeyImportTotalCount)
                                    value: app.roomKeyImportImportedCount
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: importPanel.statusText !== ""
                                    color: importPanel.statusColor
                                    text: importPanel.statusText
                                }
                                QtObject {
                                    id: importPanel
                                    property url selectedFileUrl: ""
                                    property string selectedFileName: ""
                                    property bool running: false
                                    property string statusText: ""
                                    property color statusColor: AppTheme.stormTextMuted
                                }
                                FileDialog {
                                    id: importFileDialog
                                    title: qsTr("Select encrypted Matrix room-key export")
                                    fileMode: FileDialog.OpenFile
                                    // Deliberately no nameFilters — Element writes
                                    // .txt exports; users may rename.
                                    onAccepted: {
                                        importPanel.selectedFileUrl = selectedFile
                                        importPanel.selectedFileName =
                                            selectedFile.toString().split('/').pop()
                                        importPanel.statusText = ""
                                    }
                                }
                                Connections {
                                    target: app
                                    function onRoomKeyImportStateChanged() {
                                        var state = app.roomKeyImportState
                                        if (state === "importing") {
                                            importPanel.running = true
                                            importPanel.statusText =
                                                qsTr("Importing room keys…")
                                            importPanel.statusColor = AppTheme.stormTextMuted
                                        } else if (state === "done") {
                                            importPanel.running = false
                                            var doneText = qsTr(
                                                "Room-key import complete.\n" +
                                                "Imported sessions: %1\n" +
                                                "Affected rooms: %2\n" +
                                                "Note: importing keys does not verify this session.")
                                                .arg(app.roomKeyImportImportedCount)
                                                .arg(app.roomKeyImportAffectedRoomCount)
                                            if (app.roomKeyImportLastMessage !== "")
                                                doneText += "\n" + app.roomKeyImportLastMessage
                                            importPanel.statusText = doneText
                                            importPanel.statusColor = AppTheme.stormSuccess
                                            importPanel.selectedFileUrl = ""
                                            importPanel.selectedFileName = ""
                                            importPassphraseField.text = ""
                                        } else if (state === "failed") {
                                            importPanel.running = false
                                            importPanel.statusText =
                                                app.roomKeyImportLastMessage
                                            importPanel.statusColor = AppTheme.stormDanger
                                            importPassphraseField.text = ""
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr(
                                        "Verification establishes trust in this session. " +
                                        "Secure Backup and room-key imports provide decryption keys " +
                                        "for message history. These are separate operations.")
                                }
                            }
                        }

                        // Danger Zone — collapsed by default, clearly apart.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            background: Rectangle {
                                color: AppTheme.stormCanvas
                                border.color: dangerZone.expanded ? AppTheme.stormDanger
                                                                  : AppTheme.stormBorder
                                radius: AppTheme.radiusMd
                            }
                            ColumnLayout {
                                id: dangerZone
                                property bool expanded: false
                                width: parent.width
                                spacing: AppTheme.spacing8

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Danger Zone")
                                        color: AppTheme.stormDanger
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    AppButton {
                                        storm: true
                                        text: dangerZone.expanded ? qsTr("Hide") : qsTr("Show")
                                        Accessible.name: qsTr("Toggle danger zone")
                                        onClicked: dangerZone.expanded = !dangerZone.expanded
                                    }
                                }
                                ColumnLayout {
                                    visible: dangerZone.expanded
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.fontCaption
                                        text: qsTr(
                                            "Reset deletes only Lightning's local Rust SDK store " +
                                            "for this account (also available from a terminal: " +
                                            "matrix-client --reset-crypto-store). It does not touch " +
                                            "server messages or Element data. You will need to sign " +
                                            "in again afterwards.")
                                    }
                                    AppButton {
                                        storm: true
                                        id: resetDangerButton
                                        kind: "danger"
                                        text: qsTr("Reset local Lightning session")
                                        onClicked: resetConfirmDialog.open()
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: resetStatus.text !== ""
                                        color: resetStatus.ok ? AppTheme.stormSuccess : AppTheme.stormDanger
                                        text: resetStatus.text
                                    }
                                }
                                QtObject {
                                    id: resetStatus
                                    property bool ok: false
                                    property string text: ""
                                }
                                Connections {
                                    target: app
                                    function onLocalRustStoreResetResult(ok, message) {
                                        resetStatus.ok = ok
                                        resetStatus.text = message
                                    }
                                }
                                Dialog {
                                    id: resetConfirmDialog
                                    title: qsTr("Reset local Lightning session?")
                                    standardButtons: Dialog.Ok | Dialog.Cancel
                                    modal: true
                                    // Explicit bounded width: sizing this
                                    // dialog from its fixed-width content
                                    // fed implicitWidth back into itself
                                    // (a latent loop the runtime font
                                    // re-polish exposed).
                                    width: 420
                                    Label {
                                        width: 380
                                        wrapMode: Text.WordWrap
                                        text: qsTr(
                                            "This deletes Lightning's local Matrix Rust SDK " +
                                            "store and any saved smoke session for this " +
                                            "account. Server messages, Element data, and " +
                                            "other accounts are untouched. You will need to " +
                                            "sign in again after this.")
                                    }
                                    onAccepted: app.resetLocalRustStore()
                                }
                            }
                        }
                    }

                    // ════════════ Labs (design 1d) ════════════
                    ColumnLayout {
                        visible: root.section === "labs"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("Labs")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.ExtraBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("No experimental features are available in "
                                       + "this build. Diagnostics live here.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Backend: %1").arg(app.backendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    visible: app.syncModeLabel !== ""
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Sync mode: %1").arg(app.syncModeLabel)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Connection: %1").arg(app.connectionStatus)
                                }
                                AppButton {
                                    storm: true
                                    text: qsTr("Refresh current room")
                                    enabled: app.currentRoomId !== ""
                                    onClicked: app.reloadCurrentRoomTimeline(50)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("Rebuilds the open room's timeline from the "
                                               + "SDK. Safe at any time.")
                                }
                            }
                        }
                    }

                    // ════════════ About ════════════
                    ColumnLayout {
                        visible: root.section === "about"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("About")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Lightning %1").arg(app.appVersion)
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.fontBody
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("A native C++/Qt Matrix desktop client. "
                                               + "No Electron, no web view.")
                                }
                                Label {
                                    visible: app.backendName === "rust"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    // Pinned in rust/Cargo.toml; update together.
                                    text: qsTr("Matrix engine: matrix-sdk 0.18.0 / matrix-sdk-ui 0.18.0 (Rust)")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontCaption
                                    text: qsTr("License: GPL-3.0-or-later")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
