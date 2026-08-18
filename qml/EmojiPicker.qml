import QtQuick
import QtQuick.Controls
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
// field, bolt-underlined active category, stormSelection hover cells, bolt
// mono footer preview. Colors/fonts only — structure and behavior unchanged.
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
    modal: false
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

    background: Rectangle {
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 2
        radius: AppTheme.menuRadius + 6
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
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
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
                            color: categoryCell.selected ? AppTheme.bolt
                                                         : AppTheme.stormTextMuted
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusControl
                            color: categoryCell.selected || categoryCell.hovered
                                   ? AppTheme.stormSelection : "transparent"
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
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
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

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
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
                                visible: cell.hasSkinTones
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                text: "◢"
                                color: AppTheme.stormTextMuted
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
                        visible: app.emojiCatalog.count === 0
                        text: app.emojiCatalog.category === "Recently Used" && search.text.length === 0
                              ? qsTr("No recently used emoji") : qsTr("No emoji found")
                        color: AppTheme.stormTextMuted
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
                    font.pixelSize: 22
                }
                Label {
                    // Data source: EmojiCatalog's TSV has no shortcode column
                    // (fields are emoji/name/keywords/category/baseEmoji/tone
                    // — see data/emoji-catalog.tsv), so this shows the
                    // display name only rather than a fabricated shortcode.
                    Layout.fillWidth: true
                    text: picker.previewName
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontMonoSm
                    font.weight: Font.Bold
                    // 2e footer: the previewed emoji's mono line inks bolt
                    // (the ":zap:" slot; this catalogue carries no shortcode
                    // column, so the display name rides that treatment).
                    color: AppTheme.bolt
                    elide: Label.ElideRight
                }
            }
            Label {
                anchors.fill: parent
                visible: picker.previewEmoji.length === 0
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Enter selects · Alt+V or right-click opens skin tones · Esc closes")
                color: AppTheme.stormTextMuted
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontChip
                font.weight: Font.Medium
                elide: Label.ElideRight
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
        property var variants: []
        parent: picker.contentItem
        width: Math.min(Math.max(variants.length, 1), 6) * 42 + 8
        height: Math.ceil(Math.max(variants.length, 1) / 6) * 42 + 8
        padding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusSm
        }
        Grid {
            columns: 6
            Repeater {
                model: tonePopup.variants
                ToolButton {
                    id: toneButton
                    required property var modelData
                    width: 42; height: 42
                    text: modelData.emoji
                    font.pixelSize: 22
                    // Storm hover fill in place of the Basic style's
                    // palette-derived flat highlight on the navy panel.
                    background: Rectangle {
                        radius: AppTheme.radiusControl
                        color: toneButton.hovered || toneButton.visualFocus
                               ? AppTheme.stormSelection : "transparent"
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
