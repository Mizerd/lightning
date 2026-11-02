import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MatrixClient

MenuItem {
    id: root

    property string iconName: ""
    property bool danger: false
    property string accel: ""
    property string accelIconName: ""
    property bool radio: false
    property bool radioSelected: false

    readonly property bool _active: root.highlighted || root.hovered
    readonly property color _ink: !root.enabled ? AppTheme.stormTextFaint
                                  : root.danger ? AppTheme.menuDangerText
                                  : root._active ? AppTheme.menuItemActiveText
                                  : AppTheme.menuItemText

    function requestMenuFit() {
        var owner = root.menu
        if (owner && owner.visible && typeof owner.refitWidth === "function")
            Qt.callLater(owner.refitWidth)
    }
    onImplicitWidthChanged: requestMenuFit()
    onVisibleChanged: requestMenuFit()

    implicitHeight: visible ? AppTheme.menuItemHeight : 0
    padding: AppTheme.menuItemPadding
    leftPadding: AppTheme.menuItemPadding + 6
    topPadding: 0
    bottomPadding: 0

    Accessible.description: radio ? (radioSelected ? qsTr("Selected")
                                                   : qsTr("Not selected"))
                                  : ""

    contentItem: RowLayout {
        spacing: AppTheme.menuIconGap
        Loader {
            active: root.radio
            visible: active
            sourceComponent: StormNode {
                size: 16
                complete: root.radioSelected
                iconName: root.radioSelected ? "check" : ""
            }
        }
        Icon {
            visible: !root.radio && root.iconName.length > 0
            name: root.iconName
            size: AppTheme.menuIconSize
            color: !root.enabled ? AppTheme.stormTextFaint
                   : root.danger ? AppTheme.stormDanger
                   : root._active ? AppTheme.bolt
                   : AppTheme.stormTextMuted
        }
        Label {
            Layout.fillWidth: true
            text: root.text
            // Menu rows carry remote text (room, member, file names) and
            // MenuItem has no textFormat, so this label pins plain text.
            textFormat: Text.PlainText
            elide: Label.ElideRight
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            color: root.radio && !root.radioSelected && root.enabled
                   && !root._active ? AppTheme.stormTextMuted : root._ink
        }
        MenuKeycap {
            visible: root.accel.length > 0 || root.accelIconName.length > 0
            keys: root.accel
            iconName: root.accelIconName
            active: root._active && root.enabled && !root.danger
            danger: root.danger
        }
        Icon {
            visible: root.subMenu !== null && root.subMenu !== undefined
            name: "chevron_right"
            size: AppTheme.menuIconSize - 2
            color: root._active ? AppTheme.stormTextSecondary
                                : AppTheme.stormTextMuted
        }
    }

    background: Rectangle {
        radius: AppTheme.menuItemRadius
        color: root._active && root.enabled
               ? (root.danger ? Qt.alpha(AppTheme.menuDangerTint, 0.10)
                              : AppTheme.menuHighlight)
               : root.radio && root.radioSelected ? AppTheme.menuHighlight
               : "transparent"

        Icon {
            visible: root._active && root.enabled && !root.danger
            name: "bolt"
            size: 12
            color: AppTheme.bolt
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 0
        }
    }
}
