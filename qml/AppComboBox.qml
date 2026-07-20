import QtQuick
import QtQuick.Controls
import MatrixClient

// Lightning combo box: a flat themed field with a Material Symbols chevron,
// a themed popup, and flat delegates with an accent-soft selected state.
// Every visual part is owned here — no native gradient, no platform arrow,
// no default popup chrome.
ComboBox {
    id: root

    implicitHeight: 32
    font.pixelSize: 13
    hoverEnabled: true

    contentItem: Label {
        leftPadding: 12
        rightPadding: 28
        text: root.displayText
        font: root.font
        color: root.enabled ? AppTheme.textPrimary : AppTheme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Label.ElideRight
    }

    indicator: Icon {
        x: root.width - width - 8
        anchors.verticalCenter: parent.verticalCenter
        name: "expand_more"
        size: 18
        color: root.enabled ? AppTheme.icon : AppTheme.textDisabled
        rotation: root.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: 120 } }
    }

    background: Rectangle {
        radius: AppTheme.radiusMd
        color: AppTheme.inputBackground
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? AppTheme.focusRing
                      : root.hovered ? AppTheme.borderStrong
                      : AppTheme.border
    }

    delegate: ItemDelegate {
        id: entry
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: 30
        hoverEnabled: true
        highlighted: root.highlightedIndex === index
        contentItem: Label {
            leftPadding: 8
            text: entry.model[root.textRole] !== undefined
                  ? entry.model[root.textRole]
                  : entry.model.display !== undefined ? entry.model.display
                                                      : String(entry.model.modelData)
            font.pixelSize: 13
            color: root.currentIndex === entry.index ? AppTheme.accentText
                                                     : AppTheme.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Label.ElideRight
        }
        background: Rectangle {
            radius: 6
            color: root.currentIndex === entry.index ? AppTheme.accentSoft
                   : entry.highlighted || entry.hovered ? AppTheme.hover
                   : "transparent"
        }
    }

    popup: Popup {
        y: root.height + 4
        width: root.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.borderStrong
            border.width: 1
            radius: AppTheme.radiusMd
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        }
    }
}
