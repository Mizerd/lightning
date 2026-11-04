import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

AnchoredPopup {
    id: picker
    property string mode: "composer"
    property bool closeAfterSelection: true
    property bool allowCustomEmoji: true

    readonly property bool reactionMode: mode === "reaction"
    property bool expanded: false
    property bool openExpanded: false
    readonly property bool compact: reactionMode && !expanded
    signal emojiChosen(string emoji)
    signal customChosen(var row)
    signal toneOpened()
    signal toneClosed()

    property string previewEmoji: ""
    property string previewName: ""
    property Item previewCell: null

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

    readonly property string sectionHeading:
        search.text.length > 0 ? qsTr("Search results")
        : app.emojiCatalog.category === "Recently Used" ? qsTr("Recently used")
        : app.emojiCatalog.category

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
        search.forceActiveFocus()
    }

    function chooseFirstResult() {
        if (searchTimer.running) {
            searchTimer.stop()
            app.emojiCatalog.searchText = search.text
        }
        // Read row 0 from the MODEL, not a delegate: the reset above clears
        // the grid's delegates and they are recreated on the next layout
        // pass, so itemAtIndex(0) is null (or stale) right after a flush.
        // 257 is EmojiCatalog::EmojiRole (Qt.UserRole + 1), which is exactly
        // the `emoji` the delegate would have shown.
        var m = app.emojiCatalog
        if (m.rowCount() <= 0)
            return
        var first = m.data(m.index(0, 0), Qt.UserRole + 1)
        if (first)
            picker.choose(first)
    }

    function choose(emoji) {
        if (!emoji || emoji.length === 0) return
        app.emojiCatalog.recordUse(emoji)
        emojiChosen(emoji)
        if (closeAfterSelection) close()
    }

    // Custom emoji from the image packs, in both modes. A custom-emoji
    // reaction is an m.reaction whose key is the pack image's mxc URI (the
    // Cinny/Sable convention, an ecosystem extension rather than part of the
    // image-pack standard); MessageDelegate renders such a key as an image. In
    // the composer a pick inserts the shortcode text, bound to that exact image.
    // Doesn't call recordUse(): recents are the Unicode catalogue's.
    readonly property bool customEmojiOffered: allowCustomEmoji && app.stickers.available
    // Empty: every pack, favourites and recents first.
    property string customPackId: ""
    readonly property var customEmoji: {
        var _live = app.stickers.revision
        if (!(picker.customEmojiOffered && picker.visible && !picker.compact))
            return []
        if (picker.customPackId.length > 0) {
            var rows = app.stickers.packEmoticons(picker.customPackId)
            if (rows.length > 0)
                return rows
        }
        return app.stickers.reactionEmoticons()
    }
    function chooseCustom(url, row) {
        if (!url || url.indexOf("mxc://") !== 0) return
        if (reactionMode) {
            if (row)
                app.stickers.recordCustomUse(row)
            emojiChosen(url)
        }
        else if (row)
            customChosen(row)
        if (closeAfterSelection) close()
    }

    function openTonePopupFor(cellItem, baseEmoji) {
        tonePopup.variants = app.emojiCatalog.variantsFor(baseEmoji)
        if (tonePopup.variants.length === 0)
            return
        var p = cellItem.mapToItem(picker.contentItem, 0, cellItem.height)
        tonePopup.originCell = cellItem
        tonePopup.x = Math.max(0, Math.min(p.x,
                                           picker.contentItem.width
                                           - tonePopup.width))
        tonePopup.y = Math.max(0, Math.min(p.y,
                                           picker.contentItem.height
                                           - tonePopup.height))
        tonePopup.open()
    }

    Shortcut {
        sequences: ["Alt+V"]
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

    onAboutToShow: {
        expanded = openExpanded
        openExpanded = false
        if (!anchorItem)
            placeAtPoint()
        search.text = ""
        app.emojiCatalog.searchText = ""
        if (reactionMode) {
            app.emojiCatalog.category = "Recently Used"
            if (app.emojiCatalog.count === 0)
                app.emojiCatalog.category = "Smileys & Emotion"
        }
        previewEmoji = ""
        previewName = ""
        previewCell = null
        customPackId = ""
        quickBar.focusIndex = 0
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
            radius: picker.compact ? height / 2 : AppTheme.menuRadius + 6

            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -picker.leftInset
                anchors.rightMargin: -picker.rightInset
                anchors.topMargin: -picker.topInset
                anchors.bottomMargin: -picker.bottomInset
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
        property int focusIndex: 0
        property bool keyboardActive: false
        implicitWidth: 2 * pad + (quickCount + 1) * cellSize
                       + quickSeparator.width
        implicitHeight: 2 * pad + cellSize
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
                        font.family: app.emojiFontFamily || ""
                        font.pixelSize: 24
                        scale: quickHover.hovered ? 1.18 : 1
                        Behavior on scale {
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }
                    }
                    HoverHandler { id: quickHover }
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
            Keys.onReturnPressed: picker.chooseFirstResult()
            Keys.onEnterPressed: picker.chooseFirstResult()
            Timer {
                id: searchTimer
                interval: 150
                repeat: false
                onTriggered: app.emojiCatalog.searchText = search.text
            }
        }

        ScrollView {
            id: railScroll
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.bottomMargin: AppTheme.spacing8
            Layout.preferredHeight: 28
            contentWidth: categoryRow.implicitWidth
            clip: true
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AlwaysOff }
            ScrollBar.horizontal: AppScrollBar { thin: true; policy: ScrollBar.AsNeeded }
            SmoothWheelArea {
                axis: "horizontal"
                scrollTarget: railScroll.contentItem as Flickable
            }
            Row {
                id: categoryRow
                spacing: AppTheme.spacing2
                Repeater {
                    model: app.emojiCatalog.categories
                    delegate: AbstractButton {
                        id: categoryCell
                        required property string modelData
                        readonly property bool selected:
                            search.text.length === 0
                            && app.emojiCatalog.category === modelData
                        implicitWidth: 28
                        implicitHeight: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData
                        Accessible.checked: categoryCell.selected
                        onActiveFocusChanged: {
                            var f = railScroll.contentItem as Flickable
                            if (!activeFocus || !f)
                                return
                            if (categoryCell.x < f.contentX)
                                f.contentX = categoryCell.x
                            else if (categoryCell.x + categoryCell.width
                                     > f.contentX + f.width)
                                f.contentX = categoryCell.x + categoryCell.width
                                             - f.width
                        }
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
                                 : categoryCell.hovered || categoryCell.visualFocus
                                   ? AppTheme.stormText
                                   : AppTheme.stormTextMuted
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusControl
                            color: categoryCell.selected || categoryCell.hovered
                                   || categoryCell.visualFocus
                                   ? AppTheme.stormSelection : "transparent"
                            border.width: categoryCell.visualFocus ? 2 : 0
                            border.color: AppTheme.bolt
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
                    visible: picker.customEmoji.length > 0
                    text: qsTr("Custom emoji")
                }
                PackChipStrip {
                    id: customPackStrip
                    objectName: "customEmojiPackStrip"
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    visible: picker.customEmoji.length > 0 && packCount > 1
                    usage: "emoticon"
                    showAll: true
                    allCurrent: picker.customPackId.length === 0
                    currentPackId: picker.customPackId
                    onPackChosen: (packId) => picker.customPackId = packId
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
                    SmoothWheelArea { axis: "horizontal" }

                    delegate: AbstractButton {
                        id: customCell
                        required property var modelData
                        width: 28
                        height: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.Button
                        Accessible.name: customCell.modelData.shortcode
                        Accessible.description: qsTr("Right-click to add to or "
                                                     + "remove from favourites")
                        ToolTip.text: customCell.modelData.packName
                                      ? ":" + customCell.modelData.shortcode + ":  ·  "
                                        + customCell.modelData.packName
                                      : ":" + customCell.modelData.shortcode + ":"
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
                            cache: false
                            sourceSize.width: 28
                            sourceSize.height: 28
                            property int resolveTick: 0
                            property var bridge: (typeof app !== "undefined" && app)
                                                 ? app.mediaBridge : null
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
                                target: customCellImage.bridge
                                function onMediaCached(cacheKey) {
                                    if (cacheKey.endsWith(
                                            ":" + customCell.modelData.url))
                                        customCellImage.resolveTick++
                                }
                            }
                        }
                        onClicked: picker.chooseCustom(customCell.modelData.url,
                                                       customCell.modelData)
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            gesturePolicy: TapHandler.WithinBounds
                            onTapped: app.stickers.toggleFavourite(customCell.modelData)
                        }
                        onPressAndHold: app.stickers.toggleFavourite(customCell.modelData)
                        Rectangle {
                            visible: app.stickers.revision >= 0
                                     && app.stickers.isFavourite(customCell.modelData.url)
                            width: 6
                            height: 6
                            radius: 3
                            anchors.right: parent.right
                            anchors.top: parent.top
                            color: AppTheme.accent
                        }
                    }
                }

                MenuSectionLabel {
                    Layout.fillWidth: true
                    text: picker.sectionHeading
                    SmoothWheelArea { scrollTarget: emojiGrid }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    GridView {
                        id: emojiGrid
                        objectName: "emojiGrid"
                        anchors.fill: parent
                        clip: true
                        readonly property int columns: Math.max(
                            1, Math.floor(width / AppTheme.emojiCellSize))
                        cellWidth: Math.floor(width / columns)
                        cellHeight: AppTheme.emojiCellSize
                        model: app.emojiCatalog
                        keyNavigationWraps: false
                        activeFocusOnTab: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                        SmoothWheelArea {}

                        function navigate(key, ctrl) {
                            var n = count
                            if (n <= 0)
                                return false
                            var cols = columns
                            var page = Math.max(1, Math.floor(height / cellHeight) - 1) * cols
                            var to = currentIndex
                            switch (key) {
                            case Qt.Key_Up:
                                if (currentIndex >= cols)
                                    return false
                                search.forceActiveFocus()
                                return true
                            case Qt.Key_Home:
                                to = ctrl ? 0 : currentIndex - currentIndex % cols
                                break
                            case Qt.Key_End:
                                to = ctrl ? n - 1
                                    : Math.min(n - 1, currentIndex - currentIndex % cols + cols - 1)
                                break
                            case Qt.Key_PageUp:
                                to = Math.max(0, currentIndex - page)
                                break
                            case Qt.Key_PageDown:
                                to = Math.min(n - 1, currentIndex + page)
                                break
                            default:
                                return false
                            }
                            currentIndex = to
                            positionViewAtIndex(to, GridView.Contain)
                            return true
                        }
                        function resetToStart() {
                            currentIndex = 0
                            positionViewAtBeginning()
                        }
                        Connections {
                            target: app.emojiCatalog
                            function onCategoryChanged() { Qt.callLater(emojiGrid.resetToStart) }
                            function onSearchTextChanged() { Qt.callLater(emojiGrid.resetToStart) }
                        }

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
                            // Not a Tab stop: the grid is ONE (arrows move
                            // inside it), or a keyboard user would have to Tab
                            // through every emoji to leave it.
                            activeFocusOnTab: false
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
                                    if (mouse.button === Qt.RightButton) {
                                        if (cell.hasSkinTones)
                                            cell.openVariants()
                                    } else {
                                        picker.choose(cell.emoji)
                                    }
                                }
                            }
                            function openVariants() {
                                picker.openTonePopupFor(cell, baseEmoji)
                            }
                            Keys.onReturnPressed: picker.choose(emoji)
                            Keys.onEnterPressed: picker.choose(emoji)
                            Keys.onSpacePressed: picker.choose(emoji)
                            Keys.onMenuPressed: if (hasSkinTones) openVariants()
                            // Unhandled keys must stay unaccepted: a handler
                            // accepts by default, which swallowed Escape and
                            // the popup could not be closed from the grid.
                            Keys.onPressed: (event) => {
                                event.accepted = false
                                if (event.key === Qt.Key_V && event.modifiers & Qt.AltModifier
                                        && hasSkinTones) {
                                    openVariants()
                                    event.accepted = true
                                    return
                                }
                                if (event.modifiers & (Qt.AltModifier | Qt.MetaModifier))
                                    return
                                if (emojiGrid.navigate(event.key,
                                        !!(event.modifiers & Qt.ControlModifier)))
                                    event.accepted = true
                            }
                        }
                    }

                    MiddleClickScroller {
                        objectName: "emojiGridMiddleClickScroller"
                        anchors.fill: parent
                        z: 1
                        view: emojiGrid
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
            SmoothWheelArea { scrollTarget: emojiGrid }

            RowLayout {
                anchors.fill: parent
                spacing: AppTheme.spacing8
                visible: picker.previewEmoji.length > 0
                Label {
                    text: picker.previewEmoji
                    font.pixelSize: AppTheme.textDisplay
                }
                Label {
                    Layout.fillWidth: true
                    text: picker.previewName
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textSubtitle
                    font.weight: AppTheme.weightStrong
                    color: AppTheme.stormText
                    elide: Label.ElideRight
                }
            }

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
                          label: qsTr("select"), minBar: 0,
                          needsTarget: true },
                        { keys: "Alt+V", icon: "",
                          label: qsTr("skin tones"), minBar: 180,
                          needsTarget: false },
                        { keys: "Esc", icon: "",
                          label: qsTr("close"), minBar: 270,
                          needsTarget: false },
                    ]
                    delegate: Row {
                        id: hint
                        required property var modelData
                        visible: footerBar.width >= hint.modelData.minBar
                                 && (!hint.modelData.needsTarget
                                     || emojiGrid.count > 0)
                        spacing: AppTheme.spacing4
                        MenuKeycap {
                            keys: hint.modelData.keys
                            iconName: hint.modelData.icon
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
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

    PopupResizeGrip {
        popup: picker
        visible: picker.gripShown
        corner: picker.gripCorner
        grabMargin: picker.gripGrab
        arcCentre: AppTheme.menuRadius + 6
        outerRadius: AppTheme.menuRadius + 2
        x: picker.gripOnRight ? parent.width - width + picker.rightPadding
                              : -picker.leftPadding
        y: picker.gripOnBottom ? parent.height - height + picker.bottomPadding
                               : -picker.topPadding
    }

    }

    Popup {
        id: tonePopup
        objectName: "emojiTonePopup"
        property var variants: []
        property Item originCell: null
        parent: picker.contentItem
        padding: 4
        readonly property int toneCell: 42
        readonly property int toneColumns: {
            var room = picker.contentItem ? picker.contentItem.width : 0
            return Math.max(1, Math.min(6, Math.floor(
                (room - 2 * padding) / toneCell)))
        }
        width: Math.min(Math.max(variants.length, 1), toneColumns) * toneCell
               + 2 * padding
        height: Math.ceil(Math.max(variants.length, 1) / toneColumns) * toneCell
                + 2 * padding
        modal: true
        dim: false
        // Without focus the popup is modal yet receives no key: Escape and
        // typing went to a dead item behind it and the picker could not be
        // closed from the keyboard. It takes focus on open and hands it back
        // on close (the search field, which every cell path shares).
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        function moveTone(from, delta) {
            var n = tonePopup.variants.length
            var to = from + delta
            if (to < 0 || to >= n)
                return
            var b = toneRepeater.itemAt(to)
            if (b)
                b.forceActiveFocus(Qt.TabFocusReason)
        }
        onOpened: {
            var first = toneRepeater.itemAt(0)
            if (first)
                first.forceActiveFocus(Qt.TabFocusReason)
            picker.toneOpened()
        }
        onClosed: {
            if (picker.visible && !picker.compact) {
                var origin = tonePopup.originCell
                if (origin && origin.GridView.view === emojiGrid
                        && emojiGrid.currentItem === origin)
                    origin.forceActiveFocus()
                else
                    search.forceActiveFocus()
            }
            tonePopup.originCell = null
            picker.previewEmoji = ""
            picker.previewName = ""
            picker.previewCell = null
            picker.toneClosed()
        }
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
                id: toneRepeater
                model: tonePopup.variants
                ToolButton {
                    id: toneButton
                    required property var modelData
                    required property int index
                    Keys.onLeftPressed: tonePopup.moveTone(index, -1)
                    Keys.onRightPressed: tonePopup.moveTone(index, 1)
                    Keys.onUpPressed: tonePopup.moveTone(index, -tonePopup.toneColumns)
                    Keys.onDownPressed: tonePopup.moveTone(index, tonePopup.toneColumns)
                    Keys.onReturnPressed: toneButton.clicked()
                    Keys.onEnterPressed: toneButton.clicked()
                    width: tonePopup.toneCell; height: tonePopup.toneCell
                    text: modelData.emoji
                    font.pixelSize: AppTheme.textDisplay
                    background: Rectangle {
                        radius: AppTheme.radiusControl
                        color: toneButton.hovered || toneButton.visualFocus
                               ? AppTheme.stormSelection : "transparent"
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
                    function showName() {
                        picker.previewEmoji = modelData.emoji
                        picker.previewName = modelData.name
                    }
                    onHoveredChanged: if (hovered) showName()
                    onVisualFocusChanged: if (visualFocus) showName()
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
