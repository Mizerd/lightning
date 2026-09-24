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

    onModelChanged: if (root.syncedValue !== undefined) Qt.callLater(root._resync)
    onCountChanged: if (root.syncedValue !== undefined) Qt.callLater(root._resync)

    implicitHeight: AppTheme.buttonHeight
    font.pixelSize: AppTheme.textBody
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
            verticalAlignment: Text.AlignVCenter
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
            ScrollBar.vertical: AppScrollBar { thin: true }
        }
    }
}
