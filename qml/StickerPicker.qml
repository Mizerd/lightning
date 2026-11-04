import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

AnchoredPopup {
    id: picker

    property string target: "room"
    signal stickerChosen(var image)
    signal kindRequested(string kind)
    property bool offerKindTabs: false

    readonly property var stickers: app.stickers

    widthFraction: 0.38
    heightFraction: 0.64
    minWidth: 300 + gripGrab
    minHeight: 320 + gripGrab
    sizeSettingsKey: "picker"
    padding: AppTheme.spacingS
    // The same transparent grab band as GifPicker: a press just outside the
    // popup's item closes it and reaches the chat behind.
    readonly property real gripGrab: 8
    leftInset: gripGrab
    topInset: gripGrab
    leftPadding: padding + gripGrab
    topPadding: padding + gripGrab
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property bool activated: false
    function choose(imageOrRow) {
        if (activated)
            return
        var image = (typeof imageOrRow === "number")
            ? picker.stickers.images.get(imageOrRow)
            : imageOrRow
        if (!image || !image.url)
            return
        activated = true
        picker.stickerChosen(image)
        close()
    }

    readonly property bool searching: picker.stickers.filter.length > 0

    onAboutToShow: {
        searchField.clear()
        picker.stickers.filter = ""
        picker.stickers.usage = "sticker"
        grid.currentIndex = -1
        activated = false
        // The only network request: account data plus a bounded /state read per
        // room pack, made when the picker opens, never on navigation or a
        // timer.
        picker.stickers.refreshIfStale()
        Qt.callLater(searchField.forceActiveFocus)
    }
    onClosed: searchField.clear()

    function focusGrid() {
        grid.forceActiveFocus()
        if (grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
    }

    readonly property var selectedPack: {
        var rev = picker.stickers.revision
        var row = picker.stickers.packs
        for (var i = 0; i < row.count; ++i) {
            var p = row.get(i)
            if (p.packId === picker.stickers.selectedPackId)
                return p
        }
        return null
    }

    background: Item {
        Rectangle {
            id: pickerPanel
            objectName: "stickerPickerPanel"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.radiusLg + 6

            // Press barrier: a Popup does not consume presses on it, so
            // unaccepted presses would reach the timeline behind. Fills the
            // popupItem below contentItem, so real controls still see presses
            // first.
            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -picker.leftInset
                anchors.topMargin: -picker.topInset
                acceptedButtons: Qt.AllButtons
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
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacingXS
        anchors.rightMargin: AppTheme.spacingXS
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
            current: "sticker"
            onActivated: (value) => {
                if (value !== "sticker")
                    picker.kindRequested(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            AppTextField {
                id: searchField
                objectName: "stickerSearchField"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                searchIcon: true
                clearButton: true
                storm: true
                placeholderText: qsTr("Search stickers")
                Accessible.name: qsTr("Search stickers by name or description")
                selectByMouse: true
                onTextChanged: picker.stickers.filter = text
                // Escape clears a query first; on an empty field it closes.
                Keys.onShortcutOverride: (event) => {
                    if (event.key === Qt.Key_Escape && searchField.text.length > 0)
                        event.accepted = true
                }
                Keys.onEscapePressed: (event) => {
                    if (searchField.text.length > 0) {
                        searchField.clear()
                        event.accepted = true
                    } else {
                        event.accepted = false
                    }
                }
                Keys.onDownPressed: picker.focusGrid()
                Keys.onReturnPressed: picker.focusGrid()
                Keys.onEnterPressed: picker.focusGrid()
            }
            NativeFileDialog {
                id: stickerFileDialog
                purpose: "image"
                title: qsTr("Choose a sticker")
                currentFolder: app.defaultFileDialogFolder()
                nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.svg)")]
                onAccepted: picker.stickers.uploadImage(selectedFile, "", picker.stickers.usage)
            }
            IconButton {
                objectName: "stickerAddButton"
                storm: true
                size: "md"
                iconName: "add"
                enabled: picker.stickers.available && !picker.stickers.saving
                Accessible.name: qsTr("Add a sticker from this computer")
                ToolTip.text: qsTr("Add a sticker")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: stickerFileDialog.open()
            }
            IconButton {
                id: manageButton
                objectName: "stickerManageButton"
                storm: true
                size: "md"
                iconName: "edit_square"
                enabled: picker.stickers.available
                         && picker.stickers.selectedPackId.length > 0
                         && !picker.stickers.editing
                Accessible.name: qsTr("Manage this pack")
                ToolTip.text: qsTr("Manage pack")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: {
                    var pack = picker.stickers.packInfo(
                        picker.stickers.selectedPackId)
                    if (!pack || !pack.packId)
                        return
                    packEditor.stickers = picker.stickers
                    packEditor.restoreTo = manageButton
                    packEditor.openFor(pack.packId, pack.displayName,
                                       pack.canManage)
                }
            }
            IconButton {
                id: refreshButton
                objectName: "stickerRefreshButton"
                storm: true
                size: "md"
                iconName: picker.stickers.loading ? "" : "refresh"
                enabled: !picker.stickers.loading
                Accessible.name: qsTr("Reload sticker packs")
                ToolTip.text: qsTr("Reload packs")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: picker.stickers.refresh()

                AppBusyIndicator {
                    objectName: "stickerPickerBusy"
                    anchors.centerIn: parent
                    size: 16
                    visible: picker.stickers.loading
                }
            }
            IconButton {
                objectName: "stickerCloseButton"
                storm: true
                size: "md"
                iconName: "close"
                Accessible.name: qsTr("Close sticker picker")
                onClicked: picker.close()
            }
        }

        PackChipStrip {
            id: packStrip
            objectName: "stickerPackStrip"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: picker.stickers.usablePackCount > 0
            stickers: picker.stickers
            usage: "sticker"
            currentPackId: picker.searching ? "" : picker.stickers.selectedPackId
            onPackChosen: (packId) => {
                searchField.clear()
                picker.stickers.selectedPackId = packId
            }
        }

        RowLayout {
            id: useEverywhereRow
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: !picker.searching
                     && picker.selectedPack !== null
                     && picker.selectedPack.source === "room"

            AppSwitch {
                id: useEverywhereSwitch
                objectName: "stickerPackUseEverywhere"
                enabled: useEverywhereRow.visible
                         && !picker.stickers.togglingRoomPack
                         && !picker.stickers.loading
                checked: picker.selectedPack !== null
                         && picker.selectedPack.enabledGlobally === true
                Accessible.name:
                    qsTr("Use this room's stickers in every room")
                onToggled: {
                    var pack = picker.selectedPack
                    if (pack)
                        picker.stickers.setRoomPackEnabled(
                            pack.packId, !useEverywhereSwitch.checked)
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Use everywhere")
                color: useEverywhereSwitch.enabled ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
                verticalAlignment: Text.AlignVCenter
                ToolTip.text: qsTr("A room's stickers are always available "
                                   + "inside that room. This makes them "
                                   + "available everywhere else too.")
                ToolTip.visible: useEverywhereHover.hovered
                ToolTip.delay: 500
                HoverHandler { id: useEverywhereHover }
            }
        }

        GridView {
            id: grid
            objectName: "stickerGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: picker.stickers.images.count > 0
            cellWidth: Math.floor(width / 4)
            cellHeight: cellWidth
            cacheBuffer: cellHeight * 2
            model: picker.stickers.images
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            SmoothWheelArea {}
            currentIndex: -1
            keyNavigationEnabled: true
            activeFocusOnTab: true
            boundsBehavior: Flickable.StopAtBounds
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0)
                    currentIndex = 0
            }
            Connections {
                target: picker.stickers.images
                function onModelReset() { grid.currentIndex = -1 }
            }
            Keys.onReturnPressed: if (grid.currentIndex >= 0)
                                      picker.choose(grid.currentIndex)
            Keys.onSpacePressed: if (grid.currentIndex >= 0)
                                     picker.choose(grid.currentIndex)
            Keys.onPressed: (event) => {
                if (grid.count <= 0)
                    return
                var cols = Math.max(1, Math.floor(grid.width / grid.cellWidth))
                var page = Math.max(1, Math.floor(grid.height / grid.cellHeight)) * cols
                var at = Math.max(0, grid.currentIndex)
                if (event.key === Qt.Key_Home)
                    grid.currentIndex = 0
                else if (event.key === Qt.Key_End)
                    grid.currentIndex = grid.count - 1
                else if (event.key === Qt.Key_PageDown)
                    grid.currentIndex = Math.min(grid.count - 1, at + page)
                else if (event.key === Qt.Key_PageUp)
                    grid.currentIndex = Math.max(0, at - page)
                else
                    return
                event.accepted = true
            }
            Keys.onEnterPressed: if (grid.currentIndex >= 0)
                                     picker.choose(grid.currentIndex)
            Keys.onUpPressed: (event) => {
                var cols = Math.max(1, Math.floor(grid.width / grid.cellWidth))
                if (grid.currentIndex >= cols) {
                    event.accepted = false
                    return
                }
                searchField.forceActiveFocus()
            }

            delegate: Item {
                id: tile
                width: grid.cellWidth
                height: grid.cellHeight
                required property int index
                required property string shortcode
                required property string url
                required property string body
                required property string mimetype
                readonly property bool current: GridView.isCurrentItem

                // Animated tiles. The still tile shows a server thumbnail (one
                // frame), and AnimatedImage cannot read an image:// URL, so
                // animation plays a materialized file via mxcAnimatedSource,
                // mirroring MessageDelegate's sticker block. The declared
                // mimetype is attacker-writable room state and optional, so it
                // only skips the request when it names PNG or JPEG; MediaBridge
                // decides from the container magic after the §6 markup/gzip
                // refusal. The request is speculative: a non-animation gets
                // silence, not a failure mark.
                readonly property string declaredMimetype:
                    (tile.mimetype || "").toLowerCase()
                readonly property bool maybeAnimated:
                    declaredMimetype === "" || declaredMimetype === "image/gif"
                    || declaredMimetype === "image/webp"
                readonly property int gifMode: MotionPolicy.mode
                readonly property bool playAnimation:
                    MotionPolicy.plays(tileHover.hovered)
                property string animatedSource: ""
                readonly property string animatedCacheKey: "mxcanim:" + tile.url
                function refreshAnimatedSource() {
                    if (tile.url.length === 0 || !app.mediaBridge.supported)
                        return
                    if (!maybeAnimated || gifMode === 2)
                        return
                    tile.animatedSource =
                        app.mediaBridge.mxcAnimatedSource(tile.url)
                }
                Component.onCompleted: refreshAnimatedSource()
                onGifModeChanged: refreshAnimatedSource()
                function snapshot() {
                    return picker.stickers.images.get(tile.index)
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: AppTheme.radiusControl
                    color: tileHover.hovered ? AppTheme.hover : "transparent"
                    border.width: tile.current ? 2 : 0
                    border.color: AppTheme.accent
                }

                Image {
                    id: tileImage
                    anchors.fill: parent
                    anchors.margins: 6
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    sourceSize.width: Math.max(1, Math.ceil(width))
                    sourceSize.height: Math.max(1, Math.ceil(height))
                    visible: !tileAnim.animating
                    property int resolveTick: 0
                    readonly property int stillEdge: 160
                    readonly property string stillCacheKey:
                        "mxcimg:" + stillEdge + ":" + tile.url
                    source: {
                        var _tick = resolveTick
                        return (tile.url.length > 0 && app.mediaBridge.supported)
                            ? app.mediaBridge.mxcImageSource(tile.url, stillEdge)
                            : ""
                    }
                    Connections {
                        target: app.mediaBridge
                        enabled: tile.url.length > 0
                        function onMediaCached(cacheKey) {
                            if (cacheKey === tileImage.stillCacheKey)
                                tileImage.resolveTick++
                        }
                    }
                }
                AnimatedImage {
                    id: tileAnim
                    objectName: "stickerPickerAnimatedTile"
                    anchors.fill: parent
                    anchors.margins: 6
                    fillMode: Image.PreserveAspectFit
                    source: tile.gifMode !== 2 ? tile.animatedSource : ""
                    readonly property bool animating:
                        status === AnimatedImage.Ready
                        && tile.animatedSource.length > 0
                        && tile.playAnimation
                    visible: animating
                    playing: animating && !MotionPolicy.suspended
                    asynchronous: true
                    cache: false
                    sourceSize.width: Math.max(1, Math.ceil(width))
                    sourceSize.height: Math.max(1, Math.ceil(height))
                    AnimationFileHold {}
                }
                Connections {
                    target: app.mediaBridge
                    enabled: tile.url.length > 0
                    function onAnimatedMediaReady(cacheKey) {
                        if (cacheKey === tile.animatedCacheKey)
                            tile.refreshAnimatedSource()
                    }
                }
                Loader {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    active: tileImage.status === Image.Error
                            && !tileAnim.animating
                    sourceComponent: Label {
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Unavailable")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMicro
                    }
                }

                HoverHandler { id: tileHover }
                ToolTip.text: tile.body.length > 0 ? tile.body : tile.shortcode
                ToolTip.visible: tileHover.hovered
                ToolTip.delay: 400
                Accessible.role: Accessible.Button
                Accessible.name: tile.body.length > 0 ? tile.body : tile.shortcode

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onClicked: {
                        grid.currentIndex = tile.index
                        picker.choose(tile.snapshot())
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: picker.stickers.images.count === 0

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing16
                spacing: AppTheme.spacing6

                Label {
                    objectName: "stickerEmptyTitle"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormText
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    text: {
                        var rev = picker.stickers.revision
                        if (picker.searching)
                            return qsTr("No stickers match “%1”")
                                .arg(picker.stickers.filter)
                        if (!picker.stickers.available)
                            return qsTr("Stickers are unavailable on this backend")
                        if (picker.stickers.loading && !picker.stickers.loaded)
                            return qsTr("Loading your sticker packs…")
                        if (picker.stickers.packs.count === 0)
                            return qsTr("You have no sticker packs")
                        if (picker.stickers.usablePackCount === 0)
                            return qsTr("Your packs hold no stickers")
                        return qsTr("This pack has no stickers")
                    }
                }
                Label {
                    objectName: "stickerEmptyBody"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    visible: text.length > 0
                    text: {
                        var rev = picker.stickers.revision
                        if (picker.searching)
                            return qsTr("Try another word, or clear the search "
                                        + "to browse your packs.")
                        if (!picker.stickers.available)
                            return ""
                        if (picker.stickers.loading && !picker.stickers.loaded)
                            return ""
                        if (picker.stickers.packs.count === 0)
                            return qsTr("Right-click any sticker in a chat and "
                                        + "choose “Add to my stickers” to start "
                                        + "your own pack. Packs a room shares "
                                        + "appear here too.")
                        return qsTr("A pack can hold custom emoji instead of "
                                    + "stickers. Those appear in the message "
                                    + "box, not here.")
                    }
                }
            }
        }
    }

    RowLayout {
        id: footerRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: AppTheme.spacing4
        anchors.leftMargin: AppTheme.spacing4 + AppTheme.spacingXS
        anchors.rightMargin: AppTheme.spacing4 + AppTheme.spacingXS
        spacing: AppTheme.spacing6

        Label {
            // Untrusted text: never markup.
            textFormat: Text.PlainText
            id: packSourceLabel
            Layout.fillWidth: true
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMicro
            elide: Text.ElideRight
            text: {
                if (picker.searching)
                    return qsTr("Results from all your packs")
                var pack = picker.selectedPack
                if (!pack)
                    return ""
                if (pack.attribution && pack.attribution.length > 0)
                    return pack.attribution
                return pack.source === "user"
                    ? qsTr("Your pack")
                    : qsTr("Shared by %1").arg(pack.displayName)
            }
            Accessible.name: packSourceLabel.text
        }
        MenuKeycap {
            iconName: "keyboard_return"
            visible: grid.visible
        }
        Label {
            visible: grid.visible
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

    StickerPackEditor {
        id: packEditor
    }
}
