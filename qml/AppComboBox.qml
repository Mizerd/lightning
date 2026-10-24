import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// Lightning combo box: a flat themed field with a Material Symbols chevron, a
// themed popup and flat delegates with an accent-soft selected state. No
// native gradient, platform arrow or default popup chrome.
//
// The dropdown popup is always storm, like every menu; `storm: true` also
// styles the closed field for hosts on storm surfaces (Settings).
ComboBox {
    id: root

    property bool storm: false

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

    // A language change re-evaluates every qsTr() model and ComboBox answers
    // by resetting currentIndex to 0 WITHOUT `activated`, so a combo that
    // binds `currentIndex: app.settings.X` (nothing re-evaluates that binding:
    // X did not change) shows row 0 while the stored value is something else.
    // Reported on privacy controls (read receipts, notification preview) and
    // on a keyword rule created disabled. Cover every combo here instead of
    // in each caller: remember the index that stood at the start of an
    // event-loop turn, and when that turn rebuilt the model with the SAME row
    // count and left the index at 0, put the old one back. A Binding with
    // RestoreNone writes without destroying the caller's own binding (an
    // imperative write would, for the life of the object). Combos that use
    // syncToValue keep their own mechanism.
    // Set false on a combo whose model is DATA (devices, fonts, rooms): a
    // same-size rebuild there is a real change and a reset to row 0 is meant.
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

    // Return and Enter open a focused, closed combo like Space does. With the
    // popup open they stay Qt's: choose the highlighted row.
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
            // Explicitly Qt's: it chooses the highlighted row on release.
            event.accepted = false
            return
        }
        event.accepted = true
        root._swallowKeyRelease = true
        root.popup.open()
    }
    // Qt's ComboBox closes an open popup on the RELEASE of Return/Enter, which
    // would shut the list this very key press just opened. Eat that one release.
    property bool _swallowKeyRelease: false
    // The release may never reach us (Tab, window deactivation, popup focus);
    // a stale flag would eat the NEXT release, i.e. a row choice.
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

    // PageUp/PageDown move the highlight a page of rows, like Home and End
    // move it to the ends. Only while the list is open.
    Keys.onPressed: (event) => {
        if (!root.popup.visible
                || (event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown))
            return
        var page = Math.max(1, Math.floor(comboPopup.height
                                          / AppTheme.menuItemHeight) - 1)
        // incrementCurrentIndex/decrementCurrentIndex move the HIGHLIGHT while
        // the popup is open (they are what Up and Down call).
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
        // Integer weights only (a 1.5px border blurs at DPR 1.0).
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

    // Same row height, corner and type as AppMenuItem: a dropdown row and a
    // context-menu row are the same thing.
    delegate: ItemDelegate {
        id: entry
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: AppTheme.menuItemHeight
        // Fixed row, whatever the script: a tall fallback face (Arabic,
        // Devanagari, Bengali) must not stretch its row or crowd the next.
        height: AppTheme.menuItemHeight
        clip: true
        hoverEnabled: true
        highlighted: root.highlightedIndex === index
        contentItem: Label {
            leftPadding: AppTheme.menuItemPadding
            text: entry.model[root.textRole] !== undefined
                  ? entry.model[root.textRole]
                  : entry.model.display !== undefined ? entry.model.display
                                                      : String(entry.model.modelData)
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            color: root.currentIndex === entry.index ? AppTheme.stormText
                                                     : AppTheme.stormTextSecondary
            height: entry.availableHeight
            verticalAlignment: Text.AlignVCenter
            // Rows read from the left whatever the row's own script is.
            horizontalAlignment: Text.AlignLeft
            elide: Label.ElideRight
        }
        background: Rectangle {
            radius: AppTheme.menuItemRadius
            color: root.currentIndex === entry.index ? AppTheme.stormSelection
                   : entry.highlighted || entry.hovered
                     ? Qt.alpha(AppTheme.stormSelection, 0.55)
                   : "transparent"
        }
    }

    popup: Popup {
        id: comboPopup
        y: root.height + 4
        width: root.width
        padding: AppTheme.menuPadding
        implicitHeight: Math.min(contentItem.implicitHeight
                                 + 2 * AppTheme.menuPadding, 320)

        // Elevation: the shadow is a sibling behind the panel, so the popup's
        // measured size is untouched (as in AppMenu). Popup sizes this wrapper
        // itself; it must not declare an implicit size derived from the panel.
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
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            // The model is attached when the popup shows; open on the row
            // that is selected rather than at the top of the list.
            onCountChanged: if (count > 0 && root.currentIndex >= 0)
                                positionViewAtIndex(root.currentIndex, ListView.Contain)
            ScrollBar.vertical: AppScrollBar { thin: true }
        }
    }
}
