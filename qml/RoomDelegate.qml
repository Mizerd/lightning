import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacingM * 2

    property bool selected: false
    signal clicked()

    Rectangle {
        anchors.fill: parent
        color: selected ? AppTheme.accent
             : hover.hovered ? AppTheme.surfaceAlt
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: AppTheme.spacingM
        spacing: AppTheme.spacingM

        Rectangle {
            width: 40; height: 40
            radius: 20
            color: selected ? AppTheme.accentText : AppTheme.surfaceAlt
            Label {
                anchors.centerIn: parent
                text: (model.name && model.name.length > 0) ? model.name.charAt(0).toUpperCase() : "?"
                color: selected ? AppTheme.accent : AppTheme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: model.name
                    color: selected ? AppTheme.accentText : AppTheme.text
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    visible: model.encrypted === true
                    text: "\u{1F512}"
                    font.pixelSize: 12
                    color: selected ? AppTheme.accentText : AppTheme.textMuted
                }
                Label {
                    visible: model.unreadCount > 0
                    text: model.unreadCount
                    color: selected ? AppTheme.accent : AppTheme.accentText
                    background: Rectangle {
                        color: selected ? AppTheme.accentText : AppTheme.accent
                        radius: 8
                    }
                    leftPadding: 6; rightPadding: 6; topPadding: 1; bottomPadding: 1
                    font.pixelSize: 11
                }
            }
            Label {
                text: model.lastMessagePreview
                color: selected ? Qt.rgba(1, 1, 1, 0.85) : AppTheme.textMuted
                font.pixelSize: 12
                elide: Label.ElideRight
                Layout.fillWidth: true
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.border
        opacity: 0.5
    }
}
