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
        { title: qsTr("Interface zoom"),
          keywords: qsTr("interface zoom scale bigger ui size"),
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

        { title: qsTr("Microphone"),
          keywords: qsTr("microphone mic input device voice call audio"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Voice & video"),
          control: "callDevice_microphone" },
        { title: qsTr("Output device"),
          keywords: qsTr("speaker output headphones device voice call audio"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Voice & video"),
          control: "callDevice_speaker" },
        { title: qsTr("Camera"),
          keywords: qsTr("camera webcam video device call"),
          section: "notifications",
          breadcrumb: qsTr("Notifications · Voice & video"),
          control: "callDevice_camera" },
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

        { title: qsTr("Share my online status"),
          keywords: qsTr("presence online idle offline status share"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Presence") },

        { title: qsTr("Ignored users"),
          keywords: qsTr("ignore ignored block user mute person hide"),
          section: "privacy",
          breadcrumb: qsTr("Privacy & security · Ignored users") },
        { title: qsTr("Sign out other sessions"),
          keywords: qsTr("sessions devices sign out remove device delete"),
          section: "sessions",
          breadcrumb: qsTr("Sessions") },

        { title: qsTr("Automatically load previews in unencrypted rooms"),
          keywords: qsTr("link preview privacy"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews"),
          control: "autoLoadLinkPreviews" },
        { title: qsTr("Load previews in encrypted rooms"),
          keywords: qsTr("link preview encrypted"), section: "privacy",
          breadcrumb: qsTr("Privacy & security · Link previews") },
        { title: qsTr("Autoplay and prefetch media"),
          keywords: qsTr("gif autoplay prefetch video audio media"),
          section: "privacy", breadcrumb: qsTr("Privacy & security · Media") },
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

        { title: qsTr("Updates"),
          keywords: qsTr("update version upgrade check download install"),
          section: "updates", breadcrumb: qsTr("Updates") },
        { title: qsTr("Automatically check for updates"),
          keywords: qsTr("update automatic check background"),
          section: "updates", breadcrumb: qsTr("Updates · Automatic checks") },
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

    // Indeterminate spinner.
    //
    // Basic's BusyIndicator inks palette.dark, which Main.qml maps to
    // AppTheme.textSecondary — so every loading state in the app was drawn
    // in the theme's secondary TEXT colour and never read as an active
    // state. This is a ring in the accent with a travelling head.
    component StormSpinner: Item {
        id: spinner
        property color ink: AppTheme.bolt
        property bool running: true
        property int diameter: 16
        implicitWidth: diameter
        implicitHeight: diameter
        visible: running
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: Qt.alpha(spinner.ink, 0.22)
        }
        Item {
            anchors.fill: parent
            transformOrigin: Item.Center
            Rectangle {
                // Centred ON the 2px ring stroke, not inside it.
                width: 6; height: 6; radius: 3
                color: spinner.ink
                anchors.horizontalCenter: parent.horizontalCenter
                y: -2
            }
            RotationAnimator on rotation {
                // Reduced motion keeps the ring and its head, static: the
                // state is still legible, it just does not travel.
                running: spinner.running && spinner.visible
                         && !AppTheme.reducedMotion
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }
        }
    }

    // Group label above a cluster of controls ("Theme", "Text size", …).
    //
    // These were JetBrains Mono, 10px, DemiBold, ALL CAPS at 1.6px tracking
    // — decorative HUD typography carrying wayfinding text, re-typed inline
    // six times in this file. Mono earns its place on code, keycaps and
    // Matrix identifiers; it does not earn it on "Text size". The
    // menuSection* tokens are the replacement recipe (UI face, 12px, 600,
    // no tracking, sentence case), so restyling it now happens in AppTheme
    // rather than in six places here. Ink is the muted role, not the faint
    // one: faint is the DISABLED ink and sits below AA on several presets,
    // and these labels are load-bearing.
    component SettingsGroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }

    // Reusable confirmation dialog.
    //
    // main.cpp sets QQuickStyle "Basic", whose Dialog is a square-cornered
    // Rectangle in palette.window outlined in palette.dark (a BODY-TEXT
    // ink), footed by DialogButtonBox's stock 100x40 square buttons. Under
    // Storm palette.window resolves to stormDeep — the exact colour this
    // screen already paints itself — so a confirm dialog used to be a
    // square outline floating on an identical background, next to an app
    // whose every other surface is rounded. Worse, Basic's Button draws its
    // keyboard-focus border in palette.highlight, which Main.qml maps to
    // the same token as palette.button, so focus on those buttons was
    // literally invisible.
    //
    // So the chrome is declared once, here, and the footer is real
    // AppButtons: matched 32px height, one radius, hover/press/focus.
    // `confirmText`/`confirmKind` keep the destructive wording and the
    // danger skin at the call site.
    component ConfirmDialog: Dialog {
        id: confirmDialog
        property string confirmText: qsTr("Confirm")
        // "primary" for a benign commit, "dangerPrimary" for a destructive
        // one — a confirm button is the committed step, so the QUIET
        // "danger" outline kind is deliberately not what these use.
        property string confirmKind: "primary"
        anchors.centerIn: parent
        modal: true
        // Explicit width everywhere: sizing a Dialog from fixed-width
        // content feeds implicitWidth back into itself (a latent loop the
        // runtime font re-polish exposed on the reset dialog).
        width: 340
        padding: AppTheme.spacing20
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            radius: AppTheme.radiusLg
        }
        header: Label {
            text: confirmDialog.title
            visible: text.length > 0
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing20
            rightPadding: AppTheme.spacing20
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }
        footer: Item {
            implicitHeight: confirmFooter.implicitHeight + AppTheme.spacing20 * 2
            RowLayout {
                id: confirmFooter
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing20
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: confirmDialog.reject()
                }
                AppButton {
                    storm: true
                    kind: confirmDialog.confirmKind
                    text: confirmDialog.confirmText
                    onClicked: confirmDialog.accept()
                }
            }
        }
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
        // v0.7.x: a small attention dot on the row that leads to the thing
        // needing attention. Dismissible (see sessionVerificationWarning),
        // so an account the user has consciously left unverified stops
        // being nagged about it.
        property bool alert: false
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
                font.pixelSize: AppTheme.textBody
                font.weight: navRow.highlighted ? AppTheme.weightBold : AppTheme.weightStrong
                elide: Label.ElideRight
            }
            Rectangle {
                objectName: "settingsNavAlert_" + navRow.sectionKey
                visible: navRow.alert
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: AppTheme.spacing4
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: AppTheme.stormDanger
                Accessible.role: Accessible.Indicator
                Accessible.name: qsTr("Needs attention")
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
        if (key === "updates") return qsTr("Updates")
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
        // Reuses the existing verified "download" glyph (Icon.qml) rather
        // than inventing an unverified codepoint — see the round's
        // completion report.
        if (key === "updates") return "download"
        if (key === "about") return "info"
        return "settings"
    }

    // v0.6.6: human-readable byte size for the Starred GIFs summary row.
    // Mirrors GifPicker.qml's own formatBytes() (kept local/duplicated
    // rather than shared: both are tiny, presentation-only, and each file
    // already owns the rest of its own formatting conventions).
    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
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
        // Qt dispatches QEvent::Shortcut BEFORE the key ever reaches the
        // focused item, so an unconditional window-scoped Escape here makes
        // every in-place editor's own Keys.onEscapePressed dead code — and
        // worse, pressing Escape to abandon an edit would leave Settings
        // entirely. Disabling it while an inline editor is open hands the key
        // back to that editor, which is the same guard TimelinePane already
        // applies for its pinned toolbar and picker. This codebase has been
        // bitten once before by a window-level Shortcut swallowing a key the
        // focused item needed (a "Space pauses media" Shortcut silently broke
        // timeline paging and the emoji grids).
        enabled: !accountIdentityCard.editingDisplayName
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

    // Confirmation before clearing local GIF collections. `kind` keeps the
    // store-accurate token ("favorites" is app.gif.favorites, the provider
    // bookmarks); only the prose speaks the v0.6.7 "saved" vocabulary. The two
    // halves of the picker's Saved tab clear separately here because only one
    // of them holds real bytes — see the starred-GIF block further down.
    ConfirmDialog {
        id: gifClearConfirm
        property string kind: ""
        function open(k) { kind = k; title = k === "favorites"
            ? qsTr("Clear saved provider GIFs?") : qsTr("Clear recent GIFs?"); visible = true }
        confirmText: qsTr("Clear")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: gifClearConfirm.kind === "favorites"
                ? qsTr("Remove every provider GIF you've saved on this device? "
                       + "GIFs you saved out of chats are unaffected. This "
                       + "cannot be undone.")
                : qsTr("Clear the list of recently used GIFs on this device?")
        }
        onAccepted: {
            if (kind === "favorites") app.gif.favorites.clearAll()
            else if (kind === "recent") app.gif.recent.clearAll()
        }
    }

    // v0.6.6: confirmation before clearing the client-local starred-GIF
    // store (see GifStarredStore) — unlike Favorites/Recents this one holds
    // actual decrypted file bytes on disk, so the same confirmed-danger
    // pattern applies with its own dedicated dialog rather than reusing
    // gifClearConfirm's two-kind switch.
    ConfirmDialog {
        id: starredGifsClearConfirm
        objectName: "starredGifsClearConfirm"
        title: qsTr("Clear images saved on this device?")
        confirmText: qsTr("Delete")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("Delete every image you saved out of a chat from this "
                       + "device? Saved provider GIFs are unaffected — they "
                       + "are only links. This cannot be undone.")
        }
        onAccepted: app.gif.starredStore.clearAll()
    }

    // v0.7.4: clearing the own display name is deliberate, never a silent
    // whitespace write — an emptied editor is REFUSED by AppController, and
    // removing the name is only reachable through this confirmation. Same
    // confirmed-consequence pattern as the two clears above.
    ConfirmDialog {
        id: displayNameClearConfirm
        objectName: "displayNameClearConfirm"
        title: qsTr("Clear your display name?")
        confirmText: qsTr("Clear name")
        confirmKind: "dangerPrimary"
        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("People will see your Matrix ID instead. You can set a "
                       + "new display name at any time.")
        }
        onAccepted: app.clearOwnDisplayName()
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
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
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
                            font.pixelSize: AppTheme.textMeta
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
                                            font.pixelSize: AppTheme.textBody
                                            font.weight: AppTheme.weightStrong
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: resultRow.modelData.breadcrumb
                                            color: AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.textMeta
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
                                        // A suggestion chip is ordinary UI
                                        // text ("theme", "notifications"),
                                        // not an identifier — mono belongs
                                        // on code, keycaps and Matrix IDs.
                                        font.pixelSize: AppTheme.textMeta
                                        font.weight: AppTheme.weightMedium
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
                        // Verification lives under Sessions, so this is the
                        // row the cog's badge is pointing at.
                        alert: app.sessionVerificationWarning
                    }
                    SettingsNavRow {
                        sectionKey: "labs"
                        iconName: "science"
                        navLabel: qsTr("Labs")
                    }
                    SettingsNavRow {
                        sectionKey: "updates"
                        iconName: "download"
                        navLabel: qsTr("Updates")
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
                            // Was mono/uppercase/tracked micro text: a
                            // terminal readout for a plain result count.
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightMedium
                            color: AppTheme.stormTextMuted
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
            // Wrapped in a plain Item so the About page's Storm Band can
            // overlay the pane's BOTTOM edge (the reference mounts the band
            // absolutely against the host pane) while the Flickable keeps
            // its exact geometry. QML ignores indentation — the Flickable
            // body below is unchanged.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
            Flickable {
                id: contentFlick
                anchors.fill: parent
                contentHeight: contentColumn.implicitHeight + AppTheme.spacing24 * 2
                clip: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the room timeline (see
                // qml/SmoothWheelArea.qml) — the maintainer report this
                // round was specifically that Settings scrolled "slower
                // and different" than the chat box.
                SmoothWheelArea { id: settingsWheelArea }
                // Jump to the top when switching categories. Stop any
                // in-flight glide first so a residual wheel motion from
                // the previous section cannot immediately fight this jump.
                Connections {
                    target: root
                    function onSectionChanged() {
                        settingsWheelArea.stopGlide()
                        contentFlick.contentY = 0
                    }
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: -AppTheme.spacing8
                            text: qsTr("Theme, message layout and text size — per account.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        SettingsGroupLabel { text: qsTr("Theme") }
                        // Four featured design themes. Every swatch is read
                        // LIVE from AppTheme.paletteForTheme(id) — the card
                        // holds no colour of its own.
                        //
                        // It used to: Storm read the palette while the other
                        // three carried hand-copied literals, and by the time
                        // anyone looked they had drifted so far that Indigo
                        // Night and Deep Teal previewed a room list LIGHTER
                        // than their canvas when both actually ship it
                        // darker. A theme picker that misrepresents its
                        // themes is worse than one with no preview, and the
                        // only fix that cannot rot is to stop duplicating
                        // the palette. One mapping, applied to all four:
                        // frame = background, rail = sidebar (the room list,
                        // which is the strip the preview draws), bar1 =
                        // border, bar2 = surface, accent = accent.
                        Flow {
                            id: featuredThemeFlow
                            objectName: "featuredThemeFlow"
                            Layout.fillWidth: true
                            spacing: 14
                            // paletteForTheme() returns the RAW per-theme
                            // literals regardless of which theme is active,
                            // so a card previews its own theme, never the
                            // current one.
                            function previewFor(id, name) {
                                var p = AppTheme.paletteForTheme(id)
                                return { id: id, name: name,
                                         frame: p.background, rail: p.sidebar,
                                         bar1: p.border, bar2: p.surface,
                                         accent: p.accent }
                            }
                            Repeater {
                                // Indigo Night leads: it is the flagship, on
                                // the maintainer's call. Storm is still
                                // featured — it is the brand theme and the
                                // shell's own chrome is built on it — but it
                                // is no longer the first thing offered.
                                model: [
                                    featuredThemeFlow.previewFor(9,  qsTr("Indigo Night")),
                                    featuredThemeFlow.previewFor(8,  qsTr("Moss Light")),
                                    featuredThemeFlow.previewFor(10, qsTr("Deep Teal")),
                                    featuredThemeFlow.previewFor(11, qsTr("Storm")),
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
                                    radius: AppTheme.radiusLg
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
                                        topLeftRadius: AppTheme.radiusLg - 1
                                        topRightRadius: AppTheme.radiusLg - 1
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
                                        bottomLeftRadius: AppTheme.radiusLg - 1
                                        bottomRightRadius: AppTheme.radiusLg - 1
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
                                                font.pixelSize: AppTheme.textBody
                                                font.weight: AppTheme.weightStrong
                                                elide: Label.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }

                                    // The card outline, above the edge-
                                    // filling children so it always renders
                                    // (SPEC 1v: an accent edge when
                                    // selected — at an INTEGER weight; 1.5px
                                    // antialiases into two half-covered rows
                                    // at DPR 1.0 and resolves unpredictably
                                    // at the 1.25/1.5 ratios common on
                                    // Windows and KDE).
                                    Rectangle {
                                        anchors.fill: parent
                                        z: 5
                                        radius: AppTheme.radiusLg
                                        color: "transparent"
                                        border.width: themeCard.selectedTheme ? 2 : 1
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
                        SettingsGroupLabel { text: qsTr("More themes") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Repeater {
                                // 8-11 are the featured cards above; 12 has
                                // its own row below with the editor attached,
                                // so a mini card for it would be a second
                                // control for the same thing.
                                model: AppTheme.themeList.filter(
                                    (t) => t.id !== 8 && t.id !== 9 && t.id !== 10
                                           && t.id !== 11 && t.id !== 12)
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
                                    radius: AppTheme.radiusTile
                                    // Hover was the same token as selected
                                    // here too — see the font rows below.
                                    color: selectedTheme ? AppTheme.stormSelection
                                           : miniHover.hovered
                                             ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                             : AppTheme.stormInset
                                    // Integer border: 1.5px cannot land on a
                                    // pixel boundary at DPR 1.0 and renders
                                    // as two half-covered rows.
                                    border.width: selectedTheme ? 2 : 0
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
                                            font.pixelSize: AppTheme.textMeta
                                            font.weight: AppTheme.weightStrong
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

                        // ── Custom theme ────────────────────────────────
                        // Offered as its own row rather than as a twelfth
                        // card: it has no fixed palette to preview until the
                        // user has made one, so a card would show either a
                        // blank swatch or a copy of whatever it was forked
                        // from.
                        SettingsGroupLabel { text: qsTr("Custom theme") }
                        Rectangle {
                            id: customThemeRow
                            objectName: "customThemeRow"
                            Layout.fillWidth: true
                            implicitHeight: customThemeLayout.implicitHeight
                                            + AppTheme.spacing12 * 2
                            radius: AppTheme.radiusTile
                            readonly property bool selectedTheme:
                                app.settings.theme === 12
                            color: selectedTheme ? AppTheme.stormSelection
                                                 : AppTheme.stormInset
                            border.width: selectedTheme ? 2 : 0
                            border.color: AppTheme.bolt

                            RowLayout {
                                id: customThemeLayout
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing12
                                spacing: AppTheme.spacing12

                                // Live swatch strip: the shell regions in
                                // window order, so the row reads as a theme
                                // rather than as a settings toggle.
                                Row {
                                    spacing: 2
                                    Repeater {
                                        model: app.customTheme.exists
                                               ? ["rail", "sidebar", "background",
                                                  "surface", "accent"]
                                               : []
                                        delegate: Rectangle {
                                            required property string modelData
                                            width: 10
                                            height: 28
                                            radius: 2
                                            color: {
                                                var o = app.customTheme.colors
                                                if (o && o[modelData] !== undefined)
                                                    return o[modelData]
                                                var pal = AppTheme.paletteForTheme(
                                                    app.customTheme.baseTheme)
                                                return pal[modelData] !== undefined
                                                       ? pal[modelData]
                                                       : AppTheme.stormTextMuted
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Label {
                                        // The ACTIVE theme's own name once it
                                        // has one: a person with four themes
                                        // needs the card to say which one is
                                        // in the window.
                                        text: !app.customTheme.exists
                                              ? qsTr("Build your own theme")
                                              : app.customTheme.name.length > 0
                                                ? app.customTheme.name
                                                : qsTr("Your theme")
                                        color: AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: !app.customTheme.exists
                                              ? qsTr("Pick a colour for any part of the window and watch a sample room repaint.")
                                              : app.customTheme.themes.length > 1
                                                ? qsTr("%n colour(s) changed. %1 themes saved.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                                      .arg(app.customTheme.themes.length)
                                                : qsTr("%n colour(s) changed. Pick a colour for any part of the window.",
                                                       "custom theme summary",
                                                       app.customTheme.overrideCount)
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                }

                                AppButton {
                                    objectName: "customThemeEditButton"
                                    kind: app.customTheme.exists ? "secondary"
                                                                 : "primary"
                                    storm: true
                                    text: app.customTheme.exists ? qsTr("Edit")
                                                                 : qsTr("Create")
                                    // Opening the editor no longer SELECTS
                                    // the theme. The preview inside it paints
                                    // the custom palette resolved by id, not
                                    // the live one, so a theme can be built
                                    // and looked at before it takes over the
                                    // window — and the editor carries its own
                                    // "Use this theme" button for when it
                                    // should. Forcing the switch here meant
                                    // opening the editor to LOOK at a theme
                                    // repainted the whole application.
                                    onClicked: themeEditorLoader.active = true
                                }
                            }
                        }

                        // Loaded on demand: the editor carries a full preview
                        // shell and a colour dialog, and Appearance is opened
                        // far more often than a theme is authored.
                        Loader {
                            id: themeEditorLoader
                            objectName: "themeEditorLoader"
                            active: false
                            sourceComponent: ThemeEditorDialog {}
                            onLoaded: item.open()
                            Connections {
                                target: themeEditorLoader.item
                                function onClosed() {
                                    themeEditorLoader.active = false
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
                                // Visually AppSwitch, because it IS the same
                                // control. It cannot BE an AppSwitch: the
                                // whole row is the click target (spec), and
                                // AppSwitch brings its own TapHandler and tab
                                // stop — TapHandlers are non-exclusive across
                                // subtrees, so nesting one inside this
                                // AbstractButton would toggle twice per click
                                // and cancel itself out. What it must never
                                // do again is drift: the off track used to be
                                // stormTextFaint here and stormBorderStrong
                                // in AppSwitch, so this one switch rendered a
                                // visibly lighter off state than the switches
                                // directly above and below it on the same
                                // page.
                                Rectangle {
                                    objectName: "matchSystemTrack"
                                    readonly property bool hot:
                                        matchSystemSwitch.hovered
                                        || matchSystemSwitch.down
                                    implicitWidth: 36
                                    implicitHeight: 20
                                    radius: AppTheme.radiusPill
                                    color: {
                                        if (app.settings.theme === 0)
                                            // accentHover, not a storm*-named
                                            // token: `bolt` IS the routed
                                            // accent, and the storm palette
                                            // maps accentHover to
                                            // _stoAccentHover, so this is the
                                            // hovered bolt on Storm and each
                                            // legacy theme's own hover
                                            // elsewhere. The name this used
                                            // to carry does not exist on the
                                            // singleton, so it was silently
                                            // assigning undefined to a QColor.
                                            return hot ? AppTheme.accentHover
                                                       : AppTheme.bolt
                                        return hot ? Qt.lighter(
                                                         AppTheme.stormBorderStrong, 1.18)
                                                   : AppTheme.stormBorderStrong
                                    }
                                    Behavior on color {
                                        enabled: !AppTheme.reducedMotion
                                        ColorAnimation { duration: 120 }
                                    }
                                    Rectangle {
                                        width: 16; height: 16; radius: 8
                                        scale: matchSystemSwitch.down ? 1.12 : 1.0
                                        Behavior on scale {
                                            enabled: !AppTheme.reducedMotion
                                            NumberAnimation { duration: 90 }
                                        }
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
                                    font.pixelSize: AppTheme.textBody
                                }
                            }
                            background: Item {}
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -4
                                radius: AppTheme.radiusMd
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
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("When on, Lightning follows the system scheme: "
                                       + "Moss Light in light mode, Indigo Night in dark mode.")
                        }

                        SettingsGroupLabel { text: qsTr("Conversation list") }
                        RowLayout {
                            objectName: "roomNavigationLayoutCards"
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12

                            NavigationLayoutCard {
                                objectName: "navLayoutClassicCard"
                                Layout.fillWidth: true
                                variant: "classic"
                                title: qsTr("Classic")
                                subtitle: qsTr("One list, most recent first, "
                                               + "with message previews.")
                                current: app.settings.roomNavigationLayout === 0
                                onClicked: app.settings.roomNavigationLayout = 0
                            }
                            NavigationLayoutCard {
                                objectName: "navLayoutChannelsCard"
                                Layout.fillWidth: true
                                variant: "channels"
                                title: qsTr("Channels")
                                subtitle: qsTr("Every space as a collapsible "
                                               + "folder of its rooms.")
                                current: app.settings.roomNavigationLayout === 1
                                onClicked: app.settings.roomNavigationLayout = 1
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            // Says what the layout IS rather than what it
                            // cannot do. It used to warn that Channels fell
                            // back to Classic at Home; it no longer falls back
                            // anywhere, so the warning would be untrue and the
                            // shape is the thing worth stating instead.
                            text: qsTr("Channels lists every space you are in as "
                                       + "a folder, with the rooms it contains "
                                       + "underneath. Rooms in no space, and your "
                                       + "direct messages, stay together in Rooms.")
                        }

                        SettingsGroupLabel { text: qsTr("Message layout") }
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
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Bubbles applies to direct messages; rooms keep "
                                       + "the Modern rows. Compact tightens every timeline.")
                        }

                        SettingsGroupLabel { text: qsTr("Text size") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            // The A/A end caps are a deliberate size PAIR
                            // illustrating the slider's range; they are not
                            // scale sizes and must not be tokenised onto it.
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
                                    radius: AppTheme.radiusPill
                                    color: AppTheme.stormInset
                                    Rectangle {
                                        width: textScaleSlider.visualPosition
                                               * parent.width
                                        height: parent.height
                                        radius: AppTheme.radiusPill
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
                                    // 2026-08-15 report: the thumb is
                                    // ALWAYS white. The earlier
                                    // switch-thumb analogy flipped it to
                                    // boltInk past visualPosition 0.5, but
                                    // a slider thumb rides the fill's
                                    // BOUNDARY — it never sits fully on
                                    // the bolt fill — so past half range
                                    // the dark boltInk disc just read as a
                                    // disabled/grey handle on the navy
                                    // panel. White reads on the stormInset
                                    // groove, on the bolt fill edge, and
                                    // on every legacy palette.
                                    color: "#FFFFFF"
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
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Scales message and list text. Interface chrome "
                                       + "and icons keep their size.")
                        }

                        // ── Interface zoom (whole-UI scale via
                        // QT_SCALE_FACTOR; startup-applied, hence the
                        // restart caption — Qt reads the factor once) ───
                        SettingsGroupLabel { text: qsTr("Interface zoom") }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Slider {
                                id: interfaceZoomSlider
                                objectName: "interfaceZoomSlider"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 320
                                from: 75
                                to: 150
                                stepSize: 5
                                snapMode: Slider.SnapAlways
                                value: app.settings.interfaceZoom
                                onMoved: app.settings.interfaceZoom
                                         = Math.round(value)
                                Accessible.name: qsTr("Interface zoom")
                                background: Rectangle {
                                    x: interfaceZoomSlider.leftPadding
                                    y: interfaceZoomSlider.topPadding
                                       + interfaceZoomSlider.availableHeight / 2
                                       - 2
                                    width: interfaceZoomSlider.availableWidth
                                    height: 4
                                    radius: AppTheme.radiusPill
                                    color: AppTheme.stormInset
                                    Rectangle {
                                        width: interfaceZoomSlider.visualPosition
                                               * parent.width
                                        height: parent.height
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.bolt
                                    }
                                }
                                handle: Rectangle {
                                    x: interfaceZoomSlider.leftPadding
                                       + interfaceZoomSlider.visualPosition
                                         * (interfaceZoomSlider.availableWidth
                                            - width)
                                    y: interfaceZoomSlider.topPadding
                                       + interfaceZoomSlider.availableHeight / 2
                                       - height / 2
                                    width: 16; height: 16; radius: 8
                                    // Always white — same 2026-08-15
                                    // correction as the text-size thumb
                                    // above: the boundary-riding thumb
                                    // never sits on the fill, and the
                                    // boltInk flip past 110% read as a
                                    // disabled handle.
                                    color: "#FFFFFF"
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.topMargin: 1
                                        anchors.bottomMargin: -1
                                        radius: 8
                                        z: -1
                                        color: "#40000000"
                                    }
                                    border.width: interfaceZoomSlider.visualFocus
                                                  ? 2 : 0
                                    border.color: AppTheme.bolt
                                }
                            }
                            Label {
                                text: app.settings.interfaceZoom + "%"
                                color: AppTheme.stormTextMuted
                                // Mono earns its place here: a live numeric
                                // readout that must not reflow as it counts.
                                font.pixelSize: AppTheme.textMeta
                                font.family: AppTheme.monoFont
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: AppTheme.spacing4
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Scales the entire interface — text, "
                                       + "icons and layout. Ctrl+= and Ctrl+- "
                                       + "adjust it anywhere. Takes effect the "
                                       + "next time Lightning starts.")
                        }

                        // ── v0.7: UI font (bundled OFL families) ────────
                        SettingsGroupLabel { text: qsTr("Font") }
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
                                        // Hover and selected used to return
                                        // the SAME token, so sweeping the
                                        // pointer down the list made the
                                        // selection appear to follow the
                                        // cursor — exactly the confusion a
                                        // selected state exists to prevent.
                                        // Same ladder AppComboBox uses:
                                        // selection at full strength, hover
                                        // at 55% of it.
                                        color: fontRow.selected
                                               ? AppTheme.stormSelection
                                               : (fontRow.hovered || fontRow.down)
                                                 ? Qt.alpha(AppTheme.stormSelection, 0.55)
                                                 : AppTheme.stormInset
                                        border.width: 1
                                        // Selection also carries a bolt edge,
                                        // not just a slightly stronger grey:
                                        // a 1px border-tone step was the only
                                        // thing separating the two states.
                                        border.color: fontRow.visualFocus
                                                      ? AppTheme.bolt
                                                      : fontRow.selected
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
                                                font.pixelSize: AppTheme.textBody
                                                font.weight: AppTheme.weightStrong
                                                color: AppTheme.stormText
                                            }
                                            // The sample previews the actual
                                            // family being offered.
                                            Label {
                                                text: qsTr("Messages, rooms and settings")
                                                font.family: fontRow.modelData
                                                font.pixelSize: AppTheme.textBody
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
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Applies to the whole interface. Code, Matrix "
                                       + "IDs, icons, and emoji keep their own fonts.")
                        }

                        // Panel visibility. Mirrors Ctrl+B / Ctrl+Shift+B, so
                        // a panel someone hides by shortcut can always be
                        // found again without knowing the shortcut.
                        SettingsGroupLabel { text: qsTr("Panels") }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showSpacesRailCheck"
                                    text: qsTr("Show the Spaces rail (Ctrl+Shift+B)")
                                    checked: app.settings.spacesRailVisible
                                    onToggled: app.settings.spacesRailVisible = checked
                                    Accessible.description: qsTr(
                                        "Show the narrow strip of Spaces down the far edge")
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "showRoomListCheck"
                                    text: qsTr("Show the room list (Ctrl+B)")
                                    checked: app.settings.roomListVisible
                                    onToggled: app.settings.roomListVisible = checked
                                    Accessible.description: qsTr(
                                        "Show the column of rooms and people")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Drag the line between two panels to "
                                               + "resize them. Widths are remembered.")
                                }
                            }
                        }

                        // System tray. The whole card is hidden where the
                        // platform has no tray: offering "close to tray" on a
                        // session without one would close the window into
                        // nothing.
                        SettingsGroupLabel {
                            visible: app.trayAvailable
                            text: qsTr("System tray")
                        }
                        SettingsCard {
                            visible: app.trayAvailable
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "closeToTrayCheck"
                                    text: qsTr("Keep running in the tray when the window is closed")
                                    checked: app.settings.closeToTray
                                    onToggled: app.settings.closeToTray = checked
                                }
                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    objectName: "startInTrayCheck"
                                    text: qsTr("Start in the tray")
                                    enabled: app.settings.closeToTray
                                    checked: app.settings.startInTray
                                    onToggled: app.settings.startInTray = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Click the tray icon to bring the window "
                                               + "back. Ctrl+Q quits.")
                                }
                            }
                        }

                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Timeline")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Hide routine joins, leaves, profile changes, "
                                               + "and room setting updates. Messages and "
                                               + "decryption warnings remain visible.")
                                }
                                Label {
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("Mouse-wheel speed")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
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
                                        syncToValue(app.settings.timelineWheelSpeed)
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("How far one physical mouse-wheel notch moves "
                                               + "the timeline. Touchpad and precision scrolling "
                                               + "stay fine-grained regardless of this setting.")
                                }
                            }
                        }

                        // Custom application icon: validated raster input,
                        // normalized to the circular presentation, applied to
                        // the running window immediately and restored at
                        // startup. Device-global (not per-account).
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Application icon")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    Image {
                                        objectName: "appIconPreview"
                                        source: app.appIconSource
                                        sourceSize.width: 48
                                        sourceSize.height: 48
                                        Layout.preferredWidth: 48
                                        Layout.preferredHeight: 48
                                        fillMode: Image.PreserveAspectFit
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name:
                                            qsTr("Current application icon")
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                        text: qsTr("Applies to the window and to task "
                                                   + "switchers that follow the running "
                                                   + "window. The desktop launcher keeps "
                                                   + "the packaged Lightning icon.")
                                    }
                                }
                                Label {
                                    id: customAppIconError
                                    objectName: "customAppIconError"
                                    property string message: ""
                                    visible: message.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.textMeta
                                    text: message
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "chooseAppIconButton"
                                        storm: true
                                        text: qsTr("Choose image…")
                                        onClicked: appIconDialog.open()
                                    }
                                    AppButton {
                                        objectName: "resetAppIconButton"
                                        storm: true
                                        visible: app.settings.customAppIconEnabled
                                        text: qsTr("Reset to Lightning default")
                                        onClicked: {
                                            customAppIconError.message = ""
                                            app.resetCustomAppIcon()
                                        }
                                    }
                                }
                                FileDialog {
                                    id: appIconDialog
                                    title: qsTr("Choose an application icon image")
                                    fileMode: FileDialog.OpenFile
                                    nameFilters: [
                                        qsTr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"),
                                        qsTr("All files (*)")
                                    ]
                                    onAccepted: customAppIconError.message =
                                        app.setCustomAppIconFromFile(selectedFile)
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
                                    font.pixelSize: AppTheme.textBody
                                }
                                AppComboBox {
                                    id: languageCombo
                                    objectName: "languageCombo"
                                    storm: true
                                    Layout.fillWidth: true
                                    model: app.localization.languages
                                    // The language's OWN name, never its
                                    // English one: a user who cannot read the
                                    // current UI language cannot find
                                    // "Russian" in a list either.
                                    textRole: "endonym"
                                    valueRole: "code"
                                    enabled: app.localization.translationsAvailable

                                    // indexOfValue() returns -1 at creation
                                    // time - the model and valueRole have not
                                    // settled - so the index is synced
                                    // explicitly here and again whenever
                                    // either side changes.
                                    function syncIndex() {
                                        syncToValue(app.localization.language)
                                    }
                                    Component.onCompleted: syncIndex()
                                    onModelChanged: Qt.callLater(syncIndex)
                                    Connections {
                                        target: app.localization
                                        function onLanguageChanged() {
                                            Qt.callLater(languageCombo.syncIndex)
                                        }
                                    }
                                    onActivated: app.localization.language = currentValue
                                    Accessible.name: qsTr("Interface language")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    // Three different truths, never conflated:
                                    // a build with no catalogs at all, a
                                    // "System default" that resolved to
                                    // something, and an explicit choice.
                                    text: {
                                        if (!app.localization.translationsAvailable)
                                            return qsTr("This build was compiled without translations, so the interface stays in English.")
                                        if (app.localization.language === "system")
                                            return qsTr("Following your desktop: %1.")
                                                .arg(app.localization.endonymOf(
                                                    app.localization.effectiveLanguage))
                                        return qsTr("The interface changes immediately. A few strings already on screen update when you next open their panel.")
                                    }
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Network privacy, encryption health, and recovery.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }

                        // v0.7.x Matrix presence: own-state publication.
                        // Offered only on a backend that owns presence
                        // (the server push-rules precedent); viewing
                        // others' presence is passive reads against the
                        // user's own homeserver and needs no toggle.
                        // Gated on backend CAPABILITY (supported), not on
                        // the read-refusal latch (active): publication
                        // keeps running when the server refuses reads, so
                        // the only control that stops it must never
                        // disappear (review M1).
                        Label {
                            visible: app.presence && app.presence.supported
                            text: qsTr("Presence")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            visible: app.presence && app.presence.supported
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                CheckBox {
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Share my online status")
                                    checked: app.settings.sharePresence
                                    onToggled: app.settings.sharePresence = checked
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Tells your homeserver when you are online or "
                                               + "idle, so people you share rooms with can see "
                                               + "it. Turning this off publishes offline once "
                                               + "and stops updates; whether others' status is "
                                               + "visible to you is decided by their servers, "
                                               + "not by this switch.")
                                }
                            }
                        }

                        // v0.7.x: ignored users (m.ignored_user_list —
                        // Matrix account data, shared with every client).
                        Label {
                            visible: app.moderation.supported
                            text: qsTr("Ignored users")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                            Layout.topMargin: AppTheme.spacing8
                        }
                        SettingsCard {
                            visible: app.moderation.supported
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8

                                Label {
                                    visible: app.moderation.ignoredUsers.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("Nobody is ignored. Ignore a "
                                               + "person from their profile "
                                               + "to hide their messages "
                                               + "everywhere, on every "
                                               + "device.")
                                }
                                Repeater {
                                    model: app.moderation.ignoredUsers
                                    delegate: RowLayout {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData
                                            color: AppTheme.stormText
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.textBody
                                            elide: Label.ElideRight
                                        }
                                        AppButton {
                                            storm: true
                                            implicitHeight: 26
                                            leftPadding: 10
                                            rightPadding: 10
                                            enabled: !app.moderation.busy
                                            text: qsTr("Stop ignoring")
                                            Accessible.name:
                                                qsTr("Stop ignoring %1")
                                                    .arg(modelData)
                                            onClicked: app.moderation
                                                .unignoreUser(modelData)
                                        }
                                    }
                                }
                                Label {
                                    visible: app.moderation.ignoredUsers.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Ignoring hides a person's "
                                               + "messages and invites in "
                                               + "every room. The list is "
                                               + "stored in your Matrix "
                                               + "account and applies on "
                                               + "all your clients.")
                                }
                            }
                        }

                        // v0.5.11: link-preview and GIF policy.
                        Label {
                            text: qsTr("Link previews & media")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textMeta
                                        text: qsTr("Loading a preview contacts the linked website "
                                                   + "directly — not through your homeserver — and "
                                                   + "may reveal your IP address and request "
                                                   + "timing to a site the sender chose. No "
                                                   + "JavaScript is executed. Both switches are "
                                                   + "off by default; leave them off and use each "
                                                   + "message's “Load link preview” action to "
                                                   + "decide one at a time.")
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }

                                Label { text: qsTr("Autoplay and prefetch media"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifAutoplayCombo
                                    objectName: "gifAutoplayCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Autoplay and prefetch media")
                                    model: [
                                        { label: qsTr("Always"),   value: 0 },
                                        { label: qsTr("On hover"), value: 1 },
                                        { label: qsTr("Never"),    value: 2 },
                                    ]
                                    // NOT a currentIndex binding, and NOT
                                    // `Math.max(0, indexOfValue(...))` either:
                                    // that idiom shipped in 2026-08-18 and did
                                    // not fix the report it was written for.
                                    // indexOfValue() is -1 at creation, and
                                    // max(0, -1) is row 0 -- so the combo went
                                    // on displaying "Always" whatever was
                                    // stored, which is what "GIF settings
                                    // reset every launch" looks like from the
                                    // outside. syncToValue retries the -1
                                    // instead of clamping it.
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifAutoplay)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifAutoplayChanged() {
                                            gifAutoplayCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifAutoplay = currentValue
                                }
                                Label {
                                    text: qsTr("Also governs passive downloads: GIF, video and audio prefetching. \"Never\" disables all of them.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    Layout.fillWidth: true
                                }

                                Label { text: qsTr("GIF safe search"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifRatingCombo
                                    objectName: "gifRatingCombo"
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
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifSafeSearch)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifSafeSearchChanged() {
                                            gifRatingCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifSafeSearch = currentValue
                                }

                                Label { text: qsTr("Preferred GIF provider"); color: AppTheme.stormTextSecondary }
                                AppComboBox {
                                    storm: true
                                    id: gifProviderCombo
                                    objectName: "gifProviderCombo"
                                    Layout.fillWidth: true
                                    textRole: "label"; valueRole: "value"
                                    Accessible.name: qsTr("Preferred GIF provider")
                                    model: [
                                        { label: "GIPHY", value: "giphy" },
                                        { label: "KLIPY", value: "klipy" },
                                    ]
                                    function syncFromSettings() {
                                        syncToValue(app.settings.gifPreferredProvider)
                                    }
                                    Component.onCompleted: syncFromSettings()
                                    Connections {
                                        target: app.settings
                                        function onGifPreferredProviderChanged() {
                                            gifProviderCombo.syncFromSettings()
                                        }
                                    }
                                    onActivated: app.settings.gifPreferredProvider = currentValue
                                }
                                // Honest per-provider availability.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: AppTheme.spacing4
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("GIF searches are sent directly to the "
                                               + "selected provider. Saved and recent "
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
                                        text: qsTr("Clear saved provider GIFs")
                                        enabled: app.gif.favorites.count > 0
                                        onClicked: gifClearConfirm.open("favorites")
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.stormBorder
                                }

                                // v0.6.6: client-local GIF starring. Unlike
                                // Favorites/Recents (small provider-CDN
                                // metadata rows) this store holds actual
                                // decrypted file bytes on this device — see
                                // GifStarredStore's header — so it gets its
                                // own visible count/size and confirmed
                                // Clear All, not folded into the row above.
                                Label {
                                    text: qsTr("Images saved from chats")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    Layout.topMargin: AppTheme.spacing4
                                }
                                Label {
                                    objectName: "starredGifsSummaryLabel"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("%1 image(s), %2 — kept on this "
                                               + "device only and removed "
                                               + "when you sign out of this "
                                               + "account.")
                                        .arg(app.gif.starredStore.count)
                                        .arg(root.formatBytes(
                                            app.gif.starredStore.totalBytes))
                                }
                                AppButton {
                                    objectName: "clearStarredGifsButton"
                                    storm: true
                                    kind: "danger"
                                    text: qsTr("Clear all images saved from chats")
                                    enabled: app.gif.starredStore.count > 0
                                    onClicked: starredGifsClearConfirm.open()
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("The sound plays only when a "
                                               + "notification is shown, so muted and "
                                               + "active rooms stay silent. Bursts are "
                                               + "coalesced into a single alert.")
                                }
                                // Voice & video devices. A separate component:
                                // this file is already one of the largest in
                                // the tree, and device pickers are a coherent
                                // unit of their own.
                                Label {
                                    Layout.topMargin: AppTheme.spacing12
                                    text: qsTr("Voice & video")
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightBold
                                }
                                CallDeviceSettings {
                                    objectName: "callDeviceSettings"
                                    Layout.fillWidth: true
                                    // Enumeration initialises Qt Multimedia,
                                    // so it waits until this section is
                                    // actually on screen.
                                    activated: visible
                                }

                                CheckBox {
                                    objectName: "ringForCallsCheck"
                                    palette.windowText: AppTheme.stormText
                                    text: qsTr("Ring for incoming voice calls")
                                    enabled: app.settings.notificationsEnabled
                                    checked: app.settings.ringForCalls
                                    onToggled:
                                        app.settings.ringForCalls = checked
                                    Accessible.description:
                                        qsTr("Repeat the call sound while an "
                                             + "incoming voice call is "
                                             + "ringing. Turning this off "
                                             + "still shows the call — it "
                                             + "only silences the ring.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    // Backend-honest: the Rust backend saves
                                    // per-room modes to the account's server
                                    // push rules; other backends keep them
                                    // device-local. The push-registration
                                    // sentence stays unconditional — that
                                    // remains true on every backend.
                                    text: (app.serverRoomNotificationModes
                                           ? qsTr("Per-room notification modes (set "
                                                  + "from Room information) are saved "
                                                  + "to your account's notification "
                                                  + "settings (server push rules). ")
                                           : qsTr("Per-room notification modes (set "
                                                  + "from Room information) apply to "
                                                  + "this device only — they are not "
                                                  + "server push rules. "))
                                          + qsTr("Push registration for "
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
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

                            // ── v0.7.4 own display name ────────────────
                            property bool editingDisplayName: false
                            function beginDisplayNameEdit() {
                                displayNameField.text =
                                    accountIdentityCard.accountDisplayName
                                app.dismissOwnDisplayNameError()
                                editingDisplayName = true
                                displayNameField.forceActiveFocus()
                                displayNameField.selectAll()
                            }
                            function cancelDisplayNameEdit() {
                                if (app.ownDisplayNameBusy) return
                                editingDisplayName = false
                                app.dismissOwnDisplayNameError()
                                displayNameField.text =
                                    accountIdentityCard.accountDisplayName
                            }
                            function commitDisplayName() {
                                if (app.ownDisplayNameBusy) return
                                var wanted = displayNameField.text.trim()
                                // Unchanged is not a save. Close the editor
                                // rather than sending a request whose only
                                // possible answer is the value the account
                                // already has — and note the server would
                                // answer it SUCCESSFULLY, so the registry
                                // would emit nothing and a UI that waited
                                // for accountsChanged would hang here.
                                if (wanted === accountIdentityCard.accountDisplayName) {
                                    editingDisplayName = false
                                    app.dismissOwnDisplayNameError()
                                    return
                                }
                                // A refusal (empty, over the ceiling, no
                                // session) leaves the editor open with the
                                // reason in app.ownDisplayNameError.
                                app.submitOwnDisplayName(wanted)
                            }
                            Connections {
                                target: app
                                // Server-CONFIRMED only. Both Save and
                                // Clear land here; nothing else closes the
                                // editor, so a failure can never look like
                                // a success.
                                function onOwnDisplayNameSaved() {
                                    accountIdentityCard.editingDisplayName = false
                                }
                                // A session teardown retires the write; the
                                // editor must not stay open over the next
                                // account's identity.
                                function onLoggedInChanged() {
                                    accountIdentityCard.editingDisplayName = false
                                }
                            }
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
                                                font.pixelSize: AppTheme.textTitle
                                                font.weight: AppTheme.weightBold
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
                                            font.pixelSize: AppTheme.textMeta
                                            elide: Label.ElideMiddle
                                        }
                                        Label {
                                            visible: app.backendName === "rust" && app.sessionDeviceId !== ""
                                            text: qsTr("Device %1").arg(app.sessionDeviceId)
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.monoFont
                                            font.pixelSize: AppTheme.textMeta
                                        }
                                    }
                                }
                                // ── v0.7.4 own display name, edited in
                                // place. Hidden entirely on a backend that
                                // cannot write a profile: the command
                                // returns void, so offering it there would
                                // leave the editor spinning with nothing
                                // left to answer it.
                                ColumnLayout {
                                    objectName: "ownDisplayNameSection"
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    visible: app.canEditOwnDisplayName
                                    Label {
                                        text: qsTr("Display name")
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        visible: !accountIdentityCard.editingDisplayName
                                        Label {
                                            objectName: "ownDisplayNameValue"
                                            Layout.fillWidth: true
                                            elide: Label.ElideRight
                                            // "Not set" is the honest empty
                                            // state — never the localpart,
                                            // which would make a cleared
                                            // name look like a set one.
                                            text: accountIdentityCard.accountDisplayName.length > 0
                                                  ? accountIdentityCard.accountDisplayName
                                                  : qsTr("Not set")
                                            color: accountIdentityCard.accountDisplayName.length > 0
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                        }
                                        AppButton {
                                            objectName: "editDisplayNameButton"
                                            storm: true
                                            text: qsTr("Edit")
                                            Accessible.name: qsTr("Edit display name")
                                            onClicked: accountIdentityCard.beginDisplayNameEdit()
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        visible: accountIdentityCard.editingDisplayName
                                        AppTextField {
                                            id: displayNameField
                                            objectName: "ownDisplayNameField"
                                            storm: true
                                            Layout.fillWidth: true
                                            enabled: !app.ownDisplayNameBusy
                                            placeholderText: qsTr("Your display name")
                                            Accessible.name: qsTr("Display name")
                                            // No maximumLength: it counts
                                            // UTF-16 code units, so a 255
                                            // cap there would cut an emoji
                                            // in half between its
                                            // surrogates. The ceiling is
                                            // enforced by code point in
                                            // AppController instead, and
                                            // shown by the counter below.
                                            onAccepted: accountIdentityCard.commitDisplayName()
                                            Keys.onEscapePressed: accountIdentityCard.cancelDisplayNameEdit()
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: AppTheme.textMeta
                                            readonly property int used:
                                                app.displayNameLength(displayNameField.text.trim())
                                            // Only near the ceiling: a
                                            // permanent counter on a field
                                            // nobody fills is noise.
                                            visible: used > app.ownDisplayNameMaxLength() - 40
                                            color: used > app.ownDisplayNameMaxLength()
                                                   ? AppTheme.stormDanger
                                                   : AppTheme.stormTextMuted
                                            text: qsTr("%1 / %2 characters")
                                                  .arg(used)
                                                  .arg(app.ownDisplayNameMaxLength())
                                        }
                                        Label {
                                            objectName: "ownDisplayNameError"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            visible: app.ownDisplayNameError.length > 0
                                            color: AppTheme.stormDanger
                                            font.pixelSize: AppTheme.textMeta
                                            text: app.ownDisplayNameError
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: AppTheme.spacing8
                                            AppButton {
                                                objectName: "saveDisplayNameButton"
                                                storm: true
                                                kind: "primary"
                                                text: app.ownDisplayNameBusy
                                                      ? qsTr("Saving…") : qsTr("Save")
                                                // Duplicate submissions are
                                                // refused by AppController
                                                // too; this only keeps the
                                                // pointer honest.
                                                enabled: !app.ownDisplayNameBusy
                                                         && displayNameField.text.trim().length > 0
                                                         && displayNameField.text.trim()
                                                            !== accountIdentityCard.accountDisplayName
                                                         && app.displayNameLength(
                                                                displayNameField.text.trim())
                                                            <= app.ownDisplayNameMaxLength()
                                                onClicked: accountIdentityCard.commitDisplayName()
                                            }
                                            AppButton {
                                                objectName: "cancelDisplayNameButton"
                                                storm: true
                                                text: qsTr("Cancel")
                                                enabled: !app.ownDisplayNameBusy
                                                onClicked: accountIdentityCard.cancelDisplayNameEdit()
                                            }
                                            Item { Layout.fillWidth: true }
                                            AppButton {
                                                objectName: "clearDisplayNameButton"
                                                storm: true
                                                kind: "danger"
                                                text: qsTr("Clear")
                                                visible: accountIdentityCard.accountDisplayName.length > 0
                                                enabled: !app.ownDisplayNameBusy
                                                onClicked: displayNameClearConfirm.visible = true
                                            }
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("To sign out, use the account menu at the "
                                               + "bottom of the sidebar.")
                                }
                            }
                        }
                        // Profile banner (MSC4427 over MSC4133 extended
                        // profile fields). Hidden outright when the BACKEND
                        // cannot read them: a control that cannot work is
                        // worse than no control.
                        //
                        // A homeserver that does not implement extended
                        // profiles is a different case and is DISCLOSED
                        // rather than hidden. Hiding it there answered the
                        // wrong question — the user has already seen the
                        // feature, tried it and been refused, and a surface
                        // that silently disappears at that moment tells them
                        // nothing about why. The account is asked once on
                        // open so the answer is known BEFORE a file is
                        // picked, instead of after an upload fails.
                        SettingsCard {
                            id: profileBannerCard
                            visible: app.banners && app.banners.available
                            readonly property bool serverSupports:
                                app.banners && app.banners.supported
                            readonly property string ownUserId:
                                app.accounts ? app.accounts.activeUserId : ""
                            Component.onCompleted: profileBannerCard.ask()
                            onOwnUserIdChanged: profileBannerCard.ask()
                            function ask() {
                                if (app.banners && app.banners.available
                                        && profileBannerCard.ownUserId !== "")
                                    app.banners.request(profileBannerCard.ownUserId)
                            }
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    text: qsTr("Profile banner")
                                    color: AppTheme.stormTextSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                Rectangle {
                                    objectName: "ownProfileBannerPreview"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.round(
                                        Math.max(60, width / 3))
                                    radius: AppTheme.radiusMd
                                    color: AppTheme.stormInset
                                    border.width: 1
                                    border.color: AppTheme.stormBorder
                                    clip: true
                                    Image {
                                        id: ownBannerImage
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        visible: status === Image.Ready
                                        readonly property string mxc: {
                                            if (!app.banners)
                                                return ""
                                            var _dep = app.banners.revision
                                            return app.banners.ownBanner
                                        }
                                        // A counter, never an assignment to
                                        // `source`: assigning a bound
                                        // property imperatively destroys the
                                        // binding, and this card would then
                                        // keep showing a banner the account
                                        // has since replaced or removed.
                                        property int resolveTick: 0
                                        source: {
                                            var _tick = resolveTick
                                            return mxc.length > 0
                                                && app.mediaBridge.supported
                                                ? app.mediaBridge.wideImageSource(mxc)
                                                : ""
                                        }
                                        Connections {
                                            target: app.mediaBridge
                                            enabled: ownBannerImage.mxc.length > 0
                                            function onMediaCached(key) {
                                                if (key.endsWith(":" + ownBannerImage.mxc)
                                                    && ownBannerImage.source.toString().length === 0)
                                                    ownBannerImage.resolveTick++
                                            }
                                        }
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible: !ownBannerImage.visible
                                        text: qsTr("No banner")
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                }
                                RowLayout {
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "chooseProfileBannerButton"
                                        storm: true
                                        text: qsTr("Choose image…")
                                        enabled: !app.banners.busy
                                                 && profileBannerCard.serverSupports
                                        onClicked: bannerFileDialog.open()
                                    }
                                    AppButton {
                                        objectName: "removeProfileBannerButton"
                                        storm: true
                                        kind: "danger"
                                        text: qsTr("Remove")
                                        visible: app.banners.ownBanner.length > 0
                                        enabled: !app.banners.busy
                                                 && profileBannerCard.serverSupports
                                        onClicked: app.banners.clearOwnBanner()
                                    }
                                }
                                Label {
                                    objectName: "profileBannerError"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.textMeta
                                    // Named causes get a sentence a person
                                    // can act on. A raw category told the
                                    // user "(unsupported)" for a homeserver
                                    // limitation they would reasonably read
                                    // as a rejected image — and they read it
                                    // exactly that way.
                                    text: {
                                        if (!profileBannerCard.serverSupports)
                                            return ""
                                        var e = app.banners.lastError
                                        if (e.length === 0)
                                            return ""
                                        if (e === "unsupported_image")
                                            return qsTr("That file is not an image Lightning can "
                                                        + "upload. PNG, JPEG, GIF, WebP and BMP "
                                                        + "work; the file's contents decide, not "
                                                        + "its name.")
                                        if (e === "forbidden")
                                            return qsTr("Your homeserver refused the banner.")
                                        return qsTr("The banner could not be saved (%1).").arg(e)
                                    }
                                }
                                // The homeserver's own answer, stated once,
                                // where the control used to be.
                                Label {
                                    objectName: "profileBannerUnsupported"
                                    Layout.fillWidth: true
                                    visible: !profileBannerCard.serverSupports
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Your homeserver does not support profile banners "
                                               + "yet. They need extended profile fields "
                                               + "(MSC4133), which most servers have not enabled. "
                                               + "Nothing is wrong with your image.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    // Said plainly, because a banner is
                                    // PUBLIC profile data — anyone who can see
                                    // the account can see it — and because it
                                    // is written under two names on purpose.
                                    text: qsTr("A wide image shown behind your profile card, "
                                               + "about 3:1. It is part of your public profile, "
                                               + "so anyone who can see your account can see it. "
                                               + "Saved under both the standard and the Commet "
                                               + "field names, so clients that already show "
                                               + "banners will show yours.")
                                }
                                FileDialog {
                                    id: bannerFileDialog
                                    title: qsTr("Choose a banner image")
                                    nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)")]
                                    // The URL goes across as-is;
                                    // ProfileBannerManager converts it.
                                    // Stripping "file://" here produced
                                    // "/C:/..." on Windows.
                                    onAccepted: app.banners.setOwnBanner(
                                        selectedFile.toString())
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Secret backend: %1").arg(app.settings.secretBackendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: app.settings.secretsAreSecure
                                    color: AppTheme.stormSuccess
                                    text: qsTr("Access tokens are stored via the system Secret Service. Logout clears them.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: !app.settings.secretsAreSecure
                                    color: AppTheme.stormDanger
                                    text: qsTr("Insecure fallback active: access tokens are stored in QSettings (plaintext). Install a Secret Service provider (e.g. gnome-keyring, KWallet with libsecret support) and restart to enable secure storage.")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Crypto backend: %1").arg(app.crypto.backendDescription)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        objectName: "cryptoHealthRefresh"
                                        text: qsTr("Refresh")
                                        color: AppTheme.stormLink
                                        font.pixelSize: AppTheme.textBody
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    StormSpinner {
                                        diameter: 14
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: app.cryptoBootstrap.phase
                                                   === CryptoBootstrapModel.Ready
                                               ? AppTheme.stormSuccess
                                               : app.cryptoBootstrap.needsRecoveryKey
                                                 ? AppTheme.bolt
                                                 : AppTheme.stormText
                                        font.pixelSize: AppTheme.textBody
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                            font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
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
                                        font.pixelSize: AppTheme.textBody
                                        visible: app.cryptoBootstrap.requestAttempts > 0
                                        text: qsTr("Verified sessions available: %1")
                                            .arg(app.cryptoBootstrap.eligibleDevices)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textBody
                                        text: qsTr("Backup key usable: %1").arg(
                                            app.cryptoHealth.keyBackupUsable
                                                ? qsTr("yes") : qsTr("no"))
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textBody
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("This account's Matrix sessions and device "
                                       + "verification.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
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
                        // crypto trust. v0.7.x: other sessions can be signed
                        // out through the reusable UIA flow (password
                        // accounts) or the account console (OAuth/MAS
                        // accounts, which have no password stage). A tile
                        // disappears only when the authoritative refetch
                        // confirms the deletion.
                        SettingsCard {
                            visible: app.backendName === "rust"
                            ColumnLayout {
                                id: sessionsListCard
                                width: parent.width
                                spacing: AppTheme.spacing8

                                // Sign-out outcome notice + OAuth console
                                // routing. The card is visibility-toggled
                                // (never unloaded), so a result arriving
                                // while another section is shown still
                                // lands here.
                                property string actionNotice: ""
                                property bool actionNoticeError: false
                                function currentDeviceId() {
                                    for (var i = 0; i < app.sessionDevices.length; ++i) {
                                        if (app.sessionDevices[i].isCurrent === true)
                                            return app.sessionDevices[i].deviceId
                                    }
                                    return ""
                                }
                                function signOutOne(deviceId) {
                                    actionNotice = ""
                                    if (app.activeAccountIsOAuth())
                                        app.uia.requestManagementUrl(deviceId)
                                    else
                                        app.uia.signOutDevices(
                                            [deviceId], currentDeviceId())
                                }
                                function signOutAllOthers() {
                                    actionNotice = ""
                                    if (app.activeAccountIsOAuth()) {
                                        app.uia.requestManagementUrl("")
                                        return
                                    }
                                    var ids = []
                                    for (var i = 0; i < app.sessionDevices.length; ++i) {
                                        var d = app.sessionDevices[i]
                                        if (d.isCurrent !== true)
                                            ids.push(d.deviceId)
                                    }
                                    app.uia.signOutDevices(ids, currentDeviceId())
                                }
                                Connections {
                                    target: app.uia
                                    function onSignOutFinished(ok, message) {
                                        sessionsListCard.actionNotice = message
                                        sessionsListCard.actionNoticeError = !ok
                                    }
                                    function onManagementUrlReady(url) {
                                        // The account console owns OAuth
                                        // session management; open it and
                                        // let Refresh pick up the result.
                                        app.media.openWebUrl(url)
                                        sessionsListCard.actionNotice = qsTr(
                                            "Manage this in the account "
                                            + "page that just opened, then "
                                            + "press Refresh here.")
                                        sessionsListCard.actionNoticeError = false
                                    }
                                }
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
                                        font.pixelSize: AppTheme.textBody
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormDanger
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("The session list could not be loaded.")
                                }
                                Label {
                                    visible: !app.sessionDevicesFailed
                                             && !app.sessionDevicesLoading
                                             && app.sessionDevices.length === 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textBody
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
                                                        font.pixelSize: AppTheme.textBody
                                                        font.weight: AppTheme.weightStrong
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
                                                    font.pixelSize: AppTheme.textMeta
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

                                            // v0.7.x: sign out THIS OTHER
                                            // session. Never offered for the
                                            // current one — that is the
                                            // normal Sign out flow with its
                                            // store cleanup.
                                            AppButton {
                                                objectName: "sessionSignOutButton_"
                                                            + modelData.deviceId
                                                visible: modelData.isCurrent !== true
                                                         && app.uia.supported
                                                storm: true
                                                kind: "danger"
                                                implicitHeight: 26
                                                leftPadding: 10
                                                rightPadding: 10
                                                enabled: !app.uia.busy
                                                         && !app.uia.challengeActive
                                                text: qsTr("Sign out")
                                                Accessible.name:
                                                    qsTr("Sign out session %1")
                                                        .arg(modelData.deviceId)
                                                onClicked:
                                                    sessionsListCard.signOutOne(
                                                        modelData.deviceId)
                                            }
                                        }
                                    }
                                }
                                Label {
                                    visible: sessionsListCard.actionNotice.length > 0
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: sessionsListCard.actionNoticeError
                                           ? AppTheme.stormDanger
                                           : AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textBody
                                    text: sessionsListCard.actionNotice
                                    Accessible.name: text
                                }
                                AppButton {
                                    objectName: "signOutOtherSessionsButton"
                                    visible: app.uia.supported
                                             && app.sessionDevices.length > 1
                                    storm: true
                                    kind: "danger"
                                    Layout.alignment: Qt.AlignLeft
                                    enabled: !app.uia.busy
                                             && !app.uia.challengeActive
                                    text: qsTr("Sign out all other sessions")
                                    Accessible.name: text
                                    onClicked: sessionsListCard.signOutAllOthers()
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textBody
                                    text: qsTr("Signing out a session may require "
                                               + "your account password. "
                                               + "Verification below always "
                                               + "requires explicit confirmation "
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                    }
                                    Item {
                                        visible: app.sessionTrustState !== "Verified"
                                        Layout.fillWidth: true
                                    }
                                }

                                // v0.7.x: the SAS/QR flow itself moved
                                // OUT of this page and into the focused
                                // centred modal declared once in Main.qml
                                // (VerificationDialog). Burying a
                                // two-device emoji comparison at the bottom
                                // of a scrolled settings page meant the
                                // emojis could be off-screen at the exact
                                // moment the user needed to read them.
                                //
                                // The dialog follows AppController's
                                // verification state, so this page starts a
                                // flow by asking the controller and nothing
                                // more — there is no second "is the dialog
                                // showing" opinion that could disagree.
                                // What stays here is the resting FACT about
                                // this session, which the page should state
                                // whether or not a flow is running.
                                Pane {
                                    objectName: "verificationStatusCard"
                                    Layout.fillWidth: true
                                    visible: app.cryptoHealth
                                             && app.cryptoHealth.cryptoSupported
                                             && !app.verificationActive
                                             && app.verificationState === ""
                                    background: Rectangle {
                                        color: AppTheme.stormPanel
                                        border.color:
                                            app.sessionVerificationNeeded
                                            ? AppTheme.stormDanger
                                            : AppTheme.stormBorder
                                        radius: AppTheme.radiusSm
                                    }
                                    RowLayout {
                                        width: parent.width
                                        spacing: AppTheme.spacing8
                                        Icon {
                                            name: app.sessionVerificationNeeded
                                                  ? "warning" : "verified_user"
                                            size: 18
                                            color: app.sessionVerificationNeeded
                                                   ? AppTheme.stormDanger
                                                   : AppTheme.stormSuccess
                                            visible: app.sessionTrustState === "Verified"
                                                     || app.sessionVerificationNeeded
                                        }
                                        Label {
                                            objectName: "verificationRestingStatus"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: app.sessionVerificationNeeded
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                            Accessible.role: Accessible.StaticText
                                            Accessible.name: text
                                            text: app.sessionVerificationNeeded
                                                ? qsTr("This session is not verified. Verify it "
                                                       + "to prove it is yours, so your other "
                                                       + "sessions share encryption keys with it.")
                                                : app.sessionTrustState === "Verified"
                                                  ? qsTr("This session is verified through Matrix "
                                                         + "cross-signing.")
                                                  : app.sessionTrustState === "Cross-signing unavailable"
                                                    ? qsTr("This account has no cross-signing identity "
                                                           + "yet, so there is nothing to verify "
                                                           + "against.")
                                                    : qsTr("Checking this session's verification "
                                                           + "state\u2026")
                                        }
                                        // NO Verify button here on purpose:
                                        // the TrustCard above already owns
                                        // the single start affordance on
                                        // crypto-capable backends (v0.6.5),
                                        // and this card is only ever shown
                                        // on those. A second identical
                                        // trigger a few hundred pixels
                                        // below it is exactly what that
                                        // decision avoided.
                                        //
                                        // Dismiss silences the BADGES (the
                                        // rail cog and the Sessions nav
                                        // dot) — never this card, which
                                        // keeps stating the fact. It is
                                        // cleared automatically once the
                                        // session verifies, so it can
                                        // never hide a later unverified
                                        // session.
                                        AppButton {
                                            storm: true
                                            objectName: "verificationDismissButton"
                                            text: qsTr("Stop reminding me")
                                            visible: app.sessionVerificationWarning
                                            onClicked: app.dismissVerificationWarning()
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr(
                                        "Some old messages may show \"[unable to decrypt yet]\" until " +
                                        "you restore your recovery key here, or until another " +
                                        "verified device shares the room keys.")
                                }

                                Label {
                                    text: qsTr("Recovery key or passphrase")
                                    font.weight: AppTheme.weightStrong
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    font.weight: AppTheme.weightStrong
                                    color: AppTheme.stormText
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
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
                                // Same treatment as the text-size slider
                                // right above it in Appearance: 4px, pill
                                // ends, bolt on stormInset. Left at Basic's
                                // default this filled in body-text grey.
                                ProgressBar {
                                    id: keyImportProgress
                                    Layout.fillWidth: true
                                    visible: importPanel.running
                                    indeterminate: app.roomKeyImportTotalCount === 0
                                    from: 0
                                    to: Math.max(1, app.roomKeyImportTotalCount)
                                    value: app.roomKeyImportImportedCount
                                    implicitHeight: 4
                                    background: Rectangle {
                                        implicitHeight: 4
                                        radius: AppTheme.radiusPill
                                        color: AppTheme.stormInset
                                    }
                                    contentItem: Item {
                                        implicitHeight: 4
                                        clip: true
                                        Rectangle {
                                            height: parent.height
                                            radius: AppTheme.radiusPill
                                            color: AppTheme.bolt
                                            width: keyImportProgress.indeterminate
                                                   ? parent.width * 0.35
                                                   : parent.width
                                                     * keyImportProgress.position
                                            SequentialAnimation on x {
                                                running: keyImportProgress.indeterminate
                                                         && keyImportProgress.visible
                                                         && !AppTheme.reducedMotion
                                                loops: Animation.Infinite
                                                NumberAnimation {
                                                    from: -keyImportProgress.width * 0.35
                                                    to: keyImportProgress.width
                                                    duration: 1100
                                                    easing.type: Easing.InOutQuad
                                                }
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
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
                                        font.weight: AppTheme.weightStrong
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
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
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
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
                                ConfirmDialog {
                                    id: resetConfirmDialog
                                    title: qsTr("Reset local Lightning session?")
                                    confirmText: qsTr("Reset")
                                    confirmKind: "dangerPrimary"
                                    // Wider than the shared 340 default:
                                    // this one carries five lines of
                                    // consequence copy the user has to
                                    // read before agreeing.
                                    width: 440
                                    Label {
                                        width: 380
                                        wrapMode: Text.WordWrap
                                        color: AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        lineHeight: AppTheme.lineHeightBody
                                        lineHeightMode: Text.ProportionalHeight
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
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("No experimental features are available in "
                                       + "this build. Diagnostics live here.")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Backend: %1").arg(app.backendName)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    visible: app.syncModeLabel !== ""
                                    color: AppTheme.stormTextMuted
                                    text: qsTr("Sync mode: %1").arg(app.syncModeLabel)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
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
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Rebuilds the open room's timeline from the "
                                               + "SDK. Safe at any time.")
                                }
                            }
                        }
                    }

                    // ════════════ Updates ════════════
                    // Own file (UpdatesSettingsSection.qml) rather than an
                    // inline block: it is a substantial, independently
                    // ownable surface. Visibility-toggled like every other
                    // pane here (never a Loader — see the file header
                    // comment), so its own local state (the failure-banner
                    // dismissal) survives switching to another category and
                    // back.
                    UpdatesSettingsSection {
                        visible: root.section === "updates"
                        Layout.fillWidth: true
                    }

                    // ════════════ About ════════════
                    ColumnLayout {
                        visible: root.section === "about"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12

                        Label {
                            text: qsTr("About")
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightStrong
                        }
                        SettingsCard {
                            ColumnLayout {
                                width: parent.width
                                spacing: AppTheme.spacing8
                                RowLayout {
                                    spacing: AppTheme.spacing12
                                    // The application logo (the custom icon
                                    // when one is set — About is an in-app
                                    // branding surface).
                                    Image {
                                        objectName: "aboutAppLogo"
                                        source: app.appIconSource
                                        sourceSize.width: 56
                                        sourceSize.height: 56
                                        Layout.preferredWidth: 56
                                        Layout.preferredHeight: 56
                                        fillMode: Image.PreserveAspectFit
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name: qsTr("Lightning logo")
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Lightning %1").arg(app.appVersion)
                                            color: AppTheme.stormText
                                            font.pixelSize: AppTheme.textBody
                                            font.weight: AppTheme.weightStrong
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            lineHeight: AppTheme.lineHeightBody
                                            lineHeightMode: Text.ProportionalHeight
                                            color: AppTheme.stormTextMuted
                                            text: qsTr("A native C++/Qt Matrix desktop client. "
                                                       + "No Electron, no web view.")
                                        }
                                    }
                                }
                                Label {
                                    visible: app.backendName === "rust"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    // Pinned in rust/Cargo.toml; update together.
                                    text: qsTr("Matrix engine: matrix-sdk 0.18.0 / matrix-sdk-ui 0.18.0 (Rust)")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    lineHeight: AppTheme.lineHeightBody
                                    lineHeightMode: Text.ProportionalHeight
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("License: GPL-3.0-or-later")
                                }
                            }
                        }

                        // review L5: reserve the Storm Band's height at the
                        // end of the About column so the content can always
                        // scroll clear of the overlay's opaque lower part
                        // on short windows.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 190
                        }
                    }
                }
            }

            // Storm Band — pinned to the very BOTTOM of the content pane,
            // exactly as the reference mounts it (absolute against the host
            // pane, dissolving upward through its alpha mask into whatever
            // is behind it). Input-transparent; About page only.
            // backdropColor MUST stay stormDeep (the page color this file
            // paints at its root): ThemeTokensTest bans the raw themed
            // background token in this file.
            StormBand {
                objectName: "aboutStormBand"
                visible: root.section === "about"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 190
                backdropColor: AppTheme.stormDeep
            }
            }
        }
    }
}
