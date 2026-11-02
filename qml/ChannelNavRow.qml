import QtQuick
import QtQuick.Controls
import MatrixClient

ItemDelegate {
    id: root

    property string label: ""
    property string iconName: ""
    property bool current: false
    property bool comingSoon: false

    height: Math.max(AppTheme.channelRowMinHeight,
                    2 * AppTheme.scaled(AppTheme.textBody))
    padding: 0
    hoverEnabled: true
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()

    Accessible.role: Accessible.Button
    Accessible.name: root.comingSoon
                     ? qsTr("%1 — coming soon").arg(root.label) : root.label
    ToolTip.text: qsTr("Not available yet")
    ToolTip.visible: root.comingSoon && root.hovered
    ToolTip.delay: 400

    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: AppTheme.radiusSm
        color: root.comingSoon ? "transparent"
               : root.current ? AppTheme.channelSelected
                            : (root.hovered || root.activeFocus
                               ? AppTheme.channelHover : "transparent")
        border.width: root.activeFocus ? 2 : 0
        border.color: AppTheme.focusRing
        Behavior on color {
            ColorAnimation {
                duration: 90
            }
        }
    }

    contentItem: Item {
        anchors.fill: parent

        Icon {
            id: glyph
            objectName: "channelNavGlyph"
            anchors.verticalCenter: parent.verticalCenter
            // Anchored, not `x`: anchors mirror under LayoutMirroring, x does not.
            anchors.left: parent.left
            anchors.leftMargin: 14
            name: root.iconName
            size: 16
            color: root.comingSoon ? AppTheme.channelCategoryText
                   : root.current ? AppTheme.channelSelectedText
                                  : AppTheme.channelText
        }

        Loader {
            active: root.comingSoon
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                id: soonLabel
                text: qsTr("Coming soon")
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                color: AppTheme.channelCategoryText
            }
        }

        Loader {
            objectName: "channelNavName"
            active: root.label.length > 0
            anchors.left: glyph.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: root.comingSoon ? 84 : 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                textFormat: Text.PlainText
                text: root.label
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                color: root.comingSoon ? AppTheme.channelCategoryText
                       : root.current ? AppTheme.channelSelectedText
                                      : AppTheme.channelText
            }
        }
    }
}
