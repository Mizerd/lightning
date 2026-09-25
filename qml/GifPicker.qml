import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Multi-provider GIF picker shared by the room and thread composers. Bound to
// app.gif: the controller owns provider selection, request lifecycle,
// debounce, pagination and safe-search; this component only presents them.
//
// Tiles never load a provider URL themselves: previews and stills come from
// app.gif.previews as local copies of downloaded, GIF-validated bytes, so an
// SVG answer cannot reach Qt's image decoders. The full GIF is downloaded and
// validated only when chosen, on its way into the Matrix attachment pipeline.
//
// A star means one thing everywhere: "save this GIF". Saved GIFs live in one
// Saved tab (GifSavedModel merges the two stores, which stay separate for the
// security reasons documented there). The nav row holds the sources (GIPHY,
// KLIPY) and, past a divider, the cross-provider lists (Saved, Recent). Every
// tile carries a source tag so attribution stays provider-true per tile.
AnchoredPopup {
    id: picker

    // "room" or "thread": routes the send and decides which composer gets focus
    // back. The active target closes the other picker.
    property string target: "room"
    signal gifChosen(var result)
    // ── One window, two kinds ── This picker and StickerPicker carry the same
    // GIFs/Stickers strip; choosing the other segment asks the host to swap
    // them. They stay separate components (packs have owners, attribution and
    // failure states the GIF grid doesn't), but share the anchor, chrome and
    // remembered size (`sizeSettingsKey: "picker"`) and have no transitions, so
    // the swap reads as one window.
    signal kindRequested(string kind)
    // Whether to offer the strip; cleared when the other kind is unavailable.
    property bool offerKindTabs: false

    readonly property var gif: app.gif

    // The selected tab: a provider id from gif.providerIds ("giphy"/"klipy") or
    // one of the local lists, "saved"/"recent".
    property string tab: "giphy"
    // A provider tab searches, paginates and shows attribution; a local list
    // does none of those and never issues a request.
    readonly property bool providerTab: tab !== "saved" && tab !== "recent"

    readonly property var activeModel:
        tab === "saved" ? gif.saved
        : tab === "recent" ? gif.recent
        : gif.results

    // Attribution for the local lists, which can hold rows from either
    // provider: every provider's credit. Per-GIF attribution comes from each
    // tile's tag.
    readonly property string allProviderAttribution: {
        var rev = cfgRevision
        return gif.providerIds.map(function(id) {
            return gif.providerAttribution(id)
        }).join(" · ")
    }

    // Human-readable byte size for the size overlay ("" hides it).
    function formatBytes(n) {
        if (!n || n <= 0) return ""
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    // The tile's source tag. "local" is a GIF saved from a chat, on this device
    // only.
    function sourceLabel(provider) {
        if (provider === "local")
            return qsTr("Local")
        var name = gif.providerDisplayName(provider)
        return name.length > 0 ? name : provider
    }

    // Format tag for a locally saved tile. `ext` comes from
    // GifStarredStore::sourceExt(), validated at save time
    // (gif::validateRasterBytes), never from a filename.
    function formatTag(ext) {
        if (ext === "png") return "PNG"
        if (ext === "jpg") return "JPEG"
        if (ext === "webp") return "WEBP"
        return "GIF"
    }

    // Sized as a share of the composer's width and the room above it. The
    // minimum keeps three grid columns and the footer legible.
    // `sizeSettingsKey` matches the emoji picker's, so resizing one resizes
    // both.
    widthFraction: 0.38
    heightFraction: 0.64
    // Plus the grab band, which is not content.
    minWidth: 300 + gripGrab
    minHeight: 320 + gripGrab
    sizeSettingsKey: "picker"
    padding: AppTheme.spacingS

    // ── Resize grab band ── A press just outside the popup's item closes it
    // and also reaches the chat behind it, which then flicks (the picker is
    // deliberately not modal). A grip can't live outside the item (the popup
    // never delivers that press), so the panel is inset by `gripGrab` on the
    // top and left, leaving a transparent band that still counts as inside the
    // popup. The bottom-right corner is pinned to the composer, so it gets no
    // band.
    readonly property real gripGrab: 8
    leftInset: gripGrab
    topInset: gripGrab
    // Insets move only the background, so the content needs matching padding.
    leftPadding: padding + gripGrab
    topPadding: padding + gripGrab
    // Not modal, like EmojiPicker: a Popup doesn't consume presses inside it
    // anyway, and a grabbed overlay stops the timeline scrolling while open.
    // The tiles and the background sink consume presses.
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // Send the exact tile the user acted on, never a re-resolved index. A click
    // passes the delegate's own snapshot; keyboard activation passes a row that
    // is resolved against activeModel immediately, never stored, so a debounced
    // search response or list reorder can't swap in a different item.
    //
    // `activated` is a one-shot latch covering every activation path: close()
    // runs an exit transition, so a second activation (e.g. a double-click
    // delivered as two clicks) can still arrive. Reset on the next open, not on
    // close.
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

    // Whether this GIF is saved, asked of the stores and never of a row's
    // FavoriteRole (GifStoredModel answers that with a constant `true`, which
    // would make every Recent tile claim to be saved). A "local" row exists
    // only in the local-saved store and is never recorded into Recents
    // (GifSendController::startLocal), so it is always saved.
    function isSaved(provider, gifId) {
        if (!provider || !gifId)
            return false
        if (provider === "local")
            return true
        return gif.favorites.isFavorite(provider, gifId)
    }

    // isSaved() is a function call, so bindings on it don't track the stores;
    // bumping this on each collection change re-evaluates the tiles.
    property int savedRevision: 0
    Connections {
        target: picker.gif.favorites
        function onCountChanged() { picker.savedRevision++ }
    }
    Connections {
        target: picker.gif.starredStore
        function onCountChanged() { picker.savedRevision++ }
    }

    // Availability re-resolves on every open; providerConfigurationChanged
    // bumps cfgRevision to re-evaluate providerConfigured() bindings.
    property int cfgRevision: 0
    Connections {
        target: picker.gif
        function onProviderConfigurationChanged() { picker.cfgRevision++ }
        // The room and thread composers each own a picker bound to this one
        // controller. Two can be open at once (e.g. opened by keyboard),
        // sharing one results grid, so whichever opens last wins and the others
        // close.
        function onPickerOpenRequested(target) {
            if (target !== picker.target && picker.opened)
                picker.close()
        }
    }
    onAboutToShow: {
        gif.notifyPickerOpening(picker.target)
        gif.refreshProviderKeys()
        // AnchoredPopup handles placement in its own Connections, not
        // onAboutToShow, so this handler doesn't override it.
        tab = gif.providerId
        grid.currentIndex = -1
        activated = false
        if (gif.results.count === 0)
            gif.showTrending()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: gif.reset()
    onTabChanged: grid.currentIndex = -1

    // Select a provider id or a local list id. Only a provider id may reach
    // setActiveProvider(), the one network-triggering call here; local lists
    // return early and stay offline.
    function selectTab(value) {
        picker.tab = value
        if (value === "saved" || value === "recent")
            return
        picker.gif.setActiveProvider(value)
        // setActiveProvider() early-returns for an unchanged provider, so the
        // grid may be empty here. Don't start trending while a search or
        // category is pending: setActiveProvider() re-issues it and the results
        // haven't arrived.
        if (picker.gif.results.count === 0
                && picker.gif.mode !== GifSearchController.Search
                && picker.gif.mode !== GifSearchController.Category)
            picker.gif.showTrending()
    }

    // Focus hand-off into the grid. Local lists have no search field, so Down
    // on the tab strip is their entry point.
    function focusGridFromTabs() {
        if (picker.providerTab)
            return
        grid.forceActiveFocus()
        if (grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
    }

    // Toggle saved state for the tile's own snapshot, never a re-resolved row.
    // "local" rows are byte copies in GifStarredStore; everything else is a
    // provider bookmark in GifFavoritesModel (whose C++ names say "favorite").
    function toggleSaved(result) {
        if (!result || !result.provider || !result.gifId)
            return
        if (result.provider === "local") {
            gif.starredStore.unstar(result.gifId)
            return
        }
        gif.toggleFavorite(result)
    }

    // A heavier frame and wider corner, so the concentric resize ornament fits.
    background: Item {
        Rectangle {
            id: pickerPanel
            objectName: "gifPickerPanel"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.radiusLg + 6

            // Press barrier: a Popup doesn't consume presses on itself, so
            // presses on the header, tab strips, chips, footer or padding would
            // reach items behind (e.g. the message context menu). It sits below
            // contentItem, so real controls still get the press first.
            MouseArea {
                anchors.fill: parent
                // Extend over the inset band, which is inside the popup but
                // outside this Rectangle.
                anchors.leftMargin: -picker.leftInset
                anchors.topMargin: -picker.topInset
                acceptedButtons: Qt.AllButtons
            }
        }
        // Popover shadow. Effect and source must be siblings, hence the Item
        // wrapper.
        MultiEffect {
            source: pickerPanel
            anchors.fill: pickerPanel
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
    }

    contentItem: Item {

    ColumnLayout {
        id: pickerColumn
        anchors.fill: parent
        // Reserve the footer's real height (its ↵ keycap is taller than a
        // caption line), or it paints over the bottom row of tiles.
        anchors.bottomMargin: footerRow.implicitHeight + AppTheme.spacing4 * 2
        spacing: AppTheme.spacingS

        // ── Row 0: which kind of media this window shows ──
        SegmentedControl {
            objectName: "pickerKindTabs"
            visible: picker.offerKindTabs
            // Explicit left alignment, matching the rows below.
            Layout.alignment: Qt.AlignLeft
            Layout.fillWidth: false
            storm: true
            model: [
                { label: qsTr("GIFs"), value: "gif", enabled: true },
                { label: qsTr("Stickers"), value: "sticker", enabled: true },
            ]
            current: "gif"
            onActivated: (value) => {
                if (value !== "gif")
                    picker.kindRequested(value)
            }
        }

        // ── Row 1: search (provider tabs) or list title + "GIF" badge + close
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            // Local lists have no search field: a query would hit a provider
            // whose results the grid isn't showing.
            Label {
                objectName: "gifListTitle"
                visible: !picker.providerTab
                Layout.fillWidth: true
                text: picker.tab === "saved" ? qsTr("Saved GIFs")
                                             : qsTr("Recently sent")
                color: AppTheme.stormText
                // The pane title, in the UI face on the type scale.
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightStrong
                elide: Label.ElideRight
                verticalAlignment: Text.AlignVCenter
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
                    // Never send from here: the query is debounced, so Enter
                    // right after typing would pick from the previous results.
                    // Flush the query and move focus to the grid instead.
                    picker.gif.searchNow(searchField.text)
                    grid.forceActiveFocus()
                    if (grid.currentIndex < 0 && grid.count > 0)
                        grid.currentIndex = 0
                }
            }

            // The picker's inline "GIF" badge.
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
                // The shared composer-row size rung.
                size: "md"
                iconName: "close"
                Accessible.name: qsTr("Close GIF picker")
                onClicked: picker.close()
            }
        }

        // ── The single nav row: sources at the left, then your own lists,
        // pushed to the right edge behind a divider. Two SegmentedControls
        // bound to the same `picker.tab`, so a divider can sit between them.
        // Neither fills (fillWidth defaults to true for Layout-derived
        // children, and SegmentedControl is a RowLayout); one filler takes the
        // surplus so the lists stay flush right at any width.
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            SegmentedControl {
                id: providerTabs
                objectName: "gifProviderTabs"
                storm: true
                Layout.fillWidth: false
                // cfgRevision re-evaluates enabled/tip after a key refresh.
                // Unavailable providers are disabled with an explanation.
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

            // The only filler: everything after it is flush right.
            Item {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
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
                Layout.fillWidth: false
                // Neither list needs a key or network, so both are always
                // enabled.
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

            // Down on the nav row enters the grid on a local list (searchField
            // does this on provider tabs).
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
                    // The selected chip toggles off back to trending; otherwise
                    // a session has no route back to trending once a category
                    // is open.
                    readonly property bool selected:
                        picker.gif.mode === GifSearchController.Category
                        && picker.gif.query === modelData
                    implicitWidth: chipLabel.implicitWidth
                                   + AppTheme.chipPaddingH * 2 + AppTheme.spacing4
                    implicitHeight: AppTheme.buttonHeightSm
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    // CheckBox, not RadioButton: the chips can all be off.
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Category %1").arg(modelData)
                    Accessible.checked: categoryChip.selected
                    onClicked: {
                        if (categoryChip.selected)
                            picker.gif.showTrending()
                        else
                            picker.gif.openCategory(modelData)
                    }
                    // Words, not keycaps: UI face on the type scale.
                    contentItem: Label {
                        id: chipLabel
                        text: categoryChip.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: categoryChip.selected ? AppTheme.weightStrong
                                                           : AppTheme.weightMedium
                        color: categoryChip.selected ? AppTheme.chipBoltInk
                             : categoryChip.hovered ? AppTheme.stormText
                             : AppTheme.stormTextSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    // The solid chip reserved for "current selection".
                    background: Rectangle {
                        color: categoryChip.selected ? AppTheme.chipBoltFill
                             : categoryChip.hovered ? AppTheme.stormSelection
                             : AppTheme.stormInset
                        radius: AppTheme.chipRadius
                        border.width: categoryChip.selected ? 0 : 1
                        border.color: AppTheme.stormBorder
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

        // ── Result grid: 3 columns sized off the picker's width ──
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
            // Scroll position indicator for the paginating grid.
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            // Same wheel/touchpad feel as the timeline and emoji grid (see
            // SmoothWheelArea.qml).
            SmoothWheelArea {}
            currentIndex: -1
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            // Local lists hide searchField (the focus hand-off on provider
            // tabs), so the grid takes Tab focus there. Focus-gain seeds
            // currentIndex like the Down/Return hand-offs.
            activeFocusOnTab: !picker.providerTab
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0)
                    currentIndex = 0
            }

            // A highlighted/keyboard-selected row is just an int that Qt
            // doesn't remap. Drop it on a model reset or any insert/remove/move
            // at or before it, so Return can't resolve it against different
            // content. Pagination only appends after the end, so it keeps the
            // highlight.
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

            // Infinite scroll on provider tabs only; Saved and Recent are
            // complete.
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
                // Sendable-variant byte size (0 = unknown).
                required property real gifBytes
                readonly property bool current: GridView.isCurrentItem
                // Saved state from the stores (see picker.isSaved());
                // savedRevision makes it re-evaluate.
                readonly property bool saved: {
                    var rev = picker.savedRevision
                    return picker.isSaved(tile.provider, tile.gifId)
                }
                // Local rows have no CDN URL; the source is re-validated by
                // content hash on every evaluation, never trusted from the
                // persisted index.
                readonly property string localSource:
                    tile.provider === "local"
                        ? picker.gif.starredStore.source(tile.gifId) : ""
                // On-disk format of a local tile ("gif"/"png"/"jpg"/"webp"),
                // re-derived like localSource. "" for provider tiles.
                readonly property string localExt:
                    tile.provider === "local"
                        ? picker.gif.starredStore.sourceExt(tile.gifId) : ""
                // Provider tiles: validated local copies from
                // app.gif.previews, "" until downloaded. The revision
                // re-evaluates them as copies arrive.
                readonly property string stillSource: {
                    var rev = picker.gif.previews.revision
                    return tile.provider === "local" ? ""
                        : picker.gif.previews.source(tile.stillUrl, true)
                }
                readonly property string previewSource: {
                    var rev = picker.gif.previews.revision
                    return tile.provider === "local" ? ""
                        : picker.gif.previews.source(tile.previewUrl, false)
                }
                // Keeps this tile's copies cached while it exists.
                readonly property var heldUrls: tile.provider === "local"
                    ? [] : [tile.stillUrl, tile.previewUrl]
                onHeldUrlsChanged: picker.gif.previews.hold(tile, heldUrls)
                Component.onCompleted: picker.gif.previews.hold(tile, heldUrls)

                // The record this delegate is rendering, from its own
                // properties rather than re-queried by index, so a click sends
                // exactly what is on screen.
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

                    // The still (fetched first; GIF stills only) shows until
                    // the animation is decoded, which then plays on top only
                    // while the picker is visible. Local
                    // PNG/JPEG/WebP tiles never reach AnimatedImage Ready, so
                    // this Image remains their renderer.
                    Image {
                        id: still
                        anchors.fill: parent
                        source: tile.provider === "local"
                                ? tile.localSource : tile.stillSource
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        visible: anim.status !== AnimatedImage.Ready
                        Accessible.role: Accessible.Button
                        // Local stills announce their real format; every other
                        // tile rendering through this still announces as a GIF.
                        Accessible.name:
                            (tile.provider === "local" && tile.localExt.length > 0
                             && tile.localExt !== "gif")
                            ? (tile.title.length > 0
                               ? qsTr("%1: %2").arg(picker.formatTag(tile.localExt))
                                                .arg(tile.title)
                               : picker.formatTag(tile.localExt))
                            : (tile.title.length > 0
                               ? qsTr("GIF: %1").arg(tile.title) : qsTr("GIF"))
                    }
                    AnimatedImage {
                        id: anim
                        anchors.fill: parent
                        // Local stills never feed the movie backend. Legacy
                        // rows with an empty ext are GIFs.
                        source: tile.provider === "local"
                                ? (tile.localExt.length === 0
                                   || tile.localExt === "gif"
                                   ? tile.localSource : "")
                                : tile.previewSource
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        // Autoplay: 0 Always (while visible), 1 OnHover, 2
                        // Never. Also paused when the picker is hidden.
                        playing: picker.visible && app.settings.gifAutoplay !== 2
                                 && (app.settings.gifAutoplay === 0
                                     || tileHover.hovered)
                        Accessible.role: Accessible.Button
                        Accessible.name: tile.title.length > 0
                            ? qsTr("GIF: %1").arg(tile.title) : qsTr("GIF")
                    }

                    // Placeholder while a provider tile has nothing to show
                    // yet (a KLIPY still is never fetched).
                    Icon {
                        objectName: "gifTilePlaceholder"
                        anchors.centerIn: parent
                        visible: tile.provider !== "local"
                                 && still.status !== Image.Ready
                                 && anim.status !== AnimatedImage.Ready
                        name: "gif_box"
                        size: 28
                        color: AppTheme.textMuted
                    }

                    // The tile body sends; right-click or the star
                    // saves/unsaves without sending. Both use the tile's own
                    // snapshot.
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

                    // Source tag, top-left: GIPHY / KLIPY / Local.
                    // Distinguishes provider bookmarks from on-device copies in
                    // the merged Saved list and keeps attribution on each tile.
                    Rectangle {
                        id: sourceBadge
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

                    // Format tag beside the source badge, local tiles only.
                    Rectangle {
                        objectName: "gifTileFormatBadge"
                        visible: tile.provider === "local" && tile.localExt.length > 0
                        anchors.top: parent.top
                        anchors.left: sourceBadge.right
                        anchors.topMargin: 4
                        anchors.leftMargin: 4
                        radius: AppTheme.radiusSm
                        color: AppTheme.overlayScrim
                        implicitWidth: formatBadgeLabel.implicitWidth + AppTheme.spacing6
                        implicitHeight: formatBadgeLabel.implicitHeight + AppTheme.spacing2
                        Label {
                            id: formatBadgeLabel
                            anchors.centerIn: parent
                            text: picker.formatTag(tile.localExt)
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontMicro
                            font.weight: Font.Bold
                            color: AppTheme.scrimInk
                        }
                    }

                    // Size overlay, bottom-left, only when known.
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

                    // Saved state is a fill, not just a tint: the bundled
                    // Material Symbols subset has no filled star. Bolt fill +
                    // boltInk matches MessageDelegate.qml's chat star.
                    ToolButton {
                        id: saveButton
                        objectName: "gifTileSaveButton"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        // Inset like the badges so the focus ring isn't
                        // clipped.
                        anchors.margins: 4
                        width: 24; height: 24
                        contentItem: Icon {
                            name: "star"
                            size: 15
                            color: tile.saved ? AppTheme.boltInk
                                              : AppTheme.scrimInk
                        }
                        // Revealed on hover, grid selection or keyboard focus;
                        // never at rest (on the Saved tab every tile would
                        // carry one). Tab is the keyboard route, since
                        // Return/Enter/Space on the grid send.
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

    // ── State overlay, covering exactly the grid area (pickerColumn
    // fills this contentItem at 0,0, so the grid's geometry applies) ──
    Item {
        objectName: "gifStateOverlay"
        x: grid.x
        y: grid.y
        width: grid.width
        height: grid.height
        visible: overlayText.text.length > 0 || busy.running

        // The shared spinner, inked in the accent so loading reads as active.
        AppBusyIndicator {
            id: busy
            objectName: "gifStateOverlayBusy"
            anchors.centerIn: parent
            color: AppTheme.bolt
            running: picker.providerTab
                     && picker.gif.state === GifSearchController.Loading
                     && picker.gif.results.count === 0
            // AppBusyIndicator leaves visibility to its host, and a stopped
            // spinner would otherwise paint over the empty/error text.
            // `running` and every overlay string are mutually exclusive, so
            // hiding is safe.
            visible: busy.running
        }
        Label {
            id: overlayText
            objectName: "gifStateOverlayText"
            anchors.centerIn: parent
            width: parent.width - AppTheme.spacingL * 2
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
            // Wrapping text needs explicit leading (see AppTheme's lineHeight).
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: {
                // The local lists don't depend on gif.available.
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
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            id: attributionLabel
            Layout.fillWidth: true
            color: AppTheme.stormTextMuted
            // A required provider credit, at the scale's smallest step.
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMicro
            elide: Text.ElideRight
            // The privacy note lives in a tooltip. A local list credits every
            // provider since it can hold rows from either.
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
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightMedium
            Layout.alignment: Qt.AlignVCenter
        }
    }

    // Resize ornament, seated on the popup item's corner via negative margins
    // so its grab band covers the inset strip.
    PopupResizeGrip {
        popup: picker
        // arcCentre is measured from the panel corner, `gripGrab` inside.
        grabMargin: picker.gripGrab
        arcCentre: AppTheme.radiusLg + 6
        outerRadius: AppTheme.radiusLg + 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: -picker.leftPadding
        anchors.topMargin: -picker.topPadding
    }

    } // contentItem Item
}
