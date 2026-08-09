import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.1: Discord-inspired multi-provider GIF picker shared by the room and
// thread composers. Density and interaction take cues from Discord, but the
// styling is Lightning's own (AppTheme). Bound entirely to app.gif — the
// controller owns provider selection, request lifecycle, debounce, pagination
// and safe-search; this component only presents them.
//
// Preview tiles load the provider's SMALL preview variant (a public CDN URL,
// no Matrix secret) directly; the actual GIF is downloaded and validated only
// when chosen, on its way into the Matrix attachment pipeline.
//
// v0.6.5 (SPEC §1n): width 330 (down from 460) with a dynamic 3-column grid,
// a picker-owned "GIF" header badge (distinct from the composer's pinned
// composerGifKeycap), a tile size overlay, and a "return to send" footer hint.
// The nine choose()/snapshot()/latch/reset invariants below are UNCHANGED.
//
// Storm skin (SPEC-storm-language §3): stormPanel chrome, storm search field,
// bolt-filled selected chip, stormInset category chips, bolt keyboard-selection
// ring on tiles, faint mono footer.
//
// ── v0.6.7 UX rework: ONE star, ONE saved list ──────────────────────────
//
// The maintainer's report: "the same star does two different things in two
// different spaces". It did. A star on a GIPHY/KLIPY tile bookmarked a
// provider GIF into Favorites; a star on a GIF in the chat timeline copied its
// bytes to disk and put it in a separate "Starred" tab. Same glyph, two verbs,
// two destinations. Worse, the navigation contradicted itself: Favorites and
// Recent were CROSS-provider lists presented as chips *underneath* a provider
// tab, so selecting KLIPY and then Favorites showed GIPHY favorites.
//
// Now:
//   - a star means exactly one thing everywhere, in the picker and on a chat
//     GIF: "save this GIF". Filled = saved. Pressing it again unsaves;
//   - there is exactly one place saved GIFs live: the Saved tab, backed by
//     GifSavedModel (a presentation merge — the two stores stay separate for
//     the security reasons documented in that class);
//   - the two nav strips collapsed into one row of peers: the two SOURCES
//     (GIPHY, KLIPY) and, past a divider, the two LISTS that were always
//     cross-provider anyway (Saved, Recent). The Favorites chip is gone;
//   - every tile carries a source tag (GIPHY / KLIPY / LOCAL) in the same
//     badge style as the size overlay, so a merged list still says exactly
//     where each GIF came from — and provider attribution stays provider-true
//     per tile rather than resting on one footer line.
AnchoredPopup {
    id: picker

    // "room" or "thread" — routes the eventual send; also isolates which
    // composer reopens focus. The active target closes the other picker.
    property string target: "room"
    signal gifChosen(var result)

    readonly property var gif: app.gif

    // The single selected tab. Either a real provider id from gif.providerIds
    // ("giphy"/"klipy") or one of the two local lists, "saved"/"recent".
    // Replaces the old two-property pair (a browse section plus a separate
    // "is the local tab showing" flag), which could express nonsense states —
    // a "favorites" section while the local tab was also active — and forced
    // every consumer to check both.
    property string tab: "giphy"
    // A provider tab searches, paginates and shows attribution; a local list
    // does none of those and never issues a request.
    readonly property bool providerTab: tab !== "saved" && tab !== "recent"

    readonly property var activeModel:
        tab === "saved" ? gif.saved
        : tab === "recent" ? gif.recent
        : gif.results

    // Attribution for the local lists, which can hold rows from EITHER
    // provider: every known provider's required credit, so nothing displayed
    // is left uncredited. Per-GIF brand accuracy comes from each tile's own
    // source tag (ruling R15 — provider-true, never a wrong brand).
    readonly property string allProviderAttribution: {
        var rev = cfgRevision
        return gif.providerIds.map(function(id) {
            return gif.providerAttribution(id)
        }).join(" · ")
    }

    // Human-readable byte size for the size overlay ("" when unknown — the
    // overlay is hidden in that case).
    function formatBytes(n) {
        if (!n || n <= 0) return ""
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    // The tile's source tag. "local" is not a provider — it is a GIF the user
    // saved out of a chat, which lives only on this device.
    function sourceLabel(provider) {
        if (provider === "local")
            return qsTr("Local")
        var name = gif.providerDisplayName(provider)
        return name.length > 0 ? name : provider
    }

    width: Math.min(330, parent ? parent.width - AppTheme.spacingM * 2 : 330)
    height: Math.min(520, parent ? parent.height - AppTheme.spacingM * 2 : 520)
    padding: AppTheme.spacingS
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // Send the exact tile the user acted on — never a re-resolved index.
    // `resultOrRow` is normally an already-captured result map: a mouse
    // click hands over the delegate's OWN current data (see tile.snapshot()
    // below), taken from the exact delegate the user clicked, so it can
    // never drift from what was on screen. Keyboard activation has no
    // delegate to snapshot from, so it passes a plain row number instead;
    // that row is resolved against activeModel IMMEDIATELY, in this same
    // call — never stored and resolved later. Storing a row for later
    // resolution is exactly what let a debounced search response replacing
    // the grid, or a saved/recent reorder, swap in a different item under a
    // stale currentIndex/grid.currentIndex.
    //
    // Reading activeModel (not gif.results) matters too: gif.results is the
    // browse grid, but Saved/Recent show a different model, and a row must
    // never be resolved against the wrong one.
    //
    // `activated` is a one-shot latch, not a per-input-path flag: every
    // activation surface (mouse click on a tile, Return/Enter on the grid)
    // calls this SAME function, so gating choose() itself — rather than,
    // say, disabling the MouseArea — closes every path with one guard
    // instead of one per input device. It matters because close() starts an
    // exit transition rather than tearing the popup down synchronously, so
    // a second activation (e.g. two clicks landing inside Qt's
    // double-click interval, which QML's MouseArea delivers as two separate
    // "clicked" signals) can still reach this function while the popup is
    // still visually closing. Reset only on the NEXT open, not on close, so
    // a close triggered by anything other than a genuine send (Escape,
    // press-outside) still leaves a fresh latch next time.
    property bool activated: false
    function choose(resultOrRow) {
        if (activated)
            return
        var result = (typeof resultOrRow === "number")
            ? activeModel.get(resultOrRow)
            : resultOrRow
        if (!result || !result.provider || !result.gifId)
            return
        activated = true
        picker.gifChosen(result)
        close()
    }

    // Is this exact GIF currently in a saved collection? Asked of the STORES,
    // never of a row's FavoriteRole.
    //
    // v0.6.7 review (H1, MUST FIX): GifStoredModel::data() answers
    // FavoriteRole with a constant `true` — "stored == favorited" — and
    // GifRecentModel does not override it. Reading that role meant every tile
    // on the Recent tab rendered as saved, announced "Remove from saved
    // GIFs", and on activation called toggleFavorite() which INSERTS: the
    // control said Remove and did Save, with no visible change afterwards.
    // The always-true role predates this round (v0.6.6's Recent chip had the
    // same defect under the "favorites" label); what this round did was
    // promote it into the ONE global saved vocabulary, where a lie is much
    // more expensive.
    //
    // A "local" row needs no lookup: it exists only as an entry in the
    // local-saved store, it is never recorded into Recents (see
    // GifSendController::startLocal, which deliberately keeps account-scoped
    // bytes out of the global recents store), and provider grids never carry
    // one — so wherever it can appear, it is saved by construction.
    function isSaved(provider, gifId) {
        if (!provider || !gifId)
            return false
        if (provider === "local")
            return true
        return gif.favorites.isFavorite(provider, gifId)
    }

    // isSaved() is a plain function call, so a binding on it establishes no
    // dependency of its own. Bumping this on every collection change is what
    // re-evaluates the tiles — the same mechanism cfgRevision uses below.
    // Both stores toggle by insert/remove, so `count` always moves.
    property int savedRevision: 0
    Connections {
        target: picker.gif.favorites
        function onCountChanged() { picker.savedRevision++ }
    }
    Connections {
        target: picker.gif.starredStore
        function onCountChanged() { picker.savedRevision++ }
    }

    // Availability re-resolves every time the picker opens, so a picker
    // first shown before configuration finished (or a newly created env
    // file) never sticks at "off". providerConfigurationChanged bumps
    // cfgRevision, which re-evaluates the providerConfigured() bindings.
    property int cfgRevision: 0
    Connections {
        target: picker.gif
        function onProviderConfigurationChanged() { picker.cfgRevision++ }
        // "The active target closes the other picker": the room composer and
        // the thread panel each own an independent GifPicker instance, but
        // both are bound to this one shared controller.
        // Popup.CloseOnPressOutside does NOT close a sibling reached without
        // a mouse press outside it — e.g. Tab + Space/Enter onto the other
        // composer's GIF button — so two pickers can legitimately both be
        // open, sharing one live results grid; a search/page/provider change
        // in either would then silently swap what the other is showing.
        // Whichever picker opens last wins: every other one closes itself
        // here (and resets, via onClosed below) before it can touch shared
        // state again.
        function onPickerOpenRequested(target) {
            if (target !== picker.target && picker.opened)
                picker.close()
        }
    }
    onAboutToShow: {
        gif.notifyPickerOpening(picker.target)
        gif.refreshProviderKeys()
        // AnchoredPopup performs the initial placement from its own
        // Connections — deliberately not an onAboutToShow handler there,
        // because this assignment would override it.
        tab = gif.providerId
        grid.currentIndex = -1
        activated = false
        if (gif.results.count === 0)
            gif.showTrending()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: gif.reset()
    onTabChanged: grid.currentIndex = -1

    // Select `value`, which is either a provider id or a local list id. Only a
    // real provider id may reach the controller's provider switch below —
    // "saved"/"recent" are not provider ids, so routing them there would be a
    // silent no-op at best, and neither list may ever trigger a request. That
    // switch is the one network-triggering call in this file, and the early
    // return above it is what keeps a local list entirely offline.
    function selectTab(value) {
        picker.tab = value
        if (value === "saved" || value === "recent")
            return
        picker.gif.setActiveProvider(value)
        // That call early-returns when the provider is unchanged (coming back
        // from Saved/Recent to the provider that was already active), so the
        // browse grid can legitimately still be empty at this point.
        if (picker.gif.results.count === 0)
            picker.gif.showTrending()
    }

    // Focus hand-off from a tab strip into the grid. The local lists have no
    // search field — the component that hands focus to the grid on every
    // provider tab — so Down on the tab strip is their equivalent entry point.
    function focusGridFromTabs() {
        if (picker.providerTab)
            return
        grid.forceActiveFocus()
        if (grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
    }

    // Toggle SAVED state for the EXACT tile the user acted on, without
    // sending. `result` is the tile's own captured snapshot (see
    // tile.snapshot() below) — never a row index re-resolved against
    // activeModel, which could have moved on by the time this runs.
    //
    // One user-visible verb, two backing stores, routed purely by the
    // snapshot's own `provider` field and never by re-deriving anything from a
    // row position: a "local" row is a byte copy in GifStarredStore, anything
    // else is a provider bookmark in GifFavoritesModel. The C++ side keeps its
    // store-accurate names (favorites/toggleFavorite) — they describe what
    // that store holds, not what the button says.
    function toggleSaved(result) {
        if (!result || !result.provider || !result.gifId)
            return
        if (result.provider === "local") {
            gif.starredStore.unstar(result.gifId)
            return
        }
        gif.toggleFavorite(result)
    }

    background: Rectangle {
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
        radius: AppTheme.radiusLg
    }

    contentItem: Item {

    ColumnLayout {
        id: pickerColumn
        anchors.fill: parent
        // Reserve the attribution footer's REAL height (it holds a ↵ keycap
        // taller than one caption line) — a fixed reserve let the footer
        // paint over the bottom row of GIF tiles.
        anchors.bottomMargin: footerRow.implicitHeight + AppTheme.spacing4 * 2
        spacing: AppTheme.spacingS

        // ── Row 1: search (provider tabs) or list title + "GIF" badge + close
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            // A local list has no search field: typing here would debounce
            // into a GIPHY/KLIPY request whose results the user cannot even
            // see, since the grid stays bound to the local model.
            Label {
                objectName: "gifListTitle"
                visible: !picker.providerTab
                Layout.fillWidth: true
                text: picker.tab === "saved" ? qsTr("Saved GIFs")
                                             : qsTr("Recently sent")
                color: AppTheme.stormText
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontChip + 1
                font.weight: Font.Bold
            }

            AppTextField {
                id: searchField
                objectName: "gifSearchField"
                visible: picker.providerTab
                Layout.fillWidth: true
                searchIcon: true
                clearButton: true
                storm: true
                placeholderText: qsTr("Search %1").arg(picker.gif.providerName)
                Accessible.name: qsTr("Search GIFs")
                selectByMouse: true
                onTextChanged: picker.gif.setQueryText(text)
                Keys.onDownPressed: {
                    grid.forceActiveFocus()
                    if (grid.currentIndex < 0 && grid.count > 0)
                        grid.currentIndex = 0
                }
                Keys.onReturnPressed: {
                    // Never send from here: setQueryText() is debounced, so
                    // an Enter pressed right after typing must not fall
                    // through to row 0 of the PREVIOUS query (or trending)
                    // before the new query has even been dispatched. Flush
                    // the pending query immediately and hand off to the
                    // grid — the same hand-off Down already does — so the
                    // user picks explicitly once real results for THIS
                    // query have landed.
                    picker.gif.searchNow(searchField.text)
                    grid.forceActiveFocus()
                    if (grid.currentIndex < 0 && grid.count > 0)
                        grid.currentIndex = 0
                }
            }

            // The picker's own inline "GIF" badge — mono, bordered, NOT the
            // same geometry/objectName as the composer's pinned
            // composerGifKeycap (ComposerQmlTest pins that one at 1.5px
            // border / radius 5 with its own objectName; this is a distinct,
            // independent chip).
            Rectangle {
                objectName: "gifPickerHeaderBadge"
                implicitWidth: gifHeaderBadgeLabel.implicitWidth + AppTheme.spacing8
                implicitHeight: 22
                radius: AppTheme.radiusMd
                color: "transparent"
                border.width: 1.5
                border.color: AppTheme.stormBorderStrong
                Label {
                    id: gifHeaderBadgeLabel
                    anchors.centerIn: parent
                    text: qsTr("GIF")
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontChip + 1
                    font.weight: Font.Bold
                    color: AppTheme.stormTextMuted
                }
            }

            IconButton {
                storm: true
                implicitWidth: 28; implicitHeight: 28
                radius: 6
                iconName: "close"
                iconSize: 16
                Accessible.name: qsTr("Close GIF picker")
                onClicked: picker.close()
            }
        }

        // ── The single nav row: sources, a divider, then your own lists.
        // Two SegmentedControls rather than one four-entry control, purely so
        // the divider can sit between the groups; both are bound to the same
        // `picker.tab`, so exactly one segment across the pair is ever
        // selected. (Adding separator support to the shared SegmentedControl
        // would have changed a component the Room Information tabs and the
        // Settings layout selector also use.)
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            SegmentedControl {
                id: providerTabs
                objectName: "gifProviderTabs"
                storm: true
                // cfgRevision re-evaluates enabled/tip after a key refresh;
                // unavailable providers are disabled with an explanation —
                // never a bare "(off)" suffix.
                model: {
                    var rev = picker.cfgRevision
                    return picker.gif.providerIds.map(function(id) {
                        var ok = picker.gif.providerConfigured(id)
                        return {
                            label: picker.gif.providerDisplayName(id),
                            value: id,
                            enabled: ok,
                            tip: ok ? "" : qsTr("Not configured — set an API "
                                                + "key to enable this provider"),
                        }
                    })
                }
                current: picker.tab
                onActivated: (value) => picker.selectTab(value)
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 18
                Layout.alignment: Qt.AlignVCenter
                color: AppTheme.stormBorderStrong
            }

            SegmentedControl {
                id: listTabs
                objectName: "gifListTabs"
                storm: true
                Layout.fillWidth: true
                // Neither list needs a key or a network request, so both are
                // always enabled — including when no provider is configured
                // at all.
                model: [
                    {
                        label: qsTr("Saved"),
                        value: "saved",
                        enabled: true,
                        tip: qsTr("Every GIF you've starred — from a provider "
                                  + "or from a chat"),
                    },
                    {
                        label: qsTr("Recent"),
                        value: "recent",
                        enabled: true,
                        tip: qsTr("GIFs you recently sent"),
                    },
                ]
                current: picker.tab
                onActivated: (value) => picker.selectTab(value)
            }

            // Down anywhere on the nav row (unhandled by an individual
            // segment button, so it propagates up here) enters the grid on a
            // local list — the entry point searchField provides on every
            // provider tab. A no-op on a provider tab.
            Keys.onDownPressed: picker.focusGridFromTabs()
        }

        // ── Category chips (client-side search shortcuts) ───────────
        Flow {
            objectName: "gifCategoryChips"
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS
            visible: picker.providerTab && searchField.text.length === 0
                     && picker.gif.configured
            Repeater {
                model: picker.gif.categories
                delegate: AbstractButton {
                    id: categoryChip
                    required property string modelData
                    text: modelData
                    // v0.6.7 review (L2): the old section-chip row carried a
                    // "Trending" chip, and deleting that row left NO way back
                    // to trending inside one picker session — openCategory()
                    // leaves the search field empty (so its clear button never
                    // appears), selectTab() only calls showTrending() on an
                    // empty grid, and setActiveProvider() early-returns for
                    // the already-active provider. The selected chip now
                    // toggles off, which restores the route and makes the
                    // current category visible at the same time.
                    readonly property bool selected:
                        picker.gif.mode === GifSearchController.Category
                        && picker.gif.query === modelData
                    implicitWidth: chipLabel.implicitWidth + 20
                    implicitHeight: 26
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    // v0.6.7 review (N8): CheckBox, not RadioButton — a radio
                    // group implies exactly one member is always selected, and
                    // these now toggle fully off (pressing the selected chip
                    // returns to trending).
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Category %1").arg(modelData)
                    Accessible.checked: categoryChip.selected
                    onClicked: {
                        if (categoryChip.selected)
                            picker.gif.showTrending()
                        else
                            picker.gif.openCategory(modelData)
                    }
                    contentItem: Label {
                        id: chipLabel
                        text: categoryChip.text
                        font.family: AppTheme.monoFont
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: categoryChip.selected ? AppTheme.boltInk
                                                     : AppTheme.stormTextMuted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: categoryChip.selected ? AppTheme.bolt
                             : categoryChip.hovered ? AppTheme.stormSelection
                             : AppTheme.stormInset
                        radius: AppTheme.radiusPill
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: AppTheme.radiusPill
                        color: "transparent"
                        border.color: AppTheme.bolt
                        border.width: 2
                        visible: categoryChip.visualFocus
                    }
                }
            }
        }

        // ── Result grid — 3 columns, sized dynamically off the picker's
        // own width (was a fixed 132px cell at the previous 460px width) ──
        GridView {
            id: grid
            objectName: "gifResultGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / 3)
            cellHeight: cellWidth
            cacheBuffer: cellWidth * 2   // bounded off-screen retention
            model: picker.activeModel
            currentIndex: -1
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            // A local list hides searchField (the component that hands focus
            // to this grid on every provider tab) along with the category
            // chips, so without this the grid would be entirely
            // keyboard-unreachable there. Reachable via Tab directly ONLY on
            // those tabs; provider tabs keep their existing searchField
            // hand-off as the entry point, unchanged. Seeding currentIndex on
            // focus-gain applies generally, matching what the Down/Return
            // hand-offs already do by hand.
            activeFocusOnTab: !picker.providerTab
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0)
                    currentIndex = 0
            }

            // A highlighted/keyboard-selected row is just an int — Qt does
            // not remap it when the model changes underneath. A full
            // replace (a fresh search/category/provider-switch landing, or
            // Saved/Recent reloading) invalidates every existing row, so drop
            // the highlight rather than let Return later resolve it against
            // unrelated content. A row shifting because something was
            // inserted/removed/moved AT OR BEFORE it is invalidated the same
            // way — a save/unsave re-prepend, for example. Pagination only
            // ever appends AFTER the current end, so it leaves an existing
            // highlight untouched (see onRowsInserted).
            Connections {
                target: picker.activeModel
                function onModelReset() { grid.currentIndex = -1 }
                function onRowsInserted(parent, first) {
                    if (first <= grid.currentIndex)
                        grid.currentIndex = -1
                }
                function onRowsRemoved(parent, first) {
                    if (first <= grid.currentIndex)
                        grid.currentIndex = -1
                }
                function onRowsMoved(parent, start) {
                    if (start <= grid.currentIndex)
                        grid.currentIndex = -1
                }
            }

            // Infinite scroll: only a network-backed provider tab paginates;
            // Saved and Recent are complete local lists.
            onContentYChanged: {
                if (!picker.providerTab || contentHeight <= 0)
                    return
                if (contentY + height > contentHeight - cellWidth * 2)
                    picker.gif.loadMore()
            }

            delegate: Item {
                id: tile
                width: grid.cellWidth
                height: grid.cellHeight
                required property int index
                required property string provider
                required property string gifId
                required property string title
                required property string rating
                required property string previewUrl
                required property string stillUrl
                required property string gifUrl
                required property int gifWidth
                required property int gifHeight
                required property int previewWidth
                required property int previewHeight
                required property bool favorite
                // v0.6.5 stretch: the sendable-variant byte size (0 = unknown),
                // surfaced by GifResultModel::BytesRole /
                // GifStoredModel::BytesRole — always present, never undefined.
                required property real gifBytes
                readonly property bool current: GridView.isCurrentItem
                // v0.6.7: ONE saved state for ONE star, asked of the stores —
                // see picker.isSaved() for why the row's own FavoriteRole is
                // NOT the oracle here. Reading savedRevision is what makes
                // this re-evaluate when either collection changes.
                readonly property bool saved: {
                    var rev = picker.savedRevision
                    return picker.isSaved(tile.provider, tile.gifId)
                }
                // A local row has no provider CDN URL (see GifStarredStore) —
                // its previewUrl/stillUrl are deliberately empty in the
                // persisted row, so the ONLY source of truth for where to load
                // it from is a re-validated lookup by content hash, done fresh
                // on every binding evaluation (never a path trusted from the
                // persisted index).
                readonly property string localSource:
                    tile.provider === "local"
                        ? picker.gif.starredStore.source(tile.gifId) : ""

                // The exact record this delegate is rendering right now,
                // captured from its OWN bound properties rather than
                // re-queried from the model by index. This is what a click
                // sends: it cannot drift from what is on screen under this
                // tile, regardless of what the model does afterward.
                function snapshot() {
                    return {
                        provider: tile.provider,
                        gifId: tile.gifId,
                        title: tile.title,
                        rating: tile.rating,
                        previewUrl: tile.previewUrl,
                        stillUrl: tile.stillUrl,
                        gifUrl: tile.gifUrl,
                        gifWidth: tile.gifWidth,
                        gifHeight: tile.gifHeight,
                        previewWidth: tile.previewWidth,
                        previewHeight: tile.previewHeight,
                        favorite: tile.favorite,
                        gifBytes: tile.gifBytes,
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: AppTheme.radiusThumb
                    color: AppTheme.stormInset
                    border.width: tile.current ? 2 : 0
                    border.color: AppTheme.bolt
                    clip: true

                    // Still fallback shows immediately; the animation plays on
                    // top once decoded, and only while the picker is visible.
                    Image {
                        anchors.fill: parent
                        source: tile.provider === "local"
                                ? tile.localSource : tile.stillUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        visible: anim.status !== AnimatedImage.Ready
                    }
                    AnimatedImage {
                        id: anim
                        anchors.fill: parent
                        source: tile.provider === "local"
                                ? tile.localSource : tile.previewUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        // Autoplay policy: 0 Always (while visible), 1 OnHover,
                        // 2 Never. Also paused when the picker is hidden or the
                        // tile scrolls far off-screen (GridView frees non-cached
                        // delegates).
                        playing: picker.visible && app.settings.gifAutoplay !== 2
                                 && (app.settings.gifAutoplay === 0
                                     || tileHover.hovered)
                        Accessible.role: Accessible.Button
                        Accessible.name: tile.title.length > 0
                            ? qsTr("GIF: %1").arg(tile.title) : qsTr("GIF")
                    }

                    // Choosing (send) is the tile body; the star saves/unsaves
                    // WITHOUT sending. The star is always actionable. Both
                    // routes pass the tile's OWN captured snapshot — never a
                    // row index re-resolved against activeModel later — so the
                    // action can never drift from what is on screen under this
                    // exact tile, the same invariant choose() already enforces.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            grid.currentIndex = tile.index
                            if (mouse.button === Qt.RightButton)
                                picker.toggleSaved(tile.snapshot())
                            else
                                picker.choose(tile.snapshot())
                        }
                    }

                    // Source tag, top-left — GIPHY / KLIPY / Local. Replaces
                    // the old "GIF" tile badge, which said nothing new inside
                    // a GIF picker. In the merged Saved list this is what
                    // tells the user which GIFs are provider bookmarks and
                    // which are copies living on their own device, and it
                    // keeps provider credit attached to the exact tiles it
                    // belongs to.
                    Rectangle {
                        objectName: "gifTileSourceBadge"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 4
                        radius: AppTheme.radiusSm
                        color: AppTheme.overlayScrim
                        implicitWidth: sourceBadgeLabel.implicitWidth + AppTheme.spacing6
                        implicitHeight: sourceBadgeLabel.implicitHeight + AppTheme.spacing2
                        Label {
                            id: sourceBadgeLabel
                            anchors.centerIn: parent
                            text: picker.sourceLabel(tile.provider)
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontMicro
                            font.weight: Font.Bold
                            font.capitalization: Font.AllUppercase
                            color: AppTheme.scrimInk
                        }
                    }

                    // Size overlay, bottom-left — only when known.
                    Rectangle {
                        visible: picker.formatBytes(tile.gifBytes).length > 0
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.margins: 4
                        radius: AppTheme.radiusSm
                        color: AppTheme.overlayScrim
                        implicitWidth: gifSizeBadgeLabel.implicitWidth + AppTheme.spacing6
                        implicitHeight: gifSizeBadgeLabel.implicitHeight + AppTheme.spacing2
                        Label {
                            id: gifSizeBadgeLabel
                            anchors.centerIn: parent
                            text: picker.formatBytes(tile.gifBytes)
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontMicro
                            font.weight: Font.Bold
                            color: AppTheme.scrimInk
                        }
                    }

                    // The one star. Saved state is a FILL, not just a tint:
                    // the bundled Material Symbols subset is a static FILL=0
                    // instance, so there is no filled star glyph to switch to
                    // and colour alone carried the whole state. The bolt fill
                    // + boltInk pairing matches MessageDelegate.qml's chat
                    // star exactly, so one state reads identically in both
                    // places it can appear.
                    ToolButton {
                        id: saveButton
                        objectName: "gifTileSaveButton"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        // v0.6.7 review (N7): inset by the same 4px the "GIF"
                        // and size badges use, so the focus ring below (which
                        // extends 3px outward) still lands inside this
                        // wrapper's clip region. Flush to the corner, the ring
                        // was clipped on its top and right edges and rendered
                        // as an L.
                        anchors.margins: 4
                        width: 24; height: 24
                        contentItem: Icon {
                            name: "star"
                            size: 15
                            color: tile.saved ? AppTheme.boltInk
                                              : AppTheme.scrimInk
                        }
                        // v0.6.7 (maintainer request): hover only — never a
                        // star parked on the artwork at rest. It matters most
                        // here: on the Saved tab EVERY tile is saved by
                        // definition, so an at-rest star put a badge on every
                        // single thumbnail while carrying no information.
                        //
                        // v0.6.7 review (M1): `visualFocus` is part of the
                        // reveal, not just hover and grid position. This is a
                        // focusable control, so Tab lands on it — without the
                        // focus term that put keyboard focus on a fully
                        // transparent button with no ring, while the chat
                        // star (which reveals on activeFocus) did not. Grid
                        // arrow-navigation reveals it too via tile.current,
                        // but that only makes it VISIBLE: the grid's own
                        // Return/Enter/Space all send, so Tab is the actual
                        // keyboard route to this button.
                        opacity: tileHover.hovered || tile.current
                                 || saveButton.visualFocus ? 1 : 0
                        Accessible.name: tile.saved
                            ? qsTr("Remove from saved GIFs")
                            : qsTr("Save GIF")
                        onClicked: picker.toggleSaved(tile.snapshot())
                        background: Rectangle {
                            radius: 12
                            color: tile.saved ? AppTheme.bolt
                                              : AppTheme.overlayScrim
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -3
                            radius: 15
                            color: "transparent"
                            border.color: AppTheme.bolt
                            border.width: 2
                            visible: saveButton.visualFocus
                        }
                    }
                    HoverHandler { id: tileHover }
                }
            }

            Keys.onReturnPressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onEnterPressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onSpacePressed: if (currentIndex >= 0) picker.choose(currentIndex)
        }
    }

    // ── State overlay (covers exactly the grid area — pickerColumn fills
    // this same contentItem at 0,0, so the grid's layout geometry is valid
    // in these coordinates; a fixed top offset drifted over the chip rows
    // whenever the header stack's height changed) ─────────────────────
    Item {
        x: grid.x
        y: grid.y
        width: grid.width
        height: grid.height
        visible: overlayText.text.length > 0 || busy.running

        BusyIndicator {
            id: busy
            anchors.centerIn: parent
            running: picker.providerTab
                     && picker.gif.state === GifSearchController.Loading
                     && picker.gif.results.count === 0
        }
        Label {
            id: overlayText
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacingL * 2
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: 13
            text: {
                // The local lists work even when no provider is configured or
                // available — they never depend on gif.available.
                if (picker.tab === "saved")
                    return picker.gif.saved.count === 0
                        ? qsTr("No saved GIFs yet. Press the star on any GIF — "
                               + "here or in a chat — to keep it.")
                        : ""
                if (picker.tab === "recent")
                    return picker.gif.recent.count === 0
                        ? qsTr("No recent GIFs yet.") : ""
                if (!picker.gif.available)
                    return qsTr("GIFs are unavailable on this backend.")
                var s = picker.gif.state
                if (s === GifSearchController.MissingKey)
                    return qsTr("%1 is not configured. Set its API key to browse GIFs.")
                        .arg(picker.gif.providerName)
                if (s === GifSearchController.Offline)
                    return qsTr("You appear to be offline.")
                if (s === GifSearchController.RateLimited)
                    return qsTr("The GIF provider is rate limiting requests. Try again shortly.")
                if (s === GifSearchController.ProviderError)
                    return qsTr("The GIF provider had a problem. Try again.")
                if (s === GifSearchController.NoResults)
                    return qsTr("No GIFs found.")
                return ""
            }
        }
    }

    // ── Footer: real provider attribution + a "return to send" hint ─
    RowLayout {
        id: footerRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: AppTheme.spacing4
        spacing: AppTheme.spacing6

        Label {
            id: attributionLabel
            Layout.fillWidth: true
            color: AppTheme.stormTextMuted
            font.family: AppTheme.monoFont
            font.pixelSize: 9
            elide: Text.ElideRight
            // The narrower 330px design width has no room for the previous
            // long privacy sentence alongside attribution + the send hint;
            // it lives in a hover tooltip instead of being dropped outright.
            // A local list can hold rows from either provider, so it credits
            // every provider rather than whichever one happens to be active.
            text: picker.providerTab ? picker.gif.attribution
                                     : picker.allProviderAttribution
            Accessible.name: attributionLabel.text
            ToolTip.text: picker.providerTab
                ? qsTr("Searches are sent to the selected GIF provider")
                : qsTr("Nothing here is searched online. GIFs you saved from a "
                       + "chat stay on this device; provider GIFs still load "
                       + "their preview from that provider.")
            ToolTip.visible: attributionHover.hovered
            HoverHandler { id: attributionHover }
        }
        MenuKeycap {
            iconName: "keyboard_return"
        }
        Label {
            text: qsTr("send")
            color: AppTheme.stormTextMuted
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontMicro
        }
    }

    } // contentItem Item
}
