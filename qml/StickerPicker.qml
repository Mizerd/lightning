import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// The sticker picker: MSC2545 image packs, shared by the room and thread
// composers.
//
// Deliberately its OWN popup rather than a third tab on the emoji picker.
// A pack is a different kind of thing from a Unicode emoji: it is remote
// content with an owner, an attribution and a room it may belong to, it can
// be absent entirely, and its failure modes (no packs, a pack that holds only
// emoticons, media that will not load) all need words the emoji grid has no
// place for. Bolting it on would have made every one of those states a
// special case inside a component whose whole job is a fixed local catalogue.
//
// Modelled on GifPicker.qml — same AnchoredPopup chrome, same remembered
// size, same press-sink discipline, same one-shot activation latch — because
// the two are peers in the composer row and should not feel like two
// different applications.
//
// # What a tile is, and what it is not
//
// Every tile is a `mxc://` from a pack, resolved through the MediaBridge like
// any other Matrix media. Nothing here fetches an arbitrary URL: a pack image
// whose url was not a syntactically valid mxc was DROPPED in Rust, precisely
// so that a hostile pack cannot put a tracking beacon on a picker tile that
// fires once per listing.
//
// Shortcodes, bodies, pack names and attribution are remote text. They are
// set on `text` — never `textFormat: Text.RichText`, never a URL, never a
// command — and were bounded and stripped of control characters in Rust.
AnchoredPopup {
    id: picker

    // "room" or "thread" — routes the eventual send and decides which
    // composer takes focus back.
    property string target: "room"
    // The chosen image, as the manager's own row map (shortcode, url, body,
    // mimetype, width, height, size, isEmoticon, isSticker).
    signal stickerChosen(var image)

    readonly property var stickers: app.stickers

    // Same proportions and the SAME remembered size key as the emoji and GIF
    // pickers: all three float from the composer anchor, and resizing one
    // should not leave its neighbour at a different size.
    widthFraction: 0.38
    heightFraction: 0.64
    minWidth: 300
    minHeight: 320
    sizeSettingsKey: "picker"
    padding: AppTheme.spacingS
    // NOT modal, matching EmojiPicker/GifPicker: modality was never the press
    // barrier (a Popup does not consume a press landing inside it), and a
    // grabbed overlay stops the timeline scrolling while the picker is open.
    // The tiles and the background sink consume presses.
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // One-shot activation latch, exactly as in GifPicker: close() starts an
    // exit transition rather than tearing the popup down, so a second
    // activation can still reach this function while the picker is visually
    // closing. Reset on the NEXT open, so a close by Escape or press-outside
    // leaves a fresh latch.
    property bool activated: false
    // `imageOrRow` is normally the delegate's OWN captured map, taken from
    // the exact tile the user clicked, so it cannot drift from what was on
    // screen. Keyboard activation has no delegate to snapshot from and
    // passes a row number, which is resolved against the model IMMEDIATELY
    // in this same call — never stored for later.
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
        // The ONE place this component asks the network. A refresh costs a
        // global-account-data read plus a bounded /state read per room pack,
        // so it happens when a person opens the picker — never on room
        // navigation, and never on a timer.
        picker.stickers.refreshIfStale()
    }

    // The selected pack's attribution, when it declared one. MSC2545 makes it
    // optional and most packs have none, so the footer falls back to naming
    // where the pack came from rather than showing an empty strip.
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

            // The press barrier. A Popup does NOT consume a press that lands
            // on it, so anything the picker's own controls do not accept
            // keeps walking down to the timeline behind the overlay — which
            // is how a right-click on picker chrome opened the message
            // context menu on top of the picker. This fills the whole
            // popupItem, padding included, and sits below contentItem, so
            // every real control still sees the press first.
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

        // ── Header: title, refresh, close ──────────────────────────────
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
            // ADD A STICKER FROM THIS COMPUTER.
            //
            // The only way to create a pack from nothing: every other route
            // into one needs a sticker somebody already sent you, so an
            // account with no packs had no way in at all.
            FileDialog {
                id: stickerFileDialog
                title: qsTr("Choose a sticker")
                nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif)")]
                // Straight to the pack: a sticker is used at whatever size it
                // was made, so there is nothing for a crop step to decide.
                // The bytes are sniffed and bounded in Rust.
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

        // ── The pack strip ─────────────────────────────────────────────
        //
        // One tile per pack that holds at least one STICKER. A pack that is
        // emoticons-only is not shown here — a tab that opens on an empty
        // grid is worse than no tab.
        ListView {
            id: packStrip
            objectName: "stickerPackStrip"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            // `usablePackCount`, not `packs.count`: an account whose packs
            // hold only custom emoji would otherwise get an empty 40px band
            // above an empty grid. (A MIXED account still contributes the
            // list's 4px spacing for each hidden tile — cosmetic, and cheaper
            // than a second filtered model.)
            visible: picker.stickers.usablePackCount > 0
            orientation: ListView.Horizontal
            clip: true
            spacing: AppTheme.spacingXS
            model: picker.stickers.packs
            boundsBehavior: Flickable.StopAtBounds
            // Horizontal strip: the shared wheel component on its horizontal
            // axis, so a mouse wheel over the pack tabs scrolls them with the
            // same feel as every other pane. Without `axis` it would derive
            // its bounds from contentHeight/height, which are equal here, and
            // silently swallow every wheel event.
            SmoothWheelArea { axis: "horizontal" }

            delegate: AbstractButton {
                id: packTile
                required property int index
                required property string packId
                required property string displayName
                required property string avatarUrl
                required property string source
                required property int stickerCount

                // Emoticon-only packs are not sticker packs for this surface.
                visible: stickerCount > 0
                width: visible ? 36 : 0
                height: 36
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                // The pack's own name is remote text and is shown as a plain
                // accessible label and tooltip — never as markup.
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
                        // A pack with no avatar falls back to initials from
                        // its own name, exactly like a room with no picture.
                        name: packTile.displayName
                        colorKey: packTile.packId
                    }
                }
                onClicked: picker.stickers.selectedPackId = packTile.packId
            }
        }

        // ── "Use everywhere", for the SELECTED ROOM pack ───────────────
        //
        // This writes `im.ponies.emote_rooms`, the third of MSC2545's three
        // events: which room packs this ACCOUNT wants available OUTSIDE their
        // own room. It is account data, so no power level is involved — and
        // it is deliberately not offered for the account's own pack, which is
        // global by definition and has nothing that event could describe.
        //
        // NOT applied optimistically. AppSwitch is a bare control that never
        // mutates its own `checked` (that is its whole contract), so this
        // binding follows the last authoritative snapshot and only moves once
        // the write completed and the snapshot was re-read. A server refusal
        // therefore cannot leave it showing a state the account does not have.
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

        // ── The grid ───────────────────────────────────────────────────
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
            // A highlighted row is just an int, and Qt does not remap it when
            // the model changes underneath. Every pack change is a full
            // reset, so drop the highlight rather than let Return resolve it
            // against a different pack's image.
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
                readonly property bool current: GridView.isCurrentItem
                // The exact row this tile is showing, captured at activation
                // time so a refresh landing mid-click cannot swap it.
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
                    // Re-resolve through a counter the binding READS, never
                    // by assigning `source`: an imperative write destroys the
                    // binding, and the tile would then keep painting the
                    // first image it ever loaded for the rest of the session
                    // (the 2026-08-23 sticky-banner defect).
                    property int resolveTick: 0
                    source: {
                        var _tick = resolveTick
                        return (tile.url.length > 0 && app.mediaBridge.supported)
                            ? app.mediaBridge.mxcImageSource(tile.url, 160)
                            : ""
                    }
                    Connections {
                        target: app.mediaBridge
                        enabled: tile.url.length > 0
                        function onMediaCached(cacheKey) {
                            if (cacheKey.endsWith(":" + tile.url))
                                tileImage.resolveTick++
                        }
                    }
                }
                // A sticker that will not load says so rather than leaving a
                // blank square the user keeps clicking.
                Loader {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    active: tileImage.status === Image.Error
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
                // The shortcode is remote text: a plain tooltip, never markup.
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

        // ── Empty states ───────────────────────────────────────────────
        //
        // Four different facts, said differently. "Nothing has been read
        // yet", "this backend has no packs at all", "you have no packs" and
        // "this pack holds no stickers" are not the same message, and one
        // generic line for all four is how a working feature looks broken.
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

    // ── Footer: where the selected pack came from, and the send hint ────
    RowLayout {
        id: footerRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: AppTheme.spacing4
        spacing: AppTheme.spacing6

        Label {
            id: packSourceLabel
            Layout.fillWidth: true
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMicro
            elide: Text.ElideRight
            // Attribution when the pack declared one (MSC2545 makes it
            // optional), otherwise the pack's own name and where it lives.
            // Both are remote text and both are plain.
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

    } // contentItem Item
}
