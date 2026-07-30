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
// composerGifKeycap), pill-styled Trending/Favorites/Recent section chips
// (provider GIPHY/KLIPY selection stays its own SegmentedControl row —
// unrelated to the section chips despite SPEC's "provider chips" wording),
// a "GIF" tile badge + optional byte-size overlay, and a "return to send"
// footer hint. The nine choose()/snapshot()/latch/reset invariants below are
// UNCHANGED — only their surrounding visual chrome moved.
Popup {
    id: picker

    // "room" or "thread" — routes the eventual send; also isolates which
    // composer reopens focus. The active target closes the other picker.
    property string target: "room"
    property point anchorPoint: Qt.point(0, 0)
    signal gifChosen(var result)

    readonly property var gif: app.gif

    // "browse" (trending/search/categories) | "favorites" | "recent".
    property string section: "browse"
    readonly property var activeModel:
        section === "favorites" ? gif.favorites
        : section === "recent" ? gif.recent
        : gif.results

    // Human-readable byte size for the stretch size overlay ("" when unknown
    // — the overlay is hidden in that case).
    function formatBytes(n) {
        if (!n || n <= 0) return ""
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    parent: Overlay.overlay
    width: Math.min(330, parent ? parent.width - AppTheme.spacingM * 2 : 330)
    height: Math.min(520, parent ? parent.height - AppTheme.spacingM * 2 : 520)
    padding: AppTheme.spacingS
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function placeInsideWindow() {
        if (!parent)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2,
                              parent.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        y = below + height <= parent.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS, anchorPoint.y - height - AppTheme.spacingXS)
    }

    // Send the exact tile the user acted on — never a re-resolved index.
    // `resultOrRow` is normally an already-captured result map: a mouse
    // click hands over the delegate's OWN current data (see tile.snapshot()
    // below), taken from the exact delegate the user clicked, so it can
    // never drift from what was on screen. Keyboard activation has no
    // delegate to snapshot from, so it passes a plain row number instead;
    // that row is resolved against activeModel IMMEDIATELY, in this same
    // call — never stored and resolved later. Storing a row for later
    // resolution is exactly what let a debounced search response replacing
    // the grid, or a favorite/recent reorder, swap in a different item
    // under a stale currentIndex/grid.currentIndex.
    //
    // Reading activeModel (not gif.results) matters too: gif.results is the
    // browse grid, but Favorites/Recent show a different model, and a row
    // must never be resolved against the wrong one.
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

    // Availability re-resolves every time the picker opens, so a picker
    // first shown before configuration finished (or a newly created env
    // file) never sticks at "off". providerConfigurationChanged bumps
    // cfgRevision, which re-evaluates the providerConfigured() bindings.
    property int cfgRevision: 0
    Connections {
        target: picker.gif
        function onProviderConfigurationChanged() { picker.cfgRevision++ }
        // "The active target closes the other picker" (see the comment at
        // the top of this file): the room composer and the thread panel
        // each own an independent GifPicker instance, but both are bound to
        // this one shared controller. Popup.CloseOnPressOutside does NOT
        // close a sibling reached without a mouse press outside it — e.g.
        // Tab + Space/Enter onto the other composer's GIF button — so two
        // pickers can legitimately both be open, sharing one live results
        // grid; a search/page/provider change in either would then silently
        // swap what the other is showing. Whichever picker opens last wins:
        // every other one closes itself here (and resets, via onClosed
        // below) before it can touch shared state again.
        function onPickerOpenRequested(target) {
            if (target !== picker.target && picker.opened)
                picker.close()
        }
    }
    onAboutToShow: {
        gif.notifyPickerOpening(picker.target)
        gif.refreshProviderKeys()
        placeInsideWindow()
        section = "browse"
        grid.currentIndex = -1
        activated = false
        if (gif.results.count === 0)
            gif.showTrending()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: gif.reset()
    onSectionChanged: grid.currentIndex = -1

    // Toggle favorite for the tile at `row` of the currently shown model,
    // without sending. Refreshes the browse grid's star state.
    function toggleFavorite(row) {
        if (row < 0 || row >= activeModel.count)
            return
        gif.toggleFavorite(activeModel.get(row))
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
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

        // ── Row 1: search + the picker's own "GIF" badge + close ────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            AppTextField {
                id: searchField
                objectName: "gifSearchField"
                Layout.fillWidth: true
                searchIcon: true
                clearButton: true
                placeholderText: qsTr("Search %1").arg(picker.gif.providerName)
                Accessible.name: qsTr("Search GIFs")
                selectByMouse: true
                onTextChanged: {
                    if (text.length > 0)
                        picker.section = "browse"
                    picker.gif.setQueryText(text)
                }
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
                border.color: AppTheme.borderStrong
                Label {
                    id: gifHeaderBadgeLabel
                    anchors.centerIn: parent
                    text: qsTr("GIF")
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontChip + 1
                    font.weight: Font.Bold
                    color: AppTheme.textSecondary
                }
            }

            IconButton {
                implicitWidth: 28; implicitHeight: 28
                radius: 6
                iconName: "close"
                iconSize: 16
                Accessible.name: qsTr("Close GIF picker")
                onClicked: picker.close()
            }
        }

        // ── Provider tabs (GIPHY/KLIPY) — own row, unchanged behavior ──
        SegmentedControl {
            id: providerTabs
            objectName: "gifProviderTabs"
            Layout.fillWidth: true
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
            current: picker.gif.providerId
            onActivated: (value) => picker.gif.setActiveProvider(value)
        }

        // ── Row 2: section chips — Trending / Favorites / Recent ─────
        Row {
            objectName: "gifSectionTabs"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Repeater {
                model: [
                    { label: qsTr("Trending"), value: "browse", icon: "" },
                    { label: qsTr("Favorites"), value: "favorites", icon: "star" },
                    { label: qsTr("Recent"), value: "recent", icon: "" },
                ]
                delegate: AbstractButton {
                    id: sectionChip
                    required property var modelData
                    readonly property bool selected:
                        picker.section === modelData.value
                    // Real padding, not a widened implicitWidth: an
                    // AbstractButton stretches its contentItem to the full
                    // control width, so extra width without padding pins the
                    // label to the pill's left edge instead of centring it.
                    leftPadding: AppTheme.spacing8 + 2
                    rightPadding: AppTheme.spacing8 + 2
                    implicitHeight: 28
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.label
                    Accessible.checked: sectionChip.selected
                    onClicked: {
                        picker.section = modelData.value
                        if (modelData.value === "browse"
                                && picker.gif.results.count === 0)
                            picker.gif.showTrending()
                    }
                    contentItem: Row {
                        id: chipRow
                        spacing: AppTheme.spacing4
                        Icon {
                            visible: sectionChip.modelData.icon.length > 0
                            anchors.verticalCenter: parent.verticalCenter
                            name: sectionChip.modelData.icon
                            size: 13
                            color: sectionChip.selected ? AppTheme.selectedText
                                                        : AppTheme.textSecondary
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: sectionChip.modelData.label
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: sectionChip.selected ? AppTheme.selectedText
                                                        : AppTheme.textSecondary
                        }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusPill
                        color: sectionChip.selected ? AppTheme.accentSoft
                                                    : "transparent"
                        border.width: 1
                        border.color: sectionChip.selected ? AppTheme.accentBorder
                                                           : AppTheme.border
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: AppTheme.radiusPill
                        color: "transparent"
                        border.color: AppTheme.focusRing
                        border.width: 2
                        visible: sectionChip.visualFocus
                    }
                }
            }
        }

        // ── Category chips (client-side search shortcuts) ───────────
        Flow {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS
            visible: picker.section === "browse"
                     && searchField.text.length === 0 && picker.gif.configured
            Repeater {
                model: picker.gif.categories
                delegate: AbstractButton {
                    id: categoryChip
                    required property string modelData
                    text: modelData
                    implicitWidth: chipLabel.implicitWidth + 20
                    implicitHeight: 26
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Category %1").arg(modelData)
                    onClicked: picker.gif.openCategory(modelData)
                    contentItem: Label {
                        id: chipLabel
                        text: categoryChip.text
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: categoryChip.hovered ? AppTheme.hover
                                                    : AppTheme.cardElevated
                        radius: AppTheme.radiusPill
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: AppTheme.radiusPill
                        color: "transparent"
                        border.color: AppTheme.focusRing
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

            // A highlighted/keyboard-selected row is just an int — Qt does
            // not remap it when the model changes underneath. A full
            // replace (a fresh search/category/provider-switch landing, or
            // Favorites/Recent reloading) invalidates every existing row,
            // so drop the highlight rather than let Return later resolve it
            // against unrelated content. A row shifting because something
            // was inserted/removed/moved AT OR BEFORE it is invalidated the
            // same way — a favorite/recent re-prepend, for example.
            // Pagination only ever appends AFTER the current end, so it
            // leaves an existing highlight untouched (see onRowsInserted).
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

            // Infinite scroll: only the network-backed browse view paginates;
            // Favorites/Recent are complete local lists.
            onContentYChanged: {
                if (picker.section !== "browse" || contentHeight <= 0)
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
                // now surfaced by GifResultModel::BytesRole /
                // GifStoredModel::BytesRole — always present, never undefined.
                required property real gifBytes
                readonly property bool current: GridView.isCurrentItem

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
                    color: AppTheme.cardElevated
                    border.width: tile.current ? 2 : 0
                    border.color: AppTheme.accent
                    clip: true

                    // Still fallback shows immediately; the animation plays on
                    // top once decoded, and only while the picker is visible.
                    Image {
                        anchors.fill: parent
                        source: tile.stillUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        visible: anim.status !== AnimatedImage.Ready
                    }
                    AnimatedImage {
                        id: anim
                        anchors.fill: parent
                        source: tile.previewUrl
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

                    // Choosing (send) is the tile body; the star toggles
                    // favorite WITHOUT sending. The star is always actionable.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            grid.currentIndex = tile.index
                            if (mouse.button === Qt.RightButton)
                                picker.toggleFavorite(tile.index)
                            else
                                picker.choose(tile.snapshot())
                        }
                    }

                    // "GIF" tile badge, top-left.
                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 4
                        radius: AppTheme.radiusSm
                        color: AppTheme.overlayScrim
                        implicitWidth: gifTileBadgeLabel.implicitWidth + AppTheme.spacing6
                        implicitHeight: gifTileBadgeLabel.implicitHeight + AppTheme.spacing2
                        Label {
                            id: gifTileBadgeLabel
                            anchors.centerIn: parent
                            text: qsTr("GIF")
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontMicro
                            font.weight: Font.Bold
                            color: AppTheme.scrimInk
                        }
                    }

                    // Size overlay, bottom-left — stretch: only when known.
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

                    ToolButton {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        width: 24; height: 24
                        contentItem: Icon {
                            name: "star"
                            size: 15
                            color: tile.favorite ? AppTheme.presenceAway
                                                 : AppTheme.scrimInk
                        }
                        opacity: tile.favorite || tileHover.hovered ? 1 : 0
                        Accessible.name: tile.favorite
                            ? qsTr("Remove from favorites")
                            : qsTr("Add to favorites")
                        onClicked: picker.toggleFavorite(tile.index)
                        background: Rectangle {
                            radius: 12
                            color: AppTheme.overlayScrim
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
            running: picker.section === "browse"
                     && picker.gif.state === GifSearchController.Loading
                     && picker.gif.results.count === 0
        }
        Label {
            id: overlayText
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacingL * 2
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: 13
            text: {
                if (!picker.gif.available)
                    return qsTr("GIFs are unavailable on this backend.")
                if (picker.section === "favorites")
                    return picker.gif.favorites.count === 0
                        ? qsTr("No favorites yet. Tap the star on a GIF to save it.")
                        : ""
                if (picker.section === "recent")
                    return picker.gif.recent.count === 0
                        ? qsTr("No recent GIFs yet.") : ""
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
            color: AppTheme.textMuted
            font.pixelSize: 9
            elide: Text.ElideRight
            // The narrower 330px design width has no room for the previous
            // long privacy sentence alongside attribution + the send hint;
            // it moves to a hover tooltip instead of being dropped outright.
            text: picker.gif.attribution
            Accessible.name: picker.gif.attribution
            ToolTip.text: qsTr("Searches are sent to the selected GIF provider")
            ToolTip.visible: attributionHover.hovered
            HoverHandler { id: attributionHover }
        }
        MenuKeycap {
            iconName: "keyboard_return"
        }
        Label {
            text: qsTr("send")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.fontMicro
        }
    }

    } // contentItem Item
}
