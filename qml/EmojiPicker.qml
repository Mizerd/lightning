import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// One window-overlay picker shared by the reaction and composer entry points.
// Catalogue, search and recents live in the process-wide C++ model.
//
// There is deliberately no global skin-tone swatch: the grid never reads
// EmojiCatalog's preferredTone, so the shared per-emoji tone popup is the
// real mechanism. The press sink in `background` is what makes clicks work;
// read it before touching the popup flags. Button hosts set `anchorItem`;
// reaction entry points pass a bare `anchorPoint` (no stable item under a
// message-row point).
//
// Two contexts, two sizes. The composer picker is a share of the composer
// card ("picker", shared with the GIF picker). A reaction opens COMPACT: a
// quick bar of recent emoji and a "more" cell (`expanded` false), which
// expands into a compact grid with its own default and its own remembered
// size ("reaction"). A share remembered against the composer card meant
// something else entirely against the whole window, which is how one key made
// the reaction picker a 1000x740 board.
AnchoredPopup {
    id: picker
    property string mode: "composer"
    property bool closeAfterSelection: true

    readonly property bool reactionMode: mode === "reaction"
    // Reaction mode only: false shows the quick bar, true the grid. Reset on
    // every open from `openExpanded` (for callers that already offered the
    // quick reactions, e.g. the context menu's "more").
    property bool expanded: false
    property bool openExpanded: false
    readonly property bool compact: reactionMode && !expanded
    signal emojiChosen(string emoji)
    // The nested tone popup's lifetime, for the owner of transient row
    // interaction (TimelinePane's transientInteractionOwner).
    signal toneOpened()
    signal toneClosed()

    // Footer preview: the emoji and name of the hovered or keyboard-focused
    // cell. Empty falls back to the static keyboard hint.
    property string previewEmoji: ""
    property string previewName: ""
    // The preview's cell; Alt+V needs the item to position the tone popup.
    property Item previewCell: null

    // Category -> Material Symbols glyph, keyed by EmojiCatalog's exact
    // category strings (kCategories in EmojiCatalog.cpp).
    readonly property var _categoryIcons: ({
        "Recently Used": "schedule",
        "Smileys & Emotion": "mood",
        "People & Body": "group",
        "Animals & Nature": "pets",
        "Food & Drink": "restaurant",
        "Travel & Places": "flight",
        "Activities": "sports_esports",
        "Objects": "lightbulb",
        "Symbols": "emoji_symbols",
        "Flags": "flag",
    })

    // Heading for the single visible bucket. EmojiCatalog swaps one
    // precomputed bucket at a time (recents, a category or search results),
    // which keeps category switching constant-time
    // (categorySwitchingIsBucketSwapFast).
    readonly property string sectionHeading:
        search.text.length > 0 ? qsTr("Search results")
        : app.emojiCatalog.category === "Recently Used" ? qsTr("Recently used")
        : app.emojiCatalog.category

    // Composer: a share of the available space (see AnchoredPopup), sharing
    // the GIF picker's remembered size. Reaction: a compact default in pixels
    // (the available space is the whole window) under its own key. The
    // minimum keeps the category rail (298 px inside 2 x 12 px margins) whole
    // and a few grid rows usable. The grab band (below) is not content, so it
    // is added on top.
    widthFraction: 0.36
    heightFraction: 0.58
    autoWidth: reactionMode ? 360 + gripBandX : 0
    autoHeight: reactionMode ? 420 + gripBandY : 0
    // Guarded: contentItem (where quickBar lives) is deferred.
    fixedWidth: compact && quickBar ? quickBar.implicitWidth : 0
    fixedHeight: compact && quickBar ? quickBar.implicitHeight : 0
    minWidth: 324 + gripBandX
    minHeight: 320 + gripBandY
    sizeSettingsKey: reactionMode ? "reaction" : "picker"

    // ── Resize grab band ── A press just outside the popup's item is outside
    // the popup: it closes it and, unless modal, reaches the timeline behind,
    // which then scrolls under the drag that was meant to resize (the report:
    // "it starts to scroll around the room"). A grip cannot live outside the
    // item, so the panel is inset by `gripGrab` on the grip's two edges,
    // leaving a transparent band that still counts as inside the popup. As
    // GifPicker.qml. No band while there is no grip (the quick bar).
    readonly property real gripGrab: 8
    readonly property bool gripShown: !compact
    readonly property bool gripOnRight: gripCorner === "topRight"
                                        || gripCorner === "bottomRight"
    readonly property bool gripOnBottom: gripCorner === "bottomLeft"
                                         || gripCorner === "bottomRight"
    readonly property real gripBandX: gripShown ? gripGrab : 0
    readonly property real gripBandY: gripShown ? gripGrab : 0
    padding: 0
    leftInset: gripShown && !gripOnRight ? gripGrab : 0
    rightInset: gripShown && gripOnRight ? gripGrab : 0
    topInset: gripShown && !gripOnBottom ? gripGrab : 0
    bottomInset: gripShown && gripOnBottom ? gripGrab : 0
    // Insets move only the background, so the content needs matching padding.
    leftPadding: leftInset
    rightPadding: rightInset
    topPadding: topInset
    bottomPadding: bottomInset

    // The composer picker is not modal: a Popup doesn't consume presses inside
    // it anyway, and a grabbed overlay would stop the timeline scrolling while
    // it is open. Grid cells consume their own presses and the background sink
    // catches the chrome. The reaction picker IS modal: it is placed at a
    // point in a scrolling row, so scrolling under it would leave it pointing
    // at another message, and a press outside must only dismiss it, never
    // also land on (and drag) the timeline.
    modal: reactionMode
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // From the quick bar to the grid, optionally with the search already
    // typed (a key pressed on the bar). Re-placed at the same point, so the
    // grid opens next to the message too and stays on screen.
    function expand(initialText) {
        if (!reactionMode || expanded)
            return
        expanded = true
        if (!anchorItem)
            placeAtPoint()
        if (initialText && initialText.length > 0) {
            search.text = initialText
            app.emojiCatalog.searchText = initialText
        }
        // Synchronous: the rest of a typed word must land in the field.
        search.forceActiveFocus()
    }

    function choose(emoji) {
        if (!emoji || emoji.length === 0) return
        app.emojiCatalog.recordUse(emoji)
        emojiChosen(emoji)
        if (closeAfterSelection) close()
    }

    // MSC2545 custom emoji, reaction mode only. A custom-emoji reaction is an
    // m.reaction whose key is the pack image's mxc URI (the Cinny/Sable
    // convention); MessageDelegate renders such a key as an image.
    //
    // Not offered in composer mode: sending one inside a message needs
    // `<img data-mx-emoticon>` in formatted_body, which our HTML sanitizer
    // doesn't yet accept on receive. Doesn't call recordUse(): recents are the
    // Unicode catalogue's.
    readonly property bool customEmojiOffered:
        mode === "reaction" && app.stickers.available
    readonly property var customEmoji: {
        // findEmoticons() is a plain call; reading `revision` re-evaluates this
        // when a snapshot lands.
        var _live = app.stickers.revision
        return picker.customEmojiOffered
            ? app.stickers.findEmoticons("", 32) : []
    }
    function chooseCustom(url) {
        if (!url || url.indexOf("mxc://") !== 0) return
        emojiChosen(url)
        if (closeAfterSelection) close()
    }

    // The single shared skin-tone popup, positioned at the requesting cell
    // (never one per grid cell).
    function openTonePopupFor(cellItem, baseEmoji) {
        tonePopup.variants = app.emojiCatalog.variantsFor(baseEmoji)
        if (tonePopup.variants.length === 0)
            return
        var p = cellItem.mapToItem(picker.contentItem, 0, cellItem.height)
        tonePopup.x = Math.max(0, Math.min(p.x,
                                           picker.contentItem.width
                                           - tonePopup.width))
        tonePopup.y = Math.max(0, Math.min(p.y,
                                           picker.contentItem.height
                                           - tonePopup.height))
        tonePopup.open()
    }

    // Alt+V lives on the picker, not on a cell: the picker opens with focus in
    // the search field, so a cell-level handler never saw the key. Targets the
    // hovered cell, else the keyboard-current one.
    Shortcut {
        sequences: ["Alt+V"]
        // Not on the quick bar: its grid is hidden.
        enabled: picker.visible && !picker.compact
        onActivated: picker.openTonesForCurrentCell()
    }
    function openTonesForCurrentCell() {
        var cell = picker.previewCell
        if (!cell || !cell.hasSkinTones)
            cell = emojiGrid.currentItem
        if (!cell || !cell.hasSkinTones)
            return
        picker.openTonePopupFor(cell, cell.baseEmoji)
    }

    // AnchoredPopup's own onAboutToShow (a Connections in the base) loads the
    // remembered size and places the popup; the two handlers run in no
    // promised order, so a point-placed picker is placed again here once its
    // stage is known.
    onAboutToShow: {
        expanded = openExpanded
        openExpanded = false
        if (!anchorItem)
            placeAtPoint()
        search.text = ""
        app.emojiCatalog.searchText = ""
        previewEmoji = ""
        previewName = ""
        previewCell = null
        quickBar.focusIndex = 0
        // Reaction mode only: load this account's MSC2545 packs.
        // refreshIfStale() is a no-op when a snapshot is already in hand.
        if (picker.customEmojiOffered)
            app.stickers.refreshIfStale()
        Qt.callLater(function() {
            if (picker.compact)
                quickBar.forceActiveFocus()
            else
                search.forceActiveFocus()
        })
    }

    background: Item {
        Rectangle {
            id: pickerPanel
            objectName: "emojiPickerPanel"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            // The quick bar is a pill; the grid keeps the popover corner the
            // resize arc is drawn concentric with.
            radius: picker.compact ? height / 2 : AppTheme.menuRadius + 6

            // The press barrier. A Popup doesn't consume presses on itself
            // (blockInput() is false inside its own item), and nothing else
            // here stops a right press: TapHandlers grab without accepting, and
            // the GridView accepts left presses only. Right presses would reach
            // MessageDelegate's context-menu TapHandler behind the picker. It
            // sits below contentItem, so it only catches what would leave the
            // picker. Extended over the grab band, which is inside the popup
            // but outside this Rectangle.
            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -picker.leftInset
                anchors.rightMargin: -picker.rightInset
                anchors.topMargin: -picker.topInset
                anchors.bottomMargin: -picker.bottomInset
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

    // Wrapped in an Item so the resize grip can sit over the column without
    // becoming another row.
    contentItem: Item {

    // ── Reaction quick bar: the compact stage ──
    // Recent emoji first, then common defaults, eight in all, and a cell that
    // expands to the grid. One focus stop with a virtual cursor: arrows move
    // it, Enter or Space reacts, Down expands, any typed text expands with
    // the search filled in, Esc closes (the popup's own policy). Emoji
    // literals are allowed here, as in QuickReactionStrip.qml.
    Item {
        id: quickBar
        objectName: "reactionQuickBar"
        visible: picker.compact
        anchors.fill: parent
        readonly property int cellSize: 38
        readonly property int pad: 6
        readonly property var _defaults:
            ["👍", "❤️", "😂", "😮", "😢", "🙏", "🎉", "🔥"]
        readonly property var quickEmoji: {
            var out = []
            var recents = app.emojiCatalog.recentEmoji || []
            for (var i = 0; i < recents.length && out.length < 8; ++i) {
                if (recents[i] && out.indexOf(recents[i]) < 0)
                    out.push(recents[i])
            }
            for (var j = 0; j < _defaults.length && out.length < 8; ++j) {
                if (out.indexOf(_defaults[j]) < 0)
                    out.push(_defaults[j])
            }
            return out
        }
        readonly property int quickCount: quickEmoji.length
        // 0..quickCount-1 are emoji; quickCount is the "more" cell.
        property int focusIndex: 0
        // The ring shows only once the keyboard is used, not on every open.
        property bool keyboardActive: false
        implicitWidth: 2 * pad + (quickCount + 1) * cellSize
                       + quickSeparator.width
        implicitHeight: 2 * pad + cellSize
        // Constant: Qt refuses to clear it while the item has focus, and an
        // invisible item is never a Tab stop anyway.
        activeFocusOnTab: true
        Accessible.role: Accessible.ToolBar
        Accessible.name: qsTr("Quick reactions")
        onVisibleChanged: keyboardActive = false

        function activate(index) {
            if (index >= quickCount)
                picker.expand()
            else
                picker.choose(quickEmoji[index])
        }
        Keys.onPressed: (event) => {
            var plain = !(event.modifiers & (Qt.ControlModifier
                                             | Qt.AltModifier
                                             | Qt.MetaModifier))
            switch (event.key) {
            case Qt.Key_Left:
                focusIndex = Math.max(0, focusIndex - 1)
                keyboardActive = true
                event.accepted = true
                return
            case Qt.Key_Right:
                focusIndex = Math.min(quickCount, focusIndex + 1)
                keyboardActive = true
                event.accepted = true
                return
            case Qt.Key_Home:
                focusIndex = 0
                keyboardActive = true
                event.accepted = true
                return
            case Qt.Key_End:
                focusIndex = quickCount
                keyboardActive = true
                event.accepted = true
                return
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                activate(focusIndex)
                event.accepted = true
                return
            case Qt.Key_Down:
                picker.expand()
                event.accepted = true
                return
            }
            // Typing starts a search. Control characters (Esc, Tab, Backspace)
            // are not text.
            if (plain && event.text.length > 0
                    && event.text.charCodeAt(0) >= 0x20
                    && event.text.charCodeAt(0) !== 0x7f) {
                picker.expand(event.text)
                event.accepted = true
            }
        }

        Row {
            id: quickRow
            x: quickBar.pad
            y: quickBar.pad
            spacing: 0
            Repeater {
                model: quickBar.quickEmoji
                delegate: Item {
                    id: quickCell
                    required property int index
                    required property string modelData
                    readonly property bool current: quickBar.activeFocus
                        && quickBar.keyboardActive
                        && quickBar.focusIndex === index
                    width: quickBar.cellSize
                    height: quickBar.cellSize
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("React with %1").arg(modelData)
                    Accessible.onPressAction: quickBar.activate(index)
                    Rectangle {
                        anchors.fill: parent
                        radius: AppTheme.radiusControl
                        color: quickHover.hovered || quickCell.current
                               ? AppTheme.stormSelection : "transparent"
                        border.width: quickCell.current ? 2 : 0
                        border.color: AppTheme.bolt
                    }
                    Label {
                        anchors.centerIn: parent
                        text: quickCell.modelData
                        // Named, not left to fallback (see the grid cell).
                        font.family: app.emojiFontFamily || ""
                        font.pixelSize: 24
                        // A small lift under the pointer.
                        scale: quickHover.hovered ? 1.18 : 1
                        Behavior on scale {
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }
                    }
                    HoverHandler { id: quickHover }
                    // A MouseArea, as in the grid: it accepts, so the press
                    // stops here and never reaches the row underneath.
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: quickBar.activate(quickCell.index)
                    }
                }
            }
            Item {
                id: quickSeparator
                width: 9
                height: quickBar.cellSize
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height - 2 * AppTheme.spacing8
                    color: AppTheme.stormBorder
                }
            }
            Item {
                id: quickMore
                objectName: "reactionQuickMore"
                readonly property bool current: quickBar.activeFocus
                    && quickBar.keyboardActive
                    && quickBar.focusIndex === quickBar.quickCount
                width: quickBar.cellSize
                height: quickBar.cellSize
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Show all emoji")
                Accessible.onPressAction: picker.expand()
                ToolTip.text: qsTr("All emoji")
                ToolTip.visible: quickMoreHover.hovered
                ToolTip.delay: 500
                Rectangle {
                    anchors.fill: parent
                    radius: AppTheme.radiusControl
                    color: quickMoreHover.hovered || quickMore.current
                           ? AppTheme.stormSelection : "transparent"
                    border.width: quickMore.current ? 2 : 0
                    border.color: AppTheme.bolt
                }
                Icon {
                    anchors.centerIn: parent
                    name: "add_reaction"
                    size: 20
                    color: quickMoreHover.hovered || quickMore.current
                           ? AppTheme.stormText : AppTheme.stormTextMuted
                }
                HoverHandler { id: quickMoreHover }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: picker.expand()
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !picker.compact

        // ── Row 1: search only (no skin-tone swatch; see file header) ──
        AppTextField {
            id: search
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing12
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.bottomMargin: AppTheme.spacing10
            searchIcon: true
            storm: true
            placeholderText: qsTr("Search emoji")
            Accessible.name: qsTr("Search emoji by name or keyword")
            selectByMouse: true
            onTextEdited: searchTimer.restart()
            Keys.onDownPressed: emojiGrid.forceActiveFocus()
            Timer {
                id: searchTimer
                interval: 150
                repeat: false
                onTriggered: app.emojiCatalog.searchText = search.text
            }
        }

        // ── Row 2: category icon rail ──
        ScrollView {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.bottomMargin: AppTheme.spacing8
            Layout.preferredHeight: 28
            contentWidth: categoryRow.implicitWidth
            // ScrollView doesn't clip by default; the rail would paint past the
            // rounded border on a narrow popup.
            clip: true
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AlwaysOff }
            ScrollBar.horizontal: AppScrollBar { thin: true; policy: ScrollBar.AsNeeded }
            Row {
                id: categoryRow
                spacing: AppTheme.spacing2
                Repeater {
                    model: app.emojiCatalog.categories
                    delegate: AbstractButton {
                        id: categoryCell
                        required property string modelData
                        readonly property bool selected:
                            app.emojiCatalog.category === modelData
                        // 10 × 28 + 9 × 2 = 298, so the whole rail fits at the
                        // minimum gutter width.
                        implicitWidth: 28
                        implicitHeight: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData
                        Accessible.checked: categoryCell.selected
                        ToolTip.text: modelData
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            app.emojiCatalog.category = modelData
                            search.text = ""
                            app.emojiCatalog.searchText = ""
                            emojiGrid.forceActiveFocus()
                        }
                        contentItem: Icon {
                            name: picker._categoryIcons[categoryCell.modelData]
                                  || "mood"
                            size: 18
                            // Three states: selected, hovered/focused, rest.
                            color: categoryCell.selected ? AppTheme.bolt
                                 : categoryCell.hovered || categoryCell.visualFocus
                                   ? AppTheme.stormText
                                   : AppTheme.stormTextMuted
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusControl
                            color: categoryCell.selected || categoryCell.hovered
                                   || categoryCell.visualFocus
                                   ? AppTheme.stormSelection : "transparent"
                            // Keyboard focus ring (the rail is Tab-reachable).
                            border.width: categoryCell.visualFocus ? 2 : 0
                            border.color: AppTheme.bolt
                            // The active category's 2px bolt underline.
                            Rectangle {
                                visible: categoryCell.selected
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.leftMargin: 2
                                anchors.rightMargin: 2
                                height: 2
                                color: AppTheme.bolt
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        // ── Body: one section heading + the current bucket's grid ──
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8

            ColumnLayout {
                anchors.fill: parent
                spacing: AppTheme.spacing6

                // ── Custom emoji from this account's MSC2545 packs ──
                // Reaction mode only, and only when a pack holds emoticons.
                MenuSectionLabel {
                    Layout.fillWidth: true
                    visible: picker.customEmoji.length > 0
                    text: qsTr("Custom emoji")
                }
                ListView {
                    id: customEmojiStrip
                    objectName: "customEmojiStrip"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    visible: picker.customEmoji.length > 0
                    orientation: ListView.Horizontal
                    clip: true
                    spacing: AppTheme.spacing4
                    model: picker.customEmoji
                    boundsBehavior: Flickable.StopAtBounds
                    // Horizontal wheel scrolling; see qml/SmoothWheelArea.qml.
                    // Must be a direct child so `scrollTarget` finds this
                    // ListView.
                    SmoothWheelArea { axis: "horizontal" }

                    delegate: AbstractButton {
                        id: customCell
                        required property var modelData
                        width: 28
                        height: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.Button
                        // The shortcode is remote text: plain label and tooltip
                        // only.
                        Accessible.name: customCell.modelData.shortcode
                        ToolTip.text: ":" + customCell.modelData.shortcode + ":"
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        background: Rectangle {
                            anchors.fill: parent
                            radius: AppTheme.radiusControl
                            color: (customCell.down || customCell.hovered)
                                   ? AppTheme.hover : "transparent"
                            border.width: customCell.visualFocus ? 2 : 0
                            border.color: AppTheme.focusRing
                        }
                        contentItem: Image {
                            id: customCellImage
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            // Bumped to re-evaluate `source`; never assign
                            // `source` directly, which would destroy the
                            // binding.
                            property int resolveTick: 0
                            // Resolved defensively: a delegate built
                            // synchronously from a property-change handler
                            // (this model is rebuilt from
                            // `app.stickers.revision`) can see its first
                            // unqualified `app` lookup resolve to undefined.
                            // The false branch registers no dependency, so
                            // Component.onCompleted's resolveBridge() is the
                            // recovery. Keep this the first `app` reference in
                            // the delegate: it absorbs the one poisoned lookup.
                            property var bridge: (typeof app !== "undefined" && app)
                                                 ? app.mediaBridge : null
                            // Idempotent; app.mediaBridge is constant, so
                            // replacing the binding loses nothing.
                            function resolveBridge() {
                                if (!bridge && typeof app !== "undefined"
                                    && app && app.mediaBridge)
                                    bridge = app.mediaBridge
                            }
                            Component.onCompleted: resolveBridge()
                            source: {
                                var _tick = resolveTick
                                if (!bridge || !bridge.supported)
                                    return ""
                                return bridge.mxcImageSource(
                                    customCell.modelData.url, 64)
                            }
                            Connections {
                                // A null target connects to nothing, so this
                                // follows the resolved bridge.
                                target: customCellImage.bridge
                                function onMediaCached(cacheKey) {
                                    if (cacheKey.endsWith(
                                            ":" + customCell.modelData.url))
                                        customCellImage.resolveTick++
                                }
                            }
                        }
                        onClicked: picker.chooseCustom(customCell.modelData.url)
                    }
                }

                MenuSectionLabel {
                    Layout.fillWidth: true
                    text: picker.sectionHeading
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    GridView {
                        id: emojiGrid
                        objectName: "emojiGrid"
                        anchors.fill: parent
                        clip: true
                        // As many columns as cells of about emojiCellSize fit,
                        // and the width divided among them, so cells stay
                        // square-ish at every picker size. A fixed column
                        // count stretched a 1000 px picker into 125 px cells,
                        // "Recently used" scattered across a sparse board.
                        readonly property int columns: Math.max(
                            1, Math.floor(width / AppTheme.emojiCellSize))
                        cellWidth: Math.floor(width / columns)
                        cellHeight: AppTheme.emojiCellSize
                        model: app.emojiCatalog
                        keyNavigationWraps: true
                        activeFocusOnTab: true
                        boundsBehavior: Flickable.StopAtBounds
                        // The shared bar; Basic's uses palette.mid.
                        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                        // Same wheel/touchpad feel as the room timeline;
                        // see qml/SmoothWheelArea.qml.
                        SmoothWheelArea {}

                        delegate: Item {
                            id: cell
                            required property int index
                            required property string emoji
                            required property string name
                            required property string baseEmoji
                            required property bool hasSkinTones
                            required property string accessibleLabel
                            width: emojiGrid.cellWidth
                            height: emojiGrid.cellHeight
                            activeFocusOnTab: true
                            // Arrow keys move currentIndex; this gives the
                            // current cell active focus so its Keys handlers
                            // and ring work.
                            focus: GridView.isCurrentItem
                            Accessible.name: accessibleLabel
                            Accessible.role: Accessible.Button

                            onActiveFocusChanged: {
                                if (activeFocus) {
                                    picker.previewEmoji = cell.emoji
                                    picker.previewName = cell.name
                                    picker.previewCell = cell
                                } else if (picker.previewEmoji === cell.emoji) {
                                    picker.previewEmoji = ""
                                    picker.previewName = ""
                                    if (picker.previewCell === cell)
                                        picker.previewCell = null
                                }
                            }

                            // Square and centred rather than anchors.fill:
                            // cells are wider than they are tall, which would
                            // stretch the highlight into a letterbox.
                            Rectangle {
                                anchors.centerIn: parent
                                width: Math.min(cell.width, cell.height)
                                       - AppTheme.spacing4
                                height: width
                                radius: AppTheme.radiusControl
                                color: cell.activeFocus || mouse.hovered
                                       ? AppTheme.stormSelection : "transparent"
                                border.width: cell.activeFocus ? 2 : 0
                                border.color: AppTheme.bolt
                            }
                            Label {
                                anchors.centerIn: parent
                                text: cell.emoji
                                // Named, not left to fallback: Qt 6.8 picks a
                                // monochrome face. Resolved in C++; empty when
                                // the host has none.
                                font.family: app.emojiFontFamily || ""
                                font.pixelSize: AppTheme.emojiGlyphSize
                            }
                            Label {
                                // Skin-tone marker: faint at rest, accent on
                                // the hovered/focused cell.
                                visible: cell.hasSkinTones
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: AppTheme.spacing4
                                text: "◢"
                                color: cell.activeFocus || mouse.hovered
                                       ? AppTheme.bolt : AppTheme.stormTextFaint
                                font.pixelSize: 8
                            }
                            HoverHandler {
                                id: mouse
                                onHoveredChanged: {
                                    if (hovered) {
                                        picker.previewEmoji = cell.emoji
                                        picker.previewName = cell.name
                                        picker.previewCell = cell
                                    } else if (picker.previewEmoji === cell.emoji) {
                                        picker.previewEmoji = ""
                                        picker.previewName = ""
                                        if (picker.previewCell === cell)
                                            picker.previewCell = null
                                    }
                                }
                            }
                            // A MouseArea, not a TapHandler: a handler grabs
                            // without accepting, so the background sink would
                            // also get the press, take the exclusive grab and
                            // cancel the tap. A MouseArea accepts, so delivery
                            // stops here. preventStealing is false, so the
                            // Flickable still takes drags.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onPressAndHold: if (cell.hasSkinTones)
                                                    cell.openVariants()
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton
                                            && cell.hasSkinTones)
                                        cell.openVariants()
                                    else
                                        picker.choose(cell.emoji)
                                }
                            }
                            // Variants open the picker's one shared tone popup;
                            // per-cell popups were expensive to build.
                            function openVariants() {
                                picker.openTonePopupFor(cell, baseEmoji)
                            }
                            Keys.onReturnPressed: picker.choose(emoji)
                            Keys.onEnterPressed: picker.choose(emoji)
                            Keys.onSpacePressed: picker.choose(emoji)
                            Keys.onMenuPressed: if (hasSkinTones) openVariants()
                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_V && event.modifiers & Qt.AltModifier
                                        && hasSkinTones) {
                                    openVariants()
                                    event.accepted = true
                                }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - AppTheme.spacing24
                        visible: app.emojiCatalog.count === 0
                        text: app.emojiCatalog.category === "Recently Used" && search.text.length === 0
                              ? qsTr("No recently used emoji") : qsTr("No emoji found")
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        Accessible.name: text
                    }
                }
            }
        }

        // ── Footer: hover/focus preview, or the static keyboard hint ──
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }
        Item {
            id: footerBar
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8

            RowLayout {
                anchors.fill: parent
                spacing: AppTheme.spacing8
                visible: picker.previewEmoji.length > 0
                Label {
                    text: picker.previewEmoji
                    font.pixelSize: AppTheme.textDisplay
                }
                Label {
                    // The display name only: data/emoji-catalog.tsv has no
                    // shortcode column. A name, so UI face at body weight.
                    Layout.fillWidth: true
                    text: picker.previewName
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textSubtitle
                    font.weight: AppTheme.weightStrong
                    color: AppTheme.stormText
                    elide: Label.ElideRight
                }
            }

            // At-rest hint in the same MenuKeycap language as the GIF picker.
            // Hints drop from the right as the bar narrows so nothing
            // overflows; the full sentence stays on the row for assistive
            // technology.
            Row {
                id: hintRow
                anchors.centerIn: parent
                spacing: AppTheme.spacing12
                visible: picker.previewEmoji.length === 0
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Enter selects · Alt+V or right-click "
                                      + "opens skin tones · Esc closes")

                Repeater {
                    model: [
                        { keys: "", icon: "keyboard_return",
                          label: qsTr("select"), minBar: 0 },
                        { keys: "Alt+V", icon: "",
                          label: qsTr("skin tones"), minBar: 180 },
                        { keys: "Esc", icon: "",
                          label: qsTr("close"), minBar: 270 },
                    ]
                    delegate: Row {
                        id: hint
                        required property var modelData
                        visible: footerBar.width >= hint.modelData.minBar
                        spacing: AppTheme.spacing4
                        MenuKeycap {
                            keys: hint.modelData.keys
                            iconName: hint.modelData.icon
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            // Remote or externally chosen text: never markup.
                            textFormat: Text.PlainText
                            text: hint.modelData.label
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightMedium
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }
        }
    }

    // The corner ornament that resizes the picker, on the popup's free corner
    // (top-left above the composer; away from the message for a reaction) and
    // out over the grab band. Hidden on the quick bar, which is content-sized.
    PopupResizeGrip {
        popup: picker
        visible: picker.gripShown
        corner: picker.gripCorner
        // arcCentre is measured from the panel corner, `gripGrab` inside.
        grabMargin: picker.gripGrab
        arcCentre: AppTheme.menuRadius + 6
        outerRadius: AppTheme.menuRadius + 2
        // x/y, not anchors: the corner moves when the picker is re-placed,
        // and swapping a top anchor for a bottom one passes through a state
        // with both set, which stretches the grip to the full height and
        // keeps that height after (the arc then drew 200 px from the corner).
        x: picker.gripOnRight ? parent.width - width + picker.rightPadding
                              : -picker.leftPadding
        y: picker.gripOnBottom ? parent.height - height + picker.bottomPadding
                               : -picker.topPadding
    }

    } // contentItem Item

    Popup {
        id: tonePopup
        objectName: "emojiTonePopup"
        property var variants: []
        parent: picker.contentItem
        padding: 4
        // Columns bounded by the space available: the picker can be narrower
        // than its 300 px minimum on a small window, and a fixed width would
        // overhang its border. The Grid wraps.
        readonly property int toneCell: 42
        readonly property int toneColumns: {
            // Null-tolerant: contentItem is deferred and may not exist yet.
            var room = picker.contentItem ? picker.contentItem.width : 0
            return Math.max(1, Math.min(6, Math.floor(
                (room - 2 * padding) / toneCell)))
        }
        width: Math.min(Math.max(variants.length, 1), toneColumns) * toneCell
               + 2 * padding
        height: Math.ceil(Math.max(variants.length, 1) / toneColumns) * toneCell
                + 2 * padding
        // Modal so a click elsewhere in the picker dismisses it. Presses on it
        // are consumed by the background sink (see the picker's own); the tone
        // tiles are ToolButtons that accept left presses only.
        modal: true
        dim: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: picker.toneOpened()
        onClosed: picker.toneClosed()
        background: Item {
            Rectangle {
                id: tonePanel
                anchors.fill: parent
                color: AppTheme.stormPanel
                border.color: AppTheme.stormBorderStrong
                radius: AppTheme.radiusMd
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                }
            }
            // Raised above the picker: a popover over a popover.
            MultiEffect {
                source: tonePanel
                anchors.fill: tonePanel
                z: -1
                shadowEnabled: true
                shadowColor: AppTheme.shadow
                shadowBlur: 0.6
                shadowVerticalOffset: 2
                shadowHorizontalOffset: 0
            }
        }
        Grid {
            columns: tonePopup.toneColumns
            Repeater {
                model: tonePopup.variants
                ToolButton {
                    id: toneButton
                    required property var modelData
                    width: tonePopup.toneCell; height: tonePopup.toneCell
                    text: modelData.emoji
                    font.pixelSize: AppTheme.textDisplay
                    // Storm hover fill instead of Basic's palette highlight.
                    background: Rectangle {
                        radius: AppTheme.radiusControl
                        color: toneButton.hovered || toneButton.visualFocus
                               ? AppTheme.stormSelection : "transparent"
                        // Keyboard focus ring; a fill alone looks like hover.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            radius: AppTheme.radiusControl
                            color: "transparent"
                            border.width: 2
                            border.color: AppTheme.bolt
                            visible: toneButton.visualFocus
                        }
                    }
                    Accessible.name: modelData.name
                    ToolTip.text: modelData.name
                    ToolTip.visible: hovered
                    onClicked: {
                        app.emojiCatalog.preferredTone = modelData.tone
                        tonePopup.close()
                        picker.choose(modelData.emoji)
                    }
                }
            }
        }
    }
}
