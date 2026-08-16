import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Reusable bounded/virtualized room-member multi-select used by the From
// and Mentions filters. The parent owns draft state so opening one section
// never mutates an applied query.
ColumnLayout {
    id: root

    required property string title
    required property string description
    required property string sectionKey
    property var selectedValues: []
    property bool expanded: false
    property string needle: ""
    property var members: []

    signal toggleExpanded()
    signal needleChangedByUser(string value)
    signal userToggled(string userId)

    spacing: AppTheme.spacing6
    Layout.leftMargin: AppTheme.spacing12
    Layout.rightMargin: AppTheme.spacing12

    Label {
        text: root.title
        color: AppTheme.textPrimary
        font.weight: Font.Bold
    }
    Label {
        text: root.description
        color: AppTheme.textMuted
        font.pixelSize: 10
    }
    AppButton {
        Layout.fillWidth: true
        text: root.selectedValues.length > 0
              ? qsTr("%1 selected").arg(root.selectedValues.length)
              : qsTr("Select room members")
        onClicked: root.toggleExpanded()
    }
    AppTextField {
        visible: root.expanded
        Layout.fillWidth: true
        searchIcon: true
        clearButton: true
        placeholderText: qsTr("Search room members…")
        text: root.needle
        onTextEdited: root.needleChangedByUser(text)
    }
    ListView {
        id: memberList
        visible: root.expanded
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? Math.min(180, contentHeight) : 0
        clip: true
        spacing: 2
        model: root.members
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: memberRow
            required property var modelData
            width: memberList.width
            implicitHeight: 42
            hoverEnabled: true
            onClicked: root.userToggled(modelData.userId || "")
            Accessible.name: modelData.displayName
                             ? qsTr("%1, %2").arg(modelData.displayName)
                                                .arg(modelData.userId)
                             : modelData.userId
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: memberRow.hovered ? AppTheme.hover : "transparent"
            }
            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                Avatar {
                    size: 28
                    mxc: memberRow.modelData.avatarUrl || ""
                    name: memberRow.modelData.displayName
                          || memberRow.modelData.userId
                    colorKey: memberRow.modelData.userId || ""
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: memberRow.modelData.displayName
                              || memberRow.modelData.userId
                        color: AppTheme.textPrimary
                        font.pixelSize: 12
                        elide: Label.ElideRight
                    }
                    Label {
                        visible: memberRow.modelData.displayName
                                 && memberRow.modelData.displayName.length > 0
                        Layout.fillWidth: true
                        text: memberRow.modelData.userId
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                        elide: Label.ElideMiddle
                    }
                }
                CheckBox {
                    checked: root.selectedValues.indexOf(
                                 memberRow.modelData.userId) >= 0
                    Accessible.name: qsTr("Select %1")
                        .arg(memberRow.modelData.displayName
                             || memberRow.modelData.userId)
                    onClicked: root.userToggled(memberRow.modelData.userId)
                }
            }
        }
    }
}
