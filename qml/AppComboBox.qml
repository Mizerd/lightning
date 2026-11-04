import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

ComboBox {
    id: root

    property bool storm: false

    // Shows a search field above the open list that narrows its rows. The
    // model itself is never filtered, so currentIndex, indexOfValue() and
    // currentText keep meaning the unfiltered row.
    property bool filterable: false
    property string filterPlaceholder: ""
    property string filterText: ""
    readonly property var filteredRows: {
        if (!root.filterable || !root.popup.visible)
            return []
        void root.model
        var needle = root.filterText.trim().toLocaleLowerCase()
        var rows = []
        for (var i = 0; i < root.count; ++i) {
            var label = root.textAt(i)
            if (needle === "" || label.toLocaleLowerCase().indexOf(needle) >= 0)
                rows.push({ index: i, text: label })
        }
        return rows
    }

    function pickFilteredRow(row) {
        if (!row)
            return
        root.popup.close()
        if (root.currentIndex !== row.index)
            root.currentIndex = row.index
        root.activated(row.index)
    }

    component ComboRow: ItemDelegate {
        id: row
        property string label
        property bool chosen: false
        property bool hoverFill: true
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: AppTheme.menuItemHeight
        height: AppTheme.menuItemHeight
        clip: true
        hoverEnabled: true
        contentItem: Label {
            leftPadding: AppTheme.menuItemPadding
            text: row.label
            textFormat: Text.PlainText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            color: row.chosen ? AppTheme.stormText : AppTheme.stormTextSecondary
            height: row.availableHeight
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignLeft
            elide: Label.ElideRight
        }
        background: Rectangle {
            radius: AppTheme.menuItemRadius
            color: row.chosen ? AppTheme.stormSelection
                   : row.highlighted || (row.hoverFill && row.hovered)
                     ? Qt.alpha(AppTheme.stormSelection, 0.55)
                   : "transparent"
        }
    }

    // Select the row whose valueRole equals `v`. Use this instead of binding
    // currentIndex: indexOfValue() returns -1 before the model and valueRole
    // have settled, and clamping that to 0 shows row 0 while the stored value
    // is something else. A -1 is retried on the next tick; a value absent from
    // the model leaves the index alone.
    //
    // The last requested value, reapplied when the model is rebuilt (e.g. qsTr
    // models re-evaluate on a language change, which resets currentIndex to 0
    // without `activated`).
    property var syncedValue: undefined

    function syncToValue(v) {
        root.syncedValue = v
        var i = indexOfValue(v)
        if (i >= 0) {
            if (i !== currentIndex)
                currentIndex = i
            return true
        }
        Qt.callLater(root._resync)
        return false
    }

    function _resync() {
        if (root.syncedValue === undefined)
            return
        var j = root.indexOfValue(root.syncedValue)
        if (j >= 0 && j !== root.currentIndex)
            root.currentIndex = j
    }

    onModelChanged: {
        root._openTurn()
        root._modelTurn = true
        if (root.syncedValue !== undefined)
            Qt.callLater(root._resync)
    }
    onCountChanged: if (root.syncedValue !== undefined) Qt.callLater(root._resync)

    property bool restoreAfterModelReset: true
    property int _committed: -1
    property int _committedCount: -1
    property int _beforeTurn: -1
    property int _current: -1
    property bool _turnPending: false
    property bool _modelTurn: false
    property int _restoreIndex: -1
    property bool _restoring: false

    function _openTurn() {
        if (root._turnPending)
            return
        root._turnPending = true
        root._beforeTurn = root._committed
        Qt.callLater(root._endTurn)
    }

    function _endTurn() {
        root._turnPending = false
        var modelTurn = root._modelTurn
        root._modelTurn = false
        var before = root._beforeTurn
        if (modelTurn && root.restoreAfterModelReset && root.syncedValue === undefined && before > 0
                && root.currentIndex === 0 && root.count === root._committedCount
                && before < root.count) {
            root._restoreIndex = before
            root._restoring = true
            Qt.callLater(function () { root._restoring = false })
            return
        }
        root._committed = root.currentIndex
        root._committedCount = root.count
    }

    Binding {
        target: root
        property: "currentIndex"
        value: root._restoreIndex
        when: root._restoring
        restoreMode: Binding.RestoreNone
    }

    onCurrentIndexChanged: { root._current = currentIndex; root._openTurn() }
    Component.onCompleted: root._openTurn()

    Keys.onReturnPressed: (event) => root._openFromKey(event)
    Keys.onEnterPressed: (event) => root._openFromKey(event)
    function _openFromKey(event) {
        if (event.isAutoRepeat) {
            event.accepted = true
            return
        }
        if (!root.enabled)
            return
        if (root.popup.visible) {
            event.accepted = false
            return
        }
        event.accepted = true
        root._swallowKeyRelease = true
        root.popup.open()
    }
    property bool _swallowKeyRelease: false
    onActiveFocusChanged: if (!root.activeFocus) root._swallowKeyRelease = false
    Connections {
        target: root.popup
        function onClosed() { root._swallowKeyRelease = false }
    }
    Keys.onReleased: (event) => {
        if (root._swallowKeyRelease
                && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            root._swallowKeyRelease = false
            event.accepted = true
        }
    }

    Keys.onPressed: (event) => {
        if (!root.popup.visible
                || (event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown))
            return
        var page = Math.max(1, Math.floor(comboPopup.height
                                          / AppTheme.menuItemHeight) - 1)
        for (var i = 0; i < page; ++i) {
            if (event.key === Qt.Key_PageDown)
                root.incrementCurrentIndex()
            else
                root.decrementCurrentIndex()
        }
        event.accepted = true
    }

    implicitHeight: Math.max(AppTheme.buttonHeight,
                             AppTheme.settingsControlMinHeight)
    font.pixelSize: AppTheme.settingsOptionSize
    hoverEnabled: true

    contentItem: Label {
        leftPadding: AppTheme.buttonPaddingH
        rightPadding: 28
        text: root.displayText
        font: root.font
        color: root.storm
               ? (root.enabled ? AppTheme.stormText : AppTheme.stormTextFaint)
               : (root.enabled ? AppTheme.textPrimary : AppTheme.textDisabled)
        verticalAlignment: Text.AlignVCenter
        elide: Label.ElideRight
    }

    indicator: Icon {
        x: root.width - width - 8
        anchors.verticalCenter: parent.verticalCenter
        name: "expand_more"
        size: 18
        color: root.storm
               ? (root.enabled ? AppTheme.stormTextMuted : AppTheme.stormTextFaint)
               : (root.enabled ? AppTheme.icon : AppTheme.textDisabled)
        rotation: root.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: 120 } }
    }

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: root.storm ? AppTheme.stormInset : AppTheme.inputBackground
        border.width: root.visualFocus ? 2 : 1
        border.color: {
            if (root.storm)
                return root.visualFocus ? AppTheme.bolt
                     : root.hovered ? AppTheme.stormBorderStrong
                     : AppTheme.stormBorder
            return root.visualFocus ? AppTheme.focusRing
                 : root.hovered ? AppTheme.borderStrong
                 : AppTheme.border
        }
    }

    delegate: ComboRow {
        id: entry
        required property var model
        required property int index
        highlighted: root.highlightedIndex === index
        chosen: root.currentIndex === index
        label: entry.model[root.textRole] !== undefined
               ? entry.model[root.textRole]
               : entry.model.display !== undefined ? entry.model.display
                                                   : String(entry.model.modelData)
    }

    popup: Popup {
        id: comboPopup
        y: root.height + 4
        width: root.width
        padding: AppTheme.menuPadding
        focus: root.filterable
        implicitHeight: Math.min(contentItem.implicitHeight
                                 + 2 * AppTheme.menuPadding,
                                 root.filterable ? 360 : 320)
        onAboutToShow: if (root.filterable) filterField.clear()
        // PopupFocusReason: any other reason makes ComboBox close the popup
        // as its own focus leaves.
        onOpened: if (root.filterable) filterField.forceActiveFocus(Qt.PopupFocusReason)

        background: Item {
            MultiEffect {
                source: comboPanel
                anchors.fill: comboPanel
                z: -1
                shadowEnabled: !AppTheme.reducedMotion
                shadowColor: AppTheme.shadowSoft
                shadowBlur: 0.5
                shadowVerticalOffset: AppTheme.elevationCardY
                shadowHorizontalOffset: 0
            }
            Rectangle {
                id: comboPanel
                anchors.fill: parent
                color: AppTheme.stormPanel
                border.color: AppTheme.stormBorder
                border.width: 1
                radius: AppTheme.menuRadius
            }
        }
        contentItem: Item {
            // Sized from the unfiltered count so the popup does not jump
            // while the user types.
            implicitHeight: root.filterable
                ? filterField.implicitHeight + AppTheme.spacing4
                  + Math.max(1, root.count) * AppTheme.menuItemHeight
                : plainList.contentHeight

            ListView {
                id: plainList
                visible: !root.filterable
                anchors.fill: parent
                clip: true
                model: root.popup.visible && !root.filterable ? root.delegateModel : null
                currentIndex: root.highlightedIndex
                onCountChanged: if (count > 0 && root.currentIndex >= 0)
                                    positionViewAtIndex(root.currentIndex, ListView.Contain)
                ScrollBar.vertical: AppScrollBar { thin: true }
            }

            AppTextField {
                id: filterField
                objectName: "comboFilterField"
                visible: root.filterable
                width: parent.width
                storm: true
                searchIcon: true
                clearButton: true
                placeholderText: root.filterPlaceholder
                Accessible.name: root.filterPlaceholder
                onTextChanged: root.filterText = text
                Keys.onUpPressed: filteredList.moveBy(-1)
                Keys.onDownPressed: filteredList.moveBy(1)
                Keys.onPressed: (event) => {
                    if (event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown)
                        return
                    var page = Math.max(1, Math.floor(filteredList.height
                                                      / AppTheme.menuItemHeight) - 1)
                    filteredList.moveBy(event.key === Qt.Key_PageDown ? page : -page)
                    event.accepted = true
                }
                Keys.onReturnPressed: root.pickFilteredRow(root.filteredRows[filteredList.currentIndex])
                Keys.onEnterPressed: root.pickFilteredRow(root.filteredRows[filteredList.currentIndex])
                Keys.onEscapePressed: root.popup.close()
            }

            ListView {
                id: filteredList
                objectName: "comboFilteredList"
                visible: root.filterable
                anchors.top: filterField.bottom
                anchors.topMargin: AppTheme.spacing4
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true
                model: root.filteredRows
                // Empty filter: start on the chosen row; otherwise the first match.
                onModelChanged: {
                    var rows = root.filteredRows
                    var at = rows.length > 0 ? 0 : -1
                    if (root.filterText.trim() === "") {
                        for (var k = 0; k < rows.length; ++k) {
                            if (rows[k].index === root.currentIndex) {
                                at = k
                                break
                            }
                        }
                    }
                    currentIndex = at
                    Qt.callLater(filteredList.showCurrent)
                }
                function moveBy(step) {
                    if (count === 0)
                        return
                    currentIndex = Math.max(0, Math.min(count - 1, currentIndex + step))
                    showCurrent()
                }
                function showCurrent() {
                    if (currentIndex >= 0)
                        positionViewAtIndex(currentIndex, ListView.Contain)
                }
                delegate: ComboRow {
                    id: filteredEntry
                    required property var modelData
                    required property int index
                    label: modelData.text
                    chosen: root.currentIndex === modelData.index
                    highlighted: filteredList.currentIndex === index
                    hoverFill: false
                    onClicked: root.pickFilteredRow(filteredEntry.modelData)
                }
                ScrollBar.vertical: AppScrollBar { thin: true }

                // The row under the pointer becomes the keyboard row, but only
                // when the pointer moves: rows rebuilt under a resting pointer
                // while typing must not steal the first-match highlight.
                HoverHandler {
                    id: rowHover
                    property point lastScene: Qt.point(-1, -1)
                    onPointChanged: {
                        var p = rowHover.point.scenePosition
                        if (!rowHover.hovered || (p.x === lastScene.x && p.y === lastScene.y))
                            return
                        lastScene = p
                        var i = filteredList.indexAt(rowHover.point.position.x + filteredList.contentX,
                                                     rowHover.point.position.y + filteredList.contentY)
                        if (i >= 0)
                            filteredList.currentIndex = i
                    }
                }

                ComboRow {
                    objectName: "comboFilterEmpty"
                    visible: root.filterable && filteredList.count === 0
                    enabled: false
                    width: parent.width
                    label: qsTr("No matches")
                }
            }
        }
    }
}
