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
        font.weight: AppTheme.weightBold
    }
    Label {
        text: root.description
        color: AppTheme.textMuted
        font.pixelSize: AppTheme.textMeta
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
        // Derived from the MODEL, never from contentHeight. Binding the
        // height to contentHeight is circular — contentHeight is the sum of
        // the delegates the view decided to create, and how many it creates
        // depends on its height — so the view settles at whatever partial
        // height it happened to reach first and the last row is left clipped
        // mid-text (reported with two members: the second row cut through
        // its MXID).
        readonly property int rowHeight: 42
        Layout.preferredHeight: {
            if (!visible)
                return 0
            var n = root.members ? root.members.length : 0
            if (n <= 0)
                return 0
            return Math.min(180, n * rowHeight + (n - 1) * spacing)
        }
        clip: true
        spacing: 2
        model: root.members
        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
        delegate: ItemDelegate {
            id: memberRow
            required property var modelData
            width: memberList.width
            implicitHeight: memberList.rowHeight
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
                        // Identity ink: the whole point of this list is
                        // picking one person out of it.
                        color: AppTheme.userColor(
                                   memberRow.modelData.userId || "")
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    Label {
                        visible: memberRow.modelData.displayName
                                 && memberRow.modelData.displayName.length > 0
                        Layout.fillWidth: true
                        text: memberRow.modelData.userId
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                        elide: Label.ElideMiddle
                    }
                }
                CheckBox {
                    palette.windowText: AppTheme.textPrimary
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
