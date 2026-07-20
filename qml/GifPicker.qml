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
Popup {
    id: picker

    // "room" or "thread" — routes the eventual send; also isolates which
    // composer reopens focus. The active target closes the other picker.
    property string target: "room"
    property point anchorPoint: Qt.point(0, 0)
    signal gifChosen(var result)

    readonly property var gif: app.gif
    readonly property int cell: 132

    // "browse" (trending/search/categories) | "favorites" | "recent".
    property string section: "browse"
    readonly property var activeModel:
        section === "favorites" ? gif.favorites
        : section === "recent" ? gif.recent
        : gif.results

    parent: Overlay.overlay
    width: Math.min(460, parent ? parent.width - AppTheme.spacingM * 2 : 460)
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

    // Send the clicked tile of the model the user is actually LOOKING AT.
    // This must read activeModel: reading gif.results here sent the first
    // Trending item when a favorite was clicked (the Favorites grid shows
    // gif.favorites while gif.results still holds the browse content).
    // The row resolves synchronously against the visible model and the
    // result carries its own provider-qualified identity and send URL —
    // never a cross-model index, never an index-zero substitute.
    function choose(row) {
        if (row < 0 || row >= activeModel.count)
            return
        var result = activeModel.get(row)
        if (!result || !result.provider || !result.gifId)
            return
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
    }
    onAboutToShow: {
        gif.refreshProviderKeys()
        placeInsideWindow()
        section = "browse"
        if (gif.results.count === 0)
            gif.showTrending()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: gif.reset()

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
        anchors.fill: parent
        anchors.bottomMargin: 14   // reserve space for the attribution footer
        spacing: AppTheme.spacingS

        // ── Header: provider tabs + search + close ──────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            SegmentedControl {
                id: providerTabs
                objectName: "gifProviderTabs"
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

            AppTextField {
                id: searchField
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
                    if (grid.count > 0)
                        picker.choose(0)
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

        // ── Section nav: Trending / Favorites / Recent ──────────────
        SegmentedControl {
            objectName: "gifSectionTabs"
            model: [
                { label: qsTr("Trending"), value: "browse" },
                { label: qsTr("Favorites"), value: "favorites" },
                { label: qsTr("Recent"), value: "recent" },
            ]
            current: picker.section
            onActivated: (value) => {
                picker.section = value
                if (value === "browse" && picker.gif.results.count === 0)
                    picker.gif.showTrending()
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

        // ── Result grid ─────────────────────────────────────────────
        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: picker.cell
            cellHeight: picker.cell
            cacheBuffer: picker.cell * 2   // bounded off-screen retention
            model: picker.activeModel
            currentIndex: -1
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds

            // Infinite scroll: only the network-backed browse view paginates;
            // Favorites/Recent are complete local lists.
            onContentYChanged: {
                if (picker.section !== "browse" || contentHeight <= 0)
                    return
                if (contentY + height > contentHeight - picker.cell * 2)
                    picker.gif.loadMore()
            }

            delegate: Item {
                id: tile
                width: grid.cellWidth
                height: grid.cellHeight
                required property int index
                required property string previewUrl
                required property string stillUrl
                required property string title
                required property bool favorite
                readonly property bool current: GridView.isCurrentItem

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: AppTheme.radiusSm
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
                                picker.choose(tile.index)
                        }
                    }

                    ToolButton {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        width: 24; height: 24
                        contentItem: Icon {
                            name: "star"
                            size: 15
                            color: tile.favorite ? AppTheme.warning
                                                 : AppTheme.accentText
                        }
                        opacity: tile.favorite || tileHover.hovered ? 1 : 0
                        Accessible.name: tile.favorite
                            ? qsTr("Remove from favorites")
                            : qsTr("Add to favorites")
                        onClicked: picker.toggleFavorite(tile.index)
                        background: Rectangle {
                            radius: 12
                            color: Qt.rgba(0, 0, 0, 0.35)
                        }
                    }
                    HoverHandler { id: tileHover }
                }
            }

            Keys.onReturnPressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onEnterPressed: if (currentIndex >= 0) picker.choose(currentIndex)
        }
    }

    // ── State overlay (covers the grid area) ────────────────────────
    Item {
        anchors.fill: parent
        anchors.topMargin: 90
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

    // ── Footer: provider attribution + privacy ──────────────────────
    Label {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        horizontalAlignment: Text.AlignHCenter
        color: AppTheme.textDisabled
        font.pixelSize: 9
        elide: Text.ElideRight
        text: (picker.gif.attribution.length > 0
               ? picker.gif.attribution + "  ·  " : "")
              + qsTr("Searches are sent to the selected GIF provider")
        Accessible.name: picker.gif.attribution
    }

    } // contentItem Item
}
