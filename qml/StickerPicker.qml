import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Sticker picker for MSC2545 image packs, shared by the room and thread
// composers. Its own popup rather than an emoji-picker tab: a pack is remote
// content with an owner, attribution and possibly a room, and its failure
// states need their own words. Modelled on GifPicker.qml (same chrome,
// remembered size, press sink and activation latch).
//
// Every tile is a pack mxc resolved through the MediaBridge; pack images with
// an invalid mxc are dropped in Rust, so a pack cannot plant a tracking URL.
// Shortcodes, bodies, pack names and attribution are remote text, bounded and
// control-stripped in Rust, and set as plain text only.
AnchoredPopup {
    id: picker

    // "room" or "thread": routes the send and decides which composer gets focus
    // back.
    property string target: "room"
    // The chosen image as the manager's row map (shortcode, url, body,
    // mimetype, width, height, size, isEmoticon, isSticker).
    signal stickerChosen(var image)
    // See GifPicker.qml: the two pickers are one window with two tabs, swapped
    // by the host.
    signal kindRequested(string kind)
    property bool offerKindTabs: false

    readonly property var stickers: app.stickers

    // Same proportions and remembered size key as the emoji and GIF pickers.
    widthFraction: 0.38
    heightFraction: 0.64
    minWidth: 300
    minHeight: 320
    sizeSettingsKey: "picker"
    padding: AppTheme.spacingS
    // Not modal, like EmojiPicker/GifPicker: a grabbed overlay stops the
    // timeline scrolling. The tiles and background sink consume presses.
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // One-shot activation latch, as in GifPicker: close() starts an exit
    // transition, so a second activation can arrive while closing. Reset on the
    // next open.
    property bool activated: false
    // Normally the clicked tile's own captured map. Keyboard activation passes
    // a row number, resolved against the model immediately.
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

    onAboutToShow: {
        picker.stickers.usage = "sticker"
        grid.currentIndex = -1
        activated = false
        // The only network request: account data plus a bounded /state read per
        // room pack, made when the picker opens, never on navigation or a
        // timer.
        picker.stickers.refreshIfStale()
    }

    // The selected pack's attribution, if declared (optional in MSC2545);
    // otherwise the footer names where the pack came from.
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
        anchors.bottomMargin: footerRow.implicitHeight + AppTheme.spacing4 * 2
        spacing: AppTheme.spacingS

        // Row 0: which kind of media this window shows
        SegmentedControl {
            objectName: "pickerKindTabs"
            visible: picker.offerKindTabs
            // Explicit left alignment, like the rows below.
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

        // Header: title, refresh, close
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacingXS

            Label {
                objectName: "stickerPickerTitle"
                Layout.fillWidth: true
                text: qsTr("Stickers")
                color: AppTheme.stormText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightStrong
                elide: Label.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            AppBusyIndicator {
                objectName: "stickerPickerBusy"
                visible: picker.stickers.loading
                implicitWidth: 18
                implicitHeight: 18
            }
            // Add a sticker from this computer: the only way to create a pack
            // from nothing.
            FileDialog {
                id: stickerFileDialog
                title: qsTr("Choose a sticker")
                nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif)")]
                // Straight to the pack, no crop step. The bytes are sniffed and
                // bounded in Rust.
                onAccepted: picker.stickers.uploadSticker(selectedFile, "")
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
            // Manage the selected pack. The editor is read-only when this
            // account may not write it, so members can still see its contents.
            IconButton {
                objectName: "stickerManageButton"
                storm: true
                size: "md"
                // edit_square: the icon font is a subset and Icon.qml returns
                // "" for unknown names, so a wrong name is a blank button.
                iconName: "edit_square"
                enabled: picker.stickers.available
                         && picker.stickers.selectedPackId.length > 0
                         && !picker.stickers.editing
                Accessible.name: qsTr("Manage this pack")
                ToolTip.text: qsTr("Manage pack")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: {
                    // The manager owns the row lookup and the permission rule.
                    var pack = picker.stickers.packInfo(
                        picker.stickers.selectedPackId)
                    if (!pack || !pack.packId)
                        return
                    packEditor.stickers = picker.stickers
                    packEditor.openFor(pack.packId, pack.displayName,
                                       pack.canManage)
                }
            }
            IconButton {
                storm: true
                size: "md"
                iconName: "refresh"
                enabled: !picker.stickers.loading
                Accessible.name: qsTr("Reload sticker packs")
                ToolTip.text: qsTr("Reload packs")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: picker.stickers.refresh()
            }
            IconButton {
                storm: true
                size: "md"
                iconName: "close"
                Accessible.name: qsTr("Close sticker picker")
                onClicked: picker.close()
            }
        }

        // Pack strip: one tile per pack holding at least one sticker.
        ListView {
            id: packStrip
            objectName: "stickerPackStrip"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            // usablePackCount, so an emoticon-only account gets no empty band.
            // (A mixed account keeps 4px of spacing per hidden tile; cosmetic.)
            visible: picker.stickers.usablePackCount > 0
            orientation: ListView.Horizontal
            clip: true
            spacing: AppTheme.spacingXS
            model: picker.stickers.packs
            boundsBehavior: Flickable.StopAtBounds
            // Horizontal axis: the default would derive bounds from
            // contentHeight/height (equal here) and swallow every wheel event.
            SmoothWheelArea { axis: "horizontal" }

            delegate: AbstractButton {
                id: packTile
                required property int index
                required property string packId
                required property string displayName
                required property string avatarUrl
                required property string source
                required property int stickerCount

                // Emoticon-only packs are not sticker packs here.
                visible: stickerCount > 0
                width: visible ? 36 : 0
                height: 36
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                // Remote text: a plain accessible label and tooltip.
                Accessible.name: packTile.displayName
                ToolTip.text: packTile.displayName
                ToolTip.visible: hovered
                ToolTip.delay: 500
                readonly property bool current:
                    packTile.packId === picker.stickers.selectedPackId

                background: Rectangle {
                    anchors.fill: parent
                    radius: AppTheme.radiusControl
                    color: packTile.current ? AppTheme.selected
                         : (packTile.down || packTile.hovered)
                           ? AppTheme.hover : "transparent"
                    border.width: packTile.current ? 2 : 0
                    border.color: AppTheme.accent
                }
                contentItem: Item {
                    Avatar {
                        anchors.centerIn: parent
                        size: 26
                        circle: false
                        squareRadius: AppTheme.radiusSm
                        mxc: packTile.avatarUrl
                        // A pack without an avatar shows initials from its
                        // name.
                        name: packTile.displayName
                        colorKey: packTile.packId
                    }
                }
                onClicked: picker.stickers.selectedPackId = packTile.packId
            }
        }

        // "Use everywhere" for the selected room pack: writes
        // im.ponies.emote_rooms (account data, no power level). Not offered for
        // the account's own pack. Not optimistic: AppSwitch never sets its own
        // `checked`, so this follows the snapshot re-read after the write.
        RowLayout {
            id: useEverywhereRow
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            visible: picker.selectedPack !== null
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

        // The grid
        GridView {
            id: grid
            objectName: "stickerGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: picker.stickers.images.count > 0
            cellWidth: Math.floor(width / 4)
            cellHeight: cellWidth
            cacheBuffer: cellWidth * 2
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
            // The highlighted row is an int that Qt does not remap; every pack
            // change is a reset, so drop it.
            Connections {
                target: picker.stickers.images
                function onModelReset() { grid.currentIndex = -1 }
            }
            Keys.onReturnPressed: if (grid.currentIndex >= 0)
                                      picker.choose(grid.currentIndex)
            Keys.onEnterPressed: if (grid.currentIndex >= 0)
                                     picker.choose(grid.currentIndex)

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
                readonly property int gifMode: app.settings.gifAutoplay
                readonly property bool playAnimation:
                    gifMode === 0 || (gifMode === 1 && tileHover.hovered)
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
                // Turning autoplay back on starts the fetch that gifMode === 2
                // refused.
                onGifModeChanged: refreshAnimatedSource()
                // The exact row this tile shows, captured at activation.
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
                    cache: true
                    // The still frame is the default and fallback until the
                    // animation reports Ready.
                    visible: !tileAnim.animating
                    // Re-resolve through a counter the binding reads; assigning
                    // `source` would destroy the binding.
                    property int resolveTick: 0
                    // Must match the edge in the source binding: the bridge key
                    // is "mxcimg:<edge>:<mxc>".
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
                            // The exact still key; a suffix match would also
                            // fire on the animation's bytes.
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
                    // Loaded whenever animated bytes exist and animation is not
                    // off, so on-hover playback starts immediately.
                    source: tile.gifMode !== 2 ? tile.animatedSource : ""
                    readonly property bool animating:
                        status === AnimatedImage.Ready
                        && tile.animatedSource.length > 0
                        && tile.playAnimation
                    visible: animating
                    playing: animating
                    asynchronous: true
                    cache: true
                }
                Connections {
                    target: app.mediaBridge
                    enabled: tile.url.length > 0
                    function onAnimatedMediaReady(cacheKey) {
                        // MediaBridge validated these bytes as an animation;
                        // only now does the AnimatedImage get a source.
                        if (cacheKey === tile.animatedCacheKey)
                            tile.refreshAnimatedSource()
                    }
                }
                // A sticker that will not load says so.
                Loader {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    // Never over a playing animation: the thumbnail and
                    // animation are separate fetches.
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
                // Remote text: a plain tooltip.
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

        // Empty states: not read yet, no backend support, no packs, and a pack
        // with no stickers each get their own message.
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
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormText
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    text: {
                        var rev = picker.stickers.revision
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

    // Footer: where the selected pack came from, and the send hint
    RowLayout {
        id: footerRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: AppTheme.spacing4
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
            // Attribution when declared, otherwise the pack's name and where it
            // lives. Both plain text.
            text: {
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
        arcCentre: AppTheme.radiusLg + 6
        outerRadius: AppTheme.radiusLg + 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: -picker.padding
        anchors.topMargin: -picker.padding
    }

    }

    StickerPackEditor {
        id: packEditor
    }
}
