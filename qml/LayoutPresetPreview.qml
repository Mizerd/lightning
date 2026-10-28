import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A bounded production-style fixture: theme, font and effective layout values
// come from the same settings as the conversation. No room data is fetched.
Rectangle {
    id: root
    objectName: "layoutPresetPreview"
    property int messageLayout: 0
    property int messageSpacing: 1
    property int threadPlacement: 0
    property int roomListWidth: 180
    readonly property bool compact: messageLayout === 2 || messageLayout === 3
    readonly property int rowGap: messageSpacing === 0 ? 3
                                  : messageSpacing === 2 ? 10 : 6
    implicitHeight: Math.max(168, timelineContent.implicitHeight + AppTheme.spacing8 * 2)
    radius: AppTheme.radiusMd
    color: AppTheme.timelineBackground
    border.color: AppTheme.stormBorder
    border.width: 1
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Layout preview: room rail, message grouping and composer")

    RowLayout {
        anchors.fill: parent
        anchors.margins: AppTheme.spacing8
        spacing: AppTheme.spacing8

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 16
            radius: AppTheme.radiusSm
            color: AppTheme.sidebar
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                spacing: 10
                Repeater {
                    model: 3
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: index === 0 ? AppTheme.accent : AppTheme.textMuted
                    }
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Math.max(72, Math.min(112, root.roomListWidth * 0.48))
            radius: AppTheme.radiusSm
            color: AppTheme.sidebar
            Column {
                anchors.fill: parent
                anchors.margins: 6
                spacing: root.compact ? 5 : 9
                Repeater {
                    model: 3
                    Row {
                        spacing: 5
                        Rectangle {
                            width: root.compact ? 10 : 14
                            height: width; radius: width / 2
                            color: AppTheme.accentBorder
                        }
                        Label {
                            text: [qsTr("General"), qsTr("Design"), qsTr("Team")][index]
                            color: AppTheme.textPrimary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: AppTheme.radiusSm
            color: AppTheme.timelineBackground
            ColumnLayout {
                id: timelineContent
                anchors.fill: parent
                spacing: root.rowGap
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Rectangle {
                        objectName: "layoutPreviewSmallAvatar"
                        visible: root.messageLayout === 3
                        implicitWidth: visible ? 20 : 0
                        implicitHeight: visible ? 20 : 0
                        radius: 10
                        color: AppTheme.accentBorder
                    }
                    Label {
                        text: qsTr("Mira · 10:42")
                        color: AppTheme.textSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    text: root.threadPlacement === 1
                          ? qsTr("Thread opens here")
                          : qsTr("Thread opens beside")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
                Label {
                    text: qsTr("The draft is ready")
                    color: AppTheme.textPrimary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    background: Rectangle {
                        visible: root.messageLayout === 1
                        color: AppTheme.surface
                        radius: AppTheme.radiusSm
                    }
                }
                Label {
                    text: qsTr("I added the notes")
                    color: AppTheme.textPrimary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    background: Rectangle {
                        visible: root.messageLayout === 1
                        color: AppTheme.surface
                        radius: AppTheme.radiusSm
                    }
                }
                Label {
                    text: qsTr("You · 10:43")
                    color: AppTheme.textSecondary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    Layout.topMargin: root.rowGap
                }
                Label {
                    text: qsTr("Thanks!")
                    color: AppTheme.textPrimary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                }
                Item { Layout.fillHeight: true }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: root.compact ? 22 : 30
                    radius: AppTheme.radiusSm
                    color: AppTheme.surface
                    border.color: AppTheme.stormBorder
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 7
                        text: qsTr("Message…")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    }
                }
            }
        }
    }
}
