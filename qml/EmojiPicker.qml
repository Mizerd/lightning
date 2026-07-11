import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One window-overlay picker shared by reaction and composer entry points.
// Catalogue/search/recent state lives in the process-wide C++ model.
Popup {
    id: picker
    property string mode: "composer"
    property point anchorPoint: Qt.point(0, 0)
    property bool closeAfterSelection: true
    signal emojiChosen(string emoji)

    parent: Overlay.overlay
    width: Math.min(420, parent ? parent.width - AppTheme.spacingM * 2 : 420)
    height: Math.min(480, parent ? parent.height - AppTheme.spacingM * 2 : 480)
    padding: AppTheme.spacingS
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function choose(emoji) {
        if (!emoji || emoji.length === 0) return
        app.emojiCatalog.recordUse(emoji)
        emojiChosen(emoji)
        if (closeAfterSelection) close()
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
        Qt.callLater(search.forceActiveFocus)
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
        border.width: 1
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacingS

        TextField {
            id: search
            Layout.fillWidth: true
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

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            contentWidth: categoryRow.implicitWidth
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            Row {
                id: categoryRow
                spacing: 2
                Repeater {
                    model: app.emojiCatalog.categories
                    ToolButton {
                        required property string modelData
                        readonly property bool selected: app.emojiCatalog.category === modelData
                        text: {
                            var icons = {"Recently Used":"◷", "Smileys & Emotion":"☺",
                                         "People & Body":"☝", "Animals & Nature":"♞",
                                         "Food & Drink":"♨", "Travel & Places":"✈",
                                         "Activities":"⚽", "Objects":"⌨",
                                         "Symbols":"♥", "Flags":"⚑"}
                            return icons[modelData] || "•"
                        }
                        checked: selected
                        Accessible.name: modelData
                        ToolTip.text: modelData
                        ToolTip.visible: hovered
                        onClicked: {
                            app.emojiCatalog.category = modelData
                            search.text = ""
                            app.emojiCatalog.searchText = ""
                            emojiGrid.forceActiveFocus()
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: emojiGrid
                anchors.fill: parent
                clip: true
                cellWidth: 44
                cellHeight: 44
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

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: AppTheme.radiusSm
                        color: cell.activeFocus || mouse.hovered
                               ? AppTheme.hover : "transparent"
                        border.width: cell.activeFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                    }
                    Label {
                        anchors.centerIn: parent
                        text: cell.emoji
                        font.pixelSize: 24
                    }
                    Label {
                        visible: cell.hasSkinTones
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        text: "◢"
                        color: AppTheme.textMuted
                        font.pixelSize: 8
                    }
                    HoverHandler { id: mouse }
                    ToolTip.text: cell.name
                    ToolTip.visible: mouse.hovered
                    ToolTip.delay: 500
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
                    function openVariants() {
                        tonePopup.variants = app.emojiCatalog.variantsFor(baseEmoji)
                        tonePopup.open()
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

                    Popup {
                        id: tonePopup
                        property var variants: []
                        parent: picker.contentItem
                        x: Math.max(0, Math.min(cell.mapToItem(picker.contentItem, 0, 0).x,
                                                picker.contentItem.width - width))
                        y: Math.max(0, Math.min(cell.mapToItem(picker.contentItem, 0, cell.height).y,
                                                picker.contentItem.height - height))
                        width: Math.min(variants.length, 6) * 42 + 8
                        height: Math.ceil(variants.length / 6) * 42 + 8
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

        Label {
            Layout.fillWidth: true
            text: qsTr("Enter selects · Alt+V or right-click opens skin tones · Esc closes")
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.fontCaption
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
