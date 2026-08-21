import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// Lightning combo box: a flat themed field with a Material Symbols chevron,
// a themed popup, and flat delegates with an accent-soft selected state.
// Every visual part is owned here — no native gradient, no platform arrow,
// no default popup chrome.
//
// Storm: the dropdown POPUP is always storm (every menu/popover is, per
// SPEC-storm-language §5); `storm: true` additionally storms the closed
// field for hosts that sit on storm surfaces (Settings). Themed hosts
// (Room Information) keep the themed field.
ComboBox {
    id: root

    property bool storm: false

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
        // Integer weights only — the same DPR-1.0 antialiasing problem the
        // 1.5px storm focus border had in AppTextField.
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

    // One vocabulary with AppMenuItem: same row height, same corner, same
    // face/size/weight. A dropdown row and a context-menu row are the same
    // thing and were rendering 30px/radius-6/13px against 32px/radius-8/13px.
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

        // Elevation. The shadow lives on a SIBLING behind the panel, sourced
        // from it — never on the panel itself — so the popup's measured
        // implicitWidth/implicitHeight are untouched. That is the same
        // constraint AppMenu documents: a shadow that inflates popup geometry
        // breaks the anchor maths callers do against it.
        // Popup sizes its own background item, so this wrapper needs no
        // implicit size of its own — and must not declare one derived from
        // the panel it contains.
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
