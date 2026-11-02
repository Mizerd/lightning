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

    property string target: "room"
    signal gifChosen(var result)
    signal kindRequested(string kind)
    property bool offerKindTabs: false

    readonly property var gif: app.gif

    property string tab: "giphy"
    // A provider tab searches, paginates and shows attribution; a local list
    // does none of those and never issues a request.
    readonly property bool providerTab: tab !== "saved" && tab !== "recent"

    readonly property var activeModel:
        tab === "saved" ? gif.saved
        : tab === "recent" ? gif.recent
        : gif.results

    readonly property string allProviderAttribution: {
        var rev = cfgRevision
        return gif.providerIds.map(function(id) {
            return gif.providerAttribution(id)
        }).join(" · ")
    }

    function formatBytes(n) {
        if (!n || n <= 0) return ""
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return Number(n / (1024 * 1024)).toLocaleString(Qt.locale(AppLocale.name), "f", 1) + " MB"
    }

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

    widthFraction: 0.38
    heightFraction: 0.64
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
    readonly property bool narrow: width < 380
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
    // True only while onClosed empties the search field (C40-F10).
    property bool clearingField: false
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

    function isSaved(provider, gifId) {
        if (!provider || !gifId)
            return false
        if (provider === "local")
            return true
        return gif.favorites.isFavorite(provider, gifId)
    }

    property int savedRevision: 0
    Connections {
        target: picker.gif.favorites
        function onCountChanged() { picker.savedRevision++ }
    }
    Connections {
        target: picker.gif.starredStore
        function onCountChanged() { picker.savedRevision++ }
    }

    property int cfgRevision: 0
    Connections {
        target: picker.gif
        function onProviderConfigurationChanged() { picker.cfgRevision++ }
        function onPickerOpenRequested(target) {
            if (target !== picker.target && picker.opened)
                picker.close()
        }
    }
    onAboutToShow: {
        gif.notifyPickerOpening(picker.target)
        gif.refreshProviderKeys()
        tab = gif.providerId
        grid.currentIndex = -1
        activated = false
        if (gif.results.count === 0)
            gif.showTrending()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: {
        gif.reset()
        picker.clearingField = true
        searchField.clear()
        picker.clearingField = false
    }
    onTabChanged: grid.currentIndex = -1

    function selectTab(value) {
        picker.tab = value
        if (value === "saved" || value === "recent")
            return
        picker.gif.setActiveProvider(value)
        if (picker.gif.results.count === 0
                && picker.gif.mode !== GifSearchController.Search
                && picker.gif.mode !== GifSearchController.Category)
            picker.gif.showTrending()
    }

    function focusGridFromTabs() {
        if (picker.providerTab)
            return
        grid.forceActiveFocus()
        if (grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
    }

    function toggleSaved(result) {
        if (!result || !result.provider || !result.gifId)
            return
        if (result.provider === "local") {
            gif.starredStore.unstar(result.gifId)
            return
        }
        gif.toggleFavorite(result)
    }

    background: Item {
        Rectangle {
            id: pickerPanel
            objectName: "gifPickerPanel"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.radiusLg + 6

            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -picker.leftInset
                anchors.topMargin: -picker.topInset
                acceptedButtons: Qt.LeftButton | Qt.RightButton
            }
        }
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
        anchors.bottomMargin: footerRow.implicitHeight + AppTheme.spacing4 * 2
        spacing: AppTheme.spacingS

        SegmentedControl {
            objectName: "pickerKindTabs"
            visible: picker.offerKindTabs
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

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            Label {
                objectName: "gifListTitle"
                visible: !picker.providerTab
                Layout.fillWidth: true
                text: picker.tab === "saved" ? qsTr("Saved GIFs")
                                             : qsTr("Recently sent")
                color: AppTheme.stormText
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
                Layout.minimumWidth: 0
                searchIcon: true
                clearButton: true
                storm: true
                placeholderText: qsTr("Search %1").arg(picker.gif.providerName)
                Accessible.name: qsTr("Search GIFs")
                selectByMouse: true
                onTextChanged: if (!picker.clearingField) picker.gif.setQueryText(text)
                Keys.onDownPressed: {
                    grid.forceActiveFocus()
                    if (grid.currentIndex < 0 && grid.count > 0)
                        grid.currentIndex = 0
                }
                Keys.onReturnPressed: {
                    picker.gif.searchNow(searchField.text)
                    grid.forceActiveFocus()
                    if (grid.currentIndex < 0 && grid.count > 0)
                        grid.currentIndex = 0
                }
            }

            Rectangle {
                objectName: "gifPickerHeaderBadge"
                visible: !picker.narrow
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
                size: "md"
                iconName: "close"
                Accessible.name: qsTr("Close GIF picker")
                onClicked: picker.close()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            SegmentedControl {
                id: providerTabs
                objectName: "gifProviderTabs"
                storm: true
                dense: picker.narrow
                fitWidth: true
                Layout.fillWidth: false
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
                dense: picker.narrow
                fitWidth: true
                Layout.fillWidth: false
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

            Keys.onDownPressed: picker.focusGridFromTabs()
        }

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
                    readonly property bool selected:
                        picker.gif.mode === GifSearchController.Category
                        && picker.gif.query === modelData
                    implicitWidth: chipLabel.implicitWidth
                                   + AppTheme.chipPaddingH * 2 + AppTheme.spacing4
                    implicitHeight: AppTheme.buttonHeightSm
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
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

        GridView {
            id: grid
            objectName: "gifResultGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor((width - AppTheme.scrollbarWidth) / 3)
            cellHeight: cellWidth
            cacheBuffer: cellWidth * 2
            model: picker.activeModel
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            SmoothWheelArea {}
            currentIndex: -1
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: !picker.providerTab
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0)
                    currentIndex = 0
            }

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
                required property real gifBytes
                readonly property bool current: GridView.isCurrentItem
                readonly property bool saved: {
                    var rev = picker.savedRevision
                    return picker.isSaved(tile.provider, tile.gifId)
                }
                readonly property string localSource:
                    tile.provider === "local"
                        ? picker.gif.starredStore.source(tile.gifId) : ""
                readonly property string localExt:
                    tile.provider === "local"
                        ? picker.gif.starredStore.sourceExt(tile.gifId) : ""
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
                readonly property var heldUrls: tile.provider === "local"
                    ? [] : [tile.stillUrl, tile.previewUrl]
                onHeldUrlsChanged: picker.gif.previews.hold(tile, heldUrls)
                Component.onCompleted: picker.gif.previews.hold(tile, heldUrls)

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

                    Image {
                        id: still
                        anchors.fill: parent
                        source: tile.provider === "local"
                                ? tile.localSource
                                : (tile.stillSource.length > 0
                                   || MotionPolicy.allowed
                                   ? tile.stillSource : tile.previewSource)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        visible: anim.status !== AnimatedImage.Ready
                        Accessible.role: Accessible.Button
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
                        source: !MotionPolicy.allowed ? ""
                                : tile.provider === "local"
                                ? (tile.localExt.length === 0
                                   || tile.localExt === "gif"
                                   ? tile.localSource : "")
                                : tile.previewSource
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        playing: picker.visible
                                 && MotionPolicy.plays(tileHover.hovered)
                                 && !MotionPolicy.suspended
                        Accessible.role: Accessible.Button
                        Accessible.name: tile.title.length > 0
                            ? qsTr("GIF: %1").arg(tile.title) : qsTr("GIF")
                    }

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
                        id: saveButton
                        objectName: "gifTileSaveButton"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 4
                        width: 24; height: 24
                        contentItem: Icon {
                            name: "star"
                            size: 15
                            color: tile.saved ? AppTheme.boltInk
                                              : AppTheme.scrimInk
                        }
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
                    Rectangle {
                        objectName: "gifTileCurrentRing"
                        anchors.fill: parent
                        z: 10
                        radius: AppTheme.radiusThumb
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.bolt
                        visible: tile.current && grid.activeFocus
                    }
                    HoverHandler { id: tileHover }
                }
            }

            Keys.onReturnPressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onEnterPressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onSpacePressed: if (currentIndex >= 0) picker.choose(currentIndex)
            Keys.onPressed: (event) => {
                if (count <= 0)
                    return
                if (event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier
                                       | Qt.AltModifier | Qt.MetaModifier))
                    return
                var cols = 3
                var rows = Math.max(1, Math.floor(height / cellHeight))
                var i = currentIndex < 0 ? 0 : currentIndex
                if (event.key === Qt.Key_Home)
                    i = 0
                else if (event.key === Qt.Key_End)
                    i = count - 1
                else if (event.key === Qt.Key_PageDown)
                    i = Math.min(count - 1, i + cols * rows)
                else if (event.key === Qt.Key_PageUp)
                    i = Math.max(0, i - cols * rows)
                else
                    return
                currentIndex = i
                positionViewAtIndex(i, GridView.Contain)
                event.accepted = true
            }
        }
    }

    MiddleClickScroller {
        objectName: "gifGridMiddleClickScroller"
        x: grid.x
        y: grid.y
        width: grid.width
        height: grid.height
        z: 1
        view: grid
    }

    Item {
        objectName: "gifStateOverlay"
        x: grid.x
        y: grid.y
        width: grid.width
        height: grid.height
        visible: overlayText.text.length > 0 || busy.running

        AppBusyIndicator {
            id: busy
            objectName: "gifStateOverlayBusy"
            anchors.centerIn: parent
            color: AppTheme.bolt
            running: picker.providerTab
                     && picker.gif.state === GifSearchController.Loading
                     && picker.gif.results.count === 0
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
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: {
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

    RowLayout {
        id: footerRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: AppTheme.spacing4
        spacing: AppTheme.spacing6

        Label {
            textFormat: Text.PlainText
            id: attributionLabel
            Layout.fillWidth: true
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMicro
            elide: Text.ElideRight
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

    PopupResizeGrip {
        popup: picker
        grabMargin: picker.gripGrab
        arcCentre: AppTheme.radiusLg + 6
        outerRadius: AppTheme.radiusLg + 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: -picker.leftPadding
        anchors.topMargin: -picker.topPadding
    }

    }
}
