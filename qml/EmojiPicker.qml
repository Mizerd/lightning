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
Popup {
    id: picker
    property string mode: "composer"
    property point anchorPoint: Qt.point(0, 0)
    property bool closeAfterSelection: true
    signal emojiChosen(string emoji)

    // Footer preview state: the emoji + name of whichever cell is currently
    // hovered or keyboard-focused. Empty when nothing is (the footer then
    // falls back to the static keyboard hint).
    property string previewEmoji: ""
    property string previewName: ""

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

    parent: Overlay.overlay
    width: Math.min(324, parent ? parent.width - AppTheme.spacingM * 2 : 324)
    height: Math.min(480, parent ? parent.height - AppTheme.spacingM * 2 : 480)
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

    function placeInsideWindow() {
        if (!parent) return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x, parent.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2, parent.width - width - AppTheme.spacingS))
        y = below + height <= parent.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS, anchorPoint.y - height - AppTheme.spacingXS)
    }

    onAboutToShow: {
        placeInsideWindow()
        search.text = ""
        app.emojiCatalog.searchText = ""
        previewEmoji = ""
        previewName = ""
        Qt.callLater(search.forceActiveFocus)
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
        border.width: 1
        radius: AppTheme.menuRadius
    }

    contentItem: ColumnLayout {
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
                            color: categoryCell.selected ? AppTheme.selectedText
                                                         : AppTheme.icon
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusControl
                            color: categoryCell.selected ? AppTheme.accentSoft
                                   : categoryCell.hovered ? AppTheme.hover
                                                          : "transparent"
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
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
                            Accessible.name: accessibleLabel
                            Accessible.role: Accessible.Button

                            onActiveFocusChanged: {
                                if (activeFocus) {
                                    picker.previewEmoji = cell.emoji
                                    picker.previewName = cell.name
                                } else if (picker.previewEmoji === cell.emoji) {
                                    picker.previewEmoji = ""
                                    picker.previewName = ""
                                }
                            }

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                radius: AppTheme.radiusControl
                                color: cell.activeFocus || mouse.hovered
                                       ? AppTheme.accentSoft : "transparent"
                                border.width: cell.activeFocus ? 2 : 0
                                border.color: AppTheme.focusRing
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
                                color: AppTheme.textMuted
                                font.pixelSize: 8
                            }
                            HoverHandler {
                                id: mouse
                                onHoveredChanged: {
                                    if (hovered) {
                                        picker.previewEmoji = cell.emoji
                                        picker.previewName = cell.name
                                    } else if (picker.previewEmoji === cell.emoji) {
                                        picker.previewEmoji = ""
                                        picker.previewName = ""
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
                        color: AppTheme.textMuted
                        Accessible.name: text
                    }
                }
            }
        }

        // ── Footer: hover/focus preview, or the static keyboard hint ──
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
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
                    color: AppTheme.textPrimary
                    elide: Label.ElideRight
                }
            }
            Label {
                anchors.fill: parent
                visible: picker.previewEmoji.length === 0
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Enter selects · Alt+V or right-click opens skin tones · Esc closes")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontCaption
            }
        }
    }

    Popup {
        id: tonePopup
        property var variants: []
        parent: picker.contentItem
        width: Math.min(Math.max(variants.length, 1), 6) * 42 + 8
        height: Math.ceil(Math.max(variants.length, 1) / 6) * 42 + 8
        padding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: AppTheme.surfaceElevated
            border.color: AppTheme.borderStrong
            radius: AppTheme.radiusSm
        }
        Grid {
            columns: 6
            Repeater {
                model: tonePopup.variants
                ToolButton {
                    required property var modelData
                    width: 42; height: 42
                    text: modelData.emoji
                    font.pixelSize: 22
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
