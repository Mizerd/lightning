import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// One window-overlay picker shared by reaction and composer entry points.
// Catalogue/search/recent state lives in the process-wide C++ model.
//
// v0.6.5 (SPEC §0/§1m): width 324, zero outer padding with per-row 12px
// gutters, a Material-icon category rail (replacing the Unicode-glyph strip),
// an 8-column glyph grid, and a footer that previews whichever cell is
// hovered or keyboard-focused instead of only ever showing the static hint.
// There is deliberately NO global skin-tone swatch: EmojiCatalog's persisted
// preferredTone is never read back by the grid's rendering (only recorded
// for later, unused), so a global control would be presentational fiction —
// the ONE shared per-emoji tone popup below is the real mechanism.
//
// Storm skin (SPEC-storm-language §4/2e): stormPanel chrome, storm search
// field, bolt-underlined active category, stormSelection hover cells.
//
// 2026-08-21 design pass: the footer's emoji NAME left the bolt/mono ":zap:"
// treatment for the UI face (there is no shortcode to render, so that slot was
// carrying a plain label in code type), the at-rest hint became MenuKeycap
// chips so both composer pickers speak one keyboard language, the cell
// highlight is square at any picker width, and the panel gained the sanctioned
// popover shadow. The press sink in `background` is the real fix for the
// reported "emoji clicking still doesnt work right" — read it before touching
// the popup flags.
// v0.6.7: the root is AnchoredPopup, not a bare Popup — anchorPoint and
// placeInsideWindow() moved there, and the popup now re-anchors on a window
// resize instead of being placed once and left behind. Callers that open it
// from a button set `anchorItem`; the reaction-picker entry points still pass
// a bare `anchorPoint` (there is no stable item under a message-row point).
AnchoredPopup {
    id: picker
    property string mode: "composer"
    property bool closeAfterSelection: true
    signal emojiChosen(string emoji)
    // The nested skin-tone popup's lifetime, reported to whoever owns
    // transient row interaction (TimelinePane's transientInteractionOwner).
    // A host that does not care simply does not connect them.
    signal toneOpened()
    signal toneClosed()

    // Footer preview state: the emoji + name of whichever cell is currently
    // hovered or keyboard-focused. Empty when nothing is (the footer then
    // falls back to the static keyboard hint).
    property string previewEmoji: ""
    property string previewName: ""
    // The cell the preview belongs to (hovered, or keyboard-focused). The
    // shared Alt+V shortcut needs the ITEM, not just its emoji: the tone
    // popup is positioned at the cell it was asked for.
    property Item previewCell: null

    // Category -> Material Symbols glyph, keyed by EmojiCatalog's exact
    // category strings (verified against EmojiCatalog.cpp's kCategories).
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

    // One section heading reflecting whichever single bucket is currently
    // visible. EmojiCatalog swaps ONE precomputed bucket at a time (recents /
    // a category / a search result set) rather than concatenating every
    // category into one scroll — that bucket-swap is what keeps category
    // switching a constant-time operation (see EmojiCatalogTest's
    // categorySwitchingIsBucketSwapFast), so the heading names whichever
    // bucket is on screen rather than implying a multi-section list that
    // does not exist.
    readonly property string sectionHeading:
        search.text.length > 0 ? qsTr("Search results")
        : app.emojiCatalog.category === "Recently Used" ? qsTr("Recently used")
        : app.emojiCatalog.category

    // v0.6.7: sized as a SHARE of the available space (see AnchoredPopup), and
    // sharing the GIF picker's remembered value — resizing either resizes
    // both. The minimum keeps the 8-column grid and the category rail usable.
    widthFraction: 0.36
    heightFraction: 0.58
    minWidth: 300
    minHeight: 320
    sizeSettingsKey: "picker"
    padding: 0
    // Modality bounds presses that land OUTSIDE the picker: a click on the
    // timeline closes it and is consumed, instead of also acting on the row
    // it landed on. That is all it does. It does NOT protect the picker's
    // own surface — the 2026-08-18 round believed it did, and the barrier
    // that actually works is the press sink in `background` below, which
    // carries the mechanism. dim: false keeps the look.
    modal: true
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function choose(emoji) {
        if (!emoji || emoji.length === 0) return
        app.emojiCatalog.recordUse(emoji)
        emojiChosen(emoji)
        if (closeAfterSelection) close()
    }

    // v0.7: the single shared skin-tone popup (one per picker, positioned
    // at the requesting cell on demand — never one per grid cell).
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

    // 2026-08-18 tester report ("alt+v emoji bar neveikia"): the shortcut was
    // implemented only as a Keys handler on a grid CELL, and the picker opens
    // with the keyboard focus in its search field — so the cell never saw the
    // key and the footer advertised a shortcut that could not fire. It now
    // lives on the picker, targeting whichever cell the user is actually on:
    // the hovered one, else the keyboard-current one.
    Shortcut {
        sequences: ["Alt+V"]
        enabled: picker.visible
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

    // placeInsideWindow()/reanchor() are AnchoredPopup's, and AnchoredPopup's
    // own onAboutToShow already performs the initial placement.
    onAboutToShow: {
        search.text = ""
        app.emojiCatalog.searchText = ""
        previewEmoji = ""
        previewName = ""
        previewCell = null
        Qt.callLater(search.forceActiveFocus)
    }

    background: Item {
        Rectangle {
            id: pickerPanel
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 2
            radius: AppTheme.menuRadius + 6

            // THE press barrier — and the reason it has to live here rather
            // than in the popup's own flags.
            //
            // A Popup does not consume a press that lands on it:
            // QQuickPopup::mousePressEvent sets accepted = blockInput(), and
            // blockInput() returns FALSE when the press is inside the popup's
            // own item, so the delivery agent keeps walking the hit list down
            // to whatever sits behind the overlay. Modality bounds presses
            // OUTSIDE a popup only.
            //
            // Nothing else in this picker stops a RIGHT press. The grid cells
            // use TapHandlers, and a pointer handler grabs the point but never
            // ACCEPTS it; the GridView is a Flickable, which Qt constructs
            // with setAcceptedMouseButtons(Qt::LeftButton) — so left presses
            // were being swallowed by accident and right presses fell straight
            // through to MessageDelegate's Qt.RightButton TapHandler, opening
            // the message context menu on top of the open picker and covering
            // the grid. That is the reported "emoji clicking still doesnt work
            // right".
            //
            // The background fills the whole popupItem, padding included, and
            // sits below contentItem, so every control the picker actually
            // handles still sees the press first. This only catches what would
            // otherwise have left the picker entirely.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }
        }
        // One of the design's sanctioned popover shadows (the AccountMenu /
        // QuickSwitcher pattern): the effect and its source must be SIBLINGS,
        // which is the only reason the background is an Item wrapping the
        // panel instead of the panel itself. Without it the picker floats over
        // the timeline on a border alone and reads as part of it.
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

    // v0.6.7: the column is wrapped in a plain Item so the resize grip can be
    // anchored over its bottom-right corner — a direct child of the
    // ColumnLayout would be laid out as another row instead.
    contentItem: Item {

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Row 1: search only — no skin-tone swatch (see file header) ──
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

        // ── Row 2: category icon rail ────────────────────────────────
        ScrollView {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.bottomMargin: AppTheme.spacing8
            Layout.preferredHeight: 28
            contentWidth: categoryRow.implicitWidth
            // A ScrollView does NOT clip by default: on a window narrow
            // enough to shrink the popup, the rail icons would paint over
            // the rounded border and out of the card.
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
                        // 10 cells × 28 + 9 × 2 = 298 ≤ the 300px gutter
                        // width, so the whole rail (Flags included) shows
                        // at rest per SPEC 1m.
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
                            // Three states, not two: the plate lit on hover
                            // while the glyph stayed muted, so hovering an
                            // inactive category read as barely anything.
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
                            // The rail is Tab-reachable (focusPolicy above),
                            // and keyboard focus drew nothing at all: the
                            // plate lit for hover and selection only, so a
                            // keyboard user could not see where they were.
                            border.width: categoryCell.visualFocus ? 2 : 0
                            border.color: AppTheme.bolt
                            // 2e: the active category carries a 2px bolt
                            // underline bar at the cell's bottom edge.
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

        // ── Body: one section heading + the current bucket's grid ───
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

                MenuSectionLabel {
                    Layout.fillWidth: true
                    text: picker.sectionHeading
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    GridView {
                        id: emojiGrid
                        anchors.fill: parent
                        clip: true
                        // SPEC 1m: exactly 8 columns — divide the body
                        // width rather than flooring width/cellSize, which
                        // yielded 9 columns at the 324px picker width.
                        cellWidth: Math.floor(width / 8)
                        cellHeight: AppTheme.emojiCellSize
                        model: app.emojiCatalog
                        keyNavigationWraps: true
                        activeFocusOnTab: true
                        boundsBehavior: Flickable.StopAtBounds
                        // The shared bar, not the Basic style's: a stock
                        // ScrollBar takes its handle from palette.mid, which
                        // on the navy panel is a grey sliver with square ends.
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
                            // Keyboard navigation moves GridView.currentIndex;
                            // without this the current cell never took active
                            // focus, so its Keys handlers (and the focus ring)
                            // were unreachable by arrow keys.
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

                            // SQUARE, centred — deliberately not
                            // anchors.fill. The column count is fixed at 8
                            // while the row height is the fixed
                            // emojiCellSize, so every cell wider than 32px
                            // (any picker over ~256px, i.e. all of them)
                            // stretched its highlight into a letterbox around
                            // a centred glyph. The grid geometry itself is
                            // contract-pinned; the highlight is not.
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
                                font.pixelSize: AppTheme.emojiGlyphSize
                            }
                            Label {
                                // The corner fold that says "this emoji has
                                // skin tones". It only ever mattered on the
                                // cell the pointer is on, so it rests faint
                                // and lifts to the accent there instead of
                                // speckling the whole grid at one weight.
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
                            TapHandler {
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onLongPressed: if (cell.hasSkinTones) cell.openVariants()
                                onTapped: (eventPoint, button) => {
                                    if (button === Qt.RightButton && cell.hasSkinTones)
                                        cell.openVariants()
                                    else
                                        picker.choose(cell.emoji)
                                }
                            }
                            // v0.7: variants open the picker's ONE shared tone
                            // popup. The previous per-cell Popup built ~70–100
                            // popup subtrees synchronously every time the grid
                            // (re)populated — the dominant "picker feels slow"
                            // cost.
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
                    // Data source: EmojiCatalog's TSV has no shortcode column
                    // (fields are emoji/name/keywords/category/baseEmoji/tone
                    // — see data/emoji-catalog.tsv), so this shows the
                    // display name only rather than a fabricated shortcode.
                    //
                    // It is a NAME, so it renders in the UI face at body
                    // weight. It used to be bolt-inked JetBrains Mono, on the
                    // theory that it occupied the ":zap:" shortcode slot of
                    // the Storm language — but with no shortcode to show, that
                    // treatment put a yellow monospace string where every
                    // other picker in the app shows a plain label, which is a
                    // large part of "the font looks out of place".
                    Layout.fillWidth: true
                    text: picker.previewName
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textSubtitle
                    font.weight: AppTheme.weightStrong
                    color: AppTheme.stormText
                    elide: Label.ElideRight
                }
            }

            // The at-rest hint, in the SAME keycap language the GIF picker's
            // footer already speaks (MenuKeycap) — the two pickers are peers
            // that share a size and an anchor, so they should not disagree
            // about how a keyboard hint looks. Hints drop from the right as
            // the picker narrows, keyed off this bar's own width so nothing
            // can overflow the rounded border on a small window; the full
            // sentence stays on the row for assistive technology.
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

    // The corner ornament that resizes the picker.
    PopupResizeGrip {
        popup: picker
        arcCentre: AppTheme.menuRadius + 6
        outerRadius: AppTheme.menuRadius + 2
        anchors.left: parent.left
        anchors.top: parent.top
    }

    } // contentItem Item

    Popup {
        id: tonePopup
        objectName: "emojiTonePopup"
        property var variants: []
        parent: picker.contentItem
        padding: 4
        // Tile size, and how many of them honestly fit. Six across was a
        // fixed 260px, but this popup is a CHILD of the picker's content
        // item and AnchoredPopup's width floor is Math.min(minWidth, cap) —
        // so a small window legitimately produces a picker narrower than its
        // own 300px minimum, and the fixed width then overhung the picker's
        // rounded border while openTonePopupFor()'s x clamp had already
        // bottomed out at 0. Bound the popup to the space it actually has and
        // let the Grid wrap; the placement maths above is correct and stays
        // untouched.
        readonly property int toneCell: 42
        readonly property int toneColumns: {
            // Null-tolerant on purpose: Control.contentItem is a DEFERRED
            // property, so this binding can be evaluated before it exists.
            var room = picker.contentItem ? picker.contentItem.width : 0
            return Math.max(1, Math.min(6, Math.floor(
                (room - 2 * padding) / toneCell)))
        }
        width: Math.min(Math.max(variants.length, 1), toneColumns) * toneCell
               + 2 * padding
        height: Math.ceil(Math.max(variants.length, 1) / toneColumns) * toneCell
                + 2 * padding
        // Modality bounds presses outside this popup (which is how a click
        // elsewhere in the picker dismisses it). Presses that land ON it are
        // consumed by the sink in its background, for the reason spelled out
        // at the picker's own background: a Popup does not accept a press
        // inside itself, and these tiles are ToolButtons — QQuickAbstractButton
        // accepts LeftButton only — so a right-click on a tone tile leaked to
        // the message row underneath exactly as the grid's did.
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
            // Raised above the picker it sits inside, not merely bordered —
            // it is a popover over a popover, and the screenshot that opened
            // this round read it as a stray floating panel.
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
                    // Storm hover fill in place of the Basic style's
                    // palette-derived flat highlight on the navy panel.
                    background: Rectangle {
                        radius: AppTheme.radiusControl
                        color: toneButton.hovered || toneButton.visualFocus
                               ? AppTheme.stormSelection : "transparent"
                        // These tiles are Tab-reachable (ToolButton keeps
                        // Qt.StrongFocus), so keyboard focus needs the same
                        // bolt ring every other focusable control in this
                        // picker draws — a fill alone is indistinguishable
                        // from hover.
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
